/*
 * fs_fat.c — FAT12/16/32 + exFAT read-only extraction
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#include "endian.h"
#include "imgextract.h"
#include "fs_fat.h"

/* ── FAT BPB (BIOS Parameter Block) ────────────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} FAT_BPB;

typedef struct {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} FAT_DirEntry;

typedef struct {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;         /* always 0x0F */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t zero;
    uint16_t name3[2];
} FAT_LFN_Entry;
#pragma pack(pop)

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

typedef struct {
    FILE     *fp;
    uint64_t  part_offset;
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  cluster_size;
    uint32_t  reserved_sectors;
    uint32_t  num_fats;
    uint32_t  fat_size;          /* in sectors */
    uint64_t  fat_offset;        /* byte offset of FAT #1 */
    uint64_t  data_offset;       /* byte offset of cluster 2 */
    uint32_t  root_cluster;      /* FAT32 only */
    uint64_t  root_dir_offset;   /* FAT12/16 root dir */
    uint32_t  root_entry_count;  /* FAT12/16 */
    uint32_t  total_clusters;
    int       fat_type;          /* 12, 16, or 32 */
    uint8_t  *fat_table;         /* loaded FAT */
    size_t    fat_table_size;
    uint64_t  total_data_size;
    uint64_t  extracted_size;
    ExtractCallbacks *cb;
} FATContext;

/* ── cluster chain traversal ───────────────────────────────────────── */
static uint32_t fat_next_cluster(FATContext *ctx, uint32_t cluster)
{
    if (ctx->fat_type == 32) {
        if (cluster * 4 + 3 >= ctx->fat_table_size) return 0x0FFFFFFF;
        uint32_t val = le32toh(*(uint32_t *)(ctx->fat_table + cluster * 4));
        return val & 0x0FFFFFFF;
    } else if (ctx->fat_type == 16) {
        if (cluster * 2 + 1 >= ctx->fat_table_size) return 0xFFFF;
        return le16toh(*(uint16_t *)(ctx->fat_table + cluster * 2));
    } else { /* FAT12 */
        uint32_t off = cluster + (cluster / 2);
        if (off + 1 >= ctx->fat_table_size) return 0xFFF;
        uint16_t val = le16toh(*(uint16_t *)(ctx->fat_table + off));
        if (cluster & 1)
            val >>= 4;
        else
            val &= 0x0FFF;
        return val;
    }
}

static int fat_is_eoc(FATContext *ctx, uint32_t cluster)
{
    if (ctx->fat_type == 32) return cluster >= 0x0FFFFFF8;
    if (ctx->fat_type == 16) return cluster >= 0xFFF8;
    return cluster >= 0xFF8; /* FAT12 */
}

static uint64_t fat_cluster_offset(FATContext *ctx, uint32_t cluster)
{
    return ctx->data_offset + (uint64_t)(cluster - 2) * ctx->cluster_size;
}

/* ── read cluster data ─────────────────────────────────────────────── */
static int fat_read_cluster(FATContext *ctx, uint32_t cluster, uint8_t *buf)
{
    uint64_t off = fat_cluster_offset(ctx, cluster);
    _fseeki64(ctx->fp, (int64_t)off, SEEK_SET);
    return (fread(buf, 1, ctx->cluster_size, ctx->fp) == ctx->cluster_size);
}

/* ── read cluster chain into malloc'd buffer ───────────────────────── */
static uint8_t *fat_read_chain(FATContext *ctx, uint32_t start, size_t *outLen)
{
    /* count clusters */
    uint32_t cl = start;
    size_t count = 0;
    while (cl >= 2 && !fat_is_eoc(ctx, cl) && count < 1000000) {
        count++;
        cl = fat_next_cluster(ctx, cl);
    }

    size_t total = count * ctx->cluster_size;
    uint8_t *data = (uint8_t *)malloc(total);
    if (!data) return NULL;

    cl = start;
    size_t pos = 0;
    while (cl >= 2 && !fat_is_eoc(ctx, cl) && pos < total) {
        if (!fat_read_cluster(ctx, cl, data + pos)) break;
        pos += ctx->cluster_size;
        cl = fat_next_cluster(ctx, cl);
    }

    *outLen = total;
    return data;
}

