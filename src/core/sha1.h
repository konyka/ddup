/* sha1.h - SHA-1 message digest (used for script cache keys, EVALSHA). */
#ifndef DDUP_SHA1_H
#define DDUP_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct sha1_ctx {
    uint32_t h[5];
    uint64_t total; /* bytes fed so far */
    uint8_t buf[64];
    size_t buflen;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
/* writes the 40-char lowercase hex digest + NUL */
void sha1_final_hex(sha1_ctx *c, char out_hex[41]);

/* one-shot convenience */
void sha1_hex(const void *data, size_t len, char out_hex[41]);

#endif /* DDUP_SHA1_H */
