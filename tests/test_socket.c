/* test_socket.c - pal_socket: loopback listen/connect/echo/close. */
#include <string.h>

#include "pal/pal_socket.h"
#include "test.h"

static void test_init_cleanup(void)
{
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pal_socket_cleanup();
    /* init must be re-entrant after cleanup */
    DD_CHECK_EQ_INT(0, pal_socket_init());
    pal_socket_cleanup();
}

static void test_listen_connect_echo(void)
{
    uint16_t port = 0;
    pal_socket_t listener;
    pal_socket_t client;
    pal_socket_t server;
    char buf[64];
    ptrdiff_t n;

    DD_CHECK_EQ_INT(0, pal_socket_init());

    listener = pal_tcp_listen("127.0.0.1", 0, 16, &port);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    DD_CHECK(port != 0);

    client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(client != PAL_SOCKET_INVALID);

    server = pal_accept(listener);
    DD_CHECK(server != PAL_SOCKET_INVALID);

    /* client -> server */
    DD_CHECK_EQ_INT(5, pal_send(client, "hello", 5));
    n = pal_recv(server, buf, sizeof(buf));
    DD_CHECK_EQ_INT(5, n);
    DD_CHECK_MEM("hello", 5, buf, 5);

    /* server -> client */
    DD_CHECK_EQ_INT(5, pal_send(server, "world", 5));
    n = pal_recv(client, buf, sizeof(buf));
    DD_CHECK_EQ_INT(5, n);
    DD_CHECK_MEM("world", 5, buf, 5);

    /* non-blocking recv with no data reports would-block */
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(server, 1));
    n = pal_recv(server, buf, sizeof(buf));
    DD_CHECK_EQ_INT(-1, n);
    DD_CHECK(pal_would_block(pal_socket_error()));
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(server, 0));

    /* orderly close: peer recv returns 0 */
    pal_close(client);
    n = pal_recv(server, buf, sizeof(buf));
    DD_CHECK_EQ_INT(0, n);

    pal_close(server);
    pal_close(listener);
    pal_socket_cleanup();
}

static void test_listen_failure(void)
{
    /* Port 1 without privileges / invalid host must fail gracefully. */
    DD_CHECK(pal_tcp_listen("256.256.256.256", 0, 16, NULL) ==
             PAL_SOCKET_INVALID);
}

static void test_tcp_nodelay(void)
{
    uint16_t port = 0;
    pal_socket_t listener;
    pal_socket_t client;
    pal_socket_t server;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    listener = pal_tcp_listen("127.0.0.1", 0, 16, &port);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(client != PAL_SOCKET_INVALID);
    server = pal_accept(listener);
    DD_CHECK(server != PAL_SOCKET_INVALID);

    DD_CHECK_EQ_INT(0, pal_set_tcp_nodelay(server, 1));
    DD_CHECK_EQ_INT(0, pal_set_tcp_nodelay(client, 0));

    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}

int main(void)
{
    DD_RUN(test_init_cleanup);
    DD_RUN(test_listen_connect_echo);
    DD_RUN(test_listen_failure);
    DD_RUN(test_tcp_nodelay);
    return DD_TEST_SUMMARY();
}
