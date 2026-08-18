/* test_stringx.c - extended string command tests (written before the impl):
 * GETDEL, GETEX, SETEX, PSETEX, GETSET, SETRANGE, GETRANGE,
 * INCRBY, DECRBY, INCRBYFLOAT. Synthetic wall time, no sleeps. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

#define T0 1000000ULL /* synthetic epoch base, ms */

/* Build an argv of bulk strings from varargs and execute with injected time.
 * out is reset first, so the reply is exactly out.data[0..out.len). */
static void exec_cmd(db *d, uint64_t now, resp_buf *out, int argc, ...)
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
    command_execute_at(d, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

static const char WRONGTYPE_REPLY[] =
    "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
static const char NOT_INT_REPLY[] =
    "-ERR value is not an integer or out of range\r\n";
static const char OVERFLOW_REPLY[] =
    "-ERR increment or decrement would overflow\r\n";
static const char NOT_FLOAT_REPLY[] =
    "-ERR value is not a valid float\r\n";
static const char SYNTAX_REPLY[] = "-ERR syntax error\r\n";

static void test_getdel(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "k", "v");
    exec_cmd(&d, T0, &out, 2, "GETDEL", "k");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n");

    /* missing key: null bulk, no error */
    exec_cmd(&d, T0, &out, 2, "GETDEL", "missing");
    EXPECT(out, "$-1\r\n");

    /* expired key is treated as missing (and lazily collected) */
    exec_cmd(&d, T0, &out, 3, "SET", "e", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "e", "1000");
    exec_cmd(&d, T0 + 2000, &out, 2, "GETDEL", "e");
    EXPECT(out, "$-1\r\n");

    /* non-string: WRONGTYPE, key untouched */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 2, "GETDEL", "h");
    EXPECT(out, WRONGTYPE_REPLY);
    exec_cmd(&d, T0, &out, 2, "EXISTS", "h");
    EXPECT(out, ":1\r\n");

    /* write semantics: a successful GETDEL bumps dirty (AOF hook) */
    exec_cmd(&d, T0, &out, 3, "SET", "d2", "v");
    {
        uint64_t before = d.dirty;
        exec_cmd(&d, T0, &out, 2, "GETDEL", "d2");
        EXPECT(out, "$1\r\nv\r\n");
        DD_CHECK(d.dirty > before);
    }

    exec_cmd(&d, T0, &out, 1, "GETDEL");
    EXPECT(out, "-ERR wrong number of arguments for 'getdel' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_getex(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* plain GETEX: value back, TTL untouched */
    exec_cmd(&d, T0, &out, 3, "SET", "k", "v");
    exec_cmd(&d, T0, &out, 2, "GETEX", "k");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":-1\r\n");

    /* missing key: null bulk; options parsed but not applied */
    exec_cmd(&d, T0, &out, 2, "GETEX", "missing");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 4, "GETEX", "missing", "EX", "10");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "missing");
    EXPECT(out, ":0\r\n");

    /* PERSIST clears an existing TTL */
    exec_cmd(&d, T0, &out, 3, "SET", "p", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "p", "100000");
    exec_cmd(&d, T0, &out, 3, "GETEX", "p", "PERSIST");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "p");
    EXPECT(out, ":-1\r\n");

    /* PERSIST without a TTL: still fine, no mutation */
    exec_cmd(&d, T0, &out, 3, "GETEX", "k", "PERSIST");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":-1\r\n");

    /* EX/PX set a fresh relative TTL */
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "EX", "100");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k");
    EXPECT(out, ":100000\r\n");
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "PX", "5000");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k");
    EXPECT(out, ":5000\r\n");

    /* EXAT/PXAT set an absolute expiry */
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "EXAT", "1100");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "PEXPIRETIME", "k");
    EXPECT(out, ":1100000\r\n");
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "PXAT", "1200000");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "PEXPIRETIME", "k");
    EXPECT(out, ":1200000\r\n");

    /* past absolute expiry: value returned, key deleted */
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "EXAT", "999");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "k");
    EXPECT(out, ":0\r\n");

    /* invalid expire values */
    exec_cmd(&d, T0, &out, 3, "SET", "k", "v");
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "EX", "0");
    EXPECT(out, "-ERR invalid expire time in 'getex' command\r\n");
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "PX", "-5");
    EXPECT(out, "-ERR invalid expire time in 'getex' command\r\n");
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "EXAT", "0");
    EXPECT(out, "-ERR invalid expire time in 'getex' command\r\n");
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "EX", "foo");
    EXPECT(out, NOT_INT_REPLY);

    /* unknown/duplicate/incomplete options: syntax error */
    exec_cmd(&d, T0, &out, 3, "GETEX", "k", "FOO");
    EXPECT(out, SYNTAX_REPLY);
    exec_cmd(&d, T0, &out, 6, "GETEX", "k", "EX", "10", "PX", "20");
    EXPECT(out, SYNTAX_REPLY);
    exec_cmd(&d, T0, &out, 4, "GETEX", "k", "PERSIST", "PERSIST");
    EXPECT(out, SYNTAX_REPLY);
    exec_cmd(&d, T0, &out, 3, "GETEX", "k", "EX");
    EXPECT(out, SYNTAX_REPLY);

    /* non-string: WRONGTYPE */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 2, "GETEX", "h");
    EXPECT(out, WRONGTYPE_REPLY);
    exec_cmd(&d, T0, &out, 4, "GETEX", "h", "EX", "10");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 1, "GETEX");
    EXPECT(out, "-ERR wrong number of arguments for 'getex' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_setex_psetex(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 4, "SETEX", "k", "10", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k");
    EXPECT(out, ":10000\r\n");

    exec_cmd(&d, T0, &out, 4, "PSETEX", "k2", "5000", "v2");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k2");
    EXPECT(out, "$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k2");
    EXPECT(out, ":5000\r\n");

    /* overwriting clears the old TTL, sets the new one */
    exec_cmd(&d, T0, &out, 3, "SET", "k3", "old");
    exec_cmd(&d, T0, &out, 4, "SETEX", "k3", "20", "new");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k3");
    EXPECT(out, "$3\r\nnew\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k3");
    EXPECT(out, ":20000\r\n");

    /* non-positive TTLs */
    exec_cmd(&d, T0, &out, 4, "SETEX", "k", "0", "v");
    EXPECT(out, "-ERR invalid expire time in 'setex' command\r\n");
    exec_cmd(&d, T0, &out, 4, "SETEX", "k", "-1", "v");
    EXPECT(out, "-ERR invalid expire time in 'setex' command\r\n");
    exec_cmd(&d, T0, &out, 4, "PSETEX", "k", "0", "v");
    EXPECT(out, "-ERR invalid expire time in 'psetex' command\r\n");
    exec_cmd(&d, T0, &out, 4, "SETEX", "k", "foo", "v");
    EXPECT(out, NOT_INT_REPLY);

    /* SET semantics: refuses to overwrite a non-string (matches SET) */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 4, "SETEX", "h", "10", "v");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 3, "SETEX", "k", "10");
    EXPECT(out, "-ERR wrong number of arguments for 'setex' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_getset(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key: null bulk, key created */
    exec_cmd(&d, T0, &out, 3, "GETSET", "k", "v1");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv1\r\n");

    /* existing key: old value back, new value stored */
    exec_cmd(&d, T0, &out, 3, "GETSET", "k", "v2");
    EXPECT(out, "$2\r\nv1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv2\r\n");

    /* old TTL is discarded (SET semantics) */
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "k", "100000");
    exec_cmd(&d, T0, &out, 3, "GETSET", "k", "v3");
    EXPECT(out, "$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":-1\r\n");

    /* non-string old value: WRONGTYPE */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 3, "GETSET", "h", "v");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 2, "GETSET", "k");
    EXPECT(out, "-ERR wrong number of arguments for 'getset' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_setrange(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* in-place overwrite */
    exec_cmd(&d, T0, &out, 3, "SET", "k", "abc");
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k", "1", "XY");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$3\r\naXY\r\n");

    /* extend beyond the end: gap is zero padded */
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k", "5", "z");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    DD_CHECK_MEM("$6\r\naXY\0\0z\r\n", 12, out.data, out.len);

    /* missing key: created as an empty string, zero padded */
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k2", "4", "ab");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k2");
    DD_CHECK_MEM("$6\r\n\0\0\0\0ab\r\n", 12, out.data, out.len);

    /* empty value + missing key: no key created, :0 */
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k3", "0", "");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "k3");
    EXPECT(out, ":0\r\n");

    /* empty value + existing key: length reported, content unchanged */
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k", "1", "");
    EXPECT(out, ":6\r\n");

    /* offset errors */
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k", "-1", "v");
    EXPECT(out, "-ERR offset is out of range\r\n");
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k", "foo", "v");
    EXPECT(out, NOT_INT_REPLY);

    /* 512MB proto ceiling: offset+len beyond it is rejected up front */
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "k", "536870913", "");
    EXPECT(out, "-ERR string exceeds maximum allowed size\r\n");

    /* non-string: WRONGTYPE */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 4, "SETRANGE", "h", "0", "v");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 3, "SETRANGE", "k", "0");
    EXPECT(out, "-ERR wrong number of arguments for 'setrange' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_setnx_msetnx(void)
{
    db d;
    resp_buf out;
    uint64_t dirty;
    db_init(&d);
    resp_buf_init(&out);

    /* SETNX writes once and never overwrites an extant value. */
    exec_cmd(&d, T0, &out, 3, "SETNX", "one", "v1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SETNX", "one", "v2");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "one");
    EXPECT(out, "$2\r\nv1\r\n");

    /* An expired value is absent, so the condition succeeds. */
    exec_cmd(&d, T0, &out, 3, "SET", "expired", "old");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "expired", "10");
    exec_cmd(&d, T0 + 11, &out, 3, "SETNX", "expired", "new");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0 + 11, &out, 2, "GET", "expired");
    EXPECT(out, "$3\r\nnew\r\n");

    /* A failed conditional write must not advance the dirty counter. */
    dirty = d.dirty;
    exec_cmd(&d, T0, &out, 3, "SETNX", "one", "ignored");
    EXPECT(out, ":0\r\n");
    DD_CHECK_EQ_INT((long long)dirty, (long long)d.dirty);

    /* MSETNX is all-or-nothing: a present key prevents every write. */
    exec_cmd(&d, T0, &out, 5, "MSETNX", "a", "1", "b", "2");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 5, "MSETNX", "a", "new", "c", "3");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "a");
    EXPECT(out, "$1\r\n1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "c");
    EXPECT(out, "$-1\r\n");

    /* Existing non-string values also make MSETNX fail, without WRONGTYPE. */
    exec_cmd(&d, T0, &out, 4, "HSET", "hash", "f", "v");
    exec_cmd(&d, T0, &out, 5, "MSETNX", "d", "4", "hash", "no");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "d");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, T0, &out, 2, "SETNX", "one");
    EXPECT(out, "-ERR wrong number of arguments for 'setnx' command\r\n");
    exec_cmd(&d, T0, &out, 4, "MSETNX", "x", "1", "y");
    EXPECT(out, "-ERR wrong number of arguments for 'msetnx' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_getrange(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "k", "Hello World");

    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "0", "4");
    EXPECT(out, "$5\r\nHello\r\n");
    /* negative indexes count from the tail */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "-5", "-1");
    EXPECT(out, "$5\r\nWorld\r\n");
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "0", "-1");
    EXPECT(out, "$11\r\nHello World\r\n");
    /* end beyond the length is clamped */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "6", "100");
    EXPECT(out, "$5\r\nWorld\r\n");
    /* start after end: empty bulk */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "5", "3");
    EXPECT(out, "$0\r\n\r\n");
    /* fully out of range: empty bulk */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "100", "200");
    EXPECT(out, "$0\r\n\r\n");
    /* both negative with start > end: empty bulk */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "-1", "-5");
    EXPECT(out, "$0\r\n\r\n");
    /* large negative bounds clamp to 0 */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "-100", "-50");
    EXPECT(out, "$1\r\nH\r\n");

    /* missing key: empty bulk */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "missing", "0", "-1");
    EXPECT(out, "$0\r\n\r\n");

    /* parse failures */
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "x", "1");
    EXPECT(out, NOT_INT_REPLY);
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "k", "0", "y");
    EXPECT(out, NOT_INT_REPLY);

    /* non-string: WRONGTYPE */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 4, "GETRANGE", "h", "0", "1");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 3, "GETRANGE", "k", "0");
    EXPECT(out, "-ERR wrong number of arguments for 'getrange' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_substr_alias(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "k", "Hello");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "SUBSTR", "k", "0", "4");
    EXPECT(out, "$5\r\nHello\r\n");
    exec_cmd(&d, T0, &out, 4, "SUBSTR", "k", "6", "100");
    EXPECT(out, "$0\r\n\r\n");
    exec_cmd(&d, T0, &out, 4, "SUBSTR", "k", "-1", "-5");
    EXPECT(out, "$0\r\n\r\n");
    exec_cmd(&d, T0, &out, 4, "SUBSTR", "missing", "0", "-1");
    EXPECT(out, "$0\r\n\r\n");
    exec_cmd(&d, T0, &out, 4, "SUBSTR", "k", "x", "1");
    EXPECT(out, NOT_INT_REPLY);
    exec_cmd(&d, T0, &out, 3, "SUBSTR", "k", "0");
    EXPECT(out, "-ERR wrong number of arguments for 'substr' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_incrby_decrby(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key starts at 0 */
    exec_cmd(&d, T0, &out, 3, "INCRBY", "k", "5");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\n5\r\n");

    exec_cmd(&d, T0, &out, 3, "SET", "k", "10");
    exec_cmd(&d, T0, &out, 3, "INCRBY", "k", "3");
    EXPECT(out, ":13\r\n");
    exec_cmd(&d, T0, &out, 3, "DECRBY", "k", "4");
    EXPECT(out, ":9\r\n");
    /* negative deltas */
    exec_cmd(&d, T0, &out, 3, "INCRBY", "k", "-20");
    EXPECT(out, ":-11\r\n");

    /* TTL is discarded (INCR/APPEND semantics here) */
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "k", "100000");
    exec_cmd(&d, T0, &out, 3, "INCRBY", "k", "1");
    EXPECT(out, ":-10\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":-1\r\n");

    /* unparseable delta or current value */
    exec_cmd(&d, T0, &out, 3, "INCRBY", "k", "foo");
    EXPECT(out, NOT_INT_REPLY);
    exec_cmd(&d, T0, &out, 3, "DECRBY", "k", "1.5");
    EXPECT(out, NOT_INT_REPLY);
    exec_cmd(&d, T0, &out, 3, "SET", "s", "abc");
    exec_cmd(&d, T0, &out, 3, "INCRBY", "s", "1");
    EXPECT(out, NOT_INT_REPLY);

    /* overflow / underflow */
    exec_cmd(&d, T0, &out, 3, "SET", "max", "9223372036854775807");
    exec_cmd(&d, T0, &out, 3, "INCRBY", "max", "1");
    EXPECT(out, OVERFLOW_REPLY);
    exec_cmd(&d, T0, &out, 3, "SET", "min", "-9223372036854775808");
    exec_cmd(&d, T0, &out, 3, "DECRBY", "min", "1");
    EXPECT(out, OVERFLOW_REPLY);
    exec_cmd(&d, T0, &out, 3, "INCRBY", "k", "-9223372036854775808");
    EXPECT(out, OVERFLOW_REPLY);
    exec_cmd(&d, T0, &out, 3, "DECRBY", "k", "-9223372036854775808");
    EXPECT(out, OVERFLOW_REPLY);

    /* non-string: WRONGTYPE */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 3, "INCRBY", "h", "1");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 2, "INCRBY", "k");
    EXPECT(out, "-ERR wrong number of arguments for 'incrby' command\r\n");
    exec_cmd(&d, T0, &out, 2, "DECRBY", "k");
    EXPECT(out, "-ERR wrong number of arguments for 'decrby' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_incrbyfloat(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key starts at 0 */
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "k", "1.5");
    EXPECT(out, "$3\r\n1.5\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$3\r\n1.5\r\n");

    exec_cmd(&d, T0, &out, 3, "SET", "k", "10");
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "k", "0.5");
    EXPECT(out, "$4\r\n10.5\r\n");
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "k", "-1.25");
    EXPECT(out, "$4\r\n9.25\r\n");

    /* exponential notation in and out (%.17g style) */
    exec_cmd(&d, T0, &out, 3, "SET", "e", "5.0e3");
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "e", "2.0e2");
    EXPECT(out, "$4\r\n5200\r\n");

    /* unparseable current value or delta */
    exec_cmd(&d, T0, &out, 3, "SET", "s", "abc");
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "s", "1");
    EXPECT(out, NOT_FLOAT_REPLY);
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "k", "xyz");
    EXPECT(out, NOT_FLOAT_REPLY);
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "k", "inf");
    EXPECT(out, NOT_FLOAT_REPLY);
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "k", "nan");
    EXPECT(out, NOT_FLOAT_REPLY);

    /* result must stay finite: overflow past the long double ceiling.
     * On platforms where long double == double the delta itself is not
     * representable, so the "not a valid float" error is also correct. */
    exec_cmd(&d, T0, &out, 3, "SET", "big", "9e4931");
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "big", "9e4931");
    {
        long double probe = strtold("9e4931", NULL);
        if (isinf(probe))
            EXPECT(out, NOT_FLOAT_REPLY);
        else
            EXPECT(out, "-ERR increment would produce NaN or Infinity\r\n");
    }

    /* non-string: WRONGTYPE */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 3, "INCRBYFLOAT", "h", "1");
    EXPECT(out, WRONGTYPE_REPLY);

    exec_cmd(&d, T0, &out, 2, "INCRBYFLOAT", "k");
    EXPECT(out,
           "-ERR wrong number of arguments for 'incrbyfloat' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_keepttl(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* KEEPTTL preserves the existing TTL across an overwrite */
    exec_cmd(&d, T0, &out, 3, "SET", "k", "v1");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "k", "10000");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "SET", "k", "v2", "KEEPTTL");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k");
    EXPECT(out, ":10000\r\n");

    /* a plain overwrite still clears the TTL */
    exec_cmd(&d, T0, &out, 3, "SET", "k", "v3");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k");
    EXPECT(out, ":-1\r\n");

    /* KEEPTTL on a missing key: plain set, no TTL to keep */
    exec_cmd(&d, T0, &out, 4, "SET", "fresh", "v", "KEEPTTL");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "fresh");
    EXPECT(out, ":-1\r\n");

    /* the absolute expiry instant is kept, not the relative ttl */
    exec_cmd(&d, T0, &out, 3, "SET", "a", "v1");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "a", "10000");
    exec_cmd(&d, T0 + 4000, &out, 4, "SET", "a", "v2", "KEEPTTL");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0 + 4000, &out, 2, "PTTL", "a");
    EXPECT(out, ":6000\r\n");

    /* KEEPTTL composes with XX / NX */
    exec_cmd(&d, T0, &out, 3, "SET", "c", "v1");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "c", "10000");
    exec_cmd(&d, T0, &out, 5, "SET", "c", "v2", "XX", "KEEPTTL");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "c");
    EXPECT(out, ":10000\r\n");
    exec_cmd(&d, T0, &out, 5, "SET", "c", "v3", "NX", "KEEPTTL");
    EXPECT(out, "$-1\r\n"); /* NX abort: value and TTL untouched */
    exec_cmd(&d, T0, &out, 2, "GET", "c");
    EXPECT(out, "$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "c");
    EXPECT(out, ":10000\r\n");
    exec_cmd(&d, T0, &out, 5, "SET", "c2", "v", "NX", "KEEPTTL");
    EXPECT(out, "+OK\r\n");

    /* KEEPTTL conflicts with EX/PX and with itself: syntax error */
    exec_cmd(&d, T0, &out, 6, "SET", "k", "v", "KEEPTTL", "EX", "10");
    EXPECT(out, SYNTAX_REPLY);
    exec_cmd(&d, T0, &out, 6, "SET", "k", "v", "EX", "10", "KEEPTTL");
    EXPECT(out, SYNTAX_REPLY);
    exec_cmd(&d, T0, &out, 6, "SET", "k", "v", "PX", "100", "KEEPTTL");
    EXPECT(out, SYNTAX_REPLY);
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "KEEPTTL", "KEEPTTL");
    EXPECT(out, SYNTAX_REPLY);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_get_option(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* GET returns the old value (null when the key did not exist) */
    exec_cmd(&d, T0, &out, 4, "SET", "k", "v1", "GET");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 4, "SET", "k", "v2", "GET");
    EXPECT(out, "$2\r\nv1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv2\r\n");

    /* non-string old value: WRONGTYPE, the set is not performed */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 4, "SET", "h", "x", "GET");
    EXPECT(out, WRONGTYPE_REPLY);
    exec_cmd(&d, T0, &out, 2, "TYPE", "h");
    EXPECT(out, "+hash\r\n");

    /* GET composes with NX / XX / EX */
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v3", "GET", "NX");
    EXPECT(out, "$-1\r\n"); /* NX abort on an existing key */
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v4", "GET", "XX");
    EXPECT(out, "$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 6, "SET", "k", "v5", "GET", "EX", "100");
    EXPECT(out, "$2\r\nv4\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":100\r\n");

    /* GET composes with KEEPTTL */
    exec_cmd(&d, T0, &out, 3, "SET", "g", "old");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "g", "9000");
    exec_cmd(&d, T0, &out, 5, "SET", "g", "new", "GET", "KEEPTTL");
    EXPECT(out, "$3\r\nold\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "g");
    EXPECT(out, ":9000\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "g");
    EXPECT(out, "$3\r\nnew\r\n");

    /* duplicate GET: syntax error */
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "GET", "GET");
    EXPECT(out, SYNTAX_REPLY);

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_getdel);
    DD_RUN(test_getex);
    DD_RUN(test_setex_psetex);
    DD_RUN(test_getset);
    DD_RUN(test_set_keepttl);
    DD_RUN(test_set_get_option);
    DD_RUN(test_setrange);
    DD_RUN(test_setnx_msetnx);
    DD_RUN(test_getrange);
    DD_RUN(test_substr_alias);
    DD_RUN(test_incrby_decrby);
    DD_RUN(test_incrbyfloat);
    return DD_TEST_SUMMARY();
}
