/* test_bitmap.c - bitmap command tests, written before the implementation. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

#define T0 1000000ULL

static void exec_cmd(db *d, resp_buf *out, int argc, ...)
{
    resp_value argv[8];
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

static void test_getbit_setbit(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, &out, 3, "GETBIT", "bits", "0");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 4, "SETBIT", "bits", "7", "1");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 3, "GETBIT", "bits", "7");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 4, "SETBIT", "bits", "7", "0");
    EXPECT(out, ":1\r\n");

    /* An offset beyond the end grows with zero-filled intermediate bytes. */
    exec_cmd(&d, &out, 4, "SETBIT", "bits", "16", "1");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 3, "GETBIT", "bits", "8");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 2, "STRLEN", "bits");
    EXPECT(out, ":3\r\n");

    exec_cmd(&d, &out, 4, "SETBIT", "bits", "-1", "1");
    EXPECT(out, "-ERR bit offset is not an integer or out of range\r\n");
    exec_cmd(&d, &out, 4, "SETBIT", "bits", "0", "2");
    EXPECT(out, "-ERR bit is not an integer or out of range\r\n");

    exec_cmd(&d, &out, 4, "HSET", "hash", "f", "v");
    exec_cmd(&d, &out, 3, "GETBIT", "hash", "0");
    EXPECT(out, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_bitcount_bitpos(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* `a` = 01100001, chosen to keep the test argv text-safe. */
    exec_cmd(&d, &out, 3, "SET", "bits", "a");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, &out, 2, "BITCOUNT", "bits");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, &out, 4, "BITCOUNT", "bits", "0", "0");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, &out, 4, "BITCOUNT", "bits", "-1", "-1");
    EXPECT(out, ":3\r\n");

    exec_cmd(&d, &out, 3, "BITPOS", "bits", "1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 3, "BITPOS", "bits", "0");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, &out, 5, "BITPOS", "bits", "1", "0", "0");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, &out, 3, "BITPOS", "missing", "1");
    EXPECT(out, ":-1\r\n");
    exec_cmd(&d, &out, 3, "BITPOS", "missing", "0");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_getbit_setbit);
    DD_RUN(test_bitcount_bitpos);
    return DD_TEST_SUMMARY();
}
