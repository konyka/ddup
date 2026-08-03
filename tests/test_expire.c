/* test_expire.c - key expiration with synthetic (injected) wall time. */
#include <stdarg.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

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

#define T0 1000000ULL /* synthetic epoch base, ms */

static void test_expire_and_ttl(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");

    /* no expiry set -> -1 */
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":-1\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k");
    EXPECT(out, ":-1\r\n");

    /* missing key -> -2 */
    exec_cmd(&d, T0, &out, 2, "TTL", "missing");
    EXPECT(out, ":-2\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "missing");
    EXPECT(out, ":-2\r\n");

    exec_cmd(&d, T0, &out, 3, "EXPIRE", "k", "10");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":10\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "k");
    EXPECT(out, ":10000\r\n");

    /* 5s later: 5s remain */
    exec_cmd(&d, T0 + 5000, &out, 2, "TTL", "k");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0 + 5000, &out, 2, "PTTL", "k");
    EXPECT(out, ":5000\r\n");

    /* key still visible just before expiry... */
    exec_cmd(&d, T0 + 9999, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");
    /* ...and lazily expired at the boundary (exp <= now) */
    exec_cmd(&d, T0 + 10000, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0 + 10000, &out, 2, "TTL", "k");
    EXPECT(out, ":-2\r\n");
    /* expiry entry was removed too: PERSIST on the gone key -> 0 */
    exec_cmd(&d, T0 + 10000, &out, 2, "PERSIST", "k");
    EXPECT(out, ":0\r\n");
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_expire_variants(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* EXPIRE on missing key -> 0 */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "nokey", "10");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "nokey", "10000");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, T0, &out, 3, "SET", "a", "1");
    EXPECT(out, "+OK\r\n");

    /* PEXPIRE with ms */
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "a", "2500");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0 + 1000, &out, 2, "PTTL", "a");
    EXPECT(out, ":1500\r\n");

    /* EXPIREAT with absolute unix seconds */
    exec_cmd(&d, T0, &out, 3, "SET", "b", "2");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "EXPIREAT", "b", "1100"); /* = T0+100s in ms */
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "b");
    EXPECT(out, ":100\r\n");

    /* PEXPIREAT with absolute unix ms in the past -> delete, return 1 */
    exec_cmd(&d, T0, &out, 3, "SET", "c", "3");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "PEXPIREAT", "c", "500"); /* < T0 */
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "c");
    EXPECT(out, "$-1\r\n");

    /* negative relative ttl -> delete, return 1 */
    exec_cmd(&d, T0, &out, 3, "SET", "e", "5");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "e", "-5");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "e");
    EXPECT(out, ":0\r\n");

    /* non-integer ttl -> error */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "a", "abc");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");

    /* PERSIST removes an existing expiry */
    exec_cmd(&d, T0, &out, 3, "SET", "p", "1");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "PERSIST", "p");
    EXPECT(out, ":0\r\n"); /* no expiry yet */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "p", "100");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "PERSIST", "p");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "p");
    EXPECT(out, ":-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_options(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* SET with EX sets ttl */
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "EX", "10");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":10\r\n");

    /* NX on existing key -> null bulk, value untouched */
    exec_cmd(&d, T0, &out, 4, "SET", "k", "v2", "NX");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");

    /* NX on missing key -> OK */
    exec_cmd(&d, T0, &out, 4, "SET", "n", "1", "NX");
    EXPECT(out, "+OK\r\n");

    /* XX on missing key -> null bulk */
    exec_cmd(&d, T0, &out, 4, "SET", "m", "1", "XX");
    EXPECT(out, "$-1\r\n");
    /* XX on existing key -> OK */
    exec_cmd(&d, T0, &out, 4, "SET", "n", "2", "XX");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "n");
    EXPECT(out, "$1\r\n2\r\n");

    /* PX option, case-insensitive */
    exec_cmd(&d, T0, &out, 5, "set", "p", "1", "px", "5000");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "p");
    EXPECT(out, ":5000\r\n");

    /* combined NX + EX */
    exec_cmd(&d, T0, &out, 6, "SET", "c", "1", "NX", "EX", "20");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "c");
    EXPECT(out, ":20\r\n");

    /* errors */
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "EX", "abc");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "EX", "-1");
    EXPECT(out, "-ERR invalid expire time in 'set' command\r\n");
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "PX", "0");
    EXPECT(out, "-ERR invalid expire time in 'set' command\r\n");
    exec_cmd(&d, T0, &out, 4, "SET", "k", "v", "FOO");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "NX", "XX");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 6, "SET", "k", "v", "EX", "10", "EX", "20");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "SET", "k", "v", "EX");
    EXPECT(out, "-ERR syntax error\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_overwrite_clears_ttl(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* plain SET clears a previous expiry */
    exec_cmd(&d, T0, &out, 5, "SET", "k", "v", "EX", "100");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "k", "v2");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "k");
    EXPECT(out, ":-1\r\n");

    /* INCR/APPEND (read-modify-write) also clear the expiry */
    exec_cmd(&d, T0, &out, 5, "SET", "n", "1", "EX", "100");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "INCR", "n");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "n");
    EXPECT(out, ":-1\r\n");

    exec_cmd(&d, T0, &out, 5, "SET", "s", "ab", "EX", "100");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "APPEND", "s", "cd");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "s");
    EXPECT(out, ":-1\r\n");

    /* DEL removes key and expiry together */
    exec_cmd(&d, T0, &out, 5, "SET", "z", "1", "EX", "100");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "DEL", "z");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "z");
    EXPECT(out, ":-2\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_expire_and_ttl);
    DD_RUN(test_expire_variants);
    DD_RUN(test_set_options);
    DD_RUN(test_overwrite_clears_ttl);
    return DD_TEST_SUMMARY();
}
