/* quicklist.c - doubly-linked list of listpack nodes. See quicklist.h. */
#include "ds/quicklist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/listpack.h"

/* Per-node fill limit; see quicklist.h. */
static uint32_t g_ql_fill = DDUP_QL_FILL;

void quicklist_set_fill(int fill)
{
    if (fill >= 1)
        g_ql_fill = (uint32_t)fill;
}

static void ql_die_oom(void)
{
    fprintf(stderr, "ddup: out of memory\n");
    exit(1);
}

/* malloc-overhead convention shared with the old per-node list: 16 bytes */
#define QL_NODE_OVERHEAD (sizeof(ql_node) + 16)

static ql_node *ql_node_new(quicklist *ql)
{
    ql_node *n = (ql_node *)malloc(sizeof(*n));
    if (n == NULL)
        ql_die_oom();
    n->prev = NULL;
    n->next = NULL;
    n->lp = lp_new();
    n->count = 0;
    n->bytes = (uint64_t)lp_bytes(n->lp);
    ql->mem += QL_NODE_OVERHEAD + n->bytes;
    return n;
}

static void ql_node_free(quicklist *ql, ql_node *n)
{
    ql->mem -= QL_NODE_OVERHEAD + n->bytes;
    lp_free(n->lp);
    free(n);
}

static void ql_node_link(quicklist *ql, ql_node *n, int left)
{
    if (left) {
        n->next = ql->head;
        if (ql->head != NULL)
            ql->head->prev = n;
        else
            ql->tail = n;
        ql->head = n;
    } else {
        n->prev = ql->tail;
        if (ql->tail != NULL)
            ql->tail->next = n;
        else
            ql->head = n;
        ql->tail = n;
    }
}

static void ql_node_unlink(quicklist *ql, ql_node *n)
{
    if (n->prev != NULL)
        n->prev->next = n->next;
    else
        ql->head = n->next;
    if (n->next != NULL)
        n->next->prev = n->prev;
    else
        ql->tail = n->prev;
}

/* account a listpack size change on n (node->bytes already updated) */
static void ql_node_mem_sync(quicklist *ql, uint64_t old_bytes,
                             const ql_node *n)
{
    ql->mem -= old_bytes;
    ql->mem += n->bytes;
}

/* 0-based index of entry inside n's listpack (linear scan; nodes are
 * small, and this runs only on the sparse-merge path). */
static long ql_entry_index(const ql_node *n, const unsigned char *entry)
{
    long idx = 0;
    unsigned char *p = lp_first(n->lp);
    while (p != NULL && p != entry) {
        idx++;
        p = lp_next(n->lp, p);
    }
    return idx;
}

/* Append src's entries in order to dst's tail, then unlink+free src. */
static void ql_node_merge_into(quicklist *ql, ql_node *dst, ql_node *src)
{
    unsigned char *p = lp_first(src->lp);
    uint64_t old_bytes = dst->bytes;
    while (p != NULL) {
        unsigned char buf[24];
        uint32_t vl = 0;
        const unsigned char *v = lp_get_str(p, buf, &vl);
        dst->lp = lp_append(dst->lp, v, vl);
        p = lp_next(src->lp, p);
    }
    dst->count += src->count;
    dst->bytes = (uint64_t)lp_bytes(dst->lp);
    ql_node_mem_sync(ql, old_bytes, dst);
    ql_node_unlink(ql, src);
    ql_node_free(ql, src);
}

/* Sparse-node merge after a removal: when n (non-empty) dropped below
 * fill/4 entries, fold a neighbour into it (preferring n->next) or fold
 * n into n->prev, as long as the combined size stays within 2*fill.
 * fill < 4 makes the threshold 0, so merging never triggers. Returns 0
 * when nothing merged, 1 when n->next was folded into n, 2 when n was
 * folded into n->prev (n freed; callers must have saved what they need).
 * Pushes never call this: only removals create sparse nodes. */
static int ql_maybe_merge(quicklist *ql, ql_node *n)
{
    if (n->count >= g_ql_fill / 4)
        return 0; /* includes fill < 4: threshold 0, count >= 1 */
    if (n->next != NULL && n->count + n->next->count <= 2 * g_ql_fill) {
        ql_node_merge_into(ql, n, n->next);
        return 1;
    }
    if (n->prev != NULL && n->count + n->prev->count <= 2 * g_ql_fill) {
        ql_node_merge_into(ql, n->prev, n); /* n's entries to prev's tail */
        return 2;
    }
    return 0;
}

void ql_init(quicklist *ql)
{
    ql->head = NULL;
    ql->tail = NULL;
    ql->len = 0;
    ql->mem = (uint64_t)sizeof(*ql);
}

