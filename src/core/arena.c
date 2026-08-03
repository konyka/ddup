/* arena.c - bump allocator; see arena.h. */
#include "core/arena.h"

#include <stdint.h>
#include <stdlib.h>

#define ARENA_ALIGN 16

static size_t align_up(size_t n)
{
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

void arena_init(arena *a, size_t block_size)
{
    a->head = NULL;
    a->block_size = block_size ? block_size : 4096;
}

static arena_block *arena_new_block(arena *a, size_t min_cap)
{
    size_t cap = a->block_size > min_cap ? a->block_size : min_cap;
    arena_block *b = malloc(sizeof(arena_block) + cap);
    if (!b)
        return NULL;
    b->used = 0;
    b->cap = cap;
    b->next = a->head;
    a->head = b;
    return b;
}

void *arena_alloc(arena *a, size_t n)
{
    n = align_up(n);
    arena_block *b = a->head;
    if (!b || n > b->cap - b->used) {
        b = arena_new_block(a, n);
        if (!b)
            return NULL;
    }
    void *p = (char *)b + sizeof(arena_block) + b->used;
    b->used += n;
    /* arena_block is malloc-aligned (max_align); keep the invariant that
     * data starts 16-byte aligned even on platforms where it is not. */
    if ((uintptr_t)p & (ARENA_ALIGN - 1)) {
        size_t fix = ARENA_ALIGN - ((uintptr_t)p & (ARENA_ALIGN - 1));
        if (fix <= b->cap - b->used) {
            p = (char *)p + fix;
            b->used += fix;
        }
    }
    return p;
}

void arena_reset(arena *a)
{
    for (arena_block *b = a->head; b; b = b->next)
        b->used = 0;
}

void arena_destroy(arena *a)
{
    arena_block *b = a->head;
    while (b) {
        arena_block *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}
