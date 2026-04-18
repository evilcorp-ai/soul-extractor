#ifndef FS_DETECT_H
#define FS_DETECT_H

#include <stdint.h>
#include <stdio.h>

typedef enum {
    FS_UNKNOWN = 0,
    FS_FAT12,
    FS_FAT16,
    FS_FAT32,
    FS_EXFAT,
    FS_NTFS,
    FS_EXT2,
    FS_EXT3,
    FS_EXT4,
    FS_HFSPLUS
} FSType;

/*
 * Detect the file system at a given byte offset in the image.
 * Reads magic bytes / superblocks to make the determination.
 */
FSType fs_detect(FILE *fp, uint64_t partition_offset);

const char *fs_type_name(FSType t);

#endif /* FS_DETECT_H */
