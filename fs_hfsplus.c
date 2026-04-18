/*
 * fs_hfsplus.c — Basic HFS+ read-only extraction
 * Supports: data fork extraction via catalog B-tree
 * Skips:    resource forks, hard links, compression, journaling
 *
 * NOTE: HFS+ is big-endian on disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#include "endian.h"
#include "imgextract.h"
#include "fs_hfsplus.h"

/* ── HFS+ on-disk structures (all big-endian) ─────────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint32_t startBlock;
    uint32_t blockCount;
} HFSPlusExtentDescriptor;

typedef struct {
    uint64_t logicalSize;
    uint32_t clumpSize;
    uint32_t totalBlocks;
    HFSPlusExtentDescriptor extents[8];
} HFSPlusForkData;

typedef struct {
    uint16_t signature;       /* 'H+' (0x482B) or 'HX' (0x4858) */
    uint16_t version;
    uint32_t attributes;
    uint32_t lastMountedVersion;
    uint32_t journalInfoBlock;
    uint32_t createDate;
    uint32_t modifyDate;
    uint32_t backupDate;
    uint32_t checkedDate;
    uint32_t fileCount;
    uint32_t folderCount;
    uint32_t blockSize;
    uint32_t totalBlocks;
    uint32_t freeBlocks;
    uint32_t nextAllocation;
    uint32_t rsrcClumpSize;
    uint32_t dataClumpSize;
    uint32_t nextCatalogID;   /* CNID */
    uint32_t writeCount;
    uint64_t encodingsBitmap;
    uint32_t finderInfo[8];
    HFSPlusForkData allocationFile;
    HFSPlusForkData extentsFile;
    HFSPlusForkData catalogFile;
    HFSPlusForkData attributesFile;
    HFSPlusForkData startupFile;
} HFSPlusVolumeHeader;

/* B-tree node descriptor */
typedef struct {
    uint32_t fLink;
    uint32_t bLink;
    int8_t   kind;
    uint8_t  height;
    uint16_t numRecords;
    uint16_t reserved;
} BTNodeDescriptor;

/* B-tree header record */
typedef struct {
    uint16_t treeDepth;
    uint32_t rootNode;
    uint32_t leafRecords;
    uint32_t firstLeafNode;
    uint32_t lastLeafNode;
    uint16_t nodeSize;
    uint16_t maxKeyLength;
    uint32_t totalNodes;
    uint32_t freeNodes;
    uint16_t reserved1;
    uint32_t clumpSize;
    uint8_t  btreeType;
    uint8_t  keyCompareType;
    uint32_t attributes;
    uint32_t reserved3[16];
} BTHeaderRec;

/* catalog key */
typedef struct {
    uint16_t keyLength;
    uint32_t parentID;
    uint16_t nameLength;
    /* uint16_t name[] follows */
} HFSPlusCatalogKey;

/* catalog record types */
#define kHFSPlusFolderRecord   1
#define kHFSPlusFileRecord     2

typedef struct {
    int16_t  recordType;
    uint16_t flags;
    uint32_t valence;
    uint32_t folderID;   /* CNID */
    uint32_t createDate;
    uint32_t contentModDate;
    uint32_t attributeModDate;
    uint32_t accessDate;
    uint32_t backupDate;
    uint8_t  permissions[16];
    uint8_t  userInfo[16];
    uint8_t  finderInfo[16];
    uint32_t textEncoding;
    uint32_t reserved;
} HFSPlusCatalogFolder;

typedef struct {
    int16_t  recordType;
    uint16_t flags;
    uint32_t reserved1;
    uint32_t fileID;     /* CNID */
    uint32_t createDate;
    uint32_t contentModDate;
    uint32_t attributeModDate;
    uint32_t accessDate;
    uint32_t backupDate;
    uint8_t  permissions[16];
    uint8_t  userInfo[16];
    uint8_t  finderInfo[16];
    uint32_t textEncoding;
    uint32_t reserved2;
    HFSPlusForkData dataFork;
    HFSPlusForkData resourceFork;
} HFSPlusCatalogFile;
#pragma pack(pop)

