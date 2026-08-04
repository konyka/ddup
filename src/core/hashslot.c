/* hashslot.c - CRC16-XMODEM and Redis hash slot computation. */
#include "core/hashslot.h"

#include <string.h>

uint16_t crc16(const char *buf, size_t len)
{
    uint16_t crc = 0;
    size_t i;
    int b;
    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint8_t)buf[i]) << 8;
        for (b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
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
