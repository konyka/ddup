/* test_event.c - pal_event: readiness loop over a loopback TCP pair.
 *
 * The whole suite runs on the default backend (epoll/kqueue/select) and,
 * when the kernel offers it, again on io_uring (Linux).
 */
#include "pal/pal_event.h"
#include "test.h"

static pal_loop *(*g_create)(void) = pal_loop_create;

static void test_create_free(void)
{
    pal_loop *l = g_create();
    DD_CHECK(l != NULL);
    /* wait on an empty loop returns 0 after the timeout */
    {
        pal_event ev[4];
        DD_CHECK_EQ_INT(0, pal_loop_wait(l, ev, 4, 20));
    }
    pal_loop_free(l);
}

/* Build listener + client + accepted server fd; all blocking. */
static pal_socket_t make_pair(pal_socket_t *listener, pal_socket_t *client)
{
    uint16_t port = 0;
    *listener = pal_tcp_listen("127.0.0.1", 0, 16, &port);
    DD_CHECK(*listener != PAL_SOCKET_INVALID);
    *client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(*client != PAL_SOCKET_INVALID);
    return pal_accept(*listener);
}

static void test_read_readiness(void)
{
    pal_socket_t listener, client, server;
    pal_event ev[4];
    char b;
    int tag;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    server = make_pair(&listener, &client);
    DD_CHECK(server != PAL_SOCKET_INVALID);

    {
        pal_loop *l = g_create();
        DD_CHECK(l != NULL);
        DD_CHECK_EQ_INT(0, pal_loop_add(l, server, 1, 0, &tag));

        /* nothing sent yet: timeout case returns 0 */
        DD_CHECK_EQ_INT(0, pal_loop_wait(l, ev, 4, 30));

        /* client sends one byte -> server fd readable */
        DD_CHECK_EQ_INT(1, pal_send(client, "x", 1));
        DD_CHECK_EQ_INT(1, pal_loop_wait(l, ev, 4, 1000));
        DD_CHECK(ev[0].fd == server);
        DD_CHECK(ev[0].userdata == &tag);
        DD_CHECK_EQ_INT(1, ev[0].readable);
        DD_CHECK_EQ_INT(1, pal_recv(server, &b, 1));

        pal_loop_free(l);
    }

    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}

static void test_write_readiness_and_mod(void)
{
    pal_socket_t listener, client, server;
    pal_event ev[4];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    server = make_pair(&listener, &client);
    DD_CHECK(server != PAL_SOCKET_INVALID);

    {
        pal_loop *l = g_create();
        DD_CHECK(l != NULL);
        /* register read-only first */
        DD_CHECK_EQ_INT(0, pal_loop_add(l, server, 1, 0, NULL));
        DD_CHECK_EQ_INT(0, pal_loop_wait(l, ev, 4, 30));
        /* mod to write interest: a fresh socket is writable */
        DD_CHECK_EQ_INT(0, pal_loop_mod(l, server, 0, 1, NULL));
        DD_CHECK_EQ_INT(1, pal_loop_wait(l, ev, 4, 1000));
        DD_CHECK(ev[0].fd == server);
        DD_CHECK_EQ_INT(1, ev[0].writable);

        pal_loop_free(l);
    }

    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}

static void test_del(void)
{
    pal_socket_t listener, client, server;
    pal_event ev[4];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    server = make_pair(&listener, &client);
    DD_CHECK(server != PAL_SOCKET_INVALID);

    {
        pal_loop *l = g_create();
        DD_CHECK(l != NULL);
        DD_CHECK_EQ_INT(0, pal_loop_add(l, server, 1, 0, NULL));
        DD_CHECK_EQ_INT(0, pal_loop_del(l, server));
        /* after del, pending data must not be reported */
        DD_CHECK_EQ_INT(1, pal_send(client, "y", 1));
        DD_CHECK_EQ_INT(0, pal_loop_wait(l, ev, 4, 30));

        pal_loop_free(l);
    }

    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}

static void run_all(void)
{
    DD_RUN(test_create_free);
    DD_RUN(test_read_readiness);
    DD_RUN(test_write_readiness_and_mod);
    DD_RUN(test_del);
}

int main(void)
{
    run_all();
    {
        /* second pass on io_uring when the kernel offers it */
        pal_loop *probe = pal_loop_create_iouring();
        if (probe != NULL) {
            pal_loop_free(probe);
            g_create = pal_loop_create_iouring;
            printf("=== backend: io_uring ===\n");
            run_all();
        }
    }
    return DD_TEST_SUMMARY();
}
