/*
 * fs_ext.c — ext2/ext3/ext4 read-only extraction
 * Supports: direct/indirect/double-indirect blocks, ext4 extents, symlinks
 * Skips:    symlinks (logged), inline data
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#include "endian.h"
#include "imgextract.h"
#include "fs_ext.h"

/* ── ext superblock ────────────────────────────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;             /* 0xEF53 */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT4_DYNAMIC_REV */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    /* ... more fields follow but we don't need them */
} ExtSuperblock;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ExtGroupDesc;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];   /* direct[0-11], indirect[12], double[13], triple[14] */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;     /* i_size_high for regular files in ext4 */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ExtInode;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    /* name follows, up to 255 bytes */
} ExtDirEntry;

/* ext4 extent structures */
typedef struct {
    uint16_t eh_magic;    /* 0xF30A */
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ExtExtentHeader;

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ExtExtent;

typedef struct {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ExtExtentIdx;
#pragma pack(pop)

/* inode mode flags */
#define EXT_S_IFMT   0xF000
#define EXT_S_IFREG  0x8000
#define EXT_S_IFDIR  0x4000
#define EXT_S_IFLNK  0xA000

/* inode flags */
#define EXT4_EXTENTS_FL 0x00080000

/* file_type in dir entry */
#define EXT_FT_UNKNOWN  0
#define EXT_FT_REG_FILE 1
#define EXT_FT_DIR      2
#define EXT_FT_SYMLINK  7

typedef struct {
    FILE      *fp;
    uint64_t   part_offset;
    uint32_t   block_size;
    uint32_t   inode_size;
    uint32_t   inodes_per_group;
    uint32_t   blocks_per_group;
    uint32_t   num_groups;
    ExtGroupDesc *gdt;     /* group descriptor table */
    uint64_t   total_data_size;
    uint64_t   extracted_size;
    int        has_extents;
    ExtractCallbacks *cb;
} ExtContext;

/* ── read block ────────────────────────────────────────────────────── */
static int ext_read_block(ExtContext *ctx, uint64_t block, uint8_t *buf)
{
    uint64_t off = ctx->part_offset + block * ctx->block_size;
    _fseeki64(ctx->fp, (int64_t)off, SEEK_SET);
    return (fread(buf, 1, ctx->block_size, ctx->fp) == ctx->block_size);
}

/* ── read inode ────────────────────────────────────────────────────── */
static int ext_read_inode(ExtContext *ctx, uint32_t ino, ExtInode *inode)
{
    if (ino == 0) return 0;
    uint32_t group = (ino - 1) / ctx->inodes_per_group;
    uint32_t index = (ino - 1) % ctx->inodes_per_group;

    if (group >= ctx->num_groups) return 0;

    uint64_t table_block = le32toh(ctx->gdt[group].bg_inode_table);
    uint64_t off = ctx->part_offset +
        table_block * ctx->block_size +
        (uint64_t)index * ctx->inode_size;

    _fseeki64(ctx->fp, (int64_t)off, SEEK_SET);
    /* read only the base inode structure (128 bytes) even if inode_size > 128 */
    memset(inode, 0, sizeof(*inode));
    return (fread(inode, 1, sizeof(*inode), ctx->fp) == sizeof(*inode));
}

/* ── get file size (ext4 uses i_dir_acl as i_size_high) ────────────── */
static uint64_t ext_file_size(ExtInode *inode)
{
    uint64_t sz = le32toh(inode->i_size);
    uint16_t mode = le16toh(inode->i_mode);
    if ((mode & EXT_S_IFMT) == EXT_S_IFREG) {
        sz |= (uint64_t)le32toh(inode->i_dir_acl) << 32;
    }
    return sz;
}

/* ── read blocks from extent tree (ext4) ───────────────────────────── */
static int ext_read_extents(ExtContext *ctx, uint8_t *extent_data,
                             uint64_t file_size, uint8_t *outBuf, size_t outLen)
{
    ExtExtentHeader *hdr = (ExtExtentHeader *)extent_data;
    uint16_t magic   = le16toh(hdr->eh_magic);
    uint16_t entries = le16toh(hdr->eh_entries);
    uint16_t depth   = le16toh(hdr->eh_depth);

    if (magic != 0xF30A) return -1;

    if (depth == 0) {
        /* leaf: contains actual extents */
        ExtExtent *ext = (ExtExtent *)(extent_data + 12);
        for (uint16_t i = 0; i < entries; i++) {
            uint32_t lblock = le32toh(ext[i].ee_block);
            uint16_t len    = le16toh(ext[i].ee_len);
            uint64_t pblock = ((uint64_t)le16toh(ext[i].ee_start_hi) << 32)
                            | le32toh(ext[i].ee_start_lo);

            /* uninitialized extent (len high bit set) — skip */
            uint16_t real_len = len > 32768 ? len - 32768 : len;

            for (uint16_t b = 0; b < real_len; b++) {
                uint64_t file_off = (uint64_t)(lblock + b) * ctx->block_size;
                if (file_off >= outLen) break;
                size_t to_read = ctx->block_size;
                if (file_off + to_read > outLen)
                    to_read = outLen - (size_t)file_off;

                uint64_t disk_off = ctx->part_offset + (pblock + b) * ctx->block_size;
                _fseeki64(ctx->fp, (int64_t)disk_off, SEEK_SET);
                fread(outBuf + file_off, 1, to_read, ctx->fp);
            }
        }
    } else {
        /* internal node: index entries pointing to child nodes */
        ExtExtentIdx *idx = (ExtExtentIdx *)(extent_data + 12);
        uint8_t *child = (uint8_t *)malloc(ctx->block_size);
        if (!child) return -1;

        for (uint16_t i = 0; i < entries; i++) {
            uint64_t child_block = ((uint64_t)le16toh(idx[i].ei_leaf_hi) << 32)
                                 | le32toh(idx[i].ei_leaf_lo);
            ext_read_block(ctx, child_block, child);
            ext_read_extents(ctx, child, file_size, outBuf, outLen);
        }
        free(child);
    }
    return 0;
}

/* ── read file data via indirect blocks (ext2/3) ───────────────────── */
static int ext_read_indirect(ExtContext *ctx, uint32_t block_no,
                              uint8_t *outBuf, uint64_t *pos, uint64_t file_size)
{
    if (block_no == 0 || *pos >= file_size) return 0;
    size_t to_read = ctx->block_size;
    if (*pos + to_read > file_size) to_read = (size_t)(file_size - *pos);
    uint64_t off = ctx->part_offset + (uint64_t)block_no * ctx->block_size;
    _fseeki64(ctx->fp, (int64_t)off, SEEK_SET);
    fread(outBuf + *pos, 1, to_read, ctx->fp);
    *pos += to_read;
    return 1;
}

static int ext_read_indirect_block(ExtContext *ctx, uint32_t ind_block,
                                    uint8_t *outBuf, uint64_t *pos,
                                    uint64_t file_size)
{
    if (ind_block == 0 || *pos >= file_size) return 0;
    uint32_t ptrs_per_block = ctx->block_size / 4;
    uint32_t *ptrs = (uint32_t *)malloc(ctx->block_size);
    if (!ptrs) return -1;
    ext_read_block(ctx, ind_block, (uint8_t *)ptrs);
    for (uint32_t i = 0; i < ptrs_per_block && *pos < file_size; i++) {
        uint32_t b = le32toh(ptrs[i]);
        if (b) ext_read_indirect(ctx, b, outBuf, pos, file_size);
    }
    free(ptrs);
    return 0;
}

static int ext_read_double_indirect(ExtContext *ctx, uint32_t dind_block,
                                     uint8_t *outBuf, uint64_t *pos,
                                     uint64_t file_size)
{
    if (dind_block == 0 || *pos >= file_size) return 0;
    uint32_t ptrs_per_block = ctx->block_size / 4;
    uint32_t *ptrs = (uint32_t *)malloc(ctx->block_size);
    if (!ptrs) return -1;
    ext_read_block(ctx, dind_block, (uint8_t *)ptrs);
    for (uint32_t i = 0; i < ptrs_per_block && *pos < file_size; i++) {
        uint32_t b = le32toh(ptrs[i]);
        if (b) ext_read_indirect_block(ctx, b, outBuf, pos, file_size);
    }
    free(ptrs);
    return 0;
}

/* ── read entire file data ─────────────────────────────────────────── */
static uint8_t *ext_read_file_data(ExtContext *ctx, ExtInode *inode,
                                    uint64_t file_size, size_t *outLen)
{
    if (file_size == 0) { *outLen = 0; return NULL; }
    /* cap at 2GB for sanity */
    if (file_size > 0x80000000ULL) { *outLen = 0; return NULL; }

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)file_size);
    if (!buf) { *outLen = 0; return NULL; }

    uint32_t iflags = le32toh(inode->i_flags);

    if (iflags & EXT4_EXTENTS_FL) {
        /* ext4 extents stored in i_block[0..14] (60 bytes) */
        ext_read_extents(ctx, (uint8_t *)inode->i_block, file_size,
                         buf, (size_t)file_size);
    } else {
        /* traditional direct + indirect blocks */
        uint64_t pos = 0;

        /* 12 direct blocks */
        for (int i = 0; i < 12 && pos < file_size; i++) {
            uint32_t b = le32toh(inode->i_block[i]);
            if (b) ext_read_indirect(ctx, b, buf, &pos, file_size);
        }

        /* single indirect */
        if (pos < file_size) {
            uint32_t ind = le32toh(inode->i_block[12]);
            ext_read_indirect_block(ctx, ind, buf, &pos, file_size);
        }

        /* double indirect */
        if (pos < file_size) {
            uint32_t dind = le32toh(inode->i_block[13]);
            ext_read_double_indirect(ctx, dind, buf, &pos, file_size);
        }

        /* triple indirect: skip for now (extremely rare) */
    }

    *outLen = (size_t)file_size;
    return buf;
}

