/* test_hash.c - hash object commands with synthetic injected time. */
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

static void test_hash_rejects_unrepresentable_lengths(void)
{
    obj_hash *h = obj_hash_new();
    const char byte = 'x';
    uint64_t before = obj_hash_mem(h);

    DD_CHECK_EQ_INT(-1, obj_hash_set(h, &byte, 1, &byte, SIZE_MAX));
    DD_CHECK_EQ_INT(-1, obj_hash_set(h, &byte, SIZE_MAX, &byte, 1));
    DD_CHECK_EQ_INT(0, rh_size(&h->fields));
    DD_CHECK(obj_hash_mem(h) == before);
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

    /* accounting: entry + object struct + field entries */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f1", "v1");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_hash) + eb(2, 2));

    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f2", "v2");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory ==
             eb(1, 9) + sizeof(obj_hash) + eb(2, 2) + eb(2, 2));

    /* deleting a field reclaims its entry bytes */
    exec_cmd(&d, T0, &out, 3, "HDEL", "h", "f2");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_hash) + eb(2, 2));

    /* TTL works on hash keys; expiry frees the whole object */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "h", "10");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "h");
    EXPECT(out, ":10\r\n");
    before = d.used_memory;
    exec_cmd(&d, T0 + 10000, &out, 3, "HGET", "h", "f1");
    EXPECT(out, "$-1\r\n");
    DD_CHECK(d.used_memory == before - (eb(1, 9) + sizeof(obj_hash) +
                                        eb(2, 2) + eb(1, 8)));
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

    /* eviction accounts object memory too */
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory-policy",
             "allkeys-lru");
    EXPECT(out, "+OK\r\n");
    {
        char maxmem[32];
        snprintf(maxmem, sizeof(maxmem), "%llu",
                 (unsigned long long)(eb(1, 9) + sizeof(obj_hash) + eb(2, 2)));
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

int main(void)
{
    DD_RUN(test_hash_rejects_unrepresentable_lengths);
    DD_RUN(test_hset_hget);
    DD_RUN(test_hdel_auto_delete);
    DD_RUN(test_hexists_hlen_hsetnx);
    DD_RUN(test_hgetall_hkeys_hvals);
    DD_RUN(test_hmset_hmget);
    DD_RUN(test_hincrby);
    DD_RUN(test_hash_wrongtype);
    DD_RUN(test_hash_ttl_and_memory);
    return DD_TEST_SUMMARY();
}