/* ── parse LFN entries into a wide string ──────────────────────────── */
static void fat_collect_lfn(const FAT_LFN_Entry *entries, int count,
                            wchar_t *name, int maxLen)
{
    int pos = 0;
    /* LFN entries are in reverse order */
    for (int i = count - 1; i >= 0 && pos < maxLen - 1; i--) {
        const FAT_LFN_Entry *e = &entries[i];
        for (int j = 0; j < 5 && pos < maxLen - 1; j++) {
            uint16_t ch = le16toh(e->name1[j]);
            if (ch == 0 || ch == 0xFFFF) goto done;
            name[pos++] = (wchar_t)ch;
        }
        for (int j = 0; j < 6 && pos < maxLen - 1; j++) {
            uint16_t ch = le16toh(e->name2[j]);
            if (ch == 0 || ch == 0xFFFF) goto done;
            name[pos++] = (wchar_t)ch;
        }
        for (int j = 0; j < 2 && pos < maxLen - 1; j++) {
            uint16_t ch = le16toh(e->name3[j]);
            if (ch == 0 || ch == 0xFFFF) goto done;
            name[pos++] = (wchar_t)ch;
        }
    }
done:
    name[pos] = 0;
}

/* ── make 8.3 name into wide string ────────────────────────────────── */
static void fat_83_to_wide(const FAT_DirEntry *e, wchar_t *name, int maxLen)
{
    int pos = 0;
    for (int i = 0; i < 8 && pos < maxLen - 1; i++) {
        if (e->name[i] == ' ') break;
        name[pos++] = (wchar_t)(unsigned char)e->name[i];
    }
    /* check for extension */
    if (e->ext[0] != ' ') {
        name[pos++] = L'.';
        for (int i = 0; i < 3 && pos < maxLen - 1; i++) {
            if (e->ext[i] == ' ') break;
            name[pos++] = (wchar_t)(unsigned char)e->ext[i];
        }
    }
    name[pos] = 0;
}

