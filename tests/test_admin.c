/* test_admin.c - server ops/introspection command family tests. */
#include "test.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/buf_pool.h"
#include "core/command.h"
#include "pal/pal_file.h"
#include "pal/pal_socket.h"
#include "server/aof.h"
#include "server/server.h"

static db g_db;
static resp_buf g_out;
static resp_value g_argv[16];

static const char *cmd(int argc, ...)
{
    va_list ap;
    int i;
    if (argc > 16)
        return NULL;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
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

#define EXPECT_REPLY(expected) DD_CHECK_STR((expected), g_out.data)

static void test_command_count_list(void)
{
    long long count = -1;
    cmd(2, "COMMAND", "COUNT");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == ':');
    if (g_out.len > 1 && g_out.data[0] == ':')
        count = strtoll(g_out.data + 1, NULL, 10);
    DD_CHECK(count > 190);

    cmd(2, "COMMAND", "LIST");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "get") != NULL);
    DD_CHECK(strstr(g_out.data, "set") != NULL);
}

static void test_command_info_getkeys(void)
{
    cmd(3, "COMMAND", "INFO", "get");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "get") != NULL);
    DD_CHECK(strstr(g_out.data, "readonly") != NULL);
    DD_CHECK(strstr(g_out.data, "2") != NULL);
    DD_CHECK(strstr(g_out.data, "1") != NULL);

    cmd(5, "COMMAND", "GETKEYS", "SET", "k", "v");
    EXPECT_REPLY("*1\r\n$1\r\nk\r\n");

    cmd(7, "COMMAND", "GETKEYS", "MSET", "k1", "v1", "k2", "v2");
    EXPECT_REPLY("*2\r\n$2\r\nk1\r\n$2\r\nk2\r\n");

    cmd(4, "COMMAND", "GETKEYS", "GET", "k");
    EXPECT_REPLY("*1\r\n$1\r\nk\r\n");

    cmd(2, "COMMAND", "NOPE");
    EXPECT_REPLY("-ERR unknown COMMAND subcommand\r\n");
}

static void test_memory_usage_stats(void)
{
    long long usage = -1;
    cmd(3, "SET", "mk", "value");
    cmd(3, "MEMORY", "USAGE", "mk");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == ':');
    if (g_out.len > 1 && g_out.data[0] == ':')
        usage = strtoll(g_out.data + 1, NULL, 10);
    DD_CHECK(usage > 0);

    cmd(3, "MEMORY", "USAGE", "missing-key");
    EXPECT_REPLY("$-1\r\n");

    cmd(2, "MEMORY", "STATS");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "used_memory") != NULL);

    cmd(2, "MEMORY", "DOCTOR");
    DD_CHECK(g_out.len > 1);
    cmd(2, "MEMORY", "NOPE");
    EXPECT_REPLY("-ERR unknown MEMORY subcommand\r\n");
}

/* Round-trip one request through a real server and one client socket. */
static void roundtrip(server *s, pal_socket_t c, const char *req,
                      const char *expected)
{
    size_t elen = strlen(expected);
    size_t rlen = strlen(req);
    size_t sent = 0, got = 0;
    char buf[4096];
    int iter = 0;

    DD_CHECK(elen <= sizeof(buf));
    while (sent < rlen) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
    }
    while (got < elen && iter < 2000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 1);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
        else if (n == 0)
            break;
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static void roundtrip_contains(server *s, pal_socket_t c, const char *req,
                                 const char *needle)
{
    size_t rlen = strlen(req);
    size_t sent = 0, got = 0;
    char buf[4096];
    int iter = 0, idle = 0;

    while (sent < rlen) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
    }
    while (iter < 2000 && idle < 20) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 1);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0) {
            got += (size_t)n;
            idle = 0;
        } else if (n == 0 || (n < 0 && !pal_would_block(pal_socket_error()))) {
            break;
        } else if (n < 0) {
            idle++;
        }
    }
    if (got < sizeof(buf))
        buf[got] = '\0';
    else
        buf[sizeof(buf) - 1] = '\0';
    DD_CHECK(strstr(buf, needle) != NULL);
}

static pal_socket_t connect_client(server *s)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", server_port(s));
    DD_CHECK(c != PAL_SOCKET_INVALID);
    if (c != PAL_SOCKET_INVALID)
        DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void test_client_name_and_id(void)
{
    server *s;
    pal_socket_t c;
    s = server_create("127.0.0.1", 0);
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);

    roundtrip(s, c, "*1\r\n$6\r\nCLIENT\r\n", "-ERR wrong number of arguments for 'client' command\r\n");
    roundtrip_contains(s, c, "*2\r\n$6\r\nCLIENT\r\n$2\r\nID\r\n", ":");
    pal_close(c);
    server_destroy(s);
}

