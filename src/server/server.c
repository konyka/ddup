/* server.c - single-threaded RESP server; see server.h.
 *
 * Model: one pal_loop, one listening socket, one db, N connections.
 * Connections use blocking sockets: a readiness event is always followed by
 * exactly one pal_recv (which returns immediately with whatever is
 * available), and replies are flushed with a blocking send loop. This is
 * deliberately simple for Phase 3; non-blocking output buffering arrives
 * with the thread-per-core phase.
 *
 * Zero-copy note: parsed values point into the connection receive buffer,
 * so the parse -> execute -> advance loop runs entirely before the buffer
 * is compacted, and the per-connection arena is reset only after the reply
 * for that command has been produced.
 */
#include "server/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/command.h"
#include "core/session.h"
#include "core/snapshot.h"
#include "server/aof.h"
#include "pal/pal_event.h"
#include "pal/pal_iocp.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "pal/pal_tls.h"
#include "server/repl.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"

/* Bytes requested per pal_recv; the receive buffer grows on demand. */
#define SERVER_RECV_CHUNK (64 * 1024)
/* Readiness events consumed per server_run_once() call. */
#define SERVER_MAX_EVENTS 64
/* Replicas with more pending output than this are dropped (re-SYNC). */
#define REPL_MAX_OUTBUF (16 * 1024 * 1024)

/* IOCP backend: single send chunk cap per overlapped WSASend. */
#define IOCP_SEND_CHUNK (256 * 1024)

typedef struct conn conn;

/* pub/sub registry: channel -> list of subscribed conns (server-owned,
 * not part of db; see session hooks in core/session.h) */
typedef struct chan_node {
    struct chan_node *next;
    conn *c;
} chan_node;

typedef struct conn_sub {
    struct conn_sub *next;
    size_t chlen;
    char *ch; /* owned copy */
} conn_sub;

struct conn {
    pal_socket_t fd;
    pal_tls *tls; /* NULL for plain connections */
    int tls_handshaking; /* non-blocking handshake in progress */
    int want_write;      /* registered for write readiness (out pending) */
    /* IOCP backend state */
    int pending_ops;     /* outstanding overlapped ops (recv+send) */
    int zombie;          /* closed with ops in flight: freed at 0 pending */
    int send_outstanding;
    size_t out_sent;     /* bytes of out already handed to the kernel */
    char *sbuf;          /* stable overlapped-send buffer */
    size_t scap;
    char *rbuf;  /* receive buffer, malloc'd, compacted after parsing */
    size_t rlen; /* valid bytes in rbuf */
    size_t rcap; /* allocated size of rbuf */
    resp_buf out;
    arena arena;
    session *sess; /* per-connection command context (MULTI/WATCH/pubsub) */
    conn_sub *subs; /* channels this conn is subscribed to */
    struct server *srv;
    int is_replica;     /* downstream replica (we are the master) */
    int is_master_link; /* our outbound link to the master (we are replica) */
    /* master-link receive state ($<len> snapshot frame, then RESP stream) */
    int link_state;
    size_t link_hdrlen;
    size_t link_need;
    size_t link_got;
    char *link_snap;
};

/* master link states */
#define LINK_SYNC_SENT 0
#define LINK_SNAPSHOT 1
#define LINK_STREAMING 2

/* cluster bus connection (cluster bus protocol v1, server side) */
typedef struct bus_conn {
    struct bus_conn *next;
    pal_socket_t fd;
    int outbound;      /* we initiated it (MEET / gossip target) */
    char *rbuf;
    size_t rlen;
    size_t rcap;
    resp_buf out;
    int want_write;
} bus_conn;

struct server {
    pal_loop *loop;
    pal_socket_t listen_fd;
    pal_socket_t tls_listen_fd; /* PAL_SOCKET_INVALID when TLS is off */
    pal_tls_ctx *tls_ctx;
    uint16_t tls_port;
    /* cluster bus (second listener on port+10000) */
    pal_socket_t bus_listen_fd;
    bus_conn *bus;
    char nodes_path[1024];
    int nodes_dirty;
    uint64_t last_cluster_changes;
    uint64_t last_gossip;
    uint64_t last_nodes_save;
    uint64_t node_timeout_ms;
    db db;
    rh_table channels; /* pub/sub: channel -> chan_node list head (8-byte ptr) */
    aof *aof;          /* NULL when appendonly=no */
    int shutdown_flag;
    int save_sec;               /* automatic snapshot interval, 0 = off */
    uint64_t last_save_check;   /* pal_now_ms of the last interval check */
    uint64_t dirty_at_last_save;
    /* replication */
    int role;             /* SESSION_ROLE_* */
    repl_backlog backlog; /* propagated command stream (master side) */
    resp_buf prop_buf;    /* reusable propagation serialization buffer */
    repl_info repl;       /* INFO replication snapshot */
    conn *master_link;    /* outbound link to the master (replica side) */
    uint64_t last_reconnect; /* pal_now_ms of the last connect attempt */
    /* IOCP backend (Windows) */
    int backend;
    pal_iocp *iocp;
    conn **zombies; /* conns closed with ops in flight, freed when drained */
    size_t nzombies;
    size_t zombie_cap;
    conn **conns;
    size_t nconns;
    size_t cap;
    uint16_t port;
    uint64_t last_active_expire; /* pal_now_ms of the last active cycle */
};

static void conn_close(server *s, size_t idx);
static conn *conn_create(server *srv, pal_socket_t fd);
static void repl_link_close(server *srv);
static int repl_link_connect(server *srv);
static int server_run_once_iocp(server *s, int timeout_ms);
static void bus_conn_free(server *s, bus_conn *bc);
static void cluster_nodes_save(server *s);
static int srv_cluster_meet(void *ctx, const char *ip, uint16_t port);

/* ------------------------------------------------------------------ */
/* pub/sub registry + session hooks                                   */
/* ------------------------------------------------------------------ */

static chan_node *chan_get(server *srv, const char *ch, size_t len)
{
    const char *v;
    size_t vl;
    chan_node *head = NULL;
    if (rh_get(&srv->channels, ch, len, &v, &vl) && vl == 8)
        memcpy(&head, v, 8);
    return head;
}

static void chan_put(server *srv, const char *ch, size_t len, chan_node *head)
{
    char b[8];
    if (head == NULL) {
        rh_del(&srv->channels, ch, len);
        return;
    }
    memcpy(b, &head, 8);
    rh_set(&srv->channels, ch, len, b, 8);
}

