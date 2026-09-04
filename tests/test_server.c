/* test_server.c - end-to-end tests: real TCP loopback against server.c.
 *
 * Single-threaded: the test acts as the client and interleaves
 * server_run_once() calls with client send/recv.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/buf_pool.h"
#include "core/hashslot.h"
#include "pal/pal_event.h"
#include "pal/pal_file.h"
#include "pal/pal_iocp.h"
#include "pal/pal_iouring_op.h"
#include "pal/pal_socket.h"
#include "server/aof.h"
#include "server/server.h"
#include "test.h"

/* Backend under test: SERVER_BACKEND_SELECT, SERVER_BACKEND_IOCP (Windows),
 * SERVER_BACKEND_IOURING / SERVER_BACKEND_IOURING_OP (Linux) when available.
 * Every scenario runs on each available backend. */
static int g_backend = SERVER_BACKEND_SELECT;

static server *make_server(void)
{
    return server_create_ex("127.0.0.1", 0, g_backend);
}

static ptrdiff_t fail_aof_write(pal_file *f, const void *buf, size_t len)
{
    (void)f;
    (void)buf;
    (void)len;
    return -1;
}

/* Send req, pump the server, and read exactly strlen(expected) reply bytes;
 * assert the reply matches expected. Client socket must be non-blocking. */
