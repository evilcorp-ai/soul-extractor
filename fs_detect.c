#include <string.h>
#include <stdio.h>
#include "endian.h"
#include "fs_detect.h"

FSType fs_detect(FILE *fp, uint64_t partition_offset)
{
    uint8_t buf[4096];
    memset(buf, 0, sizeof(buf));

    _fseeki64(fp, (int64_t)partition_offset, SEEK_SET);
    size_t n = fread(buf, 1, sizeof(buf), fp);
    if (n < 512)
        return FS_UNKNOWN;

    /* ── exFAT: "EXFAT   " at offset 3 ──────────────────────────── */
    if (memcmp(buf + 3, "EXFAT   ", 8) == 0)
        return FS_EXFAT;

    /* ── NTFS: "NTFS    " at offset 3 ───────────────────────────── */
    if (memcmp(buf + 3, "NTFS    ", 8) == 0)
        return FS_NTFS;

    /* ── FAT: check for jump byte + FAT string ──────────────────── */
    if (buf[0] == 0xEB || buf[0] == 0xE9 || buf[0] == 0xE8) {
        /* FAT32: "FAT32   " at offset 82 */
        if (memcmp(buf + 82, "FAT32   ", 8) == 0)
            return FS_FAT32;
        /* FAT16: "FAT16   " at offset 54 */
        if (memcmp(buf + 54, "FAT16   ", 8) == 0)
            return FS_FAT16;
        /* FAT12: "FAT12   " at offset 54 */
        if (memcmp(buf + 54, "FAT12   ", 8) == 0)
            return FS_FAT12;
        /* generic FAT: "FAT     " at offset 54 */
        if (memcmp(buf + 54, "FAT     ", 8) == 0)
            return FS_FAT16; /* assume FAT16 */
    }

    /* ── ext2/3/4: magic 0xEF53 at offset 1080 (superblock at 1024 + 0x38) */
    if (n >= 1082 || partition_offset == 0) {
        /* re-read if needed for superblock at offset 1024 */
        uint8_t sb[256];
        _fseeki64(fp, (int64_t)(partition_offset + 1024), SEEK_SET);
        if (fread(sb, 1, 256, fp) >= 90) {
            uint16_t magic = le16toh(*(uint16_t *)(sb + 0x38));
            if (magic == 0xEF53) {
                /* check for ext4 features */
                uint32_t incompat = le32toh(*(uint32_t *)(sb + 0x60));
                uint32_t ro_compat = le32toh(*(uint32_t *)(sb + 0x64));
                uint32_t compat = le32toh(*(uint32_t *)(sb + 0x5C));
                /* ext4 has EXTENTS (0x40) in incompat flags */
                if (incompat & 0x0040)
                    return FS_EXT4;
                /* ext3 has journal (0x0004) in compat flags */
                if (compat & 0x0004)
                    return FS_EXT3;
                return FS_EXT2;
            }
        }
    }

    /* ── HFS+: magic "H+" at offset 1024 ────────────────────────── */
    {
        uint8_t hbuf[8];
        _fseeki64(fp, (int64_t)(partition_offset + 1024), SEEK_SET);
        if (fread(hbuf, 1, 4, fp) == 4) {
            uint16_t sig = be16toh(*(uint16_t *)hbuf);
            if (sig == 0x482B)  /* 'H+' */
                return FS_HFSPLUS;
            if (sig == 0x4858)  /* 'HX' = HFSX (case-sensitive HFS+) */
                return FS_HFSPLUS;
        }
    }

    return FS_UNKNOWN;
}

const char *fs_type_name(FSType t)
{
    switch (t) {
        case FS_FAT12:   return "FAT12";
        case FS_FAT16:   return "FAT16";
        case FS_FAT32:   return "FAT32";
        case FS_EXFAT:   return "exFAT";
        case FS_NTFS:    return "NTFS";
        case FS_EXT2:    return "ext2";
        case FS_EXT3:    return "ext3";
        case FS_EXT4:    return "ext4";
        case FS_HFSPLUS: return "HFS+";
        default:         return "Unknown";
    }
}
