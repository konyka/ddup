/* test_hll.c - HyperLogLog command tests, written before the implementation. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

#define T0 1000000ULL

static void exec_cmd(db *d, resp_buf *out, int argc, ...)
{
    resp_value argv[16];
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
    command_execute_at(d, argv, (size_t)argc, out, T0);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

static void test_pfadd_pfcount(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, &out, 5, "PFADD", "hll", "a", "b", "c");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 5, "PFADD", "hll", "a", "b", "c");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 3, "PFADD", "hll", "d");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 2, "PFCOUNT", "hll");
    EXPECT(out, ":4\r\n");

    exec_cmd(&d, &out, 2, "PFCOUNT", "missing");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, &out, 2, "PFADD", "hll");
    EXPECT(out, "-ERR wrong number of arguments for 'pfadd' command\r\n");

    exec_cmd(&d, &out, 4, "HSET", "hash", "f", "v");
    exec_cmd(&d, &out, 3, "PFADD", "hash", "x");
    EXPECT(out, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_pfcount_multi_pfmerge(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, &out, 5, "PFADD", "h1", "a", "b", "c");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 5, "PFADD", "h2", "c", "d", "e");
    EXPECT(out, ":1\r\n");

    exec_cmd(&d, &out, 3, "PFCOUNT", "h1", "h2");
    EXPECT(out, ":5\r\n");

    exec_cmd(&d, &out, 4, "PFMERGE", "merged", "h1", "h2");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, &out, 2, "PFCOUNT", "merged");
    EXPECT(out, ":5\r\n");

    exec_cmd(&d, &out, 4, "PFMERGE", "merged2", "missing", "h1");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, &out, 2, "PFCOUNT", "merged2");
    EXPECT(out, ":3\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_pfdebug_pfselftest(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, &out, 3, "PFADD", "hll", "x");
    exec_cmd(&d, &out, 3, "PFDEBUG", "ENCODING", "hll");
    EXPECT(out, "$5\r\ndense\r\n");

    exec_cmd(&d, &out, 1, "PFSELFTEST");
    EXPECT(out, "+OK\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_pfadd_pfcount);
    DD_RUN(test_pfcount_multi_pfmerge);
    DD_RUN(test_pfdebug_pfselftest);
    return DD_TEST_SUMMARY();
}
