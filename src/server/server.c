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
#include <limits.h>

#include "core/arena.h"
#include "core/buf_pool.h"
#include "core/command.h"
#include "core/hashslot.h"
#include "core/redbus.h"
#include "core/session.h"
#include "core/snapshot.h"
#include "core/tier.h"
#include "ds/glob.h"
#include "server/aof.h"
#include "pal/pal_cstd.h"
#include "pal/pal_event.h"
#include "pal/pal_iocp.h"
#include "pal/pal_iouring_op.h"
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "pal/pal_tls.h"
#include "server/repl.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"

/* Bytes requested per pal_recv; the receive buffer grows on demand. */
#define SERVER_RECV_CHUNK (64 * 1024)
#define SERVER_DEFAULT_MAX_REQUEST (1024ULL * 1024ULL * 1024ULL)
/* Readiness events consumed per server_run_once() call. */
#define SERVER_MAX_EVENTS 64
/* Replicas with more pending output than this are dropped (re-SYNC). */
#define REPL_MAX_OUTBUF (16 * 1024 * 1024)

/* IOCP backend: single send chunk cap per overlapped WSASend. */
#define IOCP_SEND_CHUNK (256 * 1024)
#define IOU_SBUF_COUNT 64

/* io_uring op-mode multishot recv (Phase 33): provided-buffer ring of
 * 256 x 64KB slabs shared by all conns of this server. 64KB slots match
 * the repost model's SERVER_RECV_CHUNK reads: smaller slots fragment
 * each burst into several CQEs (Phase 32a batching lesson). */
#define IOU_PBUF_COUNT 256
#define IOU_PBUF_SIZE (64 * 1024)

/* ENOBUFS is POSIX-only; the ms-recv starvation check must still compile
 * on Windows (where iou_ms is always 0). */
#ifndef ENOBUFS
#define ENOBUFS 119
#endif

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
    int tls_client;       /* outbound client-side TLS handshake */
    uint64_t id;        /* CLIENT ID / CLIENT LIST */
    char name[64];      /* CLIENT SETNAME / GETNAME */
    size_t name_len;
    int want_write;      /* registered for write readiness (out pending) */
    /* IOCP backend state */
    int pending_ops;     /* outstanding overlapped ops (recv+send) */
    int zombie;          /* closed with ops in flight: freed at 0 pending */
    int zombie_mt_free;  /* zombie whose owner (mt layer) released it */
    int send_outstanding;
    int send_fixed_id;          /* registered io_uring SEND_ZC slot */
    int send_zc_notif_pending;  /* buffer remains pinned until notification */
    int close_after_send; /* QUIT / protocol error: reply reaches the peer
                             before the connection is closed */
    /* detached send buffer: out's allocation moves here at kick_flush time
     * (zero-copy handoff, Phase 34); owned until fully sent, then returned
     * to the pool */
    char *sbuf;
    size_t sbuf_len;       /* valid bytes in sbuf */
    size_t sbuf_sent;      /* bytes of sbuf confirmed sent */
    size_t sbuf_pool_size; /* pool allocation size, 0 = malloc'd */
    char *rbuf;  /* receive buffer, pool or malloc'd, compacted after parsing */
    size_t rlen; /* valid bytes in rbuf */
    size_t rcap; /* allocated size of rbuf */
    size_t rbuf_pool_size; /* actual allocation size when borrowed from pool */
    resp_buf out;
    arena arena;
    session *sess; /* per-connection command context (MULTI/WATCH/pubsub) */
    conn_sub *subs; /* channels this conn is subscribed to */
    conn_sub *ssubs; /* shard channels (Redis 7 sharded pub/sub) */
    conn_sub *psubs; /* patterns this conn is subscribed to (PSUBSCRIBE) */
    struct server *srv;
    int is_replica;     /* downstream replica (we are the master) */
    int is_master_link; /* our outbound link to the master (we are replica) */
    /* mt_server routing state (owned by the routing layer: seq, reorder
     * buffer, pending count and closing flag all live there) */
    void *mt_state;
    /* master-link receive state ($<len> snapshot frame, then RESP stream) */
    int link_state;
    size_t link_hdrlen;
    size_t link_need;
    size_t link_got;
    char *link_snap;
    char pending_replid[41];
    uint64_t pending_offset;
};

static int srv_monitor_start(void *ctx, session *sess);
static void srv_monitor_emit(server *srv, const conn *source,
                             const resp_value *argv, size_t argc);
static void srv_monitor_emit_session(void *ctx, session *source,
                                     const resp_value *argv, size_t argc);
static int server_token_eq(const char *s, size_t len, const char *word);
static int server_parse_ll(const resp_value *v, long long *out);
static int srv_backup_command(void *ctx, const resp_value *argv, size_t argc,
                              resp_buf *out);
static void srv_hotkeys_record(server *srv, const session *source,
                               const resp_value *argv, size_t argc);

/* master link states */
#define LINK_SYNC_SENT 0
#define LINK_SNAPSHOT 1
#define LINK_STREAMING 2
#define LINK_SNAP_HDR 3 /* PSYNC FULLRESYNC: snapshot frame header next */
#define LINK_CONNECTING 4 /* async connect in flight (readiness backend) */
#define LINK_TLS_HANDSHAKE 5 /* TCP connected, outbound TLS in progress */

/* SLOWLOG ring: newest entries are appended at the tail and read backwards. */
typedef struct slowlog_entry {
    uint64_t id;
    uint64_t ts;
    uint64_t usec;
    resp_value *argv;
    size_t argc;
} slowlog_entry;

/* cluster bus connection (cluster bus protocol v1, server side) */
typedef struct bus_conn {
    struct bus_conn *next;
    pal_socket_t fd;
    int outbound;      /* we initiated it (MEET / gossip target) */
    char peer_ip[64];  /* peer address for redbus myip auto-discovery */
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
    pal_tls_ctx *repl_tls_ctx;
    int repl_tls;
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
    uint64_t failover_deadline_ms; /* wall ms; 0 = no pending failover */
    int bus_protocol;            /* SERVER_BUS_PROTOCOL_* (default DDUP) */
    int cluster_control;         /* run gossip/failover/nodes.conf save */
    db db;            /* db 0 (also holds the cluster state) */
    db *extra_dbs;    /* dbs 1..ndbs-1 */
    int ndbs;         /* total logical databases (default 16) */
    struct tier_store *tier; /* optional shared cold layer (NULL = off) */
    rh_table channels; /* pub/sub: channel -> chan_node list head (8-byte ptr) */
    rh_table schannels; /* shard channels (Redis 7 sharded pub/sub) */
    rh_table patterns;  /* pattern pub/sub: pattern -> chan_node list head */
    size_t numpats;     /* total (conn, pattern) subscriptions (PUBSUB NUMPAT) */
    aof *aof;          /* NULL when appendonly=no */
    int aof_failed;    /* flush failed: reject writes and stop AOF growth */
    int aof_db_index;  /* last db index written to the AOF (SELECT prefix) */
    int aof_fsync_mode; /* appendfsync policy (AOF_FSYNC_*) */
    const char *requirepass; /* AUTH password (not owned); NULL/"" = off */
    uint64_t next_client_id;    /* monotonic CLIENT ID allocator */
    uint64_t client_id_stride;  /* mt allocator stride (1 for standalone) */
    uint64_t slowlog_threshold_us; /* SLOWLOG log-slower-than (0 = all) */
    size_t slowlog_max;         /* SLOWLOG max retained entries */
    slowlog_entry *slowlog;     /* SLOWLOG newest-last ring */
    size_t slowlog_len;
    size_t slowlog_cap;
    uint64_t slowlog_next_id;
    int hotkeys_active;
    int hotkeys_initialized;
    uint64_t hotkeys_start_ms;
    uint64_t hotkeys_commands;
    uint64_t hotkeys_duration_ms;
    uint64_t hotkeys_sample_ratio;
    int hotkeys_metric_cpu;
    int hotkeys_metric_net;
    uint64_t hotkeys_sampled;
    size_t hotkeys_capacity;
    struct hotkey_entry *hotkeys_entries;
    int hotkeys_slot_filter;
    uint8_t hotkeys_slots[16384];
    int backup_state;
    char *backup_path;
    char *backup_aof_path;
    uint64_t backup_aof_offset;
    uint64_t backup_start_ms;
    uint64_t backup_end_ms;
    char backup_error[128];
    int shutdown_flag;
    int save_sec;               /* automatic snapshot interval, 0 = off */
    uint64_t last_save_check;   /* pal_now_ms of the last interval check */
    uint64_t dirty_at_last_save;
    /* replication */
    int role;             /* SESSION_ROLE_* */
    repl_backlog backlog; /* propagated command stream (master side) */
    buf_pool pool;        /* per-server buffer pool for conn rbuf/out */
    resp_buf prop_buf;    /* reusable propagation serialization buffer */
    repl_info repl;       /* INFO replication snapshot */
    int backlog_used;     /* a replica attached at least once (Phase 28) */
    int repl_centralized; /* mt: worker-local sessions skip repl fan-out */
    int repl_stream_db;   /* last db index written to the central stream */
    server_repl_stream_fn repl_stream_fn; /* mt central coordinator */
    void *repl_stream_ctx;
    io_counters io;       /* always-on IO counters (Phase 27; calloc-zeroed) */
    conn *master_link;    /* outbound link to the master (replica side) */
    uint64_t last_reconnect; /* pal_now_ms of the last connect attempt */
    size_t proto_max_request_bytes;
    size_t repl_max_snapshot_bytes;
    server_repl_snapshot_fn repl_snapshot_fn; /* mt replica full-sync */
    void *repl_snapshot_ctx;
    server_repl_snapshot_serialize_fn repl_snapshot_serialize_fn;
    void *repl_snapshot_serialize_ctx;
    /* proactor backend (Windows IOCP / Linux io_uring op-mode) */
    int backend;
    pal_iocp *iocp;
    pal_iouring *iou;
    int iou_ms; /* io_uring op-mode: multishot recv + pbuf ring active */
    int iou_send_zc; /* optional registered-buffer SEND_ZC path */
    conn **zombies; /* conns closed with ops in flight, freed when drained */
    size_t nzombies;
    size_t zombie_cap;
    conn **conns;
    size_t nconns;
    size_t cap;
    uint16_t port;
    uint64_t last_active_expire; /* pal_now_ms of the last active cycle */
    /* mt_server wakeup fd (readiness backend only) */
    pal_socket_t wakeup_fd;
    void (*wakeup_cb)(void *ctx);
    void *wakeup_ctx;
    /* mt_server routing hooks */
    server_route_fn route_fn;
    server_route_flush_fn route_flush_fn;
    void (*mt_state_free)(void *ctx, void *st);
    server_mt_close_fn mt_close_fn;
    void *route_ctx;
};

typedef struct hotkey_entry {
    char key[256];
    size_t len;
    uint64_t cpu_us;
    uint64_t net_bytes;
} hotkey_entry;

static void conn_close(server *s, size_t idx);
static int conn_flush(server *s, conn *c);
static int conn_process_input(server *s, conn *c);
static void server_process_blocked(server *s);
static conn *conn_create(server *srv, pal_socket_t fd);
static db *srv_select_db(void *ctx, int idx);
static void srv_aof_log(server *srv, int db_index, const resp_value *argv,
                        size_t argc);
static long long srv_client_id(void *ctx, struct session *s);
static int srv_client_setname(void *ctx, struct session *s,
                              const char *name, size_t len);
static const char *srv_client_getname(void *ctx, struct session *s,
                                      size_t *len);
static void srv_client_list(void *ctx, resp_buf *out);
static int srv_client_kill(void *ctx, const char *filter, size_t filterlen,
                           resp_buf *out);
static void srv_slowlog_add(void *ctx, const resp_value *argv, size_t argc,
                            uint64_t usec, uint64_t now_ms);
static size_t srv_slowlog_len(void *ctx);
static void srv_slowlog_get(void *ctx, long long count, resp_buf *out);
static void srv_slowlog_reset(void *ctx);
static void srv_bgrewriteaof(void *ctx, resp_buf *out);
static int srv_aof_reject(const server *srv, const session *sess,
                          const resp_value *argv, size_t argc, resp_buf *out);
static void srv_aof_flush(server *srv);
static int srv_psync(void *ctx, session *sess, const char *replid,
                     size_t replid_len, long long offset);
static void repl_link_close(server *srv);
static int repl_link_connect(server *srv);
static int server_run_once_proactor(server *s, int timeout_ms);
static void kick_flush(server *s, conn *c);
static void server_accept_pro(server *s, pal_socket_t fd);
static void bus_conn_free(server *s, bus_conn *bc);
static void cluster_nodes_save(server *s);
static int srv_cluster_meet(void *ctx, const char *ip, uint16_t port);
static void srv_spublish_bus(void *ctx, const char *ch, size_t chlen,
                             const char *msg, size_t mlen);
static void cluster_broadcast_fail(server *s);

/* ------------------------------------------------------------------ */
/* proactor dispatch (Windows IOCP / Linux io_uring op-mode share one  */
/* server code path; op kinds carry identical values in both pals)     */
/* ------------------------------------------------------------------ */

ddup_static_assert((int)PAL_IOCP_ACCEPT == (int)PAL_IOURING_ACCEPT &&
                       (int)PAL_IOCP_RECV == (int)PAL_IOURING_RECV &&
                       (int)PAL_IOCP_SEND == (int)PAL_IOURING_SEND &&
                       (int)PAL_IOCP_WAKEUP == (int)PAL_IOURING_WAKEUP,
                   "proactor op kinds must mirror each other");

static int srv_proactor(const server *s)
{
    return s->backend == SERVER_BACKEND_IOCP ||
           s->backend == SERVER_BACKEND_IOURING_OP;
}

static int pro_accept_post(server *s, pal_socket_t listen_fd, void *ud)
{
    if (s->backend == SERVER_BACKEND_IOCP)
        return pal_iocp_accept_post(s->iocp, listen_fd, ud);
    return pal_iouring_accept_post(s->iou, listen_fd, ud);
}

static int pro_recv(server *s, pal_socket_t fd, void *buf, size_t cap,
                    void *ud)
{
    if (s->backend == SERVER_BACKEND_IOCP)
        return pal_iocp_recv(s->iocp, fd, buf, cap, ud);
    return pal_iouring_recv(s->iou, fd, buf, cap, ud);
}

static int pro_send(server *s, pal_socket_t fd, const void *buf, size_t n,
                    void *ud)
{
    if (s->backend == SERVER_BACKEND_IOCP)
        return pal_iocp_send(s->iocp, fd, buf, n, ud);
    return pal_iouring_send(s->iou, fd, buf, n, ud);
}

static void pro_close(server *s, pal_socket_t fd)
{
    if (s->backend == SERVER_BACKEND_IOCP)
        pal_iocp_close(s->iocp, fd);
    else
        pal_iouring_close(s->iou, fd);
}

static int pro_kick(server *s)
{
    if (s->backend == SERVER_BACKEND_IOCP)
        return pal_iocp_post(s->iocp, NULL);
    return pal_iouring_post(s->iou, NULL);
}

/* ------------------------------------------------------------------ */
/* pub/sub registry + session hooks                                   */
/* ------------------------------------------------------------------ */

static chan_node *chan_get(rh_table *tab, const char *ch, size_t len)
{
    const char *v;
    size_t vl;
    chan_node *head = NULL;
    if (rh_get(tab, ch, len, &v, &vl) && vl == 8)
        memcpy(&head, v, 8);
    return head;
}

static int chan_put(rh_table *tab, const char *ch, size_t len,
                    chan_node *head)
{
    char b[8];
    if (head == NULL) {
        rh_del(tab, ch, len);
        return 0;
    }
    memcpy(b, &head, 8);
    return rh_set(tab, ch, len, b, 8) < 0 ? -1 : 0;
}

/* shared subscribe/unsubscribe body over (registry table, per-conn list,
 * session counter) triples */
static ptrdiff_t chan_subscribe(rh_table *tab, conn_sub **listp, session *sess,
                                const char *ch, size_t len, size_t *counter)
{
    conn *c = (conn *)sess->owner;
    chan_node *head;
    chan_node *n;
    conn_sub *cs;
    if (len > UINT32_MAX)
        return -1;
    head = chan_get(tab, ch, len);
    for (n = head; n != NULL; n = n->next)
        if (n->c == c)
            return *counter; /* already subscribed */
    n = (chan_node *)malloc(sizeof(*n));
    if (n == NULL)
        return -1;
    cs = (conn_sub *)malloc(sizeof(*cs));
    if (cs == NULL) {
        free(n);
        return -1;
    }
    cs->ch = (char *)malloc(len > 0 ? len : 1);
    if (cs->ch == NULL) {
        free(cs);
        free(n);
        return -1;
    }
    n->c = c;
    n->next = head;
    if (chan_put(tab, ch, len, n) != 0) {
        free(cs->ch);
        free(cs);
        free(n);
        return -1;
    }
    memcpy(cs->ch, ch, len);
    cs->chlen = len;
    cs->next = *listp;
    *listp = cs;
    (*counter)++;
    return *counter;
}

static size_t chan_unsubscribe(rh_table *tab, conn_sub **listp, session *sess,
                               const char *ch, size_t len, size_t *counter)
{
    conn *c = (conn *)sess->owner;
    chan_node *head = chan_get(tab, ch, len);
    chan_node **pp = &head;
    conn_sub **csp;
    while (*pp != NULL && (*pp)->c != c)
        pp = &(*pp)->next;
    if (*pp == NULL)
        return *counter; /* not subscribed */
    {
        chan_node *dead = *pp;
        *pp = dead->next;
        if (chan_put(tab, ch, len, head) != 0) {
            *pp = dead;
            return *counter;
        }
        free(dead);
    }
    for (csp = listp; *csp != NULL; csp = &(*csp)->next) {
        if ((*csp)->chlen == len && memcmp((*csp)->ch, ch, len) == 0) {
            conn_sub *dead = *csp;
            *csp = dead->next;
            free(dead->ch);
            free(dead);
            break;
        }
    }
    if (*counter > 0)
        (*counter)--;
    return *counter;
}

static ptrdiff_t srv_subscribe(void *ctx, session *sess, const char *ch,
                               size_t len)
{
    server *srv = (server *)ctx;
    return chan_subscribe(&srv->channels, &((conn *)sess->owner)->subs,
                          sess, ch, len, &sess->nsub);
}

