/* hashslot.h - Redis cluster hash slot: CRC16-XMODEM + hashtag rule.
 *
 * slot = crc16(hashtag(key)) % 16384. The hashtag is the substring between
 * the first '{' and the next '}' when it is non-empty; otherwise the whole
 * key (same rules as Redis).
 */
#ifndef DDUP_HASHSLOT_H
#define DDUP_HASHSLOT_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/XMODEM (poly 0x1021, init 0, MSB-first), table-driven. */
uint16_t crc16(const char *buf, size_t len);

/* Extract the hashtag into out (NUL-terminated when cap allows); returns
 * the hashtag length (may exceed cap-1; always truncated safely). */
size_t hash_tag(const char *key, size_t klen, char *out, size_t cap);

/* Hash slot in [0, 16384). */
uint32_t hash_slot(const char *key, size_t klen);

#endif /* DDUP_HASHSLOT_H */
