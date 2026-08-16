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

static void test_bitop(void)
{
    db d;
    resp_buf out;
    static const char zeros[] = {0, 0, 0};
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, &out, 3, "SET", "key1", "foobar");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, &out, 3, "SET", "key2", "abcdef");
    EXPECT(out, "+OK\r\n");

    exec_cmd(&d, &out, 5, "BITOP", "AND", "dest", "key1", "key2");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, &out, 2, "GET", "dest");
    EXPECT(out, "$6\r\n`bc`ab\r\n");

    exec_cmd(&d, &out, 5, "BITOP", "OR", "dest", "key1", "key2");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, &out, 2, "GET", "dest");
    EXPECT(out, "$6\r\ngoofev\r\n");

    exec_cmd(&d, &out, 5, "BITOP", "XOR", "dest", "key1", "key2");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, &out, 2, "GET", "dest");
    DD_CHECK_MEM("\a\r\f\006\004\024", 6, out.data + 4, out.len - 6);

    exec_cmd(&d, &out, 4, "BITOP", "NOT", "notdest", "key1");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, &out, 2, "GET", "notdest");
    DD_CHECK_MEM("\x99\x90\x90\x9d\x9e\x8d", 6, out.data + 4, out.len - 6);

    /* missing sources are treated as zero-length strings */
    exec_cmd(&d, &out, 3, "SET", "abc", "abc");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, &out, 4, "BITOP", "OR", "odest", "abc", "missing");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, &out, 2, "GET", "odest");
    EXPECT(out, "$3\r\nabc\r\n");
    exec_cmd(&d, &out, 5, "BITOP", "AND", "adest", "abc", "missing");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, &out, 2, "GET", "adest");
    DD_CHECK_MEM(zeros, 3, out.data + 4, out.len - 6);

    /* destination may also be a source */
    exec_cmd(&d, &out, 5, "BITOP", "OR", "abc", "abc", "key2");
    EXPECT(out, ":6\r\n");

    exec_cmd(&d, &out, 5, "BITOP", "NOT", "dest", "key1", "key2");
    EXPECT(out, "-ERR BITOP NOT must be called with a single source key.\r\n");
    exec_cmd(&d, &out, 4, "BITOP", "NOPE", "dest", "key1");
    EXPECT(out, "-ERR syntax error\r\n");

    exec_cmd(&d, &out, 4, "HSET", "hash", "f", "v");
    exec_cmd(&d, &out, 4, "BITOP", "AND", "dest", "hash", "key1");
    EXPECT(out, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_bitfield(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, &out, 6, "BITFIELD", "bits", "SET", "u8", "0", "255");
    EXPECT(out, "*1\r\n:0\r\n");
    exec_cmd(&d, &out, 5, "BITFIELD", "bits", "GET", "u8", "0");
    EXPECT(out, "*1\r\n:255\r\n");
    exec_cmd(&d, &out, 6, "BITFIELD", "bits", "INCRBY", "u8", "0", "1");
    EXPECT(out, "*1\r\n:0\r\n");
    exec_cmd(&d, &out, 6, "BITFIELD", "bits", "SET", "u8", "0", "255");
    EXPECT(out, "*1\r\n:0\r\n");
    exec_cmd(&d, &out, 8, "BITFIELD", "bits", "OVERFLOW", "SAT", "INCRBY", "u8", "0", "10");
    EXPECT(out, "*1\r\n:255\r\n");
    exec_cmd(&d, &out, 8, "BITFIELD", "bits", "OVERFLOW", "FAIL", "INCRBY", "u8", "0", "1");
    EXPECT(out, "*1\r\n$-1\r\n");
    exec_cmd(&d, &out, 6, "BITFIELD", "bits", "SET", "i8", "0", "-1");
    EXPECT(out, "*1\r\n:-1\r\n");
    exec_cmd(&d, &out, 8, "BITFIELD_RO", "bits", "GET", "u8", "0", "GET", "i8", "0");
    EXPECT(out, "*2\r\n:255\r\n:-1\r\n");

    exec_cmd(&d, &out, 6, "BITFIELD", "neg", "SET", "u8", "0", "0");
    EXPECT(out, "*1\r\n:0\r\n");
    exec_cmd(&d, &out, 6, "BITFIELD", "neg", "SET", "u1", "-1", "1");
    EXPECT(out, "*1\r\n:0\r\n");
    exec_cmd(&d, &out, 5, "BITFIELD", "neg", "GET", "u1", "-1");
    EXPECT(out, "*1\r\n:1\r\n");

    exec_cmd(&d, &out, 5, "BITFIELD", "missing", "GET", "u8", "0");
    EXPECT(out, "*1\r\n:0\r\n");

    exec_cmd(&d, &out, 6, "BITFIELD", "signed", "SET", "i8", "0", "127");
    EXPECT(out, "*1\r\n:0\r\n");
    exec_cmd(&d, &out, 6, "BITFIELD", "signed", "INCRBY", "i8", "0", "1");
    EXPECT(out, "*1\r\n:-128\r\n");
    exec_cmd(&d, &out, 8, "BITFIELD", "signed", "OVERFLOW", "SAT", "SET", "i8", "0", "127");
    EXPECT(out, "*1\r\n:-128\r\n");
    exec_cmd(&d, &out, 8, "BITFIELD", "signed", "OVERFLOW", "SAT", "INCRBY", "i8", "0", "1");
    EXPECT(out, "*1\r\n:127\r\n");
    exec_cmd(&d, &out, 8, "BITFIELD", "signed", "OVERFLOW", "FAIL", "INCRBY", "i8", "0", "1");
    EXPECT(out, "*1\r\n$-1\r\n");

    exec_cmd(&d, &out, 6, "BITFIELD", "bits", "SET", "u64", "0", "1");
    EXPECT(out, "-ERR Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.\r\n");
    exec_cmd(&d, &out, 6, "BITFIELD", "bits", "SET", "u8", "-9", "1");
    EXPECT(out, "-ERR bit offset is not an integer or out of range\r\n");
    exec_cmd(&d, &out, 7, "BITFIELD_RO", "bits", "SET", "u8", "0", "1");
    EXPECT(out, "-ERR BITFIELD_RO only supports the GET subcommand\r\n");
    exec_cmd(&d, &out, 2, "BITFIELD", "bits");
    EXPECT(out, "-ERR wrong number of arguments for 'bitfield' command\r\n");

    exec_cmd(&d, &out, 4, "HSET", "hash", "f", "v");
    exec_cmd(&d, &out, 5, "BITFIELD", "hash", "GET", "u8", "0");
    EXPECT(out, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_getbit_setbit);
    DD_RUN(test_bitcount_bitpos);
    DD_RUN(test_bitop);
    DD_RUN(test_bitfield);
    return DD_TEST_SUMMARY();
}
