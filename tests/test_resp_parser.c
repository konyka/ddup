/* test_resp_parser.c - RESP2 parser tests (written before the implementation). */
#include "test.h"

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "core/arena.h"
#include "resp/resp_parser.h"

static arena g_a;
static resp_value g_v;

/* Parse helper: feeds the whole buffer at once. */
static ptrdiff_t parse(const char *s)
{
    arena_reset(&g_a);
    return resp_parse(s, strlen(s), &g_v, &g_a);
}

static void test_simple_string(void)
{
    DD_CHECK(parse("+OK\r\n") == 5);
    DD_CHECK(g_v.type == RESP_SIMPLE_STRING);
    DD_CHECK_MEM("OK", 2, g_v.str, g_v.len);
}

static void test_error(void)
{
    DD_CHECK(parse("-ERR unknown command\r\n") == 22);
    DD_CHECK(g_v.type == RESP_ERROR);
    DD_CHECK_MEM("ERR unknown command", 19, g_v.str, g_v.len);
}

static void test_integer(void)
{
    DD_CHECK(parse(":0\r\n") == 4);
    DD_CHECK(g_v.type == RESP_INTEGER);
    DD_CHECK_EQ_INT(0, g_v.integer);

    DD_CHECK(parse(":9223372036854775807\r\n") == 22);
    DD_CHECK_EQ_INT(9223372036854775807LL, g_v.integer);

    DD_CHECK(parse(":-42\r\n") == 6);
    DD_CHECK_EQ_INT(-42, g_v.integer);

    DD_CHECK(parse(":00042\r\n") == 8);
    DD_CHECK_EQ_INT(42, g_v.integer);
    DD_CHECK(parse(":-00042\r\n") == 9);
    DD_CHECK_EQ_INT(-42, g_v.integer);
}

static void test_integer_overflow_is_error(void)
{
    DD_CHECK(parse(":9223372036854775808\r\n") == -1);
    DD_CHECK(parse(":-9223372036854775809\r\n") == -1);
}

static void test_integer_fast_parser_contract(void)
{
    long long out = 0;
    const char *p = "00000000000000000042";
    DD_CHECK(resp_test_parse_integer(p, p + 20, &out) == 0);
    DD_CHECK_EQ_INT(42, out);
    p = "-9223372036854775808";
    DD_CHECK(resp_test_parse_integer(p, p + 20, &out) == 0);
    DD_CHECK_EQ_INT(LLONG_MIN, out);

    p = "9223372036854775807";
    DD_CHECK(resp_test_parse_integer(p, p + 19, &out) == 0);
    DD_CHECK_EQ_INT(LLONG_MAX, out);
    p = "9223372036854775808";
    DD_CHECK(resp_test_parse_integer(p, p + 19, &out) == -1);
    p = "-9223372036854775809";
    DD_CHECK(resp_test_parse_integer(p, p + 20, &out) == -1);
    p = "-0000000000000000000";
    DD_CHECK(resp_test_parse_integer(p, p + 20, &out) == 0);
    DD_CHECK_EQ_INT(0, out);
}

static void test_integer_property_samples(void)
{
    char wire[64];
    int i;

    for (i = -128; i <= 128; i++) {
        int n = snprintf(wire, sizeof(wire), ":%d\r\n", i * 7919);
        DD_CHECK(n > 0 && (size_t)n < sizeof(wire));
        DD_CHECK(parse(wire) == n);
        DD_CHECK_EQ_INT((long long)i * 7919, g_v.integer);
    }

    {
        static const long long edges[] = {
            LLONG_MIN, LLONG_MIN + 1, -1000000000000000000LL,
            -1, 0, 1, 1000000000000000000LL, LLONG_MAX - 1, LLONG_MAX};
        size_t j;
        for (j = 0; j < sizeof(edges) / sizeof(edges[0]); j++) {
            int n = snprintf(wire, sizeof(wire), ":%lld\r\n", edges[j]);
            DD_CHECK(n > 0 && (size_t)n < sizeof(wire));
            DD_CHECK(parse(wire) == n);
            DD_CHECK_EQ_INT(edges[j], g_v.integer);
        }
    }
}

