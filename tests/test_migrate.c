/* test_migrate.c - MIGRATE between two in-process servers.
 *
 * The source server blocks inside MIGRATE while the target (same thread)
 * still needs its event loop pumped: the test installs migrate's pump
 * hook to run the target's loop while the source waits for replies.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/migrate.h"
#include "pal/pal_socket.h"
#include "server/server.h"
#include "test.h"

static void pump_target(void *ctx)
{
    server_run_once((server *)ctx, 1);
}

static pal_socket_t cli(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void pump2(server *x, server *y)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
}

/* send a command, pump both servers, read the reply */
static size_t ask2(server *x, server *y, pal_socket_t c, const char *req,
                   char *buf, size_t cap)
{
    size_t got = 0;
    int iter = 0;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (iter < 2000) {
        ptrdiff_t n;
        iter++;
        pump2(x, y);
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0)
            got += (size_t)n;
        if (n > 0)
            break; /* full reply for single command */
    }
    buf[got] = '\0';
    return got;
}

/* build a RESP request from string args */
static void fmt_cmd(char *buf, size_t cap, int argc, ...)
{
    va_list ap;
    size_t off = 0;
    int i;
    off += (size_t)snprintf(buf + off, cap - off, "*%d\r\n", argc);
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *a = va_arg(ap, const char *);
        off += (size_t)snprintf(buf + off, cap - off, "$%zu\r\n%s\r\n",
                                strlen(a), a);
    }
    va_end(ap);
}

/* create two servers; b becomes the pump-hook target */
static void make2(server **a, server **b)
{
    *a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    *b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(*a != NULL && *b != NULL);
    migrate_set_pump_hook(pump_target, *b);
}

static void free2(server *a, server *b)
{
    migrate_set_pump_hook(NULL, NULL);
    server_destroy(a);
    server_destroy(b);
}

#define EXPECT(buf, s) DD_CHECK_MEM((s), strlen(s), (buf), strlen(buf))

