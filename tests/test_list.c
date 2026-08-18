/* test_list.c - list object commands with synthetic injected time. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "ds/obj.h"
#include "ds/quicklist.h"
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

/* quicklist node cost: node struct + 16B malloc estimate + the listpack
 * (7-byte empty frame + one 6-bit-string entry per element) */
static uint64_t ql_cost(const size_t *lens, size_t n)
{
    uint64_t c = (uint64_t)sizeof(ql_node) + 16 + 7;
    size_t i;
    for (i = 0; i < n; i++)
        c += 1 + lens[i] + 1; /* 6-bit str header + payload + backlen */
    return c;
}

static void test_list_rejects_unrepresentable_elements(void)
{
    obj_list *l = obj_list_new();
    const char byte = 'x';
    obj_list_iter it;
    uint64_t before;

    DD_CHECK_EQ_INT(0, obj_list_push(l, 0, "old", 3));
    before = obj_list_mem(l);
    DD_CHECK_EQ_INT(-1, obj_list_push(l, 1, &byte, SIZE_MAX));
    DD_CHECK_EQ_INT(-1, obj_list_set_at(l, 0, &byte, SIZE_MAX));
    DD_CHECK_EQ_INT(1, (long long)obj_list_len(l));
    DD_CHECK(obj_list_mem(l) == before);
    DD_CHECK_EQ_INT(1, obj_list_first(l, &it));
    {
        size_t vl = 0;
        const char *v = obj_list_iter_value(&it, &vl);
        DD_CHECK_MEM("old", 3, v, vl);
    }
    obj_list_free(l);
}

static void test_list_commands_reject_transactionally(void)
{
    db d;
    resp_buf out;
    resp_value argv[4];
    const char byte = 'x';
    const char *blob;
    size_t bloblen;
    obj_list *l;
    uint64_t used, dirty, version, list_mem;

    db_init(&d);
    resp_buf_init(&out);
    exec_cmd(&d, T0, &out, 4, "RPUSH", "l", "old", "tail");
    d.watch_refs = 1;
    used = d.used_memory;
    dirty = d.dirty;
    version = db_key_version(&d, "l", 1);
    DD_CHECK(rh_get(&d.table, "l", 1, &blob, &bloblen) == 1);
    l = (obj_list *)obj_unpack_ptr(blob, bloblen);
    list_mem = obj_list_mem(l);

    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING;
    argv[0].str = "LPUSH";
    argv[0].len = 5;
    argv[1].type = RESP_BULK_STRING;
    argv[1].str = "l";
    argv[1].len = 1;
    argv[2].type = RESP_BULK_STRING;
    argv[2].str = "valid";
    argv[2].len = 5;
    argv[3].type = RESP_BULK_STRING;
    argv[3].str = &byte;
    argv[3].len = SIZE_MAX;
    out.len = 0;
    command_execute_at(&d, argv, 4, &out, T0);
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    argv[0].str = "LSET";
    argv[0].len = 4;
    argv[2].str = "0";
    argv[2].len = 1;
    out.len = 0;
    command_execute_at(&d, argv, 4, &out, T0);
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    DD_CHECK_EQ_INT(2, (long long)obj_list_len(l));
    DD_CHECK(obj_list_mem(l) == list_mem);
    DD_CHECK(d.used_memory == used);
    DD_CHECK(d.dirty == dirty);
    DD_CHECK(db_key_version(&d, "l", 1) == version);
    {
        obj_list_iter it;
        size_t vl = 0;
        const char *v;
        DD_CHECK_EQ_INT(1, obj_list_first(l, &it));
        v = obj_list_iter_value(&it, &vl);
        DD_CHECK_MEM("old", 3, v, vl);
        DD_CHECK_EQ_INT(1, obj_list_last(l, &it));
        v = obj_list_iter_value(&it, &vl);
        DD_CHECK_MEM("tail", 4, v, vl);
    }

    d.watch_refs = 0;
    resp_buf_free(&out);
    db_destroy(&d);
}

