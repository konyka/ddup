/* repl.c - replication backlog ring buffer; see repl.h. */
#include "server/repl.h"

#include <stdlib.h>
#include <string.h>

static size_t ring_advance(size_t pos, size_t n, size_t cap)
{
    size_t tail = cap - pos;
    return n >= tail ? n - tail : pos + n;
}

static int backlog_state_valid(const repl_backlog *b)
{
    return b != NULL && b->buf != NULL && b->cap != 0 &&
           b->start < b->cap && b->len <= b->cap;
}

int repl_backlog_init(repl_backlog *b, size_t cap)
{
    if (b == NULL)
        return -1;
    b->buf = NULL;
    b->cap = 0;
    b->start = 0;
    b->len = 0;
    b->offset = 0;
    if (cap == 0)
        return -1;
    b->buf = (char *)malloc(cap);
    if (b->buf == NULL)
        return -1;
    b->cap = cap;
    return 0;
}

void repl_backlog_free(repl_backlog *b)
{
    if (b == NULL)
        return;
    free(b->buf);
    b->buf = NULL;
    b->cap = 0;
    b->start = 0;
    b->len = 0;
    b->offset = 0;
}

void repl_backlog_append(repl_backlog *b, const char *data, size_t n)
{
    size_t drop;
    size_t pos;
    size_t tail;
    if (b == NULL || (data == NULL && n != 0))
        return;
    if (b->buf != NULL && !backlog_state_valid(b))
        return;
    if (n > UINT64_MAX - b->offset)
        b->offset = UINT64_MAX;
    else
        b->offset += n;
    if (b->buf == NULL || b->cap == 0 || n == 0)
        return;
    if (n >= b->cap) { /* keep only the newest cap bytes */
        data += n - b->cap;
        n = b->cap;
        b->start = 0;
        b->len = 0;
    }
    if (n > b->cap - b->len) {
        drop = n - (b->cap - b->len);
        b->start = ring_advance(b->start, drop, b->cap);
        b->len -= drop;
    }
    pos = ring_advance(b->start, b->len, b->cap);
    tail = b->cap - pos;
    if (tail > n)
        tail = n;
    memcpy(b->buf + pos, data, tail);
    if (tail < n)
        memcpy(b->buf, data + tail, n - tail);
    b->len += n;
}

size_t repl_backlog_read(const repl_backlog *b, char *out, size_t max)
{
    size_t n;
    size_t tail;
    if (out == NULL || !backlog_state_valid(b) || b->len == 0 || max == 0)
        return 0;
    n = b->len < max ? b->len : max;
    tail = b->cap - b->start;
    if (tail > n)
        tail = n;
    memcpy(out, b->buf + b->start, tail);
    if (tail < n)
        memcpy(out + tail, b->buf, n - tail);
    return n;
}

size_t repl_backlog_read_from(const repl_backlog *b, uint64_t from_offset,
                              char *out, size_t max)
{
    uint64_t base;
    uint64_t distance;
    size_t skip;
    size_t avail;
    size_t n;
    size_t pos;
    size_t tail;
    if (out == NULL || !backlog_state_valid(b) || b->len == 0 || max == 0)
        return 0;
    if (from_offset >= b->offset)
        return 0;
    base = b->offset - b->len;
    if (from_offset <= base)
        skip = 0;
    else {
        distance = from_offset - base;
        if (distance >= b->len)
            return 0;
        skip = (size_t)distance;
    }
    avail = b->len - skip;
    n = avail < max ? avail : max;
    pos = ring_advance(b->start, skip, b->cap);
    tail = b->cap - pos;
    if (tail > n)
        tail = n;
    memcpy(out, b->buf + pos, tail);
    if (tail < n)
        memcpy(out + tail, b->buf, n - tail);
    return n;
}
