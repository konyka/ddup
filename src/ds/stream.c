/* stream.c - compact ordered stream object; see stream.h. */
#include "ds/stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *stream_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    return p;
}

static int stream_entry_bytes(size_t nfields, size_t data_len, uint64_t *out)
{
    uint64_t total = sizeof(stream_entry);
    if (nfields > UINT32_MAX ||
        (uint64_t)nfields > (UINT64_MAX - total) / (2 * sizeof(uint32_t)))
        return -1;
    total += (uint64_t)nfields * 2 * sizeof(uint32_t);
    if (data_len > UINT64_MAX - total)
        return -1;
    total += data_len;
    if (total > UINT64_MAX - 32)
        return -1;
    *out = total + 32;
    return 0;
}

static void stream_entry_free(stream_entry *e)
{
    if (e == NULL)
        return;
    free(e->lens);
    free(e->data);
}

obj_stream *obj_stream_new(void)
{
    obj_stream *s = (obj_stream *)stream_xmalloc(sizeof(*s));
    s->entries = NULL;
    s->len = 0;
    s->cap = 0;
    s->last_ms = 0;
    s->last_seq = 0;
    s->entries_added = 0;
    s->max_deleted_ms = 0;
    s->max_deleted_seq = 0;
    s->mem = (uint64_t)sizeof(*s);
    return s;
}

void obj_stream_free(obj_stream *s)
{
    size_t i;
    if (s == NULL)
        return;
    for (i = 0; i < s->len; i++)
        stream_entry_free(&s->entries[i]);
    free(s->entries);
    free(s);
}

uint64_t obj_stream_mem(const obj_stream *s)
{
    return s->mem;
}

size_t obj_stream_len(const obj_stream *s)
{
    return s->len;
}

const stream_entry *obj_stream_at(const obj_stream *s, size_t idx)
{
    return idx < s->len ? &s->entries[idx] : NULL;
}

