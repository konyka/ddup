/* listpack.c - Redis-compatible listpack compact encoding.
 *
 * See listpack.h for the on-disk layout and the realloc contract.
 * Entry encodings (first byte):
 *
 *   0xxxxxxx                          7-bit uint (0..127)
 *   10xxxxxx + payload                6-bit-length string (<= 63 bytes)
 *   110xxxxx xxxxxxxx                 13-bit int (-4096..4095)
 *   1110xxxx xxxxxxxx + payload       12-bit-length string (<= 4095 bytes)
 *   11110000 + u32le + payload        32-bit-length string
 *   11110001 + s16le                  16-bit int
 *   11110010 + s24le                  24-bit int
 *   11110011 + s32le                  32-bit int
 *   11110100 + s64le                  64-bit int
 *   11111111                          EOF terminator (not an entry)
 *
 * Every entry is trailed by a backlen field (7 bits per byte, first byte
 * MSB=0, continuation bytes MSB=1) covering encoding+payload bytes.
 */
#include "ds/listpack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LP_HDR_SIZE 6u
#define LP_NUMELE_UNKNOWN 0xFFFFu

#define LP_ENC_7BIT_UINT 0x00u
#define LP_ENC_6BIT_STR 0x80u
#define LP_ENC_13BIT_INT 0xC0u
#define LP_ENC_12BIT_STR 0xE0u
#define LP_ENC_32BIT_STR 0xF0u
#define LP_ENC_16BIT_INT 0xF1u
#define LP_ENC_24BIT_INT 0xF2u
#define LP_ENC_32BIT_INT 0xF3u
#define LP_ENC_64BIT_INT 0xF4u

#define LP_ENC_7BIT_UINT_MASK 0x80u
#define LP_ENC_6BIT_STR_MASK 0xC0u
#define LP_ENC_13BIT_INT_MASK 0xE0u
#define LP_ENC_12BIT_STR_MASK 0xF0u

static void lp_die_oom(void)
{
    fprintf(stderr, "ddup: out of memory\n");
    exit(1);
}

/* ------------------------------------------------------------------ */
/* little-endian loads (listpack format is LE on every platform)       */
/* ------------------------------------------------------------------ */

static uint32_t lp_load_total(const unsigned char *lp)
{
    return (uint32_t)lp[0] | ((uint32_t)lp[1] << 8) | ((uint32_t)lp[2] << 16) |
           ((uint32_t)lp[3] << 24);
}

static void lp_store_total(unsigned char *lp, uint32_t total)
{
    lp[0] = (unsigned char)(total & 0xFFu);
    lp[1] = (unsigned char)((total >> 8) & 0xFFu);
    lp[2] = (unsigned char)((total >> 16) & 0xFFu);
    lp[3] = (unsigned char)((total >> 24) & 0xFFu);
}

static uint16_t lp_load_numele(const unsigned char *lp)
{
    return (uint16_t)((uint16_t)lp[4] | ((uint16_t)lp[5] << 8));
}

