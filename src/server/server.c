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
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/repl.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"

/* Bytes requested per pal_recv; the receive buffer grows on demand. */
#define SERVER_RECV_CHUNK (64 * 1024)
/* Readiness events consumed per server_run_once() call. */
#define SERVER_MAX_EVENTS 64
/* Replicas with more pending output than this are dropped (re-SYNC). */
#define REPL_MAX_OUTBUF (4 * 1024 * 1024)

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

struct server {
    pal_loop *loop;
    pal_socket_t listen_fd;
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
    pal_close(c->fd);
    session_free(c->sess);
    free(c->link_snap);
    free(c->rbuf);
    resp_buf_free(&c->out);
    arena_destroy(&c->arena);
    free(c);
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
    n = pal_recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
    if (n == 0 || (n < 0 && !pal_would_block(pal_socket_error()))) {
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

server *server_create(const char *host, uint16_t port)
{
    server *s = (server *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->listen_fd = PAL_SOCKET_INVALID;
    s->loop = pal_loop_create();
    s->listen_fd = pal_tcp_listen(host, port, 128, &s->port);
    if (s->loop == NULL || s->listen_fd == PAL_SOCKET_INVALID) {
        server_destroy(s);
        return NULL;
    }
    db_init(&s->db);
    rh_init(&s->channels);
    s->role = SESSION_ROLE_MASTER;
    repl_backlog_init(&s->backlog, 1024 * 1024);
    resp_buf_init(&s->prop_buf);
    memset(&s->repl, 0, sizeof(s->repl));
    s->repl.role = SESSION_ROLE_MASTER;
    if (pal_loop_add(s->loop, s->listen_fd, 1, 0, NULL) != 0) {
        server_destroy(s);
        return NULL;
    }
    return s;
}

uint16_t server_port(const server *s)
{
    return s->port;
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
    for (i = 0; i < s->nconns; i++)
        conn_free(s->conns[i]);
    free(s->conns);
    if (s->loop != NULL && s->listen_fd != PAL_SOCKET_INVALID)
        pal_loop_del(s->loop, s->listen_fd);
    pal_close(s->listen_fd);
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

static void server_accept(server *s)
{
    /* The listener socket is blocking, but a readiness event guarantees at
     * least one pending connection, so this accept does not block. */
    pal_socket_t fd = pal_accept(s->listen_fd);
    conn *c;
    if (fd == PAL_SOCKET_INVALID)
        return;
    c = conn_create(s, fd);
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
    if (pal_loop_add(s->loop, fd, 1, 0, c) != 0)
        conn_close(s, s->nconns - 1);
}

static void conn_close(server *s, size_t idx)
{
    conn *c = s->conns[idx];
    if (c->is_replica && s->repl.connected_slaves > 0)
        s->repl.connected_slaves--;
    pal_loop_del(s->loop, c->fd);
    conn_free(c);
    s->conns[idx] = s->conns[s->nconns - 1];
    s->nconns--;
}

/* Flush conn->out with a blocking send loop (see file header). */
static int conn_flush(conn *c)
{
    size_t sent = 0;
    while (sent < c->out.len) {
        ptrdiff_t n = pal_send(c->fd, c->out.data + sent, c->out.len - sent);
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    c->out.len = 0;
    return 0;
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

    nev = pal_loop_wait(s->loop, evs, SERVER_MAX_EVENTS, timeout_ms);
    if (nev <= 0)
        return nev;

    for (i = 0; i < nev; i++) {
        if (evs[i].fd == s->listen_fd) {
            server_accept(s);
            continue;
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

            n = pal_recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen);
            if (n == 0 || (n < 0 && !pal_would_block(pal_socket_error()))) {
                conn_close(s, idx); /* orderly close or hard error */
                continue;
            }
            if (n < 0)
                continue; /* would-block: nothing to do this round */
            c->rlen += (size_t)n;

            /* parse -> execute -> advance, then compact consumed bytes */
            {
                size_t off = 0;
                int protocol_error = 0;
                while (off < c->rlen) {
                    resp_value v;
                    ptrdiff_t used = resp_parse(c->rbuf + off, c->rlen - off,
                                                &v, &c->arena);
                    if (used == 0)
                        break; /* incomplete command */
                    if (used < 0 || v.type != RESP_ARRAY || v.is_null) {
                        protocol_error = 1;
                        break;
                    }
                    session_execute(c->sess, v.items, v.count, &c->out);
                    arena_reset(&c->arena);
                    off += (size_t)used;
                }
                if (protocol_error) {
                    static const char proto_err[] = "-ERR Protocol error\r\n";
                    (void)pal_send(c->fd, proto_err, sizeof(proto_err) - 1);
                    conn_close(s, idx);
                    continue;
                }
                if (off > 0) {
                    memmove(c->rbuf, c->rbuf + off, c->rlen - off);
                    c->rlen -= off;
                }
                if (c->out.len > 0 && conn_flush(c) != 0) {
                    conn_close(s, idx);
                    continue;
                }
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
            if (c->out.len > 0 && conn_flush(c) != 0) {
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
