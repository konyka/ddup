/* mt_server.c - thread-per-core server; see mt_server.h.
 *
 * Acceptor thread: owns the public (non-blocking) listener, accepts pending
 * connections and pushes each fd onto one worker's accept queue, kicking the
 * worker's wakeup pipe. Worker threads run an ordinary server event loop;
 * the wakeup callback drains accepts, the routed-task inbox and the
 * completion queue.
 *
 * Key routing (single-key commands): the connection's home worker computes
 * hash_slot(key) % nworkers. Commands owned by another worker are deep-copied
 * into an mt_task and executed on the target worker with command_execute_at
 * (stateless path: no MULTI/WATCH/pubsub/AOF involvement). Replies return
 * through the home worker's completion queue and are appended to conn->out
 * in original pipeline order via a per-conn sequence/reorder buffer.
 */
#include "server/mt_server.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/cluster.h"
#include "core/command.h"
#include "core/hashslot.h"
#include "core/rhtable.h"
#include "ds/glob.h"
#include "core/session.h"
#include "core/snapshot.h"
#include "pal/pal_event.h"
#include "pal/pal_cstd.h"
#include "pal/pal_file.h"
#include "pal/pal_socket.h"
#include "pal/pal_thread.h"
#include "pal/pal_time.h"
#include "pal/pal_wakeup.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"
#include "server/mt_spsc.h"
#include "server/server.h"

/* case-insensitive compare of n bytes against a NUL-terminated literal */
static int mt_ci_equal(const char *a, size_t n, const char *b)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + ('a' - 'A'));
        if (ca != cb || cb == '\0')
            return 0;
    }
    return b[n] == '\0';
}

/* Strict signed 64-bit parse for routing math (SINTERCARD numkeys). */
static int mt_parse_ll(const char *s, size_t len, long long *out)
{
    size_t i = 0;
    int neg = 0;
    long long v = 0;
    if (len == 0)
        return 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (len == 1)
            return 0;
    }
    for (; i < len; i++) {
        unsigned digit;
        if (s[i] < '0' || s[i] > '9')
            return 0;
        digit = (unsigned)(s[i] - '0');
        if (v > (LLONG_MAX - (long long)digit) / 10)
            return 0;
        v = v * 10 + (long long)digit;
    }
    *out = neg ? -v : v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* routed tasks                                                        */
/* ------------------------------------------------------------------ */

typedef struct worker worker;

typedef struct mt_agg mt_agg;

/* One raw RESP command byte copy (routed tasks re-parse on the target
 * worker instead of deep-copying every argv element). */
typedef struct mt_cmd_blob {
    char *raw;
    size_t len;
} mt_cmd_blob;

/* One WATCH entry: key copy plus the key-version/flush-epoch seen at
 * WATCH time (validated again at EXEC time on the owning worker). */
typedef struct mt_watch_entry {
    char *key;
    size_t klen;
    uint64_t version;
    uint64_t epoch;
    int db_index; /* the db the key was watched in (SELECT-aware) */
} mt_watch_entry;

typedef struct mt_deferred_cmd {
    struct mt_deferred_cmd *next;
    char *raw;
    size_t rawlen;
    int db_index;
    uint16_t cmd;
    uint64_t seq;
    int authed;
} mt_deferred_cmd;

/* One subscribed channel on a connection (home-side bookkeeping). */
typedef struct mt_conn_sub {
    struct mt_conn_sub *next;
    char *ch;
    size_t chlen;
    int owner; /* worker id owning the channel */
    int pattern;
} mt_conn_sub;

/* Channel-registry entry on the owner worker (owner thread only). */
typedef struct mt_sub_entry {
    struct mt_sub_entry *next;
    char *ch;
    size_t chlen;
    int home_id; /* subscriber's home worker id */
    void *conn;  /* subscriber conn (compared by value only, never followed
                    except on the subscriber's home worker) */
    int pattern;
} mt_sub_entry;

/* Task kinds. */
#define MT_TASK_CMD 0    /* ordinary routed command batch */
#define MT_TASK_WATCH 1  /* WATCH: reply +OK, versions ride back out-of-band */
#define MT_TASK_EXEC 2   /* EXEC bundle: watch check + sequential replay */
#define MT_TASK_UNWATCH 3 /* fire-and-forget watch_refs release (key blob) */
#define MT_TASK_SUB 4    /* pub/sub register (round trip, empty reply) */
#define MT_TASK_UNSUB 5  /* pub/sub unregister (fire-and-forget) */
#define MT_TASK_PUBLISH 6 /* PUBLISH: fan-out + receiver count reply */
#define MT_TASK_PUSH 7   /* pub/sub delivery to a subscriber's home */
#define MT_TASK_MIGRATE 8 /* connection migration to another worker */
#define MT_TASK_WATCH_CLEANUP 9 /* reuse WATCH task to release owner refs */
#define MT_TASK_CLUSTER_SYNC 10 /* fire-and-forget cluster metadata copy */
#define MT_TASK_RESTORE 11     /* full-sync restore one key on a worker */
#define MT_TASK_REPL_STREAM 12 /* fire-and-forget central replication append */
#define MT_TASK_REPL_SNAPSHOT 13 /* serialize one worker's db for full sync */
#define MT_TASK_SCAN 14       /* SCAN routed with a composite cursor */
#define MT_TASK_PUBSUB_STATS 15 /* PUBSUB introspection aggregate part */
#define MT_TASK_COPY 16       /* COPY with full logical-DB selection hooks */
#define MT_MAX_LOGICAL_DBS 16

#define MT_SCAN_TAG UINT64_C(0x8000000000000000)
#define MT_SCAN_WORKER_SHIFT 56
#define MT_SCAN_LOCAL_MASK UINT64_C(0x00ffffffffffffff)

typedef struct mt_task {
    struct mt_task *next; /* reorder-buffer link (queue nodes wrap tasks) */
    void *conn;           /* home connection (opaque, home-thread only) */
    worker *home;         /* connection's home worker */
    uint64_t seq;         /* per-conn pipeline sequence base */
    uint32_t span;        /* pipeline seqs covered (>= 1; EXEC spans 1) */
    uint32_t ncmds;       /* raw command blobs in cmds (0 for local tasks) */
    mt_cmd_blob *cmds;    /* ncmds raw command copies (routed tasks only) */
    resp_buf reply;       /* filled by the executing worker */
    mt_agg *agg;          /* broadcast group this task is a part of (or NULL) */
    int kind;             /* MT_TASK_* */
    int pending_owned;    /* home pending count held by this task */
    int db_index;         /* logical db the task executes against (SELECT) */
    int repl_worker_id;   /* MT_TASK_REPL_SNAPSHOT source worker id */
    int scan_worker;      /* worker selected by a composite SCAN cursor */
    int scan_nworkers;
    int pubsub_pattern;
    uint64_t task_expire_ms; /* MT_TASK_RESTORE absolute expiry (0 = none) */
    cluster_state *cluster_state; /* MT_TASK_CLUSTER_SYNC payload */
    /* WATCH result: 2 slots per key (version, epoch), filled on the owner */
    uint64_t *watch_out;
    size_t nwatch_out;
    size_t watch_cleanup_first;
    size_t watch_home_start;
    int watch_failed;
    /* EXEC input: watch entries transferred from the home conn state */
    mt_watch_entry *exec_watches;
    size_t nexec_watches;
    /* task pool (Phase 31): pooled tasks carry their single command in
     * inline_cmd/inline_buf (raw payloads larger than inline_buf fall
     * back to a malloc'd side buffer) and return to the home worker's
     * freelist instead of the allocator. pooled tasks are allocated AND
     * recycled through mt_task_free on any thread (mutex-guarded list). */
    struct mt_task *pool_next;
    mt_cmd_blob inline_cmd;
    char inline_buf[256];
    int pooled;
} mt_task;

#define MT_TASK_INLINE_MAX 256 /* inline_buf size */
#define MT_TASK_POOL_MAX 256   /* freelist cap per worker (overflow: free) */

struct worker {
    int id;
    mt_server *ms;
    server *srv;
    pal_thread thread;
    pal_wakeup wakeup;
    /* SPSC rings: one per producer. inbox[h] carries tasks from home worker
     * h; completions[t] carries executed replies from executing worker t;
     * accepts carries fds from the acceptor thread. */
    mt_spsc accepts;
    mt_spsc accepts_tls; /* accepted fds from the TLS listener */
    mt_spsc *inbox;
    mt_spsc *completions;
    mt_spsc *migrate;     /* MT_TASK_MIGRATE conns from other workers */
    mt_task *failed_tasks; /* producer-reported failed pushes */
    arena exec_arena;     /* re-parse scratch for routed commands */
    uint64_t tasks_executed; /* routed tasks executed (test/observability) */
    /* task object pool (Phase 31): recycled single-command tasks; the
     * mutex also covers the rare cross-thread recycle (UNWATCH/UNSUB are
     * freed on the executing worker) */
    pal_mutex task_pool_mu;
    mt_task *task_pool;
    uint32_t task_pool_n;
    uint64_t task_pool_hits; /* freelist pops (observability) */
    int pool_off;            /* set at destroy: free, never recycle */
#if DDUP_HAS_C_ATOMICS
    /* Kick dedup (Phase 27): 1 while a wakeup byte/completion is queued
     * but not yet consumed; producers skip redundant kicks. Reset after
     * draining the notification and before reading any queue. */
    _Atomic int kick_pending;
#endif
    /* Guards mt_conn_state.pending/closing of every conn homed on this
     * worker (increments can come from other workers' delivery paths). */
    pal_mutex pending_mu;
    uint64_t watch_release_pending[MT_MAX_LOGICAL_DBS];
    /* Conns closed with pending work: freed when it drains (or at destroy). */
    void **zombies;
    size_t nzombies;
    size_t zombie_cap;
    /* pub/sub channel registry (channels owned by this worker) */
    mt_sub_entry *subs;
    /* per-worker persistence paths (server stores the pointer, not a copy,
     * so these must outlive the worker) */
    char aof_path[1088];
    char snap_path[1088];
    volatile int running;
    /* Batched reply flush: conns that received reply bytes during one
     * mt_drain_completions pass are flushed once at the end of the pass
     * (replies are seq-ordered into conn->out, so coalescing is free). */
    uint64_t drain_epoch;
    void **flush_list;
    size_t flush_n;
    size_t flush_cap;
};

struct mt_server {
    pal_socket_t listen_fd;
    uint16_t port;
    pal_socket_t tls_listen_fd; /* PAL_SOCKET_INVALID when TLS is off */
    uint16_t tls_port;
    int nworkers;
    int worker_backend; /* SERVER_BACKEND_* the workers run on */
    worker *workers;
    pal_thread acceptor;
    int started_workers;
    int acceptor_started;
    volatile int running;
    int destroying;
    mt_agg **abandoned_aggs;
    size_t nabandoned_aggs;
    size_t abandoned_agg_cap;
    pal_mutex abandoned_agg_mu;
    int fail_completion_pushes;
    int fail_completion_worker;
    int fail_completion_consumed;
    int cluster_enabled; /* worker 0 owns the cluster bus/gossip */
    int replica_mode;    /* worker 0 is an outbound replica link */
    ddup_atomic_int repl_synced; /* full-sync restore finished */
    int snapshot_pending; /* replica full-sync restore tasks not yet done */
    int snapshot_ser_pending; /* master full-sync snapshot tasks not yet done */
    mt_task **snapshot_ser_tasks; /* completed snapshots, indexed by worker id */
};

/* Broadcast group for aggregate commands (DBSIZE/FLUSHDB): the home worker
 * fans one sub-task out to every other worker, accumulates the parts and
 * produces the final reply once all parts arrived. Only ever touched on the
 * home worker thread. */
struct mt_agg {
    void *conn;
    worker *home;
    uint64_t seq;
    uint16_t cmd;
    int pending;    /* parts still awaited */
    long long sum;  /* DBSIZE sum / LASTSAVE max */
    int err;        /* any part replied with an error */
    int db_index;   /* logical db for DBSIZE/FLUSHDB (SELECT-aware) */
    info_stats *stats; /* INFO: summed machine-format parts (or NULL) */
    char *raw;      /* copied command bytes for aggregate mutation replay */
    size_t rawlen;
    char *random_key; /* first non-null RANDOMKEY result */
    size_t random_key_len;
    resp_buf keys_body; /* concatenated RESP bulk elements for KEYS */
    size_t keys_count;
    int fanout_done;
    int abandoned;
    int finished;
    int pubsub_mode; /* 1 NUMPAT, 2 CHANNELS, 3 NUMSUB */
    char *pubsub_pattern;
    size_t pubsub_pattern_len;
    char **pubsub_names;
    size_t *pubsub_name_lens;
    long long *pubsub_counts;
    size_t pubsub_name_count;
    char **pubsub_channels;
    size_t *pubsub_channel_lens;
    size_t pubsub_channel_count;
    size_t pubsub_channel_cap;
};

static void mt_untrack_abandoned_agg(mt_server *ms, mt_agg *agg);
static void mt_blobs_free(mt_cmd_blob *blobs, size_t n);
static mt_task *mt_task_new(void *conn, worker *home, uint64_t seq,
                            uint32_t span, uint32_t ncmds, mt_cmd_blob *cmds);
static void mt_task_free(void *ptr);
static mt_cmd_blob *mt_blob_one(const char *raw, size_t len);
static int mt_push_task(worker *self, mt_spsc *q, mt_task *t,
                        worker *target);
static void mt_drain_completions(worker *w);

/* Forward one applied mutation to the worker-0 central replication stream.
 * Called on the executing worker thread; the fire-and-forget task preserves
 * per-worker order through that worker's SPSC inbox. */
static void mt_repl_stream_forward(void *ctx, int db_index, const char *raw,
                                   size_t raw_len)
{
    worker *self = (worker *)ctx;
    mt_server *ms;
    mt_task *t;
    mt_cmd_blob *blob;

    if (self == NULL || raw == NULL || raw_len == 0)
        return;
    ms = self->ms;
    if (ms == NULL || ms->nworkers == 0)
        return;
    if (ms->replica_mode)
        return;
    blob = mt_blob_one(raw, raw_len);
    if (blob == NULL)
        return;
    t = mt_task_new(NULL, &ms->workers[0], 0, 1, 1, blob);
    if (t == NULL || t->cmds == NULL) {
        if (t != NULL)
            mt_task_free(t);
        else
            mt_blobs_free(blob, 1);
        return;
    }
    t->kind = MT_TASK_REPL_STREAM;
    t->db_index = db_index;
    (void)mt_push_task(self, &ms->workers[0].inbox[self->id], t,
                       &ms->workers[0]);
}

/* Master full-sync snapshot: one framed DDUPMT01 snapshot containing every
 * worker's DDUP0002 snapshot. Replica loaders merge the shards by db. Each
 * worker serializes its own db on its own event loop (worker 0 is serialized
 * inline here), so no follower db is ever read from another thread. */
static int mt_repl_snapshot_serialize(void *ctx, resp_buf *out)
{
    mt_server *ms = (mt_server *)ctx;
    worker *leader;
    resp_buf local;
    mt_task **tasks;
    const char **bufs;
    size_t *lens;
    int ndbs, i;
    int failed = 0;
    int rc = -1;

    if (ms == NULL || ms->nworkers == 0 || out == NULL)
        return -1;
    leader = &ms->workers[0];
    ndbs = server_ndbs(leader->srv);
    if (ndbs <= 0)
        return -1;

    tasks = (mt_task **)calloc((size_t)ms->nworkers, sizeof(*tasks));
    bufs = (const char **)calloc((size_t)ms->nworkers, sizeof(*bufs));
    lens = (size_t *)calloc((size_t)ms->nworkers, sizeof(*lens));
    if (tasks == NULL || bufs == NULL || lens == NULL)
        goto done;

    resp_buf_init(&local);
    if (snapshot_serialize_multi(leader->srv, server_select_db, ndbs,
                                 &local) != 0) {
        resp_buf_free(&local);
        goto done;
    }
    bufs[0] = local.data;
    lens[0] = local.len;

    ms->snapshot_ser_pending = 0;
    ms->snapshot_ser_tasks = tasks;
    for (i = 1; i < ms->nworkers; i++) {
        mt_task *t = mt_task_new(NULL, leader, 0, 1, 0, NULL);
        if (t == NULL) {
            failed = 1;
            break;
        }
        t->kind = MT_TASK_REPL_SNAPSHOT;
        t->repl_worker_id = i;
        ms->snapshot_ser_pending++;
        if (mt_push_task(leader, &ms->workers[i].inbox[leader->id], t,
                         &ms->workers[i]) != 0) {
            ms->snapshot_ser_pending--;
            failed = 1;
        }
    }

    while (ms->snapshot_ser_pending > 0 && leader->running) {
        mt_drain_completions(leader);
        pal_sleep_ms(1);
    }

    if (failed || ms->snapshot_ser_pending != 0) {
        for (i = 1; i < ms->nworkers; i++) {
            if (tasks[i] != NULL)
                mt_task_free(tasks[i]);
            tasks[i] = NULL;
        }
        ms->snapshot_ser_tasks = NULL;
        ms->snapshot_ser_pending = 0;
        resp_buf_free(&local);
        goto done;
    }

    for (i = 1; i < ms->nworkers; i++) {
        if (tasks[i] == NULL || tasks[i]->reply.len == 0) {
            failed = 1;
            break;
        }
        bufs[i] = tasks[i]->reply.data;
        lens[i] = tasks[i]->reply.len;
    }
    if (!failed)
        rc = snapshot_serialize_multi_buffers(bufs, lens, ms->nworkers, out);

    for (i = 1; i < ms->nworkers; i++) {
        if (tasks[i] != NULL)
            mt_task_free(tasks[i]);
    }
    ms->snapshot_ser_tasks = NULL;
    ms->snapshot_ser_pending = 0;
    resp_buf_free(&local);
done:
    free(tasks);
    free(bufs);
    free(lens);
    return rc;
}

/* Per-conn routing state (stored via server_conn_set_mt_state). */
typedef struct mt_conn_state {
    uint64_t seq_next;  /* next sequence number to assign */
    uint64_t seq_write; /* next sequence number to append to conn->out */
    mt_task *reorder;   /* ready replies waiting, sorted by seq */
    /* Open merge batch: consecutive routed commands for the same target
     * worker are merged into one task (flushed on target change, on a
     * local/blocked command, or at the end of the parse loop). */
    int batch_target;   /* -1 = no open batch */
    uint64_t batch_seq; /* base seq of the open batch */
    int batch_db;       /* db_index of the open batch's commands */
    mt_cmd_blob *batch;
    size_t batch_n;
    size_t batch_cap;
    /* inline staging for the first blob of an open batch (Phase 31):
     * batch[0].raw == batch_inline means "not heap-owned" */
    char batch_inline[MT_TASK_INLINE_MAX];
    /* mt-level transaction state (session never enters MULTI in mt mode) */
    int in_multi;
    mt_cmd_blob *mq; /* queued command blobs between MULTI and EXEC */
    size_t mq_n;
    size_t mq_cap;
    mt_watch_entry *watches;
    size_t nwatch;
    size_t watch_cap;
    int replaying_deferred;
    size_t watch_pending;
    mt_deferred_cmd *deferred_head;
    mt_deferred_cmd *deferred_tail;
    /* Lifetime: guarded by the home worker's pending_mu. closing is set
     * under the mutex before any free path, so producers can safely test
     * it; pending counts tasks/deliveries in flight for this conn. */
    int pending;
    int closing;
    /* pub/sub: channels this conn is subscribed to (home thread; the
     * authoritative registry lives on each channel's owner worker) */
    mt_conn_sub *subs;
    size_t nsub;
    /* connection-key affinity: set once the conn has been migrated to the
     * worker owning its (first) keys; further commands stay put */
    int migrated;
    /* home-worker drain pass id at which this conn was queued for a flush
     * (0 = not queued) */
    uint64_t flush_epoch;
} mt_conn_state;

static int mt_route_txn(worker *, void *, mt_conn_state *,
                        const resp_value *, size_t, const char *, size_t,
                        uint16_t, int, uint64_t, resp_buf *);
static void mt_replay_deferred(worker *, void *, mt_conn_state *);
static void mt_exec_task(worker *, mt_task *);
static void mt_drain_completions(worker *w);
static void mt_cluster_sync(worker *leader);
static void mt_route_replicaof(worker *home, const resp_value *argv,
                               size_t argc, resp_buf *out);
static int mt_route(void *, void *, session *, const resp_value *, size_t,
                    const char *, size_t, resp_buf *);
static int mt_repl_snapshot_load(void *ctx, const char *buf, size_t len);
static int mt_repl_snapshot_serialize(void *ctx, resp_buf *out);
static void mt_repl_stream_forward(void *ctx, int db_index, const char *raw,
                                   size_t raw_len);

static void mt_blobs_free(mt_cmd_blob *blobs, size_t n)
{
    size_t i;
    if (blobs == NULL)
        return;
    for (i = 0; i < n; i++)
        free(blobs[i].raw);
    free(blobs);
}

