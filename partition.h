#ifndef PARTITION_H
#define PARTITION_H

#include <stdint.h>
#include <stdio.h>

#define MAX_PARTITIONS 32

typedef enum {
    PTYPE_UNKNOWN = 0,
    PTYPE_FAT12,
    PTYPE_FAT16,
    PTYPE_FAT32,
    PTYPE_NTFS,
    PTYPE_LINUX,    /* ext2/3/4 or other */
    PTYPE_HFSPLUS,
    PTYPE_EXFAT,
    PTYPE_EFI_SYSTEM,
    PTYPE_RAW        /* bare FS, no partition table */
} PartitionTypeHint;

typedef struct {
    uint64_t offset;        /* byte offset in image */
    uint64_t size;          /* size in bytes */
    PartitionTypeHint hint; /* type hint from partition table */
    int      index;         /* partition number (1-based) */
} PartitionEntry;

typedef struct {
    int count;
    int is_gpt;             /* 1 = GPT, 0 = MBR, -1 = bare */
    PartitionEntry entries[MAX_PARTITIONS];
} PartitionTable;

/*
 * Detect partitions in a disk image.
 * If no partition table is found, returns a single "bare" entry covering
 * the entire image so FS detection can be tried on it directly.
 */
int partition_detect(FILE *fp, uint64_t image_size, PartitionTable *pt);

const char *partition_type_name(PartitionTypeHint hint);

#endif /* PARTITION_H */