/* ── recursive directory extraction ────────────────────────────────── */
static int fat_extract_dir(FATContext *ctx, const uint8_t *dirData, size_t dirLen,
                           const wchar_t *outDir)
{
    FAT_LFN_Entry lfn_buf[20];
    int lfn_count = 0;

    size_t num_entries = dirLen / 32;

    for (size_t i = 0; i < num_entries; i++) {
        const FAT_DirEntry *e = (const FAT_DirEntry *)(dirData + i * 32);

        /* end of directory */
        if (e->name[0] == 0x00) break;
        /* deleted entry */
        if ((uint8_t)e->name[0] == 0xE5) { lfn_count = 0; continue; }

        /* LFN entry */
        if (e->attr == ATTR_LFN) {
            const FAT_LFN_Entry *lfn = (const FAT_LFN_Entry *)e;
            if (lfn->order & 0x40) lfn_count = 0; /* first LFN entry */
            if (lfn_count < 20)
                lfn_buf[lfn_count++] = *lfn;
            continue;
        }

        /* skip volume label */
        if (e->attr & ATTR_VOLUME_ID) { lfn_count = 0; continue; }

        /* get name */
        wchar_t name[MAX_PATH];
        if (lfn_count > 0) {
            fat_collect_lfn(lfn_buf, lfn_count, name, MAX_PATH);
        } else {
            fat_83_to_wide(e, name, MAX_PATH);
        }
        lfn_count = 0;

        /* skip . and .. */
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
            continue;

        /* sanitize: replace illegal path chars */
        for (wchar_t *p = name; *p; p++) {
            if (*p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
                *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|')
                *p = L'_';
        }

        /* build full output path */
        wchar_t fullPath[MAX_PATH * 2];
        _snwprintf(fullPath, MAX_PATH * 2, L"%s\\%s", outDir, name);

        uint32_t cluster = ((uint32_t)le16toh(e->first_cluster_hi) << 16)
                         | (uint32_t)le16toh(e->first_cluster_lo);
        uint32_t fsize = le32toh(e->file_size);

        if (e->attr & ATTR_DIRECTORY) {
            /* recurse into subdirectory */
            ensure_directory(fullPath);
            if (cluster >= 2) {
                size_t subLen = 0;
                uint8_t *subData = fat_read_chain(ctx, cluster, &subLen);
                if (subData) {
                    fat_extract_dir(ctx, subData, subLen, fullPath);
                    free(subData);
                }
            }
        } else {
            /* extract file */
            if (cluster < 2 && fsize == 0) {
                /* empty file */
                write_extracted_file(fullPath, NULL, 0);
            } else if (cluster >= 2) {
                double pct = ctx->total_data_size > 0 ? (double)ctx->extracted_size / (double)ctx->total_data_size : 0;
                extract_progress(ctx->cb, pct, name);

                /* read file data from cluster chain */
                FILE *fout = _wfopen(fullPath, L"wb");
                if (fout) {
                    uint8_t *cbuf = (uint8_t *)malloc(ctx->cluster_size);
                    if (cbuf) {
                        uint32_t remaining = fsize;
                        uint32_t cl = cluster;
                        while (cl >= 2 && !fat_is_eoc(ctx, cl) && remaining > 0) {
                            if (fat_read_cluster(ctx, cl, cbuf)) {
                                uint32_t toWrite = remaining < ctx->cluster_size
                                    ? remaining : ctx->cluster_size;
                                fwrite(cbuf, 1, toWrite, fout);
                                remaining -= toWrite;
                                ctx->extracted_size += toWrite;
                            }
                            cl = fat_next_cluster(ctx, cl);
                        }
                        free(cbuf);
                    }
                    fclose(fout);

                    if (ctx->total_data_size > 0) {
                        double pct = (double)ctx->extracted_size / (double)ctx->total_data_size;
                        extract_progress(ctx->cb, pct, name);
                    }
                }
            }
        }
    }
    return 0;
}

int fs_extract_fat(FILE *fp, uint64_t offset, uint64_t size,
                   const wchar_t *outDir, ExtractCallbacks *cb)
{
    FATContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = fp;
    ctx.part_offset = offset;
    ctx.cb = cb;

    /* read BPB */
    FAT_BPB bpb;
    _fseeki64(fp, (int64_t)offset, SEEK_SET);
    if (fread(&bpb, 1, sizeof(bpb), fp) != sizeof(bpb)) {
        extract_log(cb, L"[FAT] Failed to read BPB");
        return -1;
    }

    ctx.bytes_per_sector    = le16toh(bpb.bytes_per_sector);
    ctx.sectors_per_cluster = bpb.sectors_per_cluster;
    ctx.reserved_sectors    = le16toh(bpb.reserved_sectors);
    ctx.num_fats            = bpb.num_fats;
    ctx.root_entry_count    = le16toh(bpb.root_entry_count);

    if (ctx.bytes_per_sector == 0 || ctx.sectors_per_cluster == 0) {
        extract_log(cb, L"[FAT] Invalid BPB parameters");
        return -1;
    }

    ctx.cluster_size = ctx.bytes_per_sector * ctx.sectors_per_cluster;

    /* determine FAT size */
    uint32_t fat16_sz = le16toh(bpb.fat_size_16);
    uint32_t fat32_sz = le32toh(bpb.fat_size_32);
    ctx.fat_size = fat16_sz ? fat16_sz : fat32_sz;

    /* total sectors */
    uint32_t total16 = le16toh(bpb.total_sectors_16);
    uint32_t total32 = le32toh(bpb.total_sectors_32);
    uint32_t total_sectors = total16 ? total16 : total32;

    /* root dir sectors (FAT12/16) */
    uint32_t root_dir_sectors = ((ctx.root_entry_count * 32) +
        (ctx.bytes_per_sector - 1)) / ctx.bytes_per_sector;

    uint32_t first_data_sector = ctx.reserved_sectors +
        (ctx.num_fats * ctx.fat_size) + root_dir_sectors;

    uint32_t data_sectors = total_sectors - first_data_sector;
    ctx.total_clusters = data_sectors / ctx.sectors_per_cluster;

    /* determine FAT type */
    if (ctx.total_clusters < 4085)
        ctx.fat_type = 12;
    else if (ctx.total_clusters < 65525)
        ctx.fat_type = 16;
    else
        ctx.fat_type = 32;

    ctx.fat_offset = offset + (uint64_t)ctx.reserved_sectors * ctx.bytes_per_sector;
    ctx.data_offset = offset + (uint64_t)first_data_sector * ctx.bytes_per_sector;

    if (ctx.fat_type != 32) {
        ctx.root_dir_offset = offset +
            (uint64_t)(ctx.reserved_sectors + ctx.num_fats * ctx.fat_size)
            * ctx.bytes_per_sector;
    } else {
        ctx.root_cluster = le32toh(bpb.root_cluster);
    }

    ctx.total_data_size = (uint64_t)data_sectors * ctx.bytes_per_sector;

    extract_log(cb, L"[FAT] Type: FAT%d, Cluster size: %u, Total clusters: %u",
                ctx.fat_type, ctx.cluster_size, ctx.total_clusters);

    /* load FAT table */
    ctx.fat_table_size = (size_t)ctx.fat_size * ctx.bytes_per_sector;
    ctx.fat_table = (uint8_t *)malloc(ctx.fat_table_size);
    if (!ctx.fat_table) {
        extract_log(cb, L"[FAT] Out of memory for FAT table (%zu bytes)",
                    ctx.fat_table_size);
        return -1;
    }
    _fseeki64(fp, (int64_t)ctx.fat_offset, SEEK_SET);
    if (fread(ctx.fat_table, 1, ctx.fat_table_size, fp) != ctx.fat_table_size) {
        extract_log(cb, L"[FAT] Failed to read FAT table");
        free(ctx.fat_table);
        return -1;
    }

    /* read root directory */
    ensure_directory(outDir);

    if (ctx.fat_type == 32) {
        /* root is a cluster chain */
        size_t rootLen = 0;
        uint8_t *rootData = fat_read_chain(&ctx, ctx.root_cluster, &rootLen);
        if (rootData) {
            fat_extract_dir(&ctx, rootData, rootLen, outDir);
            free(rootData);
        }
    } else {
        /* root is fixed at root_dir_offset */
        size_t rootSize = (size_t)ctx.root_entry_count * 32;
        uint8_t *rootData = (uint8_t *)malloc(rootSize);
        if (rootData) {
            _fseeki64(fp, (int64_t)ctx.root_dir_offset, SEEK_SET);
            if (fread(rootData, 1, rootSize, fp) == rootSize)
                fat_extract_dir(&ctx, rootData, rootSize, outDir);
            free(rootData);
        }
    }

    free(ctx.fat_table);
    extract_log(cb, L"[FAT] Extraction complete");
    return 0;
}

/* ═══════════════════════ exFAT ══════════════════════════════════════ */

#pragma pack(push, 1)
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];        /* "EXFAT   " */
    uint8_t  zeros[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;     /* in sectors */
    uint32_t fat_length;     /* in sectors */
    uint32_t cluster_heap_offset; /* in sectors */
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;
    uint8_t  sectors_per_cluster_shift;
    uint8_t  num_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved[7];
} ExFAT_Boot;

