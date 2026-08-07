/* test_redbus_elect.c - redis-mode failover vote round on the wire:
 * a ddup slave of a dead master requests votes from two fake masters
 * (raw bus peers), promotes only on majority. */
#include <stdio.h>
#include <string.h>

#include "core/cluster.h"
#include "core/redbus.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/server.h"
#include "test.h"

#define ID_D "dddddddddddddddddddddddddddddddddddddddd"
#define ID_M1 "1111111111111111111111111111111111111111"
#define ID_M2 "2222222222222222222222222222222222222222"

static void put16be(char *p, uint16_t v)
{
    p[0] = (char)((v >> 8) & 0xFFu);
    p[1] = (char)(v & 0xFFu);
}

static void put32be(char *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (char)((v >> (24 - 8 * i)) & 0xFFu);
}

static void put64be(char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (char)((v >> (56 - 8 * i)) & 0xFFu);
}

static uint16_t get16be(const char *p)
{
    return (uint16_t)(((uint16_t)(uint8_t)p[0] << 8) |
                      (uint16_t)(uint8_t)p[1]);
}

static uint32_t get32be(const char *p)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++)
        v = (v << 8) | (uint32_t)(uint8_t)p[i];
    return v;
}

typedef struct fake_peer {
    pal_socket_t fd;
    char rbuf[65536];
    size_t rlen;
    const char *id;
    uint16_t port;
    int ack_requests; /* answer AUTH_REQUEST with AUTH_ACK when set */
} fake_peer;

/* full send of a frame from the static scratch buffer (nonblocking
 * partial sends would corrupt the stream) */
static void peer_write_all(server *a, fake_peer *fp, const char *f, size_t n)
{
    size_t off = 0;
    int guard = 0;
    while (off < n && guard++ < 4000) {
        ptrdiff_t w = pal_send(fp->fd, f + off, n - off);
        if (w > 0)
            off += (size_t)w;
        else
            server_run_once(a, 1);
    }
}

/* PING: sender id (master), claims slots [base, base+count), epoch ep,
 * currentEpoch ep; empty myip (auto-discovery path) */
static void peer_send_ping(server *a, fake_peer *fp, uint32_t base,
                           uint32_t count, uint64_t ep)
{
    static char f[REDBUS_HDR_LEN + 8];
    uint32_t s;
    memset(f, 0, REDBUS_HDR_LEN);
    memcpy(f, "RCmb", 4);
    put32be(f + 4, REDBUS_HDR_LEN);
    put16be(f + 8, 1);
    put16be(f + 10, fp->port);
    put16be(f + 12, REDBUS_TYPE_PING);
    put16be(f + 14, 0);
    put64be(f + 16, ep);
    put64be(f + 24, ep);
    memcpy(f + 40, fp->id, 40);
    for (s = base; s < base + count; s++)
        f[80 + s / 8] |= (char)(1u << (s % 8));
    put16be(f + 2248, (uint16_t)(fp->port + 10000));
    put16be(f + 2250, REDBUS_NODE_MASTER);
    peer_write_all(a, fp, f, REDBUS_HDR_LEN);
}

static void peer_send_ack(server *a, fake_peer *fp, uint64_t epoch)
{
    static char f[REDBUS_HDR_LEN + 8];
    memset(f, 0, REDBUS_HDR_LEN);
    memcpy(f, "RCmb", 4);
    put32be(f + 4, REDBUS_HDR_LEN);
    put16be(f + 8, 1);
    put16be(f + 10, fp->port);
    put16be(f + 12, REDBUS_TYPE_AUTH_ACK);
    put16be(f + 14, 0);
    put64be(f + 16, epoch);
    put64be(f + 24, epoch);
    memcpy(f + 40, fp->id, 40);
    put16be(f + 2248, (uint16_t)(fp->port + 10000));
    put16be(f + 2250, REDBUS_NODE_MASTER);
    peer_write_all(a, fp, f, REDBUS_HDR_LEN);
}

