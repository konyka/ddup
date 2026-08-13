/* quicklist.c - doubly-linked list of listpack nodes. See quicklist.h. */
#include "ds/quicklist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/listpack.h"

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

quicklist *ql_new(void)
{
    quicklist *ql = (quicklist *)malloc(sizeof(*ql));
    if (ql == NULL)
        ql_die_oom();
    ql->head = NULL;
    ql->tail = NULL;
    ql->len = 0;
    ql->mem = (uint64_t)sizeof(*ql);
    return ql;
}

void ql_free(quicklist *ql)
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
    free(ql);
}

uint64_t ql_mem(const quicklist *ql)
{
    return ql->mem;
}

int ql_push(quicklist *ql, int left, const char *data, size_t len)
{
    ql_node *n;
    uint64_t old_bytes;
    if (len > UINT32_MAX)
        return -1;
    n = left ? ql->head : ql->tail;
    if (n == NULL || n->count >= DDUP_QL_FILL) {
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
    ql_node *n = left ? ql->head : ql->tail;
    unsigned char *entry;
    unsigned char buf[24];
    const unsigned char *v;
    uint32_t vlen = 0;
    uint64_t old_bytes;
    if (n == NULL)
        return 0;
    entry = left ? lp_first(n->lp) : lp_last(n->lp);
    v = lp_get_str(entry, buf, &vlen);
    *data = (char *)malloc(vlen ? vlen : 1);
    if (*data == NULL)
        ql_die_oom();
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
    }
    ql->len--;
    return 1;
}

int ql_seek(quicklist *ql, uint64_t idx, ql_iter *it)
{
    ql_node *n;
    if (idx >= ql->len)
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
    if (it->entry == NULL)
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
    if (it->entry == NULL)
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
    if (it->entry == NULL) {
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
    if (it->entry == NULL || len > UINT32_MAX)
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

void ql_remove(ql_iter *it)
{
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
        return;
    }
    /* removed the node's tail entry (or its last one) */
    if (n->count == 0) {
        ql_node_unlink(ql, n);
        ql_node_free(ql, n);
    } else {
        n->bytes = (uint64_t)lp_bytes(n->lp);
        ql_node_mem_sync(ql, old_bytes, n);
    }
    it->node = next_node;
    it->entry = next_node != NULL ? lp_first(next_node->lp) : NULL;
}