static void roundtrip(server *s, pal_socket_t c, const char *req,
                      const char *expected)
{
    size_t elen = strlen(expected);
    size_t rlen = strlen(req);
    size_t sent = 0, got = 0;
    char buf[1024];
    int iter = 0;

    DD_CHECK(elen <= sizeof(buf));
    while (sent < rlen) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)rlen, (long long)sent);

    while (got < elen && iter < 10000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static pal_socket_t connect_client(server *s)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", server_port(s));
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void test_cluster_save_close_failure_preserves_state(void)
{
    static const char path[] = "test_server_nodes.conf";
    static const char node_id[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    FILE *f;
    char disk[8];
    server *s;

    (void)remove(path);
    (void)remove("test_server_nodes.conf.tmp");
    f = fopen(path, "wb");
    DD_CHECK(f != NULL);
    if (f != NULL) {
        DD_CHECK_EQ_INT(8, (long long)fwrite("existing", 1, 8, f));
        DD_CHECK_EQ_INT(0, fclose(f));
    }

    s = make_server();
    DD_CHECK(s != NULL);
    if (s != NULL) {
        server_enable_cluster(s, node_id);
        server_set_nodes_path(s, path);
        pal_file_test_reset();
        pal_file_test_fail_next_close();
        server_test_cluster_nodes_save(s);

        f = fopen(path, "rb");
        DD_CHECK(f != NULL);
        if (f != NULL) {
            DD_CHECK_EQ_INT(8, (long long)fread(disk, 1, sizeof(disk), f));
            DD_CHECK_EQ_INT(0, fclose(f));
            DD_CHECK_MEM("existing", 8, disk, sizeof(disk));
        }
        DD_CHECK(!pal_file_exists("test_server_nodes.conf.tmp"));
        DD_CHECK_EQ_INT(1, pal_file_test_open_write_attempts());

        /* The failed close must leave the state dirty so a later save retries. */
        server_test_cluster_nodes_save(s);
        DD_CHECK_EQ_INT(2, pal_file_test_open_write_attempts());
        server_set_nodes_path(s, "");
        server_destroy(s);
    }
    (void)remove(path);
    (void)remove("test_server_nodes.conf.tmp");
}

static void test_persistence_paths_reject_truncation(void)
{
    server *s;
    char long_dir[1200];
    memset(long_dir, 'd', sizeof(long_dir) - 1);
    long_dir[sizeof(long_dir) - 1] = '\0';

    DD_CHECK_EQ_INT(-1, server_enable_tiering(NULL, ".", "tier.log", 0));
    DD_CHECK_EQ_INT(-1, server_enable_tiering(NULL, NULL, "tier.log", 0));
    DD_CHECK_EQ_INT(-1, server_enable_tiering(NULL, ".", NULL, 0));

    s = make_server();
    if (s != NULL) {
        DD_CHECK_EQ_INT(-1,
                        server_enable_tiering(s, long_dir, "tier.log", 0));
        server_set_nodes_path(s, NULL);
        server_destroy(s);
    }
}

static void test_ping_set_get(void)
{
    server *s = make_server();
    pal_socket_t c;
    DD_CHECK(s != NULL);
    DD_CHECK(server_port(s) != 0);
    c = connect_client(s);

    roundtrip(s, c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "+OK\r\n");
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", "$3\r\nbar\r\n");
    /* GET of a missing key -> null bulk */
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$5\r\nnokey\r\n", "$-1\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_client_reply_modes(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[128];
    int i;
    ptrdiff_t n;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);

    /* The mode-changing command itself is always acknowledged. */
    roundtrip(s, c, "*3\r\n$6\r\nCLIENT\r\n$5\r\nREPLY\r\n$3\r\nOFF\r\n",
              "+OK\r\n");
    {
        const char req[] = "*1\r\n$4\r\nPING\r\n";
        DD_CHECK_EQ_INT((long long)(sizeof(req) - 1), pal_send(c, req,
                                                               sizeof(req) - 1));
    }
    for (i = 0; i < 10; i++) {
        server_run_once(s, 5);
        n = pal_recv(c, buf, sizeof(buf));
        DD_CHECK(n <= 0);
    }
    /* Replies remain suppressed until ON, whose reply is suppressed too. */
    {
        const char req[] = "*3\r\n$6\r\nCLIENT\r\n$5\r\nREPLY\r\n$2\r\nON\r\n";
        DD_CHECK_EQ_INT((long long)(sizeof(req) - 1), pal_send(c, req,
                                                               sizeof(req) - 1));
    }
    for (i = 0; i < 10; i++) {
        server_run_once(s, 5);
        n = pal_recv(c, buf, sizeof(buf));
        DD_CHECK(n <= 0);
    }
    roundtrip(s, c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    roundtrip(s, c, "*3\r\n$6\r\nCLIENT\r\n$5\r\nREPLY\r\n$4\r\nSKIP\r\n",
              "+OK\r\n");
    {
        const char req[] = "*1\r\n$4\r\nPING\r\n";
        DD_CHECK_EQ_INT((long long)(sizeof(req) - 1), pal_send(c, req,
                                                               sizeof(req) - 1));
    }
    for (i = 0; i < 10; i++) {
        server_run_once(s, 5);
        n = pal_recv(c, buf, sizeof(buf));
        DD_CHECK(n <= 0);
    }
    roundtrip(s, c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    pal_close(c);
    server_destroy(s);
}

static void test_client_tracking_redirect_validation(void)
{
    server *s = make_server();
    pal_socket_t c;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);
    roundtrip(s, c,
              "*5\r\n$6\r\nCLIENT\r\n$8\r\nTRACKING\r\n$2\r\nON\r\n"
              "$8\r\nREDIRECT\r\n$2\r\n-1\r\n",
              "-ERR The client ID you want redirect to does not exist\r\n");
    pal_close(c);
    server_destroy(s);
}

static void test_client_setinfo_metadata(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[512];
    size_t got = 0;
    int iter = 0;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);
    roundtrip(s, c,
              "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$8\r\nLIB-NAME\r\n$3\r\nfoo\r\n",
              "+OK\r\n");
    roundtrip(s, c,
              "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$7\r\nLIB-VER\r\n$3\r\n1.2\r\n",
              "+OK\r\n");
    {
        const char req[] = "*2\r\n$6\r\nCLIENT\r\n$4\r\nINFO\r\n";
        DD_CHECK_EQ_INT((long long)(sizeof(req) - 1),
                        (long long)pal_send(c, req, sizeof(req) - 1));
    }
    while (got < sizeof(buf) - 1 && iter++ < 10000) {
        ptrdiff_t n;
        server_run_once(s, 50);
        n = pal_recv(c, buf + got, sizeof(buf) - 1 - got);
        if (n > 0)
            got += (size_t)n;
        if (got != 0 && buf[got - 2] == '\r' && buf[got - 1] == '\n')
            break;
    }
    buf[got] = '\0';
    DD_CHECK(strstr(buf, "lib-name=foo") != NULL);
    DD_CHECK(strstr(buf, "lib-ver=1.2") != NULL);
    {
        char longv[64];
        char req[512];
        char listbuf[512];
        size_t listgot = 0;
        memset(longv, 'x', sizeof(longv) - 1);
        longv[sizeof(longv) - 1] = '\0';
        (void)snprintf(req, sizeof(req),
                       "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$8\r\nLIB-NAME\r\n$63\r\n%s\r\n",
                       longv);
        roundtrip(s, c, req, "+OK\r\n");
        (void)snprintf(req, sizeof(req),
                       "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$7\r\nLIB-VER\r\n$63\r\n%s\r\n",
                       longv);
        roundtrip(s, c, req, "+OK\r\n");
        {
            const char info_req[] = "*2\r\n$6\r\nCLIENT\r\n$4\r\nINFO\r\n";
            got = 0;
            DD_CHECK_EQ_INT((long long)(sizeof(info_req) - 1),
                            (long long)pal_send(c, info_req,
                                                 sizeof(info_req) - 1));
            iter = 0;
            while (got < sizeof(buf) - 1 && iter++ < 10000) {
                ptrdiff_t n;
                server_run_once(s, 50);
                n = pal_recv(c, buf + got, sizeof(buf) - 1 - got);
                if (n > 0)
                    got += (size_t)n;
                if (got >= 2 && buf[got - 2] == '\r' && buf[got - 1] == '\n')
                    break;
            }
            DD_CHECK(got <= 191);
        }
        {
            const char list_req[] = "*2\r\n$6\r\nCLIENT\r\n$4\r\nLIST\r\n";
            int list_iter = 0;
            DD_CHECK_EQ_INT((long long)(sizeof(list_req) - 1),
                            (long long)pal_send(c, list_req,
                                                 sizeof(list_req) - 1));
            while (listgot < sizeof(listbuf) - 1 && list_iter++ < 10000) {
                ptrdiff_t n;
                server_run_once(s, 50);
                n = pal_recv(c, listbuf + listgot,
                             sizeof(listbuf) - 1 - listgot);
                if (n > 0)
                    listgot += (size_t)n;
                if (listgot >= 2 && listbuf[listgot - 2] == '\r' &&
                    listbuf[listgot - 1] == '\n')
                    break;
            }
            listbuf[listgot] = '\0';
            DD_CHECK(strstr(listbuf, "lib-name=") != NULL);
            DD_CHECK(strstr(listbuf, "lib-ver=") != NULL);
        }
    }
    roundtrip(s, c,
              "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$8\r\nLIB-NAME\r\n$1\r\n \r\n",
              "-ERR client info value cannot contain spaces, newlines or special characters.\r\n");
    {
        const char req[] = "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$3\r\nBAD\r\n$1\r\nx\r\n";
        char errbuf[128];
        size_t errgot = 0;
        int erriter = 0;
        DD_CHECK_EQ_INT((long long)(sizeof(req) - 1),
                        (long long)pal_send(c, req, sizeof(req) - 1));
        while (errgot < sizeof(errbuf) - 1 && erriter++ < 10000) {
            ptrdiff_t n;
            server_run_once(s, 50);
            n = pal_recv(c, errbuf + errgot, sizeof(errbuf) - 1 - errgot);
            if (n > 0)
                errgot += (size_t)n;
            if (errgot >= 2 && errbuf[errgot - 2] == '\r' &&
                errbuf[errgot - 1] == '\n')
                break;
        }
        errbuf[errgot] = '\0';
        DD_CHECK(strstr(errbuf, "Unrecognized option 'BAD'") != NULL);
    }
    roundtrip(s, c,
              "*4\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$8\r\nLIB-NAME\r\n$0\r\n\r\n",
              "+OK\r\n");
    pal_close(c);
    server_destroy(s);
}

static void test_monitor_stream(void)
{
    server *s = make_server();
    pal_socket_t mon, src;
    char buf[512];
    size_t got = 0;
    int i;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    mon = connect_client(s);
    src = connect_client(s);
    roundtrip(s, mon, "*1\r\n$7\r\nMONITOR\r\n", "+OK\r\n");
    roundtrip(s, src, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
              "+OK\r\n");
    for (i = 0; i < 100 && got == 0; i++) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(mon, buf + got, sizeof(buf) - got - 1);
        if (n > 0)
            got += (size_t)n;
    }
    buf[got] = '\0';
    DD_CHECK(got > 0);
    DD_CHECK(strstr(buf, "SET") != NULL);
    DD_CHECK(strstr(buf, "\"k\"") != NULL);
    pal_close(mon);
    pal_close(src);
    server_destroy(s);
}

static void test_hotkeys_sampled_key_metrics(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[1024];
    size_t got = 0;
    int i;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);
    roundtrip(s, c,
              "*9\r\n$7\r\nHOTKEYS\r\n$5\r\nSTART\r\n$7\r\nMETRICS\r\n"
              "$1\r\n1\r\n$3\r\nCPU\r\n$5\r\nCOUNT\r\n$1\r\n2\r\n$6\r\nSAMPLE\r\n$1\r\n1\r\n",
              "+OK\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
              "+OK\r\n");
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", "$3\r\nbar\r\n");
    roundtrip(s, c, "*2\r\n$7\r\nHOTKEYS\r\n$3\r\nGET\r\n", "");
    for (i = 0; i < 1000 && got < sizeof(buf) - 1; i++) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, sizeof(buf) - got - 1);
        if (n > 0)
            got += (size_t)n;
        buf[got] = '\0';
        if (strstr(buf, "by-cpu-time-us") != NULL)
            break;
    }
    buf[got] = '\0';
    DD_CHECK(strstr(buf, "by-cpu-time-us") != NULL);
    DD_CHECK(strstr(buf, "foo") != NULL);
    DD_CHECK(strstr(buf, "by-cpu-time-us") != NULL);
    pal_close(c);
    server_destroy(s);
}

