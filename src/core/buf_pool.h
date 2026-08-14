/* buf_pool.h - tiered fixed-size buffer pool.
 *
 * Designed for the hot path: single-threaded borrow/return, no locks.
 * Falls back to malloc/free for sizes larger than the largest tier.
 */
#ifndef DDUP_BUF_POOL_H
#define DDUP_BUF_POOL_H

#include <stddef.h>

#define BUF_POOL_TIERS 4

typedef struct buf_pool_free_node {
    struct buf_pool_free_node *next;
} buf_pool_free_node;

struct buf_pool {
    size_t sizes[BUF_POOL_TIERS];
    buf_pool_free_node *lists[BUF_POOL_TIERS];
    size_t allocs;      /* fallback allocations (not from a free list) */
    size_t hits;        /* successful borrows from a free list */
};
/* Guarded: resp/resp_writer.h may have forward-typedef'd buf_pool already
 * (repeat typedefs are C11-only). */
#ifndef DDUP_BUF_POOL_TYPEDEF
#define DDUP_BUF_POOL_TYPEDEF
typedef struct buf_pool buf_pool;
#endif

int buf_pool_init(buf_pool *pool);
void buf_pool_destroy(buf_pool *pool);

/* Borrow a buffer of at least `size` bytes. `actual_size` receives the real
 * allocation size and must be passed unchanged to buf_pool_put(). */
void *buf_pool_get(buf_pool *pool, size_t size, size_t *actual_size);

/* Return a buffer previously obtained from buf_pool_get() with the same
 * `actual_size`. */
void buf_pool_put(buf_pool *pool, void *ptr, size_t actual_size);

#endif /* DDUP_BUF_POOL_H */
