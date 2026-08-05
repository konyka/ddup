/* test_buf_pool.c - tests for the tiered fixed-size buffer pool. */
#include "test.h"

#include "core/buf_pool.h"
#include <string.h>

static void test_pool_basic(void)
{
    buf_pool pool;
    size_t sz;
    char *p;

    DD_CHECK(buf_pool_init(&pool) == 0);

    p = (char *)buf_pool_get(&pool, 64 * 1024, &sz);
    DD_CHECK(p != NULL);
    DD_CHECK(sz >= 64 * 1024);
    memset(p, 'x', sz);
    DD_CHECK(p[0] == 'x');
    DD_CHECK(p[sz - 1] == 'x');

    buf_pool_put(&pool, p, sz);

    /* Borrowing again should return the same buffer (single-threaded). */
    {
        size_t sz2;
        char *p2 = (char *)buf_pool_get(&pool, 64 * 1024, &sz2);
        DD_CHECK(p2 == p);
        DD_CHECK(sz2 == sz);
        buf_pool_put(&pool, p2, sz2);
    }

    buf_pool_destroy(&pool);
}

static void test_pool_multiple_sizes(void)
{
    buf_pool pool;
    size_t sizes[] = {1024, 4 * 1024, 16 * 1024, 64 * 1024, 200 * 1024};
    char *ptrs[5];
    size_t actuals[5];
    size_t i;

    DD_CHECK(buf_pool_init(&pool) == 0);

    for (i = 0; i < 5; i++) {
        ptrs[i] = (char *)buf_pool_get(&pool, sizes[i], &actuals[i]);
        DD_CHECK(ptrs[i] != NULL);
        DD_CHECK(actuals[i] >= sizes[i]);
        ptrs[i][0] = (char)i;
    }
    for (i = 0; i < 5; i++) {
        DD_CHECK_EQ_INT((long long)i, (long long)(unsigned char)ptrs[i][0]);
        buf_pool_put(&pool, ptrs[i], actuals[i]);
    }

    buf_pool_destroy(&pool);
}

static void test_oversized_fallback(void)
{
    buf_pool pool;
    size_t sz;
    char *p;

    DD_CHECK(buf_pool_init(&pool) == 0);

    p = (char *)buf_pool_get(&pool, 10 * 1024 * 1024, &sz);
    DD_CHECK(p != NULL);
    DD_CHECK(sz >= 10 * 1024 * 1024);
    p[0] = 'y';
    DD_CHECK(p[0] == 'y');
    buf_pool_put(&pool, p, sz);

    buf_pool_destroy(&pool);
}

int main(void)
{
    DD_RUN(test_pool_basic);
    DD_RUN(test_pool_multiple_sizes);
    DD_RUN(test_oversized_fallback);
    return DD_TEST_SUMMARY();
}