static void test_hotkeys_multi_key_commands(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[2048];
    size_t got = 0;
    int i;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);
    roundtrip(s, c,
              "*9\r\n$7\r\nHOTKEYS\r\n$5\r\nSTART\r\n$7\r\nMETRICS\r\n"
              "$1\r\n1\r\n$3\r\nCPU\r\n$5\r\nCOUNT\r\n$1\r\n8\r\n$6\r\nSAMPLE\r\n$1\r\n1\r\n",
              "+OK\r\n");
    roundtrip(s, c,
              "*5\r\n$4\r\nMSET\r\n$2\r\nk1\r\n$2\r\nv1\r\n$2\r\nk2\r\n$2\r\nv2\r\n",
              "+OK\r\n");
    roundtrip(s, c, "*3\r\n$4\r\nMGET\r\n$2\r\nk1\r\n$2\r\nk2\r\n",
              "*2\r\n$2\r\nv1\r\n$2\r\nv2\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nDEL\r\n$2\r\nk1\r\n$2\r\nk2\r\n",
              ":2\r\n");
    roundtrip(s, c, "*3\r\n$6\r\nEXISTS\r\n$2\r\nk1\r\n$2\r\nk2\r\n",
              ":0\r\n");
    roundtrip(s, c, "*2\r\n$7\r\nHOTKEYS\r\n$3\r\nGET\r\n", "");
    for (i = 0; i < 1000 && got < sizeof(buf) - 1; i++) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, sizeof(buf) - got - 1);
        if (n > 0)
            got += (size_t)n;
        buf[got] = '\0';
        if (strstr(buf, "k1") != NULL && strstr(buf, "k2") != NULL)
            break;
    }
    buf[got] = '\0';
    DD_CHECK(strstr(buf, "k1") != NULL);
    DD_CHECK(strstr(buf, "k2") != NULL);
    pal_close(c);
    server_destroy(s);
}

static void test_hotkeys_slots_filter(void)
{
    server *s = make_server();
    pal_socket_t c;
    char key[32], req[256], setreq[128], buf[2048], slotbuf[16];
    size_t got = 0;
    int slot = 0;
    int i;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    for (i = 0; i < 10000; i++) {
        snprintf(key, sizeof(key), "slot-key-%d", i);
        slot = (int)hash_slot(key, strlen(key));
        if (slot > 0)
            break;
    }
    c = connect_client(s);
    snprintf(slotbuf, sizeof(slotbuf), "%d", slot);
    snprintf(req, sizeof(req),
             "*10\r\n$7\r\nHOTKEYS\r\n$5\r\nSTART\r\n$7\r\nMETRICS\r\n"
             "$1\r\n1\r\n$3\r\nCPU\r\n$5\r\nCOUNT\r\n$1\r\n4\r\n"
             "$5\r\nSLOTS\r\n$1\r\n1\r\n$%zu\r\n%s\r\n",
             strlen(slotbuf), slotbuf);
    roundtrip(s, c, req, "+OK\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$9\r\nother-key\r\n$1\r\nx\r\n",
              "+OK\r\n");
    snprintf(setreq, sizeof(setreq),
             "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$1\r\ny\r\n",
             strlen(key), key);
    roundtrip(s, c, setreq, "+OK\r\n");
    roundtrip(s, c, "*2\r\n$7\r\nHOTKEYS\r\n$3\r\nGET\r\n", "");
    for (i = 0; i < 200 && got < sizeof(buf) - 1; i++) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, sizeof(buf) - got - 1);
        if (n > 0)
            got += (size_t)n;
        buf[got] = '\0';
    }
    DD_CHECK(strstr(buf, key) != NULL);
    DD_CHECK(strstr(buf, "other-key") == NULL);
    pal_close(c);
    server_destroy(s);
}

static void test_hotkeys_independent_metric_order(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[4096];
    size_t got = 0;
    int i;
    const char *cpu, *net;
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);
    roundtrip(s, c,
              "*8\r\n$7\r\nHOTKEYS\r\n$5\r\nSTART\r\n$7\r\nMETRICS\r\n"
              "$1\r\n2\r\n$3\r\nCPU\r\n$3\r\nNET\r\n$5\r\nCOUNT\r\n$1\r\n2\r\n",
              "+OK\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\nx\r\n",
              "+OK\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$7\r\nlongkey\r\n$1\r\ny\r\n",
              "+OK\r\n");
    roundtrip(s, c, "*2\r\n$7\r\nHOTKEYS\r\n$3\r\nGET\r\n", "");
    for (i = 0; i < 200 && got < sizeof(buf) - 1; i++) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, sizeof(buf) - got - 1);
        if (n > 0)
            got += (size_t)n;
        buf[got] = '\0';
    }
    cpu = strstr(buf, "by-cpu-time-us");
    net = strstr(buf, "by-net-bytes");
    DD_CHECK(cpu != NULL);
    DD_CHECK(net != NULL);
    if (cpu != NULL && net != NULL) {
        DD_CHECK(strstr(cpu, "a") < strstr(cpu, "longkey"));
        DD_CHECK(strstr(net, "longkey") < strstr(net, "a"));
    }
    pal_close(c);
    server_destroy(s);
}

static void test_pipeline(void)
{
    server *s = make_server();
    pal_socket_t c;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    /* three commands in one TCP send -> three replies */
    roundtrip(s, c,
              "*1\r\n$4\r\nPING\r\n"
              "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
              "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
              "+PONG\r\n+OK\r\n$1\r\nv\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_aof_failure_rejects_writes(void)
{
    static const char path[] = "test_server_aof_failure.aof";
    static const char misconf[] =
        "-MISCONF Errors writing to the AOF file\r\n";
    server *s;
    pal_socket_t c;
    pal_socket_t tx;
    size_t pending;
    int i;

    remove(path);
    s = make_server();
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    DD_CHECK_EQ_INT(0, server_enable_aof(s, path));
    server_test_set_aof_write_fn(s, fail_aof_write);
    c = connect_client(s);
    tx = connect_client(s);

    roundtrip(s, tx, "*1\r\n$5\r\nMULTI\r\n", "+OK\r\n");
    roundtrip(s, tx,
              "*3\r\n$3\r\nSET\r\n$6\r\nqueued\r\n$1\r\nx\r\n",
              "+QUEUED\r\n");

    /* This loop applies SET, then discovers the buffered AOF write failure. */
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n",
              "+OK\r\n");
    DD_CHECK(server_test_aof_failed(s));
    DD_CHECK(server_shutdown_requested(s));
    pending = server_test_aof_pending_bytes(s);
    DD_CHECK(pending > 0);

    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "$1\r\nv\r\n");
    for (i = 0; i < 32; i++)
        roundtrip(s, c,
                  "*3\r\n$3\r\nSET\r\n$5\r\nother\r\n$1\r\nx\r\n",
                  misconf);
    DD_CHECK_EQ_INT((long long)pending,
                    (long long)server_test_aof_pending_bytes(s));
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$5\r\nother\r\n", "$-1\r\n");
    roundtrip(s, tx, "*1\r\n$4\r\nEXEC\r\n", misconf);
    roundtrip(s, tx, "*1\r\n$7\r\nDISCARD\r\n", "+OK\r\n");
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$6\r\nqueued\r\n", "$-1\r\n");

    pal_close(tx);
    pal_close(c);
    server_destroy(s);
    remove(path);
}