static mt_task *mt_task_new(void *conn, worker *home, uint64_t seq,
                            uint32_t span, uint32_t ncmds, mt_cmd_blob *cmds)
{
    mt_task *t = (mt_task *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;
    t->conn = conn;
    t->home = home;
    t->seq = seq;
    t->span = span == 0 ? 1 : span;
    t->ncmds = ncmds;
    t->cmds = cmds; /* ownership transferred */
    resp_buf_init(&t->reply);
    return t;
}

static void mt_task_free(void *ptr)
{
    mt_task *t = (mt_task *)ptr;
    worker *home;
    size_t i;
    if (t == NULL)
        return;
    /* release the dynamic payloads (inline staging is not heap) */
    if (t->cmds != NULL && t->cmds != &t->inline_cmd)
        mt_blobs_free(t->cmds, t->ncmds);
    else if (t->cmds == &t->inline_cmd &&
             t->inline_cmd.raw != t->inline_buf)
        free(t->inline_cmd.raw);
    free(t->watch_out);
    free(t->cluster_state);
    if (t->exec_watches != NULL) {
        for (i = 0; i < t->nexec_watches; i++)
            free(t->exec_watches[i].key);
        free(t->exec_watches);
    }
    /* recycle the object into the home worker's freelist (struct +
     * reply capacity survive; reply buffers >256KB are let go) */
    home = t->home;
    if (home != NULL && home->pool_off)
        home = NULL; /* teardown: plain free, the mutex may be gone */
    if (home != NULL && t->reply.cap <= 256 * 1024) {
        int stocked = 0;
        pal_mutex_lock(&home->task_pool_mu);
        if (home->task_pool_n < MT_TASK_POOL_MAX) {
            t->cmds = NULL;
            t->ncmds = 0;
            t->watch_out = NULL;
            t->exec_watches = NULL;
            t->agg = NULL;
            t->pooled = 1;
            t->pool_next = home->task_pool;
            home->task_pool = t;
            home->task_pool_n++;
            stocked = 1;
        }
        pal_mutex_unlock(&home->task_pool_mu);
        if (stocked)
            return;
    }
    resp_buf_free(&t->reply);
    free(t);
}

/* Insert a ready reply into the per-conn reorder buffer (sorted by seq). */
static void mt_reorder_insert(mt_conn_state *st, mt_task *t)
{
    mt_task **pp = &st->reorder;
    while (*pp != NULL && (*pp)->seq < t->seq)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
}

static int mt_watch_add(mt_conn_state *st, const char *key, size_t klen,
                        uint64_t version, uint64_t epoch, int db_index)
{
    mt_watch_entry *e;
    if (st->nwatch == st->watch_cap) {
        size_t ncap = st->watch_cap == 0 ? 4 : st->watch_cap * 2;
        mt_watch_entry *nw = (mt_watch_entry *)realloc(
            st->watches, ncap * sizeof(*nw));
        if (nw == NULL)
            return -1;
        st->watches = nw;
        st->watch_cap = ncap;
    }
    e = &st->watches[st->nwatch];
    e->key = (char *)malloc(klen);
    if (e->key == NULL)
        return -1;
    memcpy(e->key, key, klen);
    e->klen = klen;
    e->version = version;
    e->epoch = epoch;
    e->db_index = db_index;
    st->nwatch++;
    return 0;
}

static void mt_kick(worker *w);
static int mt_push_task(worker *self, mt_spsc *q, mt_task *t,
                        worker *target);
static mt_cmd_blob *mt_blob_one(const char *raw, size_t len);
static void mt_watch_release_task_refs_direct(mt_server *ms, mt_task *t);
static void mt_pending_dec(worker *home, void *conn, mt_conn_state *st);
static void mt_worker_sub_remove(worker *w, void *conn, const char *ch,
                                 size_t chlen, int pattern);
static void mt_dispose_failed_task(worker *home, mt_task *t);

static void mt_agg_free(mt_agg *agg)
{
    if (agg == NULL || agg->finished)
        return;
    mt_untrack_abandoned_agg(agg->home->ms, agg);
    agg->finished = 1;
    free(agg->raw);
    free(agg->random_key);
    free(agg->pubsub_pattern);
    if (agg->pubsub_names != NULL) {
        size_t i;
        for (i = 0; i < agg->pubsub_name_count; i++)
            free(agg->pubsub_names[i]);
    }
    free(agg->pubsub_names);
    free(agg->pubsub_name_lens);
    free(agg->pubsub_counts);
    if (agg->pubsub_channels != NULL) {
        size_t i;
        for (i = 0; i < agg->pubsub_channel_count; i++)
            free(agg->pubsub_channels[i]);
    }
    free(agg->pubsub_channels);
    free(agg->pubsub_channel_lens);
    resp_buf_free(&agg->keys_body);
    free(agg->stats);
    free(agg);
}

/* Drop one remote part. The final free is delayed until fanout has stopped so
 * a failed push cannot invalidate the aggregate while the loop still uses it. */
/* Account for exactly one aggregate part. The caller owns finalization after
 * the decrement; this helper never frees agg. */
static int mt_agg_drop_part(mt_agg *agg)
{
    if (agg == NULL || agg->finished)
        return 0;
    if (agg->pending > 0)
        agg->pending--;
    return agg->pending == 0 && agg->fanout_done;
}

static void mt_track_abandoned_agg(mt_server *ms, mt_agg *agg)
{
    size_t i;
    if (agg == NULL)
        return;
    pal_mutex_lock(&ms->abandoned_agg_mu);
    for (i = 0; i < ms->nabandoned_aggs; i++)
        if (ms->abandoned_aggs[i] == agg)
        {
            pal_mutex_unlock(&ms->abandoned_agg_mu);
            return;
        }
    if (ms->nabandoned_aggs == ms->abandoned_agg_cap) {
        size_t ncap = ms->abandoned_agg_cap == 0
                          ? 8 : ms->abandoned_agg_cap * 2;
        mt_agg **na = (mt_agg **)realloc(ms->abandoned_aggs,
                                         ncap * sizeof(*na));
        if (na == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        ms->abandoned_aggs = na;
        ms->abandoned_agg_cap = ncap;
    }
    agg->abandoned = 1;
    ms->abandoned_aggs[ms->nabandoned_aggs++] = agg;
    pal_mutex_unlock(&ms->abandoned_agg_mu);
}

static void mt_untrack_abandoned_agg(mt_server *ms, mt_agg *agg)
{
    size_t i;
    pal_mutex_lock(&ms->abandoned_agg_mu);
    for (i = 0; i < ms->nabandoned_aggs; i++) {
        if (ms->abandoned_aggs[i] == agg) {
            ms->abandoned_aggs[i] =
                ms->abandoned_aggs[--ms->nabandoned_aggs];
            break;
        }
    }
    pal_mutex_unlock(&ms->abandoned_agg_mu);
}

static void mt_drain_failed_tasks(worker *w)
{
    for (;;) {
        mt_task *t;
        pal_mutex_lock(&w->pending_mu);
        t = w->failed_tasks;
        if (t != NULL)
            w->failed_tasks = t->next;
        pal_mutex_unlock(&w->pending_mu);
        if (t == NULL)
            break;
        if (t->agg != NULL) {
            mt_agg *agg = t->agg;
            int terminal = agg->pending == 1 && agg->fanout_done;
            mt_dispose_failed_task(w, t);
            if (terminal && !agg->finished)
                mt_agg_free(agg);
        } else {
            mt_dispose_failed_task(w, t);
        }
        pal_mutex_lock(&w->ms->abandoned_agg_mu);
        w->ms->fail_completion_consumed++;
        pal_mutex_unlock(&w->ms->abandoned_agg_mu);
    }
}

static void mt_task_drop_after_push_failure(worker *self, mt_task *t)
{
    worker *home = t->home != NULL ? t->home : self;
    pal_mutex_lock(&home->pending_mu);
    t->next = home->failed_tasks;
    home->failed_tasks = t;
    pal_mutex_unlock(&home->pending_mu);
    mt_kick(home);
}

static void mt_dispose_failed_task(worker *home, mt_task *t)
{
    if (t->pending_owned) {
        mt_conn_state *st = (mt_conn_state *)
            server_conn_mt_state(t->conn);
        t->pending_owned = 0;
        if (st != NULL)
            mt_pending_dec(home, t->conn, st);
    }
    if (t->agg != NULL) {
        mt_agg *agg = t->agg;
        mt_agg_drop_part(agg);
        mt_track_abandoned_agg(home->ms, agg);
        /* The aggregate is home-owned. The fanout loop may still inspect it
         * after this producer reports a failed push, so only the home thread
         * may finalize/free it. */
        t->agg = NULL;
    }
    if (t->kind == MT_TASK_WATCH) {
        mt_watch_release_task_refs_direct(home->ms, t);
    } else if (t->kind == MT_TASK_WATCH_CLEANUP) {
        mt_watch_release_task_refs_direct(home->ms, t);
    } else if (t->kind == MT_TASK_UNSUB &&
               t->cmds != NULL && t->cmds[0].raw != NULL) {
        int owner = (int)(hash_slot(t->cmds[0].raw, t->cmds[0].len) %
                          (uint32_t)home->ms->nworkers);
        mt_worker_sub_remove(&home->ms->workers[owner], t->conn,
                             t->cmds[0].raw, t->cmds[0].len,
                             t->pubsub_pattern);
    } else if (t->kind == MT_TASK_MIGRATE && t->conn != NULL) {
        server_conn_free_now(home->srv, t->conn);
    }
    mt_task_free(t);
}

static void mt_watch_release_pending(worker *w)
{
    int i;
    pal_mutex_lock(&w->pending_mu);
    for (i = 0; i < MT_MAX_LOGICAL_DBS; i++) {
        uint64_t n = w->watch_release_pending[i];
        w->watch_release_pending[i] = 0;
        if (n != 0) {
            db *d = server_db_at(w->srv, i);
            d->watch_refs = d->watch_refs > n ? d->watch_refs - n : 0;
        }
    }
    pal_mutex_unlock(&w->pending_mu);
}

/* Fire-and-forget watch_refs release on the owning worker. */
static mt_task *mt_unwatch_task(const char *key, size_t klen, int db_index)
{
    mt_cmd_blob *b = (mt_cmd_blob *)malloc(sizeof(*b));
    mt_task *t;
    if (b == NULL)
        return NULL;
    b->raw = (char *)malloc(klen);
    if (b->raw == NULL) {
        free(b);
        return NULL;
    }
    memcpy(b->raw, key, klen);
    b->len = klen;
    t = mt_task_new(NULL, NULL, 0, 1, 1, b);
    if (t == NULL) {
        mt_blobs_free(b, 1);
        return NULL;
    }
    t->kind = MT_TASK_UNWATCH;
    t->db_index = db_index;
    return t;
}

/* Release one watch entry's db watch_refs on its owning worker. */
static void mt_watch_release_one(worker *home, const mt_watch_entry *e)
{
    int owner = (int)(hash_slot(e->key, e->klen) %
                      (uint32_t)home->ms->nworkers);
    if (home->ms->destroying) {
        db *d = server_db_at(home->ms->workers[owner].srv, e->db_index);
        if (d->watch_refs > 0)
            d->watch_refs--;
        return;
    }
    if (owner == home->id) {
        db *d = server_db_at(home->srv, e->db_index);
        if (d->watch_refs > 0)
            d->watch_refs--;
        return;
    }
    {
        mt_task *t = mt_unwatch_task(e->key, e->klen, e->db_index);
        if (t != NULL && mt_push_task(home,
                                      &home->ms->workers[owner].inbox[home->id],
                                      t, &home->ms->workers[owner]) == 0)
            return;
        {
            worker *ow = &home->ms->workers[owner];
            pal_mutex_lock(&ow->pending_mu);
            if (e->db_index >= 0 && e->db_index < MT_MAX_LOGICAL_DBS)
                ow->watch_release_pending[e->db_index]++;
            pal_mutex_unlock(&ow->pending_mu);
            mt_kick(ow);
        }
    }
}

static void mt_watches_clear(worker *home, mt_conn_state *st)
{
    size_t i;
    for (i = 0; i < st->nwatch; i++) {
        mt_watch_release_one(home, &st->watches[i]);
        free(st->watches[i].key);
    }
    free(st->watches);
    st->watches = NULL;
    st->nwatch = 0;
    st->watch_cap = 0;
}

static void mt_watches_release_suffix(worker *home, mt_conn_state *st,
                                      size_t start)
{
    while (st->nwatch > start) {
        mt_watch_entry *e = &st->watches[st->nwatch - 1];
        mt_watch_release_one(home, e);
        free(e->key);
        st->nwatch--;
    }
}

static void mt_watches_free_suffix(mt_conn_state *st, size_t start)
{
    while (st->nwatch > start) {
        st->nwatch--;
        free(st->watches[st->nwatch].key);
    }
}

static void mt_deferred_free(mt_deferred_cmd *d)
{
    while (d != NULL) {
        mt_deferred_cmd *next = d->next;
        free(d->raw);
        free(d);
        d = next;
    }
}

static int mt_watch_requeue_cleanup(worker *home, mt_task *t)
{
    if (t->nwatch_out == 0 || t->cmds == NULL)
    {
        mt_task_free(t);
        return 1;
    }
    t->kind = MT_TASK_WATCH_CLEANUP;
    {
        resp_value v;
        ptrdiff_t used;
        int owner;
        arena_reset(&home->exec_arena);
        used = resp_parse(t->cmds[0].raw, t->cmds[0].len, &v,
                          &home->exec_arena);
        if (used != (ptrdiff_t)t->cmds[0].len || v.type != RESP_ARRAY ||
            v.count < 2 || v.items[1].str == NULL) {
            mt_task_free(t);
            return 1;
        }
        owner = (int)(hash_slot(v.items[1].str, v.items[1].len) %
                      (uint32_t)home->ms->nworkers);
        if (owner == home->id)
            mt_exec_task(home, t);
        else
            (void)mt_push_task(home,
                               &home->ms->workers[owner].inbox[home->id], t,
                               &home->ms->workers[owner]);
        return 1; /* helper or push owns/frees t on every path */
    }
}

static int mt_watch_release_task_refs_from(worker *home, mt_task *t,
                                           size_t first)
{
    t->watch_cleanup_first = first;
    return mt_watch_requeue_cleanup(home, t);
}

static int mt_watch_release_task_refs(worker *home, mt_task *t)
{
    return mt_watch_release_task_refs_from(home, t, 0);
}

static void mt_watch_release_task_refs_direct(mt_server *ms, mt_task *t)
{
    resp_value v;
    arena ar;
    ptrdiff_t used;
    size_t i;
    int first = (int)t->watch_cleanup_first + 1;
    arena_init(&ar, 256);
    used = resp_parse(t->cmds[0].raw, t->cmds[0].len, &v, &ar);
    if (used == (ptrdiff_t)t->cmds[0].len && v.type == RESP_ARRAY) {
        for (i = (size_t)first; i < v.count &&
             i - 1 < t->nwatch_out; i++) {
            resp_value *key = &v.items[i];
            int owner;
            if (key->str == NULL)
                continue;
            owner = (int)(hash_slot(key->str, key->len) %
                          (uint32_t)ms->nworkers);
            {
                db *d = server_db_at(ms->workers[owner].srv,
                                     t->db_index);
                if (d->watch_refs > 0)
                    d->watch_refs--;
            }
        }
    }
    arena_destroy(&ar);
}

/* A routed WATCH drained in pipeline order: record the versions it brought
 * back (keys re-parsed from the task blob). */
static size_t mt_watch_apply(mt_conn_state *st, mt_task *t, arena *ar)
{
    resp_value v;
    ptrdiff_t used;
    size_t i;
    arena_reset(ar);
    used = resp_parse(t->cmds[0].raw, t->cmds[0].len, &v, ar);
    if (used != (ptrdiff_t)t->cmds[0].len || v.type != RESP_ARRAY ||
        t->watch_out == NULL) {
        t->watch_failed = 1;
        return 0;
    }
    {
        size_t applied = 0;
        for (i = 1;
             i < v.count && i - 1 < t->nwatch_out; i++) {
        if (v.items[i].str == NULL)
            continue;
        if (mt_watch_add(st, v.items[i].str, v.items[i].len,
                         t->watch_out[2 * (i - 1)],
                         t->watch_out[2 * (i - 1) + 1],
                         t->db_index) != 0) {
            mt_watches_free_suffix(st, t->watch_home_start);
            t->watch_failed = 1;
            break;
        }
        applied++;
        }
        return applied;
    }
}

/* ------------------------------------------------------------------ */
/* conn lifetime under cross-thread producers                           */
/* ------------------------------------------------------------------ */

static void mt_pending_inc(worker *home, mt_conn_state *st)
{
    pal_mutex_lock(&home->pending_mu);
    st->pending++;
    pal_mutex_unlock(&home->pending_mu);
}

static void mt_zombie_remove(worker *home, void *conn);

/* Decrement; frees the conn when it was closing and nothing is left. */
static void mt_pending_dec(worker *home, void *conn, mt_conn_state *st)
{
    int free_now = 0;
    pal_mutex_lock(&home->pending_mu);
    if (st->pending > 0)
        st->pending--;
    if (st->closing && st->pending == 0)
        free_now = 1;
    pal_mutex_unlock(&home->pending_mu);
    if (free_now) {
        mt_zombie_remove(home, conn);
        server_conn_free_now(home->srv, conn);
    }
}

/* Producer from another worker: only count the delivery when the conn is
 * still open (closing is always set under this mutex before any free). */
static int mt_pending_inc_if_open(worker *home, mt_conn_state *st)
{
    int ok;
    pal_mutex_lock(&home->pending_mu);
    ok = !st->closing;
    if (ok)
        st->pending++;
    pal_mutex_unlock(&home->pending_mu);
    return ok;
}

/* ------------------------------------------------------------------ */
/* pub/sub: home-side subscription bookkeeping                          */
/* ------------------------------------------------------------------ */

static mt_conn_sub *mt_conn_sub_find_kind(mt_conn_state *st, const char *ch,
                                          size_t chlen, int kind)
{
    mt_conn_sub *s;
    for (s = st->subs; s != NULL; s = s->next)
        if (s->pattern == kind && s->chlen == chlen &&
            memcmp(s->ch, ch, chlen) == 0)
            return s;
    return NULL;
}

static int mt_conn_sub_add(mt_conn_state *st, const char *ch, size_t chlen,
                           int owner, int pattern)
{
    mt_conn_sub *s = (mt_conn_sub *)calloc(1, sizeof(*s));
    if (s == NULL)
        return -1;
    s->ch = (char *)malloc(chlen);
    if (s->ch == NULL) {
        free(s);
        return -1;
    }
    memcpy(s->ch, ch, chlen);
    s->chlen = chlen;
    s->owner = owner;
    s->pattern = pattern;
    s->next = st->subs;
    st->subs = s;
    st->nsub++;
    return 0;
}

static int mt_conn_sub_remove(mt_conn_state *st, const char *ch,
                              size_t chlen, int pattern)
{
    mt_conn_sub **pp = &st->subs;
    while (*pp != NULL) {
        mt_conn_sub *s = *pp;
        if (s->pattern == pattern && s->chlen == chlen &&
            memcmp(s->ch, ch, chlen) == 0) {
            *pp = s->next;
            free(s->ch);
            free(s);
            if (st->nsub > 0)
                st->nsub--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

static void mt_conn_subs_free(mt_conn_state *st)
{
    while (st->subs != NULL) {
        mt_conn_sub *s = st->subs;
        st->subs = s->next;
        free(s->ch);
        free(s);
    }
    st->nsub = 0;
}

static void mt_worker_subs_free(worker *w)
{
    while (w->subs != NULL) {
        mt_sub_entry *e = w->subs;
        w->subs = e->next;
        free(e->ch);
        free(e);
    }
}

static void mt_worker_sub_remove(worker *w, void *conn, const char *ch,
                                 size_t chlen, int pattern)
{
    mt_sub_entry **pp = &w->subs;
    while (*pp != NULL) {
        mt_sub_entry *e = *pp;
        if (e->conn == conn && e->pattern == pattern && e->chlen == chlen &&
            memcmp(e->ch, ch, chlen) == 0) {
            *pp = e->next;
            free(e->ch);
            free(e);
            return;
        }
        pp = &e->next;
    }
}

static void mt_zombie_push(worker *home, void *conn)
{
    if (home->nzombies == home->zombie_cap) {
        size_t ncap = home->zombie_cap == 0 ? 8 : home->zombie_cap * 2;
        void **nz = (void **)realloc(home->zombies, ncap * sizeof(*nz));
        if (nz == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        home->zombies = nz;
        home->zombie_cap = ncap;
    }
    home->zombies[home->nzombies++] = conn;
}

static void mt_zombie_remove(worker *home, void *conn)
{
    size_t i;
    for (i = 0; i < home->nzombies; i++) {
        if (home->zombies[i] == conn) {
            home->zombies[i] = home->zombies[--home->nzombies];
            return;
        }
    }
}

/* server_mt_close_fn: runs on the home thread inside conn_close. */
static int mt_conn_close(void *ctx, void *conn)
{
    worker *home = (worker *)ctx;
    mt_conn_state *st = (mt_conn_state *)server_conn_mt_state(conn);
    int held = 0;
    mt_conn_sub *s;
    if (st == NULL)
        return 0;
    pal_mutex_lock(&home->pending_mu);
    st->closing = 1;
    if (st->pending > 0) {
        mt_zombie_push(home, conn);
        held = 1;
    }
    pal_mutex_unlock(&home->pending_mu);
    /* fan out unregistration for every subscribed channel (pointer-compare
     * tasks; safe once the conn is gone) */
    for (s = st->subs; s != NULL; s = s->next) {
        mt_task *t = mt_task_new(conn, home, 0, 1, 1,
                                 mt_blob_one(s->ch, s->chlen));
        if (t == NULL || t->cmds == NULL) {
            if (t != NULL)
                mt_task_free(t);
            continue;
        }
        t->kind = MT_TASK_UNSUB;
        mt_push_task(home, &home->ms->workers[s->owner].inbox[home->id], t,
                     &home->ms->workers[s->owner]);
    }
    return held;
}

/* Append every consecutive ready reply to conn->out (or drop them when the
 * conn is a zombie: the client is gone, only resource cleanup matters). */
static void mt_drain_ready(server *srv, arena *ar, void *conn,
                           mt_conn_state *st, int append)
{
    while (st->reorder != NULL && st->reorder->seq == st->seq_write) {
        mt_task *t = st->reorder;
        st->reorder = t->next;
        if (t->kind == MT_TASK_WATCH) {
            int cleanup_queued = 0;
            if (append && !st->closing) {
                size_t applied = mt_watch_apply(st, t, ar);
                if (applied < t->nwatch_out) {
                    static const char oom[] = "-ERR out of memory\r\n";
                    if (resp_buf_reserve(&t->reply, sizeof(oom) - 1) == 0) {
                        memcpy(t->reply.data, oom, sizeof(oom) - 1);
                        t->reply.len = sizeof(oom) - 1;
                    }
                    cleanup_queued = mt_watch_release_task_refs_from(
                        t->home, t, 0);
                }
            } else {
                cleanup_queued = mt_watch_release_task_refs(t->home, t);
            }
            if (st->watch_pending > 0)
                st->watch_pending--;
            if (append)
                (void)server_conn_out_append(srv, conn, t->reply.data,
                                             t->reply.len);
            st->seq_write += t->span;
            if (!cleanup_queued)
                mt_task_free(t);
            continue;
        }
        if (append)
            (void)server_conn_out_append(srv, conn, t->reply.data, t->reply.len);
        st->seq_write += t->span;
        mt_task_free(t);
    }
}

static void mt_state_free_cb(void *ctx, void *ptr)
{
    mt_conn_state *st = (mt_conn_state *)ptr;
    (void)ctx;
    if (st == NULL)
        return;
    while (st->reorder != NULL) {
        mt_task *t = st->reorder;
        st->reorder = t->next;
        if (t->kind == MT_TASK_WATCH) {
            if (!mt_watch_release_task_refs(t->home, t))
                mt_task_free(t);
        } else {
            mt_task_free(t);
        }
    }
    /* drop an unflushed merge batch (conn closed mid-pipeline); the first
     * blob's bytes may live in st->batch_inline (never heap) */
    if (st->batch != NULL) {
        size_t bi;
        for (bi = 0; bi < st->batch_n; bi++)
            if (st->batch[bi].raw != st->batch_inline)
                free(st->batch[bi].raw);
        free(st->batch);
    }
    /* drop transaction state */
    mt_blobs_free(st->mq, st->mq_n);
    mt_deferred_free(st->deferred_head);
    mt_watches_clear((worker *)ctx, st);
    /* drop pub/sub bookkeeping (registry cleanup was fanned out at close) */
    mt_conn_subs_free(st);
    free(st);
}

/* Accumulate one part of a broadcast reply (home thread only). */
/* Machine-format INFO request fanned out to every worker (internal). */
#define MT_INFO_STATS_REQ "*2\r\n$4\r\nINFO\r\n$9\r\n__STATS__\r\n"

/* Parse one "k:v" line of an INFO __STATS__ part into the running sum. */
static void mt_agg_info_line(mt_agg *agg, const char *p, size_t n)
{
    info_stats *st = agg->stats;
    if (n >= 12 && memcmp(p, "used_memory:", 12) == 0) {
        st->used_memory += strtoull(p + 12, NULL, 10);
    } else if (n >= 13 && memcmp(p, "expired_keys:", 13) == 0) {
        st->expired_keys += strtoull(p + 13, NULL, 10);
    } else if (n >= 13 && memcmp(p, "evicted_keys:", 13) == 0) {
        st->evicted_keys += strtoull(p + 13, NULL, 10);
    } else if (n >= 16 && memcmp(p, "tier_disk_bytes:", 16) == 0) {
        st->tier_disk_bytes += strtoull(p + 16, NULL, 10);
    } else if (n >= 18 && memcmp(p, "tier_live_records:", 18) == 0) {
        st->tier_live_records += strtoull(p + 18, NULL, 10);
    } else if (n >= 12 && memcmp(p, "tier_failed:", 12) == 0) {
        st->tier_failed += strtoull(p + 12, NULL, 10);
    } else if (n >= 7 && memcmp(p, "dbsize:", 7) == 0) {
        st->dbsize += strtoull(p + 7, NULL, 10);
    } else if (n >= 5 && memcmp(p, "ndbs:", 5) == 0) {
        int v = (int)strtol(p + 5, NULL, 10);
        if (v > INFO_STATS_MAX_DBS)
            v = INFO_STATS_MAX_DBS;
        if (v > st->ndbs)
            st->ndbs = v;
    } else if (n >= 4 && memcmp(p, "db:", 3) == 0) {
        char *q = NULL;
        unsigned long i = strtoul(p + 3, &q, 10);
        if (q != NULL && *q == ':' && i < INFO_STATS_MAX_DBS) {
            unsigned long long keys = strtoull(q + 1, &q, 10);
            if (q != NULL && *q == ':') {
                st->db_keys[i] += keys;
                st->db_expires[i] += strtoull(q + 1, NULL, 10);
            }
        }
    } else if (n >= 4 && memcmp(p, "c:", 2) == 0) {
        char *q = NULL;
        unsigned long id = strtoul(p + 2, &q, 10);
        if (q != NULL && *q == ':' && id >= 1 && id <= CMD_MAX) {
            unsigned long long calls = strtoull(q + 1, &q, 10);
            if (q != NULL && *q == ':') {
                st->cmd_calls[id] += calls;
                st->cmd_usecs[id] += strtoull(q + 1, NULL, 10);
            }
        }
    } else if (n >= 9 && memcmp(p, "io_loops:", 9) == 0) {
        st->io.loops += strtoull(p + 9, NULL, 10);
    } else if (n >= 10 && memcmp(p, "io_events:", 10) == 0) {
        st->io.events += strtoull(p + 10, NULL, 10);
    } else if (n >= 9 && memcmp(p, "io_reads:", 9) == 0) {
        st->io.reads += strtoull(p + 9, NULL, 10);
    } else if (n >= 10 && memcmp(p, "io_writes:", 10) == 0) {
        st->io.writes += strtoull(p + 10, NULL, 10);
    } else if (n >= 14 && memcmp(p, "io_bytes_read:", 14) == 0) {
        st->io.bytes_read += strtoull(p + 14, NULL, 10);
    } else if (n >= 17 && memcmp(p, "io_bytes_written:", 17) == 0) {
        st->io.bytes_written += strtoull(p + 17, NULL, 10);
    }
}

/* Accumulate one worker's INFO __STATS__ part (a bulk string of "k:v"
 * lines) into the running sum (home thread only). */
static void mt_agg_info_accumulate(mt_agg *agg, const char *data, size_t len)
{
    const char *p = data;
    const char *end = data + len;
    const char *eol;
    if (len == 0 || p[0] != '$') {
        agg->err = 1;
        return;
    }
    eol = (const char *)memchr(p, '\n', len);
    if (eol == NULL) {
        agg->err = 1;
        return;
    }
    p = eol + 1;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        size_t linelen =
            nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
        if (linelen > 0 && p[linelen - 1] == '\r')
            linelen--;
        if (linelen > 0)
            mt_agg_info_line(agg, p, linelen);
        if (nl == NULL)
            break;
        p = nl + 1;
    }
}

static void mt_pubsub_stats_execute(worker *w, mt_task *t)
{
    resp_value v;
    arena ar;
    ptrdiff_t used;
    size_t i;
    size_t n = 0;
    arena_init(&ar, 1024);
    used = t->cmds != NULL && t->ncmds == 1
               ? resp_parse(t->cmds[0].raw, t->cmds[0].len, &v, &ar)
               : -1;
    if (used < 0 || used != (ptrdiff_t)t->cmds[0].len ||
        v.type != RESP_ARRAY || v.count < 2 || v.items[1].str == NULL) {
        resp_write_error(&t->reply, "ERR Protocol error", 18);
        arena_destroy(&ar);
        return;
    }
    if (v.items[1].len == 6 &&
        mt_ci_equal(v.items[1].str, v.items[1].len, "NUMPAT")) {
        for (mt_sub_entry *e = w->subs; e != NULL; e = e->next)
            if (e->pattern)
                n++;
        resp_write_integer(&t->reply, (long long)n);
    } else if (v.items[1].len == 8 &&
               mt_ci_equal(v.items[1].str, v.items[1].len, "CHANNELS")) {
        const char *pat = NULL;
        size_t plen = 0;
        if (v.count == 3) {
            pat = v.items[2].str;
            plen = v.items[2].len;
        }
        for (mt_sub_entry *e = w->subs; e != NULL; e = e->next) {
            int seen = 0;
            if (e->pattern || (pat != NULL &&
                               !ddup_glob_match(pat, plen, e->ch, e->chlen)))
                continue;
            for (mt_sub_entry *p = w->subs; p != e; p = p->next)
                if (!p->pattern && p->chlen == e->chlen &&
                    memcmp(p->ch, e->ch, e->chlen) == 0) {
                    seen = 1;
                    break;
                }
            if (!seen)
                n++;
        }
        resp_write_array_header(&t->reply, n);
        for (mt_sub_entry *e = w->subs; e != NULL; e = e->next) {
            int seen = 0;
            if (e->pattern || (pat != NULL &&
                               !ddup_glob_match(pat, plen, e->ch, e->chlen)))
                continue;
            for (mt_sub_entry *p = w->subs; p != e; p = p->next)
                if (!p->pattern && p->chlen == e->chlen &&
                    memcmp(p->ch, e->ch, e->chlen) == 0) {
                    seen = 1;
                    break;
                }
            if (!seen)
                resp_write_bulk(&t->reply, e->ch, e->chlen);
        }
    } else if (v.items[1].len == 6 &&
               mt_ci_equal(v.items[1].str, v.items[1].len, "NUMSUB")) {
        resp_write_array_header(&t->reply, (v.count - 2) * 2);
        for (i = 2; i < v.count; i++) {
            long long count = 0;
            if (v.items[i].str == NULL)
                continue;
            for (mt_sub_entry *e = w->subs; e != NULL; e = e->next)
                if (!e->pattern && e->chlen == v.items[i].len &&
                    memcmp(e->ch, v.items[i].str, e->chlen) == 0)
                    count++;
            resp_write_bulk(&t->reply, v.items[i].str, v.items[i].len);
            resp_write_integer(&t->reply, count);
        }
    } else {
        resp_write_error(&t->reply, "ERR Unknown PUBSUB subcommand", 27);
    }
    arena_destroy(&ar);
}

static int mt_pubsub_channel_add(mt_agg *agg, const char *ch, size_t len)
{
    size_t i;
    char *copy;
    if (len == 0)
        return 0;
    for (i = 0; i < agg->pubsub_channel_count; i++)
        if (agg->pubsub_channel_lens[i] == len &&
            memcmp(agg->pubsub_channels[i], ch, len) == 0)
            return 0;
    if (agg->pubsub_channel_count == agg->pubsub_channel_cap) {
        size_t cap = agg->pubsub_channel_cap == 0 ? 8 :
                     agg->pubsub_channel_cap * 2;
        char **nc = (char **)malloc(cap * sizeof(*nc));
        size_t *nl = (size_t *)malloc(cap * sizeof(*nl));
        if (nc == NULL || nl == NULL) {
            free(nc);
            free(nl);
            return -1;
        }
        if (agg->pubsub_channel_count != 0) {
            memcpy(nc, agg->pubsub_channels,
                   agg->pubsub_channel_count * sizeof(*nc));
            memcpy(nl, agg->pubsub_channel_lens,
                   agg->pubsub_channel_count * sizeof(*nl));
        }
        free(agg->pubsub_channels);
        free(agg->pubsub_channel_lens);
        agg->pubsub_channels = nc;
        agg->pubsub_channel_lens = nl;
        agg->pubsub_channel_cap = cap;
    }
    copy = (char *)malloc(len);
    if (copy == NULL)
        return -1;
    memcpy(copy, ch, len);
    agg->pubsub_channels[agg->pubsub_channel_count] = copy;
    agg->pubsub_channel_lens[agg->pubsub_channel_count++] = len;
    return 0;
}

static void mt_agg_accumulate(mt_agg *agg, const resp_buf *part)
{
    if (part->data != NULL && part->len > 0) {
        if (part->data[0] == '-')
            agg->err = 1;
        else if (agg->cmd == CMD_INFO && agg->stats != NULL)
            mt_agg_info_accumulate(agg, part->data, part->len);
        else if (agg->cmd == CMD_RANDOMKEY && part->data[0] == '$') {
            const char *eol = (const char *)memchr(part->data, '\n',
                                                   part->len);
            long long n = -1;
            const char *payload;
            size_t plen;
            if (eol != NULL && eol > part->data + 1) {
                char num[32];
                size_t nl = (size_t)(eol - (part->data + 1));
                if (nl > 0 && nl < sizeof(num)) {
                    memcpy(num, part->data + 1, nl);
                    num[nl] = '\0';
                    n = strtoll(num, NULL, 10);
                }
                payload = eol + 1;
                if (n >= 0 && (unsigned long long)n <=
                                  (unsigned long long)(part->data + part->len - payload)) {
                    plen = (size_t)n;
                    if (agg->random_key == NULL && plen > 0 &&
                        payload + plen <= part->data + part->len) {
                        agg->random_key = (char *)malloc(plen);
                        if (agg->random_key != NULL) {
                            memcpy(agg->random_key, payload, plen);
                            agg->random_key_len = plen;
                        } else
                            agg->err = 1;
                    }
                }
            }
        }
        else if (agg->cmd == CMD_KEYS && part->data[0] == '*') {
            const char *eol = (const char *)memchr(part->data, '\n',
                                                   part->len);
            const char *body;
            size_t header_len;
            long long n = -1;
            if (eol == NULL || eol <= part->data + 1) {
                agg->err = 1;
            } else {
                header_len = (size_t)(eol - (part->data + 1));
                if (header_len > 0 && eol[-1] == '\r')
                    header_len--;
                if (!mt_parse_ll(part->data + 1, header_len, &n) || n < 0) {
                    agg->err = 1;
                } else {
                    body = eol + 1;
                    if (agg->keys_count > SIZE_MAX - (size_t)n ||
                        resp_buf_reserve(&agg->keys_body,
                                         (size_t)(part->data + part->len - body)) != 0) {
                        agg->err = 1;
                    } else {
                        memcpy(agg->keys_body.data + agg->keys_body.len, body,
                               (size_t)(part->data + part->len - body));
                        agg->keys_body.len +=
                            (size_t)(part->data + part->len - body);
                        agg->keys_count += (size_t)n;
                    }
                }
            }
        }
        else if (agg->cmd == CMD_PUBSUB && agg->pubsub_mode != 0) {
            arena ar;
            resp_value v;
            ptrdiff_t used;
            arena_init(&ar, 1024);
            used = resp_parse(part->data, part->len, &v, &ar);
            if (used != (ptrdiff_t)part->len)
                agg->err = 1;
            else if (agg->pubsub_mode == 1 && v.type == RESP_INTEGER)
                agg->sum += v.integer;
            else if (agg->pubsub_mode == 2 && v.type == RESP_ARRAY) {
                size_t i;
                for (i = 0; i < v.count; i++)
                    if (v.items[i].str != NULL &&
                        mt_pubsub_channel_add(agg, v.items[i].str,
                                              v.items[i].len) != 0)
                        agg->err = 1;
            } else if (agg->pubsub_mode == 3 && v.type == RESP_ARRAY) {
                size_t i;
                for (i = 0; i + 1 < v.count; i += 2) {
                    size_t j;
                    if (v.items[i].str == NULL ||
                        v.items[i + 1].type != RESP_INTEGER)
                        continue;
                    for (j = 0; j < agg->pubsub_name_count; j++)
                        if (agg->pubsub_name_lens[j] == v.items[i].len &&
                            memcmp(agg->pubsub_names[j], v.items[i].str,
                                   v.items[i].len) == 0) {
                            agg->pubsub_counts[j] += v.items[i + 1].integer;
                            break;
                        }
                }
            } else {
                agg->err = 1;
            }
            arena_destroy(&ar);
        }
        else if (part->data[0] == ':' && part->len > 2) {
            long long v = strtoll(part->data + 1, NULL, 10);
            if (agg->cmd == CMD_DBSIZE)
                agg->sum += v;
            else if (agg->cmd == CMD_LASTSAVE && v > agg->sum)
                agg->sum = v;
        }
    }
    /* FLUSHDB/SAVE parts are "+OK" (or an error, tracked above). */
}

/* All parts arrived: build the aggregated reply and queue it in pipeline
 * order (home thread only). */
static void mt_agg_finish(server *srv, void *conn, mt_conn_state *st,
                          mt_agg *agg)
{
    mt_task *fin = mt_task_new(conn, agg->home, agg->seq, 1, 0, NULL);
    if (fin != NULL) {
        if (!agg->err && agg->raw != NULL &&
            (agg->cmd == CMD_FLUSHDB || agg->cmd == CMD_FLUSHALL ||
             agg->cmd == CMD_SWAPDB))
            server_repl_stream_forward(agg->home->srv, agg->db_index,
                                       agg->raw, agg->rawlen);
        if (agg->err)
            resp_write_error(&fin->reply,
                             "ERR command failed on a worker", 29);
        else if (agg->cmd == CMD_INFO)
            command_info_render(server_db_at(agg->home->srv, agg->db_index),
                                server_repl_info(
                                    agg->home->ms->workers[0].srv),
                                agg->stats, &fin->reply);
        else if (agg->cmd == CMD_DBSIZE || agg->cmd == CMD_LASTSAVE)
            resp_write_integer(&fin->reply, agg->sum);
        else if (agg->cmd == CMD_RANDOMKEY)
            resp_write_bulk(&fin->reply, agg->random_key,
                            agg->random_key_len);
        else if (agg->cmd == CMD_KEYS) {
            resp_write_array_header(&fin->reply, agg->keys_count);
            if (agg->keys_body.len > 0 &&
                resp_buf_reserve(&fin->reply, agg->keys_body.len) == 0) {
                memcpy(fin->reply.data + fin->reply.len, agg->keys_body.data,
                       agg->keys_body.len);
                fin->reply.len += agg->keys_body.len;
            }
        }
        else if (agg->cmd == CMD_PUBSUB && agg->pubsub_mode == 1) {
            resp_write_integer(&fin->reply, agg->sum);
        }
        else if (agg->cmd == CMD_PUBSUB && agg->pubsub_mode == 2) {
            size_t i;
            resp_write_array_header(&fin->reply, agg->pubsub_channel_count);
            for (i = 0; i < agg->pubsub_channel_count; i++)
                resp_write_bulk(&fin->reply, agg->pubsub_channels[i],
                                agg->pubsub_channel_lens[i]);
        }
        else if (agg->cmd == CMD_PUBSUB && agg->pubsub_mode == 3) {
            size_t i;
            resp_write_array_header(&fin->reply, agg->pubsub_name_count * 2);
            for (i = 0; i < agg->pubsub_name_count; i++) {
                resp_write_bulk(&fin->reply, agg->pubsub_names[i],
                                agg->pubsub_name_lens[i]);
                resp_write_integer(&fin->reply, agg->pubsub_counts[i]);
            }
        }
        else
            resp_write_simple_string(&fin->reply, "OK", 2);
        mt_reorder_insert(st, fin);
        if (st->closing) {
            mt_drain_ready(srv, &agg->home->exec_arena, conn, st, 0);
        } else {
            mt_drain_ready(srv, &agg->home->exec_arena, conn, st, 1);
            (void)server_conn_flush(srv, conn);
        }
    }
    mt_agg_free(agg);
}

/* ------------------------------------------------------------------ */
/* command classification                                              */
/* ------------------------------------------------------------------ */

#define MT_PASS (-3)      /* not classified: legacy inline path */
#define MT_BLOCKED (-2)   /* reserved for commands with no safe mt waiter */
#define MT_LOCAL (-1)     /* keyless: execute on the home worker */
#define MT_CROSSSLOT (-4) /* multi-key command spans workers */

static int mt_is_blocking_pop(uint16_t cmd)
{
    switch (cmd) {
    case CMD_BLPOP:
    case CMD_BRPOP:
    case CMD_BRPOPLPUSH:
    case CMD_BLMOVE:
    case CMD_BLMOVEM:
    case CMD_BLMPOP:
    case CMD_BZPOPMIN:
    case CMD_BZPOPMAX:
    case CMD_BZMPOP:
        return 1;
    default:
        return 0;
    }
}

/* Blocking commands execute on one owner worker so the native session
 * waiter can observe both the list/zset and the writes that wake it. */
static int mt_blocking_target(int nworkers, uint16_t cmd,
                              const resp_value *argv, size_t argc)
{
    size_t kstart = 1;
    size_t kend;
    long long nk;

    switch (cmd) {
    case CMD_BLPOP:
    case CMD_BRPOP:
        if (argc < 3)
            return MT_LOCAL;
        kend = argc - 1;
        break;
    case CMD_BRPOPLPUSH:
        if (argc != 4)
            return MT_LOCAL;
        kend = 3;
        break;
    case CMD_BLMOVE:
        if (argc != 6)
            return MT_LOCAL;
        kend = 3;
        break;
    case CMD_BLMOVEM:
        if (argc < 6)
            return MT_LOCAL;
        kend = 3;
        break;
    case CMD_BLMPOP:
    case CMD_BZMPOP:
        if (argc < 4 || argv[2].str == NULL ||
            !mt_parse_ll(argv[2].str, argv[2].len, &nk) || nk <= 0)
            return MT_LOCAL;
        if ((unsigned long long)nk > (unsigned long long)(argc - 4))
            return MT_LOCAL;
        kstart = 3;
        kend = 3 + (size_t)nk;
        break;
    case CMD_BZPOPMIN:
    case CMD_BZPOPMAX:
        if (argc < 3)
            return MT_LOCAL;
        kend = argc - 1;
        break;
    default:
        return MT_LOCAL;
    }

    {
        int target = -2;
        size_t i;
        for (i = kstart; i < kend; i++) {
            int w;
            if (argv[i].str == NULL)
                return MT_LOCAL;
            w = (int)(hash_slot(argv[i].str, argv[i].len) %
                      (uint32_t)nworkers);
            if (target == -2)
                target = w;
            else if (target != w)
                return MT_CROSSSLOT;
        }
        return target == -2 ? MT_LOCAL : target;
    }
}

/* MIGRATE reads one source key or a KEYS tail. All source keys must remain on
 * one shard; connection affinity then rehomes the connection before the
 * command executes with the full server-owned session hooks. */
static int mt_migrate_target(int nworkers, const resp_value *argv,
                             size_t argc)
{
    size_t first = 3, end = 4, i;
    int target = -2;
    if (argc < 6)
        return MT_LOCAL;
    for (i = 6; i < argc; i++) {
        if (argv[i].str != NULL && argv[i].len == 4 &&
            mt_ci_equal(argv[i].str, argv[i].len, "KEYS")) {
            first = i + 1;
            end = argc;
            break;
        }
    }
    if (first >= argc || end > argc)
        return MT_LOCAL;
    for (i = first; i < end; i++) {
        int w;
        if (argv[i].str == NULL)
            return MT_LOCAL;
        w = (int)(hash_slot(argv[i].str, argv[i].len) %
                  (uint32_t)nworkers);
        if (target == -2)
            target = w;
        else if (target != w)
            return MT_CROSSSLOT;
    }
    return target == -2 ? MT_LOCAL : target;
}

static int mt_is_single_key(uint16_t cmd)
{
    switch (cmd) {
    case CMD_GET:
    case CMD_SET:
    case CMD_SETNX:
    case CMD_GETBIT:
    case CMD_SETBIT:
    case CMD_BITCOUNT:
    case CMD_BITPOS:
    case CMD_BITFIELD:
    case CMD_BITFIELD_RO:
    case CMD_DUMP:
    case CMD_RESTORE:
    case CMD_RESTORE_ASKING:
    case CMD_INCR:
    case CMD_DECR:
    case CMD_APPEND:
    case CMD_STRLEN:
    case CMD_GETDEL:
    case CMD_GETEX:
    case CMD_SETEX:
    case CMD_PSETEX:
    case CMD_GETSET:
    case CMD_SETRANGE:
    case CMD_GETRANGE:
    case CMD_SUBSTR:
    case CMD_MOVE:
    case CMD_INCRBY:
    case CMD_DECRBY:
    case CMD_INCRBYFLOAT:
    case CMD_INCREX:
    case CMD_DELEX:
    case CMD_DIGEST:
    case CMD_EXPIRE:
    case CMD_PEXPIRE:
    case CMD_EXPIREAT:
    case CMD_PEXPIREAT:
    case CMD_TTL:
    case CMD_PTTL:
    case CMD_PERSIST:
    case CMD_TYPE:
    case CMD_EXPIRETIME:
    case CMD_PEXPIRETIME:
    case CMD_HSET:
    case CMD_HMSET:
    case CMD_HGET:
    case CMD_HDEL:
    case CMD_HEXISTS:
    case CMD_HLEN:
    case CMD_HGETALL:
    case CMD_HKEYS:
    case CMD_HVALS:
    case CMD_HMGET:
    case CMD_HINCRBY:
    case CMD_HINCRBYFLOAT:
    case CMD_HSETNX:
    case CMD_HSTRLEN:
    case CMD_HRANDFIELD:
    case CMD_HGETDEL:
    case CMD_HSETEX:
    case CMD_HGETEX:
    case CMD_HEXPIRE:
    case CMD_HPEXPIRE:
    case CMD_HEXPIREAT:
    case CMD_HPEXPIREAT:
    case CMD_HPERSIST:
    case CMD_HTTL:
    case CMD_HPTTL:
    case CMD_HEXPIRETIME:
    case CMD_HPEXPIRETIME:
    case CMD_LPUSH:
    case CMD_RPUSH:
    case CMD_LPUSHX:
    case CMD_RPUSHX:
    case CMD_LPOP:
    case CMD_RPOP:
    case CMD_LLEN:
    case CMD_LRANGE:
    case CMD_LINDEX:
    case CMD_LSET:
    case CMD_LPOS:
    case CMD_LREM:
    case CMD_LTRIM:
    case CMD_LINSERT:
    case CMD_SADD:
    case CMD_SREM:
    case CMD_SISMEMBER:
    case CMD_SMISMEMBER:
    case CMD_SCARD:
    case CMD_SMEMBERS:
    case CMD_SPOP:
    case CMD_SRANDMEMBER:
    case CMD_ZADD:
    case CMD_ZSCORE:
    case CMD_ZCARD:
    case CMD_ZINCRBY:
    case CMD_ZREM:
    case CMD_ZRANGE:
    case CMD_ZREVRANGE:
    case CMD_ZRANK:
    case CMD_ZREVRANK:
    case CMD_ZCOUNT:
    case CMD_ZRANGEBYSCORE:
    case CMD_ZREMRANGEBYSCORE:
    case CMD_ZPOPMIN:
    case CMD_ZPOPMAX:
    case CMD_ZREMRANGEBYRANK:
    case CMD_ZMSCORE:
    case CMD_ZRANDMEMBER:
    case CMD_ZRANGEBYLEX:
    case CMD_ZREVRANGEBYLEX:
    case CMD_ZREMRANGEBYLEX:
    case CMD_ZLEXCOUNT:
    case CMD_ZREVRANGEBYSCORE:
    case CMD_HSCAN:
    case CMD_SSCAN:
    case CMD_ZSCAN:
    case CMD_SORT:
    case CMD_SORT_RO:
    case CMD_PFADD:
    case CMD_GEOADD:
    case CMD_GEODIST:
    case CMD_GEOHASH:
    case CMD_GEOPOS:
    case CMD_GEOSEARCH:
    case CMD_GEORADIUS_RO:
    case CMD_GEORADIUSBYMEMBER_RO:
    case CMD_XADD:
    case CMD_XLEN:
    case CMD_XRANGE:
    case CMD_XREVRANGE:
    case CMD_XDEL:
    case CMD_XTRIM:
    case CMD_XGROUP:
    case CMD_XACK:
    case CMD_XPENDING:
    case CMD_XCLAIM:
    case CMD_XAUTOCLAIM:
    case CMD_XINFO:
    case CMD_XSETID:
    case CMD_ARSET:
    case CMD_ARGET:
    case CMD_ARLEN:
    case CMD_ARCOUNT:
    case CMD_ARGETRANGE:
    case CMD_ARMGET:
    case CMD_ARDEL:
    case CMD_ARDELRANGE:
    case CMD_ARMSET:
    case CMD_ARNEXT:
    case CMD_ARSEEK:
    case CMD_ARINSERT:
    case CMD_ARRING:
    case CMD_ARSCAN:
    case CMD_ARINFO:
    case CMD_ARLASTITEMS:
    case CMD_AROP:
    case CMD_ARGREP:
        return 1;
    default:
        return 0;
    }
}

static int mt_is_keyless(uint16_t cmd)
{
    switch (cmd) {
    case CMD_PING:
    case CMD_ECHO:
    case CMD_AUTH:
    case CMD_SELECT:
    case CMD_CONFIG:
    case CMD_DBSIZE:
    case CMD_FLUSHDB:
    case CMD_TIME:
    case CMD_READONLY:
    case CMD_READWRITE:
    case CMD_ROLE:
    case CMD_RESET:
    case CMD_ASKING:
    case CMD_HELLO:
    case CMD_PFSELFTEST:
    case CMD_COMMAND:
    case CMD_CLIENT:
    case CMD_MEMORY:
    case CMD_SLOWLOG:
    case CMD_BGREWRITEAOF:
    case CMD_QUIT:
    case CMD_SHUTDOWN:
    case CMD_LOLWUT:
        return 1;
    default:
        return 0;
    }
}

static int mt_is_aggregate(uint16_t cmd)
{
    return cmd == CMD_DBSIZE || cmd == CMD_FLUSHDB || cmd == CMD_SAVE ||
           cmd == CMD_BGSAVE || cmd == CMD_LASTSAVE || cmd == CMD_SWAPDB ||
           cmd == CMD_INFO || cmd == CMD_FLUSHALL || cmd == CMD_RANDOMKEY ||
           cmd == CMD_KEYS;
}

/* Multi-key commands: every key must map to the same worker (same rule as
 * cluster CROSSSLOT). Key positions by command:
 *   MGET/DEL/UNLINK/EXISTS/TOUCH -> argv[1..]
 *   SINTERSTORE/SUNIONSTORE/SDIFFSTORE -> argv[1..] (dst + sources)
 *   MSET/MSETNX                  -> argv[1], argv[3], ... (key/value pairs)
 *   SMOVE/RENAME/RENAMENX/RPOPLPUSH -> argv[1], argv[2] (source, destination)
 *   SINTERCARD                   -> argv[2..2+numkeys) (numkeys at argv[1]) */
static int mt_multikey_target(int nworkers, uint16_t cmd,
                              const resp_value *argv, size_t argc)
{
    size_t i;
    size_t kstart = 1, kend;
    int target = -2; /* unset */

    if (argc < 2)
        return MT_LOCAL; /* arity error: let the session report it */

    if (cmd == CMD_ZUNIONSTORE || cmd == CMD_ZINTERSTORE ||
        cmd == CMD_ZDIFFSTORE) {
        long long nk = 0;
        size_t end;
        if (argc < 4 || argv[1].str == NULL ||
            argv[2].str == NULL ||
            !mt_parse_ll(argv[2].str, argv[2].len, &nk) || nk <= 0)
            return MT_LOCAL;
        end = 3 + (size_t)nk;
        if (end > argc)
            end = argc;
        target = (int)(hash_slot(argv[1].str, argv[1].len) %
                       (uint32_t)nworkers);
        for (i = 3; i < end; i++) {
            int w;
            if (argv[i].str == NULL)
                return MT_LOCAL;
            w = (int)(hash_slot(argv[i].str, argv[i].len) %
                      (uint32_t)nworkers);
            if (w != target)
                return MT_CROSSSLOT;
        }
        return target;
    }

    if (cmd == CMD_GEORADIUS || cmd == CMD_GEORADIUSBYMEMBER) {
        size_t start = cmd == CMD_GEORADIUS ? 6u : 5u;
        if (argc < 2 || argv[1].str == NULL)
            return MT_LOCAL;
        target = (int)(hash_slot(argv[1].str, argv[1].len) %
                       (uint32_t)nworkers);
        for (i = start; i + 1 < argc; i++) {
            const char *tok = argv[i].str;
            size_t toklen = argv[i].len;
            int w;
            if (tok == NULL)
                continue;
            if (mt_ci_equal(tok, toklen, "STORE") ||
                mt_ci_equal(tok, toklen, "STOREDIST")) {
                if (argv[i + 1].str == NULL)
                    return MT_LOCAL;
                w = (int)(hash_slot(argv[i + 1].str, argv[i + 1].len) %
                          (uint32_t)nworkers);
                if (w != target)
                    return MT_CROSSSLOT;
                return target;
            }
        }
        return target;
    }
    if (cmd == CMD_GEOSEARCHSTORE) {
        kstart = 1;
        kend = argc > 2 ? 3 : argc;
    }
    if (cmd == CMD_XREAD || cmd == CMD_XREADGROUP) {
        size_t streams = 1;
        size_t start, end;
        while (streams < argc) {
            if (argv[streams].str != NULL &&
                mt_ci_equal(argv[streams].str, argv[streams].len, "STREAMS"))
                break;
            streams++;
        }
        if (streams >= argc || (argc - streams - 1) < 2 ||
            ((argc - streams - 1) & 1u) != 0)
            return MT_LOCAL;
        start = streams + 1;
        end = start + (argc - streams - 1) / 2;
        for (i = start; i < end; i++) {
            int w;
            if (argv[i].str == NULL)
                return MT_LOCAL;
            w = (int)(hash_slot(argv[i].str, argv[i].len) %
                      (uint32_t)nworkers);
            if (target == -2)
                target = w;
            else if (target != w)
                return MT_CROSSSLOT;
        }
        return target == -2 ? MT_LOCAL : target;
    }

    kend = argc;
    if (cmd == CMD_MSETEX) {
        long long nk = 0;
        if (argc < 3 || argv[1].str == NULL ||
            !mt_parse_ll(argv[1].str, argv[1].len, &nk) || nk <= 0)
            return MT_LOCAL;
        kstart = 2;
        kend = 2 + (size_t)nk * 2;
        if (kend > argc)
            kend = argc;
    }
    if (cmd == CMD_SINTERCARD || cmd == CMD_SUNIONCARD ||
        cmd == CMD_SDIFFCARD || cmd == CMD_ZUNION || cmd == CMD_ZINTER ||
        cmd == CMD_ZDIFF || cmd == CMD_ZINTERCARD || cmd == CMD_ZMPOP ||
        cmd == CMD_LMPOP) {
        long long nk = 0;
        if (argv[1].str == NULL || !mt_parse_ll(argv[1].str, argv[1].len, &nk))
            return MT_LOCAL; /* bad numkeys: let the session report it */
        if (nk <= 0)
            return MT_LOCAL;
        kstart = 2;
        kend = 2 + (size_t)nk;
        if (kend > argc)
            kend = argc;
    }
    if (cmd == CMD_ZRANGESTORE) {
        kstart = 1;
        kend = argc > 2 ? 3 : argc;
    }
    if (cmd == CMD_LCS) {
        kstart = 1;
        kend = argc > 2 ? 3 : argc;
    }
    if (cmd == CMD_PFDEBUG) {
        kstart = 2;
        kend = argc > 2 ? 3 : argc;
    }

    for (i = kstart; i < kend; i++) {
        int w;
        if ((cmd == CMD_MSET || cmd == CMD_MSETNX) && (i % 2) == 0)
            continue; /* value position */
        if (cmd == CMD_MSETEX && i >= kstart && ((i - kstart) % 2) == 1)
            continue; /* value position */
        if ((cmd == CMD_SMOVE || cmd == CMD_RENAME ||
             cmd == CMD_RENAMENX || cmd == CMD_RPOPLPUSH ||
             cmd == CMD_LMOVE || cmd == CMD_COPY) && i > 2)
            break; /* only source and destination are keys */
        if (argv[i].str == NULL)
            return MT_LOCAL;
        w = (int)(hash_slot(argv[i].str, argv[i].len) %
                  (uint32_t)nworkers);
        if (target == -2)
            target = w;
        else if (target != w)
            return MT_CROSSSLOT;
    }
    return target == -2 ? MT_LOCAL : target;
}

/* Decide where a command runs. Returns a worker id, MT_LOCAL, MT_BLOCKED or
 * MT_PASS. Blocking pop commands return their key owner here. */
static int mt_classify(int nworkers, uint16_t cmd, const resp_value *argv,
                       size_t argc)
{
    if (mt_is_blocking_pop(cmd))
        return mt_blocking_target(nworkers, cmd, argv, argc);
    if (cmd == CMD_MIGRATE)
        return mt_migrate_target(nworkers, argv, argc);
    if (cmd == CMD_CLUSTER || cmd == CMD_REPLICAOF ||
        cmd == CMD_SLAVEOF || cmd == CMD_SYNC || cmd == CMD_PSYNC)
        return 0; /* worker 0 owns the cluster/replication control plane */
    if (mt_is_single_key(cmd)) {
        size_t keyidx = (cmd == CMD_XGROUP || cmd == CMD_XINFO) ? 2u : 1u;
        if (argc <= keyidx || argv[keyidx].str == NULL)
            return MT_LOCAL; /* arity error: let the local session report it */
        return (int)(hash_slot(argv[keyidx].str, argv[keyidx].len) %
                     (uint32_t)nworkers);
    }
    switch (cmd) {
    case CMD_MGET:
    case CMD_MSET:
    case CMD_MSETNX:
    case CMD_MSETEX:
    case CMD_DEL:
    case CMD_UNLINK:
    case CMD_EXISTS:
    case CMD_TOUCH:
    case CMD_SMOVE:
    case CMD_RENAME:
    case CMD_RENAMENX:
    case CMD_RPOPLPUSH:
    case CMD_LMOVE:
    case CMD_BLMOVE:
    case CMD_BRPOPLPUSH:
    case CMD_BLMOVEM:
    case CMD_BLMPOP:
    case CMD_BZPOPMIN:
    case CMD_BZPOPMAX:
    case CMD_BZMPOP:
    case CMD_LCS:
    case CMD_COPY:
    case CMD_SINTER:
    case CMD_SUNION:
    case CMD_SDIFF:
    case CMD_BITOP:
    case CMD_SINTERCARD:
    case CMD_SUNIONCARD:
    case CMD_SDIFFCARD:
    case CMD_SINTERSTORE:
    case CMD_SUNIONSTORE:
    case CMD_SDIFFSTORE:
    case CMD_ZUNIONSTORE:
    case CMD_ZINTERSTORE:
    case CMD_ZDIFFSTORE:
    case CMD_ZUNION:
    case CMD_ZINTER:
    case CMD_ZDIFF:
    case CMD_ZINTERCARD:
    case CMD_ZMPOP:
    case CMD_LMPOP:
    case CMD_ZRANGESTORE:
    case CMD_PFCOUNT:
    case CMD_PFMERGE:
    case CMD_PFDEBUG:
    case CMD_GEOSEARCHSTORE:
    case CMD_GEORADIUS:
    case CMD_GEORADIUSBYMEMBER:
    case CMD_XREAD:
    case CMD_XREADGROUP:
        return mt_multikey_target(nworkers, cmd, argv, argc);
    default:
        break;
    }
    if (mt_is_keyless(cmd))
        return MT_LOCAL;
    return MT_PASS; /* unknown commands fall through to the legacy path */
}

/* ------------------------------------------------------------------ */
/* worker / acceptor                                                   */
/* ------------------------------------------------------------------ */

/* Make a single raw command blob for a one-command routed task. */
static mt_cmd_blob *mt_blob_one(const char *raw, size_t len)
{
    mt_cmd_blob *b = (mt_cmd_blob *)malloc(sizeof(*b));
    if (b == NULL)
        return NULL;
    b->raw = (char *)malloc(len);
    if (b->raw == NULL) {
        free(b);
        return NULL;
    }
    memcpy(b->raw, raw, len);
    b->len = len;
    return b;
}

/* Parse the decimal cursor accepted by SCAN without narrowing it through a
 * signed type. The high bit is reserved for the mt composite-cursor tag. */
static int mt_scan_parse_u64(const char *s, size_t len, uint64_t *out)
{
    size_t i;
    uint64_t v = 0;
    if (s == NULL || len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        uint64_t digit;
        if (s[i] < '0' || s[i] > '9')
            return 0;
        digit = (uint64_t)(s[i] - '0');
        if (v > (UINT64_MAX - digit) / UINT64_C(10))
            return 0;
        v = v * UINT64_C(10) + digit;
    }
    *out = v;
    return 1;
}

static uint64_t mt_scan_encode_cursor(int worker_id, uint64_t local,
                                      int nworkers)
{
    if (worker_id < 0 || worker_id >= nworkers || worker_id > 127 ||
        local > MT_SCAN_LOCAL_MASK)
        return 0;
    return MT_SCAN_TAG | ((uint64_t)(unsigned)worker_id <<
                          MT_SCAN_WORKER_SHIFT) | local;
}

/* Rebuild SCAN's request with a worker-local cursor. The MATCH/COUNT options
 * remain byte-for-byte equivalent, while the original global cursor is not
 * exposed to the target worker. */
static mt_cmd_blob *mt_scan_blob(const resp_value *argv, size_t argc,
                                 uint64_t local_cursor)
{
    resp_buf b;
    char cursor[32];
    int n;
    size_t i;
    mt_cmd_blob *blob;

    resp_buf_init(&b);
    resp_write_array_header(&b, argc);
    for (i = 0; i < argc; i++) {
        if (i == 1) {
            n = snprintf(cursor, sizeof(cursor), "%llu",
                         (unsigned long long)local_cursor);
            if (n < 0 || (size_t)n >= sizeof(cursor)) {
                resp_buf_free(&b);
                return NULL;
            }
            resp_write_bulk(&b, cursor, (size_t)n);
        } else {
            resp_write_value(&b, &argv[i]);
        }
    }
    if (b.data == NULL || b.len == 0) {
        resp_buf_free(&b);
        return NULL;
    }
    blob = (mt_cmd_blob *)malloc(sizeof(*blob));
    if (blob == NULL) {
        resp_buf_free(&b);
        return NULL;
    }
    blob->raw = b.data;
    blob->len = b.len;
    return blob;
}

/* Convert a worker-local SCAN reply back to the connection's composite
 * cursor. Only the cursor bulk is rewritten; the key array is copied through
 * the RESP value writer without parsing or reallocating individual keys. */
static void mt_scan_rewrite_reply(worker *home, mt_task *t)
{
    arena *ar;
    resp_value v;
    resp_buf rebuilt;
    uint64_t local_next;
    uint64_t global_next;
    ptrdiff_t used;
    char cursor_text[32];
    int n;

    if (home == NULL || t == NULL || t->reply.data == NULL ||
        t->reply.len == 0 || t->scan_nworkers <= 0)
        return;
    if (t->reply.data[0] == '-')
        return;
    ar = &home->exec_arena;
    arena_reset(ar);
    used = resp_parse(t->reply.data, t->reply.len, &v, ar);
    if (used != (ptrdiff_t)t->reply.len || v.type != RESP_ARRAY ||
        v.count != 2 || v.items[0].str == NULL ||
        v.items[1].type != RESP_ARRAY ||
        !mt_scan_parse_u64(v.items[0].str, v.items[0].len, &local_next))
        return;

    if (local_next != 0) {
        global_next = mt_scan_encode_cursor(t->scan_worker, local_next,
                                            t->scan_nworkers);
    } else if (t->scan_worker + 1 < t->scan_nworkers) {
        global_next = mt_scan_encode_cursor(t->scan_worker + 1, 0,
                                            t->scan_nworkers);
    } else {
        global_next = 0;
    }
    n = snprintf(cursor_text, sizeof(cursor_text), "%llu",
                 (unsigned long long)global_next);
    if (n < 0 || (size_t)n >= sizeof(cursor_text))
        return;
    resp_buf_init(&rebuilt);
    resp_write_array_header(&rebuilt, 2);
    resp_write_bulk(&rebuilt, cursor_text, (size_t)n);
    resp_write_value(&rebuilt, &v.items[1]);
    if (rebuilt.data == NULL) {
        resp_buf_free(&rebuilt);
        return;
    }
    resp_buf_free(&t->reply);
    t->reply = rebuilt;
}


/* Execute SWAPDB on one worker: swap the two logical dbs directly (the
 * sessionless path used by the aggregate home part and drain-2). */
static void mt_swapdb_exec(worker *w, int log_db_index, const resp_value *v,
                           resp_buf *dst)
{
    char ta[16], tb[16];
    char *ea, *eb;
    long long ai, bi;
    if (v->count != 3 || v->items[1].str == NULL ||
        v->items[2].str == NULL) {
        resp_write_error(dst, "ERR wrong number of arguments for 'swapdb' "
                              "command",
                         48);
        return;
    }
    if (v->items[1].len >= sizeof(ta) || v->items[2].len >= sizeof(tb)) {
        resp_write_error(dst, "ERR value is not an integer or out of range",
                         43);
        return;
    }
    memcpy(ta, v->items[1].str, v->items[1].len);
    ta[v->items[1].len] = '\0';
    memcpy(tb, v->items[2].str, v->items[2].len);
    tb[v->items[2].len] = '\0';
    ai = strtoll(ta, &ea, 10);
    bi = strtoll(tb, &eb, 10);
    if (*ea != '\0' || *eb != '\0' || ai < 0 || bi < 0 || ai >= 16 ||
        bi >= 16) {
        static const char E[] = "ERR DB index is out of range";
        resp_write_error(dst, E, sizeof(E) - 1);
        return;
    }
    if (ai != bi) {
        db *da = server_db_at(w->srv, (int)ai);
        db *dbb = server_db_at(w->srv, (int)bi);
        db tmp = *da;
        *da = *dbb;
        *dbb = tmp;
        da->flush_epoch++;
        dbb->flush_epoch++;
        da->dirty++;
        server_aof_log_cmd(w->srv, log_db_index, v->items, v->count);
    }
    resp_write_simple_string(dst, "OK", 2);
}

/* Execute MOVE on the worker owning the source key. Unlike the sessionless
 * routed path (which only sees one selected db), a stack session with the
 * selection hook gives MOVE access to every logical db on this worker, so
 * the target-db argument is honored exactly like the legacy path. */
static int mt_move_exec(worker *w, int log_db_index, const resp_value *v,
                        resp_buf *dst)
{
    session sess;
    uint64_t dirty_before;
    int changed = 0;
    session_init(&sess, server_db_at(w->srv, log_db_index));
    sess.sel_ctx = w->srv;
    sess.sel_fn = server_select_db;
    sess.sel_ndbs = server_ndbs(w->srv);
    dirty_before = sess.d->dirty;
    session_execute_at(&sess, v->items, v->count, dst, pal_wall_ms());
    if (sess.d->dirty != dirty_before) {
        server_aof_log_cmd(w->srv, log_db_index, v->items, v->count);
        changed = 1;
    }
    session_release(&sess);
    return changed;
}

/* COPY may name a destination logical DB. Execute it with the worker's
 * selection hook instead of the sessionless one-db routed path. */
static int mt_copy_exec_server(server *srv, int log_db_index,
                               const resp_value *v, resp_buf *dst)
{
    session sess;
    uint64_t dirty_before;
    int changed = 0;
    session_init(&sess, server_db_at(srv, log_db_index));
    sess.sel_ctx = srv;
    sess.sel_fn = server_select_db;
    sess.sel_ndbs = server_ndbs(srv);
    dirty_before = sess.d->dirty;
    session_execute_at(&sess, v->items, v->count, dst, pal_wall_ms());
    if (sess.d->dirty != dirty_before) {
        server_aof_log_cmd(srv, log_db_index, v->items, v->count);
        changed = 1;
    }
    session_release(&sess);
    return changed;
}

static int mt_copy_exec(worker *w, int log_db_index, const resp_value *v,
                        resp_buf *dst)
{
    return mt_copy_exec_server(w->srv, log_db_index, v, dst);
}

/* Execute INFO __STATS__ on this worker with a stack session that sees all
 * of the worker's logical dbs (the sessionless task path only covers the
 * caller's selected db). */
static void mt_info_exec(worker *w, resp_buf *out)
{
    static const char req[] = MT_INFO_STATS_REQ;
    session sess;
    resp_value v;
    arena ar;
    session_init(&sess, server_db_at(w->srv, 0));
    sess.sel_ctx = w->srv;
    sess.sel_fn = server_select_db;
    sess.sel_ndbs = server_ndbs(w->srv);
    sess.io = server_io_counters(w->srv);
    arena_init(&ar, 256);
    if (resp_parse(req, sizeof(req) - 1, &v, &ar) ==
        (ptrdiff_t)(sizeof(req) - 1))
        session_execute_at(&sess, v.items, v.count, out, pal_wall_ms());
    else
        resp_write_error(out, "ERR Protocol error", 18);
    arena_destroy(&ar);
    session_release(&sess);
}

/* Execute FLUSHALL on one worker with a stack session that sees all of the
 * worker's logical dbs (the sessionless task path only covers one db). */
static void mt_flushall_exec(worker *w, int log_db_index, resp_buf *out)
{
    static const char req[] = "*1\r\n$8\r\nFLUSHALL\r\n";
    session sess;
    resp_value v;
    arena ar;
    uint64_t dirty_before;
    session_init(&sess, server_db_at(w->srv, 0));
    sess.sel_ctx = w->srv;
    sess.sel_fn = server_select_db;
    sess.sel_ndbs = server_ndbs(w->srv);
    dirty_before = sess.d->dirty;
    arena_init(&ar, 64);
    if (resp_parse(req, sizeof(req) - 1, &v, &ar) ==
        (ptrdiff_t)(sizeof(req) - 1))
        session_execute_at(&sess, v.items, v.count, out, pal_wall_ms());
    else
        resp_write_error(out, "ERR Protocol error", 18);
    if (sess.d->dirty != dirty_before)
        server_aof_log_cmd(w->srv, log_db_index, v.items, v.count);
    arena_destroy(&ar);
    session_release(&sess);
}

/* Aggregate commands (DBSIZE sum, FLUSHDB broadcast): run the home part
 * inline, fan sub-tasks out to every other worker and finish when all
 * parts arrived. Runs on the home worker thread. */
static int mt_route_aggregate(worker *home, void *conn,
                              const resp_value *argv, size_t argc,
                              const char *raw, size_t rawlen, uint16_t cmd,
                              int db_index)
{
    mt_conn_state *st = (mt_conn_state *)server_conn_mt_state(conn);
    mt_agg *agg;
    resp_buf local;
    uint64_t seq;
    int i;

    if (st == NULL) {
        st = (mt_conn_state *)calloc(1, sizeof(*st));
        if (st == NULL)
            return 0;
        st->batch_target = -1;
        server_conn_set_mt_state(conn, st);
    }
    seq = st->seq_next++;

    agg = (mt_agg *)calloc(1, sizeof(*agg));
    if (agg != NULL && cmd == CMD_INFO) {
        agg->stats = (info_stats *)calloc(1, sizeof(*agg->stats));
        if (agg->stats == NULL) {
            free(agg);
            agg = NULL;
        }
    }
    if (agg == NULL) {
        /* OOM: degrade to the home-worker answer. */
        resp_buf_init(&local);
        command_execute_at(server_db_at(home->srv, db_index), argv, argc,
                           &local, pal_wall_ms());
        (void)server_conn_out_append(home->srv, conn, local.data, local.len);
        resp_buf_free(&local);
        st->seq_write++;
        return 1;
    }
    if (cmd == CMD_FLUSHDB || cmd == CMD_FLUSHALL || cmd == CMD_SWAPDB) {
        agg->raw = (char *)malloc(rawlen);
        if (agg->raw != NULL) {
            memcpy(agg->raw, raw, rawlen);
            agg->rawlen = rawlen;
        }
    }
    if (cmd == CMD_PUBSUB) {
        if (argc < 2 || argv[1].str == NULL)
            return 0;
        if (mt_ci_equal(argv[1].str, argv[1].len, "NUMPAT"))
            agg->pubsub_mode = 1;
        else if (mt_ci_equal(argv[1].str, argv[1].len, "CHANNELS"))
            agg->pubsub_mode = 2;
        else if (mt_ci_equal(argv[1].str, argv[1].len, "NUMSUB"))
            agg->pubsub_mode = 3;
        else
            return 0;
        if (agg->pubsub_mode == 3 && argc > 2) {
            size_t i;
            agg->pubsub_name_count = argc - 2;
            agg->pubsub_names = (char **)calloc(agg->pubsub_name_count,
                                                 sizeof(char *));
            agg->pubsub_name_lens = (size_t *)calloc(agg->pubsub_name_count,
                                                      sizeof(size_t));
            agg->pubsub_counts = (long long *)calloc(agg->pubsub_name_count,
                                                      sizeof(long long));
            if (agg->pubsub_names == NULL || agg->pubsub_name_lens == NULL ||
                agg->pubsub_counts == NULL) {
                mt_agg_free(agg);
                return 0;
            }
            for (i = 0; i < agg->pubsub_name_count; i++) {
                agg->pubsub_names[i] = (char *)malloc(argv[i + 2].len);
                if (agg->pubsub_names[i] == NULL) {
                    mt_agg_free(agg);
                    return 0;
                }
                memcpy(agg->pubsub_names[i], argv[i + 2].str,
                       argv[i + 2].len);
                agg->pubsub_name_lens[i] = argv[i + 2].len;
            }
        }
    }
    agg->conn = conn;
    agg->home = home;
    agg->seq = seq;
    agg->cmd = cmd;
    agg->pending = home->ms->nworkers - 1;
    agg->db_index = db_index;
    if (cmd == CMD_KEYS)
        resp_buf_init(&agg->keys_body);

    /* home part */
    resp_buf_init(&local);
    if (cmd == CMD_PUBSUB) {
        mt_task local_task;
        memset(&local_task, 0, sizeof(local_task));
        local_task.ncmds = 1;
        local_task.cmds = mt_blob_one(raw, rawlen);
        resp_buf_init(&local_task.reply);
        if (local_task.cmds != NULL)
            mt_pubsub_stats_execute(home, &local_task);
        mt_agg_accumulate(agg, &local_task.reply);
        mt_blobs_free(local_task.cmds, 1);
        resp_buf_free(&local_task.reply);
    } else if (cmd == CMD_INFO) {
        mt_info_exec(home, &local);
    } else if (cmd == CMD_FLUSHALL) {
        mt_flushall_exec(home, db_index, &local);
    } else if (cmd == CMD_SWAPDB) {
        resp_value v;
        arena ar;
        arena_init(&ar, 1024);
        if (resp_parse(raw, rawlen, &v, &ar) == (ptrdiff_t)rawlen)
            mt_swapdb_exec(home, db_index, &v, &local);
        else
            resp_write_error(&local, "ERR Protocol error", 18);
        arena_destroy(&ar);
    } else if (cmd == CMD_FLUSHDB) {
        db *d = server_db_at(home->srv, db_index);
        uint64_t dirty_before = d->dirty;
        command_execute_at(d, argv, argc, &local, pal_wall_ms());
        if (d->dirty != dirty_before)
            server_aof_log_cmd(home->srv, db_index, argv, argc);
    } else {
        command_execute_at(server_db_at(home->srv, db_index), argv, argc,
                           &local, pal_wall_ms());
    }
    mt_agg_accumulate(agg, &local);
    resp_buf_free(&local);

    /* fan out */
    for (i = 0; i < home->ms->nworkers; i++) {
        mt_task *t = NULL;
        mt_cmd_blob *blob;
        if (i == home->id)
            continue;
        if (cmd == CMD_PUBSUB)
            blob = mt_blob_one(raw, rawlen);
        else if (cmd == CMD_INFO)
            blob = mt_blob_one(MT_INFO_STATS_REQ,
                               sizeof(MT_INFO_STATS_REQ) - 1);
        else
            blob = mt_blob_one(raw, rawlen);
        if (blob != NULL)
            t = mt_task_new(conn, home, seq, 1, 1, blob);
        if (t != NULL) {
            t->agg = agg;
            if (cmd == CMD_PUBSUB)
                t->kind = MT_TASK_PUBSUB_STATS;
            t->db_index = db_index;
            mt_pending_inc(home, st);
            t->pending_owned = 1;
            mt_push_task(home, &home->ms->workers[i].inbox[home->id], t,
                         &home->ms->workers[i]);
        } else {
            if (blob != NULL)
                mt_blobs_free(blob, 1);
            (void)mt_agg_drop_part(agg);
        }
    }
    agg->fanout_done = 1;
    if (agg->pending == 0) {
        if (agg->abandoned)
            mt_agg_free(agg);
        else
            mt_agg_finish(home->srv, conn, st, agg);
    }
    return 1;
}

/* Pop a task from the home worker's freelist (NULL when empty: caller
 * falls back to the heap path). The reply buffer keeps its capacity. */
static mt_task *mt_pool_task_new(worker *home)
{
    mt_task *t;
    pal_mutex_lock(&home->task_pool_mu);
    t = home->task_pool;
    if (t != NULL) {
        home->task_pool = t->pool_next;
        home->task_pool_n--;
        home->task_pool_hits++;
    }
    pal_mutex_unlock(&home->task_pool_mu);
    if (t == NULL)
        return NULL;
    t->next = NULL;
    t->agg = NULL;
    t->kind = MT_TASK_CMD;
    t->pending_owned = 0;
    t->db_index = 0;
    t->watch_out = NULL;
    t->nwatch_out = 0;
    t->exec_watches = NULL;
    t->nexec_watches = 0;
    t->scan_worker = 0;
    t->scan_nworkers = 0;
    t->reply.len = 0;
    t->pool_next = NULL;
    return t;
}

/* Flush the open merge batch (if any) as one routed task. A single
 * command whose bytes sit in st->batch_inline gets a pooled task with an
 * inline blob (no allocator traffic on the hot path, Phase 31). */
static void mt_batch_flush(worker *home, void *conn, mt_conn_state *st)
{
    int target = st->batch_target;
    mt_task *t;
    if (st->batch_n == 0)
        return;
    if (st->batch_n == 1 && st->batch[0].raw == st->batch_inline) {
        t = mt_pool_task_new(home);
        if (t != NULL) {
            t->conn = conn;
            t->home = home;
            t->seq = st->batch_seq;
            t->span = 1;
            t->ncmds = 1;
            t->cmds = &t->inline_cmd;
            t->db_index = st->batch_db;
            memcpy(t->inline_buf, st->batch_inline, st->batch[0].len);
            t->inline_cmd.raw = t->inline_buf;
            t->inline_cmd.len = st->batch[0].len;
            mt_pending_inc(home, st);
            t->pending_owned = 1;
            mt_push_task(home, &home->ms->workers[target].inbox[home->id],
                         t, &home->ms->workers[target]);
            goto flushed;
        }
        /* pool empty: move the inline bytes to the heap and take the
         * normal path (double OOM: drop, parity with alloc-failure) */
        {
            char *p = (char *)malloc(st->batch[0].len);
            if (p == NULL) {
                free(st->batch); /* inline bytes stay with st */
                goto flushed;
            }
            memcpy(p, st->batch_inline, st->batch[0].len);
            st->batch[0].raw = p;
        }
    }
    t = mt_task_new(conn, home, st->batch_seq, (uint32_t)st->batch_n,
                    (uint32_t)st->batch_n, st->batch);
    if (t == NULL) {
        mt_blobs_free(st->batch, st->batch_n);
    } else {
        t->db_index = st->batch_db;
        mt_pending_inc(home, st);
        t->pending_owned = 1;
        mt_push_task(home, &home->ms->workers[target].inbox[home->id], t,
                     &home->ms->workers[target]);
    }
flushed:
    st->batch = NULL;
    st->batch_n = 0;
    st->batch_cap = 0;
    st->batch_target = -1;
}

/* Append one raw command to the open batch. The first blob's bytes live
 * in st->batch_inline when they fit (no allocation on the hot path);
 * growing past one command materializes it into a heap block. */
static int mt_batch_append(mt_conn_state *st, const char *raw, size_t rawlen)
{
    mt_cmd_blob *b;
    if (st->batch_n == st->batch_cap) {
        size_t ncap = st->batch_cap == 0 ? 8 : st->batch_cap * 2;
        mt_cmd_blob *nb =
            (mt_cmd_blob *)realloc(st->batch, ncap * sizeof(*nb));
        if (nb == NULL)
            return -1;
        st->batch = nb;
        st->batch_cap = ncap;
    }
    if (st->batch_n == 1 && st->batch[0].raw == st->batch_inline) {
        char *p = (char *)malloc(st->batch[0].len);
        if (p == NULL)
            return -1;
        memcpy(p, st->batch_inline, st->batch[0].len);
        st->batch[0].raw = p;
    }
    b = &st->batch[st->batch_n];
    if (st->batch_n == 0 && rawlen <= sizeof(st->batch_inline)) {
        memcpy(st->batch_inline, raw, rawlen);
        b->raw = st->batch_inline;
        b->len = rawlen;
        st->batch_n = 1;
        return 0;
    }
    b->raw = (char *)malloc(rawlen);
    if (b->raw == NULL)
        return -1;
    memcpy(b->raw, raw, rawlen);
    b->len = rawlen;
    st->batch_n++;
    return 0;
}

/* server.c calls this after each conn_process_input parse loop. */
static void mt_route_flush_cb(void *ctx, void *conn)
{
    worker *home = (worker *)ctx;
    mt_conn_state *st = (mt_conn_state *)server_conn_mt_state(conn);
    if (st != NULL)
        mt_batch_flush(home, conn, st);
}

/* ------------------------------------------------------------------ */
/* mt-level transactions (MULTI/EXEC/DISCARD/WATCH/UNWATCH)            */
/* ------------------------------------------------------------------ */

/* Write a locally-produced reply, respecting pipeline order: directly into
 * conn->out when nothing is outstanding, otherwise into the reorder
 * buffer. */
static void mt_reply_local(worker *home, void *conn, mt_conn_state *st,
                           uint64_t seq, const char *data, size_t len,
                           resp_buf *out)
{
    if (seq == st->seq_write) {
        if (resp_buf_reserve(out, len) != 0) {
            return;
        }
        memcpy(out->data + out->len, data, len);
        out->len += len;
        st->seq_write++;
        return;
    }
    {
        mt_task *t = mt_task_new(conn, home, seq, 1, 0, NULL);
        if (t == NULL) {
            if (resp_buf_reserve(out, len) != 0) {
                return;
            }
            memcpy(out->data + out->len, data, len);
            out->len += len;
            st->seq_write++;
            return;
        }
        if (resp_buf_reserve(&t->reply, len) != 0) {
            if (!t->watch_failed)
                mt_task_free(t);
            return;
        }
        memcpy(t->reply.data + t->reply.len, data, len);
        t->reply.len += len;
        mt_reorder_insert(st, t);
        mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
    }
}


static int mt_mq_push(mt_conn_state *st, const char *raw, size_t rawlen)
{
    mt_cmd_blob *b;
    if (st->mq_n == st->mq_cap) {
        size_t ncap = st->mq_cap == 0 ? 8 : st->mq_cap * 2;
        mt_cmd_blob *nb =
            (mt_cmd_blob *)realloc(st->mq, ncap * sizeof(*nb));
        if (nb == NULL)
            return -1;
        st->mq = nb;
        st->mq_cap = ncap;
    }
    b = &st->mq[st->mq_n];
    b->raw = (char *)malloc(rawlen);
    if (b->raw == NULL)
        return -1;
    memcpy(b->raw, raw, rawlen);
    b->len = rawlen;
    st->mq_n++;
    return 0;
}

static void mt_mq_clear(mt_conn_state *st)
{
    mt_blobs_free(st->mq, st->mq_n);
    st->mq = NULL;
    st->mq_n = 0;
    st->mq_cap = 0;
}

/* Execute an EXEC bundle on the given worker's db: validate watches, then
 * replay every queued command, packing the replies into a RESP array.
 * Watch references are released on the owner (all entries map to this
 * worker); applied commands are logged to the worker's own AOF. */
static void mt_exec_on_db(server *srv, mt_task *t, arena *ar)
{
    db *d = server_db_at(srv, t->db_index);
    size_t i;
    int aborted = 0;
    for (i = 0; i < t->nexec_watches; i++) {
        mt_watch_entry *e = &t->exec_watches[i];
        db *wd = server_db_at(srv, e->db_index);
        if (db_key_version(wd, e->key, e->klen) != e->version ||
            wd->flush_epoch != e->epoch)
            aborted = 1;
        /* release the reference on the entry's own db */
        if (wd->watch_refs > 0)
            wd->watch_refs--;
    }
    if (aborted) {
        static const char nullarr[] = "*-1\r\n";
        if (resp_buf_reserve(&t->reply, sizeof(nullarr) - 1) != 0)
            return;
        memcpy(t->reply.data + t->reply.len, nullarr, sizeof(nullarr) - 1);
        t->reply.len += sizeof(nullarr) - 1;
        return;
    }
    resp_write_array_header(&t->reply, t->ncmds);
    for (i = 0; i < t->ncmds; i++) {
        resp_value v;
        ptrdiff_t used;
        uint64_t dirty_before;
        arena_reset(ar);
        used = resp_parse(t->cmds[i].raw, t->cmds[i].len, &v, ar);
        if (used != (ptrdiff_t)t->cmds[i].len || v.type != RESP_ARRAY) {
            resp_write_error(&t->reply, "ERR Protocol error", 18);
            continue;
        }
        dirty_before = d->dirty;
        if (v.count >= 3 && v.items[0].str != NULL &&
            v.items[0].len == 4 &&
            mt_ci_equal(v.items[0].str, v.items[0].len, "copy")) {
            if (mt_copy_exec_server(srv, t->db_index, &v, &t->reply))
                server_repl_stream_forward(srv, t->db_index,
                                           t->cmds[i].raw,
                                           t->cmds[i].len);
            continue;
        }
        command_execute_at(d, v.items, v.count, &t->reply, pal_wall_ms());
        /* EXEC logs the applied commands individually (no MULTI wrapper) */
        if (d->dirty != dirty_before) {
            server_aof_log_cmd(srv, t->db_index, v.items, v.count);
            server_repl_stream_forward(srv, t->db_index,
                                       t->cmds[i].raw, t->cmds[i].len);
        }
    }
}

static int mt_txn_watch(worker *home, void *conn, mt_conn_state *st,
                        uint64_t seq, const resp_value *argv, size_t argc,
                        const char *raw, size_t rawlen, int db_index,
                        resp_buf *out)
{
    static const char ok[] = "+OK\r\n";
    static const char err_in_multi[] =
        "-ERR WATCH inside MULTI is not allowed\r\n";
    static const char err_arity[] =
        "-ERR wrong number of arguments for 'watch' command\r\n";
    static const char crossslot[] =
        "-CROSSSLOT Keys in request don't hash to the same slot\r\n";
    static const char err_oom[] = "-ERR out of memory\r\n";
    int target;

    if (st->in_multi) {
        mt_reply_local(home, conn, st, seq, err_in_multi,
                       sizeof(err_in_multi) - 1, out);
        return 1;
    }
    if (argc < 2) {
        mt_reply_local(home, conn, st, seq, err_arity,
                       sizeof(err_arity) - 1, out);
        return 1;
    }
    target = mt_multikey_target(home->ms->nworkers, CMD_WATCH, argv, argc);
    if (target == MT_CROSSSLOT) {
        mt_reply_local(home, conn, st, seq, crossslot,
                       sizeof(crossslot) - 1, out);
        return 1;
    }
    if (target == home->id || target == MT_LOCAL) {
        /* versions are read on the home worker right now */
        db *d = server_db_at(home->srv, db_index);
        size_t i;
        size_t start = st->nwatch;
        for (i = 1; i < argc; i++) {
            if (argv[i].str == NULL)
                continue;
            if (mt_watch_add(st, argv[i].str, argv[i].len,
                             db_key_version(d, argv[i].str, argv[i].len),
                             d->flush_epoch, db_index) != 0)
            {
                mt_watches_release_suffix(home, st, start);
                mt_reply_local(home, conn, st, seq, err_oom,
                               sizeof(err_oom) - 1, out);
                return 1;
            }
            d->watch_refs++; /* writes to this key now bump its version */
        }
        mt_reply_local(home, conn, st, seq, ok, sizeof(ok) - 1, out);
        return 1;
    }
    /* routed WATCH: versions come back with the task */
    {
        mt_task *t =
            mt_task_new(conn, home, seq, 1, 1, mt_blob_one(raw, rawlen));
        if (t == NULL || t->cmds == NULL) {
            if (t != NULL)
                mt_task_free(t);
            mt_reply_local(home, conn, st, seq, err_oom,
                           sizeof(err_oom) - 1, out);
            return 1;
        }
        t->kind = MT_TASK_WATCH;
        t->db_index = db_index;
        t->watch_home_start = st->nwatch;
        st->watch_pending++;
        mt_pending_inc(home, st);
        t->pending_owned = 1;
        mt_push_task(home, &home->ms->workers[target].inbox[home->id], t,
                     &home->ms->workers[target]);
        return 1;
    }
}

static int mt_txn_exec(worker *home, void *conn, mt_conn_state *st,
                       uint64_t seq, int db_index, resp_buf *out)
{
    static const char empty[] = "*0\r\n";
    static const char err_execabort[] =
        "-EXECABORT Transaction discarded because of: keys hash to "
        "different slots\r\n";
    static const char err_blocked[] =
        "-EXECABORT Transaction discarded because of: blocking command "
        "cannot run in mt transaction\r\n";
    static const char err_oom[] = "-ERR out of memory\r\n";
    arena ar;
    int target = -1;
    int bad = 0;
    int blocked = 0;
    size_t i;
    mt_task *t;

    if (st->mq_n == 0) {
        st->in_multi = 0;
        mt_watches_clear(home, st);
        mt_reply_local(home, conn, st, seq, empty, sizeof(empty) - 1, out);
        return 1;
    }

    /* every queued command and every watched key must map to ONE worker */
    arena_init(&ar, 4096);
    for (i = 0; i < st->mq_n && !bad; i++) {
        resp_value v;
        ptrdiff_t used;
        uint16_t c;
        int tg;
        arena_reset(&ar);
        used = resp_parse(st->mq[i].raw, st->mq[i].len, &v, &ar);
        if (used != (ptrdiff_t)st->mq[i].len || v.type != RESP_ARRAY ||
            v.count == 0 || v.items[0].str == NULL) {
            bad = 1;
            break;
        }
        c = cmd_resolve(v.items[0].str, v.items[0].len);
        if (mt_is_aggregate(c)) {
            bad = 1;
            break;
        }
        if (mt_is_blocking_pop(c)) {
            /* A blocked waiter owns the live connection/session and cannot
             * be replayed safely inside a sessionless EXEC task. */
            bad = 1;
            blocked = 1;
            break;
        }
        tg = mt_classify(home->ms->nworkers, c, v.items, v.count);
        if (tg == MT_BLOCKED || tg == MT_PASS || tg == MT_CROSSSLOT) {
            bad = 1;
            break;
        }
        if (tg >= 0) {
            if (target == -1)
                target = tg;
            else if (target != tg)
                bad = 1;
        }
    }
    for (i = 0; i < st->nwatch && !bad; i++) {
        int tg = (int)(hash_slot(st->watches[i].key, st->watches[i].klen) %
                       (uint32_t)home->ms->nworkers);
        if (target == -1)
            target = tg;
        else if (target != tg)
            bad = 1;
    }
    arena_destroy(&ar);

    if (bad) {
        st->in_multi = 0;
        mt_mq_clear(st);
        mt_watches_clear(home, st);
        mt_reply_local(home, conn, st, seq,
                       blocked ? err_blocked : err_execabort,
                       blocked ? sizeof(err_blocked) - 1
                               : sizeof(err_execabort) - 1,
                       out);
        return 1;
    }
    if (target == -1)
        target = home->id; /* keyless transaction */

    t = mt_task_new(conn, home, seq, 1, (uint32_t)st->mq_n, st->mq);
    if (t == NULL) {
        st->mq = NULL;
        st->mq_n = 0;
        st->mq_cap = 0;
        st->in_multi = 0;
        mt_watches_clear(home, st);
        mt_reply_local(home, conn, st, seq, err_oom, sizeof(err_oom) - 1,
                       out);
        return 1;
    }
    t->kind = MT_TASK_EXEC;
    t->db_index = db_index;
    t->exec_watches = st->watches;
    t->nexec_watches = st->nwatch;
    st->mq = NULL;
    st->mq_n = 0;
    st->mq_cap = 0;
    st->watches = NULL;
    st->nwatch = 0;
    st->watch_cap = 0;
    st->in_multi = 0;

    if (target == home->id) {
        mt_exec_on_db(home->srv, t, &home->exec_arena);
        mt_reorder_insert(st, t);
        mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
        return 1;
    }
    mt_pending_inc(home, st);
    t->pending_owned = 1;
    mt_push_task(home, &home->ms->workers[target].inbox[home->id], t,
                 &home->ms->workers[target]);
    return 1;
}

static int mt_route_txn(worker *home, void *conn, mt_conn_state *st,
                        const resp_value *argv, size_t argc, const char *raw,
                        size_t rawlen, uint16_t cmd, int db_index,
                        uint64_t seq, resp_buf *out)
{
    static const char queued[] = "+QUEUED\r\n";
    static const char ok[] = "+OK\r\n";
    static const char err_nested[] = "-ERR MULTI calls can not be nested\r\n";
    static const char err_no_exec[] = "-ERR EXEC without MULTI\r\n";
    static const char err_no_discard[] = "-ERR DISCARD without MULTI\r\n";
    static const char err_oom[] = "-ERR out of memory\r\n";

    if (st->watch_pending != 0) {
        mt_deferred_cmd *d = (mt_deferred_cmd *)calloc(1, sizeof(*d));
        if (d == NULL) {
            mt_reply_local(home, conn, st, seq, err_oom,
                           sizeof(err_oom) - 1, out);
            return 1;
        }
        d->raw = (char *)malloc(rawlen);
        if (d->raw == NULL) {
            free(d);
            mt_reply_local(home, conn, st, seq, err_oom,
                           sizeof(err_oom) - 1, out);
            return 1;
        }
        memcpy(d->raw, raw, rawlen);
        d->rawlen = rawlen;
        d->db_index = db_index;
        d->cmd = cmd;
        d->seq = seq;
        d->authed = 1;
        if (st->deferred_tail != NULL)
            st->deferred_tail->next = d;
        else
            st->deferred_head = d;
        st->deferred_tail = d;
        return 1;
    }
    switch (cmd) {
    case CMD_MULTI:
        if (st->in_multi) {
            mt_reply_local(home, conn, st, seq, err_nested,
                           sizeof(err_nested) - 1, out);
            return 1;
        }
        st->in_multi = 1;
        mt_reply_local(home, conn, st, seq, ok, sizeof(ok) - 1, out);
        return 1;
    case CMD_DISCARD:
        if (!st->in_multi) {
            mt_reply_local(home, conn, st, seq, err_no_discard,
                           sizeof(err_no_discard) - 1, out);
            return 1;
        }
        st->in_multi = 0;
        mt_mq_clear(st);
        mt_watches_clear(home, st);
        mt_reply_local(home, conn, st, seq, ok, sizeof(ok) - 1, out);
        return 1;
    case CMD_UNWATCH:
        mt_watches_clear(home, st);
        mt_reply_local(home, conn, st, seq, ok, sizeof(ok) - 1, out);
        return 1;
    case CMD_WATCH:
        return mt_txn_watch(home, conn, st, seq, argv, argc, raw, rawlen,
                            db_index, out);
    case CMD_EXEC:
        if (!st->in_multi) {
            mt_reply_local(home, conn, st, seq, err_no_exec,
                           sizeof(err_no_exec) - 1, out);
            return 1;
        }
        return mt_txn_exec(home, conn, st, seq, db_index, out);
    default:
        break;
    }

    /* inside MULTI: queue the command (validated at EXEC time) */
    if (mt_mq_push(st, raw, rawlen) != 0) {
        mt_reply_local(home, conn, st, seq, err_oom, sizeof(err_oom) - 1,
                       out);
        return 1;
    }
    mt_reply_local(home, conn, st, seq, queued, sizeof(queued) - 1, out);
    return 1;
}

static void mt_replay_deferred(worker *home, void *conn, mt_conn_state *st)
{
    while (st->watch_pending == 0 && st->deferred_head != NULL &&
           !st->closing) {
        mt_deferred_cmd *d = st->deferred_head;
        resp_value v;
        arena ar;
        resp_buf out;
        session sess;
        ptrdiff_t used;
        st->deferred_head = d->next;
        if (st->deferred_head == NULL)
            st->deferred_tail = NULL;
        arena_init(&ar, 1024);
        resp_buf_init(&out);
        st->replaying_deferred = 1;
        used = resp_parse(d->raw, d->rawlen, &v, &ar);
        if (used == (ptrdiff_t)d->rawlen && v.type == RESP_ARRAY) {
            uint64_t saved_next = st->seq_next;
            memset(&sess, 0, sizeof(sess));
            sess.authed = d->authed;
            sess.db_index = d->db_index;
            st->seq_next = d->seq;
            (void)mt_route(home, conn, &sess, v.items, v.count, d->raw,
                           d->rawlen, &out);
            mt_batch_flush(home, conn, st);
            st->seq_next = saved_next;
        }
        else
            mt_reply_local(home, conn, st, d->seq, "-ERR Protocol error\r\n",
                           20, &out);
        st->replaying_deferred = 0;
        if (out.len != 0)
            (void)server_conn_out_append(home->srv, conn, out.data, out.len);
        resp_buf_free(&out);
        arena_destroy(&ar);
        free(d->raw);
        free(d);
    }
}

/* ------------------------------------------------------------------ */
/* pub/sub routing (channel owner = hash_slot(channel) % nworkers)      */
/* ------------------------------------------------------------------ */

static void mt_write_sub_reply(resp_buf *b, const char *verb, size_t vlen,
                               const char *ch, size_t chlen, size_t nsub)
{
    resp_write_array_header(b, 3);
    resp_write_bulk(b, verb, vlen);
    if (ch != NULL)
        resp_write_bulk(b, ch, chlen);
    else
        resp_write_bulk(b, NULL, 0); /* $-1 (UNSUBSCRIBE with no channels) */
    resp_write_integer(b, (long long)nsub);
}

/* Send a register task for (conn, channel) to the channel owner. */
static void mt_pubsub_register(worker *home, void *conn, mt_conn_state *st,
                               const char *ch, size_t chlen, int owner,
                               int pattern)
{
    mt_task *t = mt_task_new(conn, home, 0, 1, 1, mt_blob_one(ch, chlen));
    if (t == NULL || t->cmds == NULL) {
        if (t != NULL)
            mt_task_free(t);
        return;
    }
    t->kind = MT_TASK_SUB;
    t->pubsub_pattern = pattern;
    mt_pending_inc(home, st);
    t->pending_owned = 1;
    mt_push_task(home, &home->ms->workers[owner].inbox[home->id], t,
                 &home->ms->workers[owner]);
}

/* Fire-and-forget unregister for (conn, channel) on the owner. */
static void mt_pubsub_unregister(worker *home, void *conn, const char *ch,
                                 size_t chlen, int owner, int pattern)
{
    mt_task *t = mt_task_new(conn, home, 0, 1, 1, mt_blob_one(ch, chlen));
    if (t == NULL || t->cmds == NULL) {
        if (t != NULL)
            mt_task_free(t);
        return;
    }
    t->kind = MT_TASK_UNSUB;
    t->pubsub_pattern = pattern;
    mt_push_task(home, &home->ms->workers[owner].inbox[home->id], t,
                 &home->ms->workers[owner]);
}

static int mt_route_subscribe_kind(worker *home, void *conn, mt_conn_state *st,
                                   const resp_value *argv, size_t argc,
                                   uint64_t seq, resp_buf *out, int pattern)
{
    const char *verb = pattern == 1 ? "psubscribe" :
                       (pattern == 2 ? "ssubscribe" : "subscribe");
    resp_buf reply;
    size_t i;
    if (argc < 2) {
        static const char arity[] =
            "-ERR wrong number of arguments for 'subscribe' command\r\n";
        mt_reply_local(home, conn, st, seq, arity, sizeof(arity) - 1, out);
        return 1;
    }
    resp_buf_init(&reply);
    for (i = 1; i < argc; i++) {
        int owner;
        if (argv[i].str == NULL)
            continue;
        owner = (int)(hash_slot(argv[i].str, argv[i].len) %
                      (uint32_t)home->ms->nworkers);
        if (mt_conn_sub_find_kind(st, argv[i].str, argv[i].len, pattern) == NULL) {
            if (mt_conn_sub_add(st, argv[i].str, argv[i].len, owner,
                                pattern) == 0)
                mt_pubsub_register(home, conn, st, argv[i].str,
                                   argv[i].len, owner, pattern);
        }
        mt_write_sub_reply(&reply, verb, strlen(verb), argv[i].str,
                           argv[i].len, st->nsub);
    }
    mt_reply_local(home, conn, st, seq, reply.data, reply.len, out);
    resp_buf_free(&reply);
    return 1;
}

static int mt_route_subscribe(worker *home, void *conn, mt_conn_state *st,
                              const resp_value *argv, size_t argc,
                              uint64_t seq, resp_buf *out)
{
    return mt_route_subscribe_kind(home, conn, st, argv, argc, seq, out, 0);
}

static int mt_route_unsubscribe_kind(worker *home, void *conn, mt_conn_state *st,
                                     const resp_value *argv, size_t argc,
                                     uint64_t seq, resp_buf *out, int pattern)
{
    const char *verb = pattern == 1 ? "punsubscribe" :
                       (pattern == 2 ? "sunsubscribe" : "unsubscribe");
    resp_buf reply;
    int emitted = 0;
    resp_buf_init(&reply);
    if (argc >= 2) {
        size_t i;
        for (i = 1; i < argc; i++) {
            if (argv[i].str == NULL)
                continue;
            mt_conn_sub *found = mt_conn_sub_find_kind(st, argv[i].str,
                                                       argv[i].len, pattern);
            if (found != NULL && mt_conn_sub_remove(st, argv[i].str,
                                                    argv[i].len, pattern)) {
                int owner = (int)(hash_slot(argv[i].str, argv[i].len) %
                                  (uint32_t)home->ms->nworkers);
                mt_pubsub_unregister(home, conn, argv[i].str, argv[i].len,
                                     owner, pattern);
            }
            mt_write_sub_reply(&reply, verb, strlen(verb), argv[i].str,
                               argv[i].len, st->nsub);
        }
    } else {
        /* unsubscribe everything */
        if (st->subs == NULL) {
            mt_write_sub_reply(&reply, verb, strlen(verb), NULL, 0, 0);
        } else {
            mt_conn_sub *prev = NULL;
            mt_conn_sub *cur = st->subs;
            while (cur != NULL) {
                mt_conn_sub *next = cur->next;
                if (cur->pattern != pattern) {
                    prev = cur;
                    cur = next;
                    continue;
                }
                mt_pubsub_unregister(home, conn, cur->ch, cur->chlen,
                                     cur->owner, pattern);
                mt_write_sub_reply(&reply, verb, strlen(verb), cur->ch,
                                   cur->chlen, st->nsub - 1);
                emitted = 1;
                if (prev != NULL)
                    prev->next = next;
                else
                    st->subs = next;
                if (st->nsub > 0)
                    st->nsub--;
                free(cur->ch);
                free(cur);
                cur = next;
            }
            if (!emitted)
                mt_write_sub_reply(&reply, verb, strlen(verb), NULL, 0,
                                   st->nsub);
        }
    }
    mt_reply_local(home, conn, st, seq, reply.data, reply.len, out);
    resp_buf_free(&reply);
    return 1;
}

static int mt_route_unsubscribe(worker *home, void *conn, mt_conn_state *st,
                                const resp_value *argv, size_t argc,
                                uint64_t seq, resp_buf *out)
{
    return mt_route_unsubscribe_kind(home, conn, st, argv, argc, seq, out, 0);
}

/* Execute PUBLISH on the channel owner: fan the message out to every
 * subscriber's home worker and reply with the receiver count. */
static void mt_publish_execute(worker *owner_w, mt_task *t)
{
    resp_value v;
    ptrdiff_t used;
    mt_sub_entry *e;
    long receivers = 0;

    arena_reset(&owner_w->exec_arena);
    used = resp_parse(t->cmds[0].raw, t->cmds[0].len, &v,
                      &owner_w->exec_arena);
    if (used != (ptrdiff_t)t->cmds[0].len || v.type != RESP_ARRAY ||
        v.count < 3 || v.items[1].str == NULL) {
        resp_write_error(&t->reply, "ERR Protocol error", 18);
        return;
    }
    for (e = owner_w->subs; e != NULL; e = e->next) {
        worker *sh;
        mt_conn_state *sst;
        mt_task *d;
        if (t->pubsub_pattern == 2 ? e->pattern != 2
                                   : e->pattern == 2)
            continue;
        if (e->pattern == 1 && !ddup_glob_match(e->ch, e->chlen,
                                                v.items[1].str, v.items[1].len))
            continue;
        if (e->pattern != 1 && (e->chlen != v.items[1].len ||
                                memcmp(e->ch, v.items[1].str, e->chlen) != 0))
            continue;
        sh = &owner_w->ms->workers[e->home_id];
        sst = (mt_conn_state *)server_conn_mt_state(e->conn);
        if (sst == NULL || !mt_pending_inc_if_open(sh, sst))
            continue;
        /* payload: the full "message" push frame, plus the channel blob so
         * the home side can re-check the subscription at delivery time */
        d = mt_task_new(e->conn, sh, 0, 1, 1,
                        mt_blob_one(e->ch, e->chlen));
        if (d == NULL || d->cmds == NULL) {
            if (d != NULL)
                mt_task_free(d);
            mt_pending_dec(sh, e->conn, sst);
            continue;
        }
        d->kind = MT_TASK_PUSH;
        d->pubsub_pattern = e->pattern;
        d->pending_owned = 1;
        resp_write_array_header(&d->reply, e->pattern == 1 ? 4 : 3);
        if (e->pattern == 1)
            resp_write_bulk(&d->reply, "pmessage", 8);
        else if (e->pattern == 2)
            resp_write_bulk(&d->reply, "smessage", 8);
        else
            resp_write_bulk(&d->reply, "message", 7);
        if (e->pattern == 1)
            resp_write_bulk(&d->reply, e->ch, e->chlen);
        resp_write_bulk(&d->reply, v.items[1].str, v.items[1].len);
        resp_write_bulk(&d->reply,
                        v.items[2].str == NULL ? "" : v.items[2].str,
                        v.items[2].len);
        mt_push_task(owner_w, &sh->completions[owner_w->id], d, sh);
        receivers++;
    }
    resp_write_integer(&t->reply, receivers);
}

static int mt_route_publish(worker *home, void *conn, mt_conn_state *st,
                            const resp_value *argv, size_t argc,
                            const char *raw, size_t rawlen, uint64_t seq,
                            resp_buf *out, int kind)
{
    static const char arity[] =
        "-ERR wrong number of arguments for 'publish' command\r\n";
    static const char err_oom[] = "-ERR out of memory\r\n";
    int owner;
    mt_task *t;

    if (argc < 3 || argv[1].str == NULL) {
        mt_reply_local(home, conn, st, seq, arity, sizeof(arity) - 1, out);
        return 1;
    }
    owner = (int)(hash_slot(argv[1].str, argv[1].len) %
                  (uint32_t)home->ms->nworkers);
    t = mt_task_new(conn, home, seq, 1, 1, mt_blob_one(raw, rawlen));
    if (t == NULL || t->cmds == NULL) {
        if (t != NULL)
            mt_task_free(t);
        mt_reply_local(home, conn, st, seq, err_oom, sizeof(err_oom) - 1,
                       out);
        return 1;
    }
    t->kind = MT_TASK_PUBLISH;
    t->pubsub_pattern = kind;
    if (owner == home->id) {
        mt_publish_execute(home, t);
        mt_reorder_insert(st, t);
        mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
        return 1;
    }
    mt_pending_inc(home, st);
    t->pending_owned = 1;
    mt_push_task(home, &home->ms->workers[owner].inbox[home->id], t,
                 &home->ms->workers[owner]);
    return 1;
}

/* Route one SCAN call to the shard encoded in its composite cursor. A
 * malformed cursor returns 0 so the normal session path preserves Redis's
 * syntax/type error wording. */
static int mt_route_scan(worker *home, void *conn, mt_conn_state *st,
                         const resp_value *argv, size_t argc, uint64_t seq,
                         int db_index, resp_buf *out)
{
    uint64_t cursor;
    uint64_t local_cursor;
    int target;
    mt_cmd_blob *blob;
    mt_task *t;

    if (argc < 2 || argv[1].str == NULL ||
        !mt_scan_parse_u64(argv[1].str, argv[1].len, &cursor))
        return 0;
    if (cursor == 0) {
        target = 0;
        local_cursor = 0;
    } else if ((cursor & MT_SCAN_TAG) != 0) {
        target = (int)((cursor >> MT_SCAN_WORKER_SHIFT) & UINT64_C(0x7f));
        local_cursor = cursor & MT_SCAN_LOCAL_MASK;
        if (target < 0 || target >= home->ms->nworkers)
            return 0;
    } else {
        /* Accept a plain Redis cursor as a worker-0 cursor for compatibility
         * with clients that reconnect to an mt server mid-iteration. */
        target = 0;
        local_cursor = cursor;
    }
    if (local_cursor > MT_SCAN_LOCAL_MASK)
        return 0;

    mt_batch_flush(home, conn, st);
    blob = mt_scan_blob(argv, argc, local_cursor);
    if (blob == NULL) {
        mt_reply_local(home, conn, st, seq, "-ERR out of memory\r\n", 18,
                       out);
        return 1;
    }
    t = mt_task_new(conn, home, seq, 1, 1, blob);
    if (t == NULL) {
        mt_blobs_free(blob, 1);
        mt_reply_local(home, conn, st, seq, "-ERR out of memory\r\n", 18,
                       out);
        return 1;
    }
    t->kind = MT_TASK_SCAN;
    t->scan_worker = target;
    t->scan_nworkers = home->ms->nworkers;
    t->db_index = db_index;
    mt_pending_inc(home, st);
    t->pending_owned = 1;
    mt_push_task(home, &home->ms->workers[target].inbox[home->id], t,
                 &home->ms->workers[target]);
    return 1;
}

/* Router installed on every worker's server. Runs on the home worker thread
 * inside conn_process_input. Returns non-zero when the command was handled
 * (locally, waiter-owned, or forwarded). */
static int mt_route(void *ctx, void *conn, session *sess,
                    const resp_value *argv, size_t argc, const char *raw,
                    size_t rawlen, resp_buf *out)
{
    static const char blocked_msg[] = "ERR command not supported in mt mode";
    static const char crossslot_msg[] =
        "CROSSSLOT Keys in request don't hash to the same slot";
    worker *home = (worker *)ctx;
    mt_conn_state *st;
    uint16_t cmd;
    int target;
    uint64_t seq;

    if (argc == 0 || argv[0].str == NULL)
        return 0;
    cmd = cmd_resolve(argv[0].str, argv[0].len);

    st = (mt_conn_state *)server_conn_mt_state(conn);

    /* AUTH gate: unauthenticated conns may only run AUTH and QUIT; routed
     * tasks are trusted once the home session is authenticated */
    if (!sess->authed && cmd != CMD_AUTH && cmd != CMD_QUIT &&
        cmd != CMD_RESET && cmd != CMD_HELLO) {
        static const char noauth[] = "-NOAUTH Authentication required.\r\n";
        uint64_t seq;
        if (st == NULL) {
            st = (mt_conn_state *)calloc(1, sizeof(*st));
            if (st == NULL)
                return 0;
            st->batch_target = -1;
            server_conn_set_mt_state(conn, st);
        }
        mt_batch_flush(home, conn, st);
        seq = st->seq_next++;
        mt_reply_local(home, conn, st, seq, noauth, sizeof(noauth) - 1,
                       out);
        return 1;
    }
    if (st == NULL) {
        st = (mt_conn_state *)calloc(1, sizeof(*st));
        if (st == NULL)
            return 0;
        st->batch_target = -1;
        server_conn_set_mt_state(conn, st);
    }

    /* A replica handshake must live on worker 0, where the aggregated
     * snapshot serializer and the central downstream backlog own the
     * connection. Move clean handshake conns from any other worker. */
    if ((cmd == CMD_SYNC || cmd == CMD_PSYNC) && home->id != 0) {
        static const char sync_migrate_msg[] =
            "-ERR SYNC must run on worker 0\r\n";
        mt_batch_flush(home, conn, st);
        if (server_backend(home->srv) != SERVER_BACKEND_IOCP &&
            server_backend(home->srv) != SERVER_BACKEND_IOURING_OP &&
            !sess->repl_link && !st->migrated && st->pending == 0 &&
            st->reorder == NULL && st->batch_n == 0 && !st->in_multi &&
            st->nwatch == 0 && st->subs == NULL && !st->closing &&
            st->deferred_head == NULL && st->watch_pending == 0) {
            st->migrated = 1;
            if (server_conn_detach(home->srv, conn) == 0) {
                mt_task *t = mt_task_new(conn, &home->ms->workers[0], 0, 1,
                                         0, NULL);
                if (t != NULL) {
                    t->kind = MT_TASK_MIGRATE;
                    mt_push_task(home,
                                 &home->ms->workers[0].migrate[home->id],
                                 t, &home->ms->workers[0]);
                    return 2;
                }
                (void)server_conn_rehome(home->srv, conn);
                if (server_conn_adopt(home->srv, conn) != 0) {
                    server_conn_free_now(home->srv, conn);
                    return 2;
                }
            }
            st->migrated = 0;
        }
        seq = st->seq_next++;
        mt_reply_local(home, conn, st, seq, sync_migrate_msg,
                       sizeof(sync_migrate_msg) - 1, out);
        return 1;
    }

    if (cmd == CMD_RESET) {
        mt_batch_flush(home, conn, st);
        st->in_multi = 0;
        mt_mq_clear(st);
        mt_watches_clear(home, st);
        mt_deferred_free(st->deferred_head);
        st->deferred_head = NULL;
        st->deferred_tail = NULL;
        st->watch_pending = 0;
        st->replaying_deferred = 0;
        while (st->subs != NULL) {
            mt_conn_sub *sub = st->subs;
            mt_pubsub_unregister(home, conn, sub->ch, sub->chlen,
                                 sub->owner, sub->pattern);
            st->subs = sub->next;
            if (st->nsub > 0)
                st->nsub--;
            free(sub->ch);
            free(sub);
        }
    }

    if (cmd == CMD_MULTI || cmd == CMD_EXEC || cmd == CMD_DISCARD ||
        cmd == CMD_WATCH || cmd == CMD_UNWATCH || st->in_multi ||
        st->watch_pending != 0 || (st->deferred_head != NULL &&
                                   !st->replaying_deferred)) {
        mt_batch_flush(home, conn, st);
        return mt_route_txn(home, conn, st, argv, argc, raw, rawlen, cmd,
                            sess->db_index, st->seq_next++, out);
    }

    if (cmd == CMD_SCAN) {
        seq = st->seq_next++;
        if (mt_route_scan(home, conn, st, argv, argc, seq, sess->db_index,
                          out))
            return 1;
        st->seq_next--; /* malformed SCAN remains on the normal path */
        return 0;
    }

    if (cmd == CMD_SUBSCRIBE || cmd == CMD_UNSUBSCRIBE ||
        cmd == CMD_PSUBSCRIBE || cmd == CMD_PUNSUBSCRIBE ||
        cmd == CMD_SSUBSCRIBE || cmd == CMD_SUNSUBSCRIBE ||
        cmd == CMD_PUBLISH || cmd == CMD_SPUBLISH) {
        uint64_t seq;
        mt_batch_flush(home, conn, st);
        seq = st->seq_next++;
        if (cmd == CMD_SUBSCRIBE)
            return mt_route_subscribe(home, conn, st, argv, argc, seq,
                                      out);
        if (cmd == CMD_PSUBSCRIBE)
            return mt_route_subscribe_kind(home, conn, st, argv, argc, seq,
                                           out, 1);
        if (cmd == CMD_UNSUBSCRIBE)
            return mt_route_unsubscribe(home, conn, st, argv, argc, seq,
                                        out);
        if (cmd == CMD_PUNSUBSCRIBE)
            return mt_route_unsubscribe_kind(home, conn, st, argv, argc, seq,
                                             out, 1);
        if (cmd == CMD_SSUBSCRIBE)
            return mt_route_subscribe_kind(home, conn, st, argv, argc, seq,
                                           out, 2);
        if (cmd == CMD_SUNSUBSCRIBE)
            return mt_route_unsubscribe_kind(home, conn, st, argv, argc, seq,
                                             out, 2);
        return mt_route_publish(home, conn, st, argv, argc, raw, rawlen,
                                seq, out, cmd == CMD_SPUBLISH ? 2 : 0);
    }

    if (cmd == CMD_DBSIZE || cmd == CMD_FLUSHDB || cmd == CMD_SAVE ||
        cmd == CMD_LASTSAVE || cmd == CMD_SWAPDB || cmd == CMD_INFO ||
        cmd == CMD_FLUSHALL || cmd == CMD_RANDOMKEY || cmd == CMD_KEYS ||
        cmd == CMD_PUBSUB) {
        mt_batch_flush(home, conn, st);
        return mt_route_aggregate(home, conn, argv, argc, raw, rawlen, cmd,
                                  sess->db_index);
    }

    if (cmd == CMD_REPLICAOF || cmd == CMD_SLAVEOF) {
        mt_batch_flush(home, conn, st);
        seq = st->seq_next++;
        if (seq == st->seq_write) {
            mt_route_replicaof(home, argv, argc, out);
            st->seq_write++;
        } else {
            mt_task *t = mt_task_new(conn, home, seq, 1, 0, NULL);
            if (t == NULL) {
                resp_write_error(out, "ERR out of memory", 17);
                st->seq_write++;
            } else {
                mt_route_replicaof(home, argv, argc, &t->reply);
                mt_reorder_insert(st, t);
                mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
            }
        }
        return 1;
    }

    if (cmd == CMD_CLUSTER && home->id == 0) {
        mt_batch_flush(home, conn, st);
        seq = st->seq_next++;
        if (seq == st->seq_write) {
            session_execute(sess, argv, argc, out);
            st->seq_write++;
            mt_cluster_sync(home);
        } else {
            mt_task *t = mt_task_new(conn, home, seq, 1, 0, NULL);
            if (t == NULL) {
                resp_write_error(out, "ERR out of memory", 17);
                st->seq_write++;
            } else {
                session_execute(sess, argv, argc, &t->reply);
                mt_reorder_insert(st, t);
                mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
                mt_cluster_sync(home);
            }
        }
        return 1;
    }

    target = mt_classify(home->ms->nworkers, cmd, argv, argc);
    if (target == MT_PASS)
        return 0; /* legacy inline path (SINTER/SUNION/SDIFF for now) */

    if (cmd == CMD_MIGRATE && target == MT_CROSSSLOT) {
        mt_batch_flush(home, conn, st);
        seq = st->seq_next++;
        mt_reply_local(home, conn, st, seq, crossslot_msg,
                       sizeof(crossslot_msg) - 1, out);
        return 1;
    }

    if (cmd == CMD_SHUTDOWN || (cmd == CMD_MIGRATE && target == home->id)) {
        /* SHUTDOWN has no RESP reply. MIGRATE executes on the source shard
         * with server-owned networking hooks and a bounded timeout. */
        mt_batch_flush(home, conn, st);
        seq = st->seq_next++;
        if (seq == st->seq_write) {
            session_execute(sess, argv, argc, out);
            st->seq_write++;
        } else {
            mt_task *t = mt_task_new(conn, home, seq, 1, 0, NULL);
            if (t == NULL) {
                resp_write_error(out, "ERR out of memory", 17);
                st->seq_write++;
            } else {
                session_execute(sess, argv, argc, &t->reply);
                mt_reorder_insert(st, t);
                mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
            }
        }
        if (cmd == CMD_SHUTDOWN && server_shutdown_requested(home->srv)) {
            int wi;
            home->ms->running = 0;
            for (wi = 0; wi < home->ms->nworkers; wi++) {
                home->ms->workers[wi].running = 0;
                if (wi != home->id)
                    mt_kick(&home->ms->workers[wi]);
            }
        }
        return 1;
    }

    seq = st->seq_next++;

    if (mt_is_blocking_pop(cmd) && target >= 0 && target != home->id &&
        (server_backend(home->srv) == SERVER_BACKEND_IOCP ||
         server_backend(home->srv) == SERVER_BACKEND_IOURING_OP)) {
        static const char blocked_backend[] =
            "-ERR blocking commands require a migratable mt connection\r\n";
        mt_batch_flush(home, conn, st);
        mt_reply_local(home, conn, st, seq, blocked_backend,
                       sizeof(blocked_backend) - 1, out);
        return 1;
    }

    /* A remote COPY gets a full worker-local session so DB <n> is honored
     * even when the connection cannot be migrated (pipeline/proactor). */
    if (cmd == CMD_COPY && target >= 0 && target != home->id) {
        mt_task *t;
        mt_cmd_blob *blob;
        mt_batch_flush(home, conn, st);
        blob = mt_blob_one(raw, rawlen);
        t = blob != NULL ? mt_task_new(conn, home, seq, 1, 1, blob) : NULL;
        if (t == NULL) {
            if (blob != NULL)
                mt_blobs_free(blob, 1);
            mt_reply_local(home, conn, st, seq, "-ERR out of memory\r\n",
                           18, out);
            return 1;
        }
        t->kind = MT_TASK_COPY;
        t->db_index = sess->db_index;
        mt_pending_inc(home, st);
        t->pending_owned = 1;
        mt_push_task(home, &home->ms->workers[target].inbox[home->id], t,
                     &home->ms->workers[target]);
        return 1;
    }

    /* Forward to the owning worker: merge consecutive commands for the same
     * target into one task (flushed on target change / local command /
     * end of the parse loop). */
    if (target >= 0 && target != home->id) {
        if (mt_is_blocking_pop(cmd) &&
            (st->pending != 0 || st->reorder != NULL || st->batch_n != 0 ||
             st->in_multi || st->nwatch != 0 || st->subs != NULL ||
             st->watch_pending != 0)) {
            static const char blocked_state[] =
                "-ERR blocking commands require an idle migratable mt "
                "connection\r\n";
            mt_batch_flush(home, conn, st);
            mt_reply_local(home, conn, st, seq, blocked_state,
                           sizeof(blocked_state) - 1, out);
            return 1;
        }
        if (cmd == CMD_MIGRATE &&
            (server_backend(home->srv) == SERVER_BACKEND_IOCP ||
             server_backend(home->srv) == SERVER_BACKEND_IOURING_OP)) {
            static const char migrate_msg[] =
                "-ERR MIGRATE requires a migratable mt connection\r\n";
            mt_batch_flush(home, conn, st);
            mt_reply_local(home, conn, st, seq, migrate_msg,
                           sizeof(migrate_msg) - 1, out);
            return 1;
        }
        /* connection-key affinity: a clean connection migrates once to the
         * worker owning its keys (the current command stays unconsumed in
         * the receive buffer and is re-processed by the new home).
         * Disabled on the IOCP backend: an overlapped recv is always in
         * flight, so the conn cannot move between completion ports safely;
         * plain task routing still applies. */
        if (server_backend(home->srv) != SERVER_BACKEND_IOCP &&
            server_backend(home->srv) != SERVER_BACKEND_IOURING_OP &&
            !sess->repl_link && !st->migrated && st->pending == 0 &&
            st->reorder == NULL &&
            st->batch_n == 0 && !st->in_multi && st->nwatch == 0 &&
            st->subs == NULL && !st->closing) {
            st->seq_next--; /* nothing was answered yet */
            st->migrated = 1;
            if (server_conn_detach(home->srv, conn) == 0) {
                mt_task *t = mt_task_new(conn, &home->ms->workers[target],
                                         0, 1, 0, NULL);
                if (t != NULL) {
                    t->kind = MT_TASK_MIGRATE;
                    mt_push_task(home,
                                 &home->ms->workers[target].migrate[home->id],
                                 t, &home->ms->workers[target]);
                    return 2;
                }
                /* task allocation failed: roll back the detach */
                (void)server_conn_rehome(home->srv, conn);
                if (server_conn_adopt(home->srv, conn) != 0) {
                    server_conn_free_now(home->srv, conn);
                    return 2;
                }
            }
            st->migrated = 0;
            st->seq_next++;
        }
        if (st->batch_target >= 0 && st->batch_target != target)
            mt_batch_flush(home, conn, st);
        if (st->batch_target < 0) {
            st->batch_target = target;
            st->batch_seq = seq;
            st->batch_db = sess->db_index;
        }
        if (mt_batch_append(st, raw, rawlen) != 0) {
            mt_batch_flush(home, conn, st); /* keep the earlier commands */
            resp_write_error(out, "ERR out of memory", 17);
            st->seq_write++;
            return 1;
        }
        return 1;
    }

    /* Local / reserved-blocked / crossslot: any open batch must go out first to keep
     * pipeline order (its seqs precede this command's). */
    mt_batch_flush(home, conn, st);

    /* Local fast path: nothing outstanding, reply straight into conn->out. */
    if (seq == st->seq_write) {
        if (target == MT_BLOCKED)
            resp_write_error(out, blocked_msg, sizeof(blocked_msg) - 1);
        else if (target == MT_CROSSSLOT)
            resp_write_error(out, crossslot_msg, sizeof(crossslot_msg) - 1);
        else
            session_execute(sess, argv, argc, out);
        st->seq_write++;
        return 1;
    }

    /* Replies for earlier routed commands are still in flight: compute the
     * reply now and hold it in the reorder buffer to preserve order. */
    {
        mt_task *t = mt_task_new(conn, home, seq, 1, 0, NULL);
        if (t == NULL) {
            resp_write_error(out, "ERR out of memory", 17);
            st->seq_write++;
            return 1;
        }
        if (target == MT_BLOCKED)
            resp_write_error(&t->reply, blocked_msg,
                             sizeof(blocked_msg) - 1);
        else if (target == MT_CROSSSLOT)
            resp_write_error(&t->reply, crossslot_msg,
                             sizeof(crossslot_msg) - 1);
        else
            session_execute(sess, argv, argc, &t->reply);
        mt_reorder_insert(st, t);
        mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
        return 1;
    }
}

/* Execute one routed task (all kinds) and push it to the home worker's
 * completion ring. */
static void mt_exec_task(worker *w, mt_task *t);

/* Drain every producer's completion ring (delivery side). */
static void mt_drain_completions(worker *w);

/* Drain every producer's inbox ring (execution side). */
static void mt_drain_inbox(worker *w);

/* Wake a worker whose queues received work: proactor workers get a WAKEUP
 * completion posted to their port, readiness workers a pipe kick. */
static void mt_kick(worker *w)
{
#if DDUP_HAS_C_ATOMICS
    /* dedup: one queued wakeup covers every push since the last drain */
    if (atomic_exchange_explicit(&w->kick_pending, 1,
                                 memory_order_acq_rel) != 0)
        return;
#endif
    if (server_is_proactor(w->srv))
        server_wakeup_kick(w->srv);
    else
        (void)pal_wakeup_kick(&w->wakeup);
}

/* Push a task with backpressure: while the downstream ring is full, drain
 * our own queues to relieve pressure (breaks circular waits between
 * workers); after a bounded spin, yield so a descheduled consumer gets
 * CPU. During shutdown (self->running cleared by mt_server_stop) the
 * target may never drain: drop the task -- in-flight replies are moot
 * when the process is exiting, and spinning here would hang the stop
 * path's thread joins, leaving the process up but permanently mute. The
 * function consumes and frees t when it returns failure. */
static int mt_push_task(worker *self, mt_spsc *q, mt_task *t,
                        worker *target)
{
    int spins = 0;
    int pr = mt_spsc_push(q, t);
    while (pr < 0) {
        if (self != NULL) {
            if (!self->running) {
                mt_task_drop_after_push_failure(self, t);
                return -1;
            }
            mt_drain_completions(self);
            mt_drain_inbox(self);
            /* yield (not sleep) past the spin bound: a 1ms sleep quantum
             * turns transient ring-full backpressure into a pool-wide
             * throughput collapse on few-core hosts */
            if (++spins > 64)
                pal_thread_yield();
        } else {
            pal_sleep_ms(1);
        }
        pr = mt_spsc_push(q, t);
    }
    mt_kick(target);
    return 0;
}

/* Fan worker 0's cluster metadata out to every follower. Fire-and-forget:
 * each target applies the snapshot in its own event loop, and the SPSC
 * producer/consumer order preserves snapshot order per target. */
static void mt_cluster_sync(worker *leader)
{
    mt_server *ms;
    int i;

    if (leader == NULL || leader->id != 0)
        return;
    ms = leader->ms;
    if (!ms->cluster_enabled)
        return;
    for (i = 1; i < ms->nworkers; i++) {
        worker *target = &ms->workers[i];
        cluster_state *snap =
            (cluster_state *)malloc(sizeof(cluster_state));
        mt_task *t;

        if (snap == NULL)
            continue;
        cluster_state_snapshot(server_db(leader->srv), snap);
        t = mt_task_new(NULL, target, 0, 1, 0, NULL);
        if (t == NULL) {
            free(snap);
            continue;
        }
        t->kind = MT_TASK_CLUSTER_SYNC;
        t->cluster_state = snap;
        (void)mt_push_task(leader, &target->inbox[leader->id], t, target);
    }
}

typedef struct mt_repl_tmp_ctx {
    db *dbs;
    int ndbs;
} mt_repl_tmp_ctx;

static db *mt_repl_tmp_get(void *ctx, int idx)
{
    mt_repl_tmp_ctx *tc = (mt_repl_tmp_ctx *)ctx;
    if (tc == NULL || idx < 0 || idx >= tc->ndbs)
        return NULL;
    return &tc->dbs[idx];
}

typedef struct mt_repl_restore_ctx {
    worker *leader;
    db *src;
    int db_index;
    int failed;
} mt_repl_restore_ctx;

static void mt_repl_restore_cb(const char *key, size_t klen,
                               const char *val, size_t vlen, void *ctx)
{
    mt_repl_restore_ctx *rc = (mt_repl_restore_ctx *)ctx;
    mt_server *ms = rc->leader->ms;
    resp_buf payload;
    mt_cmd_blob *cmds;
    mt_task *t;
    worker *target;
    const char *ev;
    size_t evl;
    uint64_t expire = 0;
    int tid;

    (void)val;
    (void)vlen;
    if (rc->failed)
        return;
    resp_buf_init(&payload);
    if (snapshot_dump_key(rc->src, key, klen, &payload) != 0) {
        resp_buf_free(&payload);
        rc->failed = 1;
        return;
    }
    if (rh_get(&rc->src->expires, key, klen, &ev, &evl) && evl == 8)
        memcpy(&expire, ev, sizeof(expire));

    cmds = (mt_cmd_blob *)calloc(2, sizeof(*cmds));
    if (cmds == NULL) {
        resp_buf_free(&payload);
        rc->failed = 1;
        return;
    }
    cmds[0].raw = (char *)malloc(klen);
    cmds[1].raw = (char *)malloc(payload.len);
    if (cmds[0].raw == NULL || cmds[1].raw == NULL) {
        free(cmds[0].raw);
        free(cmds[1].raw);
        free(cmds);
        resp_buf_free(&payload);
        rc->failed = 1;
        return;
    }
    memcpy(cmds[0].raw, key, klen);
    cmds[0].len = klen;
    memcpy(cmds[1].raw, payload.data, payload.len);
    cmds[1].len = payload.len;
    resp_buf_free(&payload);

    tid = (int)(hash_slot(key, klen) % (uint32_t)ms->nworkers);
    target = &ms->workers[tid];
    t = mt_task_new(NULL, rc->leader, 0, 1, 2, cmds);
    if (t == NULL) {
        free(cmds[0].raw);
        free(cmds[1].raw);
        free(cmds);
        rc->failed = 1;
        return;
    }
    t->kind = MT_TASK_RESTORE;
    t->db_index = rc->db_index;
    t->task_expire_ms = expire;
    if (mt_push_task(rc->leader, &target->inbox[rc->leader->id], t,
                     target) == 0)
        ms->snapshot_pending++;
    else
        rc->failed = 1;
}

/* Partition a master full-sync snapshot across the worker pool. The bytes
 * are first loaded into temporary dbs on the coordinator thread, then each
 * key is dumped back to the compact DUMP/RESTORE payload and sent to the
 * worker owning its slot. This keeps steady-state data shared-nothing; the
 * temporary copy exists only for the duration of the full sync. */
static int mt_repl_snapshot_load(void *ctx, const char *buf, size_t len)
{
    mt_server *ms = (mt_server *)ctx;
    worker *leader;
    int ndbs, i, j;
    int failed = 0;
    db *tmp;
    mt_repl_tmp_ctx tc;

    if (ms == NULL || ms->nworkers == 0)
        return -1;
    ddup_atomic_store(&ms->repl_synced, 0, ddup_memory_order_release);
    leader = &ms->workers[0];
    ndbs = server_ndbs(leader->srv);
    tmp = (db *)calloc((size_t)ndbs, sizeof(db));
    if (tmp == NULL)
        return -1;
    for (i = 0; i < ndbs; i++)
        db_init(&tmp[i]);
    tc.dbs = tmp;
    tc.ndbs = ndbs;
    if (snapshot_load_mem_multi(&tc, mt_repl_tmp_get, ndbs, buf, len,
                                pal_wall_ms()) != 0) {
        for (i = 0; i < ndbs; i++)
            db_destroy(&tmp[i]);
        free(tmp);
        return -1;
    }

    /* Full resync replaces the old shared-nothing dataset. */
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        for (j = 0; j < ndbs; j++)
            db_flush(server_db_at(w->srv, j));
    }

    ms->snapshot_pending = 0;
    for (i = 0; i < ndbs; i++) {
        mt_repl_restore_ctx rc;
        rc.leader = leader;
        rc.src = &tmp[i];
        rc.db_index = i;
        rc.failed = 0;
        rh_each(&tmp[i].table, mt_repl_restore_cb, &rc);
        if (rc.failed) {
            failed = 1;
            break;
        }
    }

    for (i = 0; i < ndbs; i++)
        db_destroy(&tmp[i]);
    free(tmp);

    while (ms->snapshot_pending > 0 && leader->running) {
        mt_drain_inbox(leader);
        mt_drain_completions(leader);
        pal_sleep_ms(1);
    }
    if (failed || ms->snapshot_pending != 0)
        return -1;
    ddup_atomic_store(&ms->repl_synced, 1, ddup_memory_order_release);
    return 0;
}

/* REPLICAOF/SLAVEOF in mt mode operates the worker-0 master link directly.
 * This avoids forwarding a session-hook command through the sessionless
 * routed-task path (command_execute_at has no server hooks). */
static void mt_route_replicaof(worker *home, const resp_value *argv,
                               size_t argc, resp_buf *out)
{
    mt_server *ms = home->ms;
    server *leader = ms->workers[0].srv;
    char hostbuf[64];
    long long port;

    if (argc != 3 || argv[1].str == NULL || argv[2].str == NULL) {
        resp_write_error(out, "ERR wrong number of arguments for "
                              "'replicaof' command",
                         46);
        return;
    }
    if (mt_ci_equal(argv[1].str, argv[1].len, "NO") &&
        mt_ci_equal(argv[2].str, argv[2].len, "ONE")) {
        (void)server_replicaof(leader, NULL, 0);
        ms->replica_mode = 0;
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (argv[1].len >= sizeof(hostbuf)) {
        resp_write_error(out, "ERR invalid master host", 23);
        return;
    }
    memcpy(hostbuf, argv[1].str, argv[1].len);
    hostbuf[argv[1].len] = '\0';
    if (!mt_parse_ll(argv[2].str, argv[2].len, &port) || port <= 0 ||
        port > 65535) {
        resp_write_error(out, "ERR value is not an integer or out of range",
                         44);
        return;
    }
    if (mt_server_replicaof(ms, hostbuf, (uint16_t)port) != 0) {
        resp_write_error(out, "ERR could not connect to master", 29);
        return;
    }
    resp_write_simple_string(out, "OK", 2);
}

static void mt_exec_task(worker *w, mt_task *t)
{
    uint32_t ci;
    if (t->kind == MT_TASK_REPL_STREAM) {
        if (t->ncmds == 1 && t->cmds != NULL)
            server_repl_stream_append_db(w->srv, t->db_index,
                                         t->cmds[0].raw, t->cmds[0].len);
        mt_task_free(t);
        return;
    }
    if (t->kind == MT_TASK_REPL_SNAPSHOT) {
        (void)snapshot_serialize_multi(w->srv, server_select_db,
                                       server_ndbs(w->srv), &t->reply);
        t->repl_worker_id = w->id;
        (void)mt_push_task(w, &t->home->completions[w->id], t, t->home);
        return;
    }
    if (t->kind == MT_TASK_CLUSTER_SYNC) {
        if (t->cluster_state != NULL)
            cluster_state_restore(server_db(w->srv), t->cluster_state);
        mt_task_free(t);
        return;
    }
    if (t->kind == MT_TASK_RESTORE) {
        db *d = server_db_at(w->srv, t->db_index);
        if (t->ncmds == 2 && t->cmds != NULL) {
            (void)snapshot_restore_key(
                d, t->cmds[0].raw, t->cmds[0].len,
                t->cmds[1].raw, t->cmds[1].len,
                t->task_expire_ms, 1, pal_wall_ms());
        }
        /* This task replies with a completion so the replica full-sync
         * barrier can count it. */
    } else if (t->kind == MT_TASK_UNWATCH) {
        /* fire-and-forget watch_refs release (key bytes in cmds) */
        db *d = server_db_at(w->srv, t->db_index);
        if (d->watch_refs > 0)
            d->watch_refs--;
        mt_task_free(t);
        return;
    } else if (t->kind == MT_TASK_WATCH_CLEANUP) {
        resp_value v;
        ptrdiff_t used;
        size_t i;
        arena_reset(&w->exec_arena);
        used = resp_parse(t->cmds[0].raw, t->cmds[0].len, &v,
                          &w->exec_arena);
        if (used == (ptrdiff_t)t->cmds[0].len && v.type == RESP_ARRAY) {
            for (i = t->watch_cleanup_first + 1;
                 i < v.count && i - 1 < t->nwatch_out; i++) {
                db *d = server_db_at(w->srv, t->db_index);
                if (v.items[i].str != NULL && d->watch_refs > 0)
                    d->watch_refs--;
            }
        }
        mt_task_free(t);
        return;
    } else if (t->kind == MT_TASK_UNSUB) {
        /* remove (conn, channel) from this worker's registry */
        mt_sub_entry **pp = &w->subs;
        while (*pp != NULL) {
            mt_sub_entry *e = *pp;
            if (e->conn == t->conn && e->pattern == t->pubsub_pattern &&
                e->chlen == t->cmds[0].len &&
                memcmp(e->ch, t->cmds[0].raw, e->chlen) == 0) {
                *pp = e->next;
                free(e->ch);
                free(e);
                break;
            }
            pp = &(*pp)->next;
        }
        mt_task_free(t);
        return;
    } else if (t->kind == MT_TASK_SUB) {
        /* register (conn, channel) and report back (round trip so
         * the subscriber conn stays alive until registered) */
        mt_sub_entry *e = (mt_sub_entry *)calloc(1, sizeof(*e));
        worker *home = t->home;
        void *conn = t->conn;
        mt_conn_state *st = (mt_conn_state *)server_conn_mt_state(conn);
        if (st == NULL || st->closing) {
            mt_task_free(t);
            if (st != NULL)
                mt_pending_dec(home, conn, st);
            return;
        }
        if (e != NULL) {
            e->ch = (char *)malloc(t->cmds[0].len);
            if (e->ch != NULL) {
                memcpy(e->ch, t->cmds[0].raw, t->cmds[0].len);
                e->chlen = t->cmds[0].len;
                e->home_id = t->home->id;
                e->conn = t->conn;
                e->pattern = t->pubsub_pattern;
                e->next = w->subs;
                w->subs = e;
            } else {
                free(e);
            }
        }
        /* falls through to the completion push (empty reply) */
    } else if (t->kind == MT_TASK_PUBLISH) {
        mt_publish_execute(w, t);
    } else if (t->kind == MT_TASK_PUBSUB_STATS) {
        mt_pubsub_stats_execute(w, t);
    } else if (t->kind == MT_TASK_WATCH) {
        /* read versions for every watched key; they ride back
         * out-of-band with the +OK reply */
        resp_value v;
        ptrdiff_t used;
        size_t i;
        db *d = server_db_at(w->srv, t->db_index);
        arena_reset(&w->exec_arena);
        used = resp_parse(t->cmds[0].raw, t->cmds[0].len, &v,
                          &w->exec_arena);
        if (used == (ptrdiff_t)t->cmds[0].len &&
            v.type == RESP_ARRAY && v.count > 1) {
            t->nwatch_out = v.count - 1;
            t->watch_out = (uint64_t *)calloc(
                t->nwatch_out * 2, sizeof(uint64_t));
            if (t->watch_out != NULL) {
                for (i = 1; i < v.count; i++) {
                    t->watch_out[2 * (i - 1)] =
                        db_key_version(d, v.items[i].str,
                                       v.items[i].len);
                    t->watch_out[2 * (i - 1) + 1] = d->flush_epoch;
                }
                d->watch_refs += (uint64_t)t->nwatch_out;
            } else {
                t->nwatch_out = 0;
                t->watch_failed = 1;
                resp_write_error(&t->reply, "ERR out of memory", 18);
            }
        }
        if (!t->watch_failed)
            resp_write_simple_string(&t->reply, "OK", 2);
    } else if (t->kind == MT_TASK_EXEC) {
        mt_exec_on_db(w->srv, t, &w->exec_arena);
    } else {
        for (ci = 0; ci < t->ncmds; ci++) {
            resp_value v;
            ptrdiff_t used;
            uint64_t dirty_before;
            int is_cluster;
            db *d = server_db_at(w->srv, t->db_index);
            arena_reset(&w->exec_arena);
            used = resp_parse(t->cmds[ci].raw, t->cmds[ci].len, &v,
                              &w->exec_arena);
            if (used != (ptrdiff_t)t->cmds[ci].len ||
                v.type != RESP_ARRAY || v.is_null) {
                resp_write_error(&t->reply, "ERR Protocol error",
                                 18);
                continue;
            }
            is_cluster = v.count > 0 && v.items[0].str != NULL &&
                         v.items[0].len == 7 &&
                         mt_ci_equal(v.items[0].str, v.items[0].len,
                                     "cluster");
            /* SWAPDB executes on every worker (broadcast): swap this
             * worker's two logical dbs directly (sessionless path) */
            if (v.count == 3 && v.items[0].str != NULL &&
                v.items[0].len == 6 &&
                mt_ci_equal(v.items[0].str, v.items[0].len, "swapdb")) {
                mt_swapdb_exec(w, t->db_index, &v, &t->reply);
                continue;
            }
            if (v.count == 3 && v.items[0].str != NULL &&
                v.items[0].len == 4 &&
                mt_ci_equal(v.items[0].str, v.items[0].len, "move")) {
                if (mt_move_exec(w, t->db_index, &v, &t->reply))
                    server_repl_stream_forward(w->srv, t->db_index,
                                               t->cmds[ci].raw,
                                               t->cmds[ci].len);
                continue;
            }
            if (v.count >= 3 && v.items[0].str != NULL &&
                v.items[0].len == 4 &&
                mt_ci_equal(v.items[0].str, v.items[0].len, "copy")) {
                if (mt_copy_exec(w, t->db_index, &v, &t->reply))
                    server_repl_stream_forward(w->srv, t->db_index,
                                               t->cmds[ci].raw,
                                               t->cmds[ci].len);
                continue;
            }
            /* INFO __STATS__ (aggregation fan-out): machine-format snapshot
             * over all of this worker's logical dbs */
            if (v.count == 2 && v.items[0].str != NULL &&
                v.items[0].len == 4 &&
                mt_ci_equal(v.items[0].str, v.items[0].len, "info") &&
                v.items[1].str != NULL && v.items[1].len == 9 &&
                mt_ci_equal(v.items[1].str, v.items[1].len, "__stats__")) {
                mt_info_exec(w, &t->reply);
                continue;
            }
            /* FLUSHALL clears every logical db on this worker, not just the
             * caller-selected one. */
            if (v.count == 1 && v.items[0].str != NULL &&
                v.items[0].len == 8 &&
                mt_ci_equal(v.items[0].str, v.items[0].len, "flushall")) {
                mt_flushall_exec(w, t->db_index, &t->reply);
                continue;
            }
        dirty_before = d->dirty;
        command_execute_at(d, v.items, v.count, &t->reply,
                           pal_wall_ms());
            /* sessionless path: log applied mutations to the
             * worker's own AOF */
            if (d->dirty != dirty_before)
                server_aof_log_cmd(w->srv, t->db_index, v.items, v.count);
            if (d->dirty != dirty_before && !is_cluster && t->agg == NULL)
                server_repl_stream_forward(w->srv, t->db_index,
                                           t->cmds[ci].raw,
                                           t->cmds[ci].len);
            if (is_cluster && w->id == 0)
                mt_cluster_sync(w);
        }
    }
    w->tasks_executed++;
    pal_mutex_lock(&t->home->ms->abandoned_agg_mu);
    if (t->home->ms->fail_completion_pushes > 0 &&
        w->id == t->home->ms->fail_completion_worker) {
        t->home->ms->fail_completion_pushes--;
        pal_mutex_unlock(&t->home->ms->abandoned_agg_mu);
        mt_task_drop_after_push_failure(w, t);
    } else {
        pal_mutex_unlock(&t->home->ms->abandoned_agg_mu);
        (void)mt_push_task(w, &t->home->completions[w->id], t, t->home);
    }
}

/* Max items popped per ring per drain call: unbounded drains can livelock
 * when other workers keep producing (the loop never returns to socket IO).
 * Leftovers are handled on the next wakeup (see mt_rings_nonempty). */
#define MT_DRAIN_MAX 512

static void mt_drain_inbox(worker *w)
{
    int pi;
    mt_watch_release_pending(w);
    for (pi = 0; pi < w->ms->nworkers; pi++) {
        int n = 0;
        while (n < MT_DRAIN_MAX) {
            mt_task *t = (mt_task *)mt_spsc_pop(&w->inbox[pi]);
            if (t == NULL)
                break;
            mt_exec_task(w, t);
            n++;
        }
    }
}

static void mt_drain_completions(worker *w)
{
    int pi;
    size_t fi;

    /* queue a conn for the single end-of-pass flush (dedup per pass) */
#define mt_mark_flush(conn_, st_)                                             \
    do {                                                                      \
        if ((st_)->flush_epoch != w->drain_epoch) {                           \
            (st_)->flush_epoch = w->drain_epoch;                              \
            if (w->flush_n == w->flush_cap) {                                 \
                size_t ncap_ = w->flush_cap == 0 ? 64 : w->flush_cap * 2;     \
                void **nl_ =                                                  \
                    (void **)realloc(w->flush_list,                           \
                                     ncap_ * sizeof(void *));                 \
                if (nl_ != NULL) {                                            \
                    w->flush_list = nl_;                                      \
                    w->flush_cap = ncap_;                                     \
                }                                                             \
            }                                                                 \
            if (w->flush_n < w->flush_cap)                                    \
                w->flush_list[w->flush_n++] = (conn_);                        \
            else                                                              \
                (void)server_conn_flush(w->srv, (conn_)); /* no room: now */  \
        }                                                                     \
    } while (0)

    w->drain_epoch++;
    mt_drain_failed_tasks(w);
    for (pi = 0; pi < w->ms->nworkers; pi++) {
        int n = 0;
        while (n < MT_DRAIN_MAX) {
            mt_task *t = (mt_task *)mt_spsc_pop(&w->completions[pi]);
            void *conn;
            mt_conn_state *st;
            if (t == NULL)
                break;
            n++;
            if (t->kind == MT_TASK_REPL_SNAPSHOT) {
                if (t->home == w && w->ms->snapshot_ser_tasks != NULL &&
                    w->ms->snapshot_ser_pending > 0) {
                    int idx = t->repl_worker_id;
                    if (idx > 0 && idx < w->ms->nworkers)
                        w->ms->snapshot_ser_tasks[idx] = t;
                    else
                        mt_task_free(t);
                    w->ms->snapshot_ser_pending--;
                } else {
                    mt_task_free(t);
                }
                continue;
            }
            conn = t->conn;
            st = (mt_conn_state *)server_conn_mt_state(conn);
            if (t->kind == MT_TASK_RESTORE) {
                if (t->home == w && w->ms->snapshot_pending > 0)
                    w->ms->snapshot_pending--;
                mt_task_free(t);
                continue;
            }
            if (t->kind == MT_TASK_SUB) {
                /* registration round trip finished: just release the ref */
                mt_task_free(t);
                if (st != NULL)
                    mt_pending_dec(w, conn, st);
                continue;
            }
            if (t->kind == MT_TASK_PUSH) {
                /* pub/sub delivery: re-check the subscription (UNSUBSCRIBE
                 * may have raced with the fan-out) and append directly,
                 * outside the command sequence (pushes are async) */
                if (st != NULL) {
                    int deliver = !st->closing && t->ncmds == 1 &&
                                  mt_conn_sub_find_kind(
                                      st, t->cmds[0].raw, t->cmds[0].len,
                                      t->pubsub_pattern) != NULL;
                    if (deliver) {
    (void)server_conn_out_append(w->srv, conn, t->reply.data, t->reply.len);
                        mt_mark_flush(conn, st);
                    }
                    mt_pending_dec(w, conn, st);
                }
                mt_task_free(t);
                continue;
            }
            if (t->kind == MT_TASK_SCAN)
                mt_scan_rewrite_reply(w, t);
            if (t->agg != NULL) {
                /* broadcast part: accumulate and finish when complete */
                mt_agg *agg = t->agg;
                mt_agg_accumulate(agg, &t->reply);
                mt_task_free(t);
                if (mt_agg_drop_part(agg) && !agg->finished) {
            if (agg->abandoned) {
                mt_agg_free(agg);
                    } else if (st != NULL) {
                        mt_agg_finish(w->srv, conn, st, agg);
                    } else {
                        mt_agg_free(agg);
                    }
                }
                if (st != NULL)
                    mt_pending_dec(w, conn, st);
                continue;
            }
            if (st != NULL) {
                mt_reorder_insert(st, t);
                if (st->closing) {
                    mt_drain_ready(w->srv, &w->exec_arena, conn, st, 0);
                } else {
                    mt_drain_ready(w->srv, &w->exec_arena, conn, st, 1);
                    if (st->watch_pending == 0 && st->deferred_head != NULL)
                        mt_replay_deferred(w, conn, st);
                    mt_mark_flush(conn, st);
                }
                mt_pending_dec(w, conn, st);
            } else {
                mt_task_free(t);
            }
        }
    }
    /* one flush per touched conn per drain pass */
    for (fi = 0; fi < w->flush_n; fi++)
        (void)server_conn_flush(w->srv, w->flush_list[fi]);
    w->flush_n = 0;
#undef mt_mark_flush
}

static void worker_on_wakeup(void *ctx)
{
    worker *w = (worker *)ctx;
    int pi;
    (void)pal_wakeup_drain(&w->wakeup);
#if DDUP_HAS_C_ATOMICS
    /* Drain the notification before re-arming the latch. This prevents a
     * producer's wake byte from being consumed by this callback's drain. */
    (void)atomic_exchange_explicit(&w->kick_pending, 0,
                                   memory_order_acq_rel);
#endif

    /* 1. adopt accepted fds */
    for (;;) {
        void *p = mt_spsc_pop(&w->accepts);
        pal_socket_t fd;
        if (p == NULL)
            break;
        fd = (pal_socket_t)(uintptr_t)p;
        (void)server_adopt_fd(w->srv, fd);
    }
    for (;;) {
        void *p = mt_spsc_pop(&w->accepts_tls);
        pal_socket_t fd;
        if (p == NULL)
            break;
        fd = (pal_socket_t)(uintptr_t)p;
        (void)server_adopt_fd_tls(w->srv, fd);
    }

    /* 1b. adopt connections migrated from other workers (key affinity) */
    for (pi = 0; pi < w->ms->nworkers; pi++) {
        for (;;) {
            mt_task *t = (mt_task *)mt_spsc_pop(&w->migrate[pi]);
            if (t == NULL)
                break;
            server_conn_rehome(w->srv, t->conn);
            if (server_conn_adopt(w->srv, t->conn) != 0)
                server_conn_free_now(w->srv, t->conn);
            mt_task_free(t);
        }
    }

    /* 2. execute commands routed to this worker */
    mt_drain_inbox(w);

    /* 3. deliver completed replies (home side) */
    mt_drain_completions(w);

    /* bounded drains: anything left behind gets an immediate re-kick so the
     * loop comes straight back instead of sleeping on leftover work */
    if (mt_spsc_nonempty(&w->accepts) ||
        mt_spsc_nonempty(&w->accepts_tls)) {
        mt_kick(w);
        return;
    }
    for (pi = 0; pi < w->ms->nworkers; pi++) {
        if (mt_spsc_nonempty(&w->migrate[pi]) ||
            mt_spsc_nonempty(&w->inbox[pi]) ||
            mt_spsc_nonempty(&w->completions[pi])) {
            mt_kick(w);
            return;
        }
    }
}

static void *worker_main(void *arg)
{
    worker *w = (worker *)arg;
    while (w->running) {
        if (server_run_once(w->srv, 50) < 0) {
            int i;
            /* A worker-local AOF failure is process-wide: stop every worker
             * and wake them so no loop can spin or remain blocked in poll. */
            for (i = 0; i < w->ms->nworkers; i++) {
                w->ms->workers[i].running = 0;
                if (i != w->id)
                    mt_kick(&w->ms->workers[i]);
            }
            w->ms->running = 0;
            break;
        }
    }
    return NULL;
}

static void *acceptor_main(void *arg)
{
    mt_server *ms = (mt_server *)arg;
    pal_loop *l = pal_loop_create();
    pal_socket_t held = PAL_SOCKET_INVALID;
    int held_tls = 0;
    int rr = 0;
    if (l == NULL)
        return NULL;
    if (pal_loop_add(l, ms->listen_fd, 1, 0, NULL) != 0) {
        pal_loop_free(l);
        return NULL;
    }
    if (ms->tls_listen_fd != PAL_SOCKET_INVALID &&
        pal_loop_add(l, ms->tls_listen_fd, 1, 0, NULL) != 0) {
        pal_loop_free(l);
        return NULL;
    }
    while (ms->running) {
        pal_event evs[8];
        int n;
        int i;
        /* a held fd (its worker's accept ring was full) goes first; never
         * drop a just-accepted client on the floor */
        if (held != PAL_SOCKET_INVALID) {
            worker *w = &ms->workers[rr % ms->nworkers];
            mt_spsc *ring = held_tls ? &w->accepts_tls : &w->accepts;
            int pr = mt_spsc_push(ring, (void *)(uintptr_t)held);
            if (pr >= 0) {
                mt_kick(w);
                rr++;
                held = PAL_SOCKET_INVALID;
            }
        }
        /* while holding, skip the accept loop (the wait below still
         * wakes on listen events or after the 50ms re-poll) */
        n = pal_loop_wait(l, evs, 8, 50);
        for (i = 0; i < n; i++) {
            int is_tls;
            if (!evs[i].readable)
                continue;
            if (evs[i].fd == ms->listen_fd) {
                is_tls = 0;
            } else if (ms->tls_listen_fd != PAL_SOCKET_INVALID &&
                       evs[i].fd == ms->tls_listen_fd) {
                is_tls = 1;
            } else {
                continue;
            }
            for (;;) {
                pal_socket_t fd;
                worker *w;
                if (held != PAL_SOCKET_INVALID)
                    break; /* one at a time; retry next round */
                fd = pal_accept(evs[i].fd);
                if (fd == PAL_SOCKET_INVALID)
                    break;
                w = &ms->workers[rr % ms->nworkers];
                {
                    mt_spsc *ring =
                        is_tls ? &w->accepts_tls : &w->accepts;
                    int pr =
                        mt_spsc_push(ring, (void *)(uintptr_t)fd);
                    if (pr < 0) {
                        /* ring full: hold the fd and stop accepting
                         * (the listen backlog absorbs the burst) */
                        held = fd;
                        held_tls = is_tls;
                        break;
                    }
                    rr++;
                    mt_kick(w);
                }
            }
        }
    }
    pal_loop_free(l);
    return NULL;
}

mt_server *mt_server_create(const char *host, uint16_t port, int nworkers)
{
    return mt_server_create_ex(host, port, nworkers, SERVER_BACKEND_SELECT);
}

mt_server *mt_server_create_ex(const char *host, uint16_t port, int nworkers,
                               int worker_backend)
{
    mt_server *ms;
    int i;

    if (nworkers < 1)
        return NULL;
    ms = (mt_server *)calloc(1, sizeof(*ms));
    if (ms == NULL)
        return NULL;
    ms->listen_fd = pal_tcp_listen(host, port, 511, &ms->port);
    if (ms->listen_fd == PAL_SOCKET_INVALID) {
        free(ms);
        return NULL;
    }
    (void)pal_set_nonblocking(ms->listen_fd, 1);
    ms->tls_listen_fd = PAL_SOCKET_INVALID;
    ms->tls_port = 0;
    ms->nworkers = nworkers;
    ms->worker_backend = worker_backend;
    if (pal_mutex_init(&ms->abandoned_agg_mu) != 0) {
        pal_close(ms->listen_fd);
        free(ms);
        return NULL;
    }
    ddup_atomic_init(&ms->repl_synced, 0);
    ms->workers = (worker *)calloc((size_t)nworkers, sizeof(worker));
    if (ms->workers == NULL) {
        pal_close(ms->listen_fd);
        free(ms);
        return NULL;
    }
    for (i = 0; i < nworkers; i++) {
        worker *w = &ms->workers[i];
        int j;
        w->id = i;
        w->ms = ms;
#if DDUP_HAS_C_ATOMICS
        atomic_init(&w->kick_pending, 0);
#endif
        w->srv = server_create_ex("127.0.0.1", 0, worker_backend);
        w->inbox = (mt_spsc *)calloc((size_t)nworkers, sizeof(mt_spsc));
        w->completions =
            (mt_spsc *)calloc((size_t)nworkers, sizeof(mt_spsc));
        w->migrate = (mt_spsc *)calloc((size_t)nworkers, sizeof(mt_spsc));
        if (w->srv == NULL || w->inbox == NULL || w->completions == NULL ||
            w->migrate == NULL || mt_spsc_init(&w->accepts, 256) != 0 ||
            mt_spsc_init(&w->accepts_tls, 256) != 0 ||
            pal_mutex_init(&w->pending_mu) != 0 ||
            pal_mutex_init(&w->task_pool_mu) != 0 ||
            pal_wakeup_create(&w->wakeup) != 0) {
            ms->nworkers = i; /* destroy only initialized workers */
            mt_server_destroy(ms);
            return NULL;
        }
        for (j = 0; j < nworkers; j++) {
            /* deep rings: a transient producer/consumer lag must not hit
             * the backpressure path at bench burst sizes */
            if (mt_spsc_init(&w->inbox[j], 8192) != 0 ||
                mt_spsc_init(&w->completions[j], 8192) != 0 ||
                mt_spsc_init(&w->migrate[j], 64) != 0) {
                ms->nworkers = i + 1;
                mt_server_destroy(ms);
                return NULL;
            }
        }
        arena_init(&w->exec_arena, 4096);
        server_close_listener(w->srv);
        if (server_set_wakeup(w->srv, w->wakeup.wait_fd, worker_on_wakeup,
                              w) != 0) {
            ms->nworkers = i + 1;
            mt_server_destroy(ms);
            return NULL;
        }
        server_set_route(w->srv, mt_route, mt_route_flush_cb,
                         mt_state_free_cb, w);
        server_set_mt_close(w->srv, mt_conn_close);
        server_set_repl_centralized(w->srv, 1);
        server_set_repl_stream_forward(w->srv, mt_repl_stream_forward, w);
        if (i == 0)
            server_set_repl_snapshot_serialize_hook(
                w->srv, mt_repl_snapshot_serialize, ms);
    }
    return ms;
}

uint16_t mt_server_port(const mt_server *ms)
{
    return ms->port;
}

int mt_server_repl_synced(const mt_server *ms)
{
    return ms != NULL &&
           ddup_atomic_load(&ms->repl_synced, ddup_memory_order_acquire);
}

void mt_server_set_requirepass(mt_server *ms, const char *pw)
{
    int i;
    for (i = 0; i < ms->nworkers; i++)
        server_set_requirepass(ms->workers[i].srv, pw);
}

void mt_server_set_maxmemory(mt_server *ms, uint64_t bytes, int policy)
{
    int i;
    for (i = 0; i < ms->nworkers; i++)
        server_set_maxmemory(ms->workers[i].srv, bytes, policy);
}

void mt_server_set_proto_max_request_bytes(mt_server *ms, size_t bytes)
{
    int i;
    for (i = 0; i < ms->nworkers; i++)
        server_set_proto_max_request_bytes(ms->workers[i].srv, bytes);
}

void mt_server_set_repl_max_snapshot_bytes(mt_server *ms, size_t bytes)
{
    int i;
    for (i = 0; i < ms->nworkers; i++)
        server_set_repl_max_snapshot_bytes(ms->workers[i].srv, bytes);
}

uint64_t mt_server_tasks_executed(const mt_server *ms)
{
    uint64_t total = 0;
    int i;
    for (i = 0; i < ms->nworkers; i++)
        total += ms->workers[i].tasks_executed;
    return total;
}

uint64_t mt_server_pool_hits(const mt_server *ms)
{
    uint64_t total = 0;
    int i;
    for (i = 0; i < ms->nworkers; i++)
        total += ms->workers[i].task_pool_hits;
    return total;
}

void mt_server_fail_next_completion_pushes(mt_server *ms, int n)
{
    if (ms != NULL) {
        pal_mutex_lock(&ms->abandoned_agg_mu);
        ms->fail_completion_pushes = n > 0 ? n : 0;
        ms->fail_completion_worker = ms->nworkers - 1;
        ms->fail_completion_consumed = 0;
        pal_mutex_unlock(&ms->abandoned_agg_mu);
    }
}

int mt_server_completion_pushes_consumed(const mt_server *ms)
{
    int n;
    if (ms == NULL)
        return 0;
    pal_mutex_lock((pal_mutex *)&ms->abandoned_agg_mu);
    n = ms->fail_completion_consumed;
    pal_mutex_unlock((pal_mutex *)&ms->abandoned_agg_mu);
    return n;
}

int mt_server_abandoned_aggregate_count(const mt_server *ms)
{
    int n;
    if (ms == NULL)
        return 0;
    pal_mutex_lock((pal_mutex *)&ms->abandoned_agg_mu);
    n = (int)ms->nabandoned_aggs;
    pal_mutex_unlock((pal_mutex *)&ms->abandoned_agg_mu);
    return n;
}

void mt_server_test_set_aof_write_fn(
    mt_server *ms, int worker_id,
    ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n))
{
    if (ms == NULL || worker_id < 0 || worker_id >= ms->nworkers)
        return;
    server_test_set_aof_write_fn(ms->workers[worker_id].srv, write_fn);
}

int mt_server_test_running(const mt_server *ms)
{
    return ms != NULL ? ms->running : 0;
}

uint64_t mt_server_test_worker_loops(const mt_server *ms, int worker_id)
{
    if (ms == NULL || worker_id < 0 || worker_id >= ms->nworkers)
        return 0;
    return server_io_counters(ms->workers[worker_id].srv)->loops;
}

int mt_server_enable_aof(mt_server *ms, const char *dir,
                         const char *appendfilename)
{
    int i;
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        snprintf(w->aof_path, sizeof(w->aof_path), "%s/worker-%d-%s", dir,
                 w->id, appendfilename);
        if (server_enable_aof(w->srv, w->aof_path) != 0)
            return -1;
    }
    return 0;
}

void mt_server_set_appendfsync(mt_server *ms, int mode)
{
    int i;
    for (i = 0; i < ms->nworkers; i++)
        server_set_appendfsync(ms->workers[i].srv, mode);
}

int mt_server_enable_snapshots(mt_server *ms, const char *dir,
                               const char *dbfilename, int save_sec)
{
    int i;
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        snprintf(w->snap_path, sizeof(w->snap_path), "%s/worker-%d-%s",
                 dir, w->id, dbfilename);
        server_set_snapshot_path(w->srv, w->snap_path);
        if (pal_file_exists(w->snap_path) &&
            server_load_snapshot(w->srv) != 0)
            return -1;
        server_set_save_interval(w->srv, save_sec);
    }
    return 0;
}

int mt_server_enable_tiering(mt_server *ms, const char *dir,
                             uint64_t max_disk_bytes)
{
    int i;
    if (ms == NULL || dir == NULL)
        return -1;
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        char logname[64];
        snprintf(logname, sizeof(logname), "tier-%d.log", w->id);
        if (server_enable_tiering(w->srv, dir, logname, max_disk_bytes) != 0)
            return -1;
    }
    return 0;
}

int mt_server_enable_cluster(mt_server *ms, const char *node_id,
                             const char *nodes_path,
                             const char *announce_ip)
{
    server *leader;
    cluster_state snap;
    int i;

    if (ms == NULL || node_id == NULL || ms->nworkers == 0)
        return -1;
    leader = ms->workers[0].srv;
    server_set_cluster_announce(leader, announce_ip, ms->port);
    server_set_cluster_control(leader, 1);
    if (nodes_path != NULL && nodes_path[0] != '\0') {
        server_load_nodes(leader, nodes_path);
        server_set_nodes_path(leader, nodes_path);
    }
    server_enable_cluster(leader, node_id);
    ms->cluster_enabled = 1;

    /* Followers have the same node/slot table but never bind the bus or run
     * gossip; worker 0 pushes updated snapshots after cluster commands. */
    cluster_state_snapshot(server_db(leader), &snap);
    for (i = 1; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        server_set_cluster_control(w->srv, 0);
        cluster_state_restore(server_db(w->srv), &snap);
    }
    return 0;
}

void mt_server_set_bus_protocol(mt_server *ms, int proto)
{
    if (ms != NULL && ms->nworkers > 0)
        server_set_bus_protocol(ms->workers[0].srv, proto);
}

int mt_server_replicaof(mt_server *ms, const char *host, uint16_t port)
{
    server *leader;

    if (ms == NULL || host == NULL || ms->nworkers == 0)
        return -1;
    leader = ms->workers[0].srv;
    server_set_repl_snapshot_hook(leader, mt_repl_snapshot_load, ms);
    if (server_replicaof(leader, host, port) != 0)
        return -1;
    ms->replica_mode = 1;
    return 0;
}

int mt_server_enable_tls(mt_server *ms, const char *host, uint16_t port,
                         const char *cert_file, const char *key_file)
{
    int i;
    if (ms->running || ms->tls_listen_fd != PAL_SOCKET_INVALID)
        return -1;
    /* one context per worker (shared-nothing; no cross-thread SSL_CTX use) */
    for (i = 0; i < ms->nworkers; i++) {
        if (server_tls_ctx_init(ms->workers[i].srv, cert_file, key_file) !=
            0)
            return -1;
    }
    ms->tls_listen_fd = pal_tcp_listen(host, port, 511, &ms->tls_port);
    if (ms->tls_listen_fd == PAL_SOCKET_INVALID)
        return -1;
    (void)pal_set_nonblocking(ms->tls_listen_fd, 1);
    return 0;
}

uint16_t mt_server_tls_port(const mt_server *ms)
{
    return ms->tls_port;
}

int mt_server_start(mt_server *ms)
{
    int i;
    int started_workers = 0;

    ms->running = 1;
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        w->running = 1;
        if (pal_thread_create(&w->thread, worker_main, w) != 0) {
            w->running = 0;
            goto fail;
        }
        started_workers++;
    }
    if (pal_thread_create(&ms->acceptor, acceptor_main, ms) != 0) {
        goto fail;
    }
    ms->started_workers = started_workers;
    ms->acceptor_started = 1;
    return 0;

fail:
    ms->running = 0;
    for (i = 0; i < started_workers; i++) {
        ms->workers[i].running = 0;
        mt_kick(&ms->workers[i]);
    }
    for (i = 0; i < started_workers; i++)
        (void)pal_thread_join(&ms->workers[i].thread, NULL);
    return -1;
}

