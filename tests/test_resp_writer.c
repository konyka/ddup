/* test_resp_writer.c - RESP writer tests, incl. parse/serialize roundtrip
 * over randomly generated values (written before the implementation). */
#include "test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/buf_pool.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"

static void expect(resp_buf *b, const char *s)
{
    DD_CHECK_MEM(s, strlen(s), b->data, b->len);
    b->len = 0;
}

static void test_scalars(void)
{
    resp_buf b;
    resp_buf_init(&b);

    resp_write_simple_string(&b, "OK", 2);
    expect(&b, "+OK\r\n");

    resp_write_error(&b, "ERR bad", 7);
    expect(&b, "-ERR bad\r\n");

    resp_write_integer(&b, 0);
    expect(&b, ":0\r\n");
    resp_write_integer(&b, -123);
    expect(&b, ":-123\r\n");
    resp_write_integer(&b, 9223372036854775807LL);
    expect(&b, ":9223372036854775807\r\n");
    resp_write_integer(&b, -9223372036854775807LL - 1);
    expect(&b, ":-9223372036854775808\r\n");

    resp_write_bulk(&b, "hello", 5);
    expect(&b, "$5\r\nhello\r\n");
    resp_write_bulk(&b, "", 0);
    expect(&b, "$0\r\n\r\n");
    resp_write_bulk(&b, NULL, 0); /* null bulk */
    expect(&b, "$-1\r\n");

    resp_write_array_header(&b, 3);
    expect(&b, "*3\r\n");

    resp_buf_free(&b);
}

static void test_integer_formatter_boundaries(void)
{
    char out[32];
    size_t n;

    n = resp_test_u64_to_str(out, 0ULL);
    DD_CHECK_EQ_INT(1, (long long)n);
    DD_CHECK_MEM("0", 1, out, n);
    n = resp_test_u64_to_str(out, 18446744073709551615ULL);
    DD_CHECK_EQ_INT(20, (long long)n);
    DD_CHECK_MEM("18446744073709551615", 20, out, n);
}

static void test_resp3_scalars(void)
{
    resp_buf b;
    resp_buf_init(&b);

    resp_write_null(&b);
    expect(&b, "_\r\n");
    resp_write_boolean(&b, 1);
    expect(&b, "#t\r\n");
    resp_write_boolean(&b, 0);
    expect(&b, "#f\r\n");
    resp_write_double(&b, 3.5);
    expect(&b, ",3.5\r\n");
    resp_write_double(&b, -2.25);
    expect(&b, ",-2.25\r\n");
    resp_write_map_header(&b, 2);
    expect(&b, "%2\r\n");
    resp_write_set_header(&b, 1);
    expect(&b, "~1\r\n");
    resp_write_push_header(&b, 4);
    expect(&b, ">4\r\n");
    resp_write_big_number(&b, "12345678901234567890", 20);
    expect(&b, "(12345678901234567890\r\n");

    resp_buf_free(&b);
}

/* --- roundtrip: random value -> bytes -> parse -> serialize -> same bytes --- */

static unsigned long long g_rng = 0x243F6A8885A308D3ULL;
static unsigned rng_next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (unsigned)(g_rng >> 32);
}

static void emit_random_value(resp_buf *b, int depth)
{
    unsigned kind = rng_next() % (depth > 3 ? 9u : 12u);
    switch (kind) {
    case 0: resp_write_simple_string(b, "OK", 2); break;
    case 1: resp_write_error(b, "ERR x", 5); break;
    case 2: resp_write_integer(b, (long long)(int)rng_next()); break;
    case 3: {
        char tmp[32];
        unsigned n = rng_next() % 16;
        for (unsigned i = 0; i < n; i++)
            tmp[i] = (char)('a' + rng_next() % 26);
        resp_write_bulk(b, tmp, n);
        break;
    }
    case 4: resp_write_bulk(b, NULL, 0); break;
    case 5: resp_write_null(b); break;
    case 6: resp_write_boolean(b, rng_next() & 1); break;
    case 7: resp_write_double(b, (double)(int)(rng_next() % 1000) / 2.0); break;
    case 8: resp_write_big_number(b, "98765432109876543210", 20); break;
    case 9: { /* array */
        unsigned n = rng_next() % 5;
        resp_write_array_header(b, n);
        for (unsigned i = 0; i < n; i++)
            emit_random_value(b, depth + 1);
        break;
    }
    case 10: { /* map */
        unsigned n = rng_next() % 4;
        resp_write_map_header(b, n);
        for (unsigned i = 0; i < n * 2; i++)
            emit_random_value(b, depth + 1);
        break;
    }
    default: { /* set */
        unsigned n = rng_next() % 4;
        resp_write_set_header(b, n);
        for (unsigned i = 0; i < n; i++)
            emit_random_value(b, depth + 1);
        break;
    }
    }
}

