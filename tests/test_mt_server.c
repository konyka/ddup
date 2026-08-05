/* test_mt_server.c - integration tests for the thread-per-core mt_server.
 *
 * Workers run on real background threads; the test acts as a loopback client
 * and talks to the public listener.
 */
#include "test.h"

#include <string.h>

#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/mt_server.h"

static pal_socket_t connect_client(uint16_t port)
{
    pal_socket_t c = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));
    return c;
}

static void roundtrip(pal_socket_t c, const char *req, const char *expected)
{
    size_t elen = strlen(expected);
    size_t rlen = strlen(req);
    size_t sent = 0, got = 0;
    char buf[512];
    uint64_t deadline = pal_now_ms() + 5000;

    DD_CHECK(elen <= sizeof(buf));
    while (sent < rlen && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_send(c, req + sent, rlen - sent);
        if (n > 0)
            sent += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT((long long)rlen, (long long)sent);

    while (got < elen && pal_now_ms() < deadline) {
        ptrdiff_t n = pal_recv(c, buf + got, sizeof(buf) - got);
        if (n > 0)
            got += (size_t)n;
        else
            pal_sleep_ms(1);
    }
    DD_CHECK_EQ_INT((long long)elen, (long long)got);
    DD_CHECK_MEM(expected, elen, buf, got);
}

static void test_two_workers_independent_keyspaces(void)
{
    mt_server *ms;
    pal_socket_t a, b;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));
    DD_CHECK(mt_server_port(ms) != 0);

    a = connect_client(mt_server_port(ms));
    b = connect_client(mt_server_port(ms));

    roundtrip(a, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
    roundtrip(b, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");

    /* Connections are assigned round-robin: a -> worker 0, b -> worker 1.
     * The skeleton has independent per-worker keyspaces (routing arrives in
     * the next milestone). */
    roundtrip(a, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", "+OK\r\n");
    roundtrip(a, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", "$3\r\nbar\r\n");
    roundtrip(b, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n", "$-1\r\n");

    pal_close(a);
    pal_close(b);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

static void test_many_connections_across_workers(void)
{
    mt_server *ms;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 4);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    for (i = 0; i < 16; i++) {
        pal_socket_t c = connect_client(mt_server_port(ms));
        roundtrip(c, "*1\r\n$4\r\nPING\r\n", "+PONG\r\n");
        pal_close(c);
    }

    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

int main(void)
{
    DD_RUN(test_two_workers_independent_keyspaces);
    DD_RUN(test_many_connections_across_workers);
    return DD_TEST_SUMMARY();
}