void mt_server_stop(mt_server *ms)
{
    int i;
    ms->running = 0;
    for (i = 0; i < ms->started_workers; i++) {
        ms->workers[i].running = 0;
        mt_kick(&ms->workers[i]);
    }
    if (ms->acceptor_started) {
        (void)pal_thread_join(&ms->acceptor, NULL);
        ms->acceptor_started = 0;
    }
    for (i = 0; i < ms->started_workers; i++)
        (void)pal_thread_join(&ms->workers[i].thread, NULL);
    ms->started_workers = 0;
}

void mt_server_destroy(mt_server *ms)
{
    int i;
    if (ms == NULL)
        return;
    ms->destroying = 1;
    pal_close(ms->listen_fd);
    if (ms->tls_listen_fd != PAL_SOCKET_INVALID)
        pal_close(ms->tls_listen_fd);
    /* stop recycling BEFORE draining any rings: drains free tasks into
     * home pools, and an earlier worker's pool mutex is destroyed by the
     * time a later worker's drain runs */
    for (i = 0; i < ms->nworkers; i++)
        ms->workers[i].pool_off = 1;
    /* Phase one: every connection state is freed while every worker DB and
     * every task ring remains alive. This makes cross-worker WATCH cleanup
     * independent of destruction order. */
    for (i = 0; i < ms->nworkers; i++) {
        mt_drain_failed_tasks(&ms->workers[i]);
    }
    for (i = 0; i < ms->nworkers; i++) {
        if (ms->workers[i].srv != NULL &&
            !server_is_proactor(ms->workers[i].srv)) {
            server_free_connections(ms->workers[i].srv);
        }
    }
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        if (w->srv == NULL)
            continue;
        mt_drain_failed_tasks(w);
        if (w->inbox != NULL) {
            int j;
            void *p;
            for (j = 0; j < ms->nworkers; j++) {
        while ((p = mt_spsc_pop(&w->inbox[j])) != NULL)
                {
                    mt_task *t = (mt_task *)p;
                    if (t->kind == MT_TASK_WATCH_CLEANUP)
                        mt_watch_release_task_refs_direct(ms, t);
                    if (t->agg != NULL) {
                        mt_track_abandoned_agg(ms, t->agg);
                    }
                    mt_task_free(t);
                }
                mt_spsc_destroy(&w->inbox[j]);
                while ((p = mt_spsc_pop(&w->completions[j])) != NULL)
                {
                    mt_task *t = (mt_task *)p;
                    if (t->kind == MT_TASK_WATCH ||
                        t->kind == MT_TASK_WATCH_CLEANUP)
                        mt_watch_release_task_refs_direct(ms, t);
                    if (t->agg != NULL) {
                        mt_track_abandoned_agg(ms, t->agg);
                    }
                    mt_task_free(t);
                }
                mt_spsc_destroy(&w->completions[j]);
                while ((p = mt_spsc_pop(&w->migrate[j])) != NULL)
                {
                    mt_task *t = (mt_task *)p;
                    mt_task_free(t);
                }
                mt_spsc_destroy(&w->migrate[j]);
            }
            free(w->inbox);
            free(w->completions);
            free(w->migrate);
        }
        {
            void *p;
            while ((p = mt_spsc_pop(&w->accepts)) != NULL)
                pal_close((pal_socket_t)(uintptr_t)p);
            mt_spsc_destroy(&w->accepts);
            while ((p = mt_spsc_pop(&w->accepts_tls)) != NULL)
                pal_close((pal_socket_t)(uintptr_t)p);
            mt_spsc_destroy(&w->accepts_tls);
            /* Worker zombies are only a non-owning pending-work index. The
             * server owns final connection destruction, especially for
             * proactor operations that can still reference conn buffers. */
            while (w->nzombies > 0) {
                void *zombie = w->zombies[--w->nzombies];
                server_conn_free_now(w->srv, zombie);
            }
            free(w->zombies);
            w->zombies = NULL;
            w->zombie_cap = 0;
            free(w->flush_list);
        }
        mt_watch_release_pending(w);
        mt_worker_subs_free(w);
        pal_mutex_destroy(&w->pending_mu);
        pal_wakeup_destroy(&w->wakeup);
        arena_destroy(&w->exec_arena);
        server_destroy(w->srv);
        w->srv = NULL;
        /* drain the task pool: recycled tasks still own their reply bufs */
        {
            mt_task *tp;
            pal_mutex_lock(&w->task_pool_mu);
            tp = w->task_pool;
            w->task_pool = NULL;
            w->task_pool_n = 0;
            pal_mutex_unlock(&w->task_pool_mu);
            while (tp != NULL) {
                mt_task *nx = tp->pool_next;
                resp_buf_free(&tp->reply);
                free(tp);
                tp = nx;
            }
        }
        pal_mutex_destroy(&w->task_pool_mu);
    }
    while (ms->nabandoned_aggs > 0)
        mt_agg_free(ms->abandoned_aggs[ms->nabandoned_aggs - 1]);
    free(ms->abandoned_aggs);
    pal_mutex_destroy(&ms->abandoned_agg_mu);
    free(ms->workers);
    free(ms);
}
