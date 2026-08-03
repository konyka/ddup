/* test_zset.c - sorted set commands with synthetic injected time. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "ds/obj.h"
#include "test.h"

static void exec_cmd(db *d, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[12];
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

#define T0 1000000ULL

static uint64_t eb(size_t klen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + klen + vlen;
}

static void fill_abc(db *d, resp_buf *out)
{
    exec_cmd(d, T0, out, 8, "ZADD", "z", "1", "a", "2.5", "b", "3", "c");
    EXPECT(*out, ":3\r\n");
}

static void test_zadd_zscore_zcard(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    fill_abc(&d, &out);
    exec_cmd(&d, T0, &out, 6, "ZADD", "z", "4", "d", "1.5", "a");
    EXPECT(out, ":1\r\n"); /* only d is new; a updated */

    exec_cmd(&d, T0, &out, 2, "ZCARD", "z");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 2, "ZCARD", "nokey");
    EXPECT(out, ":0\r\n");

    /* %.17g formatting */
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "z", "a");
    EXPECT(out, "$3\r\n1.5\r\n");
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "z", "b");
    EXPECT(out, "$3\r\n2.5\r\n");
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "z", "c");
    EXPECT(out, "$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "z", "nope");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "nokey", "a");
    EXPECT(out, "$-1\r\n");

    /* infinities round-trip */
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "inf", "hi");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "z", "hi");
    EXPECT(out, "$3\r\ninf\r\n");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "-inf", "lo");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "z", "lo");
    EXPECT(out, "$4\r\n-inf\r\n");

    /* bad scores */
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "abc", "x");
    EXPECT(out, "-ERR value is not a valid float\r\n");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "nan", "x");
    EXPECT(out, "-ERR value is not a valid float\r\n");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "1x", "x");
    EXPECT(out, "-ERR value is not a valid float\r\n");
    exec_cmd(&d, T0, &out, 3, "ZADD", "z", "1");
    EXPECT(out, "-ERR wrong number of arguments for 'zadd' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zincrby_zrem(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* creates key/member */
    exec_cmd(&d, T0, &out, 4, "ZINCRBY", "z", "5", "a");
    EXPECT(out, "$1\r\n5\r\n");
    exec_cmd(&d, T0, &out, 4, "ZINCRBY", "z", "2.5", "a");
    EXPECT(out, "$3\r\n7.5\r\n");
    exec_cmd(&d, T0, &out, 4, "ZINCRBY", "z", "-1", "a");
    EXPECT(out, "$3\r\n6.5\r\n");

    /* NaN result rejected */
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "inf", "b");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "ZINCRBY", "z", "-inf", "b");
    EXPECT(out, "-ERR resulting score is not a number (NaN)\r\n");
    exec_cmd(&d, T0, &out, 4, "ZINCRBY", "z", "nan", "a");
    EXPECT(out, "-ERR value is not a valid float\r\n");

    /* ZREM + auto-delete */
    exec_cmd(&d, T0, &out, 4, "ZREM", "z", "a", "nope");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZREM", "z", "b");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "z");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "ZREM", "z", "a");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zrange_rank(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    fill_abc(&d, &out); /* a:1 b:2.5 c:3 */

    exec_cmd(&d, T0, &out, 4, "ZRANGE", "z", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 5, "ZRANGE", "z", "0", "-1", "WITHSCORES");
    EXPECT(out,
           "*6\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$3\r\n2.5\r\n$1\r\nc\r\n$"
           "1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREVRANGE", "z", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\nc\r\n$1\r\nb\r\n$1\r\na\r\n");
    exec_cmd(&d, T0, &out, 5, "ZREVRANGE", "z", "0", "0", "WITHSCORES");
    EXPECT(out, "*2\r\n$1\r\nc\r\n$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "z", "-2", "-1");
    EXPECT(out, "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "z", "5", "9");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "nokey", "0", "-1");
    EXPECT(out, "*0\r\n");

    /* ranks; update and delete keep them correct */
    exec_cmd(&d, T0, &out, 3, "ZRANK", "z", "a");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "ZREVRANK", "z", "a");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "10", "a");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "ZRANK", "z", "a");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "ZREVRANK", "z", "a");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "ZREM", "z", "b");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZRANK", "z", "c");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "ZRANK", "z", "b");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZRANK", "nokey", "a");
    EXPECT(out, "$-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zcount_byscore(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    fill_abc(&d, &out); /* a:1 b:2.5 c:3 */

    exec_cmd(&d, T0, &out, 4, "ZCOUNT", "z", "1", "3");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 4, "ZCOUNT", "z", "(1", "3");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 4, "ZCOUNT", "z", "(1", "(3");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "ZCOUNT", "z", "-inf", "+inf");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 4, "ZCOUNT", "z", "10", "20");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZCOUNT", "nokey", "0", "1");
    EXPECT(out, ":0\r\n");

    /* ZRANGEBYSCORE with bounds */
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYSCORE", "z", "1", "3");
    EXPECT(out, "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYSCORE", "z", "(1", "(3");
    EXPECT(out, "*1\r\n$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 5, "ZRANGEBYSCORE", "z", "1", "3", "WITHSCORES");
    EXPECT(out,
           "*6\r\n$1\r\na\r\n$1\r\n1\r\n$1\r\nb\r\n$3\r\n2.5\r\n$1\r\nc\r\n$"
           "1\r\n3\r\n");
    /* LIMIT offset count */
    exec_cmd(&d, T0, &out, 7, "ZRANGEBYSCORE", "z", "1", "3", "LIMIT", "1",
             "1");
    EXPECT(out, "*1\r\n$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 7, "ZRANGEBYSCORE", "z", "1", "3", "LIMIT", "5",
             "2");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 7, "ZRANGEBYSCORE", "z", "-inf", "+inf", "LIMIT",
             "0", "2");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    /* bad options */
    exec_cmd(&d, T0, &out, 5, "ZRANGEBYSCORE", "z", "1", "3", "BOGUS");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYSCORE", "z", "abc", "3");
    EXPECT(out, "-ERR value is not a valid float\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zremrangebyscore(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 10, "ZADD", "z", "1", "a", "2", "b", "3", "c",
             "4", "d");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYSCORE", "z", "(1", "3");
    EXPECT(out, ":2\r\n"); /* b,c */
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "z", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nd\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYSCORE", "z", "-inf", "+inf");
    EXPECT(out, ":2\r\n");
    /* empty zset: key deleted */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "z");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zset_wrongtype(void)
{
    db d;
    resp_buf out;
    const char *wt =
        "-WRONGTYPE Operation against a key holding the wrong kind of "
        "value\r\n";
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "str", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "SADD", "st", "m");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "1", "a");
    EXPECT(out, ":1\r\n");

    /* zset commands on string / set keys */
    exec_cmd(&d, T0, &out, 4, "ZADD", "str", "1", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "ZSCORE", "str", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "ZCARD", "str");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "str", "0", "-1");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "ZADD", "st", "1", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "ZRANK", "st", "m");
    EXPECT(out, wt);

    /* other types on a zset key */
    exec_cmd(&d, T0, &out, 2, "GET", "z");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "SADD", "z", "m");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "HSET", "z", "f", "v");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "LPUSH", "z", "a");
    EXPECT(out, wt);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zset_ttl_and_memory(void)
{
    db d;
    resp_buf out;
    uint64_t base, grown;
    db_init(&d);
    resp_buf_init(&out);

    base = d.used_memory;
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "1", "a");
    EXPECT(out, ":1\r\n");
    grown = d.used_memory;
    /* entry + zset struct + dict entry + skiplist node */
    DD_CHECK(grown > base + eb(1, 9) + sizeof(obj_zset));

    exec_cmd(&d, T0, &out, 3, "ZREM", "z", "a");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == base); /* auto-deleted, fully reclaimed */

    /* TTL expiry frees the whole object */
    exec_cmd(&d, T0, &out, 6, "ZADD", "z", "1", "a", "2", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "z", "10");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0 + 10000, &out, 2, "ZCARD", "z");
    EXPECT(out, ":0\r\n");
    DD_CHECK(d.used_memory == base);
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_zadd_zscore_zcard);
    DD_RUN(test_zincrby_zrem);
    DD_RUN(test_zrange_rank);
    DD_RUN(test_zcount_byscore);
    DD_RUN(test_zremrangebyscore);
    DD_RUN(test_zset_wrongtype);
    DD_RUN(test_zset_ttl_and_memory);
    return DD_TEST_SUMMARY();
}
