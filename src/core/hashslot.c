/* hashslot.c - CRC16-XMODEM and Redis hash slot computation. */
#include "core/hashslot.h"

#include <string.h>

/* Table-driven CRC16-XMODEM (Phase 37): the bit-by-bit loop cost
 * 8 branchy iterations per key byte and topped the mt routing profile
 * (every routed command pays one hash_slot). One table lookup per byte
 * now. The table is initialized lazily; concurrent initializations
 * write identical contents (same pattern as cmd_hash_init). */
static uint16_t crc16_tab[256];
static int crc16_tab_inited = 0;

static void crc16_tab_init(void)
{
    unsigned i;
    int b;
    if (crc16_tab_inited)
        return;
    for (i = 0; i < 256; i++) {
        uint16_t crc = (uint16_t)(i << 8);
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            else
                crc = (uint16_t)(crc << 1);
        }
        crc16_tab[i] = crc;
    }
    crc16_tab_inited = 1;
}

uint16_t crc16(const char *buf, size_t len)
{
    uint16_t crc = 0;
    size_t i;
    if (!crc16_tab_inited)
        crc16_tab_init();
    for (i = 0; i < len; i++)
        crc = (uint16_t)((crc << 8) ^
                         crc16_tab[((crc >> 8) ^ (uint8_t)buf[i]) & 0xFF]);
    return crc;
}

size_t hash_tag(const char *key, size_t klen, char *out, size_t cap)
{
    size_t i, start = 0, end = 0, n;
    for (i = 0; i < klen; i++) {
        if (key[i] == '{') {
            size_t j;
            start = i + 1;
            for (j = start; j < klen; j++) {
                if (key[j] == '}') {
                    if (j > start)
                        end = j; /* non-empty {} pair */
                    break;
                }
            }
            break;
        }
    }
    if (end == 0) { /* no valid hashtag: whole key */
        start = 0;
        end = klen;
    }
    n = end - start;
    if (cap > 0) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(out, key + start, c);
        out[c] = '\0';
    }
    return n;
}

uint32_t hash_slot(const char *key, size_t klen)
{
    size_t i, start = 0, end = klen;
    for (i = 0; i < klen; i++) {
        if (key[i] == '{') {
            size_t j;
            for (j = i + 1; j < klen; j++) {
                if (key[j] == '}') {
                    if (j > i + 1) {
                        start = i + 1;
                        end = j;
                    }
                    break;
                }
            }
            break;
        }
    }
    return (uint32_t)(crc16(key + start, end - start) % 16384);
}
