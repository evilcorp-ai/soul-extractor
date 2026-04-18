/*
 * imgextract.c — Unified disk image extraction engine
 *
 * Orchestrates: partition detection → FS detection → per-FS extraction
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>
#include "imgextract.h"
#include "partition.h"
#include "fs_detect.h"
#include "fs_fat.h"
#include "fs_ntfs.h"
#include "fs_ext.h"
#include "fs_hfsplus.h"

/* ── helper: log via callback ──────────────────────────────────────── */
void extract_log(ExtractCallbacks *cb, const wchar_t *fmt, ...)
{
    if (!cb || !cb->on_log) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1024, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    cb->on_log(buf, cb->ctx);
}

void extract_progress(ExtractCallbacks *cb, double pct, const wchar_t *file)
{
    if (!cb || !cb->on_progress) return;
    cb->on_progress(pct, file, cb->ctx);
}

/* ── helper: create directory tree recursively ─────────────────────── */
int ensure_directory(const wchar_t *path)
{
    if (!path || !path[0]) return 0;

    /* check if already exists */
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return 1;

    /* try to create parent first */
    wchar_t parent[MAX_PATH * 2];
    wcsncpy(parent, path, MAX_PATH * 2 - 1);
    parent[MAX_PATH * 2 - 1] = 0;
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash && slash != parent) {
        *slash = 0;
        ensure_directory(parent);
    }

    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* ── helper: write extracted file ──────────────────────────────────── */
int write_extracted_file(const wchar_t *path, const uint8_t *data, size_t len)
{
    /* ensure parent directory */
    wchar_t parent[MAX_PATH * 2];
    wcsncpy(parent, path, MAX_PATH * 2 - 1);
    parent[MAX_PATH * 2 - 1] = 0;
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash) { *slash = 0; ensure_directory(parent); }

    FILE *f = _wfopen(path, L"wb");
    if (!f) return -1;
    if (data && len > 0)
        fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

/* ── main extraction ───────────────────────────────────────────────── */
int img_extract(const wchar_t *imgPath, const wchar_t *outputDir,
                ExtractCallbacks *cb)
{
    /* open image as narrow path */
    FILE *fp = _wfopen(imgPath, L"rb");
    if (!fp) {
        extract_log(cb, L"[IMG] Failed to open image file");
        return -1;
    }

    /* get file size */
    _fseeki64(fp, 0, SEEK_END);
    uint64_t image_size = (uint64_t)_ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    extract_log(cb, L"[IMG] Image size: %llu bytes (%.2f GB)",
                image_size, (double)image_size / (1024.0 * 1024.0 * 1024.0));

    /* detect partitions */
    PartitionTable pt;
    int num = partition_detect(fp, image_size, &pt);

    if (pt.is_gpt == 1)
        extract_log(cb, L"[IMG] Partition table: GPT (%d partitions)", num);
    else if (pt.is_gpt == 0)
        extract_log(cb, L"[IMG] Partition table: MBR (%d partitions)", num);
    else
        extract_log(cb, L"[IMG] No partition table found, treating as bare filesystem");

    int total_extracted = 0;

    for (int i = 0; i < pt.count; i++) {
        PartitionEntry *pe = &pt.entries[i];

        extract_log(cb, L"[IMG] Partition %d: offset=0x%llX size=%llu bytes hint=%S",
                    pe->index, pe->offset, pe->size,
                    partition_type_name(pe->hint));

        /* detect file system */
        FSType fs = fs_detect(fp, pe->offset);
        if (fs == FS_UNKNOWN) {
            extract_log(cb, L"[IMG] Partition %d: unknown filesystem, skipping",
                        pe->index);
            continue;
        }

        extract_log(cb, L"[IMG] Partition %d: detected %S filesystem",
                    pe->index, fs_type_name(fs));

        /* build output directory */
        wchar_t partDir[MAX_PATH * 2];
        if (pt.count == 1 && pt.is_gpt < 0) {
            /* bare FS — extract directly to output dir */
            wcsncpy(partDir, outputDir, MAX_PATH * 2);
        } else {
            _snwprintf(partDir, MAX_PATH * 2, L"%s\\partition_%d_%S",
                       outputDir, pe->index, fs_type_name(fs));
        }
        ensure_directory(partDir);

        /* dispatch to FS extractor */
        int result = -1;
        switch (fs) {
            case FS_FAT12:
            case FS_FAT16:
            case FS_FAT32:
                result = fs_extract_fat(fp, pe->offset, pe->size, partDir, cb);
                break;
            case FS_EXFAT:
                result = fs_extract_exfat(fp, pe->offset, pe->size, partDir, cb);
                break;
            case FS_NTFS:
                result = fs_extract_ntfs(fp, pe->offset, pe->size, partDir, cb);
                break;
            case FS_EXT2:
            case FS_EXT3:
            case FS_EXT4:
                result = fs_extract_ext(fp, pe->offset, pe->size, partDir, cb);
                break;
            case FS_HFSPLUS:
                result = fs_extract_hfsplus(fp, pe->offset, pe->size, partDir, cb);
                break;
            default:
                extract_log(cb, L"[IMG] No extractor for %S", fs_type_name(fs));
                break;
        }

        if (result == 0) {
            extract_log(cb, L"[IMG] Partition %d extraction succeeded", pe->index);
            total_extracted++;
        } else {
            extract_log(cb, L"[IMG] Partition %d extraction failed (code %d)",
                        pe->index, result);
        }
    }

    fclose(fp);

    if (total_extracted > 0) {
        extract_log(cb, L"[IMG] Done! Extracted %d partition(s)", total_extracted);
        return 0;
    } else {
        extract_log(cb, L"[IMG] FAIL - No partitions could be extracted");
        return -1;
    }
}
