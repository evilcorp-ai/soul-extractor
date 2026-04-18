#ifndef FS_EXT_H
#define FS_EXT_H
#include <stdint.h>
#include <stdio.h>
#include "imgextract.h"

int fs_extract_ext(FILE *fp, uint64_t offset, uint64_t size,
                   const wchar_t *outDir, ExtractCallbacks *cb);
#endif
