/* resp_parser.c - recursive-descent RESP parser (RESP2 + RESP3 types).
 *
 * The parser is stateless: resp_parse() tries to parse one complete value
 * from the given buffer and reports how many bytes it consumed. Callers
 * implementing streaming just keep unconsumed bytes in the receive buffer.
 */
#include "resp/resp_parser.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pal/pal_simd.h"

#define RESP_MAX_DEPTH 512
#define RESP_MAX_ARRAY_LEN (1024LL * 1024LL * 1024LL) /* protocol sanity cap */

static int resp_aggregate_bytes(size_t slots, size_t *bytes)
{
    if (bytes == NULL)
        return -1;
    if (slots > SIZE_MAX / sizeof(resp_value))
        return -1;
    *bytes = slots * sizeof(resp_value);
    return 0;
}

#ifdef DDUP_TESTING
int resp_test_aggregate_bytes(size_t slots, size_t *bytes)
{
    return resp_aggregate_bytes(slots, bytes);
}
#endif

/* Parse a signed decimal integer in [p, end). Strict: optional '-', at least
 * one digit, digits only, no overflow. Returns 0 on success. */
static int parse_ll(const char *p, const char *end, long long *out)
{
    int neg = 0;
    if (p == end)
        return -1;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p == end)
        return -1;

    /* Strip padding before selecting the common short-integer path. */
    while (p < end && *p == '0')
        p++;
    if (p == end) {
        *out = 0;
        return 0;
    }

    size_t digits = (size_t)(end - p);
    if (digits > 19)
        return -1;

    unsigned long long v = 0;
    if (digits < 19) {
        for (; p < end; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            v = v * 10ULL + (unsigned long long)(*p - '0');
        }
    } else {
        const unsigned long long limit =
            (unsigned long long)LLONG_MAX + (neg ? 1ULL : 0ULL);
        for (; p < end; p++) {
            if (*p < '0' || *p > '9')
                return -1;
            unsigned digit = (unsigned)(*p - '0');
            if (v > limit / 10ULL ||
                (v == limit / 10ULL && digit > (unsigned)(limit % 10ULL)))
                return -1;
            v = v * 10ULL + (unsigned long long)digit;
        }
    }
    if (neg)
        *out = (v == (unsigned long long)LLONG_MAX + 1ULL)
                   ? LLONG_MIN
                   : -(long long)v;
    else
        *out = (long long)v;
    return 0;
}

/* Bulk and aggregate lengths are non-negative (apart from the RESP2 null
 * bulk/array marker -1) and capped by the protocol sanity limit. Keeping a
 * size_t accumulator avoids the signed-overflow checks needed for integers. */
static int parse_bulk_len(const char *p, const char *end, long long *out)
{
    if (p == end)
        return -1;
    if (*p == '-') {
        if (end - p == 2 && p[1] == '1') {
            *out = -1;
            return 0;
        }
        return -1;
    }

    /* Ignore leading zeroes when applying the decimal-width guard. RESP
     * clients historically emit padded lengths, and those remain valid. */
    const char *first = p;
    while (first < end && *first == '0')
        first++;
    size_t significant_digits = (size_t)(end - first);
    if (significant_digits > 10)
        return -1;
    if (p == end) {
        *out = 0;
        return 0;
    }

    p = first;
    unsigned long long value = 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        value = value * 10ULL + (unsigned long long)(*p - '0');
    }
    if (value > (unsigned long long)RESP_MAX_ARRAY_LEN)
        return -1;
    *out = (long long)value;
    return 0;
}

#ifdef DDUP_TESTING
int resp_test_bulk_len(const char *p, const char *end, long long *out)
{
    return parse_bulk_len(p, end, out);
}
int resp_test_parse_integer(const char *p, const char *end, long long *out)
{
    return parse_ll(p, end, out);
}
#endif

/* Parse one value starting at *pos (absolute pointers into the buffer).
 * Returns 1 on success, 0 incomplete, -1 protocol error. On success *pos is
 * advanced past the value. */
