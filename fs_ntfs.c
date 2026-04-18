/*
 * fs_ntfs.c — Basic NTFS read-only extraction
 * Supports: resident + non-resident $DATA, $INDEX_ROOT/$INDEX_ALLOCATION dirs
 * Skips:    compressed, encrypted, sparse, alternate data streams
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#include "endian.h"
#include "imgextract.h"
#include "fs_ntfs.h"

#pragma pack(push, 1)
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];        /* "NTFS    " */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  zero1[3];
    uint16_t zero2;
    uint8_t  media;
    uint16_t zero3;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t zero4;
    uint32_t zero5;
    uint64_t total_sectors;
    uint64_t mft_lcn;
    uint64_t mft_mirror_lcn;
    int8_t   clusters_per_mft_record;
    uint8_t  pad1[3];
    int8_t   clusters_per_index_record;
    uint8_t  pad2[3];
    uint64_t volume_serial;
    uint32_t checksum;
} NTFS_Boot;

typedef struct {
    uint32_t magic;          /* "FILE" = 0x454C4946 */
    uint16_t fixup_offset;
    uint16_t fixup_count;
    uint64_t lsn;
    uint16_t sequence;
    uint16_t hard_links;
    uint16_t first_attr_offset;
    uint16_t flags;          /* 0x01=in-use, 0x02=directory */
    uint32_t used_size;
    uint32_t allocated_size;
    uint64_t base_record;
    uint16_t next_attr_id;
} NTFS_FileRecord;

typedef struct {
    uint32_t type;           /* attribute type */
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
} NTFS_AttrHeader;

typedef struct {
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  indexed;
    uint8_t  pad;
} NTFS_ResidentAttr;

typedef struct {
    uint64_t start_vcn;
    uint64_t last_vcn;
    uint16_t data_runs_offset;
    uint16_t compression_unit;
    uint32_t pad;
    uint64_t alloc_size;
    uint64_t real_size;
    uint64_t init_size;
} NTFS_NonResidentAttr;
#pragma pack(pop)

/* attribute types */
#define ATTR_STANDARD_INFO  0x10
#define ATTR_FILE_NAME      0x30
#define ATTR_DATA           0x80
#define ATTR_INDEX_ROOT     0x90
#define ATTR_INDEX_ALLOC    0xA0
#define ATTR_END            0xFFFFFFFF

/* file record flags */
#define FILE_RECORD_IN_USE  0x0001
#define FILE_RECORD_IS_DIR  0x0002

typedef struct {
    FILE     *fp;
    uint64_t  part_offset;
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  cluster_size;
    uint32_t  mft_record_size;
    uint64_t  mft_offset;    /* byte offset of MFT start */
    uint64_t  total_data_size;
    uint64_t  extracted_size;
    ExtractCallbacks *cb;
} NTFSContext;

/* ── fixup: correct sector-end bytes ───────────────────────────────── */
static void ntfs_apply_fixup(uint8_t *record, uint32_t record_size,
                              uint16_t fixup_offset, uint16_t fixup_count,
                              uint32_t sector_size)
{
    if (fixup_count < 2 || fixup_offset + fixup_count * 2 > record_size)
        return;
    uint16_t *fixup = (uint16_t *)(record + fixup_offset);
    uint16_t sig = fixup[0]; (void)sig;
    for (uint16_t i = 1; i < fixup_count; i++) {
        uint32_t pos = i * sector_size - 2;
        if (pos + 1 < record_size)
            *(uint16_t *)(record + pos) = fixup[i];
    }
}

/* ── read an MFT record by index ───────────────────────────────────── */
static uint8_t *ntfs_read_mft_record(NTFSContext *ctx, uint64_t index)
{
    uint8_t *rec = (uint8_t *)malloc(ctx->mft_record_size);
    if (!rec) return NULL;

    uint64_t off = ctx->mft_offset + index * ctx->mft_record_size;
    _fseeki64(ctx->fp, (int64_t)off, SEEK_SET);
    if (fread(rec, 1, ctx->mft_record_size, ctx->fp) != ctx->mft_record_size) {
        free(rec);
        return NULL;
    }

    NTFS_FileRecord *hdr = (NTFS_FileRecord *)rec;
    if (le32toh(hdr->magic) != 0x454C4946) { /* "FILE" */
        free(rec);
        return NULL;
    }

    ntfs_apply_fixup(rec, ctx->mft_record_size,
                     le16toh(hdr->fixup_offset),
                     le16toh(hdr->fixup_count),
                     ctx->bytes_per_sector);
    return rec;
}

