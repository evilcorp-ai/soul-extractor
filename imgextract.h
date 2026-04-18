#ifndef IMGEXTRACT_H
#define IMGEXTRACT_H

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

/* ── callback interface ─────────────────────────────────────────────── */
typedef struct {
    void (*on_progress)(double pct, const wchar_t *currentFile, void *ctx);
    void (*on_log)(const wchar_t *msg, void *ctx);
    void *ctx;
} ExtractCallbacks;

/*
 * Extract all file systems from a raw disk image (.img) to outputDir.
 * Returns 0 on success, negative on error.
 *
 * Creates subdirectories per partition if multiple are found.
 */
int img_extract(const wchar_t *imgPath, const wchar_t *outputDir,
                ExtractCallbacks *cb);

/* ── per-FS extraction (called internally) ──────────────────────────── */
int fs_extract_fat(FILE *fp, uint64_t offset, uint64_t size,
                   const wchar_t *outDir, ExtractCallbacks *cb);

int fs_extract_exfat(FILE *fp, uint64_t offset, uint64_t size,
                     const wchar_t *outDir, ExtractCallbacks *cb);

int fs_extract_ntfs(FILE *fp, uint64_t offset, uint64_t size,
                    const wchar_t *outDir, ExtractCallbacks *cb);

int fs_extract_ext(FILE *fp, uint64_t offset, uint64_t size,
                   const wchar_t *outDir, ExtractCallbacks *cb);

int fs_extract_hfsplus(FILE *fp, uint64_t offset, uint64_t size,
                       const wchar_t *outDir, ExtractCallbacks *cb);

/* ── helper: log wide-string via callback ───────────────────────────── */
void extract_log(ExtractCallbacks *cb, const wchar_t *fmt, ...);
void extract_progress(ExtractCallbacks *cb, double pct, const wchar_t *file);

/* ── helper: create directory tree recursively ──────────────────────── */
int ensure_directory(const wchar_t *path);

/* ── helper: write extracted file ──────────────────────────────────── */
int write_extracted_file(const wchar_t *path, const uint8_t *data, size_t len);

#endif /* IMGEXTRACT_H */