/* exFAT directory entry types */
#define EXFAT_ENTRY_EOD         0x00
#define EXFAT_ENTRY_ALLOC_BMP   0x81
#define EXFAT_ENTRY_UPCASE      0x82
#define EXFAT_ENTRY_LABEL       0x83
#define EXFAT_ENTRY_FILE        0x85
#define EXFAT_ENTRY_STREAM      0xC0
#define EXFAT_ENTRY_FILENAME    0xC1

typedef struct {
    uint8_t  type;
    uint8_t  secondary_count;
    uint16_t set_checksum;
    uint16_t attrs;
    uint16_t reserved1;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t access_time;
    uint16_t access_date;
    uint8_t  create_10ms;
    uint8_t  modify_10ms;
    uint8_t  create_utc_offset;
    uint8_t  modify_utc_offset;
    uint8_t  access_utc_offset;
    uint8_t  reserved2[7];
} ExFAT_FileEntry;

typedef struct {
    uint8_t  type;           /* 0xC0 */
    uint8_t  flags;          /* bit 0: alloc possible, bit 1: no FAT chain */
    uint8_t  reserved1;
    uint8_t  name_length;
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} ExFAT_StreamEntry;

typedef struct {
    uint8_t  type;           /* 0xC1 */
    uint8_t  flags;
    uint16_t name[15];
} ExFAT_FilenameEntry;
#pragma pack(pop)

typedef struct {
    FILE     *fp;
    uint64_t  part_offset;
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  cluster_size;
    uint64_t  fat_byte_offset;
    uint64_t  heap_byte_offset;
    uint32_t  cluster_count;
    uint32_t  root_dir_cluster;
    uint32_t *fat_table;
    size_t    fat_entries;
    uint64_t  total_data_size;
    uint64_t  extracted_size;
    ExtractCallbacks *cb;
} ExFATContext;

