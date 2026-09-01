/* arena.c - bump allocator; see arena.h. */
#include "core/arena.h"

#include <stdint.h>
#include <stdlib.h>

#define ARENA_ALIGN 16

static size_t align_up(size_t n)
{
    if (n > SIZE_MAX - (ARENA_ALIGN - 1))
        return 0;
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
    if (cap > SIZE_MAX - (ARENA_ALIGN - 1) ||
        sizeof(arena_block) > SIZE_MAX - cap - (ARENA_ALIGN - 1))
        return NULL;
    cap += ARENA_ALIGN - 1;
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
    size_t aligned = align_up(n);
    if (aligned == 0 && n != 0)
        return NULL;
    n = aligned;
    arena_block *b = a->head;
    size_t padding = 0;
    if (b) {
        uintptr_t raw = (uintptr_t)((char *)b + sizeof(arena_block) + b->used);
        padding = (size_t)((ARENA_ALIGN - (raw & (ARENA_ALIGN - 1))) &
                           (ARENA_ALIGN - 1));
    }
    if (!b || padding > SIZE_MAX - n || padding + n > b->cap - b->used) {
        if (padding > SIZE_MAX - n)
            return NULL;
        b = arena_new_block(a, padding + n);
        if (!b)
            return NULL;
        padding = (size_t)((ARENA_ALIGN -
                            ((uintptr_t)((char *)b + sizeof(arena_block)) &
                             (ARENA_ALIGN - 1))) &
                           (ARENA_ALIGN - 1));
    }
    void *p = (char *)b + sizeof(arena_block) + b->used + padding;
    b->used += padding + n;
    return p;
}

void arena_mark_get(const arena *a, arena_mark *mark)
{
    mark->head = a->head;
    mark->used = a->head != NULL ? a->head->used : 0;
}

void arena_rewind(arena *a, const arena_mark *mark)
{
    arena_block *b = a->head;
    while (b != NULL && b != mark->head) {
        b->used = 0;
        b = b->next;
    }
    if (mark->head != NULL)
        mark->head->used = mark->used;
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