static void test_aof_sync_failure_rejects_writes(void)
{
    static const char path[] = "test_server_aof_sync.aof";
    static const char misconf[] =
        "-MISCONF Errors writing to the AOF file\r\n";
    server *s;
    pal_socket_t c;

    remove(path);
    s = make_server();
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    DD_CHECK_EQ_INT(0, server_enable_aof(s, path));
    server_set_appendfsync(s, AOF_FSYNC_ALWAYS);
    pal_file_test_reset();
    pal_file_test_fail_next_sync();
    c = connect_client(s);

    /* SET applies; the loop-end flush+sync fails and latches fail-closed */
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "+OK\r\n");
    DD_CHECK(server_test_aof_failed(s));
    DD_CHECK(server_shutdown_requested(s));
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$1\r\ny\r\n", misconf);
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "$1\r\nv\r\n");

    pal_close(c);
    server_destroy(s);
    remove(path);
}

static void test_config_appendfsync_hook(void)
{
    resp_buf out;
    resp_buf_init(&out);
    DD_CHECK_EQ_INT(1, server_test_config_appendfsync(NULL, &out));
    DD_CHECK_MEM("*2\r\n$11\r\nappendfsync\r\n$8\r\neverysec\r\n", 36,
                 out.data, out.len);
    out.len = 0;
    DD_CHECK_EQ_INT(1, server_test_config_appendfsync("always", &out));
    DD_CHECK_MEM("+OK\r\n", 5, out.data, out.len);
    out.len = 0;
    DD_CHECK_EQ_INT(1, server_test_config_appendfsync("sometimes", &out));
    DD_CHECK_MEM("-ERR invalid argument for CONFIG SET 'appendfsync'\r\n",
                 52, out.data, out.len);
    resp_buf_free(&out);
}

static void test_eventless_loop_flushes_aof(void)
{
    static const char path[] = "test_server_aof_eventless.aof";
    server *s;
    resp_value argv[3];

    remove(path);
    s = make_server();
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    DD_CHECK_EQ_INT(0, server_enable_aof(s, path));
    server_test_set_aof_write_fn(s, fail_aof_write);
    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING;
    argv[0].str = "SET";
    argv[0].len = 3;
    argv[1].type = RESP_BULK_STRING;
    argv[1].str = "k";
    argv[1].len = 1;
    argv[2].type = RESP_BULK_STRING;
    argv[2].str = "v";
    argv[2].len = 1;
    server_aof_log_cmd(s, 0, argv, 3);

    (void)server_run_once(s, 0);
    DD_CHECK(server_test_aof_failed(s));
    DD_CHECK(server_shutdown_requested(s));

    server_destroy(s);
    remove(path);
}

static void test_mget_missing_key(void)
{
    server *s = make_server();
    pal_socket_t c;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n", "+OK\r\n");
    roundtrip(s, c, "*3\r\n$4\r\nMGET\r\n$1\r\na\r\n$7\r\nmissing\r\n",
              "*2\r\n$1\r\n1\r\n$-1\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_unknown_command(void)
{
    server *s = make_server();
    pal_socket_t c;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    roundtrip(s, c, "*1\r\n$6\r\nFOOCMD\r\n",
              "-ERR unknown command 'FOOCMD'\r\n");

    pal_close(c);
    server_destroy(s);
}

static void test_split_delivery(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[64];
    ptrdiff_t n;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "+OK\r\n");

    /* first half of a GET: server must not reply yet */
    DD_CHECK_EQ_INT(10, pal_send(c, "*2\r\n$3\r\nGE", 10));
    server_run_once(s, 50);
    n = pal_recv(c, buf, sizeof(buf));
    DD_CHECK_EQ_INT(-1, n);
    DD_CHECK(pal_would_block(pal_socket_error()));

    /* second half completes the command */
    DD_CHECK_EQ_INT(12, pal_send(c, "T\r\n$3\r\nfoo\r\n", 12));
    n = -1;
    while (n <= 0 && iter < 10000) {
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf, sizeof(buf));
    }
    DD_CHECK_EQ_INT(9, n);
    DD_CHECK_MEM("$3\r\nbar\r\n", 9, buf, 9);

    pal_close(c);
    server_destroy(s);
}

static void test_many_connections(void)
{
    server *s = make_server();
    int i;
    DD_CHECK(s != NULL);

    for (i = 0; i < 32; i++) {
        pal_socket_t c = connect_client(s);
        roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\nx\r\n$1\r\ny\r\n", "+OK\r\n");
        roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$1\r\nx\r\n", "$1\r\ny\r\n");
        pal_close(c);
        /* let the server observe the close */
        server_run_once(s, 5);
    }

    server_destroy(s);
}

static void test_protocol_error_closes_conn(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[64];
    size_t got = 0;
    ptrdiff_t n;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    /* inline (non-array) command -> protocol error, then server closes */
    DD_CHECK_EQ_INT(6, pal_send(c, "PING\r\n", 6));
    while (got < 21 && iter < 10000) {
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf + got, 21 - got);
        if (n > 0)
            got += (size_t)n;
        else if (n == 0)
            break;
    }
    DD_CHECK_EQ_INT(21, got);
    DD_CHECK_MEM("-ERR Protocol error\r\n", 21, buf, got);

    /* connection closed by the server: next recv reports orderly close */
    n = -1;
    iter = 0;
    while (n < 0 && iter < 10000) {
        iter++;
        server_run_once(s, 10);
        n = pal_recv(c, buf, sizeof(buf));
    }
    DD_CHECK_EQ_INT(0, n);

    pal_close(c);
    server_destroy(s);
}

static void test_quit_closes_connection(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[64];
    ptrdiff_t n;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    /* the ack is delivered, then the server closes the connection */
    roundtrip(s, c, "*1\r\n$4\r\nQUIT\r\n", "+OK\r\n");
    n = -1;
    while (n < 0 && iter < 10000) {
        iter++;
        server_run_once(s, 10);
        n = pal_recv(c, buf, sizeof(buf));
    }
    DD_CHECK_EQ_INT(0, n);

    pal_close(c);
    server_destroy(s);
}

