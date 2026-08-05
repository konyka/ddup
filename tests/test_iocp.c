/* test_iocp.c - pal_iocp: create/free, listen, accept, echo (Windows only). */
#include <string.h>

#include "pal/pal_iocp.h"
#include "pal/pal_socket.h"
#include "test.h"

static void test_create_free(void)
{
    pal_iocp *p = pal_iocp_create();
    pal_iocp_event ev;
    DD_CHECK(p != NULL);
    /* empty port: timeout returns 0 */
    DD_CHECK_EQ_INT(0, pal_iocp_wait(p, &ev, 1, 20));
    pal_iocp_free(p);
}

static void test_listen_accept(void)
{
    pal_iocp *p = pal_iocp_create();
    pal_iocp_event ev;
    pal_socket_t lfd, cli;
    uint16_t port = 0;
    static int tag;

    DD_CHECK(p != NULL);
    lfd = pal_iocp_listen(p, "127.0.0.1", 0, &port, &tag);
    DD_CHECK(lfd != PAL_SOCKET_INVALID);
    DD_CHECK(port != 0);

    cli = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(cli != PAL_SOCKET_INVALID);

    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_ACCEPT);
    DD_CHECK(ev.fd != PAL_SOCKET_INVALID);
    DD_CHECK(ev.userdata == &tag);

    /* re-post; no more clients -> timeout */
    DD_CHECK_EQ_INT(0, pal_iocp_accept_post(p, lfd, &tag));
    DD_CHECK_EQ_INT(0, pal_iocp_wait(p, &ev, 1, 30));

    pal_iocp_close(p, ev.fd);
    pal_close(cli);
    pal_close(lfd);
    pal_iocp_free(p);
}

static void test_echo(void)
{
    pal_iocp *p = pal_iocp_create();
    pal_iocp_event ev;
    pal_socket_t lfd, cli, srv;
    uint16_t port = 0;
    char rbuf[64], cbuf[64];
    static int tag;
    ptrdiff_t n;

    DD_CHECK(p != NULL);
    lfd = pal_iocp_listen(p, "127.0.0.1", 0, &port, &tag);
    DD_CHECK(lfd != PAL_SOCKET_INVALID);
    cli = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(cli != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    srv = ev.fd;
    DD_CHECK(ev.op == PAL_IOCP_ACCEPT);

    /* post recv; client sends; RECV completes with the bytes */
    DD_CHECK_EQ_INT(0, pal_iocp_recv(p, srv, rbuf, sizeof(rbuf), &tag));
    DD_CHECK_EQ_INT(5, pal_send(cli, "hello", 5));
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_RECV);
    DD_CHECK_EQ_INT(5, (long long)ev.bytes);
    DD_CHECK_MEM("hello", 5, rbuf, 5);

    /* echo back via overlapped send; client reads it */
    DD_CHECK_EQ_INT(0, pal_iocp_send(p, srv, rbuf, 5, &tag));
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_SEND);
    DD_CHECK_EQ_INT(5, (long long)ev.bytes);
    n = pal_recv(cli, cbuf, sizeof(cbuf));
    DD_CHECK_EQ_INT(5, n);
    DD_CHECK_MEM("hello", 5, cbuf, 5);

    /* client closes: next RECV completes with 0 bytes */
    DD_CHECK_EQ_INT(0, pal_iocp_recv(p, srv, rbuf, sizeof(rbuf), &tag));
    pal_close(cli);
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_RECV);
    DD_CHECK_EQ_INT(0, (long long)ev.bytes);

    pal_iocp_close(p, srv);
    pal_close(lfd);
    pal_iocp_free(p);
}

#define ECHO_CONNS 8
#define ECHO_ROUNDS 100