quicklist *ql_new(void)
{
    quicklist *ql = (quicklist *)malloc(sizeof(*ql));
    if (ql == NULL)
        ql_die_oom();
    ql_init(ql);
    return ql;
}

void ql_release(quicklist *ql)
{
    ql_node *n;
    if (ql == NULL)
        return;
    n = ql->head;
    while (n != NULL) {
        ql_node *next = n->next;
        lp_free(n->lp);
        free(n);
        n = next;
    }
    ql->head = NULL;
    ql->tail = NULL;
    ql->len = 0;
    ql->mem = (uint64_t)sizeof(*ql);
}

void ql_free(quicklist *ql)
{
    if (ql == NULL)
        return;
    ql_release(ql);
    free(ql);
}

uint64_t ql_mem(const quicklist *ql)
{
    return ql == NULL ? 0 : ql->mem;
}

int ql_push(quicklist *ql, int left, const char *data, size_t len)
{
    ql_node *n;
    uint64_t old_bytes;
    if (ql == NULL || (data == NULL && len != 0) || len > UINT32_MAX)
        return -1;
    n = left ? ql->head : ql->tail;
    if (n == NULL || n->count >= g_ql_fill) {
        /* only end nodes grow; a full end node splits off a fresh one */
        n = ql_node_new(ql);
        ql_node_link(ql, n, left);
    }
    old_bytes = n->bytes;
    n->lp = left ? lp_prepend(n->lp, (const unsigned char *)data, (uint32_t)len)
                 : lp_append(n->lp, (const unsigned char *)data, (uint32_t)len);
    n->count++;
    n->bytes = (uint64_t)lp_bytes(n->lp);
    ql_node_mem_sync(ql, old_bytes, n);
    ql->len++;
    return 0;
}

int ql_pop(quicklist *ql, int left, char **data, size_t *len)
{
    ql_node *n;
    unsigned char *entry;
    unsigned char buf[24];
    const unsigned char *v;
    uint32_t vlen = 0;
    uint64_t old_bytes;
    if (ql == NULL || data == NULL || len == NULL) {
        if (len != NULL)
            *len = 0;
        return 0;
    }
    n = left ? ql->head : ql->tail;
    if (n == NULL)
        return 0;
    entry = left ? lp_first(n->lp) : lp_last(n->lp);
    v = lp_get_str(entry, buf, &vlen);
    *data = (char *)malloc(vlen ? vlen : 1);
    if (*data == NULL)
        ql_die_oom();
    if (vlen != 0)
        memcpy(*data, v, vlen);
    *len = vlen;
    old_bytes = n->bytes;
    n->lp = lp_delete(n->lp, entry, NULL);
    n->count--;
    if (n->count == 0) {
        ql_node_unlink(ql, n);
        ql_node_free(ql, n);
    } else {
        n->bytes = (uint64_t)lp_bytes(n->lp);
        ql_node_mem_sync(ql, old_bytes, n);
        (void)ql_maybe_merge(ql, n); /* sparse end node folds inward */
    }
    ql->len--;
    return 1;
}

int ql_seek(quicklist *ql, uint64_t idx, ql_iter *it)
{
    ql_node *n;
    if (ql == NULL || it == NULL || idx >= ql->len)
        return 0;
    it->ql = ql;
    if (idx < ql->len / 2) {
        n = ql->head;
        while (n != NULL && idx >= n->count) {
            idx -= n->count;
            n = n->next;
        }
        it->node = n;
        it->entry = lp_seek(n->lp, (long)idx);
    } else {
        uint64_t from_end = ql->len - 1 - idx;
        n = ql->tail;
        while (n != NULL && from_end >= n->count) {
            from_end -= n->count;
            n = n->prev;
        }
        it->node = n;
        it->entry = lp_seek(n->lp, (long)(n->count - 1 - from_end));
    }
    return it->entry != NULL;
}

int ql_first(quicklist *ql, ql_iter *it)
{
    if (ql == NULL || it == NULL)
        return 0;
    it->ql = ql;
    it->node = ql->head;
    if (it->node == NULL) {
        it->entry = NULL;
        return 0;
    }
    it->entry = lp_first(it->node->lp);
    return 1;
}

int ql_last(quicklist *ql, ql_iter *it)
{
    if (ql == NULL || it == NULL)
        return 0;
    it->ql = ql;
    it->node = ql->tail;
    if (it->node == NULL) {
        it->entry = NULL;
        return 0;
    }
    it->entry = lp_last(it->node->lp);
    return 1;
}