static void fill_abcd(db *d, resp_buf *out)
{
    /* list: c b a (LPUSH order), then c b a d */
    exec_cmd(d, T0, out, 5, "LPUSH", "l", "a", "b", "c");
    EXPECT(*out, ":3\r\n");
    exec_cmd(d, T0, out, 3, "RPUSH", "l", "d");
    EXPECT(*out, ":4\r\n");
}

static void test_push_len_range(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "LPUSH", "l", "a");
    EXPECT(out, ":1\r\n");
    /* LPUSH a b c pushes each to the head in order: c b a a */
    exec_cmd(&d, T0, &out, 5, "LPUSH", "l", "a", "b", "c");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "l", "d");
    EXPECT(out, ":5\r\n");

    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*5\r\n$1\r\nc\r\n$1\r\nb\r\n$1\r\na\r\n$1\r\na\r\n$1\r\nd\r\n");
    exec_cmd(&d, T0, &out, 2, "LLEN", "l");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 2, "LLEN", "nokey");
    EXPECT(out, ":0\r\n");

    /* wrong arg counts */
    exec_cmd(&d, T0, &out, 2, "LPUSH", "l");
    EXPECT(out, "-ERR wrong number of arguments for 'lpush' command\r\n");
    exec_cmd(&d, T0, &out, 3, "LRANGE", "l", "0");
    EXPECT(out, "-ERR wrong number of arguments for 'lrange' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_lrange_index_math(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "l", "a", "b", "c");
    EXPECT(out, ":3\r\n");

    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "100");
    EXPECT(out, "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "-2", "-1");
    EXPECT(out, "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "1", "1");
    EXPECT(out, "*1\r\n$1\r\nb\r\n");
    /* out of range / inverted -> empty */
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "5", "10");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "2", "1");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "-100", "-100");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "-100", "1");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    /* missing key -> empty */
    exec_cmd(&d, T0, &out, 4, "LRANGE", "nokey", "0", "-1");
    EXPECT(out, "*0\r\n");
    /* non-integer indexes */
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "x", "1");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_lindex_lset(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "l", "a", "b", "c");
    EXPECT(out, ":3\r\n");

    exec_cmd(&d, T0, &out, 3, "LINDEX", "l", "0");
    EXPECT(out, "$1\r\na\r\n");
    exec_cmd(&d, T0, &out, 3, "LINDEX", "l", "-1");
    EXPECT(out, "$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 3, "LINDEX", "l", "3");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "LINDEX", "l", "-4");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 3, "LINDEX", "nokey", "0");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, T0, &out, 4, "LSET", "l", "1", "B");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "LINDEX", "l", "1");
    EXPECT(out, "$1\r\nB\r\n");
    exec_cmd(&d, T0, &out, 4, "LSET", "l", "-1", "C");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\na\r\n$1\r\nB\r\n$1\r\nC\r\n");
    exec_cmd(&d, T0, &out, 4, "LSET", "l", "3", "x");
    EXPECT(out, "-ERR index out of range\r\n");
    exec_cmd(&d, T0, &out, 4, "LSET", "nokey", "0", "x");
    EXPECT(out, "-ERR no such key\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_pop_auto_delete(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    fill_abcd(&d, &out); /* c b a d */
    exec_cmd(&d, T0, &out, 2, "LPOP", "l");
    EXPECT(out, "$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 2, "RPOP", "l");
    EXPECT(out, "$1\r\nd\r\n");
    exec_cmd(&d, T0, &out, 2, "LPOP", "l");
    EXPECT(out, "$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 2, "RPOP", "l");
    EXPECT(out, "$1\r\na\r\n");

    /* list is empty: the key itself is gone */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "l");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "LPOP", "l");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "RPOP", "nokey");
    EXPECT(out, "$-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_pushx(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* no create on missing key */
    exec_cmd(&d, T0, &out, 3, "LPUSHX", "l", "a");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSHX", "l", "a");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "l");
    EXPECT(out, ":0\r\n");

    exec_cmd(&d, T0, &out, 3, "RPUSH", "l", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "LPUSHX", "l", "h");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSHX", "l", "t");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\nh\r\n$1\r\na\r\n$1\r\nt\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_list_wrongtype(void)
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
    exec_cmd(&d, T0, &out, 3, "LPUSH", "l", "a");
    EXPECT(out, ":1\r\n");

    /* list commands on a string key */
    exec_cmd(&d, T0, &out, 3, "LPUSH", "s", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 3, "RPUSHX", "s", "a");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "LPOP", "s");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "LLEN", "s");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "LRANGE", "s", "0", "-1");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "LSET", "s", "0", "x");
    EXPECT(out, wt);

    /* string and hash commands on a list key */
    exec_cmd(&d, T0, &out, 2, "GET", "l");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 4, "HSET", "l", "f", "v");
    EXPECT(out, wt);
    exec_cmd(&d, T0, &out, 2, "HLEN", "l");
    EXPECT(out, wt);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_list_ttl_and_memory(void)
{
    db d;
    resp_buf out;
    uint64_t before;
    const size_t lens_a[] = {1};
    const size_t lens_abc[] = {1, 2, 3};
    const size_t lens_bc[] = {2, 3};
    db_init(&d);
    resp_buf_init(&out);

    /* accounting: entry + list struct + quicklist node cost */
    exec_cmd(&d, T0, &out, 3, "LPUSH", "l", "a");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_list) + ql_cost(lens_a, 1));

    exec_cmd(&d, T0, &out, 4, "RPUSH", "l", "bb", "ccc");
    EXPECT(out, ":3\r\n");
    DD_CHECK(d.used_memory ==
             eb(1, 9) + sizeof(obj_list) + ql_cost(lens_abc, 3));

    exec_cmd(&d, T0, &out, 2, "LPOP", "l");
    EXPECT(out, "$1\r\na\r\n");
    DD_CHECK(d.used_memory ==
             eb(1, 9) + sizeof(obj_list) + ql_cost(lens_bc, 2));

    /* TTL works on list keys; expiry frees the whole object */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "l", "10");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "l");
    EXPECT(out, ":10\r\n");
    before = d.used_memory;
    exec_cmd(&d, T0 + 10000, &out, 2, "LLEN", "l");
    EXPECT(out, ":0\r\n");
    DD_CHECK(d.used_memory ==
             before - (eb(1, 9) + sizeof(obj_list) + ql_cost(lens_bc, 2) +
                       eb(1, 8)));
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_lpos(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    /* missing key: null without COUNT, empty array with COUNT */
    exec_cmd(&d, T0, &out, 3, "LPOS", "nokey", "a");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "nokey", "a", "COUNT", "0");
    EXPECT(out, "*0\r\n");

    exec_cmd(&d, T0, &out, 7, "RPUSH", "l", "a", "b", "c", "a", "b");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "l", "a");
    EXPECT(out, ":6\r\n"); /* a b c a b a */

    /* plain form: first match from the head, integer or null */
    exec_cmd(&d, T0, &out, 3, "LPOS", "l", "a");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 3, "LPOS", "l", "c");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "LPOS", "l", "x");
    EXPECT(out, "$-1\r\n");

    /* RANK: positive from head, negative from tail */
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "RANK", "2");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "RANK", "-1");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "RANK", "-2");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "RANK", "4");
    EXPECT(out, "$-1\r\n"); /* only 3 matches */

    /* COUNT: array form; 0 = all matches, N = at most N */
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "COUNT", "0");
    EXPECT(out, "*3\r\n:0\r\n:3\r\n:5\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "COUNT", "2");
    EXPECT(out, "*2\r\n:0\r\n:3\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "COUNT", "1");
    EXPECT(out, "*1\r\n:0\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "x", "COUNT", "0");
    EXPECT(out, "*0\r\n");
    exec_cmd(&d, T0, &out, 7, "LPOS", "l", "a", "RANK", "-1", "COUNT", "2");
    EXPECT(out, "*2\r\n:5\r\n:3\r\n");

    /* MAXLEN caps the number of comparisons */
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "b", "MAXLEN", "1");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "b", "MAXLEN", "2");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 7, "LPOS", "l", "a", "COUNT", "0", "MAXLEN", "4");
    EXPECT(out, "*2\r\n:0\r\n:3\r\n");
    exec_cmd(&d, T0, &out, 7, "LPOS", "l", "a", "RANK", "-1", "MAXLEN", "0");
    EXPECT(out, ":5\r\n"); /* 0 = unlimited */

    /* errors */
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "RANK", "0");
    EXPECT(out, "-ERR RANK can't be zero\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "COUNT", "-1");
    EXPECT(out, "-ERR COUNT can't be negative\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "MAXLEN", "-1");
    EXPECT(out, "-ERR MAXLEN can't be negative\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "RANK", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 5, "LPOS", "l", "a", "BOGUS", "1");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "LPOS", "l", "a", "RANK");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 2, "LPOS", "l");
    EXPECT(out, "-ERR wrong number of arguments for 'lpos' command\r\n");

    /* wrong type */
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "LPOS", "s", "a");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_lrem(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "RPUSH", "l", "a", "b", "a", "c");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "l", "a");
    EXPECT(out, ":5\r\n"); /* a b a c a */

    /* count > 0: from the head, at most count */
    exec_cmd(&d, T0, &out, 4, "LREM", "l", "1", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*4\r\n$1\r\nb\r\n$1\r\na\r\n$1\r\nc\r\n$1\r\na\r\n");
    /* count < 0: from the tail */
    exec_cmd(&d, T0, &out, 4, "LREM", "l", "-1", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\nb\r\n$1\r\na\r\n$1\r\nc\r\n");
    /* count 0: all remaining matches */
    exec_cmd(&d, T0, &out, 4, "LREM", "l", "0", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\nb\r\n$1\r\nc\r\n");
    /* no match / missing key */
    exec_cmd(&d, T0, &out, 4, "LREM", "l", "0", "x");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "LREM", "nokey", "0", "a");
    EXPECT(out, ":0\r\n");

    /* removing the last elements deletes the key */
    exec_cmd(&d, T0, &out, 4, "LREM", "l", "2", "b");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 4, "LREM", "l", "0", "c");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "l");
    EXPECT(out, ":0\r\n");

    /* errors */
    exec_cmd(&d, T0, &out, 4, "LREM", "l", "x", "a");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 3, "LREM", "l", "1");
    EXPECT(out, "-ERR wrong number of arguments for 'lrem' command\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "LREM", "s", "0", "a");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_ltrim(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 6, "RPUSH", "l", "a", "b", "c", "d");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "l", "e");
    EXPECT(out, ":5\r\n"); /* a b c d e */

    exec_cmd(&d, T0, &out, 4, "LTRIM", "l", "1", "3");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\nb\r\n$1\r\nc\r\n$1\r\nd\r\n");
    /* negative indexes, same math as LRANGE */
    exec_cmd(&d, T0, &out, 4, "LTRIM", "l", "-2", "-1");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\nc\r\n$1\r\nd\r\n");
    /* whole range kept */
    exec_cmd(&d, T0, &out, 4, "LTRIM", "l", "0", "-1");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "LLEN", "l");
    EXPECT(out, ":2\r\n");
    /* inverted range: everything dropped, key deleted */
    exec_cmd(&d, T0, &out, 4, "LTRIM", "l", "1", "0");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "l");
    EXPECT(out, ":0\r\n");

    /* out-of-range start: key deleted too; missing key is a no-op OK */
    exec_cmd(&d, T0, &out, 4, "RPUSH", "m", "x", "y");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 4, "LTRIM", "m", "5", "10");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "m");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "LTRIM", "nokey", "0", "-1");
    EXPECT(out, "+OK\r\n");

    /* errors */
    exec_cmd(&d, T0, &out, 4, "LTRIM", "l", "x", "1");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 3, "LTRIM", "l", "0");
    EXPECT(out, "-ERR wrong number of arguments for 'ltrim' command\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 4, "LTRIM", "s", "0", "-1");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_rpoplpush(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 4, "RPUSH", "src", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "src", "c");
    EXPECT(out, ":3\r\n"); /* a b c */

    /* tail of src -> head of dst (dst created) */
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "src", "dst");
    EXPECT(out, "$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "src", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "dst", "0", "-1");
    EXPECT(out, "*1\r\n$1\r\nc\r\n");

    /* src == dst: rotate (tail moves to head) */
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "src", "src");
    EXPECT(out, "$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "src", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\nb\r\n$1\r\na\r\n");

    /* draining the source deletes its key (src is b a after the rotate) */
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "src", "dst");
    EXPECT(out, "$1\r\na\r\n");
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "src", "dst");
    EXPECT(out, "$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "src");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "dst", "0", "-1");
    EXPECT(out, "*3\r\n$1\r\nb\r\n$1\r\na\r\n$1\r\nc\r\n");

    /* missing source: null bulk, dst untouched */
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "src", "dst");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "LLEN", "dst");
    EXPECT(out, ":3\r\n");

    /* wrong types: either side rejects, nothing mutated */
    exec_cmd(&d, T0, &out, 3, "SET", "str", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "dst", "str");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");
    exec_cmd(&d, T0, &out, 2, "LLEN", "dst");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "str", "dst");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    /* arity */
    exec_cmd(&d, T0, &out, 2, "RPOPLPUSH", "dst");
    EXPECT(out,
           "-ERR wrong number of arguments for 'rpoplpush' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_rpoplpush_bumps_both_key_versions(void)
{
    db d;
    resp_buf out;
    uint64_t vsrc, vdst;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "RPUSH", "src", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "dst", "z");
    EXPECT(out, ":1\r\n");
    d.watch_refs = 1;
    vsrc = db_key_version(&d, "src", 3);
    vdst = db_key_version(&d, "dst", 3);
    exec_cmd(&d, T0, &out, 3, "RPOPLPUSH", "src", "dst");
    EXPECT(out, "$1\r\na\r\n");
    DD_CHECK(db_key_version(&d, "src", 3) > vsrc);
    DD_CHECK(db_key_version(&d, "dst", 3) > vdst);
    d.watch_refs = 0;

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_linsert(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "l", "a", "b", "c");
    EXPECT(out, ":3\r\n");

    exec_cmd(&d, T0, &out, 5, "LINSERT", "l", "BEFORE", "b", "x");
    EXPECT(out, ":4\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*4\r\n$1\r\na\r\n$1\r\nx\r\n$1\r\nb\r\n$1\r\nc\r\n");

    exec_cmd(&d, T0, &out, 5, "LINSERT", "l", "after", "b", "y");
    EXPECT(out, ":5\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out,
           "*5\r\n$1\r\na\r\n$1\r\nx\r\n$1\r\nb\r\n$1\r\ny\r\n"
           "$1\r\nc\r\n");

    exec_cmd(&d, T0, &out, 5, "LINSERT", "l", "before", "a", "z");
    EXPECT(out, ":6\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out,
           "*6\r\n$1\r\nz\r\n$1\r\na\r\n$1\r\nx\r\n$1\r\nb\r\n"
           "$1\r\ny\r\n$1\r\nc\r\n");

    /* missing key */
    exec_cmd(&d, T0, &out, 5, "LINSERT", "nokey", "BEFORE", "a", "x");
    EXPECT(out, ":0\r\n");

    /* pivot not found */
    exec_cmd(&d, T0, &out, 5, "LINSERT", "l", "BEFORE", "nope", "x");
    EXPECT(out, ":-1\r\n");

    /* wrong type and syntax errors */
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 5, "LINSERT", "s", "BEFORE", "a", "x");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");
    exec_cmd(&d, T0, &out, 5, "LINSERT", "l", "BEFOR", "a", "x");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "LINSERT", "l", "BEFORE", "a");
    EXPECT(out,
           "-ERR wrong number of arguments for 'linsert' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_lmove(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "src", "a", "b", "c");
    EXPECT(out, ":3\r\n");

    /* RIGHT -> LEFT: tail of src becomes head of dst */
    exec_cmd(&d, T0, &out, 5, "LMOVE", "src", "dst", "RIGHT", "LEFT");
    EXPECT(out, "$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "src", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "dst", "0", "-1");
    EXPECT(out, "*1\r\n$1\r\nc\r\n");

    /* LEFT -> RIGHT: head of src becomes tail of dst */
    exec_cmd(&d, T0, &out, 5, "LMOVE", "src", "dst", "left", "right");
    EXPECT(out, "$1\r\na\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "src", "0", "-1");
    EXPECT(out, "*1\r\n$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "dst", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\nc\r\n$1\r\na\r\n");

    /* same key rotates */
    exec_cmd(&d, T0, &out, 4, "RPUSH", "rot", "x", "y");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 5, "LMOVE", "rot", "rot", "RIGHT", "LEFT");
    EXPECT(out, "$1\r\ny\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "rot", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\ny\r\n$1\r\nx\r\n");
    exec_cmd(&d, T0, &out, 5, "LMOVE", "rot", "rot", "LEFT", "RIGHT");
    EXPECT(out, "$1\r\ny\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "rot", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\nx\r\n$1\r\ny\r\n");

    /* draining src deletes the key */
    exec_cmd(&d, T0, &out, 5, "LMOVE", "src", "dst", "RIGHT", "LEFT");
    EXPECT(out, "$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "src");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 5, "LMOVE", "src", "dst", "RIGHT", "LEFT");
    EXPECT(out, "$-1\r\n");

    /* wrong types reject both sides without mutation */
    exec_cmd(&d, T0, &out, 3, "SET", "str", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 5, "LMOVE", "dst", "str", "LEFT", "LEFT");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");
    exec_cmd(&d, T0, &out, 5, "LMOVE", "str", "dst", "LEFT", "LEFT");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");

    /* bad direction and arity */
    exec_cmd(&d, T0, &out, 5, "LMOVE", "dst", "x", "UP", "LEFT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "LMOVE", "dst", "x", "LEFT");
    EXPECT(out,
           "-ERR wrong number of arguments for 'lmove' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_lmpop(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 4, "RPUSH", "l1", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "l2", "c");
    EXPECT(out, ":1\r\n");

    exec_cmd(&d, T0, &out, 5, "LMPOP", "2", "l1", "l2", "LEFT");
    EXPECT(out, "*2\r\n$2\r\nl1\r\n*1\r\n$1\r\na\r\n");
    exec_cmd(&d, T0, &out, 4, "LRANGE", "l1", "0", "-1");
    EXPECT(out, "*1\r\n$1\r\nb\r\n");

    /* first non-empty list wins; missing keys are skipped */
    exec_cmd(&d, T0, &out, 7, "LMPOP", "2", "nokey", "l2", "RIGHT", "COUNT", "2");
    EXPECT(out, "*2\r\n$2\r\nl2\r\n*1\r\n$1\r\nc\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "l2");
    EXPECT(out, ":0\r\n");

    /* COUNT larger than the list pops everything and deletes the key */
    exec_cmd(&d, T0, &out, 5, "RPUSH", "l3", "x", "y", "z");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 6, "LMPOP", "1", "l3", "LEFT", "COUNT", "5");
    EXPECT(out, "*2\r\n$2\r\nl3\r\n*3\r\n$1\r\nx\r\n$1\r\ny\r\n$1\r\nz\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "l3");
    EXPECT(out, ":0\r\n");

    /* all missing -> null array */
    exec_cmd(&d, T0, &out, 4, "LMPOP", "1", "nokey", "LEFT");
    EXPECT(out, "*-1\r\n");

    /* validation errors */
    exec_cmd(&d, T0, &out, 4, "LMPOP", "0", "l1", "LEFT");
    EXPECT(out, "-ERR numkeys should be greater than 0\r\n");
    exec_cmd(&d, T0, &out, 4, "LMPOP", "2", "l1", "LEFT");
    EXPECT(out, "-ERR wrong number of arguments for 'lmpop' command\r\n");
    exec_cmd(&d, T0, &out, 4, "LMPOP", "1", "l1", "UP");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 6, "LMPOP", "1", "l1", "LEFT", "COUNT", "0");
    EXPECT(out, "-ERR value is out of range, must be positive\r\n");
    exec_cmd(&d, T0, &out, 3, "LMPOP", "1", "l1");
    EXPECT(out, "-ERR wrong number of arguments for 'lmpop' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_lpop_rpop_count(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 5, "RPUSH", "l", "a", "b", "c");
    EXPECT(out, ":3\r\n");
    exec_cmd(&d, T0, &out, 3, "RPUSH", "l", "d");
    EXPECT(out, ":4\r\n"); /* a b c d */

    /* count form: array of min(count, llen) */
    exec_cmd(&d, T0, &out, 3, "LPOP", "l", "2");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
    exec_cmd(&d, T0, &out, 3, "RPOP", "l", "2");
    EXPECT(out, "*2\r\n$1\r\nd\r\n$1\r\nc\r\n");
    /* list drained: key gone */
    exec_cmd(&d, T0, &out, 2, "EXISTS", "l");
    EXPECT(out, ":0\r\n");

    /* missing key: null (not an empty array) */
    exec_cmd(&d, T0, &out, 3, "LPOP", "l", "3");
    EXPECT(out, "*-1\r\n");
    exec_cmd(&d, T0, &out, 3, "RPOP", "l", "1");
    EXPECT(out, "*-1\r\n");
    exec_cmd(&d, T0, &out, 2, "LPOP", "l");
    EXPECT(out, "$-1\r\n");

    /* count > llen pops everything and deletes the key */
    exec_cmd(&d, T0, &out, 4, "RPUSH", "m", "x", "y");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "LPOP", "m", "5");
    EXPECT(out, "*2\r\n$1\r\nx\r\n$1\r\ny\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "m");
    EXPECT(out, ":0\r\n");

    /* count 1 still returns an array */
    exec_cmd(&d, T0, &out, 3, "RPUSH", "n", "q");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "RPOP", "n", "1");
    EXPECT(out, "*1\r\n$1\r\nq\r\n");
    exec_cmd(&d, T0, &out, 2, "EXISTS", "n");
    EXPECT(out, ":0\r\n");

    /* count must be positive */
    exec_cmd(&d, T0, &out, 4, "RPUSH", "p", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 3, "LPOP", "p", "0");
    EXPECT(out, "-ERR value is out of range, must be positive\r\n");
    exec_cmd(&d, T0, &out, 3, "RPOP", "p", "-1");
    EXPECT(out, "-ERR value is out of range, must be positive\r\n");
    exec_cmd(&d, T0, &out, 3, "LPOP", "p", "x");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 2, "LLEN", "p");
    EXPECT(out, ":2\r\n"); /* untouched */

    /* wrong type and arity */
    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "LPOP", "s", "2");
    EXPECT(out,
           "-WRONGTYPE Operation against a key holding the wrong kind of "
           "value\r\n");
    exec_cmd(&d, T0, &out, 4, "LPOP", "p", "1", "x");
    EXPECT(out, "-ERR wrong number of arguments for 'lpop' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_list_rejects_unrepresentable_elements);
    DD_RUN(test_list_commands_reject_transactionally);
    DD_RUN(test_push_len_range);
    DD_RUN(test_lrange_index_math);
    DD_RUN(test_lindex_lset);
    DD_RUN(test_pop_auto_delete);
    DD_RUN(test_pushx);
    DD_RUN(test_list_wrongtype);
    DD_RUN(test_list_ttl_and_memory);
    DD_RUN(test_lpos);
    DD_RUN(test_lrem);
    DD_RUN(test_ltrim);
    DD_RUN(test_rpoplpush);
    DD_RUN(test_rpoplpush_bumps_both_key_versions);
    DD_RUN(test_linsert);
    DD_RUN(test_lmove);
    DD_RUN(test_lmpop);
    DD_RUN(test_lpop_rpop_count);
    return DD_TEST_SUMMARY();
}
