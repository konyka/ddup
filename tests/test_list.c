/* test_list.c - list object commands with synthetic injected time. */
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

static uint64_t node_bytes(size_t elen)
{
    return (uint64_t)sizeof(list_node) + 16 + elen;
}

static void test_list_rejects_unrepresentable_elements(void)
{
    obj_list *l = obj_list_new();
    const char byte = 'x';
    list_node *head;
    uint64_t before;

    DD_CHECK_EQ_INT(0, obj_list_push(l, 0, "old", 3));
    head = l->head;
    before = obj_list_mem(l);
    DD_CHECK_EQ_INT(-1, obj_list_push(l, 1, &byte, SIZE_MAX));
    DD_CHECK_EQ_INT(-1, obj_list_set_at(l, 0, &byte, SIZE_MAX));
    DD_CHECK(l->head == head && l->tail == head);
    DD_CHECK_EQ_INT(1, l->len);
    DD_CHECK(obj_list_mem(l) == before);
    DD_CHECK_MEM("old", 3, head->data, head->len);
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

    DD_CHECK_EQ_INT(2, l->len);
    DD_CHECK(obj_list_mem(l) == list_mem);
    DD_CHECK(d.used_memory == used);
    DD_CHECK(d.dirty == dirty);
    DD_CHECK(db_key_version(&d, "l", 1) == version);
    DD_CHECK_MEM("old", 3, l->head->data, l->head->len);
    DD_CHECK_MEM("tail", 4, l->tail->data, l->tail->len);

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
    db_init(&d);
    resp_buf_init(&out);

    /* accounting: entry + list struct + per-node cost */
    exec_cmd(&d, T0, &out, 3, "LPUSH", "l", "a");
    EXPECT(out, ":1\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_list) + node_bytes(1));

    exec_cmd(&d, T0, &out, 4, "RPUSH", "l", "bb", "ccc");
    EXPECT(out, ":3\r\n");
    DD_CHECK(d.used_memory == eb(1, 9) + sizeof(obj_list) + node_bytes(1) +
                                 node_bytes(2) + node_bytes(3));

    exec_cmd(&d, T0, &out, 2, "LPOP", "l");
    EXPECT(out, "$1\r\na\r\n");
    DD_CHECK(d.used_memory ==
             eb(1, 9) + sizeof(obj_list) + node_bytes(2) + node_bytes(3));

    /* TTL works on list keys; expiry frees the whole object */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "l", "10");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "TTL", "l");
    EXPECT(out, ":10\r\n");
    before = d.used_memory;
    exec_cmd(&d, T0 + 10000, &out, 2, "LLEN", "l");
    EXPECT(out, ":0\r\n");
    DD_CHECK(d.used_memory ==
             before - (eb(1, 9) + sizeof(obj_list) + node_bytes(2) +
                       node_bytes(3) + eb(1, 8)));
    DD_CHECK_EQ_INT(1, (long long)d.expired_keys);

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
    return DD_TEST_SUMMARY();
}
