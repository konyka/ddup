/* test_hash.c - hash object commands with synthetic injected time. */
#include <stdarg.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
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

/* small-hash listpack payload: 7B empty frame + per-pair 6-bit-string
 * entries (1B header + payload + 1B backlen) */
static uint64_t hlp_cost(size_t flen, size_t vlen, size_t pairs)
{
    return 7 + pairs * ((1 + flen + 1) + (1 + vlen + 1));
}

/* obj_hash memory in listpack mode: struct + one malloc (16B estimate) */
static uint64_t hlp_mem(size_t flen, size_t vlen, size_t pairs)
{
    return sizeof(obj_hash) + 16 + hlp_cost(flen, vlen, pairs);
}

static void test_hash_rejects_unrepresentable_lengths(void)
{
    obj_hash *h = obj_hash_new();
    const char byte = 'x';
    uint64_t before = obj_hash_mem(h);

    DD_CHECK_EQ_INT(-1, obj_hash_set(h, &byte, 1, &byte, SIZE_MAX));
    DD_CHECK_EQ_INT(-1, obj_hash_set(h, &byte, SIZE_MAX, &byte, 1));
    DD_CHECK_EQ_INT(0, (long long)obj_hash_len(h));
    DD_CHECK(obj_hash_mem(h) == before);
    obj_hash_free(h);
}

static void test_hash_api_rejects_null_object(void)
{
    const char *value = NULL;
    size_t length = 0;
    uint64_t when = 123;
    uint64_t ttl = 123;
    DD_CHECK_EQ_INT(0, (long long)obj_hash_mem(NULL));
    DD_CHECK_EQ_INT(0, (long long)obj_hash_len(NULL));
    DD_CHECK(!obj_hash_is_listpack(NULL));
    DD_CHECK_EQ_INT(0, obj_hash_get(NULL, "f", 1, &value, &length));
    DD_CHECK_EQ_INT(-1, obj_hash_get_at(NULL, "f", 1, 0, &value, &length));
    DD_CHECK_EQ_INT(-1, obj_hash_expire_get(NULL, "f", 1, &when));
    DD_CHECK_EQ_INT(-1, obj_hash_ttl(NULL, "f", 1, 0, &ttl));
}

static void test_hash_mutation_rejects_malformed_views(void)
{
    obj_hash *h = obj_hash_new();

    DD_CHECK_EQ_INT(-1, obj_hash_set(NULL, "f", 1, "v", 1));
    DD_CHECK_EQ_INT(-1, obj_hash_set(h, NULL, 1, "v", 1));
    DD_CHECK_EQ_INT(-1, obj_hash_set(h, "f", 1, NULL, 1));
    DD_CHECK_EQ_INT(0, obj_hash_del(NULL, "f", 1));
    DD_CHECK_EQ_INT(0, obj_hash_del(h, NULL, 1));
    DD_CHECK_EQ_INT(-1, obj_hash_set_at(NULL, "f", 1, "v", 1, 0, 0));
    DD_CHECK_EQ_INT(-1, obj_hash_set_at(h, NULL, 1, "v", 1, 0, 0));
    DD_CHECK_EQ_INT(-1, obj_hash_set_at(h, "f", 1, NULL, 1, 0, 0));
    DD_CHECK_EQ_INT(0, (long long)obj_hash_len(h));

    DD_CHECK_EQ_INT(1, obj_hash_set(h, NULL, 0, NULL, 0));
    DD_CHECK_EQ_INT(1, (long long)obj_hash_len(h));
    obj_hash_free(h);
}

static void test_hash_iteration_outputs_fail_closed(void)
{
    obj_hash *h = obj_hash_new();
    const char *field = NULL;
    const char *value = NULL;
    size_t flen = 0;
    size_t vlen = 0;

    obj_hash_each(NULL, NULL, NULL);
    obj_hash_each(h, NULL, NULL);
    DD_CHECK_EQ_INT(0, obj_hash_pair_at(NULL, 0, &field, &flen, &value,
                                        &vlen));
    DD_CHECK_EQ_INT(0, obj_hash_pair_at(h, 0, NULL, &flen, &value, &vlen));
    DD_CHECK_EQ_INT(0, obj_hash_pair_at(h, 0, &field, NULL, &value, &vlen));
    DD_CHECK_EQ_INT(0, obj_hash_pair_at(h, 0, &field, &flen, NULL, &vlen));
    DD_CHECK_EQ_INT(0, obj_hash_pair_at(h, 0, &field, &flen, &value, NULL));
    obj_hash_free(h);
}