int ql_iter_next(ql_iter *it)
{
    unsigned char *next;
    if (it == NULL || it->entry == NULL)
        return 0;
    next = lp_next(it->node->lp, it->entry);
    if (next != NULL) {
        it->entry = next;
        return 1;
    }
    it->node = it->node->next;
    if (it->node == NULL) {
        it->entry = NULL;
        return 0;
    }
    it->entry = lp_first(it->node->lp);
    return 1;
}

int ql_iter_prev(ql_iter *it)
{
    unsigned char *prev;
    if (it == NULL || it->entry == NULL)
        return 0;
    prev = lp_prev(it->node->lp, it->entry);
    if (prev != NULL) {
        it->entry = prev;
        return 1;
    }
    it->node = it->node->prev;
    if (it->node == NULL) {
        it->entry = NULL;
        return 0;
    }
    it->entry = lp_last(it->node->lp);
    return 1;
}

const char *ql_iter_value(ql_iter *it, size_t *len)
{
    uint32_t vlen = 0;
    const unsigned char *v;
    if (it == NULL || len == NULL || it->entry == NULL) {
        if (len != NULL)
            *len = 0;
        return NULL;
    }
    v = lp_get_str(it->entry, it->buf, &vlen);
    *len = vlen;
    return (const char *)v;
}

int ql_set(ql_iter *it, const char *data, size_t len)
{
    ql_node *n;
    size_t off;
    uint64_t old_bytes;
    if (it == NULL || it->entry == NULL || (data == NULL && len != 0) ||
        len > UINT32_MAX)
        return -1;
    n = it->node;
    off = (size_t)(it->entry - n->lp);
    old_bytes = n->bytes;
    n->lp = lp_replace(n->lp, it->entry, (const unsigned char *)data,
                       (uint32_t)len);
    it->entry = n->lp + off;
    n->bytes = (uint64_t)lp_bytes(n->lp);
    ql_node_mem_sync(it->ql, old_bytes, n);
    return 1;
}

int ql_insert(ql_iter *it, int after, const char *data, size_t len)
{
    ql_node *n;
    uint64_t old_bytes;
    unsigned char *newp = NULL;
    if (it == NULL || it->entry == NULL || (data == NULL && len != 0) ||
        len > UINT32_MAX)
        return -1;
    n = it->node;
    old_bytes = n->bytes;
    n->lp = lp_insert(n->lp, (const unsigned char *)data, (uint32_t)len,
                      it->entry, after ? LP_AFTER : LP_BEFORE, &newp);
    n->count++;
    n->bytes = (uint64_t)lp_bytes(n->lp);
    ql_node_mem_sync(it->ql, old_bytes, n);
    it->entry = newp;
    it->ql->len++;
    return 1;
}

void ql_remove(ql_iter *it)
{
    if (it == NULL)
        return;
    quicklist *ql = it->ql;
    ql_node *n = it->node;
    ql_node *next_node = n->next;
    unsigned char *np = NULL;
    uint64_t old_bytes = n->bytes;
    if (it->entry == NULL)
        return;
    n->lp = lp_delete(n->lp, it->entry, &np);
    n->count--;
    ql->len--;
    if (np != NULL) {
        /* successor stays inside this node */
        n->bytes = (uint64_t)lp_bytes(n->lp);
        ql_node_mem_sync(ql, old_bytes, n);
        it->entry = np;
        if (n->count < g_ql_fill / 4) {
            /* a merge reallocs n (next folded in) or frees it (n folded
             * into prev): reposition the iterator by index so it keeps
             * pointing at np's element */
            ql_node *prev = n->prev;
            uint32_t prev_cnt = prev != NULL ? prev->count : 0;
            long idx = ql_entry_index(n, np);
            if (ql_maybe_merge(ql, n) == 2) {
                it->node = prev;
                it->entry = lp_seek(prev->lp, (long)prev_cnt + idx);
            } else {
                it->entry = lp_seek(n->lp, idx);
            }
        }
        return;
    }
    /* removed the node's tail entry (or its last one) */
    if (n->count == 0) {
        ql_node_unlink(ql, n);
        ql_node_free(ql, n);
        it->node = next_node;
        it->entry = next_node != NULL ? lp_first(next_node->lp) : NULL;
        return;
    }
    n->bytes = (uint64_t)lp_bytes(n->lp);
    ql_node_mem_sync(ql, old_bytes, n);
    it->node = next_node;
    it->entry = next_node != NULL ? lp_first(next_node->lp) : NULL;
    if (n->count < g_ql_fill / 4) {
        /* if n swallowed next_node, the successor moved into n at the
         * index n had right after the removal */
        uint32_t ncnt = n->count;
        if (ql_maybe_merge(ql, n) == 1) {
            it->node = n;
            it->entry = lp_seek(n->lp, (long)ncnt);
        }
    }
}
