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
    s->group_mem = 0;
    s->groups = NULL;
    s->ngroups = 0;
    s->groups_cap = 0;
    return s;
}

void obj_stream_free(obj_stream *s)
{
    size_t i;
    if (s == NULL)
        return;
    for (i = 0; i < s->len; i++)
        stream_entry_free(&s->entries[i]);
    while (s->ngroups > 0) {
        stream_group *g = &s->groups[s->ngroups - 1];
        size_t j;
        for (j = 0; j < g->nconsumers; j++) {
            stream_consumer *c = &g->consumers[j];
            free(c->name);
            free(c->pel);
        }
        free(g->consumers);
        free(g->name);
        s->ngroups--;
    }
    free(s->groups);
    free(s->entries);
    free(s);
}

uint64_t obj_stream_mem(const obj_stream *s)
{
    return s == NULL ? 0 : s->mem + s->group_mem;
}

size_t obj_stream_len(const obj_stream *s)
{
    return s == NULL ? 0 : s->len;
}

const stream_entry *obj_stream_at(const obj_stream *s, size_t idx)
{
    return s != NULL && idx < s->len ? &s->entries[idx] : NULL;
}

size_t obj_stream_lower_bound(const obj_stream *s, uint64_t ms, uint64_t seq)
{
    size_t lo = 0, hi;
    if (s == NULL)
        return 0;
    hi = s->len;
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

/* ------------------------------------------------------------------ */
/* consumer groups                                                    */
/* ------------------------------------------------------------------ */

static int stream_name_eq(const char *a, size_t alen, const char *b,
                          size_t blen)
{
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

static int stream_mem_add(uint64_t base, size_t struct_size, size_t name_len,
                          size_t extra, uint64_t *out)
{
    uint64_t total = base;
    if ((uint64_t)struct_size > UINT64_MAX - total)
        return -1;
    total += (uint64_t)struct_size;
    if (name_len > UINT64_MAX - total)
        return -1;
    total += (uint64_t)name_len;
    if (extra > UINT64_MAX - total)
        return -1;
    total += extra;
    if (total > UINT64_MAX - 16)
        return -1;
    *out = total + 16;
    return 0;
}

static int stream_group_mem_add(obj_stream *s, uint64_t bytes)
{
    if (bytes > UINT64_MAX - s->group_mem)
        return -1;
    s->group_mem += bytes;
    return 0;
}

static void stream_group_mem_sub(obj_stream *s, uint64_t bytes)
{
    if (s->group_mem < bytes)
        abort();
    s->group_mem -= bytes;
}

stream_group *obj_stream_group_get(obj_stream *s, const char *name,
                                   size_t name_len)
{
    size_t i;
    for (i = 0; i < s->ngroups; i++)
        if (stream_name_eq(s->groups[i].name, s->groups[i].name_len, name,
                           name_len))
            return &s->groups[i];
    return NULL;
}

stream_group *obj_stream_group_create(obj_stream *s, const char *name,
                                      size_t name_len, uint64_t last_ms,
                                      uint64_t last_seq, int *created)
{
    stream_group *g = obj_stream_group_get(s, name, name_len);
    uint64_t mem;
    if (g != NULL) {
        if (created != NULL)
            *created = 0;
        return g;
    }
    if (stream_mem_add(0, sizeof(*g), name_len, 0, &mem) != 0)
        return NULL;
    if (mem > UINT64_MAX - s->group_mem)
        return NULL;
    if (s->ngroups == s->groups_cap) {
        size_t ncap = s->groups_cap == 0 ? 4 : s->groups_cap * 2;
        stream_group *ng;
        if (ncap < s->groups_cap || ncap > SIZE_MAX / sizeof(*ng))
            return NULL;
        ng = (stream_group *)stream_xmalloc(ncap * sizeof(*ng));
        if (s->ngroups > 0)
            memcpy(ng, s->groups, s->ngroups * sizeof(*ng));
        free(s->groups);
        s->groups = ng;
        s->groups_cap = ncap;
    }
    g = &s->groups[s->ngroups++];
    memset(g, 0, sizeof(*g));
    g->stream = s;
    g->name = (char *)stream_xmalloc(name_len ? name_len : 1);
    if (name_len > 0)
        memcpy(g->name, name, name_len);
    g->name_len = name_len;
    g->last_ms = last_ms;
    g->last_seq = last_seq;
    g->entries_read = 0;
    g->consumers = NULL;
    g->nconsumers = 0;
    g->consumers_cap = 0;
    if (stream_group_mem_add(s, mem) != 0)
        abort();
    if (created != NULL)
        *created = 1;
    return g;
}

int obj_stream_group_destroy(obj_stream *s, const char *name,
                             size_t name_len)
{
    size_t i;
    for (i = 0; i < s->ngroups; i++) {
        stream_group *g = &s->groups[i];
        uint64_t mem = 0;
        size_t j;
        if (!stream_name_eq(g->name, g->name_len, name, name_len))
            continue;
        if (stream_mem_add(mem, sizeof(*g), g->name_len, 0, &mem) != 0)
            abort();
        for (j = 0; j < g->nconsumers; j++) {
            stream_consumer *c = &g->consumers[j];
            uint64_t cmem = 0;
            size_t k;
            if (stream_mem_add(cmem, sizeof(*c), c->name_len, 0, &cmem) != 0)
                abort();
            for (k = 0; k < c->pel_len; k++) {
                if (cmem > UINT64_MAX - (sizeof(stream_pending) + 16))
                    abort();
                cmem += sizeof(stream_pending) + 16;
            }
            mem += cmem;
            free(c->name);
            free(c->pel);
        }
        free(g->consumers);
        free(g->name);
        stream_group_mem_sub(s, mem);
        if (i + 1 < s->ngroups)
            memmove(&s->groups[i], &s->groups[i + 1],
                    (s->ngroups - i - 1) * sizeof(*s->groups));
        s->ngroups--;
        return 1;
    }
    return 0;
}

stream_consumer *obj_stream_consumer_get(stream_group *g, const char *name,
                                         size_t name_len)
{
    size_t i;
    for (i = 0; i < g->nconsumers; i++)
        if (stream_name_eq(g->consumers[i].name, g->consumers[i].name_len,
                           name, name_len))
            return &g->consumers[i];
    return NULL;
}

stream_consumer *obj_stream_consumer_create(stream_group *g,
                                            const char *name,
                                            size_t name_len)
{
    stream_consumer *c = obj_stream_consumer_get(g, name, name_len);
    uint64_t mem;
    if (c != NULL)
        return c;
    if (stream_mem_add(0, sizeof(*c), name_len, 0, &mem) != 0)
        return NULL;
    if (mem > UINT64_MAX - g->stream->group_mem)
        return NULL;
    if (g->nconsumers == g->consumers_cap) {
        size_t ncap = g->consumers_cap == 0 ? 4 : g->consumers_cap * 2;
        stream_consumer *nc;
        if (ncap < g->consumers_cap || ncap > SIZE_MAX / sizeof(*nc))
            return NULL;
        nc = (stream_consumer *)stream_xmalloc(ncap * sizeof(*nc));
        if (g->nconsumers > 0)
            memcpy(nc, g->consumers, g->nconsumers * sizeof(*nc));
        free(g->consumers);
        g->consumers = nc;
        g->consumers_cap = ncap;
    }
    c = &g->consumers[g->nconsumers++];
    memset(c, 0, sizeof(*c));
    c->name = (char *)stream_xmalloc(name_len ? name_len : 1);
    if (name_len > 0)
        memcpy(c->name, name, name_len);
    c->name_len = name_len;
    c->pel = NULL;
    c->pel_len = 0;
    c->pel_cap = 0;
    c->seen_time = 0;
    c->active_time = 0;
    if (stream_group_mem_add(g->stream, mem) != 0)
        abort();
    return c;
}

int obj_stream_consumer_destroy(stream_group *g, const char *name,
                                size_t name_len)
{
    size_t i;
    for (i = 0; i < g->nconsumers; i++) {
        stream_consumer *c = &g->consumers[i];
        uint64_t mem = 0;
        size_t k;
        if (!stream_name_eq(c->name, c->name_len, name, name_len))
            continue;
        if (stream_mem_add(mem, sizeof(*c), c->name_len, 0, &mem) != 0)
            abort();
        for (k = 0; k < c->pel_len; k++) {
            if (mem > UINT64_MAX - (sizeof(stream_pending) + 16))
                abort();
            mem += sizeof(stream_pending) + 16;
        }
        stream_group_mem_sub(g->stream, mem);
        free(c->name);
        free(c->pel);
        if (i + 1 < g->nconsumers)
            memmove(&g->consumers[i], &g->consumers[i + 1],
                    (g->nconsumers - i - 1) * sizeof(*g->consumers));
        g->nconsumers--;
        return 1;
    }
    return 0;
}

stream_pending *obj_stream_consumer_pel_add(stream_group *g,
                                            stream_consumer *c,
                                            uint64_t ms, uint64_t seq,
                                            uint64_t idle,
                                            uint64_t delivery_count)
{
    stream_pending *p = obj_stream_consumer_pel_find(c, ms, seq);
    if (p != NULL)
        return p;
    if (c->pel_len == c->pel_cap) {
        size_t ncap = c->pel_cap == 0 ? 8 : c->pel_cap * 2;
        stream_pending *np;
        if (ncap < c->pel_cap || ncap > SIZE_MAX / sizeof(*np))
            return NULL;
        np = (stream_pending *)stream_xmalloc(ncap * sizeof(*np));
        if (c->pel_len > 0)
            memcpy(np, c->pel, c->pel_len * sizeof(*np));
        free(c->pel);
        c->pel = np;
        c->pel_cap = ncap;
    }
    if (sizeof(*p) + 16 > UINT64_MAX - g->stream->group_mem)
        return NULL;
    if (stream_group_mem_add(g->stream, sizeof(*p) + 16) != 0)
        return NULL;
    p = &c->pel[c->pel_len++];
    p->ms = ms;
    p->seq = seq;
    p->idle = idle;
    p->delivery_count = delivery_count;
    (void)g;
    return p;
}

stream_pending *obj_stream_consumer_pel_find(stream_consumer *c,
                                             uint64_t ms, uint64_t seq)
{
    size_t i;
    for (i = 0; i < c->pel_len; i++)
        if (c->pel[i].ms == ms && c->pel[i].seq == seq)
            return &c->pel[i];
    return NULL;
}

int obj_stream_consumer_pel_remove(stream_consumer *c, uint64_t ms,
                                   uint64_t seq)
{
    size_t i;
    for (i = 0; i < c->pel_len; i++) {
        if (c->pel[i].ms == ms && c->pel[i].seq == seq) {
            /* caller provides the owning group through group_pel_remove;
             * direct consumer removal keeps mem via consumer_destroy. */
            if (i + 1 < c->pel_len)
                memmove(&c->pel[i], &c->pel[i + 1],
                        (c->pel_len - i - 1) * sizeof(*c->pel));
            c->pel_len--;
            return 1;
        }
    }
    return 0;
}

int obj_stream_group_pel_remove(stream_group *g, uint64_t ms, uint64_t seq)
{
    size_t i;
    for (i = 0; i < g->nconsumers; i++) {
        if (obj_stream_consumer_pel_remove(&g->consumers[i], ms, seq)) {
            stream_group_mem_sub(g->stream,
                                 sizeof(stream_pending) + (uint64_t)16);
            return 1;
        }
    }
    return 0;
}

uint64_t obj_stream_group_pending_count(const stream_group *g)
{
    uint64_t n = 0;
    size_t i;
    for (i = 0; i < g->nconsumers; i++)
        n += (uint64_t)g->consumers[i].pel_len;
    return n;
}
