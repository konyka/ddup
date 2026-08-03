/* test_set.c - set object commands with synthetic injected time. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "ds/obj.h"
#include "test.h"

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

#define T0 1000000ULL

static uint64_t eb(size_t klen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + klen + vlen;
}

static void check_contains(const resp_buf *out, const char *bulk)
{
    char nul[1024];
    DD_CHECK(out->len < sizeof(nul) - 1);
    memcpy(nul, out->data, out->len);
    nul[out->len] = '\0';
    DD_CHECK(strstr(nul, bulk) != NULL);
}

static void test_sadd_basics(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 4, "SADD", "s", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 5, "SADD", "s", "a", "b", "c");
    EXPECT(out, ":1\r\n"); /* only c is new */
    exec_cmd(&d, T0, &out, 2, "SCARD", "s");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 2, "SCARD", "nokey");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s", "x");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "nokey", "a");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, T0, &out, 5, "SMISMEMBER", "s", "a", "x", "c");
    EXPECT(out, "*3\r\n:1\r\n:0\r\n:1\r\n");

    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "s");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*3\r\n", 4) == 0);
    check_contains(&out, "$1\r\na\r\n");
    check_contains(&out, "$1\r\nb\r\n");
    check_contains(&out, "$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "nokey");
    EXPECT(out, "*0\r\n");

    /* wrong arg count */
    exec_cmd(&d, T0, &out, 2, "SADD", "s");
    EXPECT(out, "-ERR wrong number of arguments for 'sadd' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_srem_auto_delete(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "SADD", "s", "a", "b", "c");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 4, "SREM", "s", "a", "x");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "SREM", "s", "b", "c");
    EXPECT(out, ":2\r\n");
    /* set became empty: key deleted */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "s");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "SREM", "s", "a");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_spop(void)
{
    db d;
    resp_buf out;
    int i;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "SADD", "s", "a", "b", "c");
    EXPECT(out, ":3\r\n");

    /* single pops: distinct members, set shrinks */
    exec_cmd(&d, T0, &out, 2, "SPOP", "s");
    DD_CHECK(out.len == 7 && out.data[0] == '$');
    exec_cmd(&d, T0, &out, 2, "SCARD", "s");
    EXPECT(out, ":2\r\n");

    /* pop with count */
    exec_cmd(&d, T0, &out, 3, "SPOP", "s", "5");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    /* set is now empty: key gone */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "s");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, T0, &out, 2, "SPOP", "s");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "SPOP", "s", "3");
    EXPECT(out, "*0\r\n");

    /* count 0 -> empty array, key untouched */
    exec_cmd(&d, T0, &out, 4, "SADD", "t", "x", "y");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "SPOP", "t", "0");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 2, "SCARD", "t");
    EXPECT(out, ":2\r\n");

    /* popping all members one by one yields each exactly once */
    {
        char seen[8];
        int nseen = 0;
        for (i = 0; i < 2; i++) {
            exec_cmd(&d, T0, &out, 2, "SPOP", "t");
            DD_CHECK(out.len == 7 && out.data[0] == '$');
            DD_CHECK(memchr(seen, out.data[4], (size_t)nseen) == NULL);
            seen[nseen++] = out.data[4];
        }
        exec_cmd(&d, T0, &out, 2, "EXISTS", "t");
        EXPECT(out, ":0\r\n");
    }

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_srandmember(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 2, "SRANDMEMBER", "nokey");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "SRANDMEMBER", "nokey", "3");
    EXPECT(out, "*0\r\n");

    exec_cmd(&d, T0, &out, 5, "SADD", "s", "a", "b", "c");
    EXPECT(out, ":3\r\n");

    /* single: from the set, not removed */
    exec_cmd(&d, T0, &out, 2, "SRANDMEMBER", "s");
    DD_CHECK(out.len == 7 && out.data[0] == '$');
    exec_cmd(&d, T0, &out, 2, "SCARD", "s");
    EXPECT(out, ":3\r\n");

    /* positive count: distinct members, at most count */
    exec_cmd(&d, T0, &out, 3, "SRANDMEMBER", "s", "2");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    /* count >= card: whole set */
    exec_cmd(&d, T0, &out, 3, "SRANDMEMBER", "s", "10");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*3\r\n", 4) == 0);
    check_contains(&out, "$1\r\na\r\n");
    check_contains(&out, "$1\r\nb\r\n");
    check_contains(&out, "$1\r\nc\r\n");
    /* negative count: exactly |count|, repeats allowed */
    exec_cmd(&d, T0, &out, 3, "SRANDMEMBER", "s", "-5");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*5\r\n", 4) == 0);
    check_contains(&out, "$1\r\n");
    /* set unchanged */
    exec_cmd(&d, T0, &out, 2, "SCARD", "s");
    EXPECT(out, ":3\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_smove(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 4, "SADD", "src", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "SADD", "dst", "c");
    EXPECT(out, ":1\r\n");

    exec_cmd(&d, T0, &out, 4, "SMOVE", "src", "dst", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "dst", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "src", "a");
    EXPECT(out, ":0\r\n");

    /* non-member and missing source */
    exec_cmd(&d, T0, &out, 4, "SMOVE", "src", "dst", "zz");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "SMOVE", "nosuch", "dst", "b");
    EXPECT(out, ":0\r\n");

    /* same-key move of an existing member is a no-op returning 1 */
    exec_cmd(&d, T0, &out, 4, "SMOVE", "dst", "dst", "a");
    EXPECT(out, ":1\r\n");

    /* moving the last member deletes the source key */
    exec_cmd(&d, T0, &out, 4, "SMOVE", "src", "dst", "b");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "src");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_ops(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "SADD", "a", "1", "2", "3", "4");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 5, "SADD", "b", "3", "4", "5");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 4, "SADD", "c", "4", "5");
    EXPECT(out, ":2\r\n");

    exec_cmd(&d, T0, &out, 3, "SINTER", "a", "b");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    check_contains(&out, "$1\r\n3\r\n");
    check_contains(&out, "$1\r\n4\r\n");

    exec_cmd(&d, T0, &out, 4, "SINTER", "a", "b", "c");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*1\r\n", 4) == 0);
    check_contains(&out, "$1\r\n4\r\n");

    /* missing key empties SINTER/SDIFF but is ignored by SUNION */
    exec_cmd(&d, T0, &out, 3, "SINTER", "a", "nokey");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 3, "SDIFF", "a", "nokey");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);
    exec_cmd(&d, T0, &out, 3, "SUNION", "a", "nokey");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);

    exec_cmd(&d, T0, &out, 3, "SUNION", "a", "b");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*5\r\n", 4) == 0);
    check_contains(&out, "$1\r\n1\r\n");
    check_contains(&out, "$1\r\n5\r\n");

    exec_cmd(&d, T0, &out, 3, "SDIFF", "a", "b");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    check_contains(&out, "$1\r\n1\r\n");
    check_contains(&out, "$1\r\n2\r\n");

    /* SDIFF with missing first key -> empty */
    exec_cmd(&d, T0, &out, 3, "SDIFF", "nokey", "a");
    EXPECT(out, "*0\r\n");

    /* operands are not modified */
    exec_cmd(&d, T0, &out, 2, "SCARD", "a");
    EXPECT(out, ":4\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_wrongtype(void)
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
    exec_cmd(&d, T0, &out, 3, "SADD", "s", "a");
    EXPECT(out, ":1\r\n");

    /* set commands on a string key */
    exec_cmd(&d, T0, &out, 3, "SADD", "str", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "str", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "SCARD", "str");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "str");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "SPOP", "str");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "SINTER", "s", "str");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "SMOVE", "str", "s", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "SMOVE", "s", "str", "a");
    EXPECT(out, wt);

    /* other types on a set key */
    exec_cmd(&d, T0, &out, 2, "GET", "s");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "HSET", "s", "f", "v");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "LPUSH", "s", "a");
    EXPECT(out, wt);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_ttl_and_memory(void)
{
    db d;
    resp_buf out;
    uint64_t before;
    db_init(&d);
    resp_buf_init(&out);

    /* member entries carry an empty value */
    exec_cmd(&d, T0, &out, 3, "SADD", "s", "a");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_set) + eb(1, 0));

    exec_cmd(&d, T0, &out, 3, "SADD", "s", "bb");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_set) + eb(1, 0) + eb(2, 0));

    exec_cmd(&d, T0, &out, 3, "SREM", "s", "bb");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_set) + eb(1, 0));

    /* TTL works; expiry frees the whole object */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "s", "10");
    EXPECT(out, ":1\r\n");
    before = d.used_memory;
    exec_cmd(&d, T0 + 10000, &out, 2, "SCARD", "s");
    EXPECT(out, ":0\r\n");
    DD_CHECK(d.used_memory ==
             before - (eb(1, 9) + sizeof(obj_set) + eb(1, 0) + eb(1, 8)));
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_sadd_basics);
    DD_RUN(test_srem_auto_delete);
    DD_RUN(test_spop);
    DD_RUN(test_srandmember);
    DD_RUN(test_smove);
    DD_RUN(test_set_ops);
    DD_RUN(test_set_wrongtype);
    DD_RUN(test_set_ttl_and_memory);
    return DD_TEST_SUMMARY();
}