/* ── find attribute in MFT record ──────────────────────────────────── */
static NTFS_AttrHeader *ntfs_find_attr(uint8_t *record, uint32_t record_size,
                                        uint32_t type)
{
    NTFS_FileRecord *hdr = (NTFS_FileRecord *)record;
    uint16_t off = le16toh(hdr->first_attr_offset);

    while (off + sizeof(NTFS_AttrHeader) <= record_size) {
        NTFS_AttrHeader *attr = (NTFS_AttrHeader *)(record + off);
        uint32_t atype = le32toh(attr->type);
        uint32_t alen  = le32toh(attr->length);

        if (atype == ATTR_END || alen == 0) break;
        if (atype == type) return attr;
        off += alen;
    }
    return NULL;
}

/* ── decode data runs → read non-resident data ─────────────────────── */
static uint8_t *ntfs_read_data_runs(NTFSContext *ctx, uint8_t *runs,
                                     uint32_t runs_len, uint64_t real_size,
                                     size_t *outLen)
{
    uint8_t *data = (uint8_t *)calloc(1, (size_t)real_size);
    if (!data) return NULL;

    uint8_t *p = runs;
    uint8_t *end = runs + runs_len;
    int64_t prev_lcn = 0;
    size_t data_pos = 0;

    while (p < end && *p != 0 && data_pos < real_size) {
        uint8_t header = *p++;
        uint8_t len_size = header & 0x0F;
        uint8_t off_size = (header >> 4) & 0x0F;

        if (len_size == 0) break;

        /* read length */
        uint64_t run_length = 0;
        for (int i = 0; i < len_size && p < end; i++)
            run_length |= ((uint64_t)*p++) << (i * 8);

        /* read offset (signed) */
        int64_t run_offset = 0;
        if (off_size > 0) {
            for (int i = 0; i < off_size && p < end; i++)
                run_offset |= ((int64_t)*p++) << (i * 8);
            /* sign extend */
            if (off_size < 8 && (run_offset & ((int64_t)1 << (off_size * 8 - 1))))
                run_offset |= ~(((int64_t)1 << (off_size * 8)) - 1);
        }

        int64_t lcn = prev_lcn + run_offset;
        prev_lcn = lcn;

        if (off_size == 0) {
            /* sparse run — leave as zeros */
            size_t bytes = (size_t)(run_length * ctx->cluster_size);
            data_pos += bytes;
            continue;
        }

        /* read clusters */
        uint64_t byte_off = ctx->part_offset + (uint64_t)lcn * ctx->cluster_size;
        for (uint64_t c = 0; c < run_length && data_pos < real_size; c++) {
            size_t to_read = ctx->cluster_size;
            if (data_pos + to_read > real_size)
                to_read = (size_t)(real_size - data_pos);
            _fseeki64(ctx->fp, (int64_t)(byte_off + c * ctx->cluster_size), SEEK_SET);
            fread(data + data_pos, 1, to_read, ctx->fp);
            data_pos += to_read;
        }
    }

    *outLen = (size_t)real_size;
    return data;
}

/* ── read attribute data (resident or non-resident) ────────────────── */
static uint8_t *ntfs_read_attr_data(NTFSContext *ctx, uint8_t *record,
                                     uint32_t record_size, NTFS_AttrHeader *attr,
                                     size_t *outLen)
{
    uint32_t alen = le32toh(attr->length);
    if (!attr->non_resident) {
        /* resident */
        NTFS_ResidentAttr *res = (NTFS_ResidentAttr *)((uint8_t *)attr + sizeof(NTFS_AttrHeader));
        uint32_t vlen = le32toh(res->value_length);
        uint16_t voff = le16toh(res->value_offset);
        if (voff + vlen > alen) return NULL;
        uint8_t *data = (uint8_t *)malloc(vlen);
        if (!data) return NULL;
        memcpy(data, (uint8_t *)attr + voff, vlen);
        *outLen = vlen;
        return data;
    } else {
        /* non-resident */
        NTFS_NonResidentAttr *nres =
            (NTFS_NonResidentAttr *)((uint8_t *)attr + sizeof(NTFS_AttrHeader));
        uint16_t dr_off = le16toh(nres->data_runs_offset);
        uint64_t rsize  = le64toh(nres->real_size);
        uint8_t *runs   = (uint8_t *)attr + dr_off;
        uint32_t runs_len = alen - dr_off;
        return ntfs_read_data_runs(ctx, runs, runs_len, rsize, outLen);
    }
}