static void test_quit_discards_pipelined_tail(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[64];
    size_t got = 0;
    ptrdiff_t n;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    /* PING after QUIT in the same send is never answered */
    roundtrip(s, c,
              "*1\r\n$4\r\nQUIT\r\n"
              "*1\r\n$4\r\nPING\r\n",
              "+OK\r\n");
    n = -1;
    while (n < 0 && iter < 10000) {
        iter++;
        server_run_once(s, 10);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT(0, n); /* EOF, and no +PONG arrived */
    DD_CHECK_EQ_INT(0, (long long)got);

    pal_close(c);
    server_destroy(s);
}

static void test_request_limit_fragmentation(void)
{
    server *s = make_server();
    pal_socket_t c;
    char req[128];
    char buf[64];
    size_t sent = 0;
    ptrdiff_t n;
    int iter = 0;
    DD_CHECK(s != NULL);
    server_set_proto_max_request_bytes(s, sizeof(req));
    c = connect_client(s);
    memset(req, 'x', sizeof(req));
    memcpy(req, "*1\r\n$120\r\n", 10);
    while (sent < sizeof(req)) {
        size_t part = sizeof(req) - sent;
        if (part > 17)
            part = 17;
        n = pal_send(c, req + sent, part);
        if (n > 0)
            sent += (size_t)n;
        else if (n == 0)
            break;
        server_run_once(s, 10);
    }
    n = -1;
    while (iter++ < 10000) {
        server_run_once(s, 5);
        n = pal_recv(c, buf, sizeof(buf));
        if (n == 0)
            break;
    }
    DD_CHECK_EQ_INT(0, n);
    pal_close(c);
    server_destroy(s);
}

static void test_iouring_multishot_complete_at_limit(void)
{
    server *s;
    pal_socket_t c;
    const char *one = "*1\r\n$4\r\nPING\r\n";
    char req[14 * 5];
    char buf[128];
    size_t sent = 0, got = 0;
    int i, iter = 0;

    if (g_backend != SERVER_BACKEND_IOURING_OP)
        return;
    s = make_server();
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    server_set_proto_max_request_bytes(s, 28);
    c = connect_client(s);
    for (i = 0; i < 5; i++)
        memcpy(req + i * 14, one, strlen(one));
    while (sent < sizeof(req) && iter++ < 10000) {
        ptrdiff_t n = pal_send(c, req + sent, sizeof(req) - sent);
        if (n > 0)
            sent += (size_t)n;
        server_run_once(s, 5);
    }
    DD_CHECK_EQ_INT((long long)sizeof(req), (long long)sent);
    iter = 0;
    while (got < 35 && iter++ < 10000) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT(35, (long long)got);
    for (i = 0; i < 5; i++)
        DD_CHECK_MEM("+PONG\r\n", 7, buf + i * 7, 7);
    pal_close(c);
    server_destroy(s);
}

static void test_readiness_complete_at_limit(void)
{
    server *s;
    pal_socket_t c;
    const char req[] = "*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n";
    char buf[32];
    size_t got = 0;
    int iter = 0;

    if (g_backend != SERVER_BACKEND_SELECT)
        return;
    s = make_server();
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    server_set_proto_max_request_bytes(s, sizeof(req) - 1);
    c = connect_client(s);
    DD_CHECK_EQ_INT((long long)(sizeof(req) - 1),
                    (long long)pal_send(c, req, sizeof(req) - 1));
    while (got < 14 && iter++ < 10000) {
        ptrdiff_t n;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT(14, (long long)got);
    DD_CHECK_MEM("+PONG\r\n+PONG\r\n", 14, buf, got);
    pal_close(c);
    server_destroy(s);
}

static void test_proactor_destroy_with_open_connection(void)
{
    server *s;
    pal_socket_t c;
    int i;
    if (g_backend != SERVER_BACKEND_IOCP &&
        g_backend != SERVER_BACKEND_IOURING_OP)
        return;
    s = make_server();
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;
    c = connect_client(s);
    (void)pal_send(c, "*1\r\n$4\r\nPING\r\n", 14);
    for (i = 0; i < 4; i++)
        (void)server_run_once(s, 1);
    pal_close(c);
    server_destroy(s);
}

static void test_pubsub_over_socket(void)
{
    server *s = make_server();
    pal_socket_t a, b;
    char buf[256];
    const char *want = "*3\r\n$7\r\nmessage\r\n$2\r\nch\r\n$5\r\nhello\r\n";
    size_t wlen = strlen(want), got = 0;
    ptrdiff_t n;
    int iter = 0;
    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);

    /* A subscribes; B publishes; A gets the push without sending */
    roundtrip(s, a, "*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n",
              "*3\r\n$9\r\nsubscribe\r\n$2\r\nch\r\n:1\r\n");
    roundtrip(s, b, "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n",
              ":1\r\n");
    while (got < wlen && iter < 10000) {
        iter++;
        server_run_once(s, 50);
        n = pal_recv(a, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)wlen, (long long)got);
    DD_CHECK_MEM(want, wlen, buf, got);

    /* closing A unsubscribes it: nobody receives anymore */
    pal_close(a);
    server_run_once(s, 50);
    roundtrip(s, b, "*3\r\n$7\r\nPUBLISH\r\n$2\r\nch\r\n$5\r\nhello\r\n",
              ":0\r\n");

    pal_close(b);
    server_destroy(s);
}

/* Pump the server until exactly strlen(want) push bytes arrive on c. */
static void expect_push(server *s, pal_socket_t c, const char *want)
{
    char buf[1024];
    size_t wlen = strlen(want), got = 0;
    int iter = 0;
    DD_CHECK(wlen <= sizeof(buf));
    while (got < wlen && iter < 10000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)wlen, (long long)got);
    DD_CHECK_MEM(want, wlen, buf, got);
}

