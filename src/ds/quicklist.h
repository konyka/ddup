/* quicklist.h - doubly-linked list of listpack nodes (Phase 6).
 *
 * Each node packs up to DDUP_QL_FILL elements into one listpack; pushes to a
 * full end node allocate a new node (only end nodes ever grow), removals
 * unlink a node once it runs empty. Sparse middle nodes are NOT merged back
 * (documented simplification vs Redis, which relies on compression passes).
 *
 * Iterators are stable across reads; any mutating call (ql_set, ql_remove,
 * ql_push, ql_pop) may realloc a node's listpack, so the iterator must be
 * treated as repositioned by that call (ql_set/ql_remove keep it valid,
 * ql_push/ql_pop invalidate it).
 */
#ifndef DDUP_QUICKLIST_H
#define DDUP_QUICKLIST_H

#include <stddef.h>
#include <stdint.h>

/* Maximum entries packed into one node before a push splits (default;
 * runtime-tunable via quicklist_set_fill). */
#define DDUP_QL_FILL 128u

/* Override the per-node fill limit. Values < 1 are ignored. Applied once
 * at server startup (single write, read-only afterwards). */
void quicklist_set_fill(int fill);

typedef struct ql_node {
    struct ql_node *prev;
    struct ql_node *next;
    unsigned char *lp; /* listpack payload */
    uint32_t count;    /* cached entry count */
    uint64_t bytes;    /* lp total bytes */
} ql_node;

typedef struct quicklist {
    ql_node *head;
    ql_node *tail;
    uint64_t len; /* element count */
    uint64_t mem; /* sizeof(quicklist) + per-node cost, incremental */
} quicklist;

typedef struct ql_iter {
    quicklist *ql;
    ql_node *node;
    unsigned char *entry; /* current entry inside node->lp; NULL = invalid */
    unsigned char buf[24]; /* materialized decimal for int entries */
} ql_iter;

/* ql_new/ql_free manage a heap quicklist; ql_init/ql_release serve an
 * embedded quicklist (ql_release frees the nodes, not the struct). */
void ql_init(quicklist *ql);
void ql_release(quicklist *ql);
quicklist *ql_new(void);
void ql_free(quicklist *ql);
uint64_t ql_mem(const quicklist *ql);

/* Push at the head (left != 0) or tail. Returns 0 on success, -1 when len
 * cannot be represented (len > UINT32_MAX). */
int ql_push(quicklist *ql, int left, const char *data, size_t len);
/* Returns 1 and hands the caller a malloc'd copy of the element (free with
 * free()), 0 when the list is empty. */
int ql_pop(quicklist *ql, int left, char **data, size_t *len);

/* Position an iterator; 1 on success, 0 when idx >= len / list empty. */
int ql_seek(quicklist *ql, uint64_t idx, ql_iter *it);
int ql_first(quicklist *ql, ql_iter *it);
int ql_last(quicklist *ql, ql_iter *it);
/* Advance the iterator; 1 on success, 0 when it falls off the end (the
 * iterator is then invalid). */
int ql_iter_next(ql_iter *it);
int ql_iter_prev(ql_iter *it);
/* Current element bytes. The returned pointer is valid until the iterator
 * moves or the list is mutated (int entries point into it->buf). */
const char *ql_iter_value(ql_iter *it, size_t *len);

/* Replace the current element. Returns 1 on success, -1 on invalid length.
 * The iterator stays on the replaced element. */
int ql_set(ql_iter *it, const char *data, size_t len);
/* Remove the current element; the iterator lands on its successor, or
 * becomes invalid when the tail element was removed. */
void ql_remove(ql_iter *it);

#endif /* DDUP_QUICKLIST_H */
