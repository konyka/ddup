/* listpack.h - Redis-compatible listpack compact encoding (Phase 6).
 *
 * A listpack is a single flat allocation:
 *
 *     {4B total-bytes LE}{2B num-entries LE}{entry ...}{0xFF}
 *
 * Each entry is one of the Redis 7 encodings (7-bit uint, 13/16/24/32/64-bit
 * int, 6/12/32-bit-length string) followed by a variable-length backlen that
 * covers the encoding+payload bytes, enabling reverse traversal.
 *
 * Strings that parse as canonical int64 (no leading zeros, no "-0", in
 * range) are stored as integers; lp_get() reports the entry as an int and
 * lp_get_str() materializes its decimal form.
 *
 * All mutators may realloc the listpack: EVERY pointer into it is
 * invalidated, callers must re-seek (or use the newp out-parameter).
 * num-entries saturates at 0xFFFF ("unknown"); lp_length() then falls back
 * to a linear scan.
 */
#ifndef DDUP_LISTPACK_H
#define DDUP_LISTPACK_H

#include <stddef.h>
#include <stdint.h>

#define LP_EOF 0xFFu

enum { LP_BEFORE = 0, LP_AFTER = 1 };

/* Empty listpack (7 bytes). Never NULL (exits on OOM). */
unsigned char *lp_new(void);
void lp_free(unsigned char *lp);

/* Value of the total-bytes field (header + entries + EOF). */
size_t lp_bytes(const unsigned char *lp);
uint64_t lp_length(const unsigned char *lp);

/* Entry navigation; NULL past either end. */
unsigned char *lp_first(unsigned char *lp);
unsigned char *lp_last(unsigned char *lp);
unsigned char *lp_next(const unsigned char *lp, const unsigned char *p);
unsigned char *lp_prev(const unsigned char *lp, const unsigned char *p);
/* Index from the head (or from the tail when negative); NULL out of range. */
unsigned char *lp_seek(unsigned char *lp, long index);

/* Decode the entry at p. String entries: returns the payload pointer and
 * sets *slen. Integer entries: returns NULL and sets *ival. */
const unsigned char *lp_get(const unsigned char *p, uint32_t *slen,
                            int64_t *ival);
/* Always-as-string view: integer entries are decimal-materialized into buf
 * (at most 20 bytes + NUL needed). Returns payload/buf and sets *slen. */
const unsigned char *lp_get_str(const unsigned char *p, unsigned char buf[64],
                                uint32_t *slen);

/* Append/prepend/insert an element (int-encoded when s is canonical int64).
 * lp_insert inserts before/after p; p == NULL appends at the tail.
 * When newp is non-NULL it receives a pointer to the inserted entry. */
unsigned char *lp_append(unsigned char *lp, const unsigned char *s,
                         uint32_t slen);
unsigned char *lp_prepend(unsigned char *lp, const unsigned char *s,
                          uint32_t slen);
unsigned char *lp_insert(unsigned char *lp, const unsigned char *s,
                         uint32_t slen, unsigned char *p, int where,
                         unsigned char **newp);
/* Remove the entry at p (p must not be the EOF). When newp is non-NULL it
 * receives the entry that now sits at the deleted slot, or NULL when the
 * tail was removed. */
unsigned char *lp_delete(unsigned char *lp, unsigned char *p,
                         unsigned char **newp);
/* Replace the entry at p with s. */
unsigned char *lp_replace(unsigned char *lp, unsigned char *p,
                          const unsigned char *s, uint32_t slen);

/* Search from p onward (from the head when p == NULL) for an entry equal
 * to s. Integer entries match their canonical decimal form only. Returns
 * the entry pointer or NULL. */
unsigned char *lp_find(unsigned char *lp, unsigned char *p,
                       const unsigned char *s, uint32_t slen);

#endif /* DDUP_LISTPACK_H */