/* PING carrying one gossip entry: D flagged master+PFAIL -- a failure
 * report from this master about the dead one (drives the quorum) */
static void peer_send_ping_report_d(server *a, fake_peer *fp, uint32_t base,
                                    uint32_t count, uint64_t ep)
{
    static char f[REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN];
    uint32_t s;
    memset(f, 0, sizeof(f));
    memcpy(f, "RCmb", 4);
    put32be(f + 4, REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN);
    put16be(f + 8, 1);
    put16be(f + 10, fp->port);
    put16be(f + 12, REDBUS_TYPE_PING);
    put16be(f + 14, 1); /* one gossip entry */
    put64be(f + 16, ep);
    put64be(f + 24, ep);
    memcpy(f + 40, fp->id, 40);
    for (s = base; s < base + count; s++)
        f[80 + s / 8] |= (char)(1u << (s % 8));
    put16be(f + 2248, (uint16_t)(fp->port + 10000));
    put16be(f + 2250, REDBUS_NODE_MASTER);
    /* gossip entry @2256: nodename@0, port@94, cport@96, flags@98 */
    memcpy(f + REDBUS_HDR_LEN, ID_D, 40);
    put16be(f + REDBUS_HDR_LEN + 94, 7001);
    put16be(f + REDBUS_HDR_LEN + 96, 17001);
    put16be(f + REDBUS_HDR_LEN + 98,
            REDBUS_NODE_MASTER | REDBUS_NODE_PFAIL);
    peer_write_all(a, fp, f, REDBUS_HDR_LEN + REDBUS_GOSSIP_LEN);
}

/* pump the server once, then read/parse frames from a peer; ACK any
 * AUTH_REQUEST when allowed */
static void peer_pump(server *a, fake_peer *fp)
{
    ptrdiff_t n;
    server_run_once(a, 5);
    n = pal_recv(fp->fd, fp->rbuf + fp->rlen,
                 sizeof(fp->rbuf) - fp->rlen);
    if (n > 0)
        fp->rlen += (size_t)n;
    while (fp->rlen >= 8) {
        uint32_t tot = get32be(fp->rbuf + 4);
        if (tot < REDBUS_HDR_LEN || tot > sizeof(fp->rbuf))
            break;
        if (fp->rlen < tot)
            break;
        if (get16be(fp->rbuf + 12) == REDBUS_TYPE_AUTH_REQUEST &&
            fp->ack_requests) {
            /* epoch at +16 is u64 BE */
            uint64_t ep = 0;
            int i;
            for (i = 0; i < 8; i++)
                ep = (ep << 8) | (uint64_t)(uint8_t)fp->rbuf[16 + i];
            peer_send_ack(a, fp, ep);
        }
        memmove(fp->rbuf, fp->rbuf + tot, fp->rlen - tot);
        fp->rlen -= tot;
    }
}

static size_t cli_ask(server *a, pal_socket_t c, const char *req, char *buf,
                      size_t cap, fake_peer *p1, fake_peer *p2)
{
    size_t got = 0;
    int iter = 0;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (iter < 4000) {
        ptrdiff_t n;
        iter++;
        peer_pump(a, p1);
        peer_pump(a, p2);
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0) {
            got += (size_t)n;
            break;
        }
    }
    buf[got] = '\0';
    return got;
}

