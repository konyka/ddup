/* test_redbus_bus.c - dual-protocol cluster bus: cluster-bus-protocol
 * config and two-server convergence over the real Redis wire format. */
#include <stdio.h>
#include <string.h>

#include "core/config.h"
#include "core/hashslot.h"
#include "pal/pal_socket.h"
#include "server/server.h"
#include "test.h"

#define IDA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define IDB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void test_config_bus_protocol(void)
{
    ddup_config cfg;
    config_init(&cfg);
    DD_CHECK_STR("ddup", cfg.cluster_bus_protocol);
    DD_CHECK_EQ_INT(0, config_apply(&cfg, "cluster-bus-protocol", "redis"));
    DD_CHECK_STR("redis", cfg.cluster_bus_protocol);
    DD_CHECK_EQ_INT(-1,
                    config_apply(&cfg, "cluster-bus-protocol", "bogus"));
    DD_CHECK_STR("redis", cfg.cluster_bus_protocol); /* unchanged */
}

static void pump2(server *x, server *y)
{
    server_run_once(x, 5);
    server_run_once(y, 5);
}

static pal_socket_t cli(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

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
            break;
    }
    buf[got] = '\0';
    return got;
}

static void key_in_slot(uint32_t slot, char *out)
{
    int i;
    for (i = 0; i < 100000; i++) {
        snprintf(out, 16, "rkey%d", i);
        if (hash_slot(out, strlen(out)) == slot)
            return;
    }
    out[0] = '\0';
}

static void test_wire_redbus_convergence(void)
{
    server *a, *b;
    pal_socket_t ca, cb;
    char req[256], buf[4096], port[16], key[16], moved[64];
    int i, ok = 0;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    b = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL && b != NULL);
    server_enable_cluster(a, IDA);
    server_enable_cluster(b, IDB);
    server_set_bus_protocol(a, SERVER_BUS_PROTOCOL_REDIS);
    server_set_bus_protocol(b, SERVER_BUS_PROTOCOL_REDIS);
    ca = cli(server_port(a));
    cb = cli(server_port(b));
    key_in_slot(5, key);
    DD_CHECK(key[0] != '\0');

    snprintf(port, sizeof(port), "%u", (unsigned)server_port(b));
    snprintf(req, sizeof(req),
             "*4\r\n$7\r\nCLUSTER\r\n$4\r\nMEET\r\n$9\r\n127.0.0.1\r\n"
             "$%zu\r\n%s\r\n",
             strlen(port), port);
    ask2(a, b, ca, req, buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);

    /* both sides learn each other over redbus frames */
    for (i = 0; i < 600 && !ok; i++) {
        pump2(a, b);
        if (i % 40 == 0) {
            ask2(a, b, ca, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", buf,
                 sizeof(buf));
            ask2(a, b, cb, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", req,
                 sizeof(req));
            if (strstr(buf, IDB) != NULL && strstr(req, IDA) != NULL)
                ok = 1;
        }
    }
    DD_CHECK_EQ_INT(1, ok);

    /* slot claims propagate through the redbus sender bitmap */
    ask2(a, b, ca, "*3\r\n$7\r\nCLUSTER\r\n$8\r\nADDSLOTS\r\n$1\r\n5\r\n",
         buf, sizeof(buf));
    DD_CHECK_STR("+OK\r\n", buf);
    snprintf(moved, sizeof(moved), "-MOVED 5 127.0.0.1:%s\r\n",
             (snprintf(port, sizeof(port), "%u", (unsigned)server_port(a)),
              port));
    ok = 0;
    snprintf(req, sizeof(req), "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
             strlen(key), key);
    for (i = 0; i < 600 && !ok; i++) {
        pump2(a, b);
        if (i % 40 == 0) {
            ask2(a, b, cb, req, buf, sizeof(buf));
            if (strcmp(buf, moved) == 0)
                ok = 1;
        }
    }
    DD_CHECK_EQ_INT(1, ok);

    /* CLUSTER INFO converges on both */
    ok = 0;
    for (i = 0; i < 600 && !ok; i++) {
        pump2(a, b);
        if (i % 40 == 0) {
            ask2(a, b, ca, "*2\r\n$7\r\nCLUSTER\r\n$4\r\nINFO\r\n", buf,
                 sizeof(buf));
            if (strstr(buf, "cluster_known_nodes:2\r\n") != NULL)
                ok = 1;
        }
    }
    DD_CHECK_EQ_INT(1, ok);

    pal_close(cb);
    pal_close(ca);
    server_destroy(b);
    server_destroy(a);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_config_bus_protocol);
    DD_RUN(test_wire_redbus_convergence);
    return DD_TEST_SUMMARY();
}