static void test_hash_expiry_wrappers_fail_closed(void)
{
    obj_hash *h = obj_hash_new();

    obj_hash_purge_expired(NULL, 0);
    DD_CHECK_EQ_INT(0, (long long)obj_hash_len_at(NULL, 0));
    obj_hash_each_at(NULL, NULL, NULL, 0);
    DD_CHECK_EQ_INT(0, (long long)obj_hash_len_at(h, 0));
    obj_hash_free(h);
}

static void test_hset_hget(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* create on write; returns count of NEW fields */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f1", "v1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f1", "v1b");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f1");
    EXPECT(out, "$3\r\nv1b\r\n");

    /* multi-pair */
    exec_cmd(&d, T0, &out, 6, "HSET", "h", "f2", "v2", "f3", "v3");
    EXPECT(out, ":2\r\n");

    /* missing field / missing key */
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "nope");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "nokey", "f1");
    EXPECT(out, "$-1\r\n");

    /* wrong arg counts */
    exec_cmd(&d, T0, &out, 3, "HSET", "h", "f");
    EXPECT(out, "-ERR wrong number of arguments for 'hset' command\r\n");
    exec_cmd(&d, T0, &out, 1, "HGET");
    EXPECT(out, "-ERR wrong number of arguments for 'hget' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hdel_auto_delete(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "HSET", "h", "a", "1", "b", "2");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "HLEN", "h");
    EXPECT(out, ":2\r\n");

    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "a");
    EXPECT(out, ":0\r\n"); /* already gone */
    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "b");
    EXPECT(out, ":1\r\n");

    /* hash became empty: the key itself is gone */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "h");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "HLEN", "h");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "HDEL", "nokey", "a");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hexists_hlen_hsetnx(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "HEXISTS", "h", "f");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "HSETNX", "h", "f", "1");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "HEXISTS", "h", "f");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "HSETNX", "h", "f", "2");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f");
    EXPECT(out, "$1\r\n1\r\n");
    exec_cmd(&d, T0, &out, 2, "HLEN", "h");
    EXPECT(out, ":1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

/* order-insensitive containment check for HGETALL/HKEYS/HVALS */
static void check_contains(const resp_buf *out, const char *bulk)
{
    char nul[512];
    DD_CHECK(out->len < sizeof(nul) - 1);
    memcpy(nul, out->data, out->len);
    nul[out->len] = '\0';
    DD_CHECK(strstr(nul, bulk) != NULL);
}

static void test_hgetall_hkeys_hvals(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "HSET", "h", "f1", "v1", "f2", "v2");
    EXPECT(out, ":2\r\n");

    exec_cmd(&d, T0, &out, 1, "HGETALL");
    EXPECT(out, "-ERR wrong number of arguments for 'hgetall' command\r\n");

    exec_cmd(&d, T0, &out, 2, "HGETALL", "h");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);
    check_contains(&out, "$2\r\nf1\r\n");
    check_contains(&out, "$2\r\nv1\r\n");
    check_contains(&out, "$2\r\nf2\r\n");
    check_contains(&out, "$2\r\nv2\r\n");

    exec_cmd(&d, T0, &out, 2, "HKEYS", "h");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    check_contains(&out, "$2\r\nf1\r\n");
    check_contains(&out, "$2\r\nf2\r\n");

    exec_cmd(&d, T0, &out, 2, "HVALS", "h");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);
    check_contains(&out, "$2\r\nv1\r\n");
    check_contains(&out, "$2\r\nv2\r\n");

    /* missing key -> empty arrays */
    exec_cmd(&d, T0, &out, 2, "HGETALL", "nokey");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 2, "HKEYS", "nokey");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 2, "HVALS", "nokey");
    EXPECT(out, "*0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hmset_hmget(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "HMSET", "h", "a", "1", "b", "2");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 5, "HMGET", "h", "a", "b", "c");
    EXPECT(out, "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$-1\r\n");
    /* HMGET on missing key -> all nulls */
    exec_cmd(&d, T0, &out, 4, "HMGET", "nokey", "a", "b");
    EXPECT(out, "*2\r\n$-1\r\n$-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hincrby(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* creates key and field */
    exec_cmd(&d, T0, &out, 4, "HINCRBY", "h", "n", "5");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBY", "h", "n", "3");
    EXPECT(out, ":8\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBY", "h", "n", "-2");
    EXPECT(out, ":6\r\n");

    /* non-integer field value */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "s", "abc");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBY", "h", "s", "1");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    /* non-integer increment */
    exec_cmd(&d, T0, &out, 4, "HINCRBY", "h", "n", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    /* overflow */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "big", "9223372036854775807");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBY", "h", "big", "1");
    EXPECT(out, "-ERR increment or decrement would overflow\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hincrbyfloat(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key and field start at 0 */
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "h", "f", "1.5");
    EXPECT(out, "$3\r\n1.5\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f");
    EXPECT(out, "$3\r\n1.5\r\n");

    exec_cmd(&d, T0, &out, 4, "HSET", "h", "g", "10");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "h", "g", "0.5");
    EXPECT(out, "$4\r\n10.5\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "h", "g", "-1.25");
    EXPECT(out, "$4\r\n9.25\r\n");

    /* non-float increment */
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "h", "g", "xyz");
    EXPECT(out, "-ERR value is not a valid float\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "h", "g", "inf");
    EXPECT(out, "-ERR value is not a valid float\r\n");

    /* non-float stored field value */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "s", "abc");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "h", "s", "1");
    EXPECT(out, "-ERR hash value is not a float\r\n");

    /* result must stay finite */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "big", "9e4931");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "h", "big", "9e4931");
    {
        long double probe = strtold("9e4931", NULL);
        if (isinf(probe))
            EXPECT(out, "-ERR value is not a valid float\r\n");
        else
            EXPECT(out,
                   "-ERR increment would produce NaN or Infinity\r\n");
    }

    /* WRONGTYPE: string key */
    exec_cmd(&d, T0, &out, 3, "SET", "str", "1");
    exec_cmd(&d, T0, &out, 4, "HINCRBYFLOAT", "str", "f", "1");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    exec_cmd(&d, T0, &out, 3, "HINCRBYFLOAT", "h", "f");
    EXPECT(out,
           "-ERR wrong number of arguments for 'hincrbyfloat' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hash_wrongtype(void)
{
    db d;
    resp_buf out;
    const char *wt =
        "-WRONGTYPE Operation against a key holding the wrong kind of "
        "value\r\n";
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    EXPECT(out, ":1\r\n");

    /* hash commands on a string key */
    exec_cmd(&d, T0, &out, 4, "HSET", "s", "f", "v");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "HGET", "s", "f");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "HLEN", "s");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "HDEL", "s", "f");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "HGETALL", "s");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "HINCRBY", "s", "f", "1");
    EXPECT(out, wt);

    /* string commands on a hash key */
    exec_cmd(&d, T0, &out, 2, "GET", "h");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "SET", "h", "v");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "INCR", "h");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "STRLEN", "h");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "APPEND", "h", "x");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "MGET", "h");
    EXPECT(out, wt);

    /* type-agnostic commands still work on hash keys */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "h");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "DEL", "h");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "h");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hash_ttl_and_memory(void)
{
    db d;
    resp_buf out;
    uint64_t before;
    db_init(&d);
    resp_buf_init(&out);

    /* accounting: entry + object struct + listpack payload */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f1", "v1");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + hlp_mem(2, 2, 1));

    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f2", "v2");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + hlp_mem(2, 2, 2));

    /* deleting a field reclaims its entry bytes */
    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "f2");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + hlp_mem(2, 2, 1));

    /* TTL works on hash keys; expiry frees the whole object */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "h", "10");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "h");
    EXPECT(out, ":10\r\n");
    before = d.used_memory;
    exec_cmd(&d, T0 + 10000, &out, 3, "HGET", "h", "f1");
    EXPECT(out, "$-1\r\n");
    DD_CHECK(d.used_memory ==
             before - (eb(1, 9) + hlp_mem(2, 2, 1) + eb(1, 8)));
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

    /* eviction accounts object memory too */
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory-policy",
             "allkeys-lru");
    EXPECT(out, "+OK\r\n");
    {
        char maxmem[32];
        snprintf(maxmem, sizeof(maxmem), "%llu",
                 (unsigned long long)(eb(1, 9) + hlp_mem(2, 2, 1)));
        exec_cmd(&d, T0, &out, 6, "HSET", "big", "f1", "v1", "f2", "v2");
        EXPECT(out, ":2\r\n");
        exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory", maxmem);
        EXPECT(out, "+OK\r\n");
        DD_CHECK(d.used_memory <= d.maxmemory);
        DD_CHECK((long long)d.evicted_keys >= 1);
    }

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hstrlen(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key / missing field -> 0 */
    exec_cmd(&d, T0, &out, 3, "HSTRLEN", "nokey", "f");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, T0, &out, 6, "HSET", "h", "f", "hello", "empty", "");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "HSTRLEN", "h", "f");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 3, "HSTRLEN", "h", "empty");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "HSTRLEN", "h", "nofield");
    EXPECT(out, ":0\r\n");

    /* wrong arg counts */
    exec_cmd(&d, T0, &out, 2, "HSTRLEN", "h");
    EXPECT(out, "-ERR wrong number of arguments for 'hstrlen' command\r\n");
    exec_cmd(&d, T0, &out, 4, "HSTRLEN", "h", "f", "x");
    EXPECT(out, "-ERR wrong number of arguments for 'hstrlen' command\r\n");

    /* wrong type */
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "HSTRLEN", "s", "f");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hrandfield(void)
{
    db d;
    resp_buf out;
    int i;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key: null bulk without count, empty array with count */
    exec_cmd(&d, T0, &out, 2, "HRANDFIELD", "nokey");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "HRANDFIELD", "nokey", "3");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "HRANDFIELD", "nokey", "-2", "WITHVALUES");
    EXPECT(out, "*0\r\n");

    exec_cmd(&d, T0, &out, 8, "HSET", "h", "a", "1", "b", "2", "c", "3");
    EXPECT(out, ":3\r\n");

    /* single field: one of the fields, hash untouched */
    exec_cmd(&d, T0, &out, 2, "HRANDFIELD", "h");
    DD_CHECK(out.len == 7 && out.data[0] == '$');
    DD_CHECK(out.data[4] == 'a' || out.data[4] == 'b' || out.data[4] == 'c');
    exec_cmd(&d, T0, &out, 2, "HLEN", "h");
    EXPECT(out, ":3\r\n");

    /* positive count: distinct fields, at most count */
    exec_cmd(&d, T0, &out, 3, "HRANDFIELD", "h", "2");
    DD_CHECK(out.len == 18 && memcmp(out.data, "*2\r\n", 4) == 0);
    DD_CHECK(out.data[8] != out.data[16]);
    /* count >= hlen: every field exactly once */
    exec_cmd(&d, T0, &out, 3, "HRANDFIELD", "h", "10");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*3\r\n", 4) == 0);
    check_contains(&out, "$1\r\na\r\n");
    check_contains(&out, "$1\r\nb\r\n");
    check_contains(&out, "$1\r\nc\r\n");
    /* count 0 -> empty array */
    exec_cmd(&d, T0, &out, 3, "HRANDFIELD", "h", "0");
    EXPECT(out, "*0\r\n");

    /* negative count: exactly |count|, repeats allowed */
    exec_cmd(&d, T0, &out, 3, "HRANDFIELD", "h", "-5");
    DD_CHECK(out.len == 4 + 5 * 7 && memcmp(out.data, "*5\r\n", 4) == 0);
    for (i = 0; i < 5; i++)
        DD_CHECK(out.data[8 + i * 7] == 'a' || out.data[8 + i * 7] == 'b' ||
                 out.data[8 + i * 7] == 'c');

    /* WITHVALUES: flat field/value pairs (all fields when count >= hlen) */
    exec_cmd(&d, T0, &out, 4, "HRANDFIELD", "h", "3", "WITHVALUES");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*6\r\n", 4) == 0);
    check_contains(&out, "$1\r\na\r\n$1\r\n1\r\n");
    check_contains(&out, "$1\r\nb\r\n$1\r\n2\r\n");
    check_contains(&out, "$1\r\nc\r\n$1\r\n3\r\n");
    /* negative count with values: |count| pairs */
    exec_cmd(&d, T0, &out, 4, "HRANDFIELD", "h", "-2", "WITHVALUES");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*4\r\n", 4) == 0);
    /* lowercase option accepted */
    exec_cmd(&d, T0, &out, 4, "HRANDFIELD", "h", "1", "withvalues");
    DD_CHECK(out.len >= 4 && memcmp(out.data, "*2\r\n", 4) == 0);

    /* errors */
    exec_cmd(&d, T0, &out, 3, "HRANDFIELD", "h", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 4, "HRANDFIELD", "h", "2", "BADOPT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 1, "HRANDFIELD");
    EXPECT(out,
           "-ERR wrong number of arguments for 'hrandfield' command\r\n");
    exec_cmd(&d, T0, &out, 5, "HRANDFIELD", "h", "1", "WITHVALUES", "x");
    EXPECT(out,
           "-ERR wrong number of arguments for 'hrandfield' command\r\n");

    /* wrong type */
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "HRANDFIELD", "s");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static obj_hash *hash_of(db *d, const char *k, size_t kl)
{
    const char *blob;
    size_t bloblen;
    if (rh_get(&d->table, k, kl, &blob, &bloblen) != 1)
        return NULL;
    return (obj_hash *)obj_unpack_ptr(blob, bloblen);
}

