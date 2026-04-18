#ifndef FS_FAT_H
#define FS_FAT_H

#include <stdint.h>
#include <stdio.h>
#include "imgextract.h"

int fs_extract_fat(FILE *fp, uint64_t offset, uint64_t size,
                   const wchar_t *outDir, ExtractCallbacks *cb);

int fs_extract_exfat(FILE *fp, uint64_t offset, uint64_t size,
                     const wchar_t *outDir, ExtractCallbacks *cb);

#endif
