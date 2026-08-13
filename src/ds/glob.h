/* glob.h - Redis-style glob pattern matching (stringmatchlen semantics).
 *
 * Supported: '*' (any run), '?' (one char), '[...]' classes with ranges
 * and '^' negation, '\' escaping (also inside classes). An unterminated
 * '[' class is treated as a literal '[' (documented simplification: Redis
 * matches nothing for most unterminated classes). */
#ifndef DDUP_GLOB_H
#define DDUP_GLOB_H

#include <stddef.h>

/* Returns 1 when str[0..slen) matches pat[0..plen), 0 otherwise.
 * Binary-safe: neither buffer needs to be NUL-terminated. */
int ddup_glob_match(const char *pat, size_t plen, const char *str,
                    size_t slen);

#endif /* DDUP_GLOB_H */