static void test_redis_mode_vote_election(void)
{
    server *a;
    pal_socket_t ca;
    fake_peer dead, m1, m2;
    char buf[8192];
    uint64_t dl;
    int promoted = 0, i;

    DD_CHECK_EQ_INT(0, pal_socket_init());
    a = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
    DD_CHECK(a != NULL);
    server_enable_cluster(
        a, "9999999999999999999999999999999999999999");
    server_set_bus_protocol(a, SERVER_BUS_PROTOCOL_REDIS);
    server_set_node_timeout(a, 1000);
    ca = pal_tcp_connect("127.0.0.1", server_port(a));
    DD_CHECK(ca != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(ca, 1));

    memset(&dead, 0, sizeof(dead));
    memset(&m1, 0, sizeof(m1));
    memset(&m2, 0, sizeof(m2));
    dead.id = ID_D;
    dead.port = 7001;
    m1.id = ID_M1;
    m1.port = 7002;
    m2.id = ID_M2;
    m2.port = 7003;
    dead.fd = pal_tcp_connect("127.0.0.1",
                              (uint16_t)(server_port(a) + 10000));
    m1.fd = pal_tcp_connect("127.0.0.1", (uint16_t)(server_port(a) + 10000));
    m2.fd = pal_tcp_connect("127.0.0.1", (uint16_t)(server_port(a) + 10000));
    DD_CHECK(dead.fd != PAL_SOCKET_INVALID && m1.fd != PAL_SOCKET_INVALID &&
             m2.fd != PAL_SOCKET_INVALID);
    pal_set_nonblocking(dead.fd, 1);
    pal_set_nonblocking(m1.fd, 1);
    pal_set_nonblocking(m2.fd, 1);

    /* introduce all three: D owns 100-199 (epoch 5), M1 0-99, M2 200-299 */
    peer_send_ping(a, &dead, 100, 100, 5);
    peer_send_ping(a, &m1, 0, 100, 2);
    peer_send_ping(a, &m2, 200, 100, 3);
    for (i = 0; i < 20; i++) {
        server_run_once(a, 5);
        peer_pump(a, &m1);
        peer_pump(a, &m2);
        (void)pal_recv(dead.fd, dead.rbuf, sizeof(dead.rbuf));
    }

    /* become D's slave (also tries a data link to the dead port: fails,
     * which is fine for this test) */
    cli_ask(a, ca,
            "*3\r\n$7\r\nCLUSTER\r\n$9\r\nREPLICATE\r\n$40\r\n" ID_D "\r\n",
            buf, sizeof(buf), &m1, &m2);
    DD_CHECK_STR("+OK\r\n", buf);

    /* D goes silent: only M1 ACKs the first election -> no majority.
     * Both fake masters gossip D as PFAIL so the local quorum can mark
     * D FAIL -- promotion may only start from the objective FAIL. */
    m1.ack_requests = 1;
    promoted = 0;
    dl = pal_now_ms() + 15000;
    while (pal_now_ms() < dl && !promoted) {
        peer_pump(a, &m1);
        peer_pump(a, &m2);
        peer_send_ping_report_d(a, &m1, 0, 100, 2);
        peer_send_ping_report_d(a, &m2, 200, 100, 3);
        cli_ask(a, ca, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", buf,
                sizeof(buf), &m1, &m2);
        if (strstr(buf, "myself,master") != NULL)
            promoted = 1;
    }
    DD_CHECK_EQ_INT(0, promoted); /* 1 vote of 3 masters is not enough */

    /* M2 joins the voting: 2 of 3 -> majority -> promotion */
    m2.ack_requests = 1;
    promoted = 0;
    dl = pal_now_ms() + 15000;
    while (pal_now_ms() < dl && !promoted) {
        peer_pump(a, &m1);
        peer_pump(a, &m2);
        peer_send_ping_report_d(a, &m1, 0, 100, 2);
        peer_send_ping_report_d(a, &m2, 200, 100, 3);
        cli_ask(a, ca, "*2\r\n$7\r\nCLUSTER\r\n$5\r\nNODES\r\n", buf,
                sizeof(buf), &m1, &m2);
        if (strstr(buf, "myself,master") != NULL)
            promoted = 1;
    }
    DD_CHECK_EQ_INT(1, promoted);
    /* the promoted node claimed D's slots */
    DD_CHECK(strstr(buf, "100-199") != NULL);

    pal_close(m2.fd);
    pal_close(m1.fd);
    pal_close(dead.fd);
    pal_close(ca);
    server_destroy(a);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_redis_mode_vote_election);
    return DD_TEST_SUMMARY();
}
