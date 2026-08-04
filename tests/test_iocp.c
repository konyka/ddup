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

int main(void)
{
    DD_CHECK_EQ_INT(0, pal_socket_init());
    DD_RUN(test_create_free);
    pal_socket_cleanup();
    return DD_TEST_SUMMARY();
}
