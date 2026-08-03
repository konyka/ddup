/* repl.c - replication backlog ring buffer; see repl.h. */
#include "server/repl.h"

#include <stdlib.h>
#include <string.h>

void repl_backlog_init(repl_backlog *b, size_t cap)
{
    b->buf = (char *)malloc(cap);
    if (b->buf == NULL)
        return;
    b->cap = cap;
    b->start = 0;
    b->len = 0;
    b->offset = 0;
}

void repl_backlog_free(repl_backlog *b)
{
    free(b->buf);
    b->buf = NULL;
    b->cap = 0;
    b->len = 0;
}

void repl_backlog_append(repl_backlog *b, const char *data, size_t n)
{
    size_t tail;
    b->offset += n;
    if (n >= b->cap) { /* keep only the newest cap bytes */
        data += n - b->cap;
        n = b->cap;
        b->start = 0;
        b->len = 0;
    }
    while (b->len + n > b->cap) {
        b->start = (b->start + 1) % b->cap;
        b->len--;
    }
    tail = b->cap - ((b->start + b->len) % b->cap);
    if (tail > n)
        tail = n;
    memcpy(b->buf + (b->start + b->len) % b->cap, data, tail);
    if (tail < n)
        memcpy(b->buf, data + tail, n - tail);
    b->len += n;
}

size_t repl_backlog_read(const repl_backlog *b, char *out, size_t max)
{
    size_t n = b->len < max ? b->len : max;
    size_t tail = b->cap - b->start;
    if (tail > n)
        tail = n;
    memcpy(out, b->buf + b->start, tail);
    if (tail < n)
        memcpy(out + tail, b->buf, n - tail);
    return n;
}
