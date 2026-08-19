/* test_sort.c - SORT / SORT_RO command tests (written before the impl). */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

#define T0 1000000ULL

static void exec_cmd(db *d, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[10];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *s = va_arg(ap, const char *);
        memset(&argv[i], 0, sizeof(argv[i]));
        argv[i].type = RESP_BULK_STRING;
        argv[i].str = s;
        argv[i].len = strlen(s);
    }
    va_end(ap);
    out->len = 0;
    command_execute_at(d, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

static const char WRONGTYPE_REPLY[] =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
static const char NOT_INT_REPLY[] =
    "-ERR value is not an integer or out of range\r\n";
static const char SYNTAX_REPLY[] = "-ERR syntax error\r\n";

static void test_sort_list(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "l", "3", "1", "2");
    EXPECT(out, ":3\r\n");

    exec_cmd(&d, T0, &out, 2, "SORT", "l");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 3, "SORT", "l", "DESC");
    EXPECT(out, "*3\r\n$1\r\n3\r\n$1\r\n2\r\n$1\r\n1\r\n");
    exec_cmd(&d, T0, &out, 5, "SORT", "l", "LIMIT", "0", "2");
    EXPECT(out, "*2\r\n$1\r\n1\r\n$1\r\n2\r\n");
    exec_cmd(&d, T0, &out, 5, "SORT", "l", "LIMIT", "1", "1");
    EXPECT(out, "*1\r\n$1\r\n2\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_sort_by_get(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "l", "3", "1", "2");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "w_3", "30");
    exec_cmd(&d, T0, &out, 3, "SET", "w_1", "10");
    exec_cmd(&d, T0, &out, 3, "SET", "w_2", "20");
    exec_cmd(&d, T0, &out, 3, "SET", "o_3", "three");
    exec_cmd(&d, T0, &out, 3, "SET", "o_1", "one");
    exec_cmd(&d, T0, &out, 3, "SET", "o_2", "two");

    exec_cmd(&d, T0, &out, 4, "SORT", "l", "BY", "w_*");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 4, "SORT", "l", "GET", "o_*");
    EXPECT(out,
           "*3\r\n$3\r\none\r\n$3\r\ntwo\r\n$5\r\nthree\r\n");
    exec_cmd(&d, T0, &out, 4, "SORT", "l", "GET", "#");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 6, "SORT", "l", "BY", "w_*", "GET", "o_*");
    EXPECT(out,
           "*3\r\n$3\r\none\r\n$3\r\ntwo\r\n$5\r\nthree\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_sort_store(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "l", "3", "1", "2");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 4, "SORT", "l", "STORE", "dst");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "dst", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");

    exec_cmd(&d, T0, &out, 4, "SORT", "missing", "STORE", "dst2");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "dst2");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_sort_types(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "SADD", "s", "3", "1", "2");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 2, "SORT", "s");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");

    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "10", "3");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "20", "1");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "30", "2");
    exec_cmd(&d, T0, &out, 2, "SORT", "z");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");

    exec_cmd(&d, T0, &out, 4, "RPUSH", "alpha", "b", "a");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "SORT", "alpha", "ALPHA");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_sort_ro_and_errors(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 4, "RPUSH", "l", "3", "1");
    EXPECT(out, ":2\r\n");

    exec_cmd(&d, T0, &out, 2, "SORT_RO", "l");
    EXPECT(out, "*2\r\n$1\r\n1\r\n$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 4, "SORT_RO", "l", "STORE", "dst");
    EXPECT(out, SYNTAX_REPLY);

    exec_cmd(&d, T0, &out, 2, "SORT", "missing");
    EXPECT(out, "*0\r\n");

    exec_cmd(&d, T0, &out, 3, "SET", "str", "x");
    exec_cmd(&d, T0, &out, 2, "SORT", "str");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 5, "SORT", "l", "LIMIT", "bad", "1");
    EXPECT(out, NOT_INT_REPLY);
    exec_cmd(&d, T0, &out, 4, "SORT", "l", "LIMIT", "1");
    EXPECT(out, SYNTAX_REPLY);
    exec_cmd(&d, T0, &out, 3, "SORT", "l", "BY");
    EXPECT(out, SYNTAX_REPLY);

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_sort_list);
    DD_RUN(test_sort_by_get);
    DD_RUN(test_sort_store);
    DD_RUN(test_sort_types);
    DD_RUN(test_sort_ro_and_errors);
    return DD_TEST_SUMMARY();
}
