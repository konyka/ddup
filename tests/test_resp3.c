/* test_resp3.c - RESP3-specific type parsing tests. */
#include "test.h"

#include <math.h>
#include <string.h>

#include "core/arena.h"
#include "resp/resp_parser.h"

static arena g_a;
static resp_value g_v;

static ptrdiff_t parse(const char *s)
{
    arena_reset(&g_a);
    return resp_parse(s, strlen(s), &g_v, &g_a);
}

static void test_null(void)
{
    DD_CHECK(parse("_\r\n") == 3);
    DD_CHECK(g_v.type == RESP_NULL);
    DD_CHECK(parse("_x\r\n") == -1); /* payload must be empty */
}

static void test_boolean(void)
{
    DD_CHECK(parse("#t\r\n") == 4);
    DD_CHECK(g_v.type == RESP_BOOLEAN);
    DD_CHECK_EQ_INT(1, g_v.integer);

    DD_CHECK(parse("#f\r\n") == 4);
    DD_CHECK_EQ_INT(0, g_v.integer);

    DD_CHECK(parse("#x\r\n") == -1);
    DD_CHECK(parse("#tt\r\n") == -1);
}

static void test_double(void)
{
    DD_CHECK(parse(",3.14\r\n") == 7);
    DD_CHECK(g_v.type == RESP_DOUBLE);
    DD_CHECK(g_v.dbl > 3.13 && g_v.dbl < 3.15);

    DD_CHECK(parse(",inf\r\n") == 6);
    DD_CHECK(isinf(g_v.dbl) && g_v.dbl > 0);
    DD_CHECK(parse(",-inf\r\n") == 7);
    DD_CHECK(isinf(g_v.dbl) && g_v.dbl < 0);
    DD_CHECK(parse(",nan\r\n") == 6);
    DD_CHECK(isnan(g_v.dbl));

    DD_CHECK(parse(",\r\n") == -1);     /* empty */
    DD_CHECK(parse(",12x\r\n") == -1);  /* trailing garbage */
}

static void test_big_number(void)
{
    const char *s = "(3492890328409238509324850943850943825024385\r\n";
    DD_CHECK(parse(s) == (ptrdiff_t)strlen(s));
    DD_CHECK(g_v.type == RESP_BIG_NUMBER);
    DD_CHECK_MEM("3492890328409238509324850943850943825024385", 43,
                 g_v.str, g_v.len);
}

static void test_blob_error(void)
{
    const char *s = "!21\r\nSYNTAX invalid syntax\r\n";
    DD_CHECK(parse(s) == (ptrdiff_t)strlen(s));
    DD_CHECK(g_v.type == RESP_BLOB_ERROR);
    DD_CHECK_MEM("SYNTAX invalid syntax", 21, g_v.str, g_v.len);
}

static void test_verbatim_string(void)
{
    const char *s = "=15\r\ntxt:Some string\r\n";
    DD_CHECK(parse(s) == (ptrdiff_t)strlen(s));
    DD_CHECK(g_v.type == RESP_VERBATIM_STRING);
    DD_CHECK_MEM("txt:Some string", 15, g_v.str, g_v.len);
}

static void test_map(void)
{
    const char *s = "%2\r\n+first\r\n:1\r\n+second\r\n:2\r\n";
    DD_CHECK(parse(s) == (ptrdiff_t)strlen(s));
    DD_CHECK(g_v.type == RESP_MAP);
    DD_CHECK_EQ_INT(4, g_v.count); /* 2 pairs = 4 items */
    DD_CHECK_MEM("first", 5, g_v.items[0].str, g_v.items[0].len);
    DD_CHECK_EQ_INT(1, g_v.items[1].integer);
    DD_CHECK_MEM("second", 6, g_v.items[2].str, g_v.items[2].len);
    DD_CHECK_EQ_INT(2, g_v.items[3].integer);
}

static void test_set(void)
{
    const char *s = "~3\r\n+a\r\n+b\r\n+c\r\n";
    DD_CHECK(parse(s) == (ptrdiff_t)strlen(s));
    DD_CHECK(g_v.type == RESP_SET);
    DD_CHECK_EQ_INT(3, g_v.count);
}

static void test_push(void)
{
    const char *s = ">3\r\n+message\r\n$7\r\nchannel\r\n$5\r\nhello\r\n";
    DD_CHECK(parse(s) == (ptrdiff_t)strlen(s));
    DD_CHECK(g_v.type == RESP_PUSH);
    DD_CHECK_EQ_INT(3, g_v.count);
    DD_CHECK_MEM("channel", 7, g_v.items[1].str, g_v.items[1].len);
}

static void test_resp3_nested_in_array(void)
{
    const char *s = "*2\r\n%1\r\n+k\r\n#t\r\n_\r\n";
    DD_CHECK(parse(s) == (ptrdiff_t)strlen(s));
    DD_CHECK(g_v.type == RESP_ARRAY);
    DD_CHECK(g_v.items[0].type == RESP_MAP);
    DD_CHECK(g_v.items[0].items[1].type == RESP_BOOLEAN);
    DD_CHECK(g_v.items[1].type == RESP_NULL);
}

int main(void)
{
    arena_init(&g_a, 1024);
    DD_RUN(test_null);
    DD_RUN(test_boolean);
    DD_RUN(test_double);
    DD_RUN(test_big_number);
    DD_RUN(test_blob_error);
    DD_RUN(test_verbatim_string);
    DD_RUN(test_map);
    DD_RUN(test_set);
    DD_RUN(test_push);
    DD_RUN(test_resp3_nested_in_array);
    arena_destroy(&g_a);
    return DD_TEST_SUMMARY();
}