typedef struct {
    FILE     *fp;
    uint64_t  part_offset;
    uint32_t  block_size;
    uint32_t  total_blocks;
    uint8_t  *catalog_data;
    size_t    catalog_size;
    uint16_t  node_size;
    uint32_t  root_node;
    uint32_t  first_leaf;
    uint64_t  total_data_size;
    uint64_t  extracted_size;
    ExtractCallbacks *cb;
} HFSContext;

/* ── read blocks from fork data ────────────────────────────────────── */
static uint8_t *hfs_read_fork(HFSContext *ctx, const HFSPlusForkData *fork,
                               size_t *outLen)
{
    uint64_t logical = be64toh(fork->logicalSize);
    if (logical == 0 || logical > 0x40000000ULL) { /* cap at 1GB */
        *outLen = 0;
        return NULL;
    }

    uint8_t *data = (uint8_t *)calloc(1, (size_t)logical);
    if (!data) { *outLen = 0; return NULL; }

    size_t pos = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t start = be32toh(fork->extents[i].startBlock);
        uint32_t count = be32toh(fork->extents[i].blockCount);
        if (count == 0) break;

        for (uint32_t b = 0; b < count && pos < logical; b++) {
            uint64_t off = ctx->part_offset +
                (uint64_t)(start + b) * ctx->block_size;
            size_t to_read = ctx->block_size;
            if (pos + to_read > logical) to_read = (size_t)(logical - pos);
            _fseeki64(ctx->fp, (int64_t)off, SEEK_SET);
            fread(data + pos, 1, to_read, ctx->fp);
            pos += to_read;
        }
    }

    *outLen = (size_t)logical;
    return data;
}

/* ── entry for building path map ───────────────────────────────────── */
#define MAX_CATALOG_ENTRIES 65536

typedef struct {
    uint32_t cnid;
    uint32_t parentCNID;
    wchar_t  name[256];
    int      isFolder;
    HFSPlusForkData dataFork;
} HFSCatalogItem;

static HFSCatalogItem *g_items = NULL;
static int g_item_count = 0;

static void hfs_add_item(uint32_t cnid, uint32_t parent, const wchar_t *name,
                          int isFolder, const HFSPlusForkData *fork)
{
    if (g_item_count >= MAX_CATALOG_ENTRIES) return;
    HFSCatalogItem *item = &g_items[g_item_count++];
    item->cnid = cnid;
    item->parentCNID = parent;
    wcsncpy(item->name, name, 255);
    item->name[255] = 0;
    item->isFolder = isFolder;
    if (fork)
        memcpy(&item->dataFork, fork, sizeof(HFSPlusForkData));
    else
        memset(&item->dataFork, 0, sizeof(HFSPlusForkData));
}

