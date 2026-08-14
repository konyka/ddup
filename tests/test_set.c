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

/* small-set listpack payload: 7B empty frame + per-member 6-bit-string
 * entries (1B header + payload + 1B backlen); one malloc (16B estimate) */
static uint64_t slp_mem(uint64_t entry_bytes)
{
    return sizeof(obj_set) + 16 + 7 + entry_bytes;
}

static void test_set_rejects_unrepresentable_member(void)
{
    obj_set *s = obj_set_new();
    const char byte = 'x';
    uint64_t before = obj_set_mem(s);

    DD_CHECK_EQ_INT(-1, obj_set_add(s, &byte, SIZE_MAX));
    DD_CHECK_EQ_INT(0, (long long)obj_set_len(s));
    DD_CHECK(obj_set_is_listpack(s));
    DD_CHECK(obj_set_mem(s) == before);
    obj_set_free(s);
}

static void test_smove_rejection_preserves_both_sets(void)
{
    db d;
    resp_buf out;
    resp_value argv[4];
    const char byte = 'x';
    uint64_t used, dirty, version;
    const char *v;
    size_t vl;

    db_init(&d);
    resp_buf_init(&out);
    exec_cmd(&d, 1000000, &out, 3, "SADD", "src", "member");
    d.watch_refs = 1;
    used = d.used_memory;
    dirty = d.dirty;
    version = db_key_version(&d, "src", 3);
    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING; argv[0].str = "SMOVE"; argv[0].len = 5;
    argv[1].type = RESP_BULK_STRING; argv[1].str = "src"; argv[1].len = 3;
    argv[2].type = RESP_BULK_STRING; argv[2].str = &byte; argv[2].len = SIZE_MAX;
    argv[3].type = RESP_BULK_STRING; argv[3].str = "member"; argv[3].len = 6;
    out.len = 0;
    command_execute_at(&d, argv, 4, &out, 1000000);
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    DD_CHECK(d.used_memory == used && d.dirty == dirty);
    DD_CHECK(db_key_version(&d, "src", 3) == version);
    DD_CHECK(rh_get(&d.table, "src", 3, &v, &vl) == 1);
    DD_CHECK(obj_set_has((obj_set *)obj_unpack_ptr(v, vl), "member", 6));
    d.watch_refs = 0;
    resp_buf_free(&out);
    db_destroy(&d);
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

    /* member entries live in one flat listpack */
    exec_cmd(&d, T0, &out, 3, "SADD", "s", "a");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + slp_mem(1 + 1 + 1));

    exec_cmd(&d, T0, &out, 3, "SADD", "s", "bb");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory ==
             eb(1, 9) + slp_mem((1 + 1 + 1) + (1 + 2 + 1)));

    exec_cmd(&d, T0, &out, 3, "SREM", "s", "bb");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + slp_mem(1 + 1 + 1));

    /* TTL works; expiry frees the whole object */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "s", "10");
    EXPECT(out, ":1\r\n");
    before = d.used_memory;
    exec_cmd(&d, T0 + 10000, &out, 2, "SCARD", "s");
    EXPECT(out, ":0\r\n");
    DD_CHECK(d.used_memory ==
             before - (eb(1, 9) + slp_mem(1 + 1 + 1) + eb(1, 8)));
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_sintercard(void)
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

    exec_cmd(&d, T0, &out, 4, "SINTERCARD", "2", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 5, "SINTERCARD", "3", "a", "b", "c");
    EXPECT(out, ":1\r\n");
    /* missing key empties the intersection */
    exec_cmd(&d, T0, &out, 4, "SINTERCARD", "2", "a", "nokey");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "SINTERCARD", "1", "a");
    EXPECT(out, ":4\r\n");

    /* LIMIT: caps the result, 0 = no limit */
    exec_cmd(&d, T0, &out, 6, "SINTERCARD", "2", "a", "b", "LIMIT", "1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 6, "SINTERCARD", "2", "a", "b", "LIMIT", "0");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 6, "SINTERCARD", "2", "a", "b", "LIMIT", "100");
    EXPECT(out, ":2\r\n");

    /* operands are not modified */
    exec_cmd(&d, T0, &out, 2, "SCARD", "a");
    EXPECT(out, ":4\r\n");

    /* errors */
    exec_cmd(&d, T0, &out, 4, "SINTERCARD", "3", "a", "b");
    EXPECT(out,
           "-ERR Number of keys can't be greater than number of args\r\n");
    exec_cmd(&d, T0, &out, 3, "SINTERCARD", "0", "a");
    EXPECT(out,
           "-ERR Number of keys can't be greater than number of args\r\n");
    exec_cmd(&d, T0, &out, 3, "SINTERCARD", "x", "a");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 6, "SINTERCARD", "2", "a", "b", "LIMIT", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 6, "SINTERCARD", "2", "a", "b", "LIMIT", "-1");
    EXPECT(out, "-ERR LIMIT can't be negative\r\n");
    exec_cmd(&d, T0, &out, 6, "SINTERCARD", "2", "a", "b", "BOGUS", "1");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "SINTERCARD", "1", "a", "LIMIT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 2, "SINTERCARD", "2");
    EXPECT(out,
           "-ERR wrong number of arguments for 'sintercard' command\r\n");

    /* wrong type operand */
    exec_cmd(&d, T0, &out, 3, "SET", "str", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "SINTERCARD", "2", "a", "str");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_stores(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "SADD", "a", "1", "2", "3", "4");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 5, "SADD", "b", "3", "4", "5");
    EXPECT(out, ":3\r\n");

    /* SINTERSTORE: returns cardinality, dst becomes the result set */
    exec_cmd(&d, T0, &out, 4, "SINTERSTORE", "dst", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "dst");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    check_contains(&out, "$1\r\n3\r\n");
    check_contains(&out, "$1\r\n4\r\n");

    /* SUNIONSTORE / SDIFFSTORE */
    exec_cmd(&d, T0, &out, 4, "SUNIONSTORE", "dst", "a", "b");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 2, "SCARD", "dst");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 4, "SDIFFSTORE", "dst", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "dst");
    check_contains(&out, "$1\r\n1\r\n");
    check_contains(&out, "$1\r\n2\r\n");

    /* dst may hold any old type: overwritten, TTL cleared */
    exec_cmd(&d, T0, &out, 2, "DEL", "dst");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "dst", "str");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "dst", "100");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "SINTERSTORE", "dst", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "TYPE", "dst");
    EXPECT(out, "+set\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "dst");
    EXPECT(out, ":-1\r\n");

    /* empty result: dst deleted */
    exec_cmd(&d, T0, &out, 3, "SADD", "e1", "x");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SADD", "e2", "y");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "SINTERSTORE", "dst", "e1", "e2");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "dst");
    EXPECT(out, ":0\r\n");

    /* dst may be one of the sources (evaluated before the store) */
    exec_cmd(&d, T0, &out, 4, "SINTERSTORE", "a", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "SCARD", "a");
    EXPECT(out, ":2\r\n");

    /* wrong type source; dst untouched by the failure */
    exec_cmd(&d, T0, &out, 3, "SET", "str", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "SUNIONSTORE", "dst", "b", "str");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "dst");
    EXPECT(out, ":0\r\n");

    /* arity */
    exec_cmd(&d, T0, &out, 2, "SINTERSTORE", "dst");
    EXPECT(out,
           "-ERR wrong number of arguments for 'sinterstore' command\r\n");
    exec_cmd(&d, T0, &out, 2, "SUNIONSTORE", "dst");
    EXPECT(out,
           "-ERR wrong number of arguments for 'sunionstore' command\r\n");
    exec_cmd(&d, T0, &out, 2, "SDIFFSTORE", "dst");
    EXPECT(out,
           "-ERR wrong number of arguments for 'sdiffstore' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static obj_set *set_of(db *d, const char *k, size_t kl)
{
    const char *blob;
    size_t bloblen;
    if (rh_get(&d->table, k, kl, &blob, &bloblen) != 1)
        return NULL;
    return (obj_set *)obj_unpack_ptr(blob, bloblen);
}

static void test_set_listpack_encoding(void)
{
    db d;
    resp_buf out;
    char big[70];
    int i;
    db_init(&d);
    resp_buf_init(&out);

    /* small set starts as listpack */
    exec_cmd(&d, T0, &out, 5, "SADD", "s", "a", "b", "c");
    EXPECT(out, ":3\r\n");
    DD_CHECK(obj_set_is_listpack(set_of(&d, "s", 1)));

    /* int-looking member round-trips through an int-encoded entry */
    exec_cmd(&d, T0, &out, 3, "SADD", "s", "123");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s", "123");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "s");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);
    check_contains(&out, "$3\r\n123\r\n");

    /* LP-mode SPOP/SRANDMEMBER (copy-before-rem under listpack realloc) */
    exec_cmd(&d, T0, &out, 2, "SPOP", "s");
    DD_CHECK(out.len >= 6 && out.data[0] == '$');
    exec_cmd(&d, T0, &out, 2, "SCARD", "s");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 3, "SRANDMEMBER", "s", "2");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    exec_cmd(&d, T0, &out, 3, "SRANDMEMBER", "s", "-4");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);
    exec_cmd(&d, T0, &out, 3, "SPOP", "s", "5");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*3\r\n", 4) == 0);
    exec_cmd(&d, T0, &out, 2, "EXISTS", "s");
    EXPECT(out, ":0\r\n"); /* emptied: key deleted */

    /* LP-mode set operations */
    exec_cmd(&d, T0, &out, 5, "SADD", "s1", "1", "2", "3");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 5, "SADD", "s2", "2", "3", "4");
    EXPECT(out, ":3\r\n");
    DD_CHECK(obj_set_is_listpack(set_of(&d, "s1", 2)));
    exec_cmd(&d, T0, &out, 3, "SINTER", "s1", "s2");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    check_contains(&out, "$1\r\n2\r\n");
    check_contains(&out, "$1\r\n3\r\n");
    exec_cmd(&d, T0, &out, 3, "SUNION", "s1", "s2");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);
    exec_cmd(&d, T0, &out, 3, "SDIFF", "s1", "s2");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*1\r\n", 4) == 0);
    check_contains(&out, "$1\r\n1\r\n");
    exec_cmd(&d, T0, &out, 4, "SINTERCARD", "2", "s1", "s2");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 6, "SINTERCARD", "2", "s1", "s2", "LIMIT", "1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "SINTERSTORE", "dst", "s1", "s2");
    EXPECT(out, ":2\r\n");
    DD_CHECK(obj_set_is_listpack(set_of(&d, "dst", 3)));
    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "dst");
    check_contains(&out, "$1\r\n2\r\n");
    exec_cmd(&d, T0, &out, 4, "SUNIONSTORE", "dst", "s1", "s2");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 2, "SCARD", "dst");
    EXPECT(out, ":4\r\n");

    /* LP-mode SMOVE */
    exec_cmd(&d, T0, &out, 4, "SMOVE", "s1", "s2", "1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s2", "1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s1", "1");
    EXPECT(out, ":0\r\n");
    DD_CHECK(obj_set_is_listpack(set_of(&d, "s1", 2)));
    DD_CHECK(obj_set_is_listpack(set_of(&d, "s2", 2)));

    /* a 64-byte member still fits; 65 bytes converts to the hashtable */
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    big[64] = '\0'; /* 64-byte member */
    exec_cmd(&d, T0, &out, 3, "SADD", "s3", big);
    EXPECT(out, ":1\r\n");
    DD_CHECK(obj_set_is_listpack(set_of(&d, "s3", 2)));
    big[64] = 'y';
    big[65] = '\0'; /* 65-byte member */
    exec_cmd(&d, T0, &out, 3, "SADD", "s3", big);
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_set_is_listpack(set_of(&d, "s3", 2)));
    /* data survives the conversion */
    big[64] = '\0';
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s3", big);
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "SCARD", "s3");
    EXPECT(out, ":2\r\n");

    /* no demotion back to listpack */
    exec_cmd(&d, T0, &out, 3, "SREM", "s3", big);
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_set_is_listpack(set_of(&d, "s3", 2)));

    /* 128 members stay listpack, the 129th converts */
    for (i = 0; i < 128; i++) {
        char m[16];
        snprintf(m, sizeof(m), "m%d", i);
        exec_cmd(&d, T0, &out, 3, "SADD", "s4", m);
        EXPECT(out, ":1\r\n");
    }
    DD_CHECK(obj_set_is_listpack(set_of(&d, "s4", 2)));
    exec_cmd(&d, T0, &out, 3, "SADD", "s4", "overflow");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_set_is_listpack(set_of(&d, "s4", 2)));
    /* spot-check data after conversion */
    exec_cmd(&d, T0, &out, 2, "SCARD", "s4");
    EXPECT(out, ":129\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s4", "m0");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s4", "m127");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s4", "overflow");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "SMEMBERS", "s4");
    DD_CHECK(out.len >= 6 && memcmp(out.data, "*129\r\n", 6) == 0);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_set_listpack_limits(void)
{
    db d;
    resp_buf out;
    obj_limits saved, lim;
    db_init(&d);
    resp_buf_init(&out);

    obj_limits_get(&saved);
    lim = saved;
    lim.set_entries = 3;
    lim.set_value = 4;
    obj_limits_apply(&lim);

    /* 3 small members stay listpack at the lowered entries limit */
    exec_cmd(&d, T0, &out, 5, "SADD", "s", "a", "b", "c");
    EXPECT(out, ":3\r\n");
    DD_CHECK(obj_set_is_listpack(set_of(&d, "s", 1)));
    /* the 4th member converts; data survives */
    exec_cmd(&d, T0, &out, 3, "SADD", "s", "d");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_set_is_listpack(set_of(&d, "s", 1)));
    exec_cmd(&d, T0, &out, 3, "SISMEMBER", "s", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "SCARD", "s");
    EXPECT(out, ":4\r\n");

    /* a 5-byte member converts a fresh set at the lowered value limit */
    exec_cmd(&d, T0, &out, 3, "SADD", "s2", "12345");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_set_is_listpack(set_of(&d, "s2", 2)));

    /* entries 0 disables the compact encoding entirely */
    lim.set_entries = 0;
    lim.set_value = 64;
    obj_limits_apply(&lim);
    exec_cmd(&d, T0, &out, 3, "SADD", "s3", "x");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_set_is_listpack(set_of(&d, "s3", 2)));

    obj_limits_apply(&saved);

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_set_rejects_unrepresentable_member);
    DD_RUN(test_smove_rejection_preserves_both_sets);
    DD_RUN(test_sadd_basics);
    DD_RUN(test_srem_auto_delete);
    DD_RUN(test_spop);
    DD_RUN(test_srandmember);
    DD_RUN(test_smove);
    DD_RUN(test_set_ops);
    DD_RUN(test_set_wrongtype);
    DD_RUN(test_set_ttl_and_memory);
    DD_RUN(test_sintercard);
    DD_RUN(test_set_stores);
    DD_RUN(test_set_listpack_encoding);
    DD_RUN(test_set_listpack_limits);
    return DD_TEST_SUMMARY();
}
