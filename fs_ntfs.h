#ifndef FS_NTFS_H
#define FS_NTFS_H
#include <stdint.h>
#include <stdio.h>
#include "imgextract.h"

int fs_extract_ntfs(FILE *fp, uint64_t offset, uint64_t size,
                    const wchar_t *outDir, ExtractCallbacks *cb);
#endif