/* ── get filename from $FILE_NAME attribute ────────────────────────── */
static int ntfs_get_filename(uint8_t *record, uint32_t record_size,
                              wchar_t *name, int maxLen)
{
    NTFS_FileRecord *hdr = (NTFS_FileRecord *)record;
    uint16_t off = le16toh(hdr->first_attr_offset);
    wchar_t bestName[MAX_PATH] = {0};
    int bestNS = -1; /* prefer Win32 (1) or Win32+DOS (3) namespace */

    while (off + sizeof(NTFS_AttrHeader) <= record_size) {
        NTFS_AttrHeader *attr = (NTFS_AttrHeader *)(record + off);
        uint32_t atype = le32toh(attr->type);
        uint32_t alen  = le32toh(attr->length);
        if (atype == ATTR_END || alen == 0) break;

        if (atype == ATTR_FILE_NAME && !attr->non_resident) {
            NTFS_ResidentAttr *res = (NTFS_ResidentAttr *)((uint8_t *)attr + sizeof(NTFS_AttrHeader));
            uint16_t voff = le16toh(res->value_offset);
            uint32_t vlen = le32toh(res->value_length);
            uint8_t *val = (uint8_t *)attr + voff;

            if (vlen >= 66) {
                uint8_t ns = val[0x41]; /* namespace */
                uint8_t nlen = val[0x40]; /* name length in chars */
                uint16_t *uname = (uint16_t *)(val + 0x42);

                /* prefer Win32 (1) or Win32+DOS (3) over DOS (2) */
                if (ns != 2 || bestNS < 0) {
                    int take = nlen < maxLen - 1 ? nlen : maxLen - 1;
                    for (int i = 0; i < take; i++)
                        bestName[i] = (wchar_t)le16toh(uname[i]);
                    bestName[take] = 0;
                    bestNS = ns;
                }
            }
        }
        off += alen;
    }

    if (bestName[0]) {
        wcsncpy(name, bestName, maxLen);
        return 1;
    }
    return 0;
}

/* ── extract files from an index (directory) ───────────────────────── */
static int ntfs_extract_index_entries(NTFSContext *ctx, uint8_t *entries,
                                       size_t len, const wchar_t *outDir);

static int ntfs_extract_record(NTFSContext *ctx, uint64_t mft_index,
                                const wchar_t *outDir)
{
    uint8_t *rec = ntfs_read_mft_record(ctx, mft_index);
    if (!rec) return -1;

    NTFS_FileRecord *hdr = (NTFS_FileRecord *)rec;
    uint16_t flags = le16toh(hdr->flags);

    if (!(flags & FILE_RECORD_IN_USE)) { free(rec); return 0; }

    wchar_t name[MAX_PATH] = {0};
    ntfs_get_filename(rec, ctx->mft_record_size, name, MAX_PATH);
    if (!name[0]) { free(rec); return 0; }

    /* skip system files . and .. */
    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) { free(rec); return 0; }
    /* skip $MFT etc system files starting with $ */
    if (name[0] == L'$') { free(rec); return 0; }

    /* sanitize */
    for (wchar_t *p = name; *p; p++) {
        if (*p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
            *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|')
            *p = L'_';
    }

    wchar_t fullPath[MAX_PATH * 2];
    _snwprintf(fullPath, MAX_PATH * 2, L"%s\\%s", outDir, name);

    if (flags & FILE_RECORD_IS_DIR) {
        ensure_directory(fullPath);

        /* read $INDEX_ROOT for directory entries */
        NTFS_AttrHeader *idx_root = ntfs_find_attr(rec, ctx->mft_record_size, ATTR_INDEX_ROOT);
        if (idx_root) {
            size_t idxLen = 0;
            uint8_t *idxData = ntfs_read_attr_data(ctx, rec, ctx->mft_record_size, idx_root, &idxLen);
            if (idxData && idxLen > 32) {
                /* index root header: skip first 16 bytes (attr type, collation, size, clusters),
                   then index header at +16 */
                uint32_t entries_off = le32toh(*(uint32_t *)(idxData + 16 + 0));
                uint32_t entries_size = le32toh(*(uint32_t *)(idxData + 16 + 4));
                if (entries_off + 16 < idxLen) {
                    ntfs_extract_index_entries(ctx,
                        idxData + 16 + entries_off,
                        idxLen - 16 - entries_off, fullPath);
                }
                free(idxData);
            }
        }

        /* also check $INDEX_ALLOCATION for larger directories */
        NTFS_AttrHeader *idx_alloc = ntfs_find_attr(rec, ctx->mft_record_size, ATTR_INDEX_ALLOC);
        if (idx_alloc) {
            size_t allocLen = 0;
            uint8_t *allocData = ntfs_read_attr_data(ctx, rec, ctx->mft_record_size, idx_alloc, &allocLen);
            if (allocData && allocLen > 0) {
                /* INDEX_ALLOCATION contains INDX records (4096 bytes each typically) */
                size_t pos = 0;
                while (pos + 24 < allocLen) {
                    if (memcmp(allocData + pos, "INDX", 4) != 0) {
                        pos += 4096;
                        continue;
                    }
                    /* apply fixup for INDX record */
                    uint16_t fix_off = le16toh(*(uint16_t *)(allocData + pos + 4));
                    uint16_t fix_cnt = le16toh(*(uint16_t *)(allocData + pos + 6));
                    ntfs_apply_fixup(allocData + pos, 4096, fix_off, fix_cnt,
                                     ctx->bytes_per_sector);

                    uint32_t entry_off = le32toh(*(uint32_t *)(allocData + pos + 24 + 0));
                    uint32_t entry_sz  = le32toh(*(uint32_t *)(allocData + pos + 24 + 4));

                    if (entry_off + 24 < 4096) {
                        ntfs_extract_index_entries(ctx,
                            allocData + pos + 24 + entry_off,
                            4096 - 24 - entry_off, fullPath);
                    }
                    pos += 4096;
                }
                free(allocData);
            }
        }
    } else {
        /* extract file data */
        NTFS_AttrHeader *data_attr = ntfs_find_attr(rec, ctx->mft_record_size, ATTR_DATA);
        if (data_attr) {
            /* skip compressed/encrypted */
            uint16_t aflags = le16toh(data_attr->flags);
            if (aflags & 0x4001) {
                extract_log(ctx->cb, L"[NTFS] Skipping compressed/encrypted: %s", name);
                free(rec);
                return 0;
            }

            double pct = ctx->total_data_size > 0 ? (double)ctx->extracted_size / (double)ctx->total_data_size : 0;
            extract_progress(ctx->cb, pct, name);

            size_t dataLen = 0;
            uint8_t *fileData = ntfs_read_attr_data(ctx, rec, ctx->mft_record_size,
                                                     data_attr, &dataLen);
            if (fileData) {
                write_extracted_file(fullPath, fileData, dataLen);
                ctx->extracted_size += dataLen;
                free(fileData);
            }

            if (ctx->total_data_size > 0) {
                double pct = (double)ctx->extracted_size / (double)ctx->total_data_size;
                extract_progress(ctx->cb, pct, name);
            }
        } else {
            /* file with no $DATA (possibly zero-length) */
            write_extracted_file(fullPath, NULL, 0);
        }
    }

    free(rec);
    return 0;
}