static void test_bulk_length_fast_parser_contract(void)
{
    long long out = 123;
    const char *p = "0";
    DD_CHECK(resp_test_bulk_len(p, p + 1, &out) == 0);
    DD_CHECK_EQ_INT(0, out);

    p = "1048576";
    DD_CHECK(resp_test_bulk_len(p, p + 7, &out) == 0);
    DD_CHECK_EQ_INT(1048576, out);

    p = "-1";
    DD_CHECK(resp_test_bulk_len(p, p + 2, &out) == 0);
    DD_CHECK_EQ_INT(-1, out);

    p = "-2";
    DD_CHECK(resp_test_bulk_len(p, p + 2, &out) == -1);
    p = "12x";
    DD_CHECK(resp_test_bulk_len(p, p + 3, &out) == -1);
    p = "1073741825"; /* RESP_MAX_ARRAY_LEN + 1 */
    DD_CHECK(resp_test_bulk_len(p, p + 10, &out) == -1);
    p = "99999999999"; /* reject overlong lengths before scanning all digits */
    DD_CHECK(resp_test_bulk_len(p, p + 11, &out) == -1);

    /* Leading zeroes are accepted by the historical signed parser and by
     * Redis clients; the fast path must not turn them into a regression. */
    const char *leading_zero = "$000000000001\r\nx\r\n";
    DD_CHECK(parse(leading_zero) == (ptrdiff_t)strlen(leading_zero));
    DD_CHECK_EQ_INT(1, g_v.len);
    DD_CHECK(g_v.str[0] == 'x');

    p = "000000000000";
    DD_CHECK(resp_test_bulk_len(p, p + 12, &out) == 0);
    DD_CHECK_EQ_INT(0, out);
}

static void test_bulk_string(void)
{
    DD_CHECK(parse("$5\r\nhello\r\n") == 11);
    DD_CHECK(g_v.type == RESP_BULK_STRING);
    DD_CHECK_MEM("hello", 5, g_v.str, g_v.len);

    DD_CHECK(parse("$0\r\n\r\n") == 6);
    DD_CHECK(g_v.type == RESP_BULK_STRING);
    DD_CHECK_EQ_INT(0, g_v.len);
    DD_CHECK(g_v.str != NULL);
}

static void test_null_bulk_string(void)
{
    DD_CHECK(parse("$-1\r\n") == 5);
    DD_CHECK(g_v.type == RESP_BULK_STRING);
    DD_CHECK(g_v.str == NULL);
}

static void test_array(void)
{
    const char *cmd = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
    DD_CHECK(parse(cmd) == (ptrdiff_t)strlen(cmd));
    DD_CHECK(g_v.type == RESP_ARRAY);
    DD_CHECK_EQ_INT(3, g_v.count);
    DD_CHECK(g_v.items[0].type == RESP_BULK_STRING);
    DD_CHECK_MEM("SET", 3, g_v.items[0].str, g_v.items[0].len);
    DD_CHECK_MEM("foo", 3, g_v.items[1].str, g_v.items[1].len);
    DD_CHECK_MEM("bar", 3, g_v.items[2].str, g_v.items[2].len);
}

static void test_aggregate_allocation_size_overflow(void)
{
    size_t bytes = 123;
    DD_CHECK(resp_test_aggregate_bytes(SIZE_MAX, &bytes) == -1);
    DD_CHECK_EQ_INT(123, (long long)bytes);
    DD_CHECK(resp_test_aggregate_bytes(2, &bytes) == 0);
    DD_CHECK(bytes == 2 * sizeof(resp_value));
}

static void test_nested_array(void)
{
    const char *cmd = "*2\r\n*2\r\n:1\r\n:2\r\n$3\r\nend\r\n";
    DD_CHECK(parse(cmd) == (ptrdiff_t)strlen(cmd));
    DD_CHECK(g_v.type == RESP_ARRAY);
    DD_CHECK_EQ_INT(2, g_v.count);
    DD_CHECK(g_v.items[0].type == RESP_ARRAY);
    DD_CHECK_EQ_INT(2, g_v.items[0].count);
    DD_CHECK_EQ_INT(1, g_v.items[0].items[0].integer);
    DD_CHECK_EQ_INT(2, g_v.items[0].items[1].integer);
    DD_CHECK_MEM("end", 3, g_v.items[1].str, g_v.items[1].len);
}