/* ── scan catalog B-tree leaf nodes ────────────────────────────────── */
static void hfs_scan_catalog(HFSContext *ctx)
{
    uint32_t node = ctx->first_leaf;

    while (node != 0 && node * ctx->node_size < ctx->catalog_size) {
        uint8_t *ndata = ctx->catalog_data + (size_t)node * ctx->node_size;
        BTNodeDescriptor *nd = (BTNodeDescriptor *)ndata;

        int8_t kind = nd->kind;
        uint16_t numRec = be16toh(nd->numRecords);

        if (kind != -1) { /* -1 = leaf */
            node = be32toh(nd->fLink);
            continue;
        }

        for (uint16_t r = 0; r < numRec && g_item_count < MAX_CATALOG_ENTRIES; r++) {
            /* record offset is stored at end of node */
            uint16_t *offsets = (uint16_t *)(ndata + ctx->node_size - 2 * (r + 1));
            uint16_t roff = be16toh(*offsets);
            if (roff >= ctx->node_size - 2) continue;

            uint8_t *rec = ndata + roff;
            /* key */
            uint16_t keyLen = be16toh(*(uint16_t *)rec);
            if (keyLen < 6 || roff + 2 + keyLen >= ctx->node_size) continue;

            HFSPlusCatalogKey *key = (HFSPlusCatalogKey *)rec;
            uint32_t parentID = be32toh(key->parentID);
            uint16_t nameLen  = be16toh(key->nameLength);

            if (nameLen > 255) nameLen = 255;
            wchar_t name[256];
            uint16_t *uname = (uint16_t *)(rec + 2 + 4 + 2); /* after keyLen+parentID+nameLen */
            for (int i = 0; i < nameLen; i++)
                name[i] = (wchar_t)be16toh(uname[i]);
            name[nameLen] = 0;

            /* record data starts after key (aligned to 2 bytes) */
            uint16_t dataOff = 2 + keyLen;
            if (dataOff & 1) dataOff++;
            if (roff + dataOff + 2 >= ctx->node_size) continue;

            int16_t recType = (int16_t)be16toh(*(uint16_t *)(ndata + roff + dataOff));

            if (recType == kHFSPlusFolderRecord) {
                HFSPlusCatalogFolder *folder =
                    (HFSPlusCatalogFolder *)(ndata + roff + dataOff);
                uint32_t cnid = be32toh(folder->folderID);
                hfs_add_item(cnid, parentID, name, 1, NULL);
            } else if (recType == kHFSPlusFileRecord) {
                HFSPlusCatalogFile *file =
                    (HFSPlusCatalogFile *)(ndata + roff + dataOff);
                uint32_t cnid = be32toh(file->fileID);
                hfs_add_item(cnid, parentID, name, 0, &file->dataFork);
            }
        }

        node = be32toh(nd->fLink);
    }
}

/* ── build path for a CNID ─────────────────────────────────────────── */
static int hfs_build_path(uint32_t cnid, wchar_t *path, int maxLen)
{
    /* root folder CNID = 2 */
    if (cnid == 2 || cnid == 1) { path[0] = 0; return 1; }

    /* find item */
    for (int i = 0; i < g_item_count; i++) {
        if (g_items[i].cnid == cnid) {
            wchar_t parent_path[MAX_PATH * 2] = {0};
            hfs_build_path(g_items[i].parentCNID, parent_path, MAX_PATH * 2);

            if (parent_path[0])
                _snwprintf(path, maxLen, L"%s\\%s", parent_path, g_items[i].name);
            else
                wcsncpy(path, g_items[i].name, maxLen);
            return 1;
        }
    }
    return 0;
}

/* ── extract all files ─────────────────────────────────────────────── */
static int hfs_extract_all(HFSContext *ctx, const wchar_t *outDir)
{
    for (int i = 0; i < g_item_count; i++) {
        HFSCatalogItem *item = &g_items[i];

        /* skip items in root metadata folders */
        if (item->parentCNID < 2) continue;

        /* sanitize name */
        for (wchar_t *p = item->name; *p; p++) {
            if (*p == L'/' || *p == L':' || *p == L'*' || *p == L'?' ||
                *p == L'"' || *p == L'<' || *p == L'>' || *p == L'|' || *p == L'\\')
                *p = L'_';
        }

        /* build relative path */
        wchar_t relPath[MAX_PATH * 2] = {0};
        hfs_build_path(item->cnid, relPath, MAX_PATH * 2);
        if (!relPath[0]) continue;

        wchar_t fullPath[MAX_PATH * 2];
        _snwprintf(fullPath, MAX_PATH * 2, L"%s\\%s", outDir, relPath);

        if (item->isFolder) {
            ensure_directory(fullPath);
        } else {
            /* ensure parent directory exists */
            wchar_t parentDir[MAX_PATH * 2];
            wcsncpy(parentDir, fullPath, MAX_PATH * 2);
            wchar_t *lastSlash = wcsrchr(parentDir, L'\\');
            if (lastSlash) { *lastSlash = 0; ensure_directory(parentDir); }

            uint64_t fsize = be64toh(item->dataFork.logicalSize);
            if (fsize == 0) {
                write_extracted_file(fullPath, NULL, 0);
                continue;
            }

            double pct = ctx->total_data_size > 0 ? (double)ctx->extracted_size / (double)ctx->total_data_size : 0;
            extract_progress(ctx->cb, pct, item->name);

            size_t dataLen = 0;
            uint8_t *data = hfs_read_fork(ctx, &item->dataFork, &dataLen);
            if (data) {
                /* truncate to actual file size */
                if (dataLen > fsize) dataLen = (size_t)fsize;
                write_extracted_file(fullPath, data, dataLen);
                ctx->extracted_size += dataLen;
                free(data);
            }

            if (ctx->total_data_size > 0) {
                double pct = (double)ctx->extracted_size / (double)ctx->total_data_size;
                extract_progress(ctx->cb, pct, item->name);
            }
        }
    }
    return 0;
}