static int ntfs_extract_index_entries(NTFSContext *ctx, uint8_t *entries,
                                       size_t len, const wchar_t *outDir)
{
    size_t pos = 0;
    while (pos + 16 <= len) {
        uint64_t mft_ref = le64toh(*(uint64_t *)(entries + pos));
        uint16_t entry_len = le16toh(*(uint16_t *)(entries + pos + 8));
        uint16_t content_len = le16toh(*(uint16_t *)(entries + pos + 10));
        uint32_t entry_flags = le32toh(*(uint32_t *)(entries + pos + 12));

        if (entry_len < 16 || entry_len > len - pos) break;

        uint64_t mft_index = mft_ref & 0x0000FFFFFFFFFFFF;  /* 48-bit index */

        /* valid file records start at index 24+ (skip system metafiles) */
        if (mft_index >= 24 && content_len > 0) {
            ntfs_extract_record(ctx, mft_index, outDir);
        }

        /* last entry flag */
        if (entry_flags & 2) break;

        pos += entry_len;
    }
    return 0;
}

int fs_extract_ntfs(FILE *fp, uint64_t offset, uint64_t size,
                    const wchar_t *outDir, ExtractCallbacks *cb)
{
    NTFSContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = fp;
    ctx.part_offset = offset;
    ctx.cb = cb;

    NTFS_Boot boot;
    _fseeki64(fp, (int64_t)offset, SEEK_SET);
    if (fread(&boot, 1, sizeof(boot), fp) != sizeof(boot)) {
        extract_log(cb, L"[NTFS] Failed to read boot sector");
        return -1;
    }

    ctx.bytes_per_sector = le16toh(boot.bytes_per_sector);
    ctx.sectors_per_cluster = boot.sectors_per_cluster;
    ctx.cluster_size = ctx.bytes_per_sector * ctx.sectors_per_cluster;

    /* MFT record size */
    if (boot.clusters_per_mft_record >= 0)
        ctx.mft_record_size = (uint32_t)boot.clusters_per_mft_record * ctx.cluster_size;
    else
        ctx.mft_record_size = 1u << (uint32_t)(-boot.clusters_per_mft_record);

    ctx.mft_offset = offset + le64toh(boot.mft_lcn) * ctx.cluster_size;
    ctx.total_data_size = size;

    extract_log(cb, L"[NTFS] Cluster size: %u, MFT record: %u bytes",
                ctx.cluster_size, ctx.mft_record_size);
    extract_log(cb, L"[NTFS] MFT at offset 0x%llX", ctx.mft_offset - offset);

    ensure_directory(outDir);

    /* Extract by reading the root directory (MFT record 5) */
    ntfs_extract_record(&ctx, 5, outDir);

    extract_log(cb, L"[NTFS] Extraction complete");
    return 0;
}
