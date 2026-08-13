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

static void test_zset_rejects_unrepresentable_member(void)
{
    obj_zset *z = obj_zset_new();
    const char byte = 'x';
    uint64_t before = obj_zset_mem(z);

    DD_CHECK_EQ_INT(-1, obj_zset_add(z, &byte, SIZE_MAX, 1.0));
    DD_CHECK_EQ_INT(0, rh_size(&z->dict));
    DD_CHECK_EQ_INT(0, z->sl->length);
    DD_CHECK(obj_zset_mem(z) == before);
    obj_zset_free(z);
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

static int out_contains(const resp_buf *out, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    if (out->len < nl)
        return 0;
    for (i = 0; i + nl <= out->len; i++)
        if (memcmp(out->data + i, needle, nl) == 0)
            return 1;
    return 0;
}

static void test_zpopmin_zpopmax(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 10, "ZADD", "z", "1", "a", "2", "b", "3", "c",
             "4", "d");
    EXPECT(out, ":4\r\n");

    /* no count: still a flat member/score array (Redis 6.2+) */
    exec_cmd(&d, T0, &out, 2, "ZPOPMIN", "z");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\n1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZPOPMAX", "z", "2");
    EXPECT(out, "*4\r\n$1\r\nd\r\n$1\r\n4\r\n$1\r\nc\r\n$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "z", "0", "-1");
    EXPECT(out, "*1\r\n$1\r\nb\r\n");

    /* popping the last member deletes the key */
    exec_cmd(&d, T0, &out, 3, "ZPOPMIN", "z", "5");
    EXPECT(out, "*2\r\n$1\r\nb\r\n$1\r\n2\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "z");
    EXPECT(out, ":0\r\n");

    /* missing key / bad count */
    exec_cmd(&d, T0, &out, 2, "ZPOPMIN", "nokey");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 3, "ZPOPMAX", "nokey", "3");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 6, "ZADD", "z", "1", "a", "2", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "ZPOPMIN", "z", "0");
    EXPECT(out, "-ERR value is out of range, must be positive\r\n");
    exec_cmd(&d, T0, &out, 3, "ZPOPMAX", "z", "-1");
    EXPECT(out, "-ERR value is out of range, must be positive\r\n");
    exec_cmd(&d, T0, &out, 3, "ZPOPMIN", "z", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zremrangebyrank(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 10, "ZADD", "z", "1", "a", "2", "b", "3", "c",
             "4", "d");
    EXPECT(out, ":4\r\n");

    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYRANK", "z", "1", "2");
    EXPECT(out, ":2\r\n"); /* b,c */
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "z", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nd\r\n");

    /* negative subscripts, clamping */
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYRANK", "z", "-1", "-1");
    EXPECT(out, ":1\r\n"); /* d */
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYRANK", "z", "5", "9");
    EXPECT(out, ":0\r\n"); /* start past end */
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYRANK", "z", "0", "99");
    EXPECT(out, ":1\r\n"); /* a: clamped, empties the set */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "z");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYRANK", "nokey", "0", "-1");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYRANK", "nokey", "0", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zmscore(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "ZADD", "z", "1", "a", "2.5", "b");
    EXPECT(out, ":2\r\n");

    exec_cmd(&d, T0, &out, 5, "ZMSCORE", "z", "a", "b", "nope");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$3\r\n2.5\r\n$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZMSCORE", "nokey", "a");
    EXPECT(out, "*1\r\n$-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zrandmember(void)
{
    db d;
    resp_buf out;
    int i;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key: null bulk without count, empty array with count */
    exec_cmd(&d, T0, &out, 2, "ZRANDMEMBER", "nokey");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "ZRANDMEMBER", "nokey", "3");
    EXPECT(out, "*0\r\n");

    exec_cmd(&d, T0, &out, 8, "ZADD", "z", "1", "a", "2", "b", "3", "c");
    EXPECT(out, ":3\r\n");

    /* single member: one of the members, zset untouched */
    exec_cmd(&d, T0, &out, 2, "ZRANDMEMBER", "z");
    DD_CHECK(out.len == 7 && out.data[0] == '$');
    DD_CHECK(out.data[4] == 'a' || out.data[4] == 'b' || out.data[4] == 'c');
    exec_cmd(&d, T0, &out, 2, "ZCARD", "z");
    EXPECT(out, ":3\r\n");

    /* positive count: distinct members, at most count */
    exec_cmd(&d, T0, &out, 3, "ZRANDMEMBER", "z", "2");
    DD_CHECK(out.len == 18 && memcmp(out.data, "*2\r\n", 4) == 0);
    DD_CHECK(out.data[8] != out.data[16]);
    /* count >= card: every member exactly once */
    exec_cmd(&d, T0, &out, 3, "ZRANDMEMBER", "z", "10");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*3\r\n", 4) == 0);
    DD_CHECK(out_contains(&out, "$1\r\na\r\n"));
    DD_CHECK(out_contains(&out, "$1\r\nb\r\n"));
    DD_CHECK(out_contains(&out, "$1\r\nc\r\n"));
    exec_cmd(&d, T0, &out, 3, "ZRANDMEMBER", "z", "0");
    EXPECT(out, "*0\r\n");

    /* negative count: exactly |count|, repeats allowed */
    exec_cmd(&d, T0, &out, 3, "ZRANDMEMBER", "z", "-5");
    DD_CHECK(out.len == 4 + 5 * 7 && memcmp(out.data, "*5\r\n", 4) == 0);
    for (i = 0; i < 5; i++)
        DD_CHECK(out.data[8 + i * 7] == 'a' || out.data[8 + i * 7] == 'b' ||
                 out.data[8 + i * 7] == 'c');

    /* WITHSCORES: flat member/score pairs */
    exec_cmd(&d, T0, &out, 4, "ZRANDMEMBER", "z", "3", "WITHSCORES");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*6\r\n", 4) == 0);
    DD_CHECK(out_contains(&out, "$1\r\na\r\n$1\r\n1\r\n"));
    DD_CHECK(out_contains(&out, "$1\r\nb\r\n$1\r\n2\r\n"));
    DD_CHECK(out_contains(&out, "$1\r\nc\r\n$1\r\n3\r\n"));
    exec_cmd(&d, T0, &out, 4, "ZRANDMEMBER", "z", "-2", "withscores");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);

    /* errors */
    exec_cmd(&d, T0, &out, 3, "ZRANDMEMBER", "z", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANDMEMBER", "z", "2", "BADOPT");
    EXPECT(out, "-ERR syntax error\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void fill_lex(db *d, resp_buf *out)
{
    exec_cmd(d, T0, out, 8, "ZADD", "z", "0", "a", "0", "b", "0", "c");
    EXPECT(*out, ":3\r\n");
    exec_cmd(d, T0, out, 8, "ZADD", "z", "0", "d", "0", "e", "0", "f");
    EXPECT(*out, ":3\r\n");
}

static void test_zrangebylex(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    fill_lex(&d, &out); /* a..f all at score 0 */

    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "-", "+");
    EXPECT(out,
           "*6\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n$1\r\nd\r\n$1\r\ne\r\n$"
           "1\r\nf\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "[b", "[e");
    EXPECT(out, "*4\r\n$1\r\nb\r\n$1\r\nc\r\n$1\r\nd\r\n$1\r\ne\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "(b", "(e");
    EXPECT(out, "*2\r\n$1\r\nc\r\n$1\r\nd\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "-", "[c");
    EXPECT(out, "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "[e", "+");
    EXPECT(out, "*2\r\n$1\r\ne\r\n$1\r\nf\r\n");
    /* empty ranges */
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "(c", "(c");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "[e", "[b");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "[x", "[z");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "nokey", "-", "+");
    EXPECT(out, "*0\r\n");

    /* LIMIT offset count */
    exec_cmd(&d, T0, &out, 7, "ZRANGEBYLEX", "z", "-", "+", "LIMIT", "1",
             "2");
    EXPECT(out, "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 7, "ZRANGEBYLEX", "z", "-", "+", "LIMIT", "9",
             "2");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 7, "ZRANGEBYLEX", "z", "[b", "+", "LIMIT", "0",
             "-1");
    EXPECT(out, "*5\r\n$1\r\nb\r\n$1\r\nc\r\n$1\r\nd\r\n$1\r\ne\r\n$1\r\nf"
                "\r\n");

    /* invalid range strings / options */
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "a", "b");
    EXPECT(out, "-ERR min or max is not a valid string range item\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "[a", "]b");
    EXPECT(out, "-ERR min or max is not a valid string range item\r\n");
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "z", "", "+");
    EXPECT(out, "-ERR min or max is not a valid string range item\r\n");
    exec_cmd(&d, T0, &out, 5, "ZRANGEBYLEX", "z", "-", "+", "BOGUS");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 7, "ZRANGEBYLEX", "z", "-", "+", "LIMIT", "x",
             "1");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zrevrangebylex(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    fill_lex(&d, &out); /* a..f all at score 0; max comes first */

    exec_cmd(&d, T0, &out, 4, "ZREVRANGEBYLEX", "z", "+", "-");
    EXPECT(out,
           "*6\r\n$1\r\nf\r\n$1\r\ne\r\n$1\r\nd\r\n$1\r\nc\r\n$1\r\nb\r\n$"
           "1\r\na\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREVRANGEBYLEX", "z", "[e", "[b");
    EXPECT(out, "*4\r\n$1\r\ne\r\n$1\r\nd\r\n$1\r\nc\r\n$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREVRANGEBYLEX", "z", "(e", "(b");
    EXPECT(out, "*2\r\n$1\r\nd\r\n$1\r\nc\r\n");
    /* swapped bounds -> empty */
    exec_cmd(&d, T0, &out, 4, "ZREVRANGEBYLEX", "z", "[b", "[e");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREVRANGEBYLEX", "nokey", "+", "-");
    EXPECT(out, "*0\r\n");
    /* LIMIT */
    exec_cmd(&d, T0, &out, 7, "ZREVRANGEBYLEX", "z", "+", "-", "LIMIT", "1",
             "2");
    EXPECT(out, "*2\r\n$1\r\ne\r\n$1\r\nd\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREVRANGEBYLEX", "z", "a", "-");
    EXPECT(out, "-ERR min or max is not a valid string range item\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_zremrangebylex(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    fill_lex(&d, &out); /* a..f all at score 0 */

    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYLEX", "z", "[b", "(e");
    EXPECT(out, ":3\r\n"); /* b,c,d */
    exec_cmd(&d, T0, &out, 4, "ZRANGE", "z", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\na\r\n$1\r\ne\r\n$1\r\nf\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYLEX", "z", "[x", "[z");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYLEX", "z", "-", "+");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "z");
    EXPECT(out, ":0\r\n"); /* emptied: key deleted */
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYLEX", "nokey", "-", "+");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYLEX", "nokey", "a", "+");
    EXPECT(out, "-ERR min or max is not a valid string range item\r\n");

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
    exec_cmd(&d, T0, &out, 2, "ZPOPMIN", "str");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "ZPOPMAX", "st", "2");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYRANK", "str", "0", "-1");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "ZMSCORE", "st", "m");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "ZRANDMEMBER", "str");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "ZRANGEBYLEX", "st", "-", "+");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "ZREVRANGEBYLEX", "str", "+", "-");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "ZREMRANGEBYLEX", "st", "-", "+");
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

static void test_obj_str_zero_length_blob(void)
{
    const char byte = 'x';
    const char *s = &byte;
    size_t len = 123;
    obj_str(&byte, 0, &s, &len);
    DD_CHECK(s == &byte);
    DD_CHECK_EQ_INT(0, (long long)len);
}

int main(void)
{
    DD_RUN(test_obj_str_zero_length_blob);
    DD_RUN(test_zset_rejects_unrepresentable_member);
    DD_RUN(test_zadd_zscore_zcard);
    DD_RUN(test_zincrby_zrem);
    DD_RUN(test_zrange_rank);
    DD_RUN(test_zcount_byscore);
    DD_RUN(test_zremrangebyscore);
    DD_RUN(test_zpopmin_zpopmax);
    DD_RUN(test_zremrangebyrank);
    DD_RUN(test_zmscore);
    DD_RUN(test_zrandmember);
    DD_RUN(test_zrangebylex);
    DD_RUN(test_zrevrangebylex);
    DD_RUN(test_zremrangebylex);
    DD_RUN(test_zset_wrongtype);
    DD_RUN(test_zset_ttl_and_memory);
    return DD_TEST_SUMMARY();
}