static uint32_t exfat_next_cluster(ExFATContext *ctx, uint32_t cl)
{
    if (cl < 2 || cl - 2 >= ctx->fat_entries) return 0xFFFFFFFF;
    return le32toh(ctx->fat_table[cl]);
}

static uint64_t exfat_cluster_offset(ExFATContext *ctx, uint32_t cl)
{
    return ctx->heap_byte_offset + (uint64_t)(cl - 2) * ctx->cluster_size;
}

static int exfat_read_cluster(ExFATContext *ctx, uint32_t cl, uint8_t *buf)
{
    _fseeki64(ctx->fp, (int64_t)exfat_cluster_offset(ctx, cl), SEEK_SET);
    return (fread(buf, 1, ctx->cluster_size, ctx->fp) == ctx->cluster_size);
}

static uint8_t *exfat_read_chain(ExFATContext *ctx, uint32_t start,
                                  int no_fat_chain, uint64_t data_len,
                                  size_t *outLen)
{
    if (no_fat_chain) {
        /* contiguous allocation */
        uint32_t n_clusters = (uint32_t)((data_len + ctx->cluster_size - 1) / ctx->cluster_size);
        size_t total = (size_t)n_clusters * ctx->cluster_size;
        uint8_t *buf = (uint8_t *)malloc(total);
        if (!buf) return NULL;
        _fseeki64(ctx->fp, (int64_t)exfat_cluster_offset(ctx, start), SEEK_SET);
        fread(buf, 1, total, ctx->fp);
        *outLen = total;
        return buf;
    }

    /* follow FAT chain */
    uint32_t cl = start;
    size_t count = 0;
    while (cl >= 2 && cl < 0xFFFFFFF8 && count < 1000000) {
        count++;
        cl = exfat_next_cluster(ctx, cl);
    }
    size_t total = count * ctx->cluster_size;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;

    cl = start;
    size_t pos = 0;
    while (cl >= 2 && cl < 0xFFFFFFF8 && pos < total) {
        exfat_read_cluster(ctx, cl, buf + pos);
        pos += ctx->cluster_size;
        cl = exfat_next_cluster(ctx, cl);
    }
    *outLen = total;
    return buf;
}

