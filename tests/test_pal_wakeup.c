/* test_pal_wakeup.c - tests for the cross-platform event-loop wakeup pipe. */
#include "test.h"

#include "pal/pal_event.h"
#include "pal/pal_socket.h"
#include "pal/pal_wakeup.h"

static void test_kick_makes_wait_fd_readable(void)
{
    pal_wakeup w;
    pal_loop *l;
    pal_event evs[4];
    int n;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    DD_CHECK_EQ_INT(0, pal_wakeup_create(&w));
    l = pal_loop_create();
    DD_CHECK(l != NULL);
    DD_CHECK_EQ_INT(0, pal_loop_add(l, w.wait_fd, 1, 0, NULL));

    /* Nothing pending: the wait times out. */
    n = pal_loop_wait(l, evs, 4, 10);
    DD_CHECK_EQ_INT(0, n);

    DD_CHECK_EQ_INT(0, pal_wakeup_kick(&w));
    n = pal_loop_wait(l, evs, 4, 1000);
    DD_CHECK_EQ_INT(1, n);
    DD_CHECK(evs[0].fd == w.wait_fd);
    DD_CHECK(evs[0].readable);

    /* Draining consumes the wakeup; the next wait times out again. */
    DD_CHECK(pal_wakeup_drain(&w) > 0);
    n = pal_loop_wait(l, evs, 4, 10);
    DD_CHECK_EQ_INT(0, n);

    pal_loop_free(l);
    pal_wakeup_destroy(&w);
    pal_socket_cleanup();
}

static void test_multiple_kicks_drain_together(void)
{
    pal_wakeup w;
    int drained;
    int i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    DD_CHECK_EQ_INT(0, pal_wakeup_create(&w));

    for (i = 0; i < 8; i++)
        DD_CHECK_EQ_INT(0, pal_wakeup_kick(&w));
    drained = pal_wakeup_drain(&w);
    DD_CHECK(drained >= 8);
    DD_CHECK_EQ_INT(0, pal_wakeup_drain(&w));

    pal_wakeup_destroy(&w);
    pal_socket_cleanup();
}

int main(void)
{
    DD_RUN(test_kick_makes_wait_fd_readable);
    DD_RUN(test_multiple_kicks_drain_together);
    return DD_TEST_SUMMARY();
}
