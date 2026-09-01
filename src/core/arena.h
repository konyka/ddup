/* arena.h - bump allocator for per-connection / per-request allocations.
 *
 * Blocks are kept on arena_reset() so the steady state performs zero malloc
 * calls. All returned pointers are 16-byte aligned. Not thread-safe; one
 * arena belongs to one IO thread.
 */
#ifndef DDUP_ARENA_H
#define DDUP_ARENA_H

#include <stddef.h>

typedef struct arena_block {
    struct arena_block *next;
    size_t used;
    size_t cap;
    /* char data[] follows */
} arena_block;

typedef struct arena {
    arena_block *head;
    size_t block_size; /* minimum allocation unit for new blocks */
} arena;

typedef struct arena_mark {
    arena_block *head;
    size_t used;
} arena_mark;

/* block_size is the minimum block size; larger requests get dedicated blocks. */
void arena_init(arena *a, size_t block_size);

/* Bump-allocate n bytes (16-byte aligned). Returns NULL only on OOM. */
void *arena_alloc(arena *a, size_t n);

/* Save/restore allocation state for speculative parsing. Blocks allocated
 * after a mark are retained for reuse, but their used ranges are discarded. */
void arena_mark_get(const arena *a, arena_mark *mark);
void arena_rewind(arena *a, const arena_mark *mark);

/* Mark all memory free again; blocks are retained for reuse. */
void arena_reset(arena *a);

/* Free all blocks. */
void arena_destroy(arena *a);

#endif /* DDUP_ARENA_H */