static void test_hash_listpack_encoding(void)
{
    db d;
    resp_buf out;
    char big[70];
    int i;
    db_init(&d);
    resp_buf_init(&out);

    /* small hash starts as listpack */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f1", "v1");
    EXPECT(out, ":1\r\n");
    DD_CHECK(obj_hash_is_listpack(hash_of(&d, "h", 1)));

    /* int-looking fields/values roundtrip through int entries */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "123", "456");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "123");
    EXPECT(out, "$3\r\n456\r\n");
    exec_cmd(&d, T0, &out, 3, "HSTRLEN", "h", "123");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 2, "HGETALL", "h");
    check_contains(&out, "$3\r\n123\r\n");
    check_contains(&out, "$3\r\n456\r\n");
    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "123");
    EXPECT(out, ":1\r\n");
    DD_CHECK(obj_hash_is_listpack(hash_of(&d, "h", 1)));

    /* a 64-byte value still fits; 65 bytes converts to the hashtable */
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    big[64] = '\0'; /* 64-byte value */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f64", big);
    EXPECT(out, ":1\r\n");
    DD_CHECK(obj_hash_is_listpack(hash_of(&d, "h", 1)));
    big[64] = 'y';
    big[65] = '\0'; /* 65-byte value */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f65", big);
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_hash_is_listpack(hash_of(&d, "h", 1)));
    /* data survives the conversion */
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f1");
    EXPECT(out, "$2\r\nv1\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f65");
    DD_CHECK(out.len == 72 && memcmp(out.data, "$65\r\n", 5) == 0);
    exec_cmd(&d, T0, &out, 2, "HLEN", "h");
    EXPECT(out, ":3\r\n");

    /* no demotion back to listpack */
    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "f65");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "f64");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_hash_is_listpack(hash_of(&d, "h", 1)));

    /* 128 fields stay listpack, the 129th converts */
    exec_cmd(&d, T0, &out, 2, "DEL", "h");
    EXPECT(out, ":1\r\n");
    for (i = 0; i < 128; i++) {
        char f[16], v[16];
        snprintf(f, sizeof(f), "f%d", i);
        snprintf(v, sizeof(v), "v%d", i);
        exec_cmd(&d, T0, &out, 4, "HSET", "h", f, v);
    }
    DD_CHECK(obj_hash_is_listpack(hash_of(&d, "h", 1)));
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "overflow", "v");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_hash_is_listpack(hash_of(&d, "h", 1)));
    exec_cmd(&d, T0, &out, 2, "HLEN", "h");
    EXPECT(out, ":129\r\n");
    /* spot-check data after conversion */
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f0");
    EXPECT(out, "$2\r\nv0\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f127");
    EXPECT(out, "$4\r\nv127\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "overflow");
    EXPECT(out, "$1\r\nv\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hash_listpack_limits(void)
{
    db d;
    resp_buf out;
    obj_limits saved, lim, chk;
    db_init(&d);
    resp_buf_init(&out);

    obj_limits_get(&saved);
    lim = saved;
    lim.hash_entries = 3;
    lim.hash_value = 4;
    obj_limits_apply(&lim);

    /* 3 small fields stay listpack at the lowered entries limit */
    exec_cmd(&d, T0, &out, 6, "HSET", "h", "f1", "v1", "f2", "v2");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f3", "v3");
    EXPECT(out, ":1\r\n");
    DD_CHECK(obj_hash_is_listpack(hash_of(&d, "h", 1)));
    /* the 4th field converts; data survives */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f4", "v4");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_hash_is_listpack(hash_of(&d, "h", 1)));
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f1");
    EXPECT(out, "$2\r\nv1\r\n");

    /* a 5-byte value converts a fresh hash at the lowered value limit */
    exec_cmd(&d, T0, &out, 4, "HSET", "h2", "f", "12345");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_hash_is_listpack(hash_of(&d, "h2", 2)));

    /* entries 0 disables the compact encoding entirely */
    lim.hash_entries = 0;
    lim.hash_value = 64;
    obj_limits_apply(&lim);
    exec_cmd(&d, T0, &out, 4, "HSET", "h3", "f", "v");
    EXPECT(out, ":1\r\n");
    DD_CHECK(!obj_hash_is_listpack(hash_of(&d, "h3", 2)));

    /* restore process-wide defaults */
    obj_limits_apply(&saved);
    obj_limits_get(&chk);
    DD_CHECK_EQ_INT(saved.hash_entries, chk.hash_entries);
    DD_CHECK_EQ_INT(saved.hash_value, chk.hash_value);

    resp_buf_free(&out);
    db_destroy(&d);
}


static void test_hgetdel(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 8, "HSET", "h", "f1", "v1", "f2", "v2", "f3", "v3");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 6, "HGETDEL", "h", "FIELDS", "2", "f1", "f2");
    EXPECT(out, "*2\r\n$2\r\nv1\r\n$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 2, "HLEN", "h");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 6, "HGETDEL", "h", "FIELDS", "2", "f1", "f3");
    EXPECT(out, "*2\r\n$-1\r\n$2\r\nv3\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "h");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, T0, &out, 5, "HGETDEL", "no", "FIELDS", "1", "f");
    EXPECT(out, "*1\r\n$-1\r\n");
    exec_cmd(&d, T0, &out, 4, "HGETDEL", "h", "FIELDS", "x");
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hash_field_ttl_commands(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 8, "HSET", "h", "f1", "v1", "f2", "v2", "f3", "v3");
    EXPECT(out, ":3\r\n");

    /* Missing key returns NO_FIELD (-2) for every requested field. */
    exec_cmd(&d, T0, &out, 6, "HEXPIRE", "no", "100", "FIELDS", "1", "f");
    EXPECT(out, "*1\r\n:-2\r\n");

    exec_cmd(&d, T0, &out, 7, "HEXPIRE", "h", "100", "FIELDS", "2", "f1", "f2");
    EXPECT(out, "*2\r\n:1\r\n:1\r\n");
    exec_cmd(&d, T0, &out, 7, "HTTL", "h", "FIELDS", "3", "f1", "f2", "f3");
    EXPECT(out, "*3\r\n:100\r\n:100\r\n:-1\r\n");
    exec_cmd(&d, T0, &out, 7, "HPTTL", "h", "FIELDS", "3", "f1", "f2", "f3");
    EXPECT(out, "*3\r\n:100000\r\n:100000\r\n:-1\r\n");
    exec_cmd(&d, T0, &out, 6, "HPERSIST", "h", "FIELDS", "2", "f1", "f3");
    EXPECT(out, "*2\r\n:1\r\n:-1\r\n");
    exec_cmd(&d, T0, &out, 5, "HTTL", "h", "FIELDS", "1", "f1");
    EXPECT(out, "*1\r\n:-1\r\n");

    /* Expired fields disappear lazily and the key is removed when empty. */
    exec_cmd(&d, T0, &out, 6, "HEXPIRE", "h", "1", "FIELDS", "1", "f3");
    EXPECT(out, "*1\r\n:1\r\n");
    exec_cmd(&d, T0 + 2000, &out, 3, "HGET", "h", "f3");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0 + 2000, &out, 2, "HLEN", "h");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0 + 2000, &out, 3, "HDEL", "h", "f1");
    exec_cmd(&d, T0 + 2000, &out, 3, "HDEL", "h", "f2");
    exec_cmd(&d, T0 + 2000, &out, 2, "EXISTS", "h");
    EXPECT(out, ":0\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hsetex_hgetex(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 10, "HSETEX", "h", "EX", "10", "FIELDS", "2", "f1", "v1", "f2", "v2");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 6, "HTTL", "h", "FIELDS", "2", "f1", "f2");
    EXPECT(out, "*2\r\n:10\r\n:10\r\n");

    exec_cmd(&d, T0, &out, 9, "HSETEX", "h", "FXX", "FIELDS", "2", "f1", "x", "f9", "x");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h", "f1");
    EXPECT(out, "$2\r\nv1\r\n");

    exec_cmd(&d, T0, &out, 7, "HGETEX", "h", "PERSIST", "FIELDS", "2", "f1", "f9");
    EXPECT(out, "*2\r\n$2\r\nv1\r\n$-1\r\n");
    exec_cmd(&d, T0, &out, 5, "HTTL", "h", "FIELDS", "1", "f1");
    EXPECT(out, "*1\r\n:-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}
static void test_hscan(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 8, "HSET", "h", "f1", "v1", "f2", "v2", "f3", "v3");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 3, "HSCAN", "h", "0");
    EXPECT(out, "*2\r\n$1\r\n0\r\n*6\r\n$2\r\nf1\r\n$2\r\nv1\r\n$2\r\nf2\r\n$2\r\nv2\r\n$2\r\nf3\r\n$2\r\nv3\r\n");

    exec_cmd(&d, T0, &out, 5, "HSCAN", "h", "0", "COUNT", "2");
    EXPECT(out, "*2\r\n$1\r\n2\r\n*4\r\n$2\r\nf1\r\n$2\r\nv1\r\n$2\r\nf2\r\n$2\r\nv2\r\n");
    exec_cmd(&d, T0, &out, 3, "HSCAN", "h", "2");
    EXPECT(out, "*2\r\n$1\r\n0\r\n*2\r\n$2\r\nf3\r\n$2\r\nv3\r\n");

    exec_cmd(&d, T0, &out, 5, "HSCAN", "h", "0", "MATCH", "f[13]");
    EXPECT(out, "*2\r\n$1\r\n0\r\n*4\r\n$2\r\nf1\r\n$2\r\nv1\r\n$2\r\nf3\r\n$2\r\nv3\r\n");

    {
        obj_limits saved, forced;
        obj_limits_get(&saved);
        forced = saved;
        forced.hash_entries = 0;
        obj_limits_apply(&forced);
        exec_cmd(&d, T0, &out, 4, "HSET", "h2", "f", "v");
        exec_cmd(&d, T0, &out, 3, "HSCAN", "h2", "0");
        EXPECT(out, "*2\r\n$1\r\n0\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n");
        obj_limits_apply(&saved);
    }

    exec_cmd(&d, T0, &out, 3, "HSCAN", "missing", "0");
    EXPECT(out, "*2\r\n$1\r\n0\r\n*0\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    exec_cmd(&d, T0, &out, 3, "HSCAN", "s", "0");
    EXPECT(out, "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_hash_rejects_unrepresentable_lengths);
    DD_RUN(test_hash_api_rejects_null_object);
    DD_RUN(test_hash_mutation_rejects_malformed_views);
    DD_RUN(test_hash_iteration_outputs_fail_closed);
    DD_RUN(test_hash_expiry_wrappers_fail_closed);
    DD_RUN(test_hset_hget);
    DD_RUN(test_hdel_auto_delete);
    DD_RUN(test_hexists_hlen_hsetnx);
    DD_RUN(test_hgetall_hkeys_hvals);
    DD_RUN(test_hmset_hmget);
    DD_RUN(test_hincrby);
    DD_RUN(test_hincrbyfloat);
    DD_RUN(test_hash_wrongtype);
    DD_RUN(test_hash_ttl_and_memory);
    DD_RUN(test_hstrlen);
    DD_RUN(test_hrandfield);
    DD_RUN(test_hash_listpack_encoding);
    DD_RUN(test_hash_listpack_limits);
    DD_RUN(test_hscan);
    DD_RUN(test_hgetdel);
    DD_RUN(test_hash_field_ttl_commands);
    DD_RUN(test_hsetex_hgetex);
    return DD_TEST_SUMMARY();
}