static void test_null_and_empty_array(void)
{
    DD_CHECK(parse("*-1\r\n") == 5);
    DD_CHECK(g_v.type == RESP_ARRAY);
    DD_CHECK(g_v.items == NULL);
    DD_CHECK_EQ_INT(0, g_v.count);

    DD_CHECK(parse("*0\r\n") == 4);
    DD_CHECK(g_v.type == RESP_ARRAY);
    DD_CHECK_EQ_INT(0, g_v.count);
}

static void test_incomplete_returns_zero(void)
{
    DD_CHECK(parse("") == 0);
    DD_CHECK(parse("+OK") == 0);          /* missing CRLF */
    DD_CHECK(parse("$5\r\nhel") == 0);    /* short payload */
    DD_CHECK(parse("*3\r\n$3\r\nSET\r\n") == 0); /* missing elements */
    DD_CHECK(parse(":") == 0);
}

static void test_protocol_errors(void)
{
    DD_CHECK(parse("?bad\r\n") == -1);      /* unknown type byte */
    DD_CHECK(parse("$abc\r\n") == -1);      /* bad bulk length */
    DD_CHECK(parse("$3\r\nab") == 0);       /* incomplete, not error */
    DD_CHECK(parse("$3\r\nabcxx") == -1);   /* payload not CRLF-terminated */
    DD_CHECK(parse(":12x\r\n") == -1);      /* garbage integer */
    DD_CHECK(parse("*999999999999999999999\r\n") == -1); /* array len overflow */
}

static void test_pipelined_consumes_one_value(void)
{
    const char *pipe = "+PONG\r\n+PONG\r\n";
    arena_reset(&g_a);
    ptrdiff_t n1 = resp_parse(pipe, strlen(pipe), &g_v, &g_a);
    DD_CHECK(n1 == 7);
    DD_CHECK_MEM("PONG", 4, g_v.str, g_v.len);
    ptrdiff_t n2 = resp_parse(pipe + n1, strlen(pipe) - (size_t)n1, &g_v, &g_a);
    DD_CHECK(n2 == 7);
}

static void test_streaming_byte_by_byte(void)
{
    /* Feed a full command one byte at a time; only the final byte may
     * complete the parse. */
    const char *cmd = "*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n";
    size_t total = strlen(cmd);
    arena_reset(&g_a);
    ptrdiff_t n = 0;
    for (size_t i = 1; i <= total; i++) {
        n = resp_parse(cmd, i, &g_v, &g_a);
        if (i < total)
            DD_CHECK(n == 0);
    }
    DD_CHECK(n == (ptrdiff_t)total);
    DD_CHECK_EQ_INT(2, g_v.count);
    DD_CHECK_MEM("ECHO", 4, g_v.items[0].str, g_v.items[0].len);
}

static void test_bulk_payload_with_binary(void)
{
    /* Bulk strings may contain \r\n and NUL bytes. */
    const char raw[] = "$4\r\n\r\n\0x\r\n";
    arena_reset(&g_a);
    ptrdiff_t n = resp_parse(raw, sizeof(raw) - 1, &g_v, &g_a);
    DD_CHECK(n == (ptrdiff_t)(sizeof(raw) - 1));
    DD_CHECK_EQ_INT(4, g_v.len);
    DD_CHECK(g_v.str[0] == '\r' && g_v.str[1] == '\n' && g_v.str[2] == '\0');
}

int main(void)
{
    arena_init(&g_a, 1024);
    DD_RUN(test_simple_string);
    DD_RUN(test_error);
    DD_RUN(test_integer);
    DD_RUN(test_integer_overflow_is_error);
    DD_RUN(test_integer_fast_parser_contract);
    DD_RUN(test_integer_property_samples);
    DD_RUN(test_bulk_length_fast_parser_contract);
    DD_RUN(test_bulk_string);
    DD_RUN(test_null_bulk_string);
    DD_RUN(test_array);
    DD_RUN(test_aggregate_allocation_size_overflow);
    DD_RUN(test_nested_array);
    DD_RUN(test_null_and_empty_array);
    DD_RUN(test_incomplete_returns_zero);
    DD_RUN(test_protocol_errors);
    DD_RUN(test_pipelined_consumes_one_value);
    DD_RUN(test_streaming_byte_by_byte);
    DD_RUN(test_bulk_payload_with_binary);
    arena_destroy(&g_a);
    return DD_TEST_SUMMARY();
}