/* tiny local substring search (memmem is GNU-only) */
static int buf_contains(const char *hay, size_t hlen, const char *needle,
                        size_t nlen)
{
    size_t i;
    if (nlen > hlen)
        return 0;
    for (i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return 1;
    return 0;
}

/* Two pushes whose relative order depends on hash-table iteration: read
 * both frames, then check each is present. */
static void expect_push2(server *s, pal_socket_t c, const char *f1,
                         const char *f2)
{
    char buf[1024];
    size_t l1 = strlen(f1), l2 = strlen(f2), got = 0;
    int iter = 0;
    DD_CHECK(l1 + l2 <= sizeof(buf));
    while (got < l1 + l2 && iter < 10000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 50);
        n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT((long long)(l1 + l2), (long long)got);
    DD_CHECK(buf_contains(buf, got, f1, l1));
    DD_CHECK(buf_contains(buf, got, f2, l2));
}

static void test_psubscribe_over_socket(void)
{
    server *s = make_server();
    pal_socket_t a, b, c;
    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);
    c = connect_client(s);

    /* psubscribe confirm frames report the total subscription count;
     * a duplicate pattern is acknowledged but not double-registered */
    roundtrip(s, a, "*2\r\n$10\r\nPSUBSCRIBE\r\n$6\r\nnews.*\r\n",
              "*3\r\n$10\r\npsubscribe\r\n$6\r\nnews.*\r\n:1\r\n");
    roundtrip(s, a, "*2\r\n$10\r\nPSUBSCRIBE\r\n$6\r\nnews.*\r\n",
              "*3\r\n$10\r\npsubscribe\r\n$6\r\nnews.*\r\n:1\r\n");
    roundtrip(s, b, "*2\r\n$9\r\nSUBSCRIBE\r\n$9\r\nnews.tech\r\n",
              "*3\r\n$9\r\nsubscribe\r\n$9\r\nnews.tech\r\n:1\r\n");

    /* mixed delivery: channel subscriber gets "message", the pattern
     * subscriber gets "pmessage"; PUBLISH counts both */
    roundtrip(s, c, "*3\r\n$7\r\nPUBLISH\r\n$9\r\nnews.tech\r\n$5\r\nhello\r\n",
              ":2\r\n");
    expect_push(s, a, "*4\r\n$8\r\npmessage\r\n$6\r\nnews.*\r\n$9\r\nnews.tech"
                      "\r\n$5\r\nhello\r\n");
    expect_push(s, b, "*3\r\n$7\r\nmessage\r\n$9\r\nnews.tech\r\n$5\r\nhello"
                      "\r\n");

    /* a second matching pattern yields one pmessage per (conn, pattern) */
    roundtrip(s, a, "*2\r\n$10\r\nPSUBSCRIBE\r\n$6\r\n*.tech\r\n",
              "*3\r\n$10\r\npsubscribe\r\n$6\r\n*.tech\r\n:2\r\n");
    roundtrip(s, c, "*3\r\n$7\r\nPUBLISH\r\n$9\r\nnews.tech\r\n$2\r\nyo\r\n",
              ":3\r\n");
    expect_push2(s, a,
                 "*4\r\n$8\r\npmessage\r\n$6\r\nnews.*\r\n$9\r\nnews.tech"
                 "\r\n$2\r\nyo\r\n",
                 "*4\r\n$8\r\npmessage\r\n$6\r\n*.tech\r\n$9\r\nnews.tech"
                 "\r\n$2\r\nyo\r\n");
    expect_push(s, b, "*3\r\n$7\r\nmessage\r\n$9\r\nnews.tech\r\n$2\r\nyo\r\n");
    roundtrip(s, c, "*3\r\n$7\r\nPUBLISH\r\n$5\r\nother\r\n$1\r\nx\r\n",
              ":0\r\n");

    /* PUBSUB introspection */
    roundtrip(s, c, "*2\r\n$6\r\nPUBSUB\r\n$6\r\nNUMPAT\r\n", ":2\r\n");
    roundtrip(s, c, "*2\r\n$6\r\nPUBSUB\r\n$8\r\nCHANNELS\r\n",
              "*1\r\n$9\r\nnews.tech\r\n");
    roundtrip(s, c, "*3\r\n$6\r\nPUBSUB\r\n$8\r\nCHANNELS\r\n$2\r\nn*\r\n",
              "*1\r\n$9\r\nnews.tech\r\n");
    roundtrip(s, c, "*3\r\n$6\r\nPUBSUB\r\n$8\r\nCHANNELS\r\n$2\r\nz*\r\n",
              "*0\r\n");
    roundtrip(s, c,
              "*4\r\n$6\r\nPUBSUB\r\n$6\r\nNUMSUB\r\n$9\r\nnews.tech\r\n"
              "$4\r\nnone\r\n",
              "*4\r\n$9\r\nnews.tech\r\n:1\r\n$4\r\nnone\r\n:0\r\n");

    /* subscribed mode: regular commands are rejected, PSUBSCRIBE allowed */
    roundtrip(s, a, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
              "-ERR Can't execute 'get': only (P)SUBSCRIBE / (P)UNSUBSCRIBE / "
              "PING / QUIT / SHUTDOWN are allowed in this context\r\n");
    roundtrip(s, a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    /* punsubscribe: unknown patterns are acknowledged with the count
     * unchanged; the no-arg form drops every remaining pattern */
    roundtrip(s, a, "*2\r\n$12\r\nPUNSUBSCRIBE\r\n$6\r\nnews.*\r\n",
              "*3\r\n$12\r\npunsubscribe\r\n$6\r\nnews.*\r\n:1\r\n");
    roundtrip(s, a, "*2\r\n$12\r\nPUNSUBSCRIBE\r\n$6\r\nnosuch\r\n",
              "*3\r\n$12\r\npunsubscribe\r\n$6\r\nnosuch\r\n:1\r\n");
    roundtrip(s, a, "*1\r\n$12\r\nPUNSUBSCRIBE\r\n",
              "*3\r\n$12\r\npunsubscribe\r\n$6\r\n*.tech\r\n:0\r\n");
    roundtrip(s, c, "*2\r\n$6\r\nPUBSUB\r\n$6\r\nNUMPAT\r\n", ":0\r\n");
    roundtrip(s, a, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "+OK\r\n");

    /* the count mixes channel and pattern subscriptions */
    roundtrip(s, b, "*2\r\n$10\r\nPSUBSCRIBE\r\n$1\r\nx\r\n",
              "*3\r\n$10\r\npsubscribe\r\n$1\r\nx\r\n:2\r\n");
    roundtrip(s, b, "*2\r\n$12\r\nPUNSUBSCRIBE\r\n$1\r\nx\r\n",
              "*3\r\n$12\r\npunsubscribe\r\n$1\r\nx\r\n:1\r\n");

    /* closing a pattern subscriber unregisters its patterns */
    roundtrip(s, a, "*2\r\n$10\r\nPSUBSCRIBE\r\n$6\r\nnews.*\r\n",
              "*3\r\n$10\r\npsubscribe\r\n$6\r\nnews.*\r\n:1\r\n");
    pal_close(a);
    server_run_once(s, 50);
    roundtrip(s, c, "*2\r\n$6\r\nPUBSUB\r\n$6\r\nNUMPAT\r\n", ":0\r\n");
    roundtrip(s, c, "*3\r\n$7\r\nPUBLISH\r\n$9\r\nnews.tech\r\n$5\r\nhello\r\n",
              ":1\r\n");

    pal_close(b);
    pal_close(c);
    server_destroy(s);
}

static void test_auth_over_socket(void)
{
    server *s = make_server();
    pal_socket_t c;
    DD_CHECK(s != NULL);
    server_set_requirepass(s, "pw");
    c = connect_client(s);

    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
              "-NOAUTH Authentication required.\r\n");
    roundtrip(s, c, "*2\r\n$4\r\nAUTH\r\n$3\r\nbad\r\n",
              "-WRONGPASS invalid username-password pair or user is "
              "disabled.\r\n");
    roundtrip(s, c, "*2\r\n$4\r\nAUTH\r\n$2\r\npw\r\n", "+OK\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "+OK\r\n");
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "$1\r\nv\r\n");

    /* a fresh connection must authenticate again */
    {
        pal_socket_t c2 = connect_client(s);
        roundtrip(s, c2, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n",
                  "-NOAUTH Authentication required.\r\n");
        pal_close(c2);
    }

    pal_close(c);
    server_destroy(s);
}

static void test_shutdown_command(void)
{
    server *s = make_server();
    pal_socket_t c;
    int iter = 0;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    DD_CHECK_EQ_INT(0, server_shutdown_requested(s));
    DD_CHECK_EQ_INT(18, pal_send(c, "*1\r\n$8\r\nSHUTDOWN\r\n", 18));
    while (!server_shutdown_requested(s) && iter < 10000) {
        iter++;
        server_run_once(s, 50);
    }
    DD_CHECK_EQ_INT(1, server_shutdown_requested(s));

    pal_close(c);
    server_destroy(s);
}