static void test_concurrent_echo(void)
{
    pal_iocp *p = pal_iocp_create();
    pal_iocp_event ev;
    pal_socket_t lfd, cli[ECHO_CONNS], srv[ECHO_CONNS];
    char rbuf[ECHO_CONNS][1], c;
    uint16_t port = 0;
    int accepted = 0, i, round, echoes;
    static int tag[ECHO_CONNS];

    DD_CHECK(p != NULL);
    lfd = pal_iocp_listen(p, "127.0.0.1", 0, &port, NULL);
    DD_CHECK(lfd != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_iocp_accept_post(p, lfd, NULL));

    for (i = 0; i < ECHO_CONNS; i++) {
        tag[i] = i;
        cli[i] = pal_tcp_connect("127.0.0.1", port);
        DD_CHECK(cli[i] != PAL_SOCKET_INVALID);
        /* accept may complete after each connect; drain both pending accepts */
        while (accepted < ECHO_CONNS) {
            int rc = pal_iocp_wait(p, &ev, 1, accepted + 1 < ECHO_CONNS ? 10 : 2000);
            if (rc == 0)
                break;
            DD_CHECK(ev.op == PAL_IOCP_ACCEPT);
            srv[accepted] = ev.fd;
            accepted++;
            /* keep an accept outstanding so every client gets a completion */
            DD_CHECK_EQ_INT(0, pal_iocp_accept_post(p, lfd, NULL));
        }
    }
    DD_CHECK_EQ_INT(ECHO_CONNS, accepted);

    /* post initial recv on every conn */
    for (i = 0; i < ECHO_CONNS; i++)
        DD_CHECK_EQ_INT(0,
                        pal_iocp_recv(p, srv[i], rbuf[i], 1, &tag[i]));

    for (round = 0; round < ECHO_ROUNDS; round++) {
        for (i = 0; i < ECHO_CONNS; i++)
            DD_CHECK_EQ_INT(1, pal_send(cli[i], "x", 1));
        echoes = 0;
        while (echoes < ECHO_CONNS) {
            DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
            if (ev.op == PAL_IOCP_RECV) {
                DD_CHECK_EQ_INT(1, (long long)ev.bytes);
                DD_CHECK_EQ_INT(0,
                                pal_iocp_send(p, ev.fd, rbuf[*(int *)ev.userdata], 1, ev.userdata));
            } else if (ev.op == PAL_IOCP_SEND) {
                echoes++;
            }
        }
        for (i = 0; i < ECHO_CONNS; i++) {
            DD_CHECK_EQ_INT(1, pal_recv(cli[i], &c, 1));
            DD_CHECK(c == 'x');
        }
        if (round + 1 < ECHO_ROUNDS)
            for (i = 0; i < ECHO_CONNS; i++)
                DD_CHECK_EQ_INT(0, pal_iocp_recv(p, srv[i], rbuf[i], 1, &tag[i]));
    }

    for (i = 0; i < ECHO_CONNS; i++) {
        pal_close(cli[i]);
        pal_iocp_close(p, srv[i]);
    }
    /* outstanding recvs on closed conns complete with error; drain quietly */
    pal_close(lfd);
    pal_iocp_free(p);
}

/* pal_iocp_post wakes a waiter with a WAKEUP event carrying the userdata
 * (mt worker task-queue kick on the IOCP backend). */
static void test_wakeup_post(void)
{
    pal_iocp *p = pal_iocp_create();
    pal_iocp_event ev;
    static int tag;

    DD_CHECK(p != NULL);
    DD_CHECK_EQ_INT(0, pal_iocp_post(p, &tag));
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_WAKEUP);
    DD_CHECK(ev.userdata == &tag);

    /* posts are not lost: two posts deliver two events */
    DD_CHECK_EQ_INT(0, pal_iocp_post(p, NULL));
    DD_CHECK_EQ_INT(0, pal_iocp_post(p, NULL));
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_WAKEUP);
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_WAKEUP);

    pal_iocp_free(p);
}

static void test_close_cancels_outstanding(void)
{
    pal_iocp *p = pal_iocp_create();
    pal_iocp_event ev;
    pal_socket_t lfd, cli, srv;
    uint16_t port = 0;
    char rbuf[64];

    DD_CHECK(p != NULL);
    lfd = pal_iocp_listen(p, "127.0.0.1", 0, &port, NULL);
    DD_CHECK(lfd != PAL_SOCKET_INVALID);
    cli = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(cli != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    srv = ev.fd;

    /* outstanding RECV with no data; close cancels it -> completion -1 */
    DD_CHECK_EQ_INT(0, pal_iocp_recv(p, srv, rbuf, sizeof(rbuf), NULL));
    pal_iocp_close(p, srv);
    DD_CHECK_EQ_INT(1, pal_iocp_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOCP_RECV);
    DD_CHECK_EQ_INT(-1, (long long)ev.bytes);

    pal_close(cli);
    pal_close(lfd);
    pal_iocp_free(p);
}

int main(void)
{
    DD_CHECK_EQ_INT(0, pal_socket_init());
    DD_RUN(test_create_free);
    DD_RUN(test_listen_accept);
    DD_RUN(test_echo);
    DD_RUN(test_concurrent_echo);
    DD_RUN(test_close_cancels_outstanding);
    DD_RUN(test_wakeup_post);
    pal_socket_cleanup();
    return DD_TEST_SUMMARY();
}
