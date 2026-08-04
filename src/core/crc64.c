/* crc64.c - table-driven CRC-64/XZ; see crc64.h. */
#include "core/crc64.h"

#define CRC64_POLY 0xC96C5795D7870F42ULL /* reflected ECMA-182 */

static uint64_t crc64_tab[256];
static int crc64_tab_ready = 0;

/* Idempotent init; every concurrent writer computes identical bytes. */
static void crc64_init(void)
{
    int i, j;
    for (i = 0; i < 256; i++) {
        uint64_t c = (uint64_t)i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (c >> 1) ^ CRC64_POLY : (c >> 1);
        crc64_tab[i] = c;
    }
    crc64_tab_ready = 1;
}

uint64_t crc64(uint64_t crc, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    if (!crc64_tab_ready)
        crc64_init();
    crc = ~crc;
    while (len--)
        crc = crc64_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