static void test_connection_buf_pool(void)
{
    server *s = make_server();
    const buf_pool *pool;
    pal_socket_t c;
    DD_CHECK(s != NULL);
    pool = server_buf_pool(s);
    DD_CHECK(pool != NULL);
    DD_CHECK(pool->sizes[2] == 64 * 1024);

    c = connect_client(s);
    roundtrip(s, c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    pal_close(c);
    /* Let the server observe the close and return connection buffers. */
    server_run_once(s, 5);
    DD_CHECK(pool->hits > 0 || pool->allocs > 0);

    server_destroy(s);
}

/* send everything, pumping the server between partial sends */
static void send_all(server *s, pal_socket_t c, const char *buf, size_t len)
{
    size_t sent = 0;
    int iter = 0;
    while (sent < len && iter < 100000) {
        ptrdiff_t n = pal_send(c, buf + sent, len - sent);
        iter++;
        if (n > 0)
            sent += (size_t)n;
        else
            server_run_once(s, 5);
    }
    DD_CHECK_EQ_INT((long long)len, (long long)sent);
}

/* read exactly len bytes, pumping the server between reads */
static size_t read_all(server *s, pal_socket_t c, char *buf, size_t len,
                       size_t chunk)
{
    size_t got = 0;
    int iter = 0;
    while (got < len && iter < 200000) {
        size_t want = len - got < chunk ? len - got : chunk;
        ptrdiff_t n;
        iter++;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, want);
        if (n > 0)
            got += (size_t)n;
        if (n == 0)
            break;
    }
    return got;
}

static void test_blocking_pop_over_socket(void)
{
    static const char blpop[] =
        "*3\r\n$5\r\nBLPOP\r\n$1\r\nl\r\n$1\r\n0\r\n";
    static const char rpush[] =
        "*3\r\n$5\r\nRPUSH\r\n$1\r\nl\r\n$1\r\nx\r\n";
    static const char want[] = "*2\r\n$1\r\nl\r\n$1\r\nx\r\n";
    static const char blpop_timeout[] =
        "*3\r\n$5\r\nBLPOP\r\n$5\r\nnokey\r\n$3\r\n0.1\r\n";
    server *s = make_server();
    pal_socket_t a, b;
    char buf[64];
    size_t got = 0;
    int iter = 0;
    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);

    /* BLPOP blocks until another connection pushes onto the list. */
    send_all(s, a, blpop, sizeof(blpop) - 1);
    roundtrip(s, b, rpush, ":1\r\n");
    expect_push(s, a, want);

    /* A short timeout expires without a writer and replies a null bulk. */
    send_all(s, a, blpop_timeout, sizeof(blpop_timeout) - 1);
    got = 0;
    iter = 0;
    while (got < 5 && iter < 300) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 10);
        n = pal_recv(a, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT(5, (int)got);
    DD_CHECK_MEM("$-1\r\n", 5, buf, got);

    pal_close(a);
    pal_close(b);
    server_destroy(s);
}

static void test_pipeline_2000(void)
{
    enum { NCMD = 2000, CMDLEN = 14, REPLYLEN = 7 };
    server *s = make_server();
    pal_socket_t c;
    char *req, *rep;
    int i, ok = 1;
    DD_CHECK(s != NULL);
    c = connect_client(s);

    req = (char *)malloc((size_t)NCMD * CMDLEN);
    rep = (char *)malloc((size_t)NCMD * REPLYLEN);
    DD_CHECK(req != NULL && rep != NULL);
    for (i = 0; i < NCMD; i++)
        memcpy(req + (size_t)i * CMDLEN, "*1\r\n$4\r\nPING\r\n", CMDLEN);

    send_all(s, c, req, (size_t)NCMD * CMDLEN);
    DD_CHECK_EQ_INT((long long)((size_t)NCMD * REPLYLEN),
                    (long long)read_all(s, c, rep,
                                        (size_t)NCMD * REPLYLEN, 65536));
    for (i = 0; i < NCMD; i++)
        if (memcmp(rep + (size_t)i * REPLYLEN, "+PONG\r\n", REPLYLEN) != 0)
            ok = 0;
    DD_CHECK_EQ_INT(1, ok);

    free(rep);
    free(req);
    pal_close(c);
    server_destroy(s);
}

/* 256 KiB value: big enough to exercise the streaming reply path. */
#define BIGVAL_LEN (256 * 1024)

static char *make_bigval_cmd(const char *key, size_t *outlen)
{
    char hdr[64];
    int hl = snprintf(hdr, sizeof(hdr), "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n"
                                       "$%d\r\n",
                      strlen(key), key, BIGVAL_LEN);
    size_t total = (size_t)hl + BIGVAL_LEN + 2;
    char *cmd = (char *)malloc(total);
    DD_CHECK(cmd != NULL);
    memcpy(cmd, hdr, (size_t)hl);
    memset(cmd + hl, 'x', BIGVAL_LEN);
    cmd[total - 2] = '\r';
    cmd[total - 1] = '\n';
    *outlen = total;
    return cmd;
}

static void test_slow_client_no_stall(void)
{
    server *s = make_server();
    pal_socket_t a, b, c;
    char *cmd, *big;
    size_t cmdlen, got;
    char pb[16];
    int iter = 0;
    char exp_hdr[16];
    int ehl;
    size_t exp_total;

    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);
    c = connect_client(s);

    /* A writes a 4 MiB value */
    cmd = make_bigval_cmd("big", &cmdlen);
    send_all(s, a, cmd, cmdlen);
    free(cmd);
    DD_CHECK_EQ_INT(5, (long long)read_all(s, a, pb, 5, 16));
    DD_CHECK_MEM("+OK\r\n", 5, pb, 5);

    /* B asks for it and does NOT read (slow/stalled client) */
    DD_CHECK_EQ_INT(22, pal_send(b, "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n", 22));

    /* C must still be served promptly (bounded poll) */
    DD_CHECK_EQ_INT(14, pal_send(c, "*1\r\n$4\r\nPING\r\n", 14));
    got = 0;
    while (got < 7 && iter < 1000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 5);
        n = pal_recv(c, pb + got, sizeof(pb) - got);
        if (n > 0)
            got += (size_t)n;
    }
    DD_CHECK_EQ_INT(7, (long long)got);
    DD_CHECK_MEM("+PONG\r\n", 7, pb, 7);
    DD_CHECK(iter < 200); /* prompt: no head-of-line blocking */

    /* drain B: the whole reply arrives intact */
    ehl = snprintf(exp_hdr, sizeof(exp_hdr), "$%d\r\n", BIGVAL_LEN);
    exp_total = (size_t)ehl + BIGVAL_LEN + 2;
    big = (char *)malloc(exp_total);
    DD_CHECK(big != NULL);
    got = read_all(s, b, big, exp_total, 65536);
    DD_CHECK_EQ_INT((long long)exp_total, (long long)got);
    DD_CHECK_MEM(exp_hdr, (size_t)ehl, big, (size_t)ehl);
    DD_CHECK(big[ehl] == 'x' && big[exp_total - 3] == 'x');
    DD_CHECK(big[exp_total - 2] == '\r' && big[exp_total - 1] == '\n');

    free(big);
    pal_close(c);
    pal_close(b);
    pal_close(a);
    server_destroy(s);
}