/* ── recursive directory extraction ────────────────────────────────── */
static int ext_extract_dir(ExtContext *ctx, uint32_t dir_ino,
                           const wchar_t *outDir);

static int ext_extract_inode(ExtContext *ctx, uint32_t ino,
                              const wchar_t *name, const wchar_t *outDir)
{
    ExtInode inode;
    if (!ext_read_inode(ctx, ino, &inode)) return -1;

    uint16_t mode = le16toh(inode.i_mode);
    uint64_t fsize = ext_file_size(&inode);

    wchar_t fullPath[MAX_PATH * 2];
    _snwprintf(fullPath, MAX_PATH * 2, L"%s\\%s", outDir, name);

    if ((mode & EXT_S_IFMT) == EXT_S_IFDIR) {
        ensure_directory(fullPath);
        ext_extract_dir(ctx, ino, fullPath);
    } else if ((mode & EXT_S_IFMT) == EXT_S_IFREG) {
        double pct = ctx->total_data_size > 0 ? (double)ctx->extracted_size / (double)ctx->total_data_size : 0;
        extract_progress(ctx->cb, pct, name);

        size_t dataLen = 0;
        uint8_t *data = ext_read_file_data(ctx, &inode, fsize, &dataLen);
        if (data) {
            write_extracted_file(fullPath, data, dataLen);
            ctx->extracted_size += dataLen;
            free(data);
        } else if (fsize == 0) {
            write_extracted_file(fullPath, NULL, 0);
        }

        if (ctx->total_data_size > 0) {
            double pct = (double)ctx->extracted_size / (double)ctx->total_data_size;
            extract_progress(ctx->cb, pct, name);
        }
    } else if ((mode & EXT_S_IFMT) == EXT_S_IFLNK) {
        /*
         * Symlinks: if the target path fits in i_block[] (60 bytes = "fast"
         * symlink), the path is stored inline. Otherwise it's in data blocks.
         * We save a small text file containing "-> <target>" so the user
         * can see what it pointed to, and also attempt to copy the target
         * file if it exists in the same directory.
         */
        char target[256];
        memset(target, 0, sizeof(target));

        if (fsize < 60) {
            /* fast symlink — target stored directly in i_block[] */
            memcpy(target, (char *)inode.i_block, (size_t)fsize);
            target[fsize] = 0;
        } else {
            /* slow symlink — read from data blocks */
            size_t dataLen = 0;
            uint8_t *data = ext_read_file_data(ctx, &inode, fsize, &dataLen);
            if (data) {
                size_t copyLen = dataLen < 255 ? dataLen : 255;
                memcpy(target, data, copyLen);
                target[copyLen] = 0;
                free(data);
            }
        }

        if (target[0]) {
            /* Write a .symlink text file with the target path */
            wchar_t symlinkPath[MAX_PATH * 2];
            _snwprintf(symlinkPath, MAX_PATH * 2, L"%s.symlink", fullPath);

            char content[300];
            int clen = snprintf(content, sizeof(content), "-> %s\n", target);
            write_extracted_file(symlinkPath, (const uint8_t *)content, clen);

            /*
             * If the target is a relative path in the same directory
             * (no slashes), check if we already extracted it.
             * If so, copy it so the symlink "works" on Windows.
             */
            if (!strchr(target, '/')) {
                wchar_t targetW[256];
                MultiByteToWideChar(CP_UTF8, 0, target, -1, targetW, 256);
                wchar_t srcPath[MAX_PATH * 2];
                _snwprintf(srcPath, MAX_PATH * 2, L"%s\\%s", outDir, targetW);
                if (GetFileAttributesW(srcPath) != INVALID_FILE_ATTRIBUTES) {
                    CopyFileW(srcPath, fullPath, FALSE);
                }
            }

            extract_log(ctx->cb, L"[ext] Symlink: %s -> %S", name, target);
        } else {
            extract_log(ctx->cb, L"[ext] Symlink (unreadable): %s", name);
        }
    }
    return 0;
}