static size_t srv_unsubscribe(void *ctx, session *sess, const char *ch,
                              size_t len)
{
    server *srv = (server *)ctx;
    return chan_unsubscribe(&srv->channels, &((conn *)sess->owner)->subs,
                            sess, ch, len, &sess->nsub);
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

/* ------------------------------------------------------------------ */
/* pattern pub/sub hooks (Redis PSUBSCRIBE)                            */
/* ------------------------------------------------------------------ */

static ptrdiff_t srv_psubscribe(void *ctx, session *sess, const char *pat,
                                size_t len)
{
    server *srv = (server *)ctx;
    size_t before = sess->npsub;
    ptrdiff_t rc = chan_subscribe(&srv->patterns,
                                  &((conn *)sess->owner)->psubs, sess, pat,
                                  len, &sess->npsub);
    if (rc > (ptrdiff_t)before)
        srv->numpats++;
    return rc;
}

static size_t srv_punsubscribe(void *ctx, session *sess, const char *pat,
                               size_t len)
{
    server *srv = (server *)ctx;
    size_t before = sess->npsub;
    size_t rc = chan_unsubscribe(&srv->patterns,
                                 &((conn *)sess->owner)->psubs, sess, pat,
                                 len, &sess->npsub);
    if (rc < before)
        srv->numpats--;
    return rc;
}

static void srv_each_pattern(void *ctx, session *sess,
                             void (*cb)(const char *pat, size_t len,
                                        void *arg),
                             void *arg)
{
    conn *c = (conn *)sess->owner;
    conn_sub *cs;
    (void)ctx;
    for (cs = c->psubs; cs != NULL; cs = cs->next)
        cb(cs->ch, cs->chlen, arg);
}

static void srv_deliver_pattern(void *owner, const char *pat, size_t patlen,
                                const char *ch, size_t chlen, const char *msg,
                                size_t mlen)
{
    conn *c = (conn *)owner;
    resp_write_array_header(&c->out, 4);
    resp_write_bulk(&c->out, "pmessage", 8);
    resp_write_bulk(&c->out, pat, patlen);
    resp_write_bulk(&c->out, ch, chlen);
    resp_write_bulk(&c->out, msg, mlen);
}

/* PUBLISH fan-out over the pattern registry (see srv_publish). */
typedef struct pat_pub_ctx {
    const char *ch;
    size_t chlen;
    const char *msg;
    size_t mlen;
    long count;
} pat_pub_ctx;

static void pat_pub_cb(const char *pat, size_t plen, const char *v,
                       size_t vlen, void *arg)
{
    pat_pub_ctx *pp = (pat_pub_ctx *)arg;
    chan_node *head;
    chan_node *n;
    (void)vlen;
    if (!ddup_glob_match(pat, plen, pp->ch, pp->chlen))
        return;
    memcpy(&head, v, 8);
    for (n = head; n != NULL; n = n->next) {
        conn *sc = n->c;
        if (sc->sess->deliver_pattern != NULL)
            sc->sess->deliver_pattern(sc->sess->owner, pat, plen, pp->ch,
                                      pp->chlen, pp->msg, pp->mlen);
        pp->count++;
    }
}

static long srv_numpat(void *ctx)
{
    server *srv = (server *)ctx;
    return (long)srv->numpats;
}

static long srv_channel_nsub(void *ctx, const char *ch, size_t len)
{
    server *srv = (server *)ctx;
    chan_node *n = chan_get(&srv->channels, ch, len);
    long count = 0;
    for (; n != NULL; n = n->next)
        count++;
    return count;
}

/* PUBSUB CHANNELS: two-pass list over the regular channel registry with
 * optional glob filtering (full ddup_glob_match semantics). */
typedef struct chan_list_ctx {
    resp_buf *out;
    const char *pat;
    size_t patlen;
    size_t n;
    int write; /* 0 = count pass, 1 = write pass */
} chan_list_ctx;

static void pubsub_chan_cb(const char *ch, size_t len, const char *v,
                           size_t vlen, void *arg)
{
    chan_list_ctx *lc = (chan_list_ctx *)arg;
    (void)v;
    (void)vlen;
    if (lc->pat != NULL && !ddup_glob_match(lc->pat, lc->patlen, ch, len))
        return;
    if (lc->write)
        resp_write_bulk(lc->out, ch, len);
    lc->n++;
}

static size_t srv_pubsub_channels(void *ctx, const char *pat, size_t patlen,
                                  resp_buf *out)
{
    server *srv = (server *)ctx;
    chan_list_ctx lc;
    lc.out = out;
    lc.pat = patlen ? pat : NULL;
    lc.patlen = patlen;
    lc.n = 0;
    lc.write = 0;
    rh_each(&srv->channels, pubsub_chan_cb, &lc);
    resp_write_array_header(out, lc.n);
    lc.n = 0;
    lc.write = 1;
    rh_each(&srv->channels, pubsub_chan_cb, &lc);
    return lc.n;
}

static long srv_publish(void *ctx, const char *ch, size_t chlen,
                        const char *msg, size_t mlen)
{
    server *srv = (server *)ctx;
    chan_node *n = chan_get(&srv->channels, ch, chlen);
    long count = 0;
    for (; n != NULL; n = n->next) {
        conn *sc = n->c;
        if (sc->sess->deliver != NULL)
            sc->sess->deliver(sc->sess->owner, ch, chlen, msg, mlen);
        count++;
    }
    /* pattern subscribers: one "pmessage" per matching (conn, pattern)
     * pair. The pattern set is small in practice, so a linear scan with
     * glob matching per pattern is acceptable (documented). */
    if (rh_size(&srv->patterns) > 0) {
        pat_pub_ctx pp;
        pp.ch = ch;
        pp.chlen = chlen;
        pp.msg = msg;
        pp.mlen = mlen;
        pp.count = 0;
        rh_each(&srv->patterns, pat_pub_cb, &pp);
        count += pp.count;
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

/* ------------------------------------------------------------------ */
/* shard channel hooks (Redis 7 sharded pub/sub)                       */
/* ------------------------------------------------------------------ */

static ptrdiff_t srv_ssubscribe(void *ctx, session *sess, const char *ch,
                                size_t len)
{
    server *srv = (server *)ctx;
    return chan_subscribe(&srv->schannels, &((conn *)sess->owner)->ssubs,
                          sess, ch, len, &sess->nssub);
}

int server_test_pubsub_rejected_subscribe(void)
{
#if SIZE_MAX > UINT32_MAX
    rh_table tab;
    conn c;
    session sess;
    conn_sub *list = NULL;
    const char byte = 'x';
    size_t count = 0;
    ptrdiff_t rc;

    memset(&c, 0, sizeof(c));
    session_init(&sess, NULL);
    sess.owner = &c;
    rh_init(&tab);
    rc = chan_subscribe(&tab, &list, &sess, &byte,
                        (size_t)UINT32_MAX + 1, &count);
    if (rc != -1 || list != NULL || count != 0 || rh_size(&tab) != 0) {
        rh_destroy(&tab);
        return -1;
    }
    rh_destroy(&tab);
#endif
    return 0;
}

static size_t srv_sunsubscribe(void *ctx, session *sess, const char *ch,
                               size_t len)
{
    server *srv = (server *)ctx;
    return chan_unsubscribe(&srv->schannels, &((conn *)sess->owner)->ssubs,
                            sess, ch, len, &sess->nssub);
}

static void srv_reset(void *ctx, session *sess)
{
    server *srv = (server *)ctx;
    conn *c = (conn *)sess->owner;
    while (c->subs != NULL)
        srv_unsubscribe(srv, sess, c->subs->ch, c->subs->chlen);
    while (c->ssubs != NULL)
        srv_sunsubscribe(srv, sess, c->ssubs->ch, c->ssubs->chlen);
    while (c->psubs != NULL)
        srv_punsubscribe(srv, sess, c->psubs->ch, c->psubs->chlen);
}

static void srv_each_schannel(void *ctx, session *sess,
                              void (*cb)(const char *ch, size_t len,
                                         void *arg),
                              void *arg)
{
    conn *c = (conn *)sess->owner;
    conn_sub *cs;
    (void)ctx;
    for (cs = c->ssubs; cs != NULL; cs = cs->next)
        cb(cs->ch, cs->chlen, arg);
}

static long srv_spublish(void *ctx, const char *ch, size_t chlen,
                         const char *msg, size_t mlen)
{
    server *srv = (server *)ctx;
    chan_node *n = chan_get(&srv->schannels, ch, chlen);
    long count = 0;
    for (; n != NULL; n = n->next) {
        conn *sc = n->c;
        if (sc->sess->deliver_shard != NULL)
            sc->sess->deliver_shard(sc->sess->owner, ch, chlen, msg, mlen);
        count++;
    }
    return count;
}

static long srv_schannel_nsub(void *ctx, const char *ch, size_t len)
{
    server *srv = (server *)ctx;
    chan_node *n = chan_get(&srv->schannels, ch, len);
    long count = 0;
    for (; n != NULL; n = n->next)
        count++;
    return count;
}

/* minimal glob: * and ? only (documented; no [] classes) */
static int shard_pat_match(const char *pat, size_t plen, const char *s,
                           size_t slen)
{
    size_t p = 0, i = 0, star = (size_t)-1, mark = 0;
    while (i < slen) {
        if (p < plen && (pat[p] == '?' || pat[p] == s[i])) {
            p++;
            i++;
        } else if (p < plen && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != (size_t)-1) {
            p = star + 1;
            i = ++mark;
        } else {
            return 0;
        }
    }
    while (p < plen && pat[p] == '*')
        p++;
    return p == plen;
}

typedef struct shard_list_ctx {
    resp_buf *out;
    const char *pat;
    size_t patlen;
    size_t n;
    int write; /* 0 = count pass, 1 = write pass */
} shard_list_ctx;

static void shard_chan_cb(const char *ch, size_t len, const char *v,
                          size_t vlen, void *arg)
{
    shard_list_ctx *lc = (shard_list_ctx *)arg;
    (void)v;
    (void)vlen;
    if (lc->pat != NULL && !shard_pat_match(lc->pat, lc->patlen, ch, len))
        return;
    if (lc->write)
        resp_write_bulk(lc->out, ch, len);
    lc->n++;
}

static size_t srv_shard_channels(void *ctx, const char *pat, size_t patlen,
                                 resp_buf *out)
{
    server *srv = (server *)ctx;
    shard_list_ctx lc;
    lc.out = out;
    lc.pat = patlen ? pat : NULL;
    lc.patlen = patlen;
    lc.n = 0;
    lc.write = 0;
    rh_each(&srv->schannels, shard_chan_cb, &lc);
    resp_write_array_header(out, lc.n);
    lc.n = 0;
    lc.write = 1;
    rh_each(&srv->schannels, shard_chan_cb, &lc);
    return lc.n;
}

static void srv_deliver_shard(void *owner, const char *ch, size_t chlen,
                              const char *msg, size_t mlen)
{
    conn *c = (conn *)owner;
    resp_write_array_header(&c->out, 3);
    resp_write_bulk(&c->out, "smessage", 8);
    resp_write_bulk(&c->out, ch, chlen);
    resp_write_bulk(&c->out, msg, mlen);
}

static int srv_monitor_start(void *ctx, session *sess)
{
    (void)ctx;
    if (sess == NULL || sess->monitor_enabled)
        return sess != NULL ? 0 : -1;
    sess->monitor_enabled = 1;
    return 0;
}

static void monitor_append_quoted(resp_buf *out, const char *s, size_t len)
{
    size_t i;
    if (resp_buf_reserve(out, len * 2 + 2) != 0)
        return;
    out->data[out->len++] = '"';
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            out->data[out->len++] = '\\';
        out->data[out->len++] = (c >= 0x20 && c != 0x7f) ? (char)c : '?';
    }
    out->data[out->len++] = '"';
}

static void srv_monitor_emit(server *srv, const conn *source,
                             const resp_value *argv, size_t argc)
{
    size_t i, j;
    uint64_t now = pal_wall_ms();
    for (i = 0; i < srv->nconns; i++) {
        conn *c = srv->conns[i];
        char head[96];
        int n;
        if (c == source || c->sess == NULL || !c->sess->monitor_enabled)
            continue;
        n = snprintf(head, sizeof(head), "%llu [0 0.0.0.0] ",
                     (unsigned long long)now);
        if (n < 0 || resp_buf_reserve(&c->out, (size_t)n + 3) != 0)
            continue;
        memcpy(c->out.data + c->out.len, head, (size_t)n);
        c->out.len += (size_t)n;
        for (j = 0; j < argc; j++) {
            if (j != 0)
                c->out.data[c->out.len++] = ' ';
            monitor_append_quoted(&c->out, argv[j].str, argv[j].len);
        }
        c->out.data[c->out.len++] = '\r';
        c->out.data[c->out.len++] = '\n';
    }
}

static void srv_monitor_emit_session(void *ctx, session *source,
                                     const resp_value *argv, size_t argc)
{
    conn *origin = source != NULL ? (conn *)source->owner : NULL;
    server *srv = (server *)ctx;
    if (srv->hotkeys_active && argc > 0 && argv[0].str != NULL &&
        !server_token_eq(argv[0].str, argv[0].len, "HOTKEYS")) {
        if (srv->hotkeys_duration_ms != 0 &&
            pal_wall_ms() - srv->hotkeys_start_ms >= srv->hotkeys_duration_ms)
            srv->hotkeys_active = 0;
    }
    if (srv->hotkeys_active && argc > 0 && argv[0].str != NULL &&
        !server_token_eq(argv[0].str, argv[0].len, "HOTKEYS"))
        srv->hotkeys_commands++;
    if (srv->hotkeys_active)
        srv_hotkeys_record(srv, source, argv, argc);
    srv_monitor_emit(srv, origin, argv, argc);
}

static void srv_hotkeys_record_key(server *srv, const resp_value *argv,
                                   size_t argc, const session *source,
                                   const char *key, size_t klen)
{
    size_t i, slot = SIZE_MAX;
    uint64_t net = source != NULL ? source->last_cmd_net_bytes : 0;
    hotkey_entry *e;
    uint16_t cmd;
    (void)argc;
    if (key == NULL || klen == 0 ||
        klen >= sizeof(srv->hotkeys_entries[0].key))
        return;
    if (srv->hotkeys_slot_filter &&
        !srv->hotkeys_slots[hash_slot(key, klen)])
        return;
    cmd = cmd_resolve(argv[0].str, argv[0].len);
    if (cmd == CMD_PING || cmd == CMD_ECHO || cmd == CMD_AUTH ||
        cmd == CMD_SELECT || cmd == CMD_HELLO || cmd == CMD_COMMAND ||
        cmd == CMD_CLIENT || cmd == CMD_CONFIG || cmd == CMD_MEMORY ||
        cmd == CMD_SLOWLOG || cmd == CMD_INFO || cmd == CMD_TIME ||
        cmd == CMD_ROLE || cmd == CMD_LATENCY || cmd == CMD_SENTINEL ||
        cmd == CMD_MODULE || cmd == CMD_DEBUG || cmd == CMD_ACL ||
        cmd == CMD_HOTKEYS || cmd == CMD_BACKUP)
        return;
    for (i = 0; i < srv->hotkeys_capacity; i++) {
        e = &srv->hotkeys_entries[i];
        if (e->len == klen && memcmp(e->key, key, klen) == 0) {
            e->cpu_us += source != NULL && source->last_cmd_usecs != 0
                             ? source->last_cmd_usecs
                             : 1;
            e->net_bytes += net;
            return;
        }
        if (slot == SIZE_MAX && e->len == 0)
            slot = i;
    }
    if (slot == SIZE_MAX && srv->hotkeys_capacity != 0) {
        uint64_t min = UINT64_MAX;
        for (i = 0; i < srv->hotkeys_capacity; i++) {
            uint64_t score = srv->hotkeys_metric_cpu
                                 ? srv->hotkeys_entries[i].cpu_us
                                 : srv->hotkeys_entries[i].net_bytes;
            if (score < min) {
                min = score;
                slot = i;
            }
        }
    }
    if (slot == SIZE_MAX)
        return;
    e = &srv->hotkeys_entries[slot];
    memcpy(e->key, key, klen);
    e->len = klen;
    e->cpu_us = source != NULL && source->last_cmd_usecs != 0
                    ? source->last_cmd_usecs
                    : 1;
    e->net_bytes = net;
}

static void srv_hotkeys_record(server *srv, const session *source,
                               const resp_value *argv, size_t argc)
{
    uint16_t cmd;
    size_t i;
    if (argc < 2 || argv[0].str == NULL)
        return;
    if (srv->hotkeys_sample_ratio == 0 ||
        (++srv->hotkeys_sampled % srv->hotkeys_sample_ratio) != 0)
        return;
    cmd = cmd_resolve(argv[0].str, argv[0].len);
    if (cmd == CMD_MSET || cmd == CMD_MSETNX) {
        for (i = 1; i + 1 < argc; i += 2)
            if (argv[i].str != NULL)
                srv_hotkeys_record_key(srv, argv, argc, source, argv[i].str,
                                       argv[i].len);
        return;
    }
    if (cmd == CMD_MGET || cmd == CMD_DEL || cmd == CMD_UNLINK ||
        cmd == CMD_EXISTS || cmd == CMD_TOUCH) {
        for (i = 1; i < argc; i++)
            if (argv[i].str != NULL)
                srv_hotkeys_record_key(srv, argv, argc, source, argv[i].str,
                                       argv[i].len);
        return;
    }
    if (argc >= 2 && argv[1].str != NULL)
        srv_hotkeys_record_key(srv, argv, argc, source, argv[1].str,
                               argv[1].len);
}

static int server_token_eq(const char *s, size_t len, const char *word)
{
    size_t i, n = strlen(word);
    if (s == NULL)
        return 0;
    if (len != n)
        return 0;
    for (i = 0; i < n; i++) {
        char a = s[i], b = word[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

static int server_parse_ll(const resp_value *v, long long *out)
{
    size_t i = 0;
    int neg = 0;
    unsigned long long n = 0;
    if (v == NULL || v->str == NULL || v->len == 0)
        return 0;
    if (v->str[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i == v->len)
        return 0;
    for (; i < v->len; i++) {
        unsigned digit;
        if (v->str[i] < '0' || v->str[i] > '9')
            return 0;
        digit = (unsigned)(v->str[i] - '0');
        if (n > 922337203685477580ULL ||
            (n == 922337203685477580ULL && digit > (neg ? 8u : 7u)))
            return 0;
        n = n * 10ULL + digit;
    }
    if (neg)
        *out = n == 9223372036854775808ULL ? LLONG_MIN : -(long long)n;
    else
        *out = (long long)n;
    return 1;
}

static int srv_hotkeys_command(void *ctx, const resp_value *argv, size_t argc,
                               resp_buf *out)
{
    server *srv = (server *)ctx;
    const char *sub;
    size_t sl;
    size_t i;
    if (argc < 2 || argv[1].str == NULL)
        return -1;
    sub = argv[1].str;
    sl = argv[1].len;
    if (server_token_eq(sub, sl, "START")) {
        uint64_t duration = 0, sample = 1;
        long long metric_count;
        long long requested_count = 10;
        size_t metrics_seen;
        int cpu = 0, net = 0;
        int slots_seen = 0;
        if (argc < 4 || !server_token_eq(argv[2].str, argv[2].len, "METRICS") ||
            !server_parse_ll(&argv[3], &metric_count) || metric_count < 1 ||
            metric_count > 2 || (uint64_t)metric_count > (uint64_t)(argc - 4))
            return resp_write_error(out, "ERR syntax error", 16), 0;
        for (metrics_seen = 0; metrics_seen < (size_t)metric_count; metrics_seen++) {
            const resp_value *metric = &argv[4 + metrics_seen];
            if (server_token_eq(metric->str, metric->len, "CPU") && !cpu)
                cpu = 1;
            else if (server_token_eq(metric->str, metric->len, "NET") && !net)
                net = 1;
            else
                return resp_write_error(out, "ERR syntax error", 16), 0;
        }
        i = 4 + (size_t)metric_count;
        while (i < argc) {
            const char *tok = argv[i].str;
            size_t len = argv[i].len;
            if (server_token_eq(tok, len, "CPU") || server_token_eq(tok, len, "NET")) {
                i++;
            } else if (server_token_eq(tok, len, "COUNT") && i + 1 < argc) {
                long long count;
                if (!server_parse_ll(&argv[++i], &count) || count < 1)
                    return resp_write_error(out, "ERR syntax error", 16), 0;
                requested_count = count;
                i++;
            } else if (server_token_eq(tok, len, "DURATION") && i + 1 < argc) {
                long long seconds;
                if (!server_parse_ll(&argv[++i], &seconds) || seconds < 1)
                    return resp_write_error(out, "ERR syntax error", 16), 0;
                if ((uint64_t)seconds > UINT64_MAX / 1000ULL)
                    return resp_write_error(out, "ERR syntax error", 16), 0;
                duration = (uint64_t)seconds * 1000ULL;
                i++;
            } else if (server_token_eq(tok, len, "SAMPLE") && i + 1 < argc) {
                long long ratio;
                if (!server_parse_ll(&argv[++i], &ratio) || ratio < 1)
                    return resp_write_error(out, "ERR syntax error", 16), 0;
                sample = (uint64_t)ratio;
                i++;
            } else if (server_token_eq(tok, len, "SLOTS")) {
                long long count;
                size_t j;
                if (i + 1 >= argc || !server_parse_ll(&argv[++i], &count) || count < 1 ||
                    (uint64_t)count > (uint64_t)(argc - i - 1))
                    return resp_write_error(out, "ERR syntax error", 16), 0;
                for (j = 0; j < (size_t)count; j++) {
                    long long slot;
                    if (!server_parse_ll(&argv[i + 1 + j], &slot) ||
                        slot < 0 || slot > 16383)
                        return resp_write_error(out, "ERR syntax error", 16), 0;
                }
                if (!slots_seen) {
                    memset(srv->hotkeys_slots, 0, sizeof(srv->hotkeys_slots));
                    slots_seen = 1;
                }
                for (j = 0; j < (size_t)count; j++) {
                    long long slot = 0;
                    (void)server_parse_ll(&argv[i + 1 + j], &slot);
                    srv->hotkeys_slots[slot] = 1;
                }
                i += (size_t)count + 1u;
            } else {
                return resp_write_error(out, "ERR syntax error", 16), 0;
            }
        }
        if (requested_count > 4096)
            requested_count = 4096;
        {
            hotkey_entry *entries = (hotkey_entry *)calloc(
                (size_t)requested_count, sizeof(*entries));
            if (entries == NULL)
                return resp_write_error(out, "ERR out of memory", 17), 0;
            free(srv->hotkeys_entries);
            srv->hotkeys_entries = entries;
        }
        srv->hotkeys_active = 1;
        srv->hotkeys_initialized = 1;
        srv->hotkeys_start_ms = pal_wall_ms();
        srv->hotkeys_commands = 0;
        srv->hotkeys_duration_ms = duration;
        srv->hotkeys_sample_ratio = sample;
        srv->hotkeys_metric_cpu = cpu;
        srv->hotkeys_metric_net = net;
        srv->hotkeys_capacity = (size_t)requested_count;
        srv->hotkeys_slot_filter = slots_seen;
        srv->hotkeys_sampled = 0;
        resp_write_simple_string(out, "OK", 2);
        return 0;
    }
    if (server_token_eq(sub, sl, "STOP")) {
        srv->hotkeys_active = 0;
        resp_write_simple_string(out, "OK", 2);
        return 0;
    }
    if (server_token_eq(sub, sl, "RESET")) {
        srv->hotkeys_active = 0;
        srv->hotkeys_initialized = 0;
        srv->hotkeys_start_ms = 0;
        srv->hotkeys_commands = 0;
        srv->hotkeys_duration_ms = 0;
        srv->hotkeys_sample_ratio = 1;
        srv->hotkeys_metric_cpu = 0;
        srv->hotkeys_metric_net = 0;
        srv->hotkeys_sampled = 0;
        srv->hotkeys_capacity = 0;
        srv->hotkeys_slot_filter = 0;
        memset(srv->hotkeys_slots, 0, sizeof(srv->hotkeys_slots));
        free(srv->hotkeys_entries);
        srv->hotkeys_entries = NULL;
        resp_write_simple_string(out, "OK", 2);
        return 0;
    }
    if (server_token_eq(sub, sl, "GET")) {
        if (srv->hotkeys_active && srv->hotkeys_duration_ms != 0 &&
            pal_wall_ms() - srv->hotkeys_start_ms >= srv->hotkeys_duration_ms)
            srv->hotkeys_active = 0;
        if (!srv->hotkeys_initialized) {
            resp_write_null(out);
            return 0;
        }
        {
            size_t pairs = 4 + (srv->hotkeys_metric_cpu ? 1u : 0u) +
                           (srv->hotkeys_metric_net ? 1u : 0u);
            size_t i, j;
            size_t cap = srv->hotkeys_capacity == 0 ? 1 : srv->hotkeys_capacity;
            size_t cpu_order[cap];
            size_t net_order[cap];
            for (i = 0; i < srv->hotkeys_capacity; i++) {
                cpu_order[i] = i;
                net_order[i] = i;
            }
            /* Sort independent metric views without mutating collection state. */
            for (i = 1; i < srv->hotkeys_capacity; i++) {
                size_t ci = cpu_order[i], ni = net_order[i];
                j = i;
                while (j > 0 &&
                       srv->hotkeys_entries[cpu_order[j - 1]].cpu_us <
                           srv->hotkeys_entries[ci].cpu_us) {
                    cpu_order[j] = cpu_order[j - 1];
                    j--;
                }
                cpu_order[j] = ci;
                j = i;
                while (j > 0 &&
                       srv->hotkeys_entries[net_order[j - 1]].net_bytes <
                           srv->hotkeys_entries[ni].net_bytes) {
                    net_order[j] = net_order[j - 1];
                    j--;
                }
                net_order[j] = ni;
            }
            resp_write_array_header(out, 1);
            resp_write_map_header(out, pairs);
        resp_write_bulk(out, "tracking-active", 15);
        resp_write_integer(out, srv->hotkeys_active);
        resp_write_bulk(out, "sample-ratio", 12);
        resp_write_integer(out, (long long)srv->hotkeys_sample_ratio);
        resp_write_bulk(out, "collection-start-time-unix-ms", 29);
        resp_write_integer(out, (long long)srv->hotkeys_start_ms);
        resp_write_bulk(out, "total-commands", 14);
        resp_write_integer(out, (long long)srv->hotkeys_commands);
            if (srv->hotkeys_metric_cpu) {
                resp_write_bulk(out, "by-cpu-time-us", 14);
                resp_write_array_header(out, srv->hotkeys_capacity * 2u);
                for (i = 0; i < srv->hotkeys_capacity; i++) {
                    hotkey_entry *e = &srv->hotkeys_entries[cpu_order[i]];
                    if (e->len == 0) {
                        resp_write_bulk(out, NULL, 0);
                        resp_write_integer(out, 0);
                    } else {
                        resp_write_bulk(out, e->key, e->len);
                        resp_write_integer(out, (long long)e->cpu_us);
                    }
                }
            }
            if (srv->hotkeys_metric_net) {
                resp_write_bulk(out, "by-net-bytes", 12);
                resp_write_array_header(out, srv->hotkeys_capacity * 2u);
                for (i = 0; i < srv->hotkeys_capacity; i++) {
                    hotkey_entry *e = &srv->hotkeys_entries[net_order[i]];
                    if (e->len == 0) {
                        resp_write_bulk(out, NULL, 0);
                        resp_write_integer(out, 0);
                    } else {
                        resp_write_bulk(out, e->key, e->len);
                        resp_write_integer(out, (long long)e->net_bytes);
                    }
                }
            }
        }
        return 0;
    }
    resp_write_error(out, "ERR unknown HOTKEYS subcommand", 30);
    return 0;
}

enum {
    BACKUP_IDLE = 0,
    BACKUP_PENDING = 1,
    BACKUP_SNAPSHOT = 2,
    BACKUP_INCREMENTING = 3,
    BACKUP_SEALED = 4,
    BACKUP_FAILED = 5
};

static const char *backup_state_name(int state)
{
    static const char *const names[] = {
        "idle", "pending", "snapshotting", "incrementing", "sealed", "failed"
    };
    return state >= BACKUP_IDLE && state <= BACKUP_FAILED ? names[state] : "failed";
}

static int srv_backup_command(void *ctx, const resp_value *argv, size_t argc,
                              resp_buf *out)
{
    server *srv = (server *)ctx;
    const char *sub;
    size_t len;
    if (argc < 2 || argv[1].str == NULL ||
        (argv[1].type != RESP_BULK_STRING && argv[1].type != RESP_SIMPLE_STRING))
        return resp_write_error(out, "ERR wrong number of arguments", 31), 0;
    sub = argv[1].str;
    len = argv[1].len;
    if (server_token_eq(sub, len, "START")) {
        if (argc != 2)
            return resp_write_error(out, "ERR wrong number of arguments", 31), 0;
        if (srv->backup_state == BACKUP_PENDING ||
            srv->backup_state == BACKUP_SNAPSHOT ||
            srv->backup_state == BACKUP_INCREMENTING)
            return resp_write_error(out, "ERR backup already in progress", 31), 0;
        if (srv->backup_state == BACKUP_SEALED)
            return resp_write_error(out, "ERR backup is sealed", 21), 0;
        if (srv->db.snapshot_path == NULL || srv->db.snapshot_path[0] == '\0') {
            srv->backup_state = BACKUP_FAILED;
            snprintf(srv->backup_error, sizeof(srv->backup_error),
                     "snapshot path not configured");
            return resp_write_error(out, "ERR snapshot path not configured", 32), 0;
        }
        free(srv->backup_path);
        srv->backup_path = NULL;
        free(srv->backup_aof_path);
        srv->backup_aof_path = NULL;
        srv->backup_aof_offset = UINT64_MAX;
        {
            size_t n = strlen(srv->db.snapshot_path);
            srv->backup_path = (char *)malloc(n + sizeof(".backup"));
            if (srv->backup_path == NULL) {
                srv->backup_state = BACKUP_FAILED;
                snprintf(srv->backup_error, sizeof(srv->backup_error), "out of memory");
                return resp_write_error(out, "ERR out of memory", 17), 0;
            }
            memcpy(srv->backup_path, srv->db.snapshot_path, n);
            memcpy(srv->backup_path + n, ".backup", sizeof(".backup"));
        }
        if (srv->aof != NULL) {
            srv->backup_aof_offset = aof_durable_offset(srv->aof);
            if (srv->backup_aof_offset == UINT64_MAX) {
                srv->backup_state = BACKUP_FAILED;
                snprintf(srv->backup_error, sizeof(srv->backup_error),
                         "aof flush failed");
                return resp_write_error(out, "ERR backup AOF flush failed", 28), 0;
            }
            srv->backup_aof_path = (char *)malloc(strlen(srv->backup_path) + 5);
            if (srv->backup_aof_path == NULL) {
                srv->backup_state = BACKUP_FAILED;
                snprintf(srv->backup_error, sizeof(srv->backup_error),
                         "out of memory");
                return resp_write_error(out, "ERR out of memory", 17), 0;
            }
            sprintf(srv->backup_aof_path, "%s.aof", srv->backup_path);
        }
        srv->backup_state = BACKUP_SNAPSHOT;
        srv->backup_start_ms = pal_wall_ms();
        srv->backup_end_ms = 0;
        srv->backup_error[0] = '\0';
        if (snapshot_save_multi(srv, srv_select_db, srv->ndbs,
                                srv->backup_path) != 0) {
            srv->backup_state = BACKUP_FAILED;
            snprintf(srv->backup_error, sizeof(srv->backup_error), "snapshot save failed");
            return resp_write_error(out, "ERR backup snapshot failed", 28), 0;
        }
        srv->backup_state = BACKUP_INCREMENTING;
        srv->backup_end_ms = pal_wall_ms();
        resp_write_simple_string(out, "OK", 2);
        return 0;
    }
    if (server_token_eq(sub, len, "STATUS")) {
        resp_write_map_header(out, 4);
        resp_write_bulk(out, "state", 5);
        resp_write_bulk(out, backup_state_name(srv->backup_state),
                        strlen(backup_state_name(srv->backup_state)));
        resp_write_bulk(out, "start_time", 10);
        resp_write_integer(out, (long long)srv->backup_start_ms);
        resp_write_bulk(out, "end_time", 8);
        resp_write_integer(out, (long long)srv->backup_end_ms);
        resp_write_bulk(out, "error", 5);
        resp_write_bulk(out, srv->backup_error[0] ? srv->backup_error : NULL,
                        srv->backup_error[0] ? strlen(srv->backup_error) : 0);
        return 0;
    }
        if (server_token_eq(sub, len, "LIST")) {
        size_t count = (srv->backup_path != NULL ? 1u : 0u) +
                       (srv->backup_aof_path != NULL ? 1u : 0u);
        resp_write_array_header(out, count);
        if (srv->backup_path != NULL)
            resp_write_bulk(out, srv->backup_path, strlen(srv->backup_path));
        if (srv->backup_aof_path != NULL)
            resp_write_bulk(out, srv->backup_aof_path,
                            strlen(srv->backup_aof_path));
        return 0;
    }
    if (server_token_eq(sub, len, "SEAL")) {
        if (argc != 2)
            return resp_write_error(out, "ERR wrong number of arguments", 31), 0;
        if (srv->backup_state != BACKUP_INCREMENTING)
            return resp_write_error(out, "ERR no pending backup", 22), 0;
        if (srv->backup_aof_path != NULL &&
            aof_copy_delta(srv->aof, srv->backup_aof_offset,
                           srv->backup_aof_path) != 0) {
            srv->backup_state = BACKUP_FAILED;
            snprintf(srv->backup_error, sizeof(srv->backup_error),
                     "aof delta copy failed");
            return resp_write_error(out, "ERR backup AOF seal failed", 27), 0;
        }
        srv->backup_state = BACKUP_SEALED;
        resp_write_simple_string(out, "OK", 2);
        return 0;
    }
    if (server_token_eq(sub, len, "ABORT")) {
        if (argc != 2)
            return resp_write_error(out, "ERR wrong number of arguments", 31), 0;
        if (srv->backup_state == BACKUP_SEALED)
            return resp_write_error(out, "ERR backup is sealed", 21), 0;
        if (srv->backup_path != NULL)
            pal_file_unlink(srv->backup_path);
        if (srv->backup_aof_path != NULL)
            pal_file_unlink(srv->backup_aof_path);
        free(srv->backup_path);
        srv->backup_path = NULL;
        free(srv->backup_aof_path);
        srv->backup_aof_path = NULL;
        srv->backup_aof_offset = UINT64_MAX;
        srv->backup_state = BACKUP_IDLE;
        srv->backup_start_ms = 0;
        srv->backup_end_ms = 0;
        srv->backup_error[0] = '\0';
        resp_write_simple_string(out, "OK", 2);
        return 0;
    }
    if (server_token_eq(sub, len, "CLEANUP")) {
        if (argc != 2)
            return resp_write_error(out, "ERR wrong number of arguments", 31), 0;
        if (srv->backup_state != BACKUP_SEALED)
            return resp_write_error(out, "ERR no sealed backup", 21), 0;
        if (srv->backup_path != NULL && pal_file_unlink(srv->backup_path) != 0 &&
            pal_file_exists(srv->backup_path))
            return resp_write_error(out, "ERR backup cleanup failed", 27), 0;
        if (srv->backup_aof_path != NULL &&
            pal_file_unlink(srv->backup_aof_path) != 0 &&
            pal_file_exists(srv->backup_aof_path))
            return resp_write_error(out, "ERR backup AOF cleanup failed", 31), 0;
        srv->backup_state = BACKUP_IDLE;
        free(srv->backup_path);
        srv->backup_path = NULL;
        free(srv->backup_aof_path);
        srv->backup_aof_path = NULL;
        srv->backup_aof_offset = UINT64_MAX;
        resp_write_simple_string(out, "OK", 2);
        return 0;
    }
    resp_write_error(out, "ERR unknown BACKUP subcommand", 30);
    return 0;
}

/* Propagation sink for every successfully-applied mutating command:
 * serialize once, then fan out to AOF (if any), the replication backlog
 * and all downstream replica conns (flushed at end of run_once). */
static void srv_propagate(void *ctx, int db_index, const resp_value *argv,
                          size_t argc, const char *raw, size_t raw_len)
{
    server *srv = (server *)ctx;
    size_t i;
    if (srv->aof != NULL && !srv->aof_failed)
        srv_aof_log(srv, db_index, argv, argc);

    if (srv->repl_centralized) {
        /* mt: worker-local backlogs are inert; forward the authoritative
         * command bytes to the coordinator instead of fanning out here. */
        if (raw != NULL) {
            server_repl_stream_forward(srv, db_index, raw, raw_len);
        } else {
            srv->prop_buf.len = 0;
            resp_write_array_header(&srv->prop_buf, argc);
            for (i = 0; i < argc; i++) {
                if (argv[i].type == RESP_BULK_STRING ||
                    argv[i].type == RESP_SIMPLE_STRING)
                    resp_write_bulk(&srv->prop_buf, argv[i].str,
                                    argv[i].len);
                else
                    resp_write_bulk(&srv->prop_buf, "", 0);
            }
            server_repl_stream_forward(srv, db_index, srv->prop_buf.data,
                                       srv->prop_buf.len);
        }
        return;
    }

    /* no replication sinks: with zero downstream replicas and no replica
     * ever attached, the backlog can never be resumed from, so skip the
     * append entirely (Phase 28). A first replica always full-resyncs. */
    if (srv->repl.connected_slaves == 0 && !srv->backlog_used)
        return;

    /* raw fast path (Phase 27): top-level client commands forward their
     * exact request bytes; the re-serialization below is only for
     * replays, script effects and internal executions */
    if (raw != NULL) {
        repl_backlog_append(&srv->backlog, raw, raw_len);
        srv->repl.offset = srv->backlog.offset;
        if (srv->repl.connected_slaves == 0)
            return;
        for (i = 0; i < srv->nconns; i++) {
            conn *rc = srv->conns[i];
            if (rc->is_replica) {
                if (resp_buf_reserve(&rc->out, raw_len) != 0) {
                    rc->close_after_send = 1;
                    continue;
                }
                memcpy(rc->out.data + rc->out.len, raw, raw_len);
                rc->out.len += raw_len;
            }
        }
        return;
    }

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
            if (resp_buf_reserve(&rc->out, srv->prop_buf.len) != 0) {
                rc->close_after_send = 1;
                continue;
            }
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
    /* pointing at a different master invalidates the PSYNC resume cache */
    if (strcmp(srv->repl.master_host, host) != 0 ||
        srv->repl.master_port != port) {
        srv->repl.master_replid[0] = '\0';
        srv->repl.master_offset = 0;
    }
    snprintf(srv->repl.master_host, sizeof(srv->repl.master_host), "%s",
             host);
    srv->repl.master_port = port;
    srv->role = SESSION_ROLE_REPLICA;
    srv->repl.role = SESSION_ROLE_REPLICA;
    srv->repl.link_up = 0;
    return repl_link_connect(srv);
}

int server_set_replica_tls(server *s, int enabled, const char *ca_file)
{
    pal_tls_ctx *ctx = NULL;
    if (s == NULL)
        return -1;
    if (!enabled) {
        if (s->master_link != NULL)
            repl_link_close(s);
        if (s->repl_tls_ctx != NULL)
            pal_tls_ctx_free(s->repl_tls_ctx);
        s->repl_tls_ctx = NULL;
        s->repl_tls = 0;
        return 0;
    }
    if (srv_proactor(s))
        return -1; /* TLS link uses readiness read/write semantics. */
    ctx = pal_tls_ctx_new_client(ca_file);
    if (ctx == NULL)
        return -1;
    /* A live master link retains its TLS context through SSL, but it would
     * keep the old transport policy. Reconnect so the change is atomic. */
    if (s->master_link != NULL)
        repl_link_close(s);
    if (s->repl_tls_ctx != NULL)
        pal_tls_ctx_free(s->repl_tls_ctx);
    s->repl_tls_ctx = ctx;
    s->repl_tls = 1;
    return 0;
}

int server_replicaof(server *s, const char *host, uint16_t port)
{
    return srv_replicaof(s, host, port);
}

int server_set_backlog_size(server *s, size_t bytes)
{
    repl_backlog replacement;
    if (repl_backlog_init(&replacement, bytes) != 0)
        return -1;
    replacement.offset = s->backlog.offset;
    repl_backlog_free(&s->backlog);
    s->backlog = replacement;
    s->repl.offset = s->backlog.offset;
    return 0;
}

/* Append an optional prefix and $<len>\r\n<snapshot> only after serialization
 * succeeds, so callers never expose a partial full-resync response. */
static int srv_write_snapshot(server *srv, resp_buf *out, const char *prefix,
                              size_t prefix_len)
{
    resp_buf snap;
    char hdr[32];
    int hl;
    size_t frame_len;

    resp_buf_init(&snap);
    if (srv->repl_snapshot_serialize_fn != NULL) {
        if (srv->repl_snapshot_serialize_fn(srv->repl_snapshot_serialize_ctx,
                                            &snap) != 0) {
            resp_buf_free(&snap);
            return -1;
        }
    } else if (snapshot_serialize_multi(srv, srv_select_db, srv->ndbs,
                                        &snap) != 0) {
        resp_buf_free(&snap);
        return -1;
    }
    hl = snprintf(hdr, sizeof(hdr), "$%llu\r\n",
                  (unsigned long long)snap.len);
    if (prefix_len > SIZE_MAX - (size_t)hl ||
        snap.len > SIZE_MAX - prefix_len - (size_t)hl) {
        resp_buf_free(&snap);
        return -1;
    }
    frame_len = prefix_len + (size_t)hl + snap.len;
    if (frame_len > SIZE_MAX - out->len) {
        resp_buf_free(&snap);
        return -1;
    }
    if (resp_buf_reserve(out, frame_len) != 0) {
        resp_buf_free(&snap);
        return -1;
    }
    if (prefix_len > 0) {
        memcpy(out->data + out->len, prefix, prefix_len);
        out->len += prefix_len;
    }
    memcpy(out->data + out->len, hdr, (size_t)hl);
    out->len += (size_t)hl;
    memcpy(out->data + out->len, snap.data, snap.len);
    out->len += snap.len;
    resp_buf_free(&snap);
    return 0;
}

/* SYNC: write the snapshot frame, then mark the client as a replica. */
static void srv_sync(void *ctx, session *sess)
{
    server *srv = (server *)ctx;
    conn *c = (conn *)sess->owner;

    if (srv_write_snapshot(srv, &c->out, NULL, 0) != 0) {
        resp_write_error(&c->out, "ERR snapshot serialization failed", 33);
        return;
    }
    if (!c->is_replica) {
        c->is_replica = 1;
        srv->repl.connected_slaves++;
        srv->backlog_used = 1; /* PSYNC resumes need appends from now on */
    }
}

static int srv_write_continue(server *srv, resp_buf *out, uint64_t offset,
                              int force_reserve_failure)
{
    char hdr[96];
    size_t backlog_len, frame_len, copied = 0, start = out->len;
    int hl = snprintf(hdr, sizeof(hdr), "+CONTINUE %s\r\n", srv->repl.replid);

    if (offset > srv->backlog.offset)
        return -1;
    backlog_len = (size_t)(srv->backlog.offset - offset);
    if ((uint64_t)backlog_len != srv->backlog.offset - offset ||
        (size_t)hl > SIZE_MAX - backlog_len)
        return -1;
    frame_len = (size_t)hl + backlog_len;
    if (force_reserve_failure || resp_buf_reserve(out, frame_len) != 0)
        return -1;

    memcpy(out->data + start, hdr, (size_t)hl);
    while (copied < backlog_len) {
        size_t got = repl_backlog_read_from(
            &srv->backlog, offset + copied, out->data + start + (size_t)hl + copied,
            backlog_len - copied);
        if (got == 0)
            return -1;
        copied += got;
    }
    out->len = start + frame_len;
    return 0;
}

/* PSYNC: partial resync when the caller's replid matches and the offset is
 * still inside the backlog (+CONTINUE), otherwise full resync
 * (+FULLRESYNC + snapshot frame). */
static int srv_psync_continue(server *srv, conn *c, uint64_t offset,
                              int force_reserve_failure)
{
    if (srv_write_continue(srv, &c->out, offset, force_reserve_failure) != 0)
        return -1;
    if (!c->is_replica) {
        c->is_replica = 1;
        srv->repl.connected_slaves++;
        srv->backlog_used = 1;
    }
    return 0;
}

int server_test_psync_continue_reserve_failure(void)
{
    server *srv = (server *)calloc(1, sizeof(*srv));
    conn c;
    char byte = 'x';
    int rc;

    if (srv == NULL)
        return -1;
    if (repl_backlog_init(&srv->backlog, 16) != 0) {
        free(srv);
        return -1;
    }
    memcpy(srv->repl.replid, "0123456789abcdef0123456789abcdef01234567", 41);
    repl_backlog_append(&srv->backlog, "abc", 3);
    srv->repl.offset = srv->backlog.offset;
    memset(&c, 0, sizeof(c));
    c.out.data = &byte;
    c.out.len = SIZE_MAX;
    c.out.cap = SIZE_MAX;

    rc = srv_psync_continue(srv, &c, srv->backlog.offset - 3, 0);
    if (rc != -1 || c.out.data != &byte || c.out.len != SIZE_MAX ||
        c.out.cap != SIZE_MAX || c.is_replica != 0 ||
        srv->repl.connected_slaves != 0 || srv->backlog_used != 0) {
        repl_backlog_free(&srv->backlog);
        free(srv);
        return -1;
    }
    resp_buf_init(&c.out);
    rc = srv_psync_continue(srv, &c, srv->backlog.offset - 3, 1);
    if (rc != -1 || c.out.len != 0 || c.is_replica != 0 ||
        srv->repl.connected_slaves != 0 || srv->backlog_used != 0) {
        resp_buf_free(&c.out);
        repl_backlog_free(&srv->backlog);
        free(srv);
        return -1;
    }
    resp_write_error(&c.out, "ERR partial resync output failed", 32);
    if (c.out.len != 35 ||
        memcmp(c.out.data, "-ERR partial resync output failed\r\n", 35) != 0) {
        resp_buf_free(&c.out);
        repl_backlog_free(&srv->backlog);
        free(srv);
        return -1;
    }
    resp_buf_free(&c.out);
    repl_backlog_free(&srv->backlog);
    free(srv);
    return 0;
}

static int srv_psync(void *ctx, session *sess, const char *replid,
                     size_t replid_len, long long offset)
{
    server *srv = (server *)ctx;
    conn *c = (conn *)sess->owner;
    char hdr[96];
    int hl;

    /* partial resync: same history and the resume point is still covered
     * by the backlog ring */
    if (replid_len == 40 && memcmp(replid, srv->repl.replid, 40) == 0 &&
        offset >= 0 &&
        (uint64_t)offset >= srv->backlog.offset - srv->backlog.len &&
        (uint64_t)offset <= srv->backlog.offset) {
        return srv_psync_continue(srv, c, (uint64_t)offset, 0);
    }

    hl = snprintf(hdr, sizeof(hdr), "+FULLRESYNC %s %llu\r\n",
                  srv->repl.replid,
                  (unsigned long long)srv->repl.offset);
    if (srv_write_snapshot(srv, &c->out, hdr, (size_t)hl) != 0) {
        return -2;
    }
    if (!c->is_replica) {
        c->is_replica = 1;
        srv->repl.connected_slaves++;
        srv->backlog_used = 1;
    }
    return 0;
}

static int srv_ci_equal(const char *a, size_t alen, const char *b)
{
    size_t i, blen = strlen(b);
    if (alen != blen)
        return 0;
    for (i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

/* CONFIG parameters that live on server rather than in a logical db. */
static int srv_config_command(void *ctx, const char *sub, size_t sub_len,
                              const char *param, size_t param_len,
                              const char *value, size_t value_len,
                              resp_buf *out)
{
    server *srv = (server *)ctx;
    (void)sub;
    (void)sub_len;
    if (!srv_ci_equal(param, param_len, "appendfsync"))
        return 0;
    if (value == NULL) {
        const char *mode = srv->aof_fsync_mode == AOF_FSYNC_ALWAYS
                               ? "always"
                               : srv->aof_fsync_mode == AOF_FSYNC_NO
                                     ? "no"
                                     : "everysec";
        resp_write_array_header(out, 2);
        resp_write_bulk(out, "appendfsync", 11);
        resp_write_bulk(out, mode, strlen(mode));
        return 1;
    }
    if (srv_ci_equal(value, value_len, "always"))
        srv->aof_fsync_mode = AOF_FSYNC_ALWAYS;
    else if (srv_ci_equal(value, value_len, "everysec"))
        srv->aof_fsync_mode = AOF_FSYNC_EVERYSEC;
    else if (srv_ci_equal(value, value_len, "no"))
        srv->aof_fsync_mode = AOF_FSYNC_NO;
    else {
        static const char E[] =
            "ERR invalid argument for CONFIG SET 'appendfsync'";
        resp_write_error(out, E, sizeof(E) - 1);
        return 1;
    }
    if (srv->aof != NULL)
        aof_set_fsync_mode(srv->aof, srv->aof_fsync_mode);
    resp_write_simple_string(out, "OK", 2);
    return 1;
}

int server_config_command(void *ctx, const char *sub, size_t sub_len,
                          const char *param, size_t param_len,
                          const char *value, size_t value_len,
                          resp_buf *out)
{
    return srv_config_command(ctx, sub, sub_len, param, param_len, value,
                              value_len, out);
}

#ifdef DDUP_TESTING
int server_test_config_appendfsync(const char *value, resp_buf *out)
{
    server s;
    memset(&s, 0, sizeof(s));
    s.aof_fsync_mode = AOF_FSYNC_EVERYSEC;
    return srv_config_command(&s, "GET", 3, "appendfsync", 11, value,
                              value == NULL ? 0 : strlen(value), out);
}
#endif

static conn *conn_create(server *srv, pal_socket_t fd)
{
    conn *c = (conn *)calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->fd = fd;
    c->srv = srv;
    c->send_fixed_id = -1;
    c->id = srv->next_client_id;
    srv->next_client_id += srv->client_id_stride == 0 ? 1 : srv->client_id_stride;
    c->name_len = 0;
    c->sess = session_create(&srv->db);
    if (srv->proto_max_request_bytes < SERVER_RECV_CHUNK) {
        c->rbuf = (char *)malloc(srv->proto_max_request_bytes);
        c->rbuf_pool_size = 0;
    } else {
        c->rbuf = (char *)buf_pool_get(&srv->pool, SERVER_RECV_CHUNK,
                                       &c->rbuf_pool_size);
    }
    if (c->rbuf != NULL) {
        c->rcap = c->rbuf_pool_size > 0 ? c->rbuf_pool_size
                                       : srv->proto_max_request_bytes;
    } else {
        /* Pool exhausted: fall back to malloc. */
        c->rcap = srv->proto_max_request_bytes < SERVER_RECV_CHUNK
                      ? srv->proto_max_request_bytes
                      : SERVER_RECV_CHUNK;
        c->rbuf = (char *)malloc(c->rcap);
        c->rbuf_pool_size = 0;
    }
    if (c->rbuf == NULL || c->sess == NULL) {
        session_free(c->sess);
        if (c->rbuf_pool_size > 0)
            buf_pool_put(&srv->pool, c->rbuf, c->rbuf_pool_size);
        else
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
    c->sess->ssubscribe = srv_ssubscribe;
    c->sess->sunsubscribe = srv_sunsubscribe;
    c->sess->each_schannel = srv_each_schannel;
    c->sess->spublish = srv_spublish;
    c->sess->schannel_nsub = srv_schannel_nsub;
    c->sess->shard_channels = srv_shard_channels;
    c->sess->deliver_shard = srv_deliver_shard;
    c->sess->spublish_bus = srv_spublish_bus;
    c->sess->deliver = srv_deliver;
    c->sess->psubscribe = srv_psubscribe;
    c->sess->punsubscribe = srv_punsubscribe;
    c->sess->each_pattern = srv_each_pattern;
    c->sess->deliver_pattern = srv_deliver_pattern;
    c->sess->pubsub_channels = srv_pubsub_channels;
    c->sess->channel_nsub = srv_channel_nsub;
    c->sess->numpat = srv_numpat;
    c->sess->io = &srv->io;
    c->sess->shutdown_ctx = srv;
    c->sess->request_shutdown = srv_request_shutdown;
    c->sess->repl = &srv->repl;
    c->sess->role = &srv->role;
    c->sess->sync_ctx = srv;
    c->sess->sync_hook = srv_sync;
    c->sess->psync_ctx = srv;
    c->sess->psync_hook = srv_psync;
    c->sess->requirepass = srv->requirepass;
    c->sess->authed = (srv->requirepass == NULL ||
                       srv->requirepass[0] == '\0')
                          ? 1
                          : 0;
    c->sess->sel_ctx = srv;
    c->sess->sel_fn = srv_select_db;
    c->sess->sel_ndbs = srv->ndbs;
    c->sess->replicaof_ctx = srv;
    c->sess->replicaof_hook = srv_replicaof;
    c->sess->reset_ctx = srv;
    c->sess->reset_hook = srv_reset;
    c->sess->client_ctx = srv;
    c->sess->client_id = srv_client_id;
    c->sess->client_setname = srv_client_setname;
    c->sess->client_getname = srv_client_getname;
    c->sess->client_list = srv_client_list;
    c->sess->client_kill = srv_client_kill;
    c->sess->slowlog_ctx = srv;
    c->sess->slowlog_add = srv_slowlog_add;
    c->sess->slowlog_len = srv_slowlog_len;
    c->sess->slowlog_get = srv_slowlog_get;
    c->sess->slowlog_reset = srv_slowlog_reset;
    c->sess->monitor_ctx = srv;
    c->sess->monitor_start = srv_monitor_start;
    c->sess->monitor_emit = srv_monitor_emit_session;
    c->sess->hotkeys_ctx = srv;
    c->sess->hotkeys_command = srv_hotkeys_command;
    c->sess->backup_ctx = srv;
    c->sess->backup_command = srv_backup_command;
    c->sess->bgrewriteaof_ctx = srv;
    c->sess->bgrewriteaof = srv_bgrewriteaof;
    c->sess->config_ctx = srv;
    c->sess->config_command = srv_config_command;
    c->sess->cluster_ctx = srv;
    c->sess->cluster_meet = srv_cluster_meet;
    c->sess->cluster_replicate = srv_replicaof;
    /* every conn propagates mutations through the server mux (AOF +
     * backlog + downstream replicas) */
    c->sess->aof_ctx = srv;
    c->sess->aof_log = srv_propagate;
    resp_buf_init(&c->out);
    c->out.pool = &srv->pool;
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
    while (c->ssubs != NULL)
        srv_sunsubscribe(c->srv, c->sess, c->ssubs->ch, c->ssubs->chlen);
    while (c->psubs != NULL)
        srv_punsubscribe(c->srv, c->sess, c->psubs->ch, c->psubs->chlen);
    if (c->tls != NULL) {
        pal_tls_shutdown(c->tls);
        pal_tls_free(c->tls);
    }
    pal_close(c->fd);
    if (c->srv->mt_state_free != NULL && c->mt_state != NULL)
        c->srv->mt_state_free(c->srv->route_ctx, c->mt_state);
    session_free(c->sess);
    free(c->link_snap);
    if (c->sbuf_pool_size > 0)
        buf_pool_put(&c->srv->pool, c->sbuf, c->sbuf_pool_size);
    else
        free(c->sbuf);
    if (c->rbuf_pool_size > 0)
        buf_pool_put(&c->srv->pool, c->rbuf, c->rbuf_pool_size);
    else
        free(c->rbuf);
    resp_buf_free(&c->out);
    arena_destroy(&c->arena);
    free(c);
}

/* Grow the connection receive buffer, preserving already-read bytes.
 * Returns 0 on success, -1 on allocation failure. */
static int conn_rbuf_grow(conn *c)
{
    size_t need;
    size_t actual;
    char *nb;

    if (c->rlen >= c->srv->proto_max_request_bytes)
        return -1;
    need = c->rcap > c->srv->proto_max_request_bytes / 2
               ? c->srv->proto_max_request_bytes
               : c->rcap * 2;
    if (need > c->srv->proto_max_request_bytes)
        need = c->srv->proto_max_request_bytes;
    if (need < SERVER_RECV_CHUNK) {
        nb = (char *)malloc(need);
        actual = nb == NULL ? 0 : need;
    } else {
        nb = (char *)buf_pool_get(&c->srv->pool, need, &actual);
        if (actual > c->srv->proto_max_request_bytes) {
            buf_pool_put(&c->srv->pool, nb, actual);
            nb = (char *)malloc(need);
            actual = nb == NULL ? 0 : need;
        }
    }
    if (nb == NULL)
        return -1;
    if (c->rlen > 0)
        memcpy(nb, c->rbuf, c->rlen);
    if (c->rbuf_pool_size > 0)
        buf_pool_put(&c->srv->pool, c->rbuf, c->rbuf_pool_size);
    else
        free(c->rbuf);
    c->rbuf = nb;
    c->rcap = actual;
    c->rbuf_pool_size = actual == need && need >= SERVER_RECV_CHUNK ? actual : 0;
    return 0;
}

/* conn IO: TLS when attached, plain socket otherwise; the IO counters
 * (Phase 27) count raw calls here, bytes only on success */
static ptrdiff_t conn_read(conn *c, void *buf, size_t n)
{
    ptrdiff_t r;
    if (c->tls != NULL)
        r = pal_tls_read(c->tls, buf, n);
    else
        r = pal_recv(c->fd, buf, n);
    c->srv->io.reads++;
    if (r > 0)
        c->srv->io.bytes_read += (uint64_t)r;
    return r;
}

static ptrdiff_t conn_write(conn *c, const void *buf, size_t n)
{
    ptrdiff_t r;
    if (c->tls != NULL)
        r = pal_tls_write(c->tls, buf, n);
    else
        r = pal_send(c->fd, buf, n);
    c->srv->io.writes++;
    if (r > 0)
        c->srv->io.bytes_written += (uint64_t)r;
    return r;
}

/* ------------------------------------------------------------------ */
/* master link (replica side)                                         */
/* ------------------------------------------------------------------ */

/* queue the PSYNC handshake (resume attempt when we have a cached master
 * id/offset, full resync otherwise) into the link's out buffer */
static int repl_link_queue_psync(server *srv, conn *c)
{
    char psync[160];
    int pl;
    if (srv->repl.master_replid[0] != '\0') {
        char offstr[24];
        int ol = snprintf(offstr, sizeof(offstr), "%llu",
                          (unsigned long long)srv->repl.master_offset);
        pl = snprintf(psync, sizeof(psync),
                      "*3\r\n$5\r\nPSYNC\r\n$40\r\n%s\r\n$%d\r\n%s\r\n",
                      srv->repl.master_replid, ol, offstr);
    } else {
        pl = snprintf(psync, sizeof(psync),
                      "*3\r\n$5\r\nPSYNC\r\n$1\r\n?\r\n$2\r\n-1\r\n");
    }
    if (pl < 0 || (size_t)pl > SIZE_MAX - c->out.len ||
        resp_buf_reserve(&c->out, (size_t)pl) != 0)
        return -1;
    memcpy(c->out.data, psync, (size_t)pl);
    c->out.len = (size_t)pl;
    c->link_state = LINK_SYNC_SENT;
    return 0;
}

static int repl_link_connect(server *srv)
{
    pal_socket_t fd = PAL_SOCKET_INVALID;
    conn *c;
    int cr;

    srv->last_reconnect = pal_now_ms();
    cr = pal_tcp_connect_start(srv->repl.master_host, srv->repl.master_port,
                               &fd);
    if (cr == 0 && srv_proactor(srv)) {
        /* no readiness on the proactor: bounded finish (refused peers fail
         * fast, healthy ones complete instantly; a dropping network costs
         * 50ms, retried by the 500ms timer without stalling the loop) */
        if (pal_connect_wait(fd, 50) != 0) {
            pal_close(fd);
            return -1;
        }
        cr = 1;
    }
    if (cr < 0)
        return -1;
    (void)pal_set_tcp_nodelay(fd, 1);
    c = conn_create(srv, fd);
    if (c == NULL) {
        pal_close(fd);
        return -1;
    }
    c->is_master_link = 1;
    c->sess->repl_link = 1;
    /* An in-progress nonblocking TCP connection cannot start TLS yet. */
    if (srv->repl_tls && cr == 1) {
        c->tls = pal_tls_new(srv->repl_tls_ctx, fd);
        if (c->tls == NULL) {
            conn_free(c);
            return -1;
        }
        c->tls_client = 1;
        c->tls_handshaking = 1;
    }
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
    if (srv_proactor(srv)) {
        /* proactor path: post the first overlapped recv; the PSYNC goes
         * out through kick_flush below */
        c->pending_ops++;
        srv->io.reads++;
        if (pro_recv(srv, fd, c->rbuf, c->rcap, c) < 0) {
            c->pending_ops--;
            conn_close(srv, srv->nconns - 1);
            return -1;
        }
    } else if (pal_loop_add(srv->loop, fd, 1, cr == 0, c) != 0) {
        conn_close(srv, srv->nconns - 1);
        return -1;
    }
    srv->master_link = c;
    if (cr == 1) {
        if (srv->repl_tls) {
            int hs = pal_tls_connect_handshake_nb(c->tls);
            if (hs < 0) {
                repl_link_close(srv);
                return -1;
            }
            if (hs == 1) {
                c->tls_handshaking = 0;
                if (repl_link_queue_psync(srv, c) != 0) {
                    repl_link_close(srv);
                    return -1;
                }
                (void)conn_flush(srv, c);
            } else {
                c->link_state = LINK_TLS_HANDSHAKE;
                if (pal_loop_mod(srv->loop, c->fd, 1, hs == 2, c) != 0) {
                    repl_link_close(srv);
                    return -1;
                }
            }
        } else if (repl_link_queue_psync(srv, c) != 0) {
            repl_link_close(srv);
            return -1;
        } else if (srv_proactor(srv))
            kick_flush(srv, c); /* ship the PSYNC via the proactor */
    } else {
        c->link_state = LINK_CONNECTING; /* PSYNC on writability */
    }
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

static void repl_link_close_fatal(server *srv)
{
    srv->repl.master_replid[0] = '\0';
    srv->repl.master_offset = 0;
    repl_link_close(srv);
}

static void repl_link_close_transport(server *srv, conn *c)
{
    if (c->link_state == LINK_SNAP_HDR || c->link_state == LINK_SNAPSHOT)
        repl_link_close_fatal(srv);
    else
        repl_link_close(srv);
}

/* Consume c->rbuf bytes through the master-link state machine (shared by
 * the readiness and IOCP paths; reads nothing from the socket itself). */
static void repl_link_feed(server *srv, conn *c)
{
    if (srv->aof_failed) {
        repl_link_close_fatal(srv);
        return;
    }
    if (c->link_state == LINK_SYNC_SENT) {
        size_t pos = 0;
        while (pos < c->rlen && c->rbuf[pos] != '\n')
            pos++;
        if (pos == c->rlen) {
            if (c->rlen > 128)
                repl_link_close_fatal(srv); /* garbage instead of a frame */
            return;                 /* wait for the rest of the header */
        }
        if (c->rbuf[0] == '+') {
            /* PSYNC handshake line */
            if (pos >= 12 && memcmp(c->rbuf, "+FULLRESYNC ", 12) == 0) {
                uint64_t moff = 0;
                size_t i;
                if (pos < 12 + 40 + 2) {
                    repl_link_close_fatal(srv);
                    return;
                }
                memcpy(c->pending_replid, c->rbuf + 12, 40);
                c->pending_replid[40] = '\0';
                if (c->rbuf[pos - 1] != '\r' || pos <= 12 + 40 + 1 ||
                    c->rbuf[12 + 40 + 1] < '0' ||
                    c->rbuf[12 + 40 + 1] > '9') {
                    repl_link_close_fatal(srv);
                    return;
                }
                for (i = 12 + 40 + 1; i < pos - 1; i++) {
                    if (c->rbuf[i] < '0' || c->rbuf[i] > '9' ||
                        moff > (UINT64_MAX - (unsigned)(c->rbuf[i] - '0')) / 10) {
                        repl_link_close_fatal(srv);
                        return;
                    }
                    moff = moff * 10 + (unsigned)(c->rbuf[i] - '0');
                }
                c->pending_offset = moff;
                memmove(c->rbuf, c->rbuf + pos + 1, c->rlen - pos - 1);
                c->rlen -= pos + 1;
                c->link_state = LINK_SNAP_HDR;
            } else if (pos >= 10 && memcmp(c->rbuf, "+CONTINUE ", 10) == 0) {
                if (pos < 10 + 40) {
                    repl_link_close_fatal(srv);
                    return;
                }
                memcpy(srv->repl.master_replid, c->rbuf + 10, 40);
                srv->repl.master_replid[40] = '\0';
                memmove(c->rbuf, c->rbuf + pos + 1, c->rlen - pos - 1);
                c->rlen -= pos + 1;
                c->link_state = LINK_STREAMING;
                srv->repl.link_up = 1;
            } else {
                repl_link_close_fatal(srv);
                return;
            }
        } else if (c->rbuf[0] == '$') {
            c->link_state = LINK_SNAP_HDR; /* legacy SYNC path */
        } else {
            repl_link_close(srv);
            return;
        }
    }
    if (c->link_state == LINK_SNAP_HDR) {
        size_t pos = 0;
        size_t i;
        uint64_t slen = 0;
        while (pos < c->rlen && c->rbuf[pos] != '\n')
            pos++;
        if (pos == c->rlen) {
            if (c->rlen > 64)
                repl_link_close_fatal(srv); /* garbage instead of a frame */
            return;                 /* wait for the rest of the header */
        }
        if (c->rbuf[0] != '$' || pos < 3 || c->rbuf[pos - 1] != '\r' ||
            c->rbuf[1] < '0' || c->rbuf[1] > '9') {
            repl_link_close_fatal(srv);
            return;
        }
        for (i = 1; i < pos - 1; i++) {
            if (c->rbuf[i] < '0' || c->rbuf[i] > '9' ||
                slen > (UINT64_MAX - (unsigned)(c->rbuf[i] - '0')) / 10) {
                repl_link_close_fatal(srv);
                return;
            }
            slen = slen * 10 + (unsigned)(c->rbuf[i] - '0');
        }
        if ((uint64_t)(size_t)slen != slen ||
            slen > (uint64_t)c->srv->repl_max_snapshot_bytes) {
            repl_link_close_fatal(srv);
            return;
        }
        c->link_need = (size_t)slen;
        c->link_snap = (char *)malloc(c->link_need == 0 ? 1 : c->link_need);
        if (c->link_snap == NULL) {
            repl_link_close_fatal(srv);
            return;
        }
        c->link_got = 0;
        /* consume the header; snapshot bytes are then taken from the front
         * of rbuf as they arrive (works for frames bigger than the chunk) */
        memmove(c->rbuf, c->rbuf + pos + 1, c->rlen - pos - 1);
        c->rlen -= pos + 1;
        c->link_state = LINK_SNAPSHOT;
    }
    if (c->link_state == LINK_SNAPSHOT) {
        size_t want = c->link_need - c->link_got;
        size_t take = c->rlen < want ? c->rlen : want;
        memcpy(c->link_snap + c->link_got, c->rbuf, take);
        c->link_got += take;
        memmove(c->rbuf, c->rbuf + take, c->rlen - take);
        c->rlen -= take;
        if (c->link_got < c->link_need)
            return; /* wait for the rest of the snapshot */
        if (srv->repl_snapshot_fn != NULL) {
            if (srv->repl_snapshot_fn(srv->repl_snapshot_ctx,
                                      c->link_snap, c->link_need) != 0) {
                free(c->link_snap);
                c->link_snap = NULL;
                repl_link_close_fatal(srv);
                return;
            }
        } else if (snapshot_load_mem_multi(srv, srv_select_db, srv->ndbs,
                                          c->link_snap, c->link_need,
                                          pal_wall_ms()) != 0) {
            free(c->link_snap);
            c->link_snap = NULL;
            repl_link_close_fatal(srv);
            return;
        }
        memcpy(srv->repl.master_replid, c->pending_replid, 41);
        srv->repl.master_offset = c->pending_offset;
        free(c->link_snap);
        c->link_snap = NULL;
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
            repl_link_close_fatal(srv); /* stream desync: full resync */
                return;
            }
            c->out.len = 0; /* replies are discarded */
            if (srv->route_fn != NULL) {
                int rr = srv->route_fn(srv->route_ctx, c, c->sess,
                                       v.items, v.count,
                                       c->rbuf + off, (size_t)used,
                                       &c->out);
                if (rr == 2) {
                    repl_link_close_fatal(srv); /* replica link never migrates */
                    return;
                }
                if (rr == 0)
                    session_execute(c->sess, v.items, v.count, &c->out);
            } else {
                session_execute(c->sess, v.items, v.count, &c->out);
            }
            c->out.len = 0;
            /* Replication batches may contain many mutations in one recv.
             * Detect persistence failure before applying the next frame. */
            srv_aof_flush(srv);
            if (srv->aof_failed) {
                repl_link_close_fatal(srv);
                return;
            }
            arena_reset(&c->arena);
            off += (size_t)used;
            srv->repl.master_offset += (uint64_t)used; /* PSYNC resume */
        }
        if (off > 0) {
            memmove(c->rbuf, c->rbuf + off, c->rlen - off);
            c->rlen -= off;
        }
    }
}

/* Service the outbound master link on the readiness backend: read, then
 * feed the state machine. */
static void repl_link_service(server *srv, conn *c)
{
    ptrdiff_t n;

    if (c->rcap - c->rlen < SERVER_RECV_CHUNK) {
        if (conn_rbuf_grow(c) != 0) {
            repl_link_close_transport(srv, c);
            return;
        }
    }
    n = conn_read(c, c->rbuf + c->rlen, c->rcap - c->rlen);
    if (n == 0 || (n < 0 && n != -2 && !pal_would_block(pal_socket_error()))) {
        repl_link_close_transport(srv, c); /* link down; retry timer reconnects */
        return;
    }
    if (n < 0)
        return;
    c->rlen += (size_t)n;
    repl_link_feed(srv, c);
}


static conn *client_conn_from_session(server *srv, struct session *s)
{
    conn *c;
    size_t i;
    if (s == NULL || s->owner == NULL)
        return NULL;
    c = (conn *)s->owner;
    for (i = 0; i < srv->nconns; i++)
        if (srv->conns[i] == c)
            return c;
    return c; /* rehomed or detached path still owns the conn */
}

static long long srv_client_id(void *ctx, struct session *s)
{
    conn *c = client_conn_from_session((server *)ctx, s);
    return c == NULL ? 0 : (long long)c->id;
}

static int srv_client_setname(void *ctx, struct session *s,
                              const char *name, size_t len)
{
    conn *c = client_conn_from_session((server *)ctx, s);
    if (c == NULL || len >= sizeof(c->name))
        return -1;
    memcpy(c->name, name, len);
    c->name[len] = '\0';
    c->name_len = len;
    return 0;
}

static const char *srv_client_getname(void *ctx, struct session *s,
                                      size_t *len)
{
    conn *c = client_conn_from_session((server *)ctx, s);
    if (c == NULL)
        return NULL;
    *len = c->name_len;
    return c->name;
}

static void srv_client_list(void *ctx, resp_buf *out)
{
    server *srv = (server *)ctx;
    resp_buf body;
    size_t i;
    resp_buf_init(&body);
    for (i = 0; i < srv->nconns; i++) {
        conn *c = srv->conns[i];
        char ip[64] = "0.0.0.0";
        char line[256];
        int n;
        (void)pal_get_peer_ip(c->fd, ip, sizeof(ip));
        n = snprintf(line, sizeof(line),
                     "id=%llu addr=%s name=%s\n",
                     (unsigned long long)c->id, ip,
                     c->name_len == 0 ? "" : c->name);
        if (n > 0 && resp_buf_reserve(&body, (size_t)n) == 0) {
            memcpy(body.data + body.len, line, (size_t)n);
            body.len += (size_t)n;
        }
    }
    resp_write_bulk(out, body.data, body.len);
    resp_buf_free(&body);
}

static int srv_client_kill(void *ctx, const char *filter, size_t filterlen,
                           resp_buf *out)
{
    server *srv = (server *)ctx;
    const char *p = filter;
    const char *end = filter + filterlen;
    long long id = -1;
    char addr[64] = "";
    size_t i;
    size_t killed = 0;

    while (p < end) {
        const char *tok;
        size_t toklen;
        const char *val = NULL;
        size_t vallen = 0;
        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        tok = p;
        while (p < end && *p != ' ' && *p != '\t')
            p++;
        toklen = (size_t)(p - tok);
        if (toklen == 0)
            break;
        if (toklen == 2 && memcmp(tok, "ID", 2) == 0) {
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            val = p;
            while (p < end && *p != ' ' && *p != '\t')
                p++;
            vallen = (size_t)(p - val);
            if (vallen > 0 && vallen < 32) {
                char tmp[32];
                memcpy(tmp, val, vallen);
                tmp[vallen] = '\0';
                id = strtoll(tmp, NULL, 10);
            }
        } else if (toklen == 4 && memcmp(tok, "ADDR", 4) == 0) {
            while (p < end && (*p == ' ' || *p == '\t'))
                p++;
            val = p;
            while (p < end && *p != ' ' && *p != '\t')
                p++;
            vallen = (size_t)(p - val);
            if (vallen < sizeof(addr)) {
                memcpy(addr, val, vallen);
                addr[vallen] = '\0';
            }
        }
    }

    for (i = 0; i < srv->nconns; i++) {
        conn *c = srv->conns[i];
        char ip[64] = "0.0.0.0";
        int match = 0;
        if (id >= 0 && (long long)c->id == id)
            match = 1;
        if (addr[0] != '\0') {
            (void)pal_get_peer_ip(c->fd, ip, sizeof(ip));
            if (strcmp(ip, addr) == 0)
                match = 1;
        }
        if (match) {
            c->close_after_send = 1;
            killed++;
        }
    }
    resp_write_integer(out, (long long)killed);
    return 1;
}

static void slowlog_entry_free(slowlog_entry *e)
{
    size_t i;
    if (e->argv == NULL)
        return;
    for (i = 0; i < e->argc; i++)
        free((void *)e->argv[i].str);
    free(e->argv);
    e->argv = NULL;
    e->argc = 0;
}

static void srv_slowlog_add(void *ctx, const resp_value *argv, size_t argc,
                            uint64_t usec, uint64_t now_ms)
{
    server *srv = (server *)ctx;
    slowlog_entry *e;
    resp_value *copy;
    size_t i;

    if (usec < srv->slowlog_threshold_us)
        return;
    if (srv->slowlog_max == 0)
        return;

    if (srv->slowlog_len == srv->slowlog_max) {
        slowlog_entry_free(&srv->slowlog[0]);
        memmove(&srv->slowlog[0], &srv->slowlog[1],
                (srv->slowlog_len - 1) * sizeof(srv->slowlog[0]));
        srv->slowlog_len--;
    }
    if (srv->slowlog_len == srv->slowlog_cap) {
        size_t ncap = srv->slowlog_cap == 0 ? 8 : srv->slowlog_cap * 2;
        slowlog_entry *ns = (slowlog_entry *)realloc(
            srv->slowlog, ncap * sizeof(*ns));
        if (ns == NULL)
            return;
        srv->slowlog = ns;
        srv->slowlog_cap = ncap;
    }
    copy = (resp_value *)calloc(argc, sizeof(*copy));
    if (copy == NULL && argc != 0)
        return;
    for (i = 0; i < argc; i++) {
        copy[i] = argv[i];
        copy[i].items = NULL;
        if (argv[i].str != NULL) {
            char *cp = (char *)malloc(argv[i].len);
            if (cp == NULL) {
                while (i > 0) {
                    i--;
                    free((void *)copy[i].str);
                }
                free(copy);
                return;
            }
            memcpy(cp, argv[i].str, argv[i].len);
            copy[i].str = cp;
        }
    }
    e = &srv->slowlog[srv->slowlog_len++];
    e->id = srv->slowlog_next_id++;
    e->ts = now_ms / 1000;
    e->usec = usec;
    e->argv = copy;
    e->argc = argc;
}

static size_t srv_slowlog_len(void *ctx)
{
    return ((server *)ctx)->slowlog_len;
}

static void srv_slowlog_get(void *ctx, long long count, resp_buf *out)
{
    server *srv = (server *)ctx;
    size_t i, n;
    if (count < 0)
        count = 10;
    n = srv->slowlog_len;
    if ((size_t)count < n)
        n = (size_t)count;
    resp_write_array_header(out, n);
    for (i = srv->slowlog_len - n; i < srv->slowlog_len; i++) {
        slowlog_entry *e = &srv->slowlog[i];
        size_t j;
        resp_write_array_header(out, 4);
        resp_write_integer(out, (long long)e->id);
        resp_write_integer(out, (long long)e->ts);
        resp_write_integer(out, (long long)e->usec);
        resp_write_array_header(out, e->argc);
        for (j = 0; j < e->argc; j++)
            resp_write_bulk(out, e->argv[j].str, e->argv[j].len);
    }
}

static void srv_slowlog_reset(void *ctx)
{
    server *srv = (server *)ctx;
    while (srv->slowlog_len > 0)
        slowlog_entry_free(&srv->slowlog[--srv->slowlog_len]);
}

static void srv_bgrewriteaof(void *ctx, resp_buf *out)
{
    server *srv = (server *)ctx;
    if (srv->aof != NULL && !srv->aof_failed && aof_flush(srv->aof) != 0) {
        srv->aof_failed = 1;
        srv->shutdown_flag = 1;
        resp_write_error(out, "MISCONF Errors writing to the AOF file",
                         35);
        return;
    }
    resp_write_simple_string(out,
                             "Background append only file rewriting started",
                             45);
}


server *server_create_ex(const char *host, uint16_t port, int backend)
{
    server *s = (server *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->listen_fd = PAL_SOCKET_INVALID;
    s->tls_listen_fd = PAL_SOCKET_INVALID;
    s->bus_listen_fd = PAL_SOCKET_INVALID;
    s->wakeup_fd = PAL_SOCKET_INVALID;
    s->node_timeout_ms = 15000;
    s->cluster_control = 1;
    s->aof_fsync_mode = AOF_FSYNC_EVERYSEC;
    s->next_client_id = 1;
    s->client_id_stride = 1;
    s->slowlog_threshold_us = 10000; /* 10 ms, Redis default */
    s->slowlog_max = 128;
    s->proto_max_request_bytes = (size_t)SERVER_DEFAULT_MAX_REQUEST;
    s->repl_max_snapshot_bytes = (size_t)SERVER_DEFAULT_MAX_REQUEST;
    s->backend = backend;
    if (backend == SERVER_BACKEND_IOURING) {
        s->loop = pal_loop_create_iouring();
        if (s->loop == NULL)
            s->loop = pal_loop_create(); /* unavailable: fall back to epoll */
    } else {
        s->loop = pal_loop_create();
    }
    if (backend == SERVER_BACKEND_IOCP) {
        s->iocp = pal_iocp_create();
        if (s->iocp == NULL)
            s->backend = SERVER_BACKEND_SELECT; /* unavailable: fall back */
    } else if (backend == SERVER_BACKEND_IOURING_OP) {
        unsigned iou_flags = 0;
        const char *e;
        /* Phase 33 hints, env-gated for A/B; each is runtime-probed with
         * silent fallback inside pal_iouring_create_ex */
        e = getenv("DDUP_IOU_SQPOLL");
        if (e != NULL && strcmp(e, "1") == 0)
            iou_flags |= PAL_IOURING_F_SQPOLL;
        e = getenv("DDUP_IOU_DEFER");
        if (e != NULL && strcmp(e, "1") == 0)
            iou_flags |= PAL_IOURING_F_DEFER;
        s->iou = pal_iouring_create_ex(iou_flags);
        if (s->iou == NULL)
            s->backend = SERVER_BACKEND_SELECT; /* unavailable: fall back */
        else if (getenv("DDUP_IOU_SEND_ZC") != NULL &&
                 strcmp(getenv("DDUP_IOU_SEND_ZC"), "0") != 0 &&
                 pal_iouring_enable_sbuf(s->iou, IOU_SBUF_COUNT,
                                         IOCP_SEND_CHUNK) == 0)
            s->iou_send_zc = 1;
    }
    if (s->backend == SERVER_BACKEND_IOCP) {
        s->listen_fd = pal_iocp_listen(s->iocp, host, port, &s->port, NULL);
        /* accept pool (Phase 32a): keep a second AcceptEx in flight; each
         * accept completion replenishes one, so two are always posted */
        if (s->listen_fd != PAL_SOCKET_INVALID)
            (void)pal_iocp_accept_post(s->iocp, s->listen_fd, NULL);
    } else if (s->backend == SERVER_BACKEND_IOURING_OP) {
        /* multishot accept is armed inside listen; nothing to replenish */
        s->listen_fd = pal_iouring_listen(s->iou, host, port, &s->port,
                                          NULL);
        /* multishot recv + provided-buffer ring (Phase 33); on kernels
         * without pbuf support the enable fails and the server keeps the
         * per-completion repost model. DDUP_IOU_RECV_MS=0 forces the
         * repost model (A/B comparisons). */
        if (s->listen_fd != PAL_SOCKET_INVALID &&
            (getenv("DDUP_IOU_RECV_MS") == NULL ||
             strcmp(getenv("DDUP_IOU_RECV_MS"), "0") != 0) &&
            pal_iouring_enable_pbuf(s->iou, IOU_PBUF_COUNT,
                                    IOU_PBUF_SIZE) == 0)
            s->iou_ms = 1;
    } else
        s->listen_fd = pal_tcp_listen(host, port, 511, &s->port);
    if (s->loop == NULL || s->listen_fd == PAL_SOCKET_INVALID) {
        server_destroy(s);
        return NULL;
    }
    db_init(&s->db);
    s->ndbs = 16;
    s->extra_dbs = (db *)calloc((size_t)(s->ndbs - 1), sizeof(db));
    if (s->extra_dbs == NULL) {
        server_destroy(s);
        return NULL;
    }
    {
        int i;
        for (i = 0; i < s->ndbs - 1; i++)
            db_init(&s->extra_dbs[i]);
    }
    rh_init(&s->channels);
    rh_init(&s->schannels);
    rh_init(&s->patterns);
    if (host != NULL)
        snprintf(s->db.cluster_ip, sizeof(s->db.cluster_ip), "%s", host);
    s->db.cluster_port = s->port;
    s->role = SESSION_ROLE_MASTER;
    buf_pool_init(&s->pool);
    if (repl_backlog_init(&s->backlog, 1024 * 1024) != 0) {
        server_destroy(s);
        return NULL;
    }
    resp_buf_init(&s->prop_buf);
    s->prop_buf.pool = &s->pool;
    memset(&s->repl, 0, sizeof(s->repl));
    s->repl.role = SESSION_ROLE_MASTER;
    s->repl_centralized = 0;
    cluster_gen_id(s->repl.replid);
    if (!srv_proactor(s) &&
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

int server_backend(const server *s)
{
    return s->backend;
}

int server_is_proactor(const server *s)
{
    return srv_proactor(s);
}

const buf_pool *server_buf_pool(const server *s)
{
    return &s->pool;
}

size_t server_pool_hits(const server *s)
{
    return s->pool.hits;
}

size_t server_pool_allocs(const server *s)
{
    return s->pool.allocs;
}

db *server_db(server *s)
{
    return &s->db;
}

static db *srv_select_db(void *ctx, int idx)
{
    server *srv = (server *)ctx;
    if (idx == 0)
        return &srv->db;
    return &srv->extra_dbs[idx - 1];
}

db *server_db_at(server *s, int idx)
{
    return srv_select_db(s, idx);
}

db *server_select_db(void *ctx, int idx)
{
    return srv_select_db(ctx, idx);
}

int server_ndbs(const server *s)
{
    return s->ndbs;
}

const io_counters *server_io_counters(server *s)
{
    return &s->io;
}

/* shared AOF writer with the multi-db SELECT prefix rule */
static void srv_aof_log(server *srv, int db_index, const resp_value *argv,
                        size_t argc)
{
    if (srv->aof == NULL || srv->aof_failed)
        return;
    if (db_index != srv->aof_db_index) {
        resp_value sel[2];
        char nbuf[16];
        int nl = snprintf(nbuf, sizeof(nbuf), "%d", db_index);
        memset(sel, 0, sizeof(sel));
        sel[0].type = RESP_BULK_STRING;
        sel[0].str = "SELECT";
        sel[0].len = 6;
        sel[1].type = RESP_BULK_STRING;
        sel[1].str = nbuf;
        sel[1].len = (size_t)nl;
        aof_log_cmd(srv->aof, sel, 2);
        srv->aof_db_index = db_index;
    }
    aof_log_cmd(srv->aof, argv, argc);
}

static int srv_aof_reject(const server *srv, const session *sess,
                          const resp_value *argv, size_t argc, resp_buf *out)
{
    static const char msg[] = "MISCONF Errors writing to the AOF file";
    const char *name;
    size_t nlen;
    uint16_t id;
    size_t i;
    if (!srv->aof_failed || argc == 0 ||
        (argv[0].type != RESP_BULK_STRING &&
         argv[0].type != RESP_SIMPLE_STRING))
        return 0;
    name = argv[0].str;
    nlen = argv[0].len;
    id = cmd_resolve(name, nlen);
    if (!cmd_is_write(id)) {
        if (id != CMD_EXEC)
            return 0;
        for (i = 0; i < sess->queue_len; i++) {
            const queued_cmd *q = &sess->queue[i];
            if (q->argc > 0 &&
                (q->argv[0].type == RESP_BULK_STRING ||
                 q->argv[0].type == RESP_SIMPLE_STRING) &&
                cmd_is_write(cmd_resolve(q->argv[0].str, q->argv[0].len)))
                break;
        }
        if (i == sess->queue_len)
            return 0;
    }
    resp_write_error(out, msg, sizeof(msg) - 1);
    return 1;
}

static void srv_aof_flush(server *srv)
{
    /* A failed append makes acknowledged mutations non-durable. Stop the
     * normal run loop, reject writes if an embedding keeps pumping it, and
     * leave the failed AOF buffer frozen for diagnosis/shutdown. */
    if (srv->aof != NULL && !srv->aof_failed && aof_flush(srv->aof) != 0) {
        srv->aof_failed = 1;
        srv->shutdown_flag = 1;
    }
}

void server_aof_log_cmd(server *s, int db_index, const resp_value *argv,
                        size_t argc)
{
    if (s->aof != NULL && !s->aof_failed)
        srv_aof_log(s, db_index, argv, argc);
}

void server_test_set_aof_write_fn(
    server *s,
    ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n))
{
    if (s->aof != NULL)
        aof_test_set_write_fn(s->aof, write_fn);
}

int server_test_aof_failed(const server *s)
{
    return s->aof_failed;
}

size_t server_test_aof_pending_bytes(const server *s)
{
    return s->aof != NULL ? s->aof->pending.len : 0;
}

/* Start a TLS listener alongside the plain one (port 0 = ephemeral).
 * Returns 0 on success; -1 when TLS is unavailable (stub build) or the
 * cert/key/listen setup failed. */
int server_enable_tls(server *s, const char *host, uint16_t port,
                      const char *cert_file, const char *key_file)
{
    if (server_tls_ctx_init(s, cert_file, key_file) != 0)
        return -1;
    s->tls_listen_fd = pal_tcp_listen(host, port, 511, &s->tls_port);
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

int server_tls_ctx_init(server *s, const char *cert_file,
                        const char *key_file)
{
    if (srv_proactor(s))
        return -1; /* TLS needs the readiness backend (documented) */
    if (s->tls_ctx != NULL)
        return -1; /* one context per server */
    s->tls_ctx = pal_tls_ctx_new(cert_file, key_file);
    return s->tls_ctx != NULL ? 0 : -1;
}

uint16_t server_tls_port(const server *s)
{
    return s->tls_port;
}

int server_enable_aof(server *s, const char *path)
{
    if (pal_file_exists(path)) {
        int rc;
        /* replay through a session with the selection hook so embedded
         * SELECT commands land on the right db */
        session *rs = session_create(&s->db);
        if (rs == NULL)
            return -1;
        rs->sel_ctx = s;
        rs->sel_fn = srv_select_db;
        rs->sel_ndbs = s->ndbs;
        rc = aof_replay_session(rs, path);
        session_free(rs);
        if (rc != 0)
            return -1;
    }
    s->aof = aof_open(path);
    if (s->aof != NULL)
        aof_set_fsync_mode(s->aof, s->aof_fsync_mode);
    return s->aof != NULL ? 0 : -1;
}

void server_set_appendfsync(server *s, int mode)
{
    s->aof_fsync_mode = mode;
    if (s->aof != NULL)
        aof_set_fsync_mode(s->aof, mode);
}

void server_aof_flush(server *s)
{
    if (s != NULL)
        srv_aof_flush(s);
}

void server_bgrewriteaof(server *s, resp_buf *out)
{
    if (s != NULL && out != NULL)
        srv_bgrewriteaof(s, out);
}

void server_client_list(server *s, resp_buf *out)
{
    if (s != NULL && out != NULL)
        srv_client_list(s, out);
}

int server_client_kill(server *s, const char *filter, size_t filterlen,
                       resp_buf *out)
{
    if (s == NULL || out == NULL)
        return 0;
    return srv_client_kill(s, filter, filterlen, out);
}

void server_slowlog_reset(server *s)
{
    if (s != NULL)
        srv_slowlog_reset(s);
}

int server_hotkeys_command(server *s, const resp_value *argv, size_t argc,
                           resp_buf *out)
{
    if (s == NULL || argv == NULL || out == NULL)
        return -1;
    return srv_hotkeys_command(s, argv, argc, out);
}

void server_set_requirepass(server *s, const char *pw)
{
    s->requirepass = pw;
}

void server_set_client_id_allocator(server *s, uint64_t first,
                                    uint64_t stride)
{
    if (s == NULL)
        return;
    s->next_client_id = first == 0 ? 1 : first;
    s->client_id_stride = stride == 0 ? 1 : stride;
}

void server_set_maxmemory(server *s, uint64_t bytes, int policy)
{
    int i;
    for (i = 0; i < s->ndbs; i++) {
        db *d = srv_select_db(s, i);
        d->maxmemory = bytes;
        d->maxmemory_policy = policy;
    }
}

int server_enable_tiering(server *s, const char *dir, const char *logname,
                          uint64_t max_disk_bytes)
{
    char path[1088];
    tier_store *tier = NULL;
    int i;

    if (s == NULL || dir == NULL || logname == NULL || dir[0] == '\0' ||
        logname[0] == '\0')
        return -1;
    snprintf(path, sizeof(path), "%s/%s", dir, logname);
    if (tier_open(&tier, path, max_disk_bytes) != 0)
        return -1;
    for (i = 0; i < s->ndbs; i++) {
        db *d = srv_select_db(s, i);
        db_set_tier(d, tier, i);
        snprintf(d->tier_dir, sizeof(d->tier_dir), "%s", dir);
        d->tier_max_disk_bytes = max_disk_bytes;
        d->tier_enabled = 1;
    }
    s->tier = tier;
    return 0;
}

void server_set_proto_max_request_bytes(server *s, size_t bytes)
{
    if (bytes > 0)
        s->proto_max_request_bytes = bytes;
}

void server_set_repl_max_snapshot_bytes(server *s, size_t bytes)
{
    if (bytes > 0)
        s->repl_max_snapshot_bytes = bytes;
}

void server_set_snapshot_path(server *s, const char *path)
{
    s->db.snapshot_path = path;
}

int server_load_snapshot(server *s)
{
    if (s->db.snapshot_path == NULL)
        return -1;
    return snapshot_load_multi(s, srv_select_db, s->ndbs,
                               s->db.snapshot_path, pal_wall_ms());
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
    if (srv_proactor(s) ||
        s->bus_listen_fd != PAL_SOCKET_INVALID)
        return; /* unsupported backend, or already listening */
    s->bus_listen_fd = pal_tcp_listen("0.0.0.0",
                                      (uint16_t)(s->db.cluster_port + 10000),
                                      511, NULL);
    if (s->bus_listen_fd != PAL_SOCKET_INVALID)
        (void)pal_loop_add(s->loop, s->bus_listen_fd, 1, 0, NULL);
}

void server_set_cluster_announce(server *s, const char *ip, uint16_t port)
{
    if (s == NULL)
        return;
    if (ip != NULL && ip[0] != '\0')
        snprintf(s->db.cluster_ip, sizeof(s->db.cluster_ip), "%s", ip);
    s->db.cluster_port = port;
}

void server_set_cluster_control(server *s, int on)
{
    if (s != NULL)
        s->cluster_control = on;
}

void server_set_repl_snapshot_hook(server *s, server_repl_snapshot_fn fn,
                                   void *ctx)
{
    if (s == NULL)
        return;
    s->repl_snapshot_fn = fn;
    s->repl_snapshot_ctx = ctx;
}

void server_set_repl_snapshot_serialize_hook(
    server *s, server_repl_snapshot_serialize_fn fn, void *ctx)
{
    if (s == NULL)
        return;
    s->repl_snapshot_serialize_fn = fn;
    s->repl_snapshot_serialize_ctx = ctx;
}

void server_set_repl_centralized(server *s, int on)
{
    if (s != NULL)
        s->repl_centralized = on;
}

void server_set_repl_stream_forward(server *s, server_repl_stream_fn fn,
                                    void *ctx)
{
    if (s == NULL)
        return;
    s->repl_stream_fn = fn;
    s->repl_stream_ctx = ctx;
}

void server_repl_stream_forward(server *s, int db_index, const char *raw,
                                size_t raw_len)
{
    if (s != NULL && s->repl_centralized &&
        s->role == SESSION_ROLE_MASTER && s->repl_stream_fn != NULL)
        s->repl_stream_fn(s->repl_stream_ctx, db_index, raw, raw_len);
}

int server_repl_active(const server *s)
{
    return s != NULL && (s->repl.connected_slaves != 0 ||
                         s->backlog_used != 0);
}

const repl_info *server_repl_info(const server *s)
{
    return s != NULL ? &s->repl : NULL;
}

void server_repl_stream_append(server *s, const char *raw, size_t raw_len)
{
    size_t i;
    if (s == NULL || raw == NULL || raw_len == 0)
        return;
    if (!server_repl_active(s))
        return;
    repl_backlog_append(&s->backlog, raw, raw_len);
    s->repl.offset = s->backlog.offset;
    if (s->repl.connected_slaves == 0)
        return;
    for (i = 0; i < s->nconns; i++) {
        conn *rc = s->conns[i];
        if (rc->is_replica) {
            if (resp_buf_reserve(&rc->out, raw_len) != 0) {
                rc->close_after_send = 1;
                continue;
            }
            memcpy(rc->out.data + rc->out.len, raw, raw_len);
            rc->out.len += raw_len;
        }
    }
}

void server_repl_stream_append_db(server *s, int db_index, const char *raw,
                                  size_t raw_len)
{
    char sel[48];
    int n;
    if (s == NULL || raw == NULL || raw_len == 0 ||
        db_index < 0 || db_index > 999999999)
        return;
    if (!server_repl_active(s))
        return;
    if (db_index != s->repl_stream_db) {
        n = snprintf(sel, sizeof(sel), "*2\r\n$6\r\nSELECT\r\n$%d\r\n%d\r\n",
                     db_index, db_index);
        if (n < 0 || (size_t)n >= sizeof(sel))
            return;
        server_repl_stream_append(s, sel, (size_t)n);
        s->repl_stream_db = db_index;
    }
    server_repl_stream_append(s, raw, raw_len);
}

void server_set_slowlog_threshold(server *s, uint64_t usec)
{
    s->slowlog_threshold_us = usec;
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
        snapshot_save_multi(s, srv_select_db, s->ndbs,
                            s->db.snapshot_path) == 0)
        s->db.last_save = pal_wall_ms() / 1000;
}

void server_destroy(server *s)
{
    if (s == NULL)
        return;
    if (s->db.cluster_enabled && s->cluster_control) {
        s->nodes_dirty = 1;
        cluster_nodes_save(s);
    }
    while (s->bus != NULL)
        bus_conn_free(s, s->bus);
    if (s->bus_listen_fd != PAL_SOCKET_INVALID) {
        pal_loop_del(s->loop, s->bus_listen_fd);
        pal_close(s->bus_listen_fd);
    }
    /* proactor teardown before any conn is freed: closing the IOCP port
     * discards pending completions, and closing the io_uring fd cancels
     * every in-flight request synchronously -- afterwards no kernel
     * operation can still write into a conn buffer being freed below */
    pal_iocp_free(s->iocp);
    pal_iouring_free(s->iou);
    s->iocp = NULL;
    s->iou = NULL;
    server_free_connections(s);
    srv_slowlog_reset(s);
    free(s->slowlog);
    s->slowlog = NULL;
    s->slowlog_cap = 0;
    if (s->loop != NULL && s->listen_fd != PAL_SOCKET_INVALID &&
        !srv_proactor(s))
        pal_loop_del(s->loop, s->listen_fd);
    pal_close(s->listen_fd);
    if (s->tls_listen_fd != PAL_SOCKET_INVALID) {
        pal_loop_del(s->loop, s->tls_listen_fd);
        pal_close(s->tls_listen_fd);
    }
    pal_tls_ctx_free(s->tls_ctx);
    pal_tls_ctx_free(s->repl_tls_ctx);
    if (s->loop != NULL)
        pal_loop_free(s->loop);
    rh_destroy(&s->channels);
    rh_destroy(&s->schannels);
    rh_destroy(&s->patterns);
    aof_close(s->aof);
    repl_backlog_free(&s->backlog);
    resp_buf_free(&s->prop_buf);
    db_destroy(&s->db);
    if (s->extra_dbs != NULL) {
        int i;
        for (i = 0; i < s->ndbs - 1; i++)
            db_destroy(&s->extra_dbs[i]);
        free(s->extra_dbs);
    }
    tier_close(s->tier);
    free(s->hotkeys_entries);
    free(s->backup_path);
    free(s->backup_aof_path);
    buf_pool_destroy(&s->pool);
    free(s);
}

void server_free_connections(server *s)
{
    size_t i;
    if (s == NULL)
        return;
    for (i = 0; i < s->nconns; i++)
        conn_free(s->conns[i]);
    free(s->conns);
    s->conns = NULL;
    s->nconns = 0;
    for (i = 0; i < s->nzombies; i++)
        conn_free(s->zombies[i]);
    free(s->zombies);
    s->zombies = NULL;
    s->nzombies = 0;
}

/* ------------------------------------------------------------------ */
/* connection lifecycle                                               */
/* ------------------------------------------------------------------ */

void server_close_listener(server *s)
{
    if (s->listen_fd == PAL_SOCKET_INVALID)
        return;
    if (s->loop != NULL && !srv_proactor(s))
        (void)pal_loop_del(s->loop, s->listen_fd);
    pal_close(s->listen_fd);
    s->listen_fd = PAL_SOCKET_INVALID;
}

int server_adopt_fd(server *s, pal_socket_t fd)
{
    conn *c;
    if (srv_proactor(s)) {
        /* completion model: register the conn and post the first recv */
        server_accept_pro(s, fd);
        return 0;
    }
    (void)pal_set_tcp_nodelay(fd, 1);
    if (pal_set_nonblocking(fd, 1) != 0) {
        pal_close(fd);
        return -1;
    }
    c = conn_create(s, fd);
    if (c == NULL) {
        pal_close(fd);
        return -1;
    }
    if (s->nconns == s->cap) {
        size_t ncap = s->cap == 0 ? 16 : s->cap * 2;
        conn **nc = (conn **)realloc(s->conns, ncap * sizeof(*nc));
        if (nc == NULL) {
            conn_free(c);
            return -1;
        }
        s->conns = nc;
        s->cap = ncap;
    }
    s->conns[s->nconns++] = c;
    if (pal_loop_add(s->loop, fd, 1, 0, c) != 0) {
        conn_close(s, s->nconns - 1);
        return -1;
    }
    return 0;
}

int server_adopt_fd_tls(server *s, pal_socket_t fd)
{
    conn *c;
    pal_tls *tls;
    if (s->tls_ctx == NULL)
        return -1;
    (void)pal_set_tcp_nodelay(fd, 1);
    if (pal_set_nonblocking(fd, 1) != 0) {
        pal_close(fd);
        return -1;
    }
    tls = pal_tls_new(s->tls_ctx, fd);
    if (tls == NULL) {
        pal_close(fd);
        return -1;
    }
    c = conn_create(s, fd);
    if (c == NULL) {
        pal_tls_free(tls);
        pal_close(fd);
        return -1;
    }
    c->tls = tls;
    c->tls_handshaking = 1;
    if (s->nconns == s->cap) {
        size_t ncap = s->cap == 0 ? 16 : s->cap * 2;
        conn **nc = (conn **)realloc(s->conns, ncap * sizeof(*nc));
        if (nc == NULL) {
            conn_free(c);
            return -1;
        }
        s->conns = nc;
        s->cap = ncap;
    }
    s->conns[s->nconns++] = c;
    if (pal_loop_add(s->loop, fd, 1, 1, c) != 0) {
        conn_close(s, s->nconns - 1);
        return -1;
    }
    return 0;
}

int server_set_wakeup(server *s, pal_socket_t fd, void (*cb)(void *ctx),
                      void *ctx)
{
    if (srv_proactor(s)) {
        /* no fd registration: kicks arrive as WAKEUP completions posted
         * via server_wakeup_kick() */
        s->wakeup_cb = cb;
        s->wakeup_ctx = ctx;
        return 0;
    }
    if (pal_loop_add(s->loop, fd, 1, 0, NULL) != 0)
        return -1;
    s->wakeup_fd = fd;
    s->wakeup_cb = cb;
    s->wakeup_ctx = ctx;
    return 0;
}

void server_wakeup_kick(server *s)
{
    if (srv_proactor(s) && (s->iocp != NULL || s->iou != NULL))
        (void)pro_kick(s);
}

void server_set_route(server *s, server_route_fn fn,
                      server_route_flush_fn flush_fn,
                      void (*mt_state_free)(void *ctx, void *st), void *ctx)
{
    s->route_fn = fn;
    s->route_flush_fn = flush_fn;
    s->mt_state_free = mt_state_free;
    s->route_ctx = ctx;
}

void server_set_mt_close(server *s, server_mt_close_fn fn)
{
    s->mt_close_fn = fn;
}

void *server_conn_mt_state(void *conn_ptr)
{
    return ((conn *)conn_ptr)->mt_state;
}

void server_conn_set_mt_state(void *conn_ptr, void *st)
{
    ((conn *)conn_ptr)->mt_state = st;
}

void server_conn_free_now(server *s, void *conn_ptr)
{
    conn *c = (conn *)conn_ptr;
    if (srv_proactor(s)) {
        if (c->pending_ops > 0) {
            /* overlapped ops still in flight: the loop frees the conn
             * when the last completion drains */
            c->zombie_mt_free = 1;
            return;
        }
        if (c->zombie) {
            /* drained already; drop it from the zombie list before free */
            size_t zi;
            for (zi = 0; zi < s->nzombies; zi++)
                if (s->zombies[zi] == c)
                    break;
            if (zi < s->nzombies) {
                s->zombies[zi] = s->zombies[s->nzombies - 1];
                s->nzombies--;
            }
        }
    }
    conn_free(c);
}

int server_conn_detach(server *s, void *conn_ptr)
{
    conn *c = (conn *)conn_ptr;
    size_t i;
    for (i = 0; i < s->nconns; i++)
        if (s->conns[i] == c)
            break;
    if (i == s->nconns)
        return -1;
    s->conns[i] = s->conns[s->nconns - 1];
    s->nconns--;
    (void)pal_loop_del(s->loop, c->fd);
    return 0;
}

void server_conn_rehome(server *s, void *conn_ptr)
{
    conn *c = (conn *)conn_ptr;
    c->srv = s;
    /* rewire the session to the new worker's server, preserving the
     * selected database (SELECT state travels with the connection) */
    c->sess->d = srv_select_db(s, c->sess->db_index);
    c->sess->sel_ctx = s;
    c->sess->ps_ctx = s;
    c->sess->shutdown_ctx = s;
    c->sess->sync_ctx = s;
    c->sess->psync_ctx = s;
    c->sess->replicaof_ctx = s;
    c->sess->reset_ctx = s;
    c->sess->cluster_ctx = s;
    c->sess->client_ctx = s;
    c->sess->slowlog_ctx = s;
    c->sess->bgrewriteaof_ctx = s;
    c->sess->hotkeys_ctx = s;
    c->sess->hotkeys_command = srv_hotkeys_command;
    c->sess->backup_ctx = s;
    c->sess->backup_command = srv_backup_command;
    c->sess->aof_ctx = s;
    c->sess->repl = &s->repl;
    c->sess->role = &s->role;
}

int server_conn_adopt(server *s, void *conn_ptr)
{
    conn *c = (conn *)conn_ptr;
    if (s->nconns == s->cap) {
        size_t ncap = s->cap == 0 ? 16 : s->cap * 2;
        conn **nc = (conn **)realloc(s->conns, ncap * sizeof(*nc));
        if (nc == NULL)
            return -1;
        s->conns = nc;
        s->cap = ncap;
    }
    s->conns[s->nconns++] = c;
    if (pal_loop_add(s->loop, c->fd, 1, c->want_write, c) != 0) {
        s->nconns--; /* c was appended at the tail */
        return -1;
    }
    /* bytes already in the receive buffer (including the command that
     * triggered the migration) are processed right away */
    if (c->rlen > 0) {
        int pr = conn_process_input(s, c);
        if (pr < 0) {
            size_t idx;
            for (idx = 0; idx < s->nconns; idx++)
                if (s->conns[idx] == c)
                    break;
            if (idx < s->nconns)
                conn_close(s, idx);
            return -1;
        }
        if (c->out.len > 0)
            (void)conn_flush(s, c);
    }
    return 0;
}

int server_conn_out_append(server *s, void *conn_ptr, const char *data,
                           size_t len)
{
    conn *c = (conn *)conn_ptr;
    (void)s;
    if (resp_buf_reserve(&c->out, len) != 0) {
        c->close_after_send = 1;
        return -1;
    }
    memcpy(c->out.data + c->out.len, data, len);
    c->out.len += len;
    return 0;
}

int server_conn_flush(server *s, void *conn_ptr)
{
    if (srv_proactor(s)) {
        /* completion model: post an overlapped send if none is in flight */
        kick_flush(s, (conn *)conn_ptr);
        return 0;
    }
    return conn_flush(s, (conn *)conn_ptr);
}

static void server_accept(server *s, pal_socket_t lfd, int use_tls)
{
    /* The listener socket is blocking, but a readiness event guarantees at
     * least one pending connection, so this accept does not block. */
    pal_socket_t fd = pal_accept(lfd);
    conn *c;
    pal_tls *tls = NULL;
    if (fd == PAL_SOCKET_INVALID)
        return;
    (void)pal_set_tcp_nodelay(fd, 1);
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
    if (srv_proactor(s)) {
        int mt_kept = 0;
        /* the routing layer may keep the conn until its pending work
         * drains; on the proactor the free additionally waits for
         * outstanding overlapped ops to drain (whichever comes last
         * frees) */
        if (s->mt_close_fn != NULL)
            mt_kept = s->mt_close_fn(s->route_ctx, c) != 0;
        if (c->pending_ops > 0) {
            pro_close(s, c->fd);
            c->zombie = 1;
            c->zombie_mt_free = !mt_kept;
            zombie_push(s, c);
            return;
        }
        pal_close(c->fd);
        if (mt_kept) {
            c->fd = PAL_SOCKET_INVALID;
            return; /* freed later via server_conn_free_now */
        }
        conn_free(c);
        return;
    }
    pal_loop_del(s->loop, c->fd);
    if (s->mt_close_fn != NULL && s->mt_close_fn(s->route_ctx, c) != 0) {
        /* the routing layer keeps the conn (zombie) until its pending work
         * drains; the fd is closed now, the conn is freed later */
        pal_close(c->fd);
        c->fd = PAL_SOCKET_INVALID;
        return;
    }
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
    s->db.cluster_node_timeout_ms = ms;
}

void server_set_bus_protocol(server *s, int proto)
{
    s->bus_protocol = proto;
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
    if (s->bus_protocol == SERVER_BUS_PROTOCOL_REDIS) {
        /* RCM2 type constants (PING=1/PONG=2/MEET=3) -> redbus (0/1/2) */
        static const int rmap[4] = {-1, REDBUS_TYPE_PING, REDBUS_TYPE_PONG,
                                    REDBUS_TYPE_MEET};
        if (redbus_build_frame(&s->db, rmap[type], &bc->out) != 0)
            return;
    } else {
        if (cluster_bus_build_frame(&s->db, type, &bc->out) != 0)
            return;
    }
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
    (void)pal_get_peer_ip(fd, bc->peer_ip, sizeof(bc->peer_ip));
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
    (void)pal_set_tcp_nodelay(fd, 1);
    if (pal_set_nonblocking(fd, 1) != 0) {
        pal_close(fd);
        return NULL;
    }
    bc = bus_conn_new(fd, 1);
    if (bc == NULL) {
        pal_close(fd);
        return NULL;
    }
    snprintf(bc->peer_ip, sizeof(bc->peer_ip), "%s", ip);
    bus_conn_add(s, bc);
    return bc;
}

static void srv_spublish_bus(void *ctx, const char *ch, size_t chlen,
                             const char *msg, size_t mlen)
{
    server *s = (server *)ctx;
    bus_conn *bc;
    /* Broadcast to every bus peer. Redis propagates a shard publish only
     * within the channel's shard; ddup broadcasts to all peers instead
     * (documented divergence). Frames that would exceed the bus size cap
     * are dropped (local delivery already happened regardless). */
    if (chlen > SIZE_MAX - 8 || mlen > SIZE_MAX - 8 - chlen ||
        chlen + mlen + 8 > CLUSTER_MSG_MAX - REDBUS_HDR_LEN)
        return;
    for (bc = s->bus; bc != NULL; bc = bc->next) {
        int rc;
        if (s->bus_protocol == SERVER_BUS_PROTOCOL_REDIS)
            rc = redbus_build_publish(&s->db, REDBUS_TYPE_PUBLISHSHARD, ch,
                                      chlen, msg, mlen, &bc->out);
        else
            rc = cluster_bus_build_publish(&s->db, ch, chlen, msg, mlen,
                                           &bc->out);
        if (rc != 0)
            continue;
        bus_flush(s, bc);
    }
}

/* Intercept a bus PUBLISH frame before the node-table handlers: deliver
 * to local subscribers directly. Returns 1 when the frame was consumed
 * (delivered, or dropped as malformed), 0 when it is not a publish
 * frame. redbus type 4 feeds regular subscribers ("message"), redbus
 * type 10 and RCM2 CLUSTER_MSG_PUBLISH feed shard ones ("smessage"). */
static int bus_try_publish(server *s, const char *frame, uint32_t totlen)
{
    if (s->bus_protocol == SERVER_BUS_PROTOCOL_REDIS) {
        uint16_t wt;
        uint32_t cl, ml;
        if (totlen < 14)
            return 0;
        wt = (uint16_t)(((uint16_t)(uint8_t)frame[12] << 8) |
                        (uint16_t)(uint8_t)frame[13]);
        if (wt != REDBUS_TYPE_PUBLISH && wt != REDBUS_TYPE_PUBLISHSHARD)
            return 0;
        if (totlen < REDBUS_HDR_LEN + 8)
            return 1; /* malformed: drop */
        cl = ((uint32_t)(uint8_t)frame[REDBUS_HDR_LEN] << 24) |
             ((uint32_t)(uint8_t)frame[REDBUS_HDR_LEN + 1] << 16) |
             ((uint32_t)(uint8_t)frame[REDBUS_HDR_LEN + 2] << 8) |
             (uint32_t)(uint8_t)frame[REDBUS_HDR_LEN + 3];
        ml = ((uint32_t)(uint8_t)frame[REDBUS_HDR_LEN + 4] << 24) |
             ((uint32_t)(uint8_t)frame[REDBUS_HDR_LEN + 5] << 16) |
             ((uint32_t)(uint8_t)frame[REDBUS_HDR_LEN + 6] << 8) |
             (uint32_t)(uint8_t)frame[REDBUS_HDR_LEN + 7];
        if ((uint64_t)REDBUS_HDR_LEN + 8 + cl + ml > totlen)
            return 1; /* malformed: drop */
        if (wt == REDBUS_TYPE_PUBLISHSHARD)
            srv_spublish(s, frame + REDBUS_HDR_LEN + 8, cl,
                         frame + REDBUS_HDR_LEN + 8 + cl, ml);
        else
            srv_publish(s, frame + REDBUS_HDR_LEN + 8, cl,
                        frame + REDBUS_HDR_LEN + 8 + cl, ml);
        return 1;
    }
    {
        uint16_t wt = (uint16_t)((uint16_t)(uint8_t)frame[8] |
                                 ((uint16_t)(uint8_t)frame[9] << 8));
        uint32_t cl, ml;
        if (wt != CLUSTER_MSG_PUBLISH)
            return 0;
        if (totlen < 18)
            return 1; /* malformed: drop */
        cl = (uint32_t)(uint8_t)frame[10] |
             ((uint32_t)(uint8_t)frame[11] << 8) |
             ((uint32_t)(uint8_t)frame[12] << 16) |
             ((uint32_t)(uint8_t)frame[13] << 24);
        if ((uint64_t)18 + cl > totlen)
            return 1; /* malformed: drop */
        ml = (uint32_t)(uint8_t)frame[14 + cl] |
             ((uint32_t)(uint8_t)frame[15 + cl] << 8) |
             ((uint32_t)(uint8_t)frame[16 + cl] << 16) |
             ((uint32_t)(uint8_t)frame[17 + cl] << 24);
        if ((uint64_t)18 + cl + ml > totlen)
            return 1; /* malformed: drop */
        srv_spublish(s, frame + 14, cl, frame + 18 + cl, ml);
        return 1;
    }
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
    /* parse complete frames (totlen endianness follows the protocol) */
    while (bc->rlen >= 10) {
        uint32_t totlen;
        int rc;
        if (s->bus_protocol == SERVER_BUS_PROTOCOL_REDIS)
            totlen = ((uint32_t)(uint8_t)bc->rbuf[4] << 24) |
                     ((uint32_t)(uint8_t)bc->rbuf[5] << 16) |
                     ((uint32_t)(uint8_t)bc->rbuf[6] << 8) |
                     (uint32_t)(uint8_t)bc->rbuf[7];
        else
            totlen = (uint32_t)(uint8_t)bc->rbuf[4] |
                     ((uint32_t)(uint8_t)bc->rbuf[5] << 8) |
                     ((uint32_t)(uint8_t)bc->rbuf[6] << 16) |
                     ((uint32_t)(uint8_t)bc->rbuf[7] << 24);
        if (totlen > CLUSTER_MSG_MAX || totlen < 10) {
            bus_conn_free(s, bc);
            return;
        }
        if (bc->rlen < totlen)
            break; /* incomplete frame */
        if (bus_try_publish(s, bc->rbuf, totlen)) {
            memmove(bc->rbuf, bc->rbuf + totlen, bc->rlen - totlen);
            bc->rlen -= totlen;
            continue;
        }
        if (s->bus_protocol == SERVER_BUS_PROTOCOL_REDIS)
            rc = redbus_handle_frame(&s->db, bc->rbuf, totlen, &bc->out,
                                     pal_wall_ms(),
                                     bc->peer_ip[0] ? bc->peer_ip : NULL);
        else
            rc = cluster_bus_handle_frame(&s->db, bc->rbuf, totlen,
                                          &bc->out, pal_wall_ms());
        if (rc != 0) {
            bus_conn_free(s, bc);
            return;
        }
        s->nodes_dirty = 1;
        if (s->db.fail_broadcast_id[0] != '\0')
            cluster_broadcast_fail(s); /* quorum reached via this gossip */
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

/* broadcast a FAIL frame for db.fail_broadcast_id on every bus conn */
static void cluster_broadcast_fail(server *s)
{
    bus_conn *bc;
    for (bc = s->bus; bc != NULL; bc = bc->next) {
        int rc;
        if (s->bus_protocol == SERVER_BUS_PROTOCOL_REDIS)
            rc = redbus_build_fail(&s->db, s->db.fail_broadcast_id, &bc->out);
        else
            rc = cluster_bus_build_fail(&s->db, s->db.fail_broadcast_id,
                                        &bc->out);
        if (rc != 0)
            continue;
        bus_flush(s, bc);
    }
    s->db.fail_broadcast_id[0] = '\0';
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
            n->flags |= CLUSTER_NODE_DISCONNECTED | CLUSTER_NODE_PFAIL;
            s->nodes_dirty = 1;
            /* fresh local suspicion may complete a quorum whose reports
             * arrived while the node was still reachable from here */
            (void)cluster_mark_fail_if_quorum(&s->db, n, now_ms);
        }
    }
    if (s->db.fail_broadcast_id[0] != '\0')
        cluster_broadcast_fail(s); /* quorum completed on local suspicion */

    /* failover: a slave whose master is objectively FAIL (quorum
     * confirmed -- mere PFAIL suspicion is not enough) schedules its
     * promotion (election delay = node timeout + 500 ms, fixed instead
     * of Redis's randomized delay: documented) */
    {
        cluster_node *me = cluster_myself(&s->db);
        cluster_node *m =
            me != NULL && !(me->master_id[0] == '-' &&
                            me->master_id[1] == '\0')
                ? cluster_node_find(&s->db, me->master_id)
                : NULL;
        if (me != NULL && (me->flags & CLUSTER_NODE_SLAVE) && m != NULL &&
            (m->flags & (CLUSTER_NODE_MASTER | CLUSTER_NODE_FAIL)) ==
                (CLUSTER_NODE_MASTER | CLUSTER_NODE_FAIL) &&
            s->failover_deadline_ms == 0) {
            uint32_t sl;
            for (sl = 0; sl < 16384; sl++)
                if (cluster_slots_get(m->slots, sl)) {
                    s->failover_deadline_ms =
                        now_ms + s->node_timeout_ms + 500;
                    break;
                }
        }
    }
}

/* broadcast a FAILOVER_AUTH_REQUEST on every bus conn (redis mode) */
static void cluster_request_votes(server *s)
{
    bus_conn *bc;
    for (bc = s->bus; bc != NULL; bc = bc->next) {
        if (redbus_build_auth_request(&s->db, s->db.failover_req_epoch,
                                      &bc->out) != 0)
            continue;
        bus_flush(s, bc);
    }
}

/* promote myself when the failover election delay has elapsed and the
 * master is still down; announce the claims immediately. In redis bus
 * mode this runs the vote round instead: request on the first expiry,
 * promote on majority of slot-serving masters at the second. */
static void cluster_failover_check(server *s, uint64_t now_ms)
{
    cluster_node *me, *m;
    db *d = &s->db;
    if (s->failover_deadline_ms == 0 || now_ms < s->failover_deadline_ms)
        return;
    s->failover_deadline_ms = 0;
    me = cluster_myself(d);
    m = me != NULL && !(me->master_id[0] == '-' && me->master_id[1] == '\0')
            ? cluster_node_find(d, me->master_id)
            : NULL;
    if (me == NULL || !(me->flags & CLUSTER_NODE_SLAVE) || m == NULL ||
        !(m->flags & CLUSTER_NODE_FAIL)) {
        d->failover_req_epoch = 0; /* master recovered (or we promoted) */
        return;
    }
    if (s->bus_protocol == SERVER_BUS_PROTOCOL_REDIS) {
        if (d->failover_req_epoch == 0) {
            /* start the election: bump the epoch and request votes */
            d->failover_req_epoch = cluster_next_epoch(d);
            d->failover_ack_mask = 0;
            d->failover_ack_count = 0;
            cluster_request_votes(s);
            s->failover_deadline_ms = now_ms + s->node_timeout_ms;
            return;
        }
        /* election window closed: promote on a majority of slot-serving
         * masters, else retry shortly */
        {
            int masters = 0, i;
            uint32_t sl;
            for (i = 0; i < d->nnodes; i++) {
                cluster_node *n = &d->nodes[i];
                if (!(n->flags & CLUSTER_NODE_MASTER) ||
                    (n->flags & CLUSTER_NODE_MYSELF))
                    continue;
                for (sl = 0; sl < 16384; sl++)
                    if (cluster_slots_get(n->slots, sl))
                        break;
                if (sl < 16384)
                    masters++;
            }
            if (d->failover_ack_count > masters / 2) {
                d->failover_req_epoch = 0;
                if (cluster_failover_promote(d)) {
                    srv_replicaof(s, NULL, 0); /* stop data replication */
                    s->nodes_dirty = 1;
                    cluster_gossip_round(s);
                }
            } else {
                d->failover_req_epoch = 0;
                s->failover_deadline_ms = now_ms + 500; /* retry election */
            }
            return;
        }
    }
    if (cluster_failover_promote(&s->db)) {
        srv_replicaof(s, NULL, 0); /* stop data replication */
        s->nodes_dirty = 1;
        cluster_gossip_round(s); /* announce the claims now */
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
            if (pal_file_close(f) == 0 && pal_file_rename(tmp, s->nodes_path) == 0) {
                s->nodes_dirty = 0;
                s->last_cluster_changes = s->db.cluster_changes;
            } else {
                pal_file_unlink(tmp);
            }
        } else {
            pal_file_close(f);
            pal_file_unlink(tmp);
        }
    }
    resp_buf_free(&buf);
}

#ifdef DDUP_TESTING
void server_test_cluster_nodes_save(server *s)
{
    cluster_nodes_save(s);
}
#endif

/* Parse and execute all complete commands in the conn recv buffer and
 * compact consumed bytes. Returns 0 ok, -1 protocol error (caller closes),
 * 2 when the connection was migrated to another worker by the route hook
 * (caller must not touch it again). */
static int conn_process_input(server *s, conn *c)
{
    size_t off = 0;
    if (c->sess->quit) {
        /* closing after QUIT: discard anything still arriving */
        c->rlen = 0;
        return 0;
    }
    while (off < c->rlen) {
        resp_value v;
        ptrdiff_t used =
            resp_parse(c->rbuf + off, c->rlen - off, &v, &c->arena);
        if (used == 0)
            break; /* incomplete command */
        if (used < 0 || v.type != RESP_ARRAY || v.is_null)
            return -1;
        if (srv_aof_reject(s, c->sess, v.items, v.count, &c->out)) {
            arena_reset(&c->arena);
            off += (size_t)used;
            continue;
        }
        if (s->route_fn != NULL) {
            int rr = s->route_fn(s->route_ctx, c, c->sess, v.items, v.count,
                                 c->rbuf + off, (size_t)used, &c->out);
            if (rr == 2)
                return 2; /* migrated: the current command is unconsumed */
            if (rr != 0) {
                arena_reset(&c->arena);
                off += (size_t)used;
                if (c->sess->quit)
                    break; /* QUIT handled locally by the routing layer */
                if (c->sess->blocked)
                    break; /* routed blocking command owns this connection */
                continue; /* routed / handled by the mt layer */
            }
        }
        c->sess->raw_cmd = c->rbuf + off;
        c->sess->raw_cmd_len = (size_t)used;
        session_execute(c->sess, v.items, v.count, &c->out);
        c->sess->raw_cmd = NULL;
        c->sess->raw_cmd_len = 0;
        arena_reset(&c->arena);
        off += (size_t)used;
        if (c->sess->blocked)
            break;
        if (c->sess->quit)
            break;
    }
    if (c->sess->quit) {
        /* QUIT: the reply goes out, then the connection closes (same
         * send-then-close mechanism as a protocol error); anything
         * pipelined after QUIT is discarded */
        c->close_after_send = 1;
        off = 0;
        c->rlen = 0;
    }
    if (s->route_flush_fn != NULL)
        s->route_flush_fn(s->route_ctx, c);
    if (off > 0) {
        memmove(c->rbuf, c->rbuf + off, c->rlen - off);
        c->rlen -= off;
    }
    if (c->rlen >= s->proto_max_request_bytes)
        return -1;
    return 0;
}

/* Proactor backend (IOCP / io_uring op-mode): post at most one overlapped
 * send per conn; no-op while one is in flight (its completion re-kicks).
 * The reply buffer is DETACHED, not copied (Phase 34): out's allocation
 * moves to the send role and out restarts empty, so a reply is copied
 * exactly once (value -> out) even for overlapped IO -- appends while a
 * send is in flight simply grow the fresh out. The detached buffer is
 * returned to the pool once its last byte is confirmed sent. */
static void kick_flush(server *s, conn *c)
{
    size_t n;
    if (c->send_outstanding)
        return;
    if (c->sbuf == NULL) {
        size_t old_cap;
        if (c->out.len == 0)
            return;
        /* detach: out's buffer becomes the send buffer */
        old_cap = c->out.pool != NULL ? c->out.pool_size : c->out.cap;
        c->sbuf = c->out.data;
        c->sbuf_len = c->out.len;
        c->sbuf_sent = 0;
        c->sbuf_pool_size = c->out.pool != NULL ? c->out.pool_size : 0;
        c->out.data = NULL;
        c->out.len = 0;
        c->out.cap = 0;
        c->out.pool_size = 0;
        /* warm spare: hand out a same-size buffer right away, otherwise
         * the next batch would re-climb the 256B..128KB doubling chain
         * (a memcpy per doubling) -- that regrowth costs more than the
         * copy this detach just eliminated */
        if (c->out.pool != NULL) {
            size_t actual;
            char *p = (char *)buf_pool_get(c->out.pool, old_cap, &actual);
            if (p != NULL) {
                c->out.data = p;
                c->out.cap = actual;
                c->out.pool_size = actual;
            }
        }
    }
    n = c->sbuf_len - c->sbuf_sent;
    if (n > IOCP_SEND_CHUNK)
        n = IOCP_SEND_CHUNK;
    s->io.writes++;
    if (s->iou_send_zc && c->send_fixed_id < 0 && n <= IOCP_SEND_CHUNK) {
        void *fixed = pal_iouring_sbuf_acquire(s->iou, &c->send_fixed_id);
        if (fixed != NULL) {
            memcpy(fixed, c->sbuf + c->sbuf_sent, n);
            if (pal_iouring_send_zc_fixed(s->iou, c->fd,
                                          c->send_fixed_id, 0, n, c) >= 0) {
                c->send_zc_notif_pending = 1;
                c->send_outstanding = 1;
                c->pending_ops++;
                return;
            }
            pal_iouring_sbuf_release(s->iou, c->send_fixed_id);
            c->send_fixed_id = -1;
        }
    }
    if (pro_send(s, c->fd, c->sbuf + c->sbuf_sent, n, c) >= 0) {
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

/* Arm/rearm the multishot recv of a live conn (io_uring ms mode only);
 * one armed request counts as one pending op until its final CQE. */
static void iou_ms_rearm(server *s, conn *c, size_t idx)
{
    c->pending_ops++;
    s->io.reads++;
    if (pal_iouring_recv_ms(s->iou, c->fd, c) < 0) {
        c->pending_ops--;
        conn_close(s, idx);
    }
}

/* Accept a connection on the proactor backend and post its first recv. */
static void server_accept_pro(server *s, pal_socket_t fd)
{
    conn *c;
    (void)pal_set_tcp_nodelay(fd, 1);
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
    if (s->iou_ms) {
        iou_ms_rearm(s, c, s->nconns - 1);
        return;
    }
    c->pending_ops++;
    s->io.reads++;
    if (pro_recv(s, c->fd, c->rbuf, c->rcap, c) < 0) {
        c->pending_ops--;
        conn_close(s, s->nconns - 1);
    }
}

static int server_run_once_proactor(server *s, int timeout_ms)
{
    /* backend-native completions normalized into one shape; op kinds have
     * identical values in pal_iocp_op and pal_iouring_ev (asserted above),
     * so the body below compares against PAL_IOCP_* for either backend */
    typedef struct pro_event {
        void *userdata;
        int op;
        pal_socket_t fd;
        ptrdiff_t bytes;
        int err;     /* errno when bytes == -1 (io_uring), else 0 */
        int buf_id;  /* io_uring pbuf ring slot, -1 otherwise */
        int op_done; /* 0 = multishot request still armed (IOCP: always 1) */
        int notif;   /* SEND_ZC buffer-release notification */
    } pro_event;
    pro_event evs[SERVER_MAX_EVENTS];
    int nev;
    int i;

    if (s->backend == SERVER_BACKEND_IOCP) {
        pal_iocp_event raw[SERVER_MAX_EVENTS];
        nev = pal_iocp_wait(s->iocp, raw, SERVER_MAX_EVENTS, timeout_ms);
        for (i = 0; i < nev; i++) {
            evs[i].userdata = raw[i].userdata;
            evs[i].op = (int)raw[i].op;
            evs[i].fd = raw[i].fd;
            evs[i].bytes = raw[i].bytes;
            evs[i].err = 0;
            evs[i].buf_id = -1;
            evs[i].op_done = 1;
            evs[i].notif = 0;
        }
    } else {
        pal_iouring_event raw[SERVER_MAX_EVENTS];
        nev = pal_iouring_wait(s->iou, raw, SERVER_MAX_EVENTS, timeout_ms);
        for (i = 0; i < nev; i++) {
            evs[i].userdata = raw[i].userdata;
            evs[i].op = (int)raw[i].op;
            evs[i].fd = raw[i].fd;
            evs[i].bytes = raw[i].bytes;
            evs[i].err = raw[i].err;
            evs[i].buf_id = raw[i].buf_id;
            evs[i].op_done = raw[i].op_done;
            evs[i].notif = raw[i].notif;
        }
    }
    if (nev <= 0) {
        srv_aof_flush(s);
        return nev;
    }
    s->io.events += (uint64_t)nev;

    for (i = 0; i < nev; i++) {
        pro_event *ev = &evs[i];
        conn *c;
        size_t idx;

        if (ev->op == PAL_IOCP_WAKEUP) {
            /* mt task-queue kick: drain via the registered callback */
            if (s->wakeup_cb != NULL)
                s->wakeup_cb(s->wakeup_ctx);
            continue;
        }

        if (ev->op == PAL_IOCP_ACCEPT) {
            if (s->listen_fd != PAL_SOCKET_INVALID) {
                if (ev->fd != PAL_SOCKET_INVALID)
                    server_accept_pro(s, ev->fd);
                /* IOCP: replenishes the accept pool; io_uring op-mode:
                 * no-op while the multishot accept stays armed, re-arms
                 * it after termination */
                (void)pro_accept_post(s, s->listen_fd, NULL);
            } else if (ev->fd != PAL_SOCKET_INVALID) {
                /* listener already closed (mt worker): drop the straggler */
                pal_close(ev->fd);
            }
            continue;
        }

        c = (conn *)ev->userdata;
        if (c == NULL)
            continue;
        if (c->zombie) {
            /* drained op from a closed conn: free when nothing is left
             * and the owner (mt routing layer) released it. A multishot
             * CQE may still carry a ring buffer: hand it back even here,
             * and only the final CQE retires the pending op. */
            if (ev->buf_id >= 0)
                pal_iouring_recycle(s->iou, ev->buf_id);
            if (ev->notif && c->send_fixed_id >= 0) {
                pal_iouring_sbuf_release(s->iou, c->send_fixed_id);
                c->send_fixed_id = -1;
                c->send_zc_notif_pending = 0;
            }
            if (ev->op_done)
                c->pending_ops--;
            if (c->pending_ops <= 0 && c->zombie_mt_free) {
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
        if (ev->op_done &&
            !(ev->op == PAL_IOCP_SEND && c->send_zc_notif_pending &&
              !ev->notif))
            c->pending_ops--;

        if (ev->op == PAL_IOCP_RECV) {
            if (c->close_after_send) {
                if (ev->buf_id >= 0)
                    pal_iouring_recycle(s->iou, ev->buf_id);
                continue;
            }
            if (c->is_master_link) {
                /* replica master link: feed the link state machine and
                 * re-post the recv (never goes through conn_process_input) */
                if (ev->bytes <= 0) {
                    repl_link_close_transport(s, c); /* retry timer reconnects */
                    continue;
                }
                c->rlen += (size_t)ev->bytes;
                s->io.bytes_read += (uint64_t)ev->bytes;
                repl_link_feed(s, c);
                if (s->master_link != c)
                    continue; /* the feed closed the link */
                if (c->rcap - c->rlen < SERVER_RECV_CHUNK &&
                    c->rcap < c->srv->proto_max_request_bytes) {
                    if (conn_rbuf_grow(c) != 0) {
                        repl_link_close_transport(s, c);
                        continue;
                    }
                }
                c->pending_ops++;
                s->io.reads++;
                if (pro_recv(s, c->fd, c->rbuf + c->rlen,
                             c->rcap - c->rlen, c) < 0) {
                    c->pending_ops--;
                    repl_link_close_transport(s, c);
                }
                continue;
            }
            if (s->iou_ms) {
                /* multishot recv: the chunk sits in a provided-buffer ring
                 * slot; CQEs of one socket arrive in receive order (the
                 * request is a single sequential consumer of the socket
                 * receive queue), so appending in CQE order is exact */
                const char *chunk;
                if (ev->bytes < 0) {
                    if (ev->err == ENOBUFS) {
                        /* ring starvation ends the request (final CQE,
                         * already decremented above); buffers recycled by
                         * this same batch are back -- rearm, do NOT close */
                        iou_ms_rearm(s, c, idx);
                        continue;
                    }
                    conn_close(s, idx);
                    continue;
                }
                if (ev->bytes == 0) { /* orderly close (final CQE) */
                    conn_close(s, idx);
                    continue;
                }
                chunk = (const char *)pal_iouring_buf(s->iou, ev->buf_id);
                if (chunk == NULL) { /* corrupt buf_id: bail out */
                    conn_close(s, idx);
                    continue;
                }
                {
                    size_t copied = 0;
                    int process_error = 0;
                    while (copied < (size_t)ev->bytes) {
                        size_t avail = c->rcap - c->rlen;
                        size_t take;
                        int pr;
                        if (avail == 0) {
                            pr = conn_process_input(s, c);
                            if (pr != 0) {
                                process_error = 1;
                                break;
                            }
                            avail = c->rcap - c->rlen;
                            if (avail == 0) {
                                if (c->rcap >= c->srv->proto_max_request_bytes ||
                                    conn_rbuf_grow(c) != 0) {
                                    process_error = 1;
                                    break;
                                }
                                avail = c->rcap - c->rlen;
                            }
                        }
                        take = (size_t)ev->bytes - copied;
                        if (take > avail)
                            take = avail;
                        memcpy(c->rbuf + c->rlen, chunk + copied, take);
                        c->rlen += take;
                        copied += take;
                        pr = conn_process_input(s, c);
                        if (pr != 0) {
                            process_error = 1;
                            break;
                        }
                    }
                    pal_iouring_recycle(s->iou, ev->buf_id);
                    if (process_error) {
                        resp_write_error(&c->out, "ERR Protocol error", 18);
                        c->close_after_send = 1;
                        kick_flush(s, c);
                        continue;
                    }
                }
                s->io.bytes_read += (uint64_t)ev->bytes;
                if (conn_process_input(s, c) != 0) {
                    resp_write_error(&c->out, "ERR Protocol error", 18);
                    c->close_after_send = 1;
                    kick_flush(s, c);
                    continue;
                }
                kick_flush(s, c);
                if (!ev->op_done)
                    continue; /* request still armed: zero reposts */
                iou_ms_rearm(s, c, idx); /* final CQE carried data: rearm */
                continue;
            }
            if (ev->bytes <= 0) { /* orderly close or error */
                conn_close(s, idx);
                continue;
            }
            c->rlen += (size_t)ev->bytes;
            s->io.bytes_read += (uint64_t)ev->bytes;
            if (conn_process_input(s, c) != 0) {
                resp_write_error(&c->out, "ERR Protocol error", 18);
                c->close_after_send = 1;
                kick_flush(s, c);
                continue;
            }
            kick_flush(s, c);
            /* re-post the next recv (grow the buffer when nearly full) */
            if (c->rcap - c->rlen < SERVER_RECV_CHUNK &&
                c->rcap < c->srv->proto_max_request_bytes) {
                if (conn_rbuf_grow(c) != 0) {
                    conn_close(s, idx);
                    continue;
                }
            }
            c->pending_ops++;
            s->io.reads++;
            if (pro_recv(s, c->fd, c->rbuf + c->rlen,
                         c->rcap - c->rlen, c) != 0) {
                c->pending_ops--;
                conn_close(s, idx);
                continue;
            }
        } else if (ev->op == PAL_IOCP_SEND) {
            if (ev->bytes < 0) {
                if (c->send_fixed_id >= 0) {
                    pal_iouring_sbuf_release(s->iou, c->send_fixed_id);
                    c->send_fixed_id = -1;
                    c->send_zc_notif_pending = 0;
                }
                c->send_outstanding = 0;
                if (c->send_zc_notif_pending)
                    c->pending_ops--;
                conn_close(s, idx);
                continue;
            }
            if (!ev->notif) {
                s->io.bytes_written += (uint64_t)ev->bytes;
                c->sbuf_sent += (size_t)ev->bytes;
            }
            if (c->sbuf_sent >= c->sbuf_len &&
                (!c->send_zc_notif_pending || ev->notif)) {
                /* detached buffer fully sent: return it to the pool */
                if (c->sbuf_pool_size > 0)
                    buf_pool_put(&s->pool, c->sbuf, c->sbuf_pool_size);
                else
                    free(c->sbuf);
                c->sbuf = NULL;
                c->sbuf_len = 0;
                c->sbuf_sent = 0;
                c->sbuf_pool_size = 0;
            }
            if (ev->notif && c->send_fixed_id >= 0) {
                pal_iouring_sbuf_release(s->iou, c->send_fixed_id);
                c->send_fixed_id = -1;
                c->send_zc_notif_pending = 0;
                c->send_outstanding = 0;
            } else if (!c->send_zc_notif_pending) {
                c->send_outstanding = 0;
            }
            if (c->send_zc_notif_pending && !ev->notif)
                continue;
            if (c->close_after_send && c->sbuf == NULL && c->out.len == 0) {
                conn_close(s, idx);
                continue;
            }
            /* next chunk of the same buffer, or detach whatever
             * accumulated in out while the send was in flight */
            kick_flush(s, c);
        }
    }

    /* fan-out: pub/sub pushes, replication stream, SYNC frames */
    {
        size_t ci;
        for (ci = 0; ci < s->nconns; ci++) {
            conn *c = s->conns[ci];
            /* pending output = fresh out + the unsent tail of the
             * detached send buffer */
            if (c->is_replica &&
                c->out.len + (c->sbuf_len - c->sbuf_sent) >
                    REPL_MAX_OUTBUF) {
                conn_close(s, ci);
                ci--;
                continue;
            }
            kick_flush(s, c);
        }
    }
    srv_aof_flush(s);
    return nev;
}

/* Wake every blocked client that is now ready, and reply to those whose
 * deadline has passed. Any pipelined bytes buffered behind a blocking
 * command are processed once the connection unblocks. */
static void server_process_blocked(server *s)
{
    size_t i;
    uint64_t now = pal_wall_ms();
    for (i = 0; i < s->nconns; i++) {
        conn *c = s->conns[i];
        if (c->sess == NULL || !c->sess->blocked)
            continue;
        if (command_blocked_try(c->sess, &c->out, now)) {
            if (srv_proactor(s))
                kick_flush(s, c);
            else if (conn_flush(s, c) != 0) {
                conn_close(s, i);
                i--;
                continue;
            }
        }
    }
    for (i = 0; i < s->nconns; i++) {
        conn *c = s->conns[i];
        if (c->sess == NULL || c->sess->blocked || c->rlen == 0)
            continue;
        {
            int pr = conn_process_input(s, c);
            if (pr < 0) {
                static const char proto_err[] = "-ERR Protocol error\r\n";
                (void)conn_write(c, proto_err, sizeof(proto_err) - 1);
                conn_close(s, i);
                i--;
                continue;
            }
            if (pr == 2)
                continue;
            if (c->out.len > 0) {
                if (srv_proactor(s))
                    kick_flush(s, c);
                else if (conn_flush(s, c) != 0) {
                    conn_close(s, i);
                    i--;
                }
            }
        }
    }
}

/* Cap the readiness wait at the earliest blocking-command deadline so the
 * event loop wakes in time to emit a timeout reply. */
static int server_blocked_wait_ms(server *s, int timeout_ms)
{
    uint64_t now;
    uint64_t wait;
    size_t i;
    if (timeout_ms < 0)
        return timeout_ms;
    wait = (uint64_t)timeout_ms;
    now = pal_wall_ms();
    for (i = 0; i < s->nconns; i++) {
        conn *c = s->conns[i];
        uint64_t rem;
        if (c->sess == NULL || !c->sess->blocked ||
            c->sess->blocked_deadline_ms == 0)
            continue;
        if (c->sess->blocked_deadline_ms <= now)
            return 0;
        rem = c->sess->blocked_deadline_ms - now;
        if (rem < wait)
            wait = rem;
    }
    if (wait > (uint64_t)INT_MAX)
        wait = (uint64_t)INT_MAX;
    return (int)wait;
}

int server_run_once(server *s, int timeout_ms)
{
    pal_event evs[SERVER_MAX_EVENTS];
    int nev;
    int i;

    s->io.loops++;
    server_process_blocked(s);
    timeout_ms = server_blocked_wait_ms(s, timeout_ms);
    if (s->aof_failed) {
        if (s->route_fn != NULL)
            return -1;
        goto service_io;
    }
    /* active expiration: at most one cycle per 100 ms of monotonic time;
     * every logical db gets a pass (empty expires tables exit in O(1)) */
    {
        uint64_t now = pal_now_ms();
        if (now - s->last_active_expire >= 100) {
            int i;
            s->last_active_expire = now;
            for (i = 0; i < s->ndbs; i++)
                db_active_expire(srv_select_db(s, i), pal_wall_ms(), 20);
        }
    }

    /* automatic snapshot: save interval elapsed and the db changed */
    if (s->aof == NULL && s->save_sec > 0 &&
        s->db.snapshot_path != NULL) {
        uint64_t now = pal_now_ms();
        if (now - s->last_save_check >= (uint64_t)s->save_sec * 1000) {
            s->last_save_check = now;
            if (s->db.dirty != s->dirty_at_last_save &&
                snapshot_save_multi(s, srv_select_db, s->ndbs,
                                    s->db.snapshot_path) == 0) {
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
            cluster_failover_check(s, pal_wall_ms());
        }
        if (now - s->last_nodes_save >= 10000) {
            s->last_nodes_save = now;
            cluster_nodes_save(s);
        }
    }

service_io:
    if (srv_proactor(s)) {
        int pr = server_run_once_proactor(s, timeout_ms);
        server_process_blocked(s);
        return pr;
    }

    nev = pal_loop_wait(s->loop, evs, SERVER_MAX_EVENTS, timeout_ms);
    if (nev <= 0) {
        srv_aof_flush(s);
        server_process_blocked(s);
        return nev;
    }
    s->io.events += (uint64_t)nev;

    for (i = 0; i < nev; i++) {
        if (s->wakeup_fd != PAL_SOCKET_INVALID &&
            evs[i].fd == s->wakeup_fd) {
            if (s->wakeup_cb != NULL)
                s->wakeup_cb(s->wakeup_ctx);
            continue;
        }
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
                if (c->link_state == LINK_CONNECTING) {
                    if (pal_connect_finish(c->fd) != 0) {
                        conn_close(s, idx); /* retry timer reconnects */
                        continue;
                    }
                    if (s->repl_tls) {
                        int hs;
                        c->tls = pal_tls_new(s->repl_tls_ctx, c->fd);
                        if (c->tls == NULL) {
                            conn_close(s, idx);
                            continue;
                        }
                        c->tls_client = 1;
                        c->tls_handshaking = 1;
                        c->link_state = LINK_TLS_HANDSHAKE;
                        hs = pal_tls_connect_handshake_nb(c->tls);
                        if (hs < 0) {
                            conn_close(s, idx);
                            continue;
                        }
                        if (hs == 1) {
                            c->tls_handshaking = 0;
                            if (repl_link_queue_psync(s, c) != 0) {
                                conn_close(s, idx);
                                continue;
                            }
                            pal_loop_mod(s->loop, c->fd, 1, 1, c);
                            (void)conn_flush(s, c);
                        } else {
                            pal_loop_mod(s->loop, c->fd, 1, hs == 2, c);
                        }
                        continue;
                    }
                    if (repl_link_queue_psync(s, c) != 0) {
                        conn_close(s, idx);
                        continue;
                    }
                    pal_loop_mod(s->loop, c->fd, 1, 0, c);
                    (void)conn_flush(s, c); /* ship the PSYNC */
                    continue;
                }
                if (c->link_state == LINK_TLS_HANDSHAKE) {
                    int hs = pal_tls_connect_handshake_nb(c->tls);
                    if (hs < 0) {
                        conn_close(s, idx);
                        continue;
                    }
                    if (hs == 1) {
                        c->tls_handshaking = 0;
                        c->link_state = LINK_SYNC_SENT;
                        if (repl_link_queue_psync(s, c) != 0) {
                            conn_close(s, idx);
                            continue;
                        }
                        pal_loop_mod(s->loop, c->fd, 1, 1, c);
                        (void)conn_flush(s, c);
                    } else {
                        pal_loop_mod(s->loop, c->fd, 1, hs == 2, c);
                    }
                    continue;
                }
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

            /* drain the socket (bounded) so pipelined input coalesces into
             * one dispatch+flush batch per readiness event */
            {
                int reads, dead = 0, protocol_error = 0;
                for (reads = 0; reads < 4 && !dead; reads++) {
                    if (c->rlen >= c->srv->proto_max_request_bytes) {
                        int pr = conn_process_input(s, c);
                        if (pr < 0) {
                            protocol_error = 1;
                            break;
                        }
                        if (pr == 2)
                            break;
                        if (c->rlen >= c->srv->proto_max_request_bytes) {
                            dead = 1;
                            break;
                        }
                    }
                    if (c->rcap - c->rlen < SERVER_RECV_CHUNK &&
                        conn_rbuf_grow(c) != 0) {
                        dead = 1;
                        break;
                    }
                    n = conn_read(c, c->rbuf + c->rlen, c->rcap - c->rlen);
                    if (n == 0 ||
                        (n < 0 && n != -2 &&
                         !pal_would_block(pal_socket_error()))) {
                        dead = 1;
                        break; /* orderly close or hard error */
                    }
                    if (n < 0)
                        break; /* drained for now */
                    c->rlen += (size_t)n;
                }
                if (dead) {
                    conn_close(s, idx);
                    continue;
                }
                if (protocol_error) {
                    static const char proto_err[] =
                        "-ERR Protocol error\r\n";
                    (void)conn_write(c, proto_err, sizeof(proto_err) - 1);
                    conn_close(s, idx);
                    continue;
                }
            }

            /* parse -> execute -> advance, then compact consumed bytes */
            {
                int pr = conn_process_input(s, c);
                if (pr < 0) {
                    static const char proto_err[] =
                        "-ERR Protocol error\r\n";
                    (void)conn_write(c, proto_err, sizeof(proto_err) - 1);
                    conn_close(s, idx);
                    continue;
                }
                if (pr == 2)
                    continue; /* migrated to another worker */
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
                continue;
            }
            /* send-then-close (QUIT, protocol error): the last buffered
             * byte is out, the peer has the reply */
            if (c->close_after_send && c->out.len == 0) {
                conn_close(s, ci);
                ci--;
            }
        }
    }
    /* flush the AOF buffer once per loop iteration */
    srv_aof_flush(s);
    server_process_blocked(s);
    return nev;
}
