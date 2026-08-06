/* sha1.c - SHA-1 per FIPS 180-1; see sha1.h. */
#include "core/sha1.h"

#include <string.h>

static uint32_t rol32(uint32_t v, int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80], a, b, d, e, f, k, t, cc;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = c->h[0];
    b = c->h[1];
    cc = c->h[2];
    d = c->h[3];
    e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6u;
        }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = rol32(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301u;
    c->h[1] = 0xEFCDAB89u;
    c->h[2] = 0x98BADCFEu;
    c->h[3] = 0x10325476u;
    c->h[4] = 0xC3D2E1F0u;
    c->total = 0;
    c->buflen = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->total += len;
    if (c->buflen > 0) {
        size_t need = 64 - c->buflen;
        if (len < need) {
            memcpy(c->buf + c->buflen, p, len);
            c->buflen += len;
            return;
        }
        memcpy(c->buf + c->buflen, p, need);
        sha1_block(c, c->buf);
        c->buflen = 0;
        p += need;
        len -= need;
    }
    while (len >= 64) {
        sha1_block(c, p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(c->buf, p, len);
        c->buflen = len;
    }
}

void sha1_final_hex(sha1_ctx *c, char out_hex[41])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t bits = c->total * 8;
    uint8_t pad[8];
    int i;
    /* 0x80 then zeros to 56 mod 64, then 8-byte big-endian bit count */
    pad[0] = 0x80;
    sha1_update(c, pad, 1);
    memset(pad, 0, 8);
    while (c->buflen != 56)
        sha1_update(c, pad, 1);
    for (i = 0; i < 8; i++)
        pad[i] = (uint8_t)(bits >> (56 - 8 * i));
    /* length append must not count towards c->total... but the digest is
     * finalized right after; the extra 8 bytes do not matter */
    {
        const uint8_t *p = pad;
        size_t len = 8;
        if (c->buflen > 0) {
            size_t need = 64 - c->buflen;
            memcpy(c->buf + c->buflen, p, need);
            sha1_block(c, c->buf);
            c->buflen = 0;
            p += need;
            len -= need;
        }
        while (len >= 64) {
            sha1_block(c, p);
            p += 64;
            len -= 64;
        }
    }
    for (i = 0; i < 5; i++) {
        int j;
        for (j = 0; j < 4; j++) {
            uint8_t b = (uint8_t)(c->h[i] >> (24 - 8 * j));
            out_hex[i * 8 + j * 2] = hex[b >> 4];
            out_hex[i * 8 + j * 2 + 1] = hex[b & 0xF];
        }
    }
    out_hex[40] = '\0';
}

void sha1_hex(const void *data, size_t len, char out_hex[41])
{
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final_hex(&c, out_hex);
}