static void test_migrate_string(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[512], port[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n", buf,
         sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "k", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    ask2(a, b, ca, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$-1\r\n");
    ask2(a, b, cb, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$5\r\nhello\r\n");

    pal_close(ca);
    pal_close(cb);
    free2(a, b);
}

static void test_migrate_api_rejects_null_key_array(void)
{
    db d;
    db_init(&d);
    DD_CHECK_EQ_INT(MIGRATE_IOERR,
                    migrate_run(&d, "127.0.0.1", 1, NULL, 1, 1, 0, 0, 0));
    DD_CHECK_EQ_INT(MIGRATE_IOERR,
                    migrate_run(NULL, "127.0.0.1", 1, NULL, 0, 1, 0, 0,
                                0));
    DD_CHECK_EQ_INT(MIGRATE_OK,
                    migrate_run(&d, "127.0.0.1", 1, NULL, 0, 1, 0, 0, 0));
    db_destroy(&d);
}

static void test_migrate_hash_and_zset(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[512], port[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    ask2(a, b, ca, "*4\r\n$4\r\nHSET\r\n$1\r\nh\r\n$2\r\nf1\r\n$2\r\nv1\r\n",
         buf, sizeof(buf));
    EXPECT(buf, ":1\r\n");
    ask2(a, b, ca,
         "*4\r\n$4\r\nZADD\r\n$1\r\nz\r\n$3\r\n1.5\r\n$2\r\nm1\r\n", buf,
         sizeof(buf));
    EXPECT(buf, ":1\r\n");

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "h", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "z", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    ask2(a, b, cb, "*3\r\n$4\r\nHGET\r\n$1\r\nh\r\n$2\r\nf1\r\n", buf,
         sizeof(buf));
    EXPECT(buf, "$2\r\nv1\r\n");
    ask2(a, b, cb, "*3\r\n$6\r\nZSCORE\r\n$1\r\nz\r\n$2\r\nm1\r\n", buf,
         sizeof(buf));
    EXPECT(buf, "$3\r\n1.5\r\n");
    ask2(a, b, ca, "*2\r\n$6\r\nEXISTS\r\n$1\r\nh\r\n", buf, sizeof(buf));
    EXPECT(buf, ":0\r\n");
    ask2(a, b, ca, "*2\r\n$6\r\nEXISTS\r\n$1\r\nz\r\n", buf, sizeof(buf));
    EXPECT(buf, ":0\r\n");

    pal_close(ca);
    pal_close(cb);
    free2(a, b);
}

static void test_migrate_nokey_and_dbid(void)
{
    server *a, *b;
    pal_socket_t ca;
    char req[512], buf[512], port[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "missing",
            "0", "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "-NOKEY No such key\r\n");

    /* nonzero db index rejected */
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "k", "5",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "-ERR DB index is out of range\r\n");

    pal_close(ca);
    free2(a, b);
}

static void test_migrate_copy(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[512], port[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", buf,
         sizeof(buf));
    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    fmt_cmd(req, sizeof(req), 7, "MIGRATE", "127.0.0.1", port, "k", "0",
            "1000", "COPY");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    /* source keeps the key, target has a copy */
    ask2(a, b, ca, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$1\r\nv\r\n");
    ask2(a, b, cb, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$1\r\nv\r\n");

    pal_close(ca);
    pal_close(cb);
    free2(a, b);
}

static void test_migrate_busykey_and_replace(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[512], port[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nnew\r\n", buf,
         sizeof(buf));
    ask2(a, b, cb, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$3\r\nold\r\n", buf,
         sizeof(buf));

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    /* target has the key and no REPLACE: migration fails, source kept */
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "k", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "-IOERR error or timeout writing to target instance\r\n");
    ask2(a, b, ca, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$3\r\nnew\r\n");
    ask2(a, b, cb, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$3\r\nold\r\n");

    /* REPLACE overwrites at the target */
    fmt_cmd(req, sizeof(req), 7, "MIGRATE", "127.0.0.1", port, "k", "0",
            "1000", "REPLACE");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");
    ask2(a, b, cb, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$3\r\nnew\r\n");
    ask2(a, b, ca, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$-1\r\n");

    pal_close(ca);
    pal_close(cb);
    free2(a, b);
}

static void test_migrate_target_down(void)
{
    server *a, *b;
    pal_socket_t ca;
    char req[512], buf[512], port[16];
    uint16_t dead_port;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));

    /* a port that is definitely closed: bind then destroy */
    dead_port = server_port(b);
    free2(a, b);
    make2(&a, &b); /* fresh pair (b on a new port, hook re-armed) */

    pal_close(ca);
    ca = cli(server_port(a));
    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", buf,
         sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    snprintf(port, sizeof(port), "%u", (unsigned)dead_port);
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "k", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "-IOERR error or timeout writing to target instance\r\n");

    /* failure leaves the source key in place */
    ask2(a, b, ca, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", buf, sizeof(buf));
    EXPECT(buf, "$1\r\nv\r\n");

    pal_close(ca);
    free2(a, b);
}

static void test_migrate_ttl_preserved(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[512], port[16];
    long long pttl;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    ask2(a, b, ca,
         "*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nPX\r\n$5\r\n60000\r\n",
         buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "k", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    ask2(a, b, cb, "*2\r\n$4\r\nPTTL\r\n$1\r\nk\r\n", buf, sizeof(buf));
    pttl = strtoll(buf + 1, NULL, 10);
    DD_CHECK(buf[0] == ':');
    DD_CHECK(pttl > 30000 && pttl <= 60000);

    /* key without ttl stays persistent */
    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$1\r\nv\r\n", buf,
         sizeof(buf));
    fmt_cmd(req, sizeof(req), 6, "MIGRATE", "127.0.0.1", port, "k2", "0",
            "1000");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");
    ask2(a, b, cb, "*2\r\n$4\r\nPTTL\r\n$2\r\nk2\r\n", buf, sizeof(buf));
    EXPECT(buf, ":-1\r\n");

    pal_close(ca);
    pal_close(cb);
    free2(a, b);
}

static void test_migrate_keys_form(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[512], buf[512], port[16];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    make2(&a, &b);
    ca = cli(server_port(a));
    cb = cli(server_port(b));

    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n", buf,
         sizeof(buf));
    ask2(a, b, ca, "*3\r\n$3\r\nSET\r\n$2\r\nk2\r\n$2\r\nv2\r\n", buf,
         sizeof(buf));

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    /* empty 3rd arg + KEYS; 'gone' does not exist and is skipped */
    fmt_cmd(req, sizeof(req), 10, "MIGRATE", "127.0.0.1", port, "", "0",
            "1000", "KEYS", "k1", "k2", "gone");
    ask2(a, b, ca, req, buf, sizeof(buf));
    EXPECT(buf, "+OK\r\n");

    ask2(a, b, cb, "*2\r\n$3\r\nGET\r\n$2\r\nk1\r\n", buf, sizeof(buf));
    EXPECT(buf, "$2\r\nv1\r\n");
    ask2(a, b, cb, "*2\r\n$3\r\nGET\r\n$2\r\nk2\r\n", buf, sizeof(buf));
    EXPECT(buf, "$2\r\nv2\r\n");
    ask2(a, b, ca, "*2\r\n$6\r\nEXISTS\r\n$2\r\nk1\r\n", buf, sizeof(buf));
    EXPECT(buf, ":0\r\n");
    ask2(a, b, ca, "*2\r\n$6\r\nEXISTS\r\n$2\r\nk2\r\n", buf, sizeof(buf));
    EXPECT(buf, ":0\r\n");

    pal_close(ca);
    pal_close(cb);
    free2(a, b);
}

int main(void)
{
    DD_CHECK_EQ_INT(0, migrate_test_output_failures());
    DD_RUN(test_migrate_string);
    DD_RUN(test_migrate_api_rejects_null_key_array);
    DD_RUN(test_migrate_hash_and_zset);
    DD_RUN(test_migrate_nokey_and_dbid);
    DD_RUN(test_migrate_copy);
    DD_RUN(test_migrate_busykey_and_replace);
    DD_RUN(test_migrate_target_down);
    DD_RUN(test_migrate_ttl_preserved);
    DD_RUN(test_migrate_keys_form);
    return DD_TEST_SUMMARY();
}
