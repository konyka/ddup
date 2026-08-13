/* resp_parser.h - incremental RESP parser.
 *
 * resp_parse() attempts to parse exactly one value from buf[0..len).
 * Returns:
 *   > 0  success: number of bytes consumed; *out is filled.
 *   = 0  incomplete: more bytes needed; *out untouched.
 *   = -1 protocol error.
 *
 * Nested values (arrays/maps/sets/pushes) are allocated from arena `a`;
 * the caller resets the arena after the parsed value has been consumed.
 * String payloads are zero-copy views into `buf`, which must outlive the
 * returned value.
 */
#ifndef DDUP_RESP_PARSER_H
#define DDUP_RESP_PARSER_H

#include <stddef.h>

#include "core/arena.h"
#include "resp/resp.h"

ptrdiff_t resp_parse(const char *buf, size_t len, resp_value *out, arena *a);

#ifdef DDUP_TESTING
int resp_test_aggregate_bytes(size_t slots, size_t *bytes);
#endif

#endif /* DDUP_RESP_PARSER_H */