static void test_roundtrip_random(void)
{
    arena a;
    arena_init(&a, 4096);
    resp_buf b, out;
    resp_buf_init(&b);
    resp_buf_init(&out);

    for (int iter = 0; iter < 2000; iter++) {
        b.len = 0;
        emit_random_value(&b, 0);

        arena_reset(&a);
        resp_value v;
        ptrdiff_t n = resp_parse(b.data, b.len, &v, &a);
        DD_CHECK(n == (ptrdiff_t)b.len);

        out.len = 0;
        resp_write_value(&out, &v);
        if (out.len != b.len || memcmp(out.data, b.data, b.len) != 0) {
            dd_test_checks++;
            dd_test_failures++;
            fprintf(stderr, "FAIL roundtrip iter %d\n  in : %.*s\n  out: %.*s\n",
                    iter, (int)b.len, b.data, (int)out.len, out.data);
            break;
        }
        dd_test_checks++;
    }

    resp_buf_free(&b);
    resp_buf_free(&out);
    arena_destroy(&a);
}

static void test_roundtrip_nested_command(void)
{
    /* a realistic command pipeline roundtrip */
    const char *cmd = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$5\r\nv1 v2\r\n";
    arena a;
    arena_init(&a, 1024);
    resp_value v;
    ptrdiff_t n = resp_parse(cmd, strlen(cmd), &v, &a);
    DD_CHECK(n == (ptrdiff_t)strlen(cmd));

    resp_buf out;
    resp_buf_init(&out);
    resp_write_value(&out, &v);
    DD_CHECK_MEM(cmd, strlen(cmd), out.data, out.len);

    resp_buf_free(&out);
    arena_destroy(&a);
}

static void test_resp_buf_pool_reuse(void)
{
    buf_pool pool;
    resp_buf a, b;
    char *first;
    size_t first_cap;

    DD_CHECK(buf_pool_init(&pool) == 0);

    resp_buf_init(&a);
    a.pool = &pool;
    resp_write_simple_string(&a, "OK", 2);
    first = a.data;
    first_cap = a.cap;
    DD_CHECK(first != NULL);
    DD_CHECK(first_cap >= 4 * 1024);
    resp_buf_free(&a);

    resp_buf_init(&b);
    b.pool = &pool;
    resp_buf_reserve(&b, 1);
    DD_CHECK(b.data == first);
    DD_CHECK(b.cap == first_cap);
    resp_buf_free(&b);

    buf_pool_destroy(&pool);
}

static void test_resp_buf_pool_growth(void)
{
    buf_pool pool;
    resp_buf b;
    char *first;

    DD_CHECK(buf_pool_init(&pool) == 0);

    resp_buf_init(&b);
    b.pool = &pool;
    resp_buf_reserve(&b, 1024);
    first = b.data;
    DD_CHECK(first != NULL);
    DD_CHECK(b.cap >= 1024);

    /* Force a jump to the next pool tier; the old buffer is returned. */
    resp_buf_reserve(&b, 100 * 1024);
    DD_CHECK(b.data != first);
    DD_CHECK(b.cap >= 100 * 1024);

    resp_buf_free(&b);
    buf_pool_destroy(&pool);
}

static void test_resp_buf_reserve_overflow(void)
{
    resp_buf b;
    char byte = 'x';

    resp_buf_init(&b);
    b.data = &byte;
    b.len = SIZE_MAX;
    b.cap = SIZE_MAX;
    DD_CHECK_EQ_INT(-1, resp_buf_reserve(&b, 1));
    DD_CHECK(b.data == &byte);
    DD_CHECK(b.len == SIZE_MAX);
    DD_CHECK(b.cap == SIZE_MAX);
    b.data = NULL;
    b.len = 0;
    b.cap = 0;
}

static void test_writer_null_inputs_fail_closed(void)
{
    resp_buf b;

    resp_buf_init(NULL);
    resp_buf_free(NULL);
    DD_CHECK_EQ_INT(-1, resp_buf_reserve(NULL, 1));

    resp_buf_init(&b);
    resp_write_simple_string(NULL, "x", 1);
    resp_write_error(NULL, "x", 1);
    resp_write_integer(NULL, 1);
    resp_write_bulk(NULL, "x", 1);
    resp_write_bulk(&b, NULL, 1);
    resp_write_big_number(&b, NULL, 1);
    resp_write_value(NULL, NULL);
    resp_buf_free(&b);
}

int main(void)
{
    DD_RUN(test_scalars);
    DD_RUN(test_integer_formatter_boundaries);
    DD_RUN(test_resp3_scalars);
    DD_RUN(test_roundtrip_random);
    DD_RUN(test_roundtrip_nested_command);
    DD_RUN(test_resp_buf_pool_reuse);
    DD_RUN(test_resp_buf_pool_growth);
    DD_RUN(test_resp_buf_reserve_overflow);
    DD_RUN(test_writer_null_inputs_fail_closed);
    return DD_TEST_SUMMARY();
}
