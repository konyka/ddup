/* buf_pool.c - tiered fixed-size buffer pool. */
#include "core/buf_pool.h"

#include <stdlib.h>
#include <string.h>

#include "pal/pal_cstd.h"

int buf_pool_init(buf_pool *pool)
{
    ddup_static_assert(BUF_POOL_TIERS == 4, "buf_pool tiers mismatch");
    if (pool == NULL)
        return -1;
    pool->sizes[0] = 4 * 1024;
    pool->sizes[1] = 16 * 1024;
    pool->sizes[2] = 64 * 1024;
    pool->sizes[3] = 256 * 1024;
    memset(pool->lists, 0, sizeof(pool->lists));
    pool->allocs = 0;
    pool->hits = 0;
    return 0;
}

static int buf_pool_tier(buf_pool *pool, size_t size)
{
    int i;
    for (i = 0; i < BUF_POOL_TIERS; i++) {
        if (size <= pool->sizes[i])
            return i;
    }
    return -1;
}

void *buf_pool_get(buf_pool *pool, size_t size, size_t *actual_size)
{
    if (pool == NULL || actual_size == NULL)
        return NULL;
    int tier = buf_pool_tier(pool, size);
    if (tier >= 0) {
        buf_pool_free_node *n = pool->lists[tier];
        if (n) {
            pool->lists[tier] = n->next;
            pool->hits++;
            *actual_size = pool->sizes[tier];
            return n;
        }
        n = (buf_pool_free_node *)malloc(pool->sizes[tier]);
        if (!n)
            return NULL;
        pool->allocs++;
        *actual_size = pool->sizes[tier];
        return n;
    }
    /* Oversized: fall back to exact malloc. */
    {
        void *p = malloc(size);
        if (p)
            pool->allocs++;
        *actual_size = size;
        return p;
    }
}

void buf_pool_put(buf_pool *pool, void *ptr, size_t actual_size)
{
    if (pool == NULL || ptr == NULL)
        return;
    int tier = buf_pool_tier(pool, actual_size);
    if (tier >= 0 && actual_size == pool->sizes[tier]) {
        buf_pool_free_node *n = (buf_pool_free_node *)ptr;
        n->next = pool->lists[tier];
        pool->lists[tier] = n;
    } else {
        free(ptr);
    }
}

void buf_pool_destroy(buf_pool *pool)
{
    int i;
    if (pool == NULL)
        return;
    for (i = 0; i < BUF_POOL_TIERS; i++) {
        buf_pool_free_node *n = pool->lists[i];
        while (n) {
            buf_pool_free_node *next = n->next;
            free(n);
            n = next;
        }
        pool->lists[i] = NULL;
    }
    pool->allocs = 0;
    pool->hits = 0;
    memset(pool->sizes, 0, sizeof(pool->sizes));
}