static void test_slowlog(void)
{
    static const char path[] = "test_admin_slowlog.aof";
    server *s;
    pal_socket_t c;
    remove(path);
    s = server_create("127.0.0.1", 0);
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    DD_CHECK_EQ_INT(0, server_enable_aof(s, path));
    c = connect_client(s);

    server_set_slowlog_threshold(s, 0); /* log every command */
    roundtrip(s, c, "*2\r\n$7\r\nSLOWLOG\r\n$3\r\nLEN\r\n", ":0\r\n");
    roundtrip(s, c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    roundtrip(s, c, "*2\r\n$7\r\nSLOWLOG\r\n$5\r\nRESET\r\n", "+OK\r\n");
    roundtrip(s, c, "*2\r\n$7\r\nSLOWLOG\r\n$3\r\nLEN\r\n", ":1\r\n");
    roundtrip_contains(s, c,
                       "*3\r\n$7\r\nSLOWLOG\r\n$3\r\nGET\r\n$1\r\n5\r\n",
                       "RESET");

    pal_close(c);
    server_destroy(s);
    remove(path);
}

static void test_bgsave_bgrewriteaof(void)
{
    static const char snap[] = "test_admin_snapshot.ddr";
    static const char path[] = "test_admin_bgrewriteaof.aof";
    server *s;
    pal_socket_t c;
    remove(snap);
    remove(path);
    s = server_create("127.0.0.1", 0);
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    server_set_snapshot_path(s, snap);
    DD_CHECK_EQ_INT(0, server_enable_aof(s, path));
    c = connect_client(s);

    roundtrip(s, c, "*1\r\n$6\r\nBGSAVE\r\n", "+Background saving started\r\n");
    DD_CHECK(pal_file_exists(snap));

    roundtrip(s, c, "*1\r\n$12\r\nBGREWRITEAOF\r\n", "+Background append only file rewriting started\r\n");

    pal_close(c);
    server_destroy(s);
    remove(snap);
    remove(path);
}

static void test_container_help(void)
{
    cmd(2, "COMMAND", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "INFO") != NULL);

    cmd(2, "CLIENT", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "ID") != NULL);

    cmd(2, "MEMORY", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "USAGE") != NULL);

    cmd(2, "SLOWLOG", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "GET") != NULL);

    cmd(2, "OBJECT", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "ENCODING") != NULL);

    cmd(2, "CONFIG", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "GET") != NULL);

    cmd(2, "SCRIPT", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "LOAD") != NULL);

    cmd(2, "PUBSUB", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "CHANNELS") != NULL);

    cmd(2, "CLUSTER", "HELP");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '*');
    DD_CHECK(strstr(g_out.data, "INFO") != NULL);
}

static void test_lolwut(void)
{
    cmd(1, "LOLWUT");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '$');
    DD_CHECK(strstr(g_out.data, "Redis ver.") != NULL);

    cmd(3, "LOLWUT", "VERSION", "5");
    DD_CHECK(g_out.len > 1 && g_out.data[0] == '$');
    DD_CHECK(strstr(g_out.data, "Redis ver.") != NULL);

    cmd(3, "LOLWUT", "VERSION", "99");
    DD_CHECK(g_out.len > 5 && g_out.data[0] == '-');
}

static void test_config_resetstat_rewrite(void)
{
    cmd(3, "SET", "cfgkey", "v");
    cmd(2, "CONFIG", "RESETSTAT");
    EXPECT_REPLY("+OK\r\n");
    cmd(2, "CONFIG", "REWRITE");
    EXPECT_REPLY("-ERR The server is running without a config file\r\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_CHECK_EQ_INT(0, pal_socket_init());
    db_init(&g_db);
    resp_buf_init(&g_out);
    DD_RUN(test_command_count_list);
    DD_RUN(test_command_info_getkeys);
    DD_RUN(test_config_resetstat_rewrite);
    DD_RUN(test_memory_usage_stats);
    DD_RUN(test_client_name_and_id);
    DD_RUN(test_slowlog);
    DD_RUN(test_bgsave_bgrewriteaof);
    DD_RUN(test_container_help);
    DD_RUN(test_lolwut);
    resp_buf_free(&g_out);
    db_destroy(&g_db);
    return DD_TEST_SUMMARY();
}
