/* test_iouring_op.c - Linux io_uring proactor correctness coverage. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pal/pal_iouring_op.h"
#include "pal/pal_socket.h"
#include "test.h"

#define WAKEUP_ROUNDS 2048
#define TIMEOUT_ROUNDS 64
#define PBUF_COUNT 1
#define PBUF_SIZE 4096
#define PBUF_ROUNDS 8
#define PBUF_PAGE_BOUNDARY_COUNT 256
#define SBUF_COUNT 4
#define SBUF_SIZE 4096

static int wait_for_accept(pal_iouring *p, pal_socket_t listener,
                           pal_iouring_event *ev);

static void test_registered_send_buffers(void)
{
    pal_iouring *p = pal_iouring_create();
    int bid = -1;
    void *buf;
    int held_id[SBUF_COUNT];
    int i;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (pal_iouring_enable_sbuf(p, SBUF_COUNT, SBUF_SIZE) != 0) {
        printf("registered send buffers unsupported; skipping\n");
        pal_iouring_free(p);
        return;
    }
    DD_CHECK_EQ_INT(1, pal_iouring_sbuf_active(p));
    buf = pal_iouring_sbuf_acquire(p, &bid);
    DD_CHECK(buf != NULL);
    DD_CHECK(bid >= 0 && bid < SBUF_COUNT);
    if (buf != NULL)
        memset(buf, 'x', SBUF_SIZE);
    pal_iouring_sbuf_release(p, bid);
    for (i = 0; i < SBUF_COUNT; i++)
        DD_CHECK(pal_iouring_sbuf_acquire(p, &held_id[i]) != NULL);
    DD_CHECK(pal_iouring_sbuf_acquire(p, &bid) == NULL);
    for (i = 0; i < SBUF_COUNT; i++)
        pal_iouring_sbuf_release(p, held_id[i]);
    pal_iouring_free(p);
}

static void test_send_zc_fixed_completion(void)
{
    pal_iouring *p = pal_iouring_create();
    pal_iouring_event ev;
    pal_socket_t listener = PAL_SOCKET_INVALID;
    pal_socket_t client = PAL_SOCKET_INVALID;
    pal_socket_t server = PAL_SOCKET_INVALID;
    uint16_t port = 0;
    int bid = -1;
    static long long zc_tag;
    char recv_buf[32];
    void *buf;
    int got_send = 0, got_notif = 0;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (pal_iouring_enable_sbuf(p, 1, SBUF_SIZE) != 0) {
        printf("registered send buffers unsupported; skipping SEND_ZC\n");
        pal_iouring_free(p);
        return;
    }
    listener = pal_iouring_listen(p, "127.0.0.1", 0, &port, NULL);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    if (listener == PAL_SOCKET_INVALID)
        goto cleanup;
    client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(client != PAL_SOCKET_INVALID);
    if (client == PAL_SOCKET_INVALID)
        goto cleanup;
    DD_CHECK_EQ_INT(1, wait_for_accept(p, listener, &ev));
    server = ev.fd;
    buf = pal_iouring_sbuf_acquire(p, &bid);
    DD_CHECK(buf != NULL);
    if (buf == NULL)
        goto cleanup;
    memcpy(buf, "hello-zc", 8);
    DD_CHECK_EQ_INT(0, pal_iouring_send_zc_fixed(p, server, bid, 0, 8,
                                                  &zc_tag));
    while (!got_notif) {
        DD_CHECK_EQ_INT(1, pal_iouring_wait(p, &ev, 1, 2000));
        if (ev.op != PAL_IOURING_SEND)
            continue;
        if (ev.notif)
            got_notif = 1;
        else
            got_send = 1;
    }
    DD_CHECK_EQ_INT(1, got_send);
    DD_CHECK_EQ_INT(1, got_notif);
    DD_CHECK_EQ_INT(8, pal_recv(client, recv_buf, sizeof(recv_buf)));
    DD_CHECK_MEM("hello-zc", 8, recv_buf, 8);
    pal_iouring_sbuf_release(p, bid);

cleanup:
    if (server != PAL_SOCKET_INVALID)
        pal_iouring_close(p, server);
    if (client != PAL_SOCKET_INVALID)
        pal_close(client);
    if (listener != PAL_SOCKET_INVALID)
        pal_close(listener);
    pal_iouring_free(p);
}

static void test_pbuf_validation_and_recycle(void)
{
    pal_iouring *p = pal_iouring_create();

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    DD_CHECK_EQ_INT(-1, pal_iouring_enable_pbuf(p, 65536, 4096));
    DD_CHECK_EQ_INT(0, pal_iouring_pbuf_active(p));
    DD_CHECK_EQ_INT(-1, pal_iouring_enable_pbuf(p, 2, (size_t)UINT32_MAX + 1));
    DD_CHECK_EQ_INT(-1, pal_iouring_enable_pbuf(p, 2, SIZE_MAX));
    pal_iouring_recycle(p, -1);
    pal_iouring_recycle(p, 0);
    pal_iouring_free(p);
}

static void test_pbuf_header_allocation(void)
{
    pal_iouring *p = pal_iouring_create();

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (pal_iouring_enable_pbuf(p, PBUF_PAGE_BOUNDARY_COUNT, 1) != 0) {
        printf("provided-buffer ring unsupported; skipping pbuf allocation\n");
        pal_iouring_free(p);
        return;
    }
    DD_CHECK_EQ_INT(1, pal_iouring_pbuf_active(p));
    pal_iouring_free(p);
}

static void test_wakeup_publication(void)
{
    pal_iouring *p = pal_iouring_create_ex(PAL_IOURING_F_SQPOLL);
    pal_iouring_event ev[32];
    int completed = 0;
    int idle_polls = 0;
    int i;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (!pal_iouring_sqpoll_active(p)) {
        printf("SQPOLL setup unavailable; skipping publication stress\n");
        pal_iouring_free(p);
        return;
    }
    for (i = 0; i < WAKEUP_ROUNDS; i++)
        DD_CHECK_EQ_INT(0, pal_iouring_post(p, NULL));

    while (completed < WAKEUP_ROUNDS && idle_polls < 2000) {
        int n = pal_iouring_wait(p, ev, 32, 0);
        DD_CHECK(n >= 0);
        if (n < 0)
            break;
        if (n == 0) {
            idle_polls++;
            usleep(1000);
            continue;
        }
        idle_polls = 0;
        for (i = 0; i < n; i++) {
            DD_CHECK(ev[i].op == PAL_IOURING_WAKEUP);
            DD_CHECK(ev[i].userdata == NULL);
        }
        completed += n;
    }
    DD_CHECK_EQ_INT(WAKEUP_ROUNDS, completed);
    pal_iouring_free(p);
}

static void test_published_wake_failure(void)
{
    pal_iouring *p = pal_iouring_create_ex(PAL_IOURING_F_SQPOLL);
    pal_iouring_event ev;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (!pal_iouring_sqpoll_active(p)) {
        printf("SQPOLL setup unavailable; skipping wake failure test\n");
        pal_iouring_free(p);
        return;
    }
    pal_iouring_test_fail_next_sqpoll_wake(p, EIO);
    DD_CHECK_EQ_INT(PAL_IOURING_PUBLISHED_RETRY, pal_iouring_post(p, NULL));
    DD_CHECK_EQ_INT(1, pal_iouring_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOURING_WAKEUP);
    pal_iouring_free(p);
}

static void test_timeout_operand_lifetime(void)
{
    pal_iouring *p = pal_iouring_create();
    pal_iouring_event ev;
    int i;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    for (i = 0; i < TIMEOUT_ROUNDS; i++) {
        DD_CHECK_EQ_INT(0, pal_iouring_post(p, NULL));
        DD_CHECK_EQ_INT(1, pal_iouring_wait(p, &ev, 1, 100));
        DD_CHECK(ev.op == PAL_IOURING_WAKEUP);
    }
    DD_CHECK_EQ_INT(0, pal_iouring_wait(p, &ev, 1, 20));
    pal_iouring_free(p);
}

static void test_defer_taskrun_or_fallback(void)
{
    pal_iouring *p = pal_iouring_create_ex(PAL_IOURING_F_DEFER);
    pal_iouring_event ev;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    DD_CHECK_EQ_INT(0, pal_iouring_post(p, NULL));
    DD_CHECK_EQ_INT(1, pal_iouring_wait(p, &ev, 1, 2000));
    DD_CHECK(ev.op == PAL_IOURING_WAKEUP);
    pal_iouring_free(p);
}

static int wait_for_op(pal_iouring *p, pal_iouring_ev op,
                       pal_iouring_event *ev)
{
    int i;
    for (i = 0; i < 4; i++) {
        int rc = pal_iouring_wait(p, ev, 1, 2000);
        if (rc != 1)
            return rc;
        if (ev->op == op)
            return 1;
    }
    return 0;
}

static int wait_for_accept(pal_iouring *p, pal_socket_t listener,
                           pal_iouring_event *ev)
{
    int i;
    for (i = 0; i < 4; i++) {
        int rc = wait_for_op(p, PAL_IOURING_ACCEPT, ev);
        if (rc != 1)
            return rc;
        if (ev->bytes >= 0 && ev->fd != PAL_SOCKET_INVALID)
            return 1;
        if (ev->err != EINVAL ||
            pal_iouring_accept_post(p, listener, NULL) != 0)
            return -1;
    }
    return 0;
}

static void test_published_listen_wake_failure(void)
{
    pal_iouring *p = pal_iouring_create_ex(PAL_IOURING_F_SQPOLL);
    pal_iouring_event ev;
    pal_socket_t listener = PAL_SOCKET_INVALID;
    pal_socket_t client = PAL_SOCKET_INVALID;
    pal_socket_t server = PAL_SOCKET_INVALID;
    uint16_t port = 0;
    int rc;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (!pal_iouring_sqpoll_active(p)) {
        printf("SQPOLL setup unavailable; skipping listen wake test\n");
        pal_iouring_free(p);
        return;
    }
    pal_iouring_test_fail_next_sqpoll_wake(p, EIO);
    listener = pal_iouring_listen(p, "127.0.0.1", 0, &port, NULL);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    if (listener == PAL_SOCKET_INVALID)
        goto cleanup;
    client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(client != PAL_SOCKET_INVALID);
    if (client == PAL_SOCKET_INVALID)
        goto cleanup;
    rc = wait_for_accept(p, listener, &ev);
    DD_CHECK_EQ_INT(1, rc);
    if (rc == 1)
        server = ev.fd;

cleanup:
    if (server != PAL_SOCKET_INVALID)
        pal_iouring_close(p, server);
    if (client != PAL_SOCKET_INVALID)
        pal_close(client);
    if (listener != PAL_SOCKET_INVALID)
        pal_close(listener);
    pal_iouring_free(p);
}

static void test_published_recv_wake_failure(void)
{
    pal_iouring *p = pal_iouring_create_ex(PAL_IOURING_F_SQPOLL);
    pal_iouring_event ev;
    pal_socket_t listener = PAL_SOCKET_INVALID;
    pal_socket_t client = PAL_SOCKET_INVALID;
    pal_socket_t server = PAL_SOCKET_INVALID;
    uint16_t port = 0;
    char buf[32];
    static long long tag;
    int rc;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (!pal_iouring_sqpoll_active(p)) {
        printf("SQPOLL setup unavailable; skipping recv wake test\n");
        pal_iouring_free(p);
        return;
    }
    listener = pal_iouring_listen(p, "127.0.0.1", 0, &port, NULL);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    if (listener == PAL_SOCKET_INVALID)
        goto cleanup;
    client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(client != PAL_SOCKET_INVALID);
    if (client == PAL_SOCKET_INVALID)
        goto cleanup;
    rc = wait_for_accept(p, listener, &ev);
    DD_CHECK_EQ_INT(1, rc);
    if (rc != 1)
        goto cleanup;
    server = ev.fd;
    pal_iouring_test_fail_next_sqpoll_wake(p, EIO);
    DD_CHECK_EQ_INT(PAL_IOURING_PUBLISHED_RETRY,
                    pal_iouring_recv(p, server, buf, sizeof(buf), &tag));
    DD_CHECK_EQ_INT(5, pal_send(client, "hello", 5));
    DD_CHECK_EQ_INT(1, wait_for_op(p, PAL_IOURING_RECV, &ev));
    DD_CHECK(ev.userdata == &tag);
    DD_CHECK_EQ_INT(5, ev.bytes);
    DD_CHECK_MEM("hello", 5, buf, 5);

cleanup:
    if (server != PAL_SOCKET_INVALID)
        pal_iouring_close(p, server);
    if (client != PAL_SOCKET_INVALID)
        pal_close(client);
    if (listener != PAL_SOCKET_INVALID)
        pal_close(listener);
    pal_iouring_free(p);
}

static void test_multishot_provided_buffer(void)
{
    pal_iouring *p = pal_iouring_create();
    pal_iouring_event ev;
    pal_socket_t listener = PAL_SOCKET_INVALID;
    pal_socket_t client = PAL_SOCKET_INVALID;
    pal_socket_t server = PAL_SOCKET_INVALID;
    uint16_t port = 0;
    static long long tag;
    int rc;
    int round;

    memset(&ev, 0, sizeof(ev));
    ev.fd = PAL_SOCKET_INVALID;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (pal_iouring_enable_pbuf(p, PBUF_COUNT, PBUF_SIZE) != 0) {
        printf("provided-buffer ring unsupported; skipping pbuf runtime\n");
        pal_iouring_free(p);
        return;
    }
    DD_CHECK_EQ_INT(1, pal_iouring_pbuf_active(p));

    listener = pal_iouring_listen(p, "127.0.0.1", 0, &port, NULL);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    if (listener == PAL_SOCKET_INVALID)
        goto cleanup;
    client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(client != PAL_SOCKET_INVALID);
    if (client == PAL_SOCKET_INVALID)
        goto cleanup;
    rc = wait_for_accept(p, listener, &ev);
    DD_CHECK_EQ_INT(1, rc);
    if (rc != 1 || ev.op != PAL_IOURING_ACCEPT ||
        ev.fd == PAL_SOCKET_INVALID)
        goto cleanup;
    server = ev.fd;

    DD_CHECK_EQ_INT(0, pal_iouring_recv_ms(p, server, &tag));
    for (round = 0; round < PBUF_ROUNDS; round++) {
        DD_CHECK_EQ_INT(5, pal_send(client, "hello", 5));
        DD_CHECK_EQ_INT(1, wait_for_op(p, PAL_IOURING_RECV, &ev));
        DD_CHECK(ev.op == PAL_IOURING_RECV);
        DD_CHECK(ev.userdata == &tag);
        DD_CHECK_EQ_INT(5, ev.bytes);
        DD_CHECK_EQ_INT(0, ev.buf_id);
        if (ev.buf_id == 0) {
            const void *buf = pal_iouring_buf(p, ev.buf_id);
            DD_CHECK(buf != NULL);
            if (buf != NULL)
                DD_CHECK_MEM("hello", 5, buf, (size_t)ev.bytes);
            pal_iouring_recycle(p, ev.buf_id);
        }
        if (ev.op_done && round + 1 < PBUF_ROUNDS)
            DD_CHECK_EQ_INT(0, pal_iouring_recv_ms(p, server, &tag));
    }

cleanup:
    if (server != PAL_SOCKET_INVALID)
        pal_iouring_close(p, server);
    if (client != PAL_SOCKET_INVALID)
        pal_close(client);
    if (listener != PAL_SOCKET_INVALID)
        pal_close(listener);
    pal_iouring_free(p);
}

static void test_pbuf_free_with_armed_recv(void)
{
    pal_iouring *p = pal_iouring_create();
    pal_iouring_event ev;
    pal_socket_t listener = PAL_SOCKET_INVALID;
    pal_socket_t client = PAL_SOCKET_INVALID;
    pal_socket_t server = PAL_SOCKET_INVALID;
    uint16_t port = 0;
    static long long tag;
    int rc;

    DD_CHECK(p != NULL);
    if (p == NULL)
        return;
    if (pal_iouring_enable_pbuf(p, PBUF_COUNT, PBUF_SIZE) != 0) {
        printf("provided-buffer ring unsupported; skipping armed pbuf free\n");
        pal_iouring_free(p);
        return;
    }
    listener = pal_iouring_listen(p, "127.0.0.1", 0, &port, NULL);
    DD_CHECK(listener != PAL_SOCKET_INVALID);
    if (listener == PAL_SOCKET_INVALID)
        goto cleanup;
    client = pal_tcp_connect("127.0.0.1", port);
    DD_CHECK(client != PAL_SOCKET_INVALID);
    if (client == PAL_SOCKET_INVALID)
        goto cleanup;
    rc = wait_for_accept(p, listener, &ev);
    DD_CHECK_EQ_INT(1, rc);
    if (rc != 1)
        goto cleanup;
    server = ev.fd;
    DD_CHECK_EQ_INT(0, pal_iouring_recv_ms(p, server, &tag));
    pal_iouring_free(p);
    p = NULL;

cleanup:
    if (server != PAL_SOCKET_INVALID)
        pal_close(server);
    if (client != PAL_SOCKET_INVALID)
        pal_close(client);
    if (listener != PAL_SOCKET_INVALID)
        pal_close(listener);
    pal_iouring_free(p);
}

int main(void)
{
    DD_RUN(test_registered_send_buffers);
    DD_RUN(test_send_zc_fixed_completion);
    pal_iouring *probe;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    probe = pal_iouring_create();
    if (probe == NULL) {
        printf("io_uring unavailable; skipping runtime tests\n");
        pal_socket_cleanup();
        return DD_TEST_SUMMARY();
    }
    pal_iouring_free(probe);
    DD_RUN(test_wakeup_publication);
    DD_RUN(test_published_wake_failure);
    DD_RUN(test_published_listen_wake_failure);
    DD_RUN(test_published_recv_wake_failure);
    DD_RUN(test_timeout_operand_lifetime);
    DD_RUN(test_defer_taskrun_or_fallback);
    DD_RUN(test_multishot_provided_buffer);
    DD_RUN(test_pbuf_free_with_armed_recv);
    DD_RUN(test_pbuf_header_allocation);
    DD_RUN(test_pbuf_validation_and_recycle);
    pal_socket_cleanup();
    return DD_TEST_SUMMARY();
}
