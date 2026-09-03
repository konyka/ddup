/* test_command.c - command dispatch tests (written before the impl). */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"

static db g_db;
static resp_buf g_out;
static resp_value g_argv[8];

/* Build argv from cstrings and execute. Returns reply as NUL-terminated
 * string (replies here never contain NULs). */
static const char *cmd(int argc, ...)
{
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        const char *s = va_arg(ap, const char *);
        g_argv[i].type = RESP_BULK_STRING;
        g_argv[i].str = s;
        g_argv[i].len = strlen(s);
        g_argv[i].is_null = 0;
    }
    va_end(ap);
    g_out.len = 0;
    command_execute(&g_db, g_argv, (size_t)argc, &g_out);
    resp_buf_reserve(&g_out, 1);
    g_out.data[g_out.len] = '\0';
    return g_out.data;
}

#define EXPECT_REPLY(expected)                                                \
    DD_CHECK_STR((expected), g_out.data)

static void test_ping_echo(void)
{
    cmd(1, "PING");
    EXPECT_REPLY("+PONG\r\n");
    cmd(2, "PING", "hello");
    EXPECT_REPLY("$5\r\nhello\r\n");
    cmd(1, "ping"); /* case-insensitive */
    EXPECT_REPLY("+PONG\r\n");
    cmd(2, "ECHO", "world");
    EXPECT_REPLY("$5\r\nworld\r\n");
    cmd(1, "ECHO");
    EXPECT_REPLY("-ERR wrong number of arguments for 'echo' command\r\n");
}

static void test_get_set(void)
{
    cmd(3, "SET", "foo", "bar");
    EXPECT_REPLY("+OK\r\n");
    cmd(2, "GET", "foo");
    EXPECT_REPLY("$3\r\nbar\r\n");
    cmd(2, "GET", "missing");
    EXPECT_REPLY("$-1\r\n");
    cmd(3, "SET", "foo", "baz"); /* overwrite */
    EXPECT_REPLY("+OK\r\n");
    cmd(2, "GET", "foo");
    EXPECT_REPLY("$3\r\nbaz\r\n");
    cmd(2, "SET", "onlykey");
    EXPECT_REPLY("-ERR wrong number of arguments for 'set' command\r\n");
}

static void test_del_exists(void)
{
    cmd(3, "SET", "a", "1");
    cmd(3, "SET", "b", "2");
    cmd(3, "DEL", "a", "c");
    EXPECT_REPLY(":1\r\n");
    cmd(2, "EXISTS", "a");
    EXPECT_REPLY(":0\r\n");
    cmd(3, "EXISTS", "b", "b"); /* duplicates count twice, like Redis */
    EXPECT_REPLY(":2\r\n");
    cmd(1, "DEL");
    EXPECT_REPLY("-ERR wrong number of arguments for 'del' command\r\n");
}

static void test_incr_decr(void)
{
    cmd(2, "INCR", "counter"); /* missing key starts at 0 */
    EXPECT_REPLY(":1\r\n");
    cmd(2, "INCR", "counter");
    EXPECT_REPLY(":2\r\n");
    cmd(2, "DECR", "counter");
    EXPECT_REPLY(":1\r\n");
    cmd(3, "SET", "counter", "100");
    cmd(2, "INCR", "counter");
    EXPECT_REPLY(":101\r\n");
    cmd(3, "SET", "counter", "notanumber");
    cmd(2, "INCR", "counter");
    EXPECT_REPLY("-ERR value is not an integer or out of range\r\n");
    cmd(3, "SET", "counter", "9223372036854775807");
    cmd(2, "INCR", "counter"); /* overflow */
    EXPECT_REPLY("-ERR increment or decrement would overflow\r\n");
}

static void test_append_strlen(void)
{
    cmd(2, "STRLEN", "s");
    EXPECT_REPLY(":0\r\n");
    cmd(3, "APPEND", "s", "Hello");
    EXPECT_REPLY(":5\r\n");
    cmd(3, "APPEND", "s", " World");
    EXPECT_REPLY(":11\r\n");
    cmd(2, "GET", "s");
    EXPECT_REPLY("$11\r\nHello World\r\n");
    cmd(2, "STRLEN", "s");
    EXPECT_REPLY(":11\r\n");
}

static void test_mget_mset(void)
{
    cmd(5, "MSET", "k1", "v1", "k2", "v2");
    EXPECT_REPLY("+OK\r\n");
    cmd(4, "MGET", "k1", "k2", "k3");
    EXPECT_REPLY("*3\r\n$2\r\nv1\r\n$2\r\nv2\r\n$-1\r\n");
    cmd(4, "MSET", "odd", "pairs", "here");
    EXPECT_REPLY("-ERR wrong number of arguments for 'mset' command\r\n");
}

static void test_unknown_command(void)
{
    cmd(1, "NOSUCHCMD");
    EXPECT_REPLY("-ERR unknown command 'NOSUCHCMD'\r\n");
}