static void test_partial_reads(void)
{
    server *s = make_server();
    pal_socket_t a, b;
    char *cmd, *big;
    size_t cmdlen, got;
    char pb[16];
    char exp_hdr[16];
    int ehl;
    size_t exp_total;

    DD_CHECK(s != NULL);
    a = connect_client(s);
    b = connect_client(s);

    cmd = make_bigval_cmd("big", &cmdlen);
    send_all(s, a, cmd, cmdlen);
    free(cmd);
    DD_CHECK_EQ_INT(5, (long long)read_all(s, a, pb, 5, 16));

    DD_CHECK_EQ_INT(22, pal_send(b, "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n", 22));

    /* read the 4 MiB reply in small chunks, interleaved with the loop */
    ehl = snprintf(exp_hdr, sizeof(exp_hdr), "$%d\r\n", BIGVAL_LEN);
    exp_total = (size_t)ehl + BIGVAL_LEN + 2;
    big = (char *)malloc(exp_total);
    DD_CHECK(big != NULL);
    got = read_all(s, b, big, exp_total, 4096);
    DD_CHECK_EQ_INT((long long)exp_total, (long long)got);
    DD_CHECK_MEM(exp_hdr, (size_t)ehl, big, (size_t)ehl);
    DD_CHECK(big[exp_total - 3] == 'x');

    free(big);
    pal_close(b);
    pal_close(a);
    server_destroy(s);
}

static long long info_val(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    if (p == NULL)
        return -1;
    return strtoll(p + strlen(key), NULL, 10);
}

static void test_info_io_counters(void)
{
    server *s = make_server();
    pal_socket_t c;
    char buf[8192];
    size_t got = 0;
    int idle = 0, iter = 0;

    DD_CHECK(s != NULL);
    c = connect_client(s);
    roundtrip(s, c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    roundtrip(s, c, "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n", "+OK\r\n");
    roundtrip(s, c, "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n", "$1\r\nv\r\n");

    /* fetch the full INFO (pump until the reply stops growing) */
    DD_CHECK_EQ_INT(14, pal_send(c, "*1\r\n$4\r\nINFO\r\n", 14));
    while (idle < 3 && iter < 2000) {
        ptrdiff_t n;
        iter++;
        server_run_once(s, 5);
        n = pal_recv(c, buf + got, sizeof(buf) - got - 1);
        if (n > 0) {
            got += (size_t)n;
            idle = 0;
        } else {
            idle++;
        }
    }
    buf[got] = '\0';
    /* always-on server IO counters (Phase 27): present and nonzero */
    DD_CHECK(info_val(buf, "io_loops:") > 0);
    DD_CHECK(info_val(buf, "io_events:") > 0);
    DD_CHECK(info_val(buf, "io_reads:") > 0);
    DD_CHECK(info_val(buf, "io_writes:") > 0);
    DD_CHECK(info_val(buf, "io_bytes_read:") > 0);
    DD_CHECK(info_val(buf, "io_bytes_written:") > 0);
    /* the running INFO counts itself only after rendering: 3 visible */
    DD_CHECK(info_val(buf, "total_commands:") >= 3);

    pal_close(c);
    server_destroy(s);
}

static void run_all_tests(void)
{
    DD_RUN(test_cluster_save_close_failure_preserves_state);
    DD_RUN(test_persistence_paths_reject_truncation);
    DD_RUN(test_ping_set_get);
    DD_RUN(test_client_reply_modes);
    DD_RUN(test_client_tracking_redirect_validation);
    DD_RUN(test_client_setinfo_metadata);
    DD_RUN(test_monitor_stream);
    DD_RUN(test_hotkeys_sampled_key_metrics);
    DD_RUN(test_hotkeys_multi_key_commands);
    DD_RUN(test_hotkeys_slots_filter);
    DD_RUN(test_hotkeys_independent_metric_order);
    DD_RUN(test_pipeline);
    DD_RUN(test_aof_failure_rejects_writes);
    DD_RUN(test_aof_sync_failure_rejects_writes);
    DD_RUN(test_config_appendfsync_hook);
    DD_RUN(test_eventless_loop_flushes_aof);
    DD_RUN(test_mget_missing_key);
    DD_RUN(test_unknown_command);
    DD_RUN(test_split_delivery);
    DD_RUN(test_many_connections);
    DD_RUN(test_protocol_error_closes_conn);
    DD_RUN(test_quit_closes_connection);
    DD_RUN(test_quit_discards_pipelined_tail);
    DD_RUN(test_request_limit_fragmentation);
    DD_RUN(test_iouring_multishot_complete_at_limit);
    DD_RUN(test_readiness_complete_at_limit);
    DD_RUN(test_proactor_destroy_with_open_connection);
    DD_RUN(test_pubsub_over_socket);
    DD_RUN(test_psubscribe_over_socket);
    DD_RUN(test_auth_over_socket);
    DD_RUN(test_shutdown_command);
    DD_RUN(test_blocking_pop_over_socket);
    DD_RUN(test_connection_buf_pool);
    DD_RUN(test_pipeline_2000);
    DD_RUN(test_slow_client_no_stall);
    DD_RUN(test_partial_reads);
    DD_RUN(test_info_io_counters);
}

int main(void)
{
    DD_CHECK_EQ_INT(0, pal_socket_init());
    if (getenv("DDUP_TEST_IOCP_ONLY") != NULL) {
        g_backend = SERVER_BACKEND_IOCP;
        printf("=== backend: IOCP ===\n");
        run_all_tests();
        pal_socket_cleanup();
        return DD_TEST_SUMMARY();
    }
    if (getenv("DDUP_TEST_IOURING_OP_ONLY") != NULL) {
        g_backend = SERVER_BACKEND_IOURING_OP;
        printf("=== backend: io_uring op-mode ===\n");
        run_all_tests();
        pal_socket_cleanup();
        return DD_TEST_SUMMARY();
    }
    g_backend = SERVER_BACKEND_SELECT;
    printf("=== backend: readiness (select) ===\n");
    run_all_tests();
    {
        /* run the whole suite again on the IOCP backend when available */
        pal_iocp *probe = pal_iocp_create();
        if (probe != NULL) {
            pal_iocp_free(probe);
            g_backend = SERVER_BACKEND_IOCP;
            printf("=== backend: IOCP ===\n");
            run_all_tests();
        }
    }
    {
        /* and once more on io_uring when the kernel offers it (Linux) */
        pal_loop *probe = pal_loop_create_iouring();
        if (probe != NULL) {
            pal_loop_free(probe);
            g_backend = SERVER_BACKEND_IOURING;
            printf("=== backend: io_uring ===\n");
            run_all_tests();
        }
    }
    {
        /* and the io_uring op-mode (proactor) flavor, same probe rule */
        pal_iouring *probe = pal_iouring_create();
        if (probe != NULL) {
            pal_iouring_free(probe);
            g_backend = SERVER_BACKEND_IOURING_OP;
            printf("=== backend: io_uring op-mode ===\n");
            run_all_tests();
        }
    }
    pal_socket_cleanup();
    return DD_TEST_SUMMARY();
}
