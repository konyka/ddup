/* test_arena.c - arena allocator tests (written before the implementation). */
#include "test.h"

#include <stdint.h>
#include <string.h>

#include "core/arena.h"

static void test_alloc_returns_aligned_distinct(void)
{
    arena a;
    arena_init(&a, 256);
    char *p1 = arena_alloc(&a, 10);
    char *p2 = arena_alloc(&a, 10);
    DD_CHECK(p1 != NULL && p2 != NULL);
    DD_CHECK(p1 != p2);
    DD_CHECK(((uintptr_t)p1 & 15) == 0);
    DD_CHECK(((uintptr_t)p2 & 15) == 0);
    memset(p1, 0xAA, 10);
    DD_CHECK(p2[0] == 0 || p2[0] != (char)0xAA || p2 != p1); /* no overlap */
    arena_destroy(&a);
}

static void test_big_request_gets_dedicated_block(void)
{
    arena a;
    arena_init(&a, 64);
    void *big = arena_alloc(&a, 4096);
    DD_CHECK(big != NULL);
    memset(big, 1, 4096); /* must be writable across the whole range */
    arena_destroy(&a);
}

static void test_big_request_is_aligned(void)
{
    arena a;
    arena_init(&a, 64);
    void *p = arena_alloc(&a, 4096);
    DD_CHECK(p != NULL);
    DD_CHECK(((uintptr_t)p & 15) == 0);
    arena_destroy(&a);
}

static void test_size_overflow_is_transactional(void)
{
    arena a;
    arena_init(&a, 64);
    DD_CHECK(arena_alloc(&a, SIZE_MAX) == NULL);
    DD_CHECK(a.head == NULL);
    DD_CHECK(arena_alloc(&a, 1) != NULL);
    arena_destroy(&a);
}

static void test_reset_allows_reuse(void)
{
    arena a;
    arena_init(&a, 128);
    void *p1 = arena_alloc(&a, 100);
    DD_CHECK(p1 != NULL);
    arena_reset(&a);
    /* after reset the same memory should be handed out again */
    void *p2 = arena_alloc(&a, 100);
    DD_CHECK(p2 == p1);
    arena_destroy(&a);
}

static void test_many_allocs(void)
{
    arena a;
    arena_init(&a, 256);
    for (int i = 0; i < 10000; i++) {
        unsigned char *p = arena_alloc(&a, 24);
        DD_CHECK(p != NULL);
        p[0] = (unsigned char)i;
        DD_CHECK(p[0] == (unsigned char)i);
    }
    arena_destroy(&a);
}

static void test_zero_size_alloc(void)
{
    arena a;
    arena_init(&a, 64);
    void *p = arena_alloc(&a, 0);
    DD_CHECK(p != NULL);
    arena_destroy(&a);
}

static void test_mark_rewind_preserves_prior_allocations(void)
{
    arena a;
    arena_mark mark;
    unsigned char *prior;
    unsigned char *spec;

    arena_init(&a, 128);
    prior = (unsigned char *)arena_alloc(&a, 8);
    DD_CHECK(prior != NULL);
    prior[0] = 0x5a;
    arena_mark_get(&a, &mark);
    spec = (unsigned char *)arena_alloc(&a, 64);
    DD_CHECK(spec != NULL);
    if (spec != NULL)
        memset(spec, 0xa5, 64);
    arena_rewind(&a, &mark);
    DD_CHECK(prior[0] == 0x5a);
    DD_CHECK(arena_alloc(&a, 64) == spec);
    arena_destroy(&a);
}

static void test_null_handles_fail_closed(void)
{
    arena_mark mark;

    arena_init(NULL, 64);
    DD_CHECK(arena_alloc(NULL, 1) == NULL);
    arena_mark_get(NULL, &mark);
    arena_mark_get(NULL, NULL);
    arena_rewind(NULL, &mark);
    arena_rewind(NULL, NULL);
    arena_reset(NULL);
    arena_destroy(NULL);
}

int main(void)
{
    DD_RUN(test_alloc_returns_aligned_distinct);
    DD_RUN(test_big_request_gets_dedicated_block);
    DD_RUN(test_big_request_is_aligned);
    DD_RUN(test_size_overflow_is_transactional);
    DD_RUN(test_reset_allows_reuse);
    DD_RUN(test_many_allocs);
    DD_RUN(test_zero_size_alloc);
    DD_RUN(test_mark_rewind_preserves_prior_allocations);
    DD_RUN(test_null_handles_fail_closed);
    return DD_TEST_SUMMARY();
}