static void test_empty_argv(void)
{
    g_out.len = 0;
    command_execute(&g_db, g_argv, 0, &g_out);
    resp_buf_reserve(&g_out, 1);
    g_out.data[g_out.len] = '\0';
    EXPECT_REPLY("-ERR empty command\r\n");
}

static void test_set_rejection_is_transactional(void)
{
    const char byte = 'x';
    const char *v;
    size_t vl;
    uint64_t used, dirty, version;

    cmd(3, "SET", "guarded", "old");
    cmd(3, "PEXPIRE", "guarded", "100000");
    g_db.watch_refs = 1;
    used = g_db.used_memory;
    dirty = g_db.dirty;
    version = db_key_version(&g_db, "guarded", 7);

    memset(g_argv, 0, sizeof(g_argv));
    g_argv[0].type = RESP_BULK_STRING;
    g_argv[0].str = "SET";
    g_argv[0].len = 3;
    g_argv[1].type = RESP_BULK_STRING;
    g_argv[1].str = "guarded";
    g_argv[1].len = 7;
    g_argv[2].type = RESP_BULK_STRING;
    g_argv[2].str = &byte;
    g_argv[2].len = SIZE_MAX;
    g_out.len = 0;
    command_execute(&g_db, g_argv, 3, &g_out);

    DD_CHECK(g_out.len > 1 && g_out.data[0] == '-');
    DD_CHECK(g_db.used_memory == used);
    DD_CHECK(g_db.dirty == dirty);
    DD_CHECK(db_key_version(&g_db, "guarded", 7) == version);
    DD_CHECK(rh_get(&g_db.table, "guarded", 7, &v, &vl) == 1);
    DD_CHECK_MEM("old", 3, v + 1, vl - 1);
    DD_CHECK(rh_get(&g_db.expires, "guarded", 7, &v, &vl) == 1);
    g_db.watch_refs = 0;
}

static void test_object_encoding(void)
{
    int i;
    char m[16];

    cmd(3, "SET", "ostr", "v");
    EXPECT_REPLY("+OK\r\n");
    cmd(3, "OBJECT", "ENCODING", "ostr");
    EXPECT_REPLY("$3\r\nraw\r\n");

    cmd(3, "RPUSH", "ol", "a");
    EXPECT_REPLY(":1\r\n");
    cmd(3, "OBJECT", "ENCODING", "ol");
    EXPECT_REPLY("$9\r\nquicklist\r\n");

    /* small containers: listpack */
    cmd(4, "HSET", "oh", "f", "v");
    EXPECT_REPLY(":1\r\n");
    cmd(3, "OBJECT", "ENCODING", "oh");
    EXPECT_REPLY("$8\r\nlistpack\r\n");
    cmd(3, "SADD", "os", "m");
    EXPECT_REPLY(":1\r\n");
    cmd(3, "OBJECT", "ENCODING", "os");
    EXPECT_REPLY("$8\r\nlistpack\r\n");
    cmd(4, "ZADD", "oz", "1", "m");
    EXPECT_REPLY(":1\r\n");
    cmd(3, "OBJECT", "ENCODING", "oz");
    EXPECT_REPLY("$8\r\nlistpack\r\n");

    /* crossing the 128-entry threshold flips the encoding */
    for (i = 0; i < 129; i++) {
        snprintf(m, sizeof(m), "f%d", i);
        cmd(4, "HSET", "oh", m, "v");
    }
    cmd(3, "OBJECT", "ENCODING", "oh");
    EXPECT_REPLY("$9\r\nhashtable\r\n");
    for (i = 0; i < 129; i++) {
        snprintf(m, sizeof(m), "m%d", i);
        cmd(3, "SADD", "os", m);
    }
    cmd(3, "OBJECT", "ENCODING", "os");
    EXPECT_REPLY("$9\r\nhashtable\r\n");
    for (i = 0; i < 129; i++) {
        snprintf(m, sizeof(m), "m%d", i);
        cmd(4, "ZADD", "oz", "1", m);
    }
    cmd(3, "OBJECT", "ENCODING", "oz");
    EXPECT_REPLY("$8\r\nskiplist\r\n");

    /* missing key -> null bulk; command and subcommand case-insensitive */
    cmd(3, "object", "encoding", "nokey");
    EXPECT_REPLY("$-1\r\n");

    /* unknown subcommand / arity */
    cmd(3, "OBJECT", "FOO", "oh");
    EXPECT_REPLY("-ERR unknown OBJECT subcommand\r\n");
    cmd(2, "OBJECT", "ENCODING");
    EXPECT_REPLY("-ERR wrong number of arguments for 'object' command\r\n");
    cmd(4, "OBJECT", "ENCODING", "oh", "x");
    EXPECT_REPLY("-ERR wrong number of arguments for 'object' command\r\n");
}

