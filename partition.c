#include <string.h>
#include <stdio.h>
#include "endian.h"
#include "partition.h"

/* ── MBR structures ────────────────────────────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sectors;
} MBR_Entry;

typedef struct {
    uint8_t   bootstrap[446];
    MBR_Entry parts[4];
    uint16_t  signature;   /* 0xAA55 */
} MBR;
#pragma pack(pop)

/* ── GPT structures ────────────────────────────────────────────────── */
#pragma pack(push, 1)
typedef struct {
    uint8_t  signature[8];   /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t part_entry_lba;
    uint32_t num_part_entries;
    uint32_t part_entry_size;
    uint32_t part_array_crc32;
} GPT_Header;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} GPT_Entry;
#pragma pack(pop)

/* well-known GPT type GUIDs */
static const uint8_t GUID_EFI_SYSTEM[]   = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const uint8_t GUID_MS_BASIC[]     = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};
static const uint8_t GUID_LINUX_FS[]     = {0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4};
static const uint8_t GUID_APPLE_HFS[]    = {0x00,0x53,0x46,0x48,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC};

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}

static PartitionTypeHint mbr_type_to_hint(uint8_t type) {
    switch (type) {
        case 0x01: return PTYPE_FAT12;
        case 0x04: case 0x06: case 0x0E: return PTYPE_FAT16;
        case 0x0B: case 0x0C: return PTYPE_FAT32;
        case 0x07: return PTYPE_NTFS;  /* also exFAT sometimes */
        case 0x83: return PTYPE_LINUX;
        case 0xAF: return PTYPE_HFSPLUS;
        case 0xEE: return PTYPE_UNKNOWN; /* GPT protective */
        case 0xEF: return PTYPE_EFI_SYSTEM;
        default:   return PTYPE_UNKNOWN;
    }
}

static PartitionTypeHint gpt_type_to_hint(const uint8_t *guid) {
    if (guid_eq(guid, GUID_EFI_SYSTEM)) return PTYPE_EFI_SYSTEM;
    if (guid_eq(guid, GUID_MS_BASIC))   return PTYPE_NTFS; /* could be FAT/NTFS/exFAT */
    if (guid_eq(guid, GUID_LINUX_FS))   return PTYPE_LINUX;
    if (guid_eq(guid, GUID_APPLE_HFS))  return PTYPE_HFSPLUS;
    return PTYPE_UNKNOWN;
}

static int try_mbr(FILE *fp, PartitionTable *pt) {
    MBR mbr;
    _fseeki64(fp, 0, SEEK_SET);
    if (fread(&mbr, 1, sizeof(mbr), fp) != sizeof(mbr))
        return 0;

    if (le16toh(mbr.signature) != 0xAA55)
        return 0;

    /* check if it's a GPT protective MBR */
    if (mbr.parts[0].type == 0xEE)
        return 0; /* let GPT handle it */

    int count = 0;
    for (int i = 0; i < 4 && count < MAX_PARTITIONS; i++) {
        uint32_t lba  = le32toh(mbr.parts[i].lba_start);
        uint32_t secs = le32toh(mbr.parts[i].sectors);
        if (secs == 0 || mbr.parts[i].type == 0)
            continue;
        pt->entries[count].offset = (uint64_t)lba * 512;
        pt->entries[count].size   = (uint64_t)secs * 512;
        pt->entries[count].hint   = mbr_type_to_hint(mbr.parts[i].type);
        pt->entries[count].index  = count + 1;
        count++;
    }

    pt->count = count;
    pt->is_gpt = 0;
    return count > 0;
}

static int try_gpt(FILE *fp, PartitionTable *pt) {
    /* read LBA 1 */
    GPT_Header hdr;
    _fseeki64(fp, 512, SEEK_SET);
    if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
        return 0;

    if (memcmp(hdr.signature, "EFI PART", 8) != 0)
        return 0;

    uint32_t num   = le32toh(hdr.num_part_entries);
    uint32_t esize = le32toh(hdr.part_entry_size);
    uint64_t elba  = le64toh(hdr.part_entry_lba);

    if (num > MAX_PARTITIONS) num = MAX_PARTITIONS;
    if (esize < sizeof(GPT_Entry)) esize = sizeof(GPT_Entry);

    uint8_t ebuf[512]; /* big enough for one entry */
    int count = 0;

    for (uint32_t i = 0; i < num && count < MAX_PARTITIONS; i++) {
        _fseeki64(fp, (int64_t)(elba * 512 + (uint64_t)i * esize), SEEK_SET);
        memset(ebuf, 0, sizeof(ebuf));
        if (fread(ebuf, 1, esize < sizeof(ebuf) ? esize : sizeof(ebuf), fp) < 48)
            continue;

        GPT_Entry *e = (GPT_Entry *)ebuf;
        uint64_t first = le64toh(e->first_lba);
        uint64_t last  = le64toh(e->last_lba);

        /* skip empty entries */
        uint8_t zero[16] = {0};
        if (memcmp(e->type_guid, zero, 16) == 0)
            continue;
        if (first == 0 && last == 0)
            continue;

        pt->entries[count].offset = first * 512;
        pt->entries[count].size   = (last - first + 1) * 512;
        pt->entries[count].hint   = gpt_type_to_hint(e->type_guid);
        pt->entries[count].index  = count + 1;
        count++;
    }

    pt->count = count;
    pt->is_gpt = 1;
    return count > 0;
}

int partition_detect(FILE *fp, uint64_t image_size, PartitionTable *pt) {
    memset(pt, 0, sizeof(*pt));

    /* try GPT first (it has a more definitive signature) */
    if (try_gpt(fp, pt))
        return pt->count;

    /* try MBR */
    if (try_mbr(fp, pt))
        return pt->count;

    /* no partition table — treat entire image as bare FS */
    pt->count = 1;
    pt->is_gpt = -1;
    pt->entries[0].offset = 0;
    pt->entries[0].size   = image_size;
    pt->entries[0].hint   = PTYPE_RAW;
    pt->entries[0].index  = 1;
    return 1;
}

const char *partition_type_name(PartitionTypeHint hint) {
    switch (hint) {
        case PTYPE_FAT12:      return "FAT12";
        case PTYPE_FAT16:      return "FAT16";
        case PTYPE_FAT32:      return "FAT32";
        case PTYPE_NTFS:       return "NTFS";
        case PTYPE_LINUX:      return "Linux";
        case PTYPE_HFSPLUS:    return "HFS+";
        case PTYPE_EXFAT:      return "exFAT";
        case PTYPE_EFI_SYSTEM: return "EFI System";
        case PTYPE_RAW:        return "Raw/Bare";
        default:               return "Unknown";
    }
}
