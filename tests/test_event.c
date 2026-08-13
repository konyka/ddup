/* test_event.c - pal_event: readiness loop over a loopback TCP pair.
 *
 * The whole suite runs on the default backend (epoll/kqueue/select) and,
 * when the kernel offers it, again on io_uring (Linux).
 */
#include "pal/pal_event.h"
#include "test.h"

#ifdef DDUP_TEST_IOURING
#include <errno.h>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

static pal_loop *(*g_create)(void) = pal_loop_create;

#ifdef DDUP_TEST_IOURING
#define TEST_URING_ENTRIES 256

static int g_enter_fail;
static int g_enter_eintr;
static unsigned g_enter_limit;
static unsigned g_enter_calls;

static int test_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                            unsigned flags)
{
    g_enter_calls++;
    if (g_enter_eintr) {
        g_enter_eintr = 0;
        errno = EINTR;
        return -1;
    }
    if (g_enter_fail) {
        g_enter_fail = 0;
        errno = EIO;
        return -1;
    }
    if (g_enter_limit > 0 && to_submit > g_enter_limit)
        to_submit = g_enter_limit;
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete,
                        flags, NULL, 0);
}

static void reset_uring_enter(void)
{
    g_enter_fail = 0;
    g_enter_eintr = 0;
    g_enter_limit = 0;
    g_enter_calls = 0;
    pal_event_test_set_uring_enter(NULL);
}

static int events_have_userdata(const pal_event *events, int n,
                                const void *userdata)
{
    int i;
    for (i = 0; i < n; i++)
        if (events[i].userdata == userdata)
            return 1;
    return 0;
}
#endif

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

#ifdef DDUP_TEST_IOURING
static void test_iouring_add_failure_and_partial_recovery(void)
{
    pal_socket_t listener, client, server;
    pal_event events[TEST_URING_ENTRIES];
    pal_loop *l;
    int tag;
    int i;
    int n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    server = make_pair(&listener, &client);
    DD_CHECK(server != PAL_SOCKET_INVALID);
    l = pal_loop_create_iouring();
    DD_CHECK(l != NULL);
    if (l == NULL)
        goto cleanup;

    pal_event_test_set_uring_enter(test_uring_enter);
    for (i = 0; i < TEST_URING_ENTRIES; i++)
        DD_CHECK_EQ_INT(0, pal_loop_add(l, server, 1, 0, &tag));
    g_enter_fail = 1;
    DD_CHECK_EQ_INT(-1, pal_loop_add(l, server, 1, 0, &tag));

    g_enter_limit = 1;
    DD_CHECK_EQ_INT(1, pal_send(client, "a", 1));
    n = pal_loop_wait(l, events, TEST_URING_ENTRIES, 1000);
    DD_CHECK(n > 0);
    DD_CHECK(g_enter_calls > TEST_URING_ENTRIES);
    DD_CHECK(events_have_userdata(events, n, &tag));

    reset_uring_enter();
    pal_loop_free(l);
cleanup:
    reset_uring_enter();
    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}

static void test_iouring_mod_failure_keeps_poll(void)
{
    pal_socket_t listener, client, server;
    pal_event events[TEST_URING_ENTRIES];
    pal_loop *l;
    int old_tag;
    int new_tag;
    int filler_tag;
    int i;
    int n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    server = make_pair(&listener, &client);
    DD_CHECK(server != PAL_SOCKET_INVALID);
    l = pal_loop_create_iouring();
    DD_CHECK(l != NULL);
    if (l == NULL)
        goto cleanup;

    pal_event_test_set_uring_enter(test_uring_enter);
    DD_CHECK_EQ_INT(0, pal_loop_add(l, server, 1, 0, &old_tag));
    for (i = 1; i < TEST_URING_ENTRIES; i++)
        DD_CHECK_EQ_INT(0, pal_loop_add(l, listener, 1, 0, &filler_tag));
    g_enter_fail = 1;
    DD_CHECK_EQ_INT(-1, pal_loop_mod(l, server, 0, 1, &new_tag));

    DD_CHECK_EQ_INT(1, pal_send(client, "m", 1));
    n = pal_loop_wait(l, events, TEST_URING_ENTRIES, 1000);
    DD_CHECK(n > 0);
    DD_CHECK(events_have_userdata(events, n, &old_tag));

    reset_uring_enter();
    pal_loop_free(l);
cleanup:
    reset_uring_enter();
    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}

static void test_iouring_del_failure_keeps_poll(void)
{
    pal_socket_t listener, client, server;
    pal_event events[TEST_URING_ENTRIES];
    pal_loop *l;
    int target_tag;
    int filler_tag;
    int i;
    int n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    server = make_pair(&listener, &client);
    DD_CHECK(server != PAL_SOCKET_INVALID);
    l = pal_loop_create_iouring();
    DD_CHECK(l != NULL);
    if (l == NULL)
        goto cleanup;

    pal_event_test_set_uring_enter(test_uring_enter);
    DD_CHECK_EQ_INT(0, pal_loop_add(l, server, 1, 0, &target_tag));
    for (i = 1; i < TEST_URING_ENTRIES; i++)
        DD_CHECK_EQ_INT(0, pal_loop_add(l, listener, 1, 0, &filler_tag));
    g_enter_fail = 1;
    DD_CHECK_EQ_INT(-1, pal_loop_del(l, server));

    DD_CHECK_EQ_INT(1, pal_send(client, "d", 1));
    n = pal_loop_wait(l, events, TEST_URING_ENTRIES, 1000);
    DD_CHECK(n > 0);
    DD_CHECK(events_have_userdata(events, n, &target_tag));

    reset_uring_enter();
    pal_loop_free(l);
cleanup:
    reset_uring_enter();
    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}

static void test_iouring_timed_wait_failure_recovers(void)
{
    pal_socket_t listener, client, server;
    pal_event event;
    pal_loop *l;
    int tag;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    server = make_pair(&listener, &client);
    DD_CHECK(server != PAL_SOCKET_INVALID);
    l = pal_loop_create_iouring();
    DD_CHECK(l != NULL);
    if (l == NULL)
        goto cleanup;

    pal_event_test_set_uring_enter(test_uring_enter);
    DD_CHECK_EQ_INT(0, pal_loop_add(l, server, 1, 0, &tag));
    g_enter_eintr = 1;
    g_enter_fail = 1;
    DD_CHECK_EQ_INT(-1, pal_loop_wait(l, &event, 1, 20));
    DD_CHECK_EQ_INT(2, (int)g_enter_calls);
    DD_CHECK_EQ_INT(0, pal_loop_wait(l, &event, 1, 20));
    DD_CHECK_EQ_INT(1, pal_send(client, "t", 1));
    DD_CHECK_EQ_INT(1, pal_loop_wait(l, &event, 1, 1000));
    DD_CHECK(event.userdata == &tag);

    reset_uring_enter();
    pal_loop_free(l);
cleanup:
    reset_uring_enter();
    pal_close(server);
    pal_close(client);
    pal_close(listener);
    pal_socket_cleanup();
}
#endif

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
#ifdef DDUP_TEST_IOURING
            DD_RUN(test_iouring_add_failure_and_partial_recovery);
            DD_RUN(test_iouring_mod_failure_keeps_poll);
            DD_RUN(test_iouring_del_failure_keeps_poll);
            DD_RUN(test_iouring_timed_wait_failure_recovers);
#endif
        }
    }
    return DD_TEST_SUMMARY();
}
