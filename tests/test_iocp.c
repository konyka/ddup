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

int main(void)
{
    DD_CHECK_EQ_INT(0, pal_socket_init());
    DD_RUN(test_create_free);
    DD_RUN(test_listen_accept);
    pal_socket_cleanup();
    return DD_TEST_SUMMARY();
}