static void test_object_metadata_and_getkeysflags(void)
{
    cmd(3, "SET", "meta", "value");
    cmd(3, "OBJECT", "REFCOUNT", "meta");
    EXPECT_REPLY(":1\r\n");
    cmd(3, "OBJECT", "FREQ", "meta");
    EXPECT_REPLY(":0\r\n");
    cmd(3, "OBJECT", "IDLETIME", "meta");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == ':');
    cmd(3, "OBJECT", "REFCOUNT", "nokey");
    EXPECT_REPLY("$-1\r\n");

    cmd(5, "COMMAND", "GETKEYSANDFLAGS", "SET", "k", "v");
    EXPECT_REPLY("*1\r\n*2\r\n$1\r\nk\r\n$2\r\nRW\r\n");
    cmd(4, "COMMAND", "GETKEYSANDFLAGS", "GET", "k");
    EXPECT_REPLY("*1\r\n*2\r\n$1\r\nk\r\n$2\r\nRO\r\n");
    cmd(7, "COMMAND", "GETKEYS", "EVAL_RO", "return 1", "2", "k1",
        "k2");
    EXPECT_REPLY("*2\r\n$2\r\nk1\r\n$2\r\nk2\r\n");
    cmd(6, "COMMAND", "GETKEYS", "FCALL_RO", "fn", "1", "k1");
    EXPECT_REPLY("*1\r\n$2\r\nk1\r\n");
    cmd(7, "COMMAND", "GETKEYS", "SORT", "src", "STORE", "dst");
    EXPECT_REPLY("*2\r\n$3\r\nsrc\r\n$3\r\ndst\r\n");
    cmd(5, "COMMAND", "GETKEYS", "MEMORY", "USAGE", "key");
    EXPECT_REPLY("*1\r\n$3\r\nkey\r\n");
    cmd(5, "COMMAND", "GETKEYS", "OBJECT", "ENCODING", "key");
    EXPECT_REPLY("*1\r\n$3\r\nkey\r\n");
}

static void test_server_management_commands(void)
{
    cmd(3, "WAIT", "1", "0");
    EXPECT_REPLY(":0\r\n");
    cmd(4, "WAITAOF", "1", "1", "0");
    EXPECT_REPLY("*2\r\n:0\r\n:0\r\n");
    cmd(3, "REPLCONF", "ACK", "0");
    EXPECT_REPLY("+OK\r\n");
    cmd(3, "REPLCONF", "GETACK", "*");
    EXPECT_REPLY("*3\r\n$8\r\nREPLCONF\r\n$3\r\nACK\r\n:0\r\n");
    cmd(2, "FAILOVER", "ABORT");
    EXPECT_REPLY("+OK\r\n");
    cmd(1, "FAILOVER");
    DD_CHECK(g_out.len > 0 && g_out.data[0] == '-');
    cmd(1, "MONITOR");
    EXPECT_REPLY("-ERR MONITOR is not supported in this build\r\n");

    cmd(2, "ACL", "WHOAMI");
    EXPECT_REPLY("$7\r\ndefault\r\n");
    cmd(2, "LATENCY", "LATEST");
    EXPECT_REPLY("*0\r\n");
    cmd(3, "LATENCY", "GRAPH", "missing-event");
    EXPECT_REPLY("-ERR No samples available for event 'missing-event'\r\n");
    cmd(3, "LATENCY", "HISTORY", "missing-event");
    EXPECT_REPLY("-ERR No samples available for event 'missing-event'\r\n");
    cmd(2, "MODULE", "LIST");
    EXPECT_REPLY("*0\r\n");
    cmd(2, "SENTINEL", "MASTERS");
    EXPECT_REPLY("*0\r\n");
    cmd(3, "DEBUG", "SLEEP", "0");
    EXPECT_REPLY("+OK\r\n");
    cmd(4, "DEBUG", "STRINGMATCH", "cache:42", "cache:*");
    EXPECT_REPLY(":1\r\n");
    cmd(4, "DEBUG", "STRINGMATCH", "cache:42", "user:*");
    EXPECT_REPLY(":0\r\n");
    cmd(2, "INFO", "SERVER");
    DD_CHECK(strstr(g_out.data, "# Memory") != NULL);
    cmd(3, "INFO", "SERVER", "STATS");
    DD_CHECK(strstr(g_out.data, "# Stats") != NULL);
}

int main(void)
{
    db_init(&g_db);
    resp_buf_init(&g_out);
    DD_RUN(test_ping_echo);
    DD_RUN(test_get_set);
    DD_RUN(test_del_exists);
    DD_RUN(test_incr_decr);
    DD_RUN(test_append_strlen);
    DD_RUN(test_mget_mset);
    DD_RUN(test_unknown_command);
    DD_RUN(test_empty_argv);
    DD_RUN(test_set_rejection_is_transactional);
    DD_RUN(test_object_encoding);
    DD_RUN(test_object_metadata_and_getkeysflags);
    DD_RUN(test_server_management_commands);
    resp_buf_free(&g_out);
    db_destroy(&g_db);
    return DD_TEST_SUMMARY();
}
