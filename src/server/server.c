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
#include "pal/pal_event.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"

/* Bytes requested per pal_recv; the receive buffer grows on demand. */
#define SERVER_RECV_CHUNK (64 * 1024)
/* Readiness events consumed per server_run_once() call. */
#define SERVER_MAX_EVENTS 64

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
};

struct server {
    pal_loop *loop;
    pal_socket_t listen_fd;
    db db;
    rh_table channels; /* pub/sub: channel -> chan_node list head (8-byte ptr) */
    conn **conns;
    size_t nconns;
    size_t cap;
    uint16_t port;
    uint64_t last_active_expire; /* pal_now_ms of the last active cycle */
};

static void conn_close(server *s, size_t idx);

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
    free(c->rbuf);
    resp_buf_free(&c->out);
    arena_destroy(&c->arena);
    free(c);
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
            if (c->out.len > 0 && conn_flush(c) != 0) {
                conn_close(s, ci);
                ci--;
            }
        }
    }
    return nev;
}