static int ext_extract_dir(ExtContext *ctx, uint32_t dir_ino,
                           const wchar_t *outDir)
{
    ExtInode inode;
    if (!ext_read_inode(ctx, dir_ino, &inode)) return -1;

    uint64_t dir_size = ext_file_size(&inode);
    if (dir_size == 0) return 0;
    if (dir_size > 64 * 1024 * 1024) return -1; /* safety cap */

    size_t dataLen = 0;
    uint8_t *dirData = ext_read_file_data(ctx, &inode, dir_size, &dataLen);
    if (!dirData) return -1;

    size_t pos = 0;
    while (pos + 8 <= dataLen) {
        ExtDirEntry *de = (ExtDirEntry *)(dirData + pos);
        uint32_t d_ino   = le32toh(de->inode);
        uint16_t rec_len = le16toh(de->rec_len);
        uint8_t  name_len = de->name_len;

        if (rec_len < 8 || rec_len > dataLen - pos) break;
        if (d_ino == 0 || name_len == 0) { pos += rec_len; continue; }

        /* extract name */
        char nameA[256];
        memcpy(nameA, (char *)(de) + 8, name_len);
        nameA[name_len] = 0;

        /* skip . and .. */
        if (strcmp(nameA, ".") == 0 || strcmp(nameA, "..") == 0) {
            pos += rec_len;
            continue;
        }

        /* convert to wide */
        wchar_t nameW[256];
        MultiByteToWideChar(CP_UTF8, 0, nameA, -1, nameW, 256);

        /* sanitize */
        for (wchar_t *p = nameW; *p; p++) {
            if (*p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
                *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|' || *p == L'\\')
                *p = L'_';
        }

        ext_extract_inode(ctx, d_ino, nameW, outDir);
        pos += rec_len;
    }

    free(dirData);
    return 0;
}

int fs_extract_ext(FILE *fp, uint64_t offset, uint64_t size,
                   const wchar_t *outDir, ExtractCallbacks *cb)
{
    ExtContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = fp;
    ctx.part_offset = offset;
    ctx.cb = cb;

    /* read superblock at offset + 1024 */
    ExtSuperblock sb;
    _fseeki64(fp, (int64_t)(offset + 1024), SEEK_SET);
    if (fread(&sb, 1, sizeof(sb), fp) != sizeof(sb)) {
        extract_log(cb, L"[ext] Failed to read superblock");
        return -1;
    }

    if (le16toh(sb.s_magic) != 0xEF53) {
        extract_log(cb, L"[ext] Invalid superblock magic");
        return -1;
    }

    ctx.block_size = 1024u << le32toh(sb.s_log_block_size);
    ctx.inode_size = le16toh(sb.s_inode_size);
    if (ctx.inode_size == 0) ctx.inode_size = 128;
    ctx.inodes_per_group = le32toh(sb.s_inodes_per_group);
    ctx.blocks_per_group = le32toh(sb.s_blocks_per_group);

    uint32_t total_blocks = le32toh(sb.s_blocks_count);
    ctx.num_groups = (total_blocks + ctx.blocks_per_group - 1) / ctx.blocks_per_group;
    ctx.total_data_size = (uint64_t)total_blocks * ctx.block_size;
    ctx.has_extents = (le32toh(sb.s_feature_incompat) & 0x0040) ? 1 : 0;

    const char *type = ctx.has_extents ? "ext4" :
        ((le32toh(sb.s_feature_compat) & 0x0004) ? "ext3" : "ext2");

    extract_log(cb, L"[ext] Type: %S, Block size: %u, Groups: %u, Inodes/group: %u",
                type, ctx.block_size, ctx.num_groups, ctx.inodes_per_group);

    /* read group descriptor table (block after superblock) */
    uint64_t gdt_block = (ctx.block_size == 1024) ? 2 : 1;
    size_t gdt_size = ctx.num_groups * sizeof(ExtGroupDesc);
    ctx.gdt = (ExtGroupDesc *)malloc(gdt_size);
    if (!ctx.gdt) {
        extract_log(cb, L"[ext] Out of memory for GDT");
        return -1;
    }
    _fseeki64(fp, (int64_t)(offset + gdt_block * ctx.block_size), SEEK_SET);
    if (fread(ctx.gdt, 1, gdt_size, fp) != gdt_size) {
        extract_log(cb, L"[ext] Failed to read GDT");
        free(ctx.gdt);
        return -1;
    }

    ensure_directory(outDir);

    /* root directory is always inode 2 */
    ext_extract_dir(&ctx, 2, outDir);

    free(ctx.gdt);
    extract_log(cb, L"[ext] Extraction complete");
    return 0;
}