size_t obj_stream_lower_bound(const obj_stream *s, uint64_t ms, uint64_t seq)
{
    size_t lo = 0, hi = s->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const stream_entry *e = &s->entries[mid];
        if (e->ms < ms || (e->ms == ms && e->seq < seq))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

int obj_stream_append(obj_stream *s, uint64_t ms, uint64_t seq,
                      const char *const *fields, const size_t *flens,
                      const char *const *values, const size_t *vlens,
                      size_t nfields)
{
    stream_entry *e;
    size_t i;
    size_t total = 0;
    uint64_t bytes;
    char *data;
    uint32_t *lens;
    uint32_t *lp;

    if (ms < s->last_ms || (ms == s->last_ms && seq <= s->last_seq))
        return OBJ_STREAM_ADD_SMALL;
    if (nfields > UINT32_MAX)
        return OBJ_STREAM_ADD_ERR;
    for (i = 0; i < nfields; i++) {
        if (flens[i] > UINT32_MAX || vlens[i] > UINT32_MAX ||
            flens[i] > SIZE_MAX - total)
            return OBJ_STREAM_ADD_ERR;
        total += flens[i];
        if (vlens[i] > SIZE_MAX - total)
            return OBJ_STREAM_ADD_ERR;
        total += vlens[i];
    }
    if (stream_entry_bytes(nfields, total, &bytes) != 0)
        return OBJ_STREAM_ADD_ERR;

    if (s->len == s->cap) {
        size_t ncap = s->cap == 0 ? 8 : s->cap * 2;
        stream_entry *ne;
        if (ncap < s->cap || ncap > SIZE_MAX / sizeof(*ne))
            return OBJ_STREAM_ADD_ERR;
        ne = (stream_entry *)stream_xmalloc(ncap * sizeof(*ne));
        if (s->len > 0)
            memcpy(ne, s->entries, s->len * sizeof(*ne));
        free(s->entries);
        s->entries = ne;
        s->cap = ncap;
    }

    data = (char *)stream_xmalloc(total);
    if ((size_t)nfields > SIZE_MAX / (2 * sizeof(uint32_t)))
        return OBJ_STREAM_ADD_ERR;
    lens = (uint32_t *)stream_xmalloc(nfields * 2 * sizeof(uint32_t));
    lp = lens;
    total = 0;
    for (i = 0; i < nfields; i++) {
        *lp++ = (uint32_t)flens[i];
        *lp++ = (uint32_t)vlens[i];
        if (flens[i] != 0)
            memcpy(data + total, fields[i], flens[i]);
        total += flens[i];
        if (vlens[i] != 0)
            memcpy(data + total, values[i], vlens[i]);
        total += vlens[i];
    }

    e = &s->entries[s->len++];
    e->ms = ms;
    e->seq = seq;
    e->nfields = (uint32_t)nfields;
    e->lens = lens;
    e->data = data;
    e->data_len = total;
    s->last_ms = ms;
    s->last_seq = seq;
    s->entries_added++;
    if (s->mem > UINT64_MAX - bytes)
        abort();
    s->mem += bytes;
    return OBJ_STREAM_ADD_OK;
}

static int stream_id_cmp(uint64_t a_ms, uint64_t a_seq, uint64_t b_ms,
                         uint64_t b_seq)
{
    if (a_ms < b_ms)
        return -1;
    if (a_ms > b_ms)
        return 1;
    if (a_seq < b_seq)
        return -1;
    if (a_seq > b_seq)
        return 1;
    return 0;
}

static void stream_update_deleted(obj_stream *s, const stream_entry *e)
{
    if (stream_id_cmp(e->ms, e->seq, s->max_deleted_ms,
                      s->max_deleted_seq) > 0) {
        s->max_deleted_ms = e->ms;
        s->max_deleted_seq = e->seq;
    }
}

int obj_stream_delete(obj_stream *s, uint64_t ms, uint64_t seq)
{
    size_t idx = obj_stream_lower_bound(s, ms, seq);
    uint64_t bytes;
    stream_entry *e;
    if (idx >= s->len)
        return 0;
    e = &s->entries[idx];
    if (e->ms != ms || e->seq != seq)
        return 0;
    if (stream_entry_bytes(e->nfields, e->data_len, &bytes) != 0)
        abort();
    stream_update_deleted(s, e);
    stream_entry_free(e);
    if (idx + 1 < s->len)
        memmove(&s->entries[idx], &s->entries[idx + 1],
                (s->len - idx - 1) * sizeof(*s->entries));
    s->len--;
    s->mem -= bytes;
    return 1;
}

static void stream_remove_front(obj_stream *s, size_t n)
{
    uint64_t bytes;
    size_t i;
    if (n == 0)
        return;
    stream_update_deleted(s, &s->entries[n - 1]);
    for (i = 0; i < n; i++) {
        if (stream_entry_bytes(s->entries[i].nfields, s->entries[i].data_len,
                               &bytes) != 0)
            abort();
        s->mem -= bytes;
        stream_entry_free(&s->entries[i]);
    }
    if (n < s->len)
        memmove(s->entries, s->entries + n,
                (s->len - n) * sizeof(*s->entries));
    s->len -= n;
}

size_t obj_stream_trim_maxlen(obj_stream *s, uint64_t maxlen, uint64_t limit)
{
    size_t n;
    if (maxlen >= (uint64_t)s->len)
        return 0;
    n = s->len - (size_t)maxlen;
    if (limit < n)
        n = (size_t)limit;
    stream_remove_front(s, n);
    return n;
}

size_t obj_stream_trim_minid(obj_stream *s, uint64_t ms, uint64_t seq,
                             uint64_t limit)
{
    size_t n = obj_stream_lower_bound(s, ms, seq);
    if (limit < n)
        n = (size_t)limit;
    stream_remove_front(s, n);
    return n;
}