static int parse_at(const char *start, const char *end, const char **pos,
                    resp_value *out, arena *a, int depth)
{
    if (depth > RESP_MAX_DEPTH)
        return -1;
    if (*pos >= end)
        return 0;

    const char *p = *pos;
    char type = *p++;
    out->is_null = 0;

    switch (type) {
    case '+':
    case '-':
    case ':':
    case '_':
    case '#':
    case ',':
    case '(': {
        /* Line-based types: <type><payload>\r\n */
        const char *crlf = ddup_find_crlf(p, end);
        if (!crlf)
            return 0;
        switch (type) {
        case '+':
            out->type = RESP_SIMPLE_STRING;
            out->str = p;
            out->len = (size_t)(crlf - p);
            break;
        case '-':
            out->type = RESP_ERROR;
            out->str = p;
            out->len = (size_t)(crlf - p);
            break;
        case ':':
            out->type = RESP_INTEGER;
            if (parse_ll(p, crlf, &out->integer) != 0)
                return -1;
            break;
        case '_':
            if (crlf != p) /* payload must be empty */
                return -1;
            out->type = RESP_NULL;
            break;
        case '#':
            if (crlf - p != 1 || (*p != 't' && *p != 'f'))
                return -1;
            out->type = RESP_BOOLEAN;
            out->integer = (*p == 't') ? 1 : 0;
            break;
        case ',': {
            out->type = RESP_DOUBLE;
            if (crlf - p == 3 && memcmp(p, "inf", 3) == 0)
                out->dbl = INFINITY;
            else if (crlf - p == 4 && memcmp(p, "-inf", 4) == 0)
                out->dbl = -INFINITY;
            else if (crlf - p == 3 && memcmp(p, "nan", 3) == 0)
                out->dbl = NAN;
            else {
                /* strtod needs NUL termination; copy into a small arena
                 * scratch buffer (payloads are short by design). */
                size_t n = (size_t)(crlf - p);
                if (n == 0)
                    return -1;
                char *tmp = arena_alloc(a, n + 1);
                if (!tmp)
                    return -1;
                memcpy(tmp, p, n);
                tmp[n] = '\0';
                char *endp = NULL;
                out->dbl = strtod(tmp, &endp);
                if (endp != tmp + n)
                    return -1;
            }
            break;
        }
        case '(':
            out->type = RESP_BIG_NUMBER;
            out->str = p;
            out->len = (size_t)(crlf - p);
            break;
        }
        *pos = crlf + 2;
        return 1;
    }

    case '$':
    case '!':
    case '=': {
        /* Blob types: <type><len>\r\n<payload>\r\n ; $-1 = null bulk */
        const char *crlf = ddup_find_crlf(p, end);
        if (!crlf)
            return 0;
        long long blen;
        if (parse_bulk_len(p, crlf, &blen) != 0)
            return -1;
        if (type == '$' && blen == -1) {
            out->type = RESP_BULK_STRING;
            out->str = NULL;
            out->len = 0;
            *pos = crlf + 2;
            return 1;
        }
        if (blen < 0 || blen > RESP_MAX_ARRAY_LEN)
            return -1;
        const char *payload = crlf + 2;
        if ((size_t)(end - payload) < (size_t)blen + 2)
            return 0; /* incomplete */
        if (payload[blen] != '\r' || payload[blen + 1] != '\n')
            return -1;
        out->type = (type == '$')  ? RESP_BULK_STRING
                    : (type == '!') ? RESP_BLOB_ERROR
                                    : RESP_VERBATIM_STRING;
        out->str = payload;
        out->len = (size_t)blen;
        *pos = payload + blen + 2;
        return 1;
    }

    case '*':
    case '%':
    case '~':
    case '>': {
        /* Aggregate types: <type><count>\r\n<children> ; *-1 = null array.
         * For maps the count is a pair count; items holds 2*count values. */
        const char *crlf = ddup_find_crlf(p, end);
        if (!crlf)
            return 0;
        long long count;
        if (parse_bulk_len(p, crlf, &count) != 0)
            return -1;
        if (type == '*' && count == -1) {
            out->type = RESP_ARRAY;
            out->items = NULL;
            out->count = 0;
            out->is_null = 1;
            *pos = crlf + 2;
            return 1;
        }
        if (count < 0 || count > RESP_MAX_ARRAY_LEN)
            return -1;
        size_t n = (size_t)count;
        size_t slots = (type == '%') ? n * 2 : n;
        if (type == '%' && n > (size_t)RESP_MAX_ARRAY_LEN / 2)
            return -1;

        resp_value aggregate;
        memset(&aggregate, 0, sizeof(aggregate));
        aggregate.type = (type == '*')  ? RESP_ARRAY
                    : (type == '%') ? RESP_MAP
                    : (type == '~') ? RESP_SET
                                    : RESP_PUSH;
        aggregate.count = slots;
        const char *cur = crlf + 2;
        /* Every RESP child needs at least three bytes (for example `+\r\n`).
         * Do not reserve a potentially enormous item array until the current
         * input contains enough bytes to make progress toward all children. */
        if (slots > (size_t)(end - cur) / 3)
            return 0;
        if (slots > 0) {
            size_t i = 0;
            size_t bytes;
            if (resp_aggregate_bytes(slots, &bytes) != 0)
                return -1;
            aggregate.items = arena_alloc(a, bytes);
            if (!aggregate.items)
                return -1;
            /* Fast path (Phase 36): command traffic is overwhelmingly
             * *N arrays of $ bulks; parse bulk children inline instead of
             * re-entering the full recursive dispatch per child. The
             * semantics match the '$' case below exactly; the first
             * non-bulk child falls back to the general recursion. */
            if (type == '*') {
                while (i < slots && cur < end && *cur == '$') {
                    resp_value *it = &aggregate.items[i];
                    const char *bp = cur + 1;
                    const char *bcrlf = ddup_find_crlf(bp, end);
                    long long blen;
                    const char *payload;
                    if (!bcrlf)
                        return 0;
                    if (parse_bulk_len(bp, bcrlf, &blen) != 0)
                        return -1;
                    it->is_null = 0;
                    if (blen == -1) { /* null bulk */
                        it->type = RESP_BULK_STRING;
                        it->str = NULL;
                        it->len = 0;
                        cur = bcrlf + 2;
                        i++;
                        continue;
                    }
                    if (blen < 0 || blen > RESP_MAX_ARRAY_LEN)
                        return -1;
                    payload = bcrlf + 2;
                    if ((size_t)(end - payload) < (size_t)blen + 2)
                        return 0; /* incomplete */
                    if (payload[blen] != '\r' || payload[blen + 1] != '\n')
                        return -1;
                    it->type = RESP_BULK_STRING;
                    it->str = payload;
                    it->len = (size_t)blen;
                    cur = payload + blen + 2;
                    i++;
                }
            }
            for (; i < slots; i++) {
                int r = parse_at(start, end, &cur, &aggregate.items[i], a,
                                 depth + 1);
                if (r <= 0)
                    return r;
            }
        }
        *pos = cur;
        *out = aggregate;
        return 1;
    }

    default:
        return -1;
    }
}

ptrdiff_t resp_parse(const char *buf, size_t len, resp_value *out, arena *a)
{
    const char *pos = buf;
    arena_mark mark;
    if (out == NULL || a == NULL || (buf == NULL && len != 0))
        return -1;
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (buf == NULL)
        return 0;
    arena_mark_get(a, &mark);
    int r = parse_at(buf, buf + len, &pos, out, a, 0);
    if (r < 0) {
        arena_rewind(a, &mark);
        return -1;
    }
    if (r == 0) {
        arena_rewind(a, &mark);
        return 0;
    }
    return pos - buf;
}
