#if !defined(UNIMGC_H_ENDIAN)
#define UNIMGC_H_ENDIAN

#include <stdint.h>

/* ── little-endian to host ─────────────────────────────────────────── */

static inline uint16_t le16toh(uint16_t n)
{
    unsigned char *p = (unsigned char *)&n;
    return ((uint16_t)p[1] << 8) | (uint16_t)p[0];
}

static inline uint32_t le32toh(uint32_t n)
{
    unsigned char *p = (unsigned char *)&n;
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16)
        |  ((uint32_t)p[1] << 8)  | (uint32_t)p[0];
}

static inline uint64_t le64toh(uint64_t n)
{
    unsigned char *p = (unsigned char *)&n;
    return ((uint64_t)p[7] << 56) | ((uint64_t)p[6] << 48)
        |  ((uint64_t)p[5] << 40) | ((uint64_t)p[4] << 32)
        |  ((uint64_t)p[3] << 24) | ((uint64_t)p[2] << 16)
        |  ((uint64_t)p[1] << 8)  | (uint64_t)p[0];
}

/* ── big-endian to host (for HFS+) ────────────────────────────────── */

static inline uint16_t be16toh(uint16_t n)
{
    unsigned char *p = (unsigned char *)&n;
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static inline uint32_t be32toh(uint32_t n)
{
    unsigned char *p = (unsigned char *)&n;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        |  ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline uint64_t be64toh(uint64_t n)
{
    unsigned char *p = (unsigned char *)&n;
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48)
        |  ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32)
        |  ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16)
        |  ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}

#endif /* !defined(UNIMGC_H_ENDIAN) */