static void lp_store_numele(unsigned char *lp, uint16_t n)
{
    lp[4] = (unsigned char)(n & 0xFFu);
    lp[5] = (unsigned char)((n >> 8) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* entry sizing / decoding                                             */
/* ------------------------------------------------------------------ */

/* Length of the encoding+payload part of the entry starting at p.
 * lp bounds the valid region; malformed lengths are clamped past the EOF
 * so that callers' bounds checks fail instead of reading out of bounds. */
static uint64_t lp_entry_payload_size(const unsigned char *lp,
                                      const unsigned char *p)
{
    uint32_t total = lp_load_total(lp);
    uint64_t avail;
    unsigned char b = p[0];
    if ((b & LP_ENC_7BIT_UINT_MASK) == LP_ENC_7BIT_UINT)
        return 1;
    if ((b & LP_ENC_6BIT_STR_MASK) == LP_ENC_6BIT_STR)
        return 1 + (b & 0x3Fu);
    if ((b & LP_ENC_13BIT_INT_MASK) == LP_ENC_13BIT_INT)
        return 2;
    if ((b & LP_ENC_12BIT_STR_MASK) == LP_ENC_12BIT_STR) {
        avail = (uint64_t)(lp + total) - (uint64_t)p;
        if (avail < 2)
            return UINT64_MAX;
        return 2 + (((uint64_t)(b & 0x0Fu) << 8) | p[1]);
    }
    if (b == LP_ENC_32BIT_STR) {
        uint32_t slen;
        avail = (uint64_t)(lp + total) - (uint64_t)p;
        if (avail < 5)
            return UINT64_MAX;
        slen = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) |
               ((uint32_t)p[4] << 24);
        return 5 + (uint64_t)slen;
    }
    if (b == LP_ENC_16BIT_INT)
        return 3;
    if (b == LP_ENC_24BIT_INT)
        return 4;
    if (b == LP_ENC_32BIT_INT)
        return 5;
    if (b == LP_ENC_64BIT_INT)
        return 9;
    return UINT64_MAX; /* EOF or unknown byte: not a valid entry */
}

static size_t lp_backlen_size(uint64_t entrylen)
{
    if (entrylen <= 127)
        return 1;
    if (entrylen < 16383)
        return 2;
    if (entrylen < 2097151)
        return 3;
    if (entrylen < 268435455)
        return 4;
    return 5;
}

static size_t lp_encode_backlen(unsigned char *buf, uint64_t l)
{
    if (l <= 127) {
        buf[0] = (unsigned char)l;
        return 1;
    }
    if (l < 16383) {
        buf[0] = (unsigned char)(l >> 7);
        buf[1] = (unsigned char)((l & 127) | 128);
        return 2;
    }
    if (l < 2097151) {
        buf[0] = (unsigned char)(l >> 14);
        buf[1] = (unsigned char)(((l >> 7) & 127) | 128);
        buf[2] = (unsigned char)((l & 127) | 128);
        return 3;
    }
    if (l < 268435455) {
        buf[0] = (unsigned char)(l >> 21);
        buf[1] = (unsigned char)(((l >> 14) & 127) | 128);
        buf[2] = (unsigned char)(((l >> 7) & 127) | 128);
        buf[3] = (unsigned char)((l & 127) | 128);
        return 4;
    }
    buf[0] = (unsigned char)(l >> 28);
    buf[1] = (unsigned char)(((l >> 21) & 127) | 128);
    buf[2] = (unsigned char)(((l >> 14) & 127) | 128);
    buf[3] = (unsigned char)(((l >> 7) & 127) | 128);
    buf[4] = (unsigned char)((l & 127) | 128);
    return 5;
}

/* p points one past the previous entry (i.e. at the next entry or EOF).
 * Returns the previous entry's backlen value, reading backwards. */
static uint64_t lp_decode_backlen(const unsigned char *lp, const unsigned char *p)
{
    uint64_t val = 0;
    uint64_t shift = 0;
    const unsigned char *q = p - 1; /* last byte of the backlen */
    for (;;) {
        if (q < lp + LP_HDR_SIZE)
            return UINT64_MAX; /* corrupt: ran past the header */
        val |= (uint64_t)(q[0] & 127u) << shift;
        if (!(q[0] & 128u))
            return val;
        shift += 7;
        if (shift > 28)
            return UINT64_MAX;
        q--;
    }
}

/* ------------------------------------------------------------------ */
/* strict int64 parse (canonical decimal form only)                    */
/* ------------------------------------------------------------------ */

static int lp_str_to_int64(const unsigned char *s, uint32_t slen, int64_t *out)
{
    uint64_t v = 0;
    uint32_t i;
    int neg = 0;
    if (slen == 0 || slen > 20)
        return 0;
    i = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (slen == 1)
            return 0;
    }
    if (s[i] == '0' && i + 1 < slen)
        return 0; /* leading zero ("01", "-0") is not canonical */
    for (; i < slen; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > (uint64_t)INT64_MAX + (neg ? 1u : 0u))
            return 0;
    }
    if (neg && v == 0)
        return 0; /* "-0" is not canonical */
    if (neg) {
        if (v == (uint64_t)INT64_MAX + 1u)
            *out = INT64_MIN;
        else
            *out = -(int64_t)v;
    } else {
        *out = (int64_t)v;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

unsigned char *lp_new(void)
{
    unsigned char *lp = (unsigned char *)malloc(LP_HDR_SIZE + 1);
    if (lp == NULL)
        lp_die_oom();
    lp_store_total(lp, LP_HDR_SIZE + 1);
    lp_store_numele(lp, 0);
    lp[LP_HDR_SIZE] = LP_EOF;
    return lp;
}

void lp_free(unsigned char *lp)
{
    free(lp);
}

size_t lp_bytes(const unsigned char *lp)
{
    return lp_load_total(lp);
}

uint64_t lp_length(const unsigned char *lp)
{
    uint16_t n = lp_load_numele(lp);
    uint64_t count = 0;
    unsigned char *p;
    if (n != LP_NUMELE_UNKNOWN)
        return n;
    for (p = lp_first((unsigned char *)lp); p != NULL; p = lp_next(lp, p))
        count++;
    return count;
}

unsigned char *lp_first(unsigned char *lp)
{
    unsigned char *p = lp + LP_HDR_SIZE;
    if (*p == LP_EOF)
        return NULL;
    return p;
}

unsigned char *lp_last(unsigned char *lp)
{
    unsigned char *eof = lp + lp_load_total(lp) - 1;
    if (eof == lp + LP_HDR_SIZE)
        return NULL;
    return lp_prev(lp, eof);
}

unsigned char *lp_next(const unsigned char *lp, const unsigned char *p)
{
    uint32_t total = lp_load_total(lp);
    uint64_t esz = lp_entry_payload_size(lp, p);
    uint64_t entry_total;
    const unsigned char *next;
    if (esz == UINT64_MAX)
        return NULL;
    entry_total = esz + lp_backlen_size(esz);
    if ((uint64_t)(p - lp) + entry_total >= (uint64_t)total)
        return NULL; /* would land past the EOF */
    next = p + entry_total;
    if (next >= lp + total || *next == LP_EOF)
        return NULL;
    return (unsigned char *)next;
}

unsigned char *lp_prev(const unsigned char *lp, const unsigned char *p)
{
    uint64_t backlen;
    size_t blen_size;
    const unsigned char *prev;
    if (p <= lp + LP_HDR_SIZE)
        return NULL; /* nothing before the first entry */
    backlen = lp_decode_backlen(lp, p);
    if (backlen == UINT64_MAX)
        return NULL;
    blen_size = lp_backlen_size(backlen);
    if ((uint64_t)(p - lp) < backlen + blen_size)
        return NULL;
    prev = p - backlen - blen_size;
    if (prev < lp + LP_HDR_SIZE)
        return NULL;
    return (unsigned char *)prev;
}

unsigned char *lp_seek(unsigned char *lp, long index)
{
    long forward = 1;
    uint16_t numele = lp_load_numele(lp);
    unsigned char *p;
    long i;
    if (numele != LP_NUMELE_UNKNOWN) {
        if (index < 0) {
            index += (long)numele;
            if (index < 0)
                return NULL;
        }
        if ((uint64_t)index >= numele)
            return NULL;
        if ((uint64_t)index > numele / 2) {
            forward = 0;
            index = (long)numele - index - 1;
        }
    } else if (index < 0) {
        forward = 0;
        index = -index - 1;
    }
    if (forward) {
        p = lp_first(lp);
        for (i = 0; p != NULL && i < index; i++)
            p = lp_next(lp, p);
        return p;
    }
    p = lp_last(lp);
    for (i = 0; p != NULL && i < index; i++)
        p = lp_prev(lp, p);
    return p;
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */

const unsigned char *lp_get(const unsigned char *p, uint32_t *slen,
                            int64_t *ival)
{
    unsigned char b = p[0];
    if ((b & LP_ENC_7BIT_UINT_MASK) == LP_ENC_7BIT_UINT) {
        *ival = b & 0x7Fu;
        return NULL;
    }
    if ((b & LP_ENC_6BIT_STR_MASK) == LP_ENC_6BIT_STR) {
        *slen = b & 0x3Fu;
        return p + 1;
    }
    if ((b & LP_ENC_13BIT_INT_MASK) == LP_ENC_13BIT_INT) {
        uint16_t u = (uint16_t)(((uint16_t)(b & 0x1Fu) << 8) | p[1]);
        if (u & 0x1000u) /* sign-extend 13 bits */
            *ival = (int64_t)(int16_t)(u | 0xE000u);
        else
            *ival = (int64_t)u;
        return NULL;
    }
    if ((b & LP_ENC_12BIT_STR_MASK) == LP_ENC_12BIT_STR) {
        *slen = (uint32_t)(((uint32_t)(b & 0x0Fu) << 8) | p[1]);
        return p + 2;
    }
    if (b == LP_ENC_32BIT_STR) {
        *slen = (uint32_t)p[1] | ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 16) |
                ((uint32_t)p[4] << 24);
        return p + 5;
    }
    if (b == LP_ENC_16BIT_INT) {
        *ival = (int64_t)(int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        return NULL;
    }
    if (b == LP_ENC_24BIT_INT) {
        uint32_t u = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                     ((uint32_t)p[3] << 16);
        if (u & 0x800000u)
            u |= 0xFF000000u;
        *ival = (int64_t)(int32_t)u;
        return NULL;
    }
    if (b == LP_ENC_32BIT_INT) {
        *ival = (int64_t)(int32_t)((uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                                   ((uint32_t)p[3] << 16) |
                                   ((uint32_t)p[4] << 24));
        return NULL;
    }
    if (b == LP_ENC_64BIT_INT) {
        uint64_t u = 0;
        int i;
        for (i = 0; i < 8; i++)
            u |= (uint64_t)p[1 + i] << (8 * i);
        *ival = (int64_t)u;
        return NULL;
    }
    *slen = 0;
    return NULL;
}

const unsigned char *lp_get_str(const unsigned char *p, unsigned char buf[64],
                                uint32_t *slen)
{
    int64_t iv;
    const unsigned char *s = lp_get(p, slen, &iv);
    int n;
    if (s != NULL)
        return s;
    n = snprintf((char *)buf, 64, "%lld", (long long)iv);
    *slen = (uint32_t)(n < 0 ? 0 : n);
    return buf;
}

/* ------------------------------------------------------------------ */
/* mutation                                                            */
/* ------------------------------------------------------------------ */

/* Encode s into buf (encoding+payload+backlen); returns the total entry
 * size. buf must have room for 10 + slen bytes. */
static uint64_t lp_encode_entry(unsigned char *buf, const unsigned char *s,
                                uint32_t slen)
{
    int64_t iv = 0;
    uint64_t enclen;
    if (lp_str_to_int64(s, slen, &iv)) {
        if (iv >= 0 && iv <= 127) {
            buf[0] = (unsigned char)iv;
            enclen = 1;
        } else if (iv >= -4096 && iv <= 4095) {
            uint16_t u = (uint16_t)(iv & 0x1FFF);
            buf[0] = (unsigned char)(LP_ENC_13BIT_INT | ((u >> 8) & 0x1Fu));
            buf[1] = (unsigned char)(u & 0xFFu);
            enclen = 2;
        } else if (iv >= -32768 && iv <= 32767) {
            uint16_t u = (uint16_t)iv;
            buf[0] = LP_ENC_16BIT_INT;
            buf[1] = (unsigned char)(u & 0xFFu);
            buf[2] = (unsigned char)((u >> 8) & 0xFFu);
            enclen = 3;
        } else if (iv >= -8388608 && iv <= 8388607) {
            uint32_t u = (uint32_t)iv;
            buf[0] = LP_ENC_24BIT_INT;
            buf[1] = (unsigned char)(u & 0xFFu);
            buf[2] = (unsigned char)((u >> 8) & 0xFFu);
            buf[3] = (unsigned char)((u >> 16) & 0xFFu);
            enclen = 4;
        } else if (iv >= INT32_MIN && iv <= INT32_MAX) {
            uint32_t u = (uint32_t)iv;
            buf[0] = LP_ENC_32BIT_INT;
            buf[1] = (unsigned char)(u & 0xFFu);
            buf[2] = (unsigned char)((u >> 8) & 0xFFu);
            buf[3] = (unsigned char)((u >> 16) & 0xFFu);
            buf[4] = (unsigned char)((u >> 24) & 0xFFu);
            enclen = 5;
        } else {
            uint64_t u = (uint64_t)iv;
            int i;
            buf[0] = LP_ENC_64BIT_INT;
            for (i = 0; i < 8; i++)
                buf[1 + i] = (unsigned char)((u >> (8 * i)) & 0xFFu);
            enclen = 9;
        }
    } else {
        if (slen <= 63) {
            buf[0] = (unsigned char)(LP_ENC_6BIT_STR | slen);
            memcpy(buf + 1, s, slen);
            enclen = 1 + slen;
        } else if (slen <= 4095) {
            buf[0] = (unsigned char)(LP_ENC_12BIT_STR | (slen >> 8));
            buf[1] = (unsigned char)(slen & 0xFFu);
            memcpy(buf + 2, s, slen);
            enclen = 2 + slen;
        } else {
            buf[0] = LP_ENC_32BIT_STR;
            buf[1] = (unsigned char)(slen & 0xFFu);
            buf[2] = (unsigned char)((slen >> 8) & 0xFFu);
            buf[3] = (unsigned char)((slen >> 16) & 0xFFu);
            buf[4] = (unsigned char)((slen >> 24) & 0xFFu);
            memcpy(buf + 5, s, slen);
            enclen = 5 + slen;
        }
    }
    return enclen + lp_encode_backlen(buf + enclen, enclen);
}

static void lp_bump_numele(unsigned char *lp, int delta)
{
    uint16_t n = lp_load_numele(lp);
    if (n == LP_NUMELE_UNKNOWN)
        return;
    if (delta > 0) {
        if ((uint32_t)n + 1 >= LP_NUMELE_UNKNOWN)
            lp_store_numele(lp, LP_NUMELE_UNKNOWN);
        else
            lp_store_numele(lp, (uint16_t)(n + 1));
    } else {
        lp_store_numele(lp, (uint16_t)(n - 1));
    }
}

unsigned char *lp_insert(unsigned char *lp, const unsigned char *s,
                         uint32_t slen, unsigned char *p, int where,
                         unsigned char **newp)
{
    uint32_t total = lp_load_total(lp);
    size_t off;
    uint64_t entry_bytes;
    unsigned char *enc;
    uint64_t newtotal;
    unsigned char *nlp;

    if (p == NULL) {
        p = lp + total - 1; /* the EOF byte */
        where = LP_BEFORE;
    }
    off = (size_t)(p - lp);
    if (where == LP_AFTER) {
        uint64_t esz = lp_entry_payload_size(lp, p);
        off += (size_t)(esz + lp_backlen_size(esz));
    }

    enc = (unsigned char *)malloc((size_t)slen + 10 + 5);
    if (enc == NULL)
        lp_die_oom();
    entry_bytes = lp_encode_entry(enc, s, slen);

    newtotal = (uint64_t)total + entry_bytes;
    if (newtotal > UINT32_MAX) {
        free(enc);
        fprintf(stderr, "ddup: listpack too large\n");
        exit(1);
    }
    nlp = (unsigned char *)realloc(lp, (size_t)newtotal);
    if (nlp == NULL)
        lp_die_oom();
    lp = nlp;
    memmove(lp + off + entry_bytes, lp + off, total - off); /* incl. EOF */
    memcpy(lp + off, enc, (size_t)entry_bytes);
    free(enc);
    lp_store_total(lp, (uint32_t)newtotal);
    lp_bump_numele(lp, 1);
    if (newp != NULL)
        *newp = lp + off;
    return lp;
}

unsigned char *lp_append(unsigned char *lp, const unsigned char *s,
                         uint32_t slen)
{
    return lp_insert(lp, s, slen, NULL, LP_BEFORE, NULL);
}

unsigned char *lp_prepend(unsigned char *lp, const unsigned char *s,
                          uint32_t slen)
{
    /* lp + LP_HDR_SIZE is the first entry, or the EOF when empty. */
    return lp_insert(lp, s, slen, lp + LP_HDR_SIZE, LP_BEFORE, NULL);
}

unsigned char *lp_delete(unsigned char *lp, unsigned char *p,
                         unsigned char **newp)
{
    uint32_t total = lp_load_total(lp);
    size_t off = (size_t)(p - lp);
    uint64_t esz = lp_entry_payload_size(lp, p);
    uint64_t entry_bytes;
    uint32_t newtotal;
    unsigned char *nlp;

    if (*p == LP_EOF || esz == UINT64_MAX) {
        fprintf(stderr, "ddup: lp_delete at EOF\n");
        exit(1);
    }
    entry_bytes = esz + lp_backlen_size(esz);
    memmove(lp + off, lp + off + entry_bytes,
            (size_t)((uint64_t)total - off - entry_bytes));
    newtotal = (uint32_t)((uint64_t)total - entry_bytes);
    nlp = (unsigned char *)realloc(lp, newtotal);
    if (nlp == NULL)
        lp_die_oom();
    lp = nlp;
    lp_store_total(lp, newtotal);
    lp_bump_numele(lp, -1);
    if (newp != NULL) {
        if (off >= (size_t)newtotal - 1)
            *newp = NULL; /* deleted the tail */
        else
            *newp = lp + off;
    }
    return lp;
}

unsigned char *lp_replace(unsigned char *lp, unsigned char *p,
                          const unsigned char *s, uint32_t slen)
{
    unsigned char *np = NULL;
    lp = lp_delete(lp, p, &np);
    /* np sits at the deleted slot; NULL means we removed the tail. */
    return lp_insert(lp, s, slen, np, LP_BEFORE, NULL);
}

/* ------------------------------------------------------------------ */
/* search                                                              */
/* ------------------------------------------------------------------ */

static int lp_entry_equal(const unsigned char *p, const unsigned char *s,
                          uint32_t slen)
{
    uint32_t elen = 0;
    int64_t iv = 0;
    const unsigned char *e = lp_get(p, &elen, &iv);
    if (e != NULL)
        return elen == slen && memcmp(e, s, slen) == 0;
    /* int entry: matches only the canonical decimal form */
    {
        int64_t sv;
        return lp_str_to_int64(s, slen, &sv) && sv == iv;
    }
}

unsigned char *lp_find(unsigned char *lp, unsigned char *p,
                       const unsigned char *s, uint32_t slen)
{
    if (p == NULL)
        p = lp_first(lp);
    else
        p = lp_next(lp, p); /* continue strictly after the previous hit */
    while (p != NULL) {
        if (lp_entry_equal(p, s, slen))
            return p;
        p = lp_next(lp, p);
    }
    return NULL;
}
