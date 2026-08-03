/* resp_parser.c - recursive-descent RESP parser (RESP2 + RESP3 types).
 *
 * The parser is stateless: resp_parse() tries to parse one complete value
 * from the given buffer and reports how many bytes it consumed. Callers
 * implementing streaming just keep unconsumed bytes in the receive buffer.
 */
#include "resp/resp_parser.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define RESP_MAX_DEPTH 512
#define RESP_MAX_ARRAY_LEN (1024LL * 1024LL * 1024LL) /* protocol sanity cap */

static const char *find_crlf(const char *p, const char *end)
{
    /* memchr for '\r', then verify '\n' follows. */
    while (p < end) {
        const char *cr = memchr(p, '\r', (size_t)(end - p));
        if (!cr || cr + 1 >= end)
            return NULL;
        if (cr[1] == '\n')
            return cr;
        p = cr + 1;
    }
    return NULL;
}

/* Parse a signed decimal integer in [p, end). Strict: optional '-', at least
 * one digit, digits only, no overflow. Returns 0 on success. */
static int parse_ll(const char *p, const char *end, long long *out)
{
    int neg = 0;
    unsigned long long v = 0;
    if (p == end)
        return -1;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p == end)
        return -1;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        unsigned digit = (unsigned)(*p - '0');
        if (v > (unsigned long long)LLONG_MAX / 10 ||
            (v == (unsigned long long)LLONG_MAX / 10 &&
             digit > (unsigned)(LLONG_MAX % 10) + (neg ? 1u : 0u)))
            return -1;
        v = v * 10 + digit;
    }
    if (neg)
        *out = (v == (unsigned long long)LLONG_MAX + 1ULL)
                   ? LLONG_MIN
                   : -(long long)v;
    else
        *out = (long long)v;
    return 0;
}

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

    switch (type) {
    case '+':
    case '-':
    case ':':
    case '_':
    case '#':
    case ',':
    case '(': {
        /* Line-based types: <type><payload>\r\n */
        const char *crlf = find_crlf(p, end);
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
                out->dbl = 1.0 / 0.0;
            else if (crlf - p == 4 && memcmp(p, "-inf", 4) == 0)
                out->dbl = -1.0 / 0.0;
            else if (crlf - p == 3 && memcmp(p, "nan", 3) == 0)
                out->dbl = 0.0 / 0.0;
            else {
                /* strtod needs NUL termination; copy into a small arena
                 * scratch buffer (payloads are short by design). */
                size_t n = (size_t)(crlf - p);
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
        const char *crlf = find_crlf(p, end);
        if (!crlf)
            return 0;
        long long blen;
        if (parse_ll(p, crlf, &blen) != 0)
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
        const char *crlf = find_crlf(p, end);
        if (!crlf)
            return 0;
        long long count;
        if (parse_ll(p, crlf, &count) != 0)
            return -1;
        if (type == '*' && count == -1) {
            out->type = RESP_ARRAY;
            out->items = NULL;
            out->count = 0;
            *pos = crlf + 2;
            return 1;
        }
        if (count < 0 || count > RESP_MAX_ARRAY_LEN)
            return -1;
        size_t n = (size_t)count;
        size_t slots = (type == '%') ? n * 2 : n;
        if (type == '%' && n > (size_t)RESP_MAX_ARRAY_LEN / 2)
            return -1;

        out->type = (type == '*')  ? RESP_ARRAY
                    : (type == '%') ? RESP_MAP
                    : (type == '~') ? RESP_SET
                                    : RESP_PUSH;
        out->count = slots;
        out->items = NULL;
        const char *cur = crlf + 2;
        if (slots > 0) {
            out->items = arena_alloc(a, slots * sizeof(resp_value));
            if (!out->items)
                return -1;
            for (size_t i = 0; i < slots; i++) {
                int r = parse_at(start, end, &cur, &out->items[i], a, depth + 1);
                if (r <= 0)
                    return r;
            }
        }
        *pos = cur;
        return 1;
    }

    default:
        return -1;
    }
}

ptrdiff_t resp_parse(const char *buf, size_t len, resp_value *out, arena *a)
{
    const char *pos = buf;
    int r = parse_at(buf, buf + len, &pos, out, a, 0);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    return pos - buf;
}