static size_t srv_subscribe(void *ctx, session *sess, const char *ch,
                            size_t len)
{
    server *srv = (server *)ctx;
    conn *c = (conn *)sess->owner;
    chan_node *head = chan_get(srv, ch, len);
    chan_node *n;
    conn_sub *cs;
    for (n = head; n != NULL; n = n->next)
        if (n->c == c)
            return sess->nsub; /* already subscribed */
    n = (chan_node *)malloc(sizeof(*n));
    if (n == NULL)
        return sess->nsub;
    n->c = c;
    n->next = head;
    chan_put(srv, ch, len, n);
    cs = (conn_sub *)malloc(sizeof(*cs));
    cs->ch = (char *)malloc(len);
    if (cs == NULL || cs->ch == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    memcpy(cs->ch, ch, len);
    cs->chlen = len;
    cs->next = c->subs;
    c->subs = cs;
    sess->nsub++;
    return sess->nsub;
}

static size_t srv_unsubscribe(void *ctx, session *sess, const char *ch,
                              size_t len)
{
    server *srv = (server *)ctx;
    conn *c = (conn *)sess->owner;
    chan_node *head = chan_get(srv, ch, len);
    chan_node **pp = &head;
    conn_sub **csp;
    while (*pp != NULL && (*pp)->c != c)
        pp = &(*pp)->next;
    if (*pp == NULL)
        return sess->nsub; /* not subscribed */
    {
        chan_node *dead = *pp;
        *pp = dead->next;
        free(dead);
    }
    chan_put(srv, ch, len, head);
    for (csp = &c->subs; *csp != NULL; csp = &(*csp)->next) {
        if ((*csp)->chlen == len && memcmp((*csp)->ch, ch, len) == 0) {
            conn_sub *dead = *csp;
            *csp = dead->next;
            free(dead->ch);
            free(dead);
            break;
        }
    }
    if (sess->nsub > 0)
        sess->nsub--;
    return sess->nsub;
}

static void srv_each_channel(void *ctx, session *sess,
                             void (*cb)(const char *ch, size_t len, void *arg),
                             void *arg)
{
    conn *c = (conn *)sess->owner;
    conn_sub *cs;
    (void)ctx;
    for (cs = c->subs; cs != NULL; cs = cs->next)
        cb(cs->ch, cs->chlen, arg);
}

static long srv_publish(void *ctx, const char *ch, size_t chlen,
                        const char *msg, size_t mlen)
{
    server *srv = (server *)ctx;
    chan_node *n = chan_get(srv, ch, chlen);
    long count = 0;
    for (; n != NULL; n = n->next) {
        conn *sc = n->c;
        if (sc->sess->deliver != NULL)
            sc->sess->deliver(sc->sess->owner, ch, chlen, msg, mlen);
        count++;
    }
    return count;
}

static void srv_deliver(void *owner, const char *ch, size_t chlen,
                        const char *msg, size_t mlen)
{
    conn *c = (conn *)owner;
    resp_write_array_header(&c->out, 3);
    resp_write_bulk(&c->out, "message", 7);
    resp_write_bulk(&c->out, ch, chlen);
    resp_write_bulk(&c->out, msg, mlen);
}

/* Propagation sink for every successfully-applied mutating command:
 * serialize once, then fan out to AOF (if any), the replication backlog
 * and all downstream replica conns (flushed at end of run_once). */
static void srv_propagate(void *ctx, const resp_value *argv, size_t argc)
{
    server *srv = (server *)ctx;
    size_t i;
    if (srv->aof != NULL)
        aof_log_cmd(srv->aof, argv, argc);

    srv->prop_buf.len = 0;
    resp_write_array_header(&srv->prop_buf, argc);
    for (i = 0; i < argc; i++) {
        if (argv[i].type == RESP_BULK_STRING ||
            argv[i].type == RESP_SIMPLE_STRING)
            resp_write_bulk(&srv->prop_buf, argv[i].str, argv[i].len);
        else
            resp_write_bulk(&srv->prop_buf, "", 0);
    }
    repl_backlog_append(&srv->backlog, srv->prop_buf.data, srv->prop_buf.len);
    srv->repl.offset = srv->backlog.offset;
    if (srv->repl.connected_slaves == 0)
        return; /* O(1): no downstream replicas to fan out to */
    for (i = 0; i < srv->nconns; i++) {
        conn *rc = srv->conns[i];
        if (rc->is_replica) {
            resp_buf_reserve(&rc->out, srv->prop_buf.len);
            memcpy(rc->out.data + rc->out.len, srv->prop_buf.data,
                   srv->prop_buf.len);
            rc->out.len += srv->prop_buf.len;
        }
    }
}

static void srv_request_shutdown(void *ctx)
{
    ((server *)ctx)->shutdown_flag = 1;
}

/* REPLICAOF hook: (re)point the replica at a master, or promote when
 * host == NULL (REPLICAOF NO ONE). */
static int srv_replicaof(void *ctx, const char *host, uint16_t port)
{
    server *srv = (server *)ctx;
    if (srv->backend == SERVER_BACKEND_IOCP)
        return -1; /* replica-side master link needs the readiness backend */
    repl_link_close(srv);
    if (host == NULL) {
        srv->role = SESSION_ROLE_MASTER;
        srv->repl.role = SESSION_ROLE_MASTER;
        srv->repl.link_up = 0;
        return 0;
    }
    snprintf(srv->repl.master_host, sizeof(srv->repl.master_host), "%s",
             host);
    srv->repl.master_port = port;
    srv->role = SESSION_ROLE_REPLICA;
    srv->repl.role = SESSION_ROLE_REPLICA;
    srv->repl.link_up = 0;
    return repl_link_connect(srv);
}

int server_replicaof(server *s, const char *host, uint16_t port)
{
    return srv_replicaof(s, host, port);
}

void server_set_backlog_size(server *s, size_t bytes)
{
    repl_backlog_free(&s->backlog);
    repl_backlog_init(&s->backlog, bytes);
}

/* SYNC: write the $<len>\r\n<snapshot> full-resync frame into the conn's
 * out buffer, then mark it as a downstream replica. */
static void srv_sync(void *ctx, session *sess)
{
    server *srv = (server *)ctx;
    conn *c = (conn *)sess->owner;
    resp_buf snap;
    char hdr[32];
    int hl;

    resp_buf_init(&snap);
    snapshot_serialize(&srv->db, &snap);
    hl = snprintf(hdr, sizeof(hdr), "$%llu\r\n",
                  (unsigned long long)snap.len);
    resp_buf_reserve(&c->out, (size_t)hl + snap.len);
    memcpy(c->out.data + c->out.len, hdr, (size_t)hl);
    c->out.len += (size_t)hl;
    memcpy(c->out.data + c->out.len, snap.data, snap.len);
    c->out.len += snap.len;
    resp_buf_free(&snap);
    if (!c->is_replica) {
        c->is_replica = 1;
        srv->repl.connected_slaves++;
    }
}

static conn *conn_create(server *srv, pal_socket_t fd)
{
    conn *c = (conn *)calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->fd = fd;
    c->srv = srv;
    c->sess = session_create(&srv->db);
    c->rcap = SERVER_RECV_CHUNK;
    c->rbuf = (char *)malloc(c->rcap);
    if (c->rbuf == NULL || c->sess == NULL) {
        session_free(c->sess);
        free(c->rbuf);
        free(c);
        return NULL;
    }
    c->sess->ps_ctx = srv;
    c->sess->owner = c;
    c->sess->subscribe = srv_subscribe;
    c->sess->unsubscribe = srv_unsubscribe;
    c->sess->each_channel = srv_each_channel;
    c->sess->publish = srv_publish;
    c->sess->deliver = srv_deliver;
    c->sess->shutdown_ctx = srv;
    c->sess->request_shutdown = srv_request_shutdown;
    c->sess->repl = &srv->repl;
    c->sess->role = &srv->role;
    c->sess->sync_ctx = srv;
    c->sess->sync_hook = srv_sync;
    c->sess->replicaof_ctx = srv;
    c->sess->replicaof_hook = srv_replicaof;
    c->sess->cluster_ctx = srv;
    c->sess->cluster_meet = srv_cluster_meet;
    c->sess->cluster_replicate = srv_replicaof;
    /* every conn propagates mutations through the server mux (AOF +
     * backlog + downstream replicas) */
    c->sess->aof_ctx = srv;
    c->sess->aof_log = srv_propagate;
    resp_buf_init(&c->out);
    arena_init(&c->arena, 4096);
    return c;
}

static void conn_free(conn *c)
{
    if (c == NULL)
        return;
    /* unsubscribe from every channel before disappearing */
    while (c->subs != NULL)
        srv_unsubscribe(c->srv, c->sess, c->subs->ch, c->subs->chlen);
    if (c->tls != NULL) {
        pal_tls_shutdown(c->tls);
        pal_tls_free(c->tls);
    }
    pal_close(c->fd);
    session_free(c->sess);
    free(c->link_snap);
    free(c->sbuf);
    free(c->rbuf);
    resp_buf_free(&c->out);
    arena_destroy(&c->arena);
    free(c);
}

/* conn IO: TLS when attached, plain socket otherwise */
static ptrdiff_t conn_read(conn *c, void *buf, size_t n)
{
    if (c->tls != NULL)
        return pal_tls_read(c->tls, buf, n);
    return pal_recv(c->fd, buf, n);
}

static ptrdiff_t conn_write(conn *c, const void *buf, size_t n)
{
    if (c->tls != NULL)
        return pal_tls_write(c->tls, buf, n);
    return pal_send(c->fd, buf, n);
}

/* ------------------------------------------------------------------ */
/* master link (replica side)                                         */
/* ------------------------------------------------------------------ */

static int repl_link_connect(server *srv)
{
    pal_socket_t fd;
    conn *c;
    static const char sync_cmd[] = "*1\r\n$4\r\nSYNC\r\n";

    srv->last_reconnect = pal_now_ms();
    fd = pal_tcp_connect(srv->repl.master_host, srv->repl.master_port);
    if (fd == PAL_SOCKET_INVALID)
        return -1;
    c = conn_create(srv, fd);
    if (c == NULL) {
        pal_close(fd);
        return -1;
    }
    c->is_master_link = 1;
    c->sess->repl_link = 1;
    if (srv->nconns == srv->cap) {
        size_t ncap = srv->cap == 0 ? 16 : srv->cap * 2;
        conn **nc = (conn **)realloc(srv->conns, ncap * sizeof(*nc));
        if (nc == NULL) {
            conn_free(c);
            return -1;
        }
        srv->conns = nc;
        srv->cap = ncap;
    }
    srv->conns[srv->nconns++] = c;
    if (pal_loop_add(srv->loop, fd, 1, 0, c) != 0) {
        conn_close(srv, srv->nconns - 1);
        return -1;
    }
    srv->master_link = c;
    resp_buf_reserve(&c->out, sizeof(sync_cmd) - 1);
    memcpy(c->out.data, sync_cmd, sizeof(sync_cmd) - 1);
    c->out.len = sizeof(sync_cmd) - 1;
    c->link_state = LINK_SYNC_SENT;
    return 0;
}

static void repl_link_close(server *srv)
{
    size_t i;
    if (srv->master_link == NULL)
        return;
    for (i = 0; i < srv->nconns; i++)
        if (srv->conns[i] == srv->master_link) {
            conn_close(srv, i);
            break;
        }
    srv->master_link = NULL;
    srv->repl.link_up = 0;
}

/* Service the outbound master link: read the $<len> snapshot frame, load
 * it, then apply the streamed commands (replies discarded). */
static void repl_link_service(server *srv, conn *c)
{
    ptrdiff_t n;

    if (c->rcap - c->rlen < SERVER_RECV_CHUNK) {
        size_t ncap = c->rcap * 2;
        char *nb = (char *)realloc(c->rbuf, ncap);
        if (nb == NULL) {
            repl_link_close(srv);
            return;
        }
        c->rbuf = nb;
        c->rcap = ncap;
    }
    n = conn_read(c, c->rbuf + c->rlen, c->rcap - c->rlen);
    if (n == 0 || (n < 0 && n != -2 && !pal_would_block(pal_socket_error()))) {
        repl_link_close(srv); /* link down; the retry timer reconnects */
        return;
    }
    if (n < 0)
        return;
    c->rlen += (size_t)n;

    if (c->link_state == LINK_SYNC_SENT) {
        size_t pos = 0;
        size_t i;
        uint64_t slen = 0;
        while (pos < c->rlen && c->rbuf[pos] != '\n')
            pos++;
        if (pos == c->rlen) {
            if (c->rlen > 64)
                repl_link_close(srv); /* garbage instead of a frame */
            return;                 /* wait for the rest of the header */
        }
        if (c->rbuf[0] != '$') {
            repl_link_close(srv);
            return;
        }
        for (i = 1; i < pos && c->rbuf[i] >= '0' && c->rbuf[i] <= '9'; i++)
            slen = slen * 10 + (unsigned)(c->rbuf[i] - '0');
        c->link_need = (size_t)slen;
        c->link_snap = (char *)malloc(c->link_need == 0 ? 1 : c->link_need);
        if (c->link_snap == NULL) {
            repl_link_close(srv);
            return;
        }
        c->link_got = 0;
        c->link_hdrlen = pos + 1;
        c->link_state = LINK_SNAPSHOT;
    }
    if (c->link_state == LINK_SNAPSHOT) {
        size_t avail = c->rlen - c->link_hdrlen - c->link_got;
        size_t want = c->link_need - c->link_got;
        size_t take = avail < want ? avail : want;
        memcpy(c->link_snap + c->link_got,
               c->rbuf + c->link_hdrlen + c->link_got, take);
        c->link_got += take;
        if (c->link_got < c->link_need) {
            c->rlen = 0; /* snapshot may exceed the recv chunk; restart fill */
            return;
        }
        db_flush(&srv->db);
        (void)snapshot_load_mem(&srv->db, c->link_snap, c->link_need,
                                pal_wall_ms());
        free(c->link_snap);
        c->link_snap = NULL;
        {
            size_t used = c->link_hdrlen + c->link_need;
            memmove(c->rbuf, c->rbuf + used, c->rlen - used);
            c->rlen -= used;
        }
        c->link_state = LINK_STREAMING;
        srv->repl.link_up = 1;
    }
    if (c->link_state == LINK_STREAMING) {
        size_t off = 0;
        while (off < c->rlen) {
            resp_value v;
            ptrdiff_t used =
                resp_parse(c->rbuf + off, c->rlen - off, &v, &c->arena);
            if (used == 0)
                break; /* incomplete command: wait for more bytes */
            if (used < 0 || v.type != RESP_ARRAY || v.is_null) {
                repl_link_close(srv); /* stream desync: full resync */
                return;
            }
            c->out.len = 0; /* replies are discarded */
            session_execute(c->sess, v.items, v.count, &c->out);
            c->out.len = 0;
            arena_reset(&c->arena);
            off += (size_t)used;
        }
        if (off > 0) {
            memmove(c->rbuf, c->rbuf + off, c->rlen - off);
            c->rlen -= off;
        }
    }
}

server *server_create_ex(const char *host, uint16_t port, int backend)
{
    server *s = (server *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->listen_fd = PAL_SOCKET_INVALID;
    s->tls_listen_fd = PAL_SOCKET_INVALID;
    s->bus_listen_fd = PAL_SOCKET_INVALID;
    s->node_timeout_ms = 15000;
    s->backend = backend;
    s->loop = pal_loop_create();
    if (backend == SERVER_BACKEND_IOCP) {
        s->iocp = pal_iocp_create();
        if (s->iocp == NULL)
            s->backend = SERVER_BACKEND_SELECT; /* unavailable: fall back */
    }
    if (s->backend == SERVER_BACKEND_IOCP)
        s->listen_fd = pal_iocp_listen(s->iocp, host, port, &s->port, NULL);
    else
        s->listen_fd = pal_tcp_listen(host, port, 128, &s->port);
    if (s->loop == NULL || s->listen_fd == PAL_SOCKET_INVALID) {
        server_destroy(s);
        return NULL;
    }
    db_init(&s->db);
    rh_init(&s->channels);
    if (host != NULL)
        snprintf(s->db.cluster_ip, sizeof(s->db.cluster_ip), "%s", host);
    s->db.cluster_port = s->port;
    s->role = SESSION_ROLE_MASTER;
    repl_backlog_init(&s->backlog, 1024 * 1024);
    resp_buf_init(&s->prop_buf);
    memset(&s->repl, 0, sizeof(s->repl));
    s->repl.role = SESSION_ROLE_MASTER;
    if (s->backend != SERVER_BACKEND_IOCP &&
        pal_loop_add(s->loop, s->listen_fd, 1, 0, NULL) != 0) {
        server_destroy(s);
        return NULL;
    }
    return s;
}

server *server_create(const char *host, uint16_t port)
{
    return server_create_ex(host, port, SERVER_BACKEND_SELECT);
}

uint16_t server_port(const server *s)
{
    return s->port;
}

/* Start a TLS listener alongside the plain one (port 0 = ephemeral).
 * Returns 0 on success; -1 when TLS is unavailable (stub build) or the
 * cert/key/listen setup failed. */
int server_enable_tls(server *s, const char *host, uint16_t port,
                      const char *cert_file, const char *key_file)
{
    if (s->backend == SERVER_BACKEND_IOCP)
        return -1; /* TLS needs the readiness backend (documented) */
    s->tls_ctx = pal_tls_ctx_new(cert_file, key_file);
    if (s->tls_ctx == NULL)
        return -1;
    s->tls_listen_fd = pal_tcp_listen(host, port, 128, &s->tls_port);
    if (s->tls_listen_fd == PAL_SOCKET_INVALID) {
        pal_tls_ctx_free(s->tls_ctx);
        s->tls_ctx = NULL;
        return -1;
    }
    if (pal_loop_add(s->loop, s->tls_listen_fd, 1, 0, NULL) != 0) {
        pal_close(s->tls_listen_fd);
        s->tls_listen_fd = PAL_SOCKET_INVALID;
        pal_tls_ctx_free(s->tls_ctx);
        s->tls_ctx = NULL;
        return -1;
    }
    return 0;
}

uint16_t server_tls_port(const server *s)
{
    return s->tls_port;
}

int server_enable_aof(server *s, const char *path)
{
    if (pal_file_exists(path))
        aof_replay(&s->db, path);
    s->aof = aof_open(path);
    return s->aof != NULL ? 0 : -1;
}

void server_set_snapshot_path(server *s, const char *path)
{
    s->db.snapshot_path = path;
}

int server_load_snapshot(server *s)
{
    if (s->db.snapshot_path == NULL)
        return -1;
    return snapshot_load(&s->db, s->db.snapshot_path, pal_wall_ms());
}

/* Load persisted nodes.conf lines into the node table (multi-node reload).
 * Best-effort: malformed lines are skipped. */
void server_load_nodes(server *s, const char *path)
{
    pal_file *f;
    char line[512];
    if (!pal_file_exists(path))
        return;
    f = pal_file_open_read(path);
    if (f == NULL)
        return;
    {
        size_t used = 0;
        char ch;
        while (pal_file_read(f, &ch, 1) == 1) {
            if (ch == '\n') {
                if (used > 0)
                    (void)cluster_nodes_parse_line(&s->db, line, used);
                used = 0;
            } else if (used + 1 < sizeof(line)) {
                line[used++] = ch;
            } else {
                used = 0; /* overlong line: drop it */
            }
        }
        if (used > 0)
            (void)cluster_nodes_parse_line(&s->db, line, used);
    }
    pal_file_close(f);
}

void server_enable_cluster(server *s, const char *node_id)
{
    cluster_node *me;

    s->db.cluster_enabled = 1;
    snprintf(s->db.node_id, sizeof(s->db.node_id), "%s", node_id);

    /* myself in the node table. Fresh boot owns NOTHING (Redis behavior);
     * assignments come from ADDSLOTS/SETSLOT or a reloaded nodes.conf. */
    me = cluster_node_add(&s->db, node_id);
    if (me != NULL) {
        snprintf(me->ip, sizeof(me->ip), "%s", s->db.cluster_ip);
        me->port = s->db.cluster_port;
        me->bus_port = (uint16_t)(s->db.cluster_port + 10000);
        me->flags = CLUSTER_NODE_MYSELF | CLUSTER_NODE_MASTER;
        me->last_seen_ms = pal_wall_ms();
        s->nodes_dirty = 1;
    }

    /* cluster bus listener on port+10000 (readiness backend only) */
    if (s->backend == SERVER_BACKEND_IOCP ||
        s->bus_listen_fd != PAL_SOCKET_INVALID)
        return; /* unsupported backend, or already listening */
    s->bus_listen_fd = pal_tcp_listen("0.0.0.0",
                                      (uint16_t)(s->db.cluster_port + 10000),
                                      128, NULL);
    if (s->bus_listen_fd != PAL_SOCKET_INVALID)
        (void)pal_loop_add(s->loop, s->bus_listen_fd, 1, 0, NULL);
}

void server_set_save_interval(server *s, int sec)
{
    s->save_sec = sec;
    s->last_save_check = pal_now_ms();
    s->dirty_at_last_save = s->db.dirty;
}

int server_shutdown_requested(const server *s)
{
    return s->shutdown_flag;
}

void server_graceful_stop(server *s)
{
    /* snapshot only when an interval was configured (and AOF is off);
     * the AOF is flushed by server_destroy -> aof_close. */
    if (s->aof == NULL && s->save_sec > 0 &&
        s->db.snapshot_path != NULL &&
        s->db.dirty != s->dirty_at_last_save &&
        snapshot_save(&s->db, s->db.snapshot_path) == 0)
        s->db.last_save = pal_wall_ms() / 1000;
}

void server_destroy(server *s)
{
    size_t i;
    if (s == NULL)
        return;
    if (s->db.cluster_enabled) {
        s->nodes_dirty = 1;
        cluster_nodes_save(s);
    }
    while (s->bus != NULL)
        bus_conn_free(s, s->bus);
    if (s->bus_listen_fd != PAL_SOCKET_INVALID) {
        pal_loop_del(s->loop, s->bus_listen_fd);
        pal_close(s->bus_listen_fd);
    }
    for (i = 0; i < s->nconns; i++)
        conn_free(s->conns[i]);
    free(s->conns);
    for (i = 0; i < s->nzombies; i++)
        conn_free(s->zombies[i]);
    free(s->zombies);
    if (s->loop != NULL && s->listen_fd != PAL_SOCKET_INVALID &&
        s->backend != SERVER_BACKEND_IOCP)
        pal_loop_del(s->loop, s->listen_fd);
    pal_close(s->listen_fd);
    if (s->tls_listen_fd != PAL_SOCKET_INVALID) {
        pal_loop_del(s->loop, s->tls_listen_fd);
        pal_close(s->tls_listen_fd);
    }
    pal_tls_ctx_free(s->tls_ctx);
    pal_iocp_free(s->iocp);
    if (s->loop != NULL)
        pal_loop_free(s->loop);
    rh_destroy(&s->channels);
    aof_close(s->aof);
    repl_backlog_free(&s->backlog);
    resp_buf_free(&s->prop_buf);
    db_destroy(&s->db);
    free(s);
}

/* ------------------------------------------------------------------ */
/* connection lifecycle                                               */
/* ------------------------------------------------------------------ */

static void server_accept(server *s, pal_socket_t lfd, int use_tls)
{
    /* The listener socket is blocking, but a readiness event guarantees at
     * least one pending connection, so this accept does not block. */
    pal_socket_t fd = pal_accept(lfd);
    conn *c;
    pal_tls *tls = NULL;
    if (fd == PAL_SOCKET_INVALID)
        return;
    if (use_tls) {
        /* non-blocking accept handshake: completes in the event loop */
        if (pal_set_nonblocking(fd, 1) != 0) {
            pal_close(fd);
            return;
        }
        tls = pal_tls_new(s->tls_ctx, fd);
        if (tls == NULL) {
            pal_close(fd);
            return;
        }
    } else if (pal_set_nonblocking(fd, 1) != 0) {
        pal_close(fd); /* all conns are non-blocking (event-driven writes) */
        return;
    }
    c = conn_create(s, fd);
    if (c == NULL) {
        pal_tls_free(tls);
        pal_close(fd);
        return;
    }
    c->tls = tls;
    c->tls_handshaking = use_tls;
    if (s->nconns == s->cap) {
        size_t ncap = s->cap == 0 ? 16 : s->cap * 2;
        conn **nc = (conn **)realloc(s->conns, ncap * sizeof(*nc));
        if (nc == NULL) {
            conn_free(c);
            return;
        }
        s->conns = nc;
        s->cap = ncap;
    }
    s->conns[s->nconns++] = c;
    if (pal_loop_add(s->loop, fd, 1, use_tls ? 1 : 0, c) != 0)
        conn_close(s, s->nconns - 1);
}

static void zombie_push(server *s, conn *c)
{
    if (s->nzombies == s->zombie_cap) {
        size_t ncap = s->zombie_cap == 0 ? 8 : s->zombie_cap * 2;
        conn **nz = (conn **)realloc(s->zombies, ncap * sizeof(*nz));
        if (nz == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        s->zombies = nz;
        s->zombie_cap = ncap;
    }
    s->zombies[s->nzombies++] = c;
}

static void conn_close(server *s, size_t idx)
{
    conn *c = s->conns[idx];
    if (c->is_replica && s->repl.connected_slaves > 0)
        s->repl.connected_slaves--;
    s->conns[idx] = s->conns[s->nconns - 1];
    s->nconns--;
    if (s->backend == SERVER_BACKEND_IOCP) {
        /* outstanding ops complete later; free only when fully drained */
        if (c->pending_ops > 0) {
            pal_iocp_close(s->iocp, c->fd);
            c->zombie = 1;
            zombie_push(s, c);
            return;
        }
        pal_close(c->fd);
        conn_free(c);
        return;
    }
    pal_loop_del(s->loop, c->fd);
    conn_free(c);
}

/* Flush conn->out without ever blocking the loop: send until the socket
 * would block, keep the remainder, and track write-readiness registration
 * (read+write while anything is pending, read-only when fully flushed).
 * Returns 0 (possibly with remainder kept), -1 on fatal error.
 * Readiness backend only; the IOCP backend uses kick_flush(). */
static int conn_flush(server *s, conn *c)
{
    size_t sent = 0;
    while (sent < c->out.len) {
        ptrdiff_t n = conn_write(c, c->out.data + sent, c->out.len - sent);
        if (n == -2 || (n < 0 && pal_would_block(pal_socket_error())))
            break; /* would-block: keep the remainder for writable events */
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    if (sent > 0) {
        memmove(c->out.data, c->out.data + sent, c->out.len - sent);
        c->out.len -= sent;
    }
    {
        int want = c->out.len > 0 ? 1 : 0;
        if (want != c->want_write) {
            pal_loop_mod(s->loop, c->fd, 1, want, c);
            c->want_write = want;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* cluster bus (ddup cluster protocol v1)                             */
/* ------------------------------------------------------------------ */

void server_set_nodes_path(server *s, const char *path)
{
    snprintf(s->nodes_path, sizeof(s->nodes_path), "%s", path);
}

void server_set_node_timeout(server *s, uint64_t ms)
{
    s->node_timeout_ms = ms;
}

static bus_conn *bus_conn_new(pal_socket_t fd, int outbound)
{
    bus_conn *bc = (bus_conn *)calloc(1, sizeof(*bc));
    if (bc == NULL)
        return NULL;
    bc->fd = fd;
    bc->outbound = outbound;
    bc->rcap = CLUSTER_MSG_MAX;
    bc->rbuf = (char *)malloc(bc->rcap);
    if (bc->rbuf == NULL) {
        free(bc);
        return NULL;
    }
    resp_buf_init(&bc->out);
    return bc;
}

static void bus_conn_free(server *s, bus_conn *bc)
{
    bus_conn **pp;
    if (bc == NULL)
        return;
    for (pp = &s->bus; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == bc) {
            *pp = bc->next;
            break;
        }
    }
    pal_loop_del(s->loop, bc->fd);
    pal_close(bc->fd);
    free(bc->rbuf);
    resp_buf_free(&bc->out);
    free(bc);
}

static void bus_conn_add(server *s, bus_conn *bc)
{
    bc->next = s->bus;
    s->bus = bc;
    (void)pal_loop_add(s->loop, bc->fd, 1, bc->want_write, bc);
}

/* non-blocking flush of a bus conn's out buffer */
static int bus_flush(server *s, bus_conn *bc)
{
    size_t sent = 0;
    while (sent < bc->out.len) {
        ptrdiff_t n = pal_send(bc->fd, bc->out.data + sent,
                               bc->out.len - sent);
        if (n < 0 && pal_would_block(pal_socket_error()))
            break;
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    if (sent > 0) {
        memmove(bc->out.data, bc->out.data + sent, bc->out.len - sent);
        bc->out.len -= sent;
    }
    {
        int want = bc->out.len > 0 ? 1 : 0;
        if (want != bc->want_write) {
            bc->want_write = want;
            pal_loop_mod(s->loop, bc->fd, 1, want, bc);
        }
    }
    return 0;
}

static void bus_queue_frame(server *s, bus_conn *bc, int type)
{
    cluster_bus_build_frame(&s->db, type, &bc->out);
    bus_flush(s, bc);
}

static void bus_accept(server *s)
{
    pal_socket_t fd = pal_accept(s->bus_listen_fd);
    bus_conn *bc;
    if (fd == PAL_SOCKET_INVALID)
        return;
    if (pal_set_nonblocking(fd, 1) != 0) {
        pal_close(fd);
        return;
    }
    bc = bus_conn_new(fd, 0);
    if (bc == NULL) {
        pal_close(fd);
        return;
    }
    bus_conn_add(s, bc);
}

/* open an outbound bus conn to a peer's bus port (loopback-fast blocking
 * connect, documented simplification) */
static bus_conn *bus_connect(server *s, const char *ip, uint16_t bus_port)
{
    pal_socket_t fd = pal_tcp_connect(ip, bus_port);
    bus_conn *bc;
    if (fd == PAL_SOCKET_INVALID)
        return NULL;
    if (pal_set_nonblocking(fd, 1) != 0) {
        pal_close(fd);
        return NULL;
    }
    bc = bus_conn_new(fd, 1);
    if (bc == NULL) {
        pal_close(fd);
        return NULL;
    }
    bus_conn_add(s, bc);
    return bc;
}

static int srv_cluster_meet(void *ctx, const char *ip, uint16_t port)
{
    server *s = (server *)ctx;
    bus_conn *bc = bus_connect(s, ip, (uint16_t)(port + 10000));
    if (bc == NULL)
        return -1;
    bus_queue_frame(s, bc, CLUSTER_MSG_MEET);
    return 0;
}

static void bus_service(server *s, bus_conn *bc, int writable)
{
    if (writable && bus_flush(s, bc) != 0) {
        bus_conn_free(s, bc);
        return;
    }
    for (;;) {
        ptrdiff_t n = pal_recv(bc->fd, bc->rbuf + bc->rlen,
                               bc->rcap - bc->rlen);
        if (n == 0 ||
            (n < 0 && !pal_would_block(pal_socket_error()))) {
            bus_conn_free(s, bc);
            return;
        }
        if (n < 0)
            break;
        bc->rlen += (size_t)n;
        if ((size_t)n < bc->rcap - bc->rlen)
            break;
    }
    /* parse complete frames */
    while (bc->rlen >= 10) {
        uint32_t totlen = (uint32_t)(uint8_t)bc->rbuf[4] |
                          ((uint32_t)(uint8_t)bc->rbuf[5] << 8) |
                          ((uint32_t)(uint8_t)bc->rbuf[6] << 16) |
                          ((uint32_t)(uint8_t)bc->rbuf[7] << 24);
        if (totlen > CLUSTER_MSG_MAX || totlen < 10) {
            bus_conn_free(s, bc);
            return;
        }
        if (bc->rlen < totlen)
            break; /* incomplete frame */
        if (cluster_bus_handle_frame(&s->db, bc->rbuf, totlen, &bc->out,
                                     pal_wall_ms()) != 0) {
            bus_conn_free(s, bc);
            return;
        }
        s->nodes_dirty = 1;
        memmove(bc->rbuf, bc->rbuf + totlen, bc->rlen - totlen);
        bc->rlen -= totlen;
    }
    bus_flush(s, bc);
}

static void cluster_gossip_round(server *s)
{
    bus_conn *bc;
    /* ping every outbound conn and every known peer we can reach */
    for (bc = s->bus; bc != NULL; bc = bc->next)
        if (bc->outbound)
            bus_queue_frame(s, bc, CLUSTER_MSG_PING);
}

static void cluster_fail_check(server *s, uint64_t now_ms)
{
    int i;
    for (i = 0; i < s->db.nnodes; i++) {
        cluster_node *n = &s->db.nodes[i];
        if ((n->flags & (CLUSTER_NODE_MYSELF | CLUSTER_NODE_DISCONNECTED)) ==
            0 &&
            n->last_seen_ms > 0 &&
            now_ms - n->last_seen_ms > s->node_timeout_ms) {
            n->flags |= CLUSTER_NODE_DISCONNECTED;
            s->nodes_dirty = 1;
        }
    }
}

static void cluster_nodes_save(server *s)
{
    resp_buf buf;
    char tmp[1088];
    pal_file *f;
    if (s->nodes_path[0] == '\0')
        return;
    if (!s->nodes_dirty && s->last_cluster_changes == s->db.cluster_changes)
        return;
    resp_buf_init(&buf);
    if (cluster_nodes_render(&s->db, &buf) != 0) {
        resp_buf_free(&buf);
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->nodes_path);
    f = pal_file_open_write(tmp);
    if (f != NULL) {
        if (pal_file_write(f, buf.data, buf.len) == (ptrdiff_t)buf.len &&
            pal_file_flush(f) == 0) {
            pal_file_close(f);
            if (pal_file_rename(tmp, s->nodes_path) == 0) {
                s->nodes_dirty = 0;
                s->last_cluster_changes = s->db.cluster_changes;
            }
        } else {
            pal_file_close(f);
            pal_file_unlink(tmp);
        }
    }
    resp_buf_free(&buf);
}

/* Parse and execute all complete commands in the conn recv buffer and
 * compact consumed bytes. Returns 0 ok, -1 protocol error (caller closes). */
static int conn_process_input(server *s, conn *c)
{
    size_t off = 0;
    (void)s;
    while (off < c->rlen) {
        resp_value v;
        ptrdiff_t used =
            resp_parse(c->rbuf + off, c->rlen - off, &v, &c->arena);
        if (used == 0)
            break; /* incomplete command */
        if (used < 0 || v.type != RESP_ARRAY || v.is_null)
            return -1;
        session_execute(c->sess, v.items, v.count, &c->out);
        arena_reset(&c->arena);
        off += (size_t)used;
    }
    if (off > 0) {
        memmove(c->rbuf, c->rbuf + off, c->rlen - off);
        c->rlen -= off;
    }
    return 0;
}

/* IOCP backend: post at most one overlapped send per conn; no-op while one
 * is in flight (its completion re-kicks). The chunk is copied into the
 * conn's stable send buffer, which is only (re)allocated while no send is
 * outstanding (the resp_buf out may realloc whenever replies are appended). */
static void kick_flush(server *s, conn *c)
{
    size_t n;
    if (c->send_outstanding || c->out_sent >= c->out.len)
        return;
    n = c->out.len - c->out_sent;
    if (n > IOCP_SEND_CHUNK)
        n = IOCP_SEND_CHUNK;
    if (c->scap < n) {
        size_t ncap = c->scap == 0 ? 64 * 1024 : c->scap;
        char *nb;
        while (ncap < n)
            ncap *= 2;
        nb = (char *)realloc(c->sbuf, ncap);
        if (nb == NULL)
            return; /* retried on a later pass */
        c->sbuf = nb;
        c->scap = ncap;
    }
    memcpy(c->sbuf, c->out.data + c->out_sent, n);
    if (pal_iocp_send(s->iocp, c->fd, c->sbuf, n, c) == 0) {
        c->send_outstanding = 1;
        c->pending_ops++;
    } else {
        size_t idx;
        for (idx = 0; idx < s->nconns; idx++)
            if (s->conns[idx] == c)
                break;
        if (idx < s->nconns)
            conn_close(s, idx);
    }
}

/* Accept a connection on the IOCP backend and post its first recv. */
static void server_accept_iocp(server *s, pal_socket_t fd)
{
    conn *c = conn_create(s, fd);
    if (c == NULL) {
        pal_close(fd);
        return;
    }
    if (s->nconns == s->cap) {
        size_t ncap = s->cap == 0 ? 16 : s->cap * 2;
        conn **nc = (conn **)realloc(s->conns, ncap * sizeof(*nc));
        if (nc == NULL) {
            conn_free(c);
            return;
        }
        s->conns = nc;
        s->cap = ncap;
    }
    s->conns[s->nconns++] = c;
    c->pending_ops++;
    if (pal_iocp_recv(s->iocp, c->fd, c->rbuf, c->rcap, c) != 0) {
        c->pending_ops--;
        conn_close(s, s->nconns - 1);
    }
}

static int server_run_once_iocp(server *s, int timeout_ms)
{
    pal_iocp_event evs[SERVER_MAX_EVENTS];
    int nev = pal_iocp_wait(s->iocp, evs, SERVER_MAX_EVENTS, timeout_ms);
    int i;
    if (nev <= 0)
        return nev;

    for (i = 0; i < nev; i++) {
        pal_iocp_event *ev = &evs[i];
        conn *c;
        size_t idx;

        if (ev->op == PAL_IOCP_ACCEPT) {
            server_accept_iocp(s, ev->fd);
            (void)pal_iocp_accept_post(s->iocp, s->listen_fd, NULL);
            continue;
        }

        c = (conn *)ev->userdata;
        if (c == NULL)
            continue;
        if (c->zombie) {
            /* drained op from a closed conn: free when nothing is left */
            c->pending_ops--;
            if (c->pending_ops <= 0) {
                for (idx = 0; idx < s->nzombies; idx++)
                    if (s->zombies[idx] == c)
                        break;
                if (idx < s->nzombies) {
                    s->zombies[idx] = s->zombies[s->nzombies - 1];
                    s->nzombies--;
                }
                conn_free(c);
            }
            continue;
        }
        for (idx = 0; idx < s->nconns; idx++)
            if (s->conns[idx] == c)
                break;
        if (idx == s->nconns)
            continue; /* closed earlier within this iteration */
        c->pending_ops--;

        if (ev->op == PAL_IOCP_RECV) {
            if (ev->bytes <= 0) { /* orderly close or error */
                conn_close(s, idx);
                continue;
            }
            c->rlen += (size_t)ev->bytes;
            if (conn_process_input(s, c) != 0) {
                resp_write_error(&c->out, "ERR Protocol error", 18);
                kick_flush(s, c);
                conn_close(s, idx);
                continue;
            }
            kick_flush(s, c);
            /* re-post the next recv (grow the buffer when nearly full) */
            if (c->rcap - c->rlen < SERVER_RECV_CHUNK) {
                size_t ncap = c->rcap * 2;
                char *nb = (char *)realloc(c->rbuf, ncap);
                if (nb == NULL) {
                    conn_close(s, idx);
                    continue;
                }
                c->rbuf = nb;
                c->rcap = ncap;
            }
            c->pending_ops++;
            if (pal_iocp_recv(s->iocp, c->fd, c->rbuf + c->rlen,
                              c->rcap - c->rlen, c) != 0) {
                c->pending_ops--;
                conn_close(s, idx);
                continue;
            }
        } else if (ev->op == PAL_IOCP_SEND) {
            c->send_outstanding = 0;
            if (ev->bytes < 0) {
                conn_close(s, idx);
                continue;
            }
            c->out_sent += (size_t)ev->bytes;
            if (c->out_sent < c->out.len) {
                kick_flush(s, c);
            } else {
                c->out.len = 0;
                c->out_sent = 0;
            }
        }
    }

    /* fan-out: pub/sub pushes, replication stream, SYNC frames */
    {
        size_t ci;
        for (ci = 0; ci < s->nconns; ci++) {
            conn *c = s->conns[ci];
            if (c->is_replica && c->out.len > REPL_MAX_OUTBUF) {
                conn_close(s, ci);
                ci--;
                continue;
            }
            kick_flush(s, c);
        }
    }
    if (s->aof != NULL)
        aof_flush(s->aof);
    return nev;
}

int server_run_once(server *s, int timeout_ms)
{
    pal_event evs[SERVER_MAX_EVENTS];
    int nev;
    int i;

    /* active expiration: at most one cycle per 100 ms of monotonic time */
    {
        uint64_t now = pal_now_ms();
        if (now - s->last_active_expire >= 100) {
            s->last_active_expire = now;
            db_active_expire(&s->db, pal_wall_ms(), 20);
        }
    }

    /* automatic snapshot: save interval elapsed and the db changed */
    if (s->aof == NULL && s->save_sec > 0 &&
        s->db.snapshot_path != NULL) {
        uint64_t now = pal_now_ms();
        if (now - s->last_save_check >= (uint64_t)s->save_sec * 1000) {
            s->last_save_check = now;
            if (s->db.dirty != s->dirty_at_last_save &&
                snapshot_save(&s->db, s->db.snapshot_path) == 0) {
                s->db.last_save = now / 1000;
                s->dirty_at_last_save = s->db.dirty;
            }
        }
    }

    /* replica: reconnect a dead master link (full resync every time) */
    if (s->role == SESSION_ROLE_REPLICA && s->master_link == NULL &&
        pal_now_ms() - s->last_reconnect >= 500)
        (void)repl_link_connect(s);

    /* cluster bus: gossip round, failure detection, nodes.conf persistence */
    if (s->db.cluster_enabled) {
        uint64_t now = pal_now_ms();
        if (now - s->last_gossip >= 1000) {
            s->last_gossip = now;
            cluster_gossip_round(s);
            cluster_fail_check(s, pal_wall_ms());
        }
        if (now - s->last_nodes_save >= 10000) {
            s->last_nodes_save = now;
            cluster_nodes_save(s);
        }
    }

    if (s->backend == SERVER_BACKEND_IOCP)
        return server_run_once_iocp(s, timeout_ms);

    nev = pal_loop_wait(s->loop, evs, SERVER_MAX_EVENTS, timeout_ms);
    if (nev <= 0)
        return nev;

    for (i = 0; i < nev; i++) {
        if (evs[i].fd == s->listen_fd) {
            server_accept(s, s->listen_fd, 0);
            continue;
        }
        if (evs[i].fd == s->tls_listen_fd &&
            s->tls_listen_fd != PAL_SOCKET_INVALID) {
            server_accept(s, s->tls_listen_fd, 1);
            continue;
        }
        if (evs[i].fd == s->bus_listen_fd &&
            s->bus_listen_fd != PAL_SOCKET_INVALID) {
            bus_accept(s);
            continue;
        }
        /* bus conn events are handled by the bus protocol path */
        {
            bus_conn *bc = s->bus;
            int is_bus = 0;
            while (bc != NULL) {
                if (bc->fd == evs[i].fd) {
                    is_bus = 1;
                    break;
                }
                bc = bc->next;
            }
            if (is_bus) {
                bus_service(s, bc, evs[i].writable);
                continue;
            }
        }
        /* connection readiness */
        {
            conn *c = (conn *)evs[i].userdata;
            size_t idx;
            ptrdiff_t n;

            /* locate the conn (index changes on swap-remove) */
            for (idx = 0; idx < s->nconns; idx++)
                if (s->conns[idx] == c)
                    break;
            if (idx == s->nconns)
                continue; /* already closed within this iteration */

            /* the outbound master link has its own protocol path */
            if (c->is_master_link) {
                repl_link_service(s, c);
                continue;
            }

            /* TLS handshake in progress: drive it instead of commands */
            if (c->tls_handshaking) {
                int hs = pal_tls_handshake_nb(c->tls);
                if (hs == 1) {
                    c->tls_handshaking = 0;
                    c->want_write = 0;
                    pal_loop_mod(s->loop, c->fd, 1, 0, c);
                } else if (hs < 0) {
                    conn_close(s, idx);
                }
                continue;
            }

            /* writable readiness: flush any pending output first */
            if (evs[i].writable && c->out.len > 0 &&
                conn_flush(s, c) != 0) {
                conn_close(s, idx);
                continue;
            }

            if (!evs[i].readable)
                continue; /* writable-only event: nothing to read */

            /* grow the receive buffer if a full chunk is pending */
            if (c->rcap - c->rlen < SERVER_RECV_CHUNK) {
                size_t ncap = c->rcap * 2;
                char *nb = (char *)realloc(c->rbuf, ncap);
                if (nb == NULL) {
                    conn_close(s, idx);
                    continue;
                }
                c->rbuf = nb;
                c->rcap = ncap;
            }

            n = conn_read(c, c->rbuf + c->rlen, c->rcap - c->rlen);
            if (n == 0 ||
                (n < 0 && n != -2 && !pal_would_block(pal_socket_error()))) {
                conn_close(s, idx); /* orderly close or hard error */
                continue;
            }
            if (n < 0)
                continue; /* would-block: nothing to do this round */
            c->rlen += (size_t)n;

            /* parse -> execute -> advance, then compact consumed bytes */
            if (conn_process_input(s, c) != 0) {
                static const char proto_err[] = "-ERR Protocol error\r\n";
                (void)conn_write(c, proto_err, sizeof(proto_err) - 1);
                conn_close(s, idx);
                continue;
            }
            if (c->out.len > 0 && conn_flush(s, c) != 0) {
                conn_close(s, idx);
                continue;
            }
        }
    }

    /* flush pub/sub pushes (and anything else pending) on all conns */
    {
        size_t ci;
        for (ci = 0; ci < s->nconns; ci++) {
            conn *c = s->conns[ci];
            /* drop replicas that fell too far behind: they must re-SYNC */
            if (c->is_replica && c->out.len > REPL_MAX_OUTBUF) {
                conn_close(s, ci);
                ci--;
                continue;
            }
            if (c->out.len > 0 && conn_flush(s, c) != 0) {
                conn_close(s, ci);
                ci--;
            }
        }
    }
    /* flush the AOF buffer once per loop iteration */
    if (s->aof != NULL)
        aof_flush(s->aof);
    return nev;
}