int fs_extract_hfsplus(FILE *fp, uint64_t offset, uint64_t size,
                       const wchar_t *outDir, ExtractCallbacks *cb)
{
    HFSContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fp = fp;
    ctx.part_offset = offset;
    ctx.cb = cb;

    /* read volume header at offset + 1024 */
    HFSPlusVolumeHeader vh;
    _fseeki64(fp, (int64_t)(offset + 1024), SEEK_SET);
    if (fread(&vh, 1, sizeof(vh), fp) != sizeof(vh)) {
        extract_log(cb, L"[HFS+] Failed to read volume header");
        return -1;
    }

    uint16_t sig = be16toh(vh.signature);
    if (sig != 0x482B && sig != 0x4858) {
        extract_log(cb, L"[HFS+] Invalid signature: 0x%04X", sig);
        return -1;
    }

    ctx.block_size = be32toh(vh.blockSize);
    ctx.total_blocks = be32toh(vh.totalBlocks);
    ctx.total_data_size = (uint64_t)ctx.total_blocks * ctx.block_size;

    extract_log(cb, L"[HFS+] Block size: %u, Total blocks: %u, Files: %u, Folders: %u",
                ctx.block_size, ctx.total_blocks,
                be32toh(vh.fileCount), be32toh(vh.folderCount));

    /* read catalog file */
    ctx.catalog_data = hfs_read_fork(&ctx, &vh.catalogFile, &ctx.catalog_size);
    if (!ctx.catalog_data || ctx.catalog_size < 512) {
        extract_log(cb, L"[HFS+] Failed to read catalog file");
        if (ctx.catalog_data) free(ctx.catalog_data);
        return -1;
    }

    /* parse B-tree header (node 0) */
    BTNodeDescriptor *nd0 = (BTNodeDescriptor *)ctx.catalog_data;
    /* header record is the first record in node 0 */
    uint16_t *offsets0 = (uint16_t *)(ctx.catalog_data + 512 - 2);
    uint16_t hdr_off = be16toh(*offsets0);

    /* actually, the header record starts right after the node descriptor (14 bytes) */
    BTHeaderRec *btHdr = (BTHeaderRec *)(ctx.catalog_data + 14);
    ctx.node_size  = be16toh(btHdr->nodeSize);
    ctx.root_node  = be32toh(btHdr->rootNode);
    ctx.first_leaf = be32toh(btHdr->firstLeafNode);

    if (ctx.node_size == 0) ctx.node_size = 4096;

    extract_log(cb, L"[HFS+] B-tree node size: %u, Root: %u, First leaf: %u",
                ctx.node_size, ctx.root_node, ctx.first_leaf);

    /* allocate catalog items */
    g_items = (HFSCatalogItem *)calloc(MAX_CATALOG_ENTRIES, sizeof(HFSCatalogItem));
    g_item_count = 0;
    if (!g_items) {
        free(ctx.catalog_data);
        return -1;
    }

    /* scan catalog */
    hfs_scan_catalog(&ctx);
    extract_log(cb, L"[HFS+] Found %d catalog entries", g_item_count);

    /* extract */
    ensure_directory(outDir);
    hfs_extract_all(&ctx, outDir);

    free(g_items);
    g_items = NULL;
    g_item_count = 0;
    free(ctx.catalog_data);

    extract_log(cb, L"[HFS+] Extraction complete");
    return 0;
}