static int exfat_extract_dir(ExFATContext *ctx, const uint8_t *dirData,
                              size_t dirLen, const wchar_t *outDir)
{
    size_t i = 0;
    size_t num = dirLen / 32;

    while (i < num) {
        uint8_t type = dirData[i * 32];
        if (type == EXFAT_ENTRY_EOD) break;
        if (type != EXFAT_ENTRY_FILE) { i++; continue; }

        const ExFAT_FileEntry *fe = (const ExFAT_FileEntry *)(dirData + i * 32);
        int sec_count = fe->secondary_count;
        uint16_t attrs = le16toh(fe->attrs);
        i++;

        /* expect stream entry next */
        if (i >= num) break;
        type = dirData[i * 32];
        if (type != EXFAT_ENTRY_STREAM) { continue; }

        const ExFAT_StreamEntry *se = (const ExFAT_StreamEntry *)(dirData + i * 32);
        uint32_t first_cl = le32toh(se->first_cluster);
        uint64_t data_len = le64toh(se->data_length);
        uint8_t  name_len = se->name_length;
        int no_fat_chain = (se->flags & 0x02) ? 1 : 0;
        i++;

        /* collect filename entries */
        wchar_t name[256];
        int npos = 0;
        for (int si = 1; si < sec_count && i < num; si++, i++) {
            type = dirData[i * 32];
            if (type == EXFAT_ENTRY_FILENAME) {
                const ExFAT_FilenameEntry *fn =
                    (const ExFAT_FilenameEntry *)(dirData + i * 32);
                for (int j = 0; j < 15 && npos < 255 && npos < name_len; j++) {
                    name[npos++] = (wchar_t)le16toh(fn->name[j]);
                }
            }
        }
        name[npos] = 0;

        if (npos == 0) continue;

        /* skip . and .. */
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
            continue;

        /* sanitize */
        for (wchar_t *p = name; *p; p++) {
            if (*p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
                *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|')
                *p = L'_';
        }

        wchar_t fullPath[MAX_PATH * 2];
        _snwprintf(fullPath, MAX_PATH * 2, L"%s\\%s", outDir, name);

        if (attrs & ATTR_DIRECTORY) {
            ensure_directory(fullPath);
            if (first_cl >= 2) {
                size_t subLen = 0;
                uint8_t *subData = exfat_read_chain(ctx, first_cl,
                    no_fat_chain, data_len, &subLen);
                if (subData) {
                    exfat_extract_dir(ctx, subData,
                        (size_t)(data_len < subLen ? data_len : subLen), fullPath);
                    free(subData);
                }
            }
        } else {
            if (first_cl >= 2 && data_len > 0) {
                double pct = ctx->total_data_size > 0 ? (double)ctx->extracted_size / (double)ctx->total_data_size : 0;
                extract_progress(ctx->cb, pct, name);
                FILE *fout = _wfopen(fullPath, L"wb");
                if (fout) {
                    uint8_t *cbuf = (uint8_t *)malloc(ctx->cluster_size);
                    if (cbuf) {
                        uint64_t remaining = data_len;
                        uint32_t cl = first_cl;
                        while (cl >= 2 && cl < 0xFFFFFFF8 && remaining > 0) {
                            exfat_read_cluster(ctx, cl, cbuf);
                            uint32_t toWrite = remaining < ctx->cluster_size
                                ? (uint32_t)remaining : ctx->cluster_size;
                            fwrite(cbuf, 1, toWrite, fout);
                            remaining -= toWrite;
                            ctx->extracted_size += toWrite;
                            if (no_fat_chain)
                                cl++;
                            else
                                cl = exfat_next_cluster(ctx, cl);
                        }
                        free(cbuf);
                    }
                    fclose(fout);
                }
            } else {
                write_extracted_file(fullPath, NULL, 0);
            }
        }
    }
    return 0;
}

int fs_extract_exfat(FILE *fp, uint64_t offset, uint64_t size,
                     const wchar_t *outDir, ExtractCallbacks *cb)
{
    ExFATContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = fp;
    ctx.part_offset = offset;
    ctx.cb = cb;

    ExFAT_Boot boot;
    _fseeki64(fp, (int64_t)offset, SEEK_SET);
    if (fread(&boot, 1, sizeof(boot), fp) != sizeof(boot)) {
        extract_log(cb, L"[exFAT] Failed to read boot sector");
        return -1;
    }

    ctx.bytes_per_sector = 1u << boot.bytes_per_sector_shift;
    ctx.sectors_per_cluster = 1u << boot.sectors_per_cluster_shift;
    ctx.cluster_size = ctx.bytes_per_sector * ctx.sectors_per_cluster;
    ctx.cluster_count = le32toh(boot.cluster_count);
    ctx.root_dir_cluster = le32toh(boot.root_dir_cluster);

    ctx.fat_byte_offset = offset +
        (uint64_t)le32toh(boot.fat_offset) * ctx.bytes_per_sector;
    ctx.heap_byte_offset = offset +
        (uint64_t)le32toh(boot.cluster_heap_offset) * ctx.bytes_per_sector;

    ctx.total_data_size = (uint64_t)ctx.cluster_count * ctx.cluster_size;

    extract_log(cb, L"[exFAT] Cluster size: %u, Clusters: %u",
                ctx.cluster_size, ctx.cluster_count);

    /* load FAT */
    uint32_t fat_len = le32toh(boot.fat_length);
    size_t fat_bytes = (size_t)fat_len * ctx.bytes_per_sector;
    ctx.fat_entries = fat_bytes / 4;
    ctx.fat_table = (uint32_t *)malloc(fat_bytes);
    if (!ctx.fat_table) {
        extract_log(cb, L"[exFAT] Out of memory for FAT");
        return -1;
    }
    _fseeki64(fp, (int64_t)ctx.fat_byte_offset, SEEK_SET);
    fread(ctx.fat_table, 1, fat_bytes, fp);

    ensure_directory(outDir);

    /* read root directory */
    size_t rootLen = 0;
    uint8_t *rootData = exfat_read_chain(&ctx, ctx.root_dir_cluster,
        0, (uint64_t)ctx.cluster_size * 256, &rootLen);
    if (rootData) {
        exfat_extract_dir(&ctx, rootData, rootLen, outDir);
        free(rootData);
    }

    free(ctx.fat_table);
    extract_log(cb, L"[exFAT] Extraction complete");
    return 0;
}
