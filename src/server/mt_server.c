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

#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/command.h"
#include "core/hashslot.h"
#include "core/session.h"
#include "pal/pal_event.h"
#include "pal/pal_socket.h"
#include "pal/pal_thread.h"
#include "pal/pal_time.h"
#include "pal/pal_wakeup.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"
#include "server/mt_spsc.h"
#include "server/server.h"

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
} mt_watch_entry;

/* Task kinds. */
#define MT_TASK_CMD 0    /* ordinary routed command batch */
#define MT_TASK_WATCH 1  /* WATCH: reply +OK, versions ride back out-of-band */
#define MT_TASK_EXEC 2   /* EXEC bundle: watch check + sequential replay */
#define MT_TASK_UNWATCH 3 /* fire-and-forget watch_refs release (key blob) */

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
    /* WATCH result: 2 slots per key (version, epoch), filled on the owner */
    uint64_t *watch_out;
    size_t nwatch_out;
    /* EXEC input: watch entries transferred from the home conn state */
    mt_watch_entry *exec_watches;
    size_t nexec_watches;
} mt_task;

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
    mt_spsc *inbox;
    mt_spsc *completions;
    arena exec_arena;     /* re-parse scratch for routed commands */
    uint64_t tasks_executed; /* routed tasks executed (test/observability) */
    volatile int running;
};

struct mt_server {
    pal_socket_t listen_fd;
    uint16_t port;
    int nworkers;
    worker *workers;
    pal_thread acceptor;
    volatile int running;
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
    int pending;   /* parts still awaited */
    long long sum; /* DBSIZE accumulation */
};

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
    mt_cmd_blob *batch;
    size_t batch_n;
    size_t batch_cap;
    /* mt-level transaction state (session never enters MULTI in mt mode) */
    int in_multi;
    mt_cmd_blob *mq; /* queued command blobs between MULTI and EXEC */
    size_t mq_n;
    size_t mq_cap;
    mt_watch_entry *watches;
    size_t nwatch;
    size_t watch_cap;
} mt_conn_state;

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
    size_t i;
    if (t == NULL)
        return;
    mt_blobs_free(t->cmds, t->ncmds);
    resp_buf_free(&t->reply);
    free(t->watch_out);
    if (t->exec_watches != NULL) {
        for (i = 0; i < t->nexec_watches; i++)
            free(t->exec_watches[i].key);
        free(t->exec_watches);
    }
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
                        uint64_t version, uint64_t epoch)
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
    st->nwatch++;
    return 0;
}

static void mt_push_task(mt_spsc *q, mt_task *t, pal_wakeup *wake);

/* Fire-and-forget watch_refs release on the owning worker. */
static mt_task *mt_unwatch_task(const char *key, size_t klen)
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
    return t;
}

/* Release one watch entry's db watch_refs on its owning worker. */
static void mt_watch_release_one(worker *home, const mt_watch_entry *e)
{
    int owner = (int)(hash_slot(e->key, e->klen) %
                      (uint32_t)home->ms->nworkers);
    if (owner == home->id) {
        db *d = server_db(home->srv);
        if (d->watch_refs > 0)
            d->watch_refs--;
        return;
    }
    {
        mt_task *t = mt_unwatch_task(e->key, e->klen);
        if (t != NULL)
            mt_push_task(&home->ms->workers[owner].inbox[home->id], t,
                         &home->ms->workers[owner].wakeup);
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

/* A routed WATCH drained in pipeline order: record the versions it brought
 * back (keys re-parsed from the task blob). */
static void mt_watch_apply(mt_conn_state *st, mt_task *t, arena *ar)
{
    resp_value v;
    ptrdiff_t used;
    size_t i;
    arena_reset(ar);
    used = resp_parse(t->cmds[0].raw, t->cmds[0].len, &v, ar);
    if (used != (ptrdiff_t)t->cmds[0].len || v.type != RESP_ARRAY ||
        t->watch_out == NULL)
        return;
    for (i = 1; i < v.count && i - 1 < t->nwatch_out; i++) {
        if (v.items[i].str == NULL)
            continue;
        if (mt_watch_add(st, v.items[i].str, v.items[i].len,
                         t->watch_out[2 * (i - 1)],
                         t->watch_out[2 * (i - 1) + 1]) != 0)
            break;
    }
}

/* Append every consecutive ready reply to conn->out (or drop them when the
 * conn is a zombie: the client is gone, only resource cleanup matters). */
static void mt_drain_ready(server *srv, arena *ar, void *conn,
                           mt_conn_state *st, int append)
{
    while (st->reorder != NULL && st->reorder->seq == st->seq_write) {
        mt_task *t = st->reorder;
        st->reorder = t->next;
        if (t->kind == MT_TASK_WATCH && append)
            mt_watch_apply(st, t, ar);
        if (append)
            server_conn_out_append(srv, conn, t->reply.data, t->reply.len);
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
        mt_task_free(t);
    }
    /* drop an unflushed merge batch (conn closed mid-pipeline) */
    mt_blobs_free(st->batch, st->batch_n);
    /* drop transaction state */
    mt_blobs_free(st->mq, st->mq_n);
    mt_watches_clear((worker *)ctx, st);
    free(st);
}

/* Accumulate one part of a broadcast reply (home thread only). */
static void mt_agg_accumulate(mt_agg *agg, const resp_buf *part)
{
    if (agg->cmd == CMD_DBSIZE && part->data != NULL && part->len > 2 &&
        part->data[0] == ':')
        agg->sum += strtoll(part->data + 1, NULL, 10);
    /* FLUSHDB parts are "+OK"; nothing to accumulate. */
}

/* All parts arrived: build the aggregated reply and queue it in pipeline
 * order (home thread only). */
static void mt_agg_finish(server *srv, void *conn, mt_conn_state *st,
                          mt_agg *agg)
{
    mt_task *fin = mt_task_new(conn, agg->home, agg->seq, 1, 0, NULL);
    if (fin != NULL) {
        if (agg->cmd == CMD_DBSIZE)
            resp_write_integer(&fin->reply, agg->sum);
        else
            resp_write_simple_string(&fin->reply, "OK", 2);
        mt_reorder_insert(st, fin);
        if (server_conn_mt_is_zombie(conn)) {
            mt_drain_ready(srv, &agg->home->exec_arena, conn, st, 0);
        } else {
            mt_drain_ready(srv, &agg->home->exec_arena, conn, st, 1);
            (void)server_conn_flush(srv, conn);
        }
    }
    free(agg);
}

/* ------------------------------------------------------------------ */
/* command classification                                              */
/* ------------------------------------------------------------------ */

#define MT_PASS (-3)      /* not classified: legacy inline path */
#define MT_BLOCKED (-2)   /* reply -ERR (not supported in mt mode yet) */
#define MT_LOCAL (-1)     /* keyless: execute on the home worker */
#define MT_CROSSSLOT (-4) /* multi-key command spans workers */

static int mt_is_blocked(uint16_t cmd)
{
    switch (cmd) {
    case CMD_SUBSCRIBE:
    case CMD_UNSUBSCRIBE:
    case CMD_PUBLISH:
    case CMD_SHUTDOWN:
    case CMD_SYNC:
    case CMD_REPLICAOF:
    case CMD_SAVE:
    case CMD_LASTSAVE:
    case CMD_CLUSTER:
    case CMD_MIGRATE:
    case CMD_ASKING:
        return 1;
    default:
        return 0;
    }
}

static int mt_is_single_key(uint16_t cmd)
{
    switch (cmd) {
    case CMD_GET:
    case CMD_SET:
    case CMD_DUMP:
    case CMD_RESTORE:
    case CMD_INCR:
    case CMD_DECR:
    case CMD_APPEND:
    case CMD_STRLEN:
    case CMD_EXPIRE:
    case CMD_PEXPIRE:
    case CMD_EXPIREAT:
    case CMD_PEXPIREAT:
    case CMD_TTL:
    case CMD_PTTL:
    case CMD_PERSIST:
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
    case CMD_HSETNX:
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
    case CMD_CONFIG:
    case CMD_INFO:
    case CMD_DBSIZE:
    case CMD_FLUSHDB:
    case CMD_QUIT:
        return 1;
    default:
        return 0;
    }
}

/* Multi-key commands: every key must map to the same worker (same rule as
 * cluster CROSSSLOT). Key positions by command:
 *   MGET/DEL/UNLINK/EXISTS -> argv[1..]
 *   MSET                   -> argv[1], argv[3], ... (key/value pairs)
 *   SMOVE                  -> argv[1], argv[2] (source, destination) */
static int mt_multikey_target(int nworkers, uint16_t cmd,
                              const resp_value *argv, size_t argc)
{
    size_t i;
    int target = -2; /* unset */

    if (argc < 2)
        return MT_LOCAL; /* arity error: let the session report it */

    for (i = 1; i < argc; i++) {
        int w;
        if (cmd == CMD_MSET && (i % 2) == 0)
            continue; /* value position */
        if (cmd == CMD_SMOVE && i > 2)
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
 * MT_PASS. */
static int mt_classify(int nworkers, uint16_t cmd, const resp_value *argv,
                       size_t argc)
{
    if (mt_is_blocked(cmd))
        return MT_BLOCKED;
    if (mt_is_single_key(cmd)) {
        if (argc < 2 || argv[1].str == NULL)
            return MT_LOCAL; /* arity error: let the local session report it */
        return (int)(hash_slot(argv[1].str, argv[1].len) %
                     (uint32_t)nworkers);
    }
    switch (cmd) {
    case CMD_MGET:
    case CMD_MSET:
    case CMD_DEL:
    case CMD_UNLINK:
    case CMD_EXISTS:
    case CMD_SMOVE:
    case CMD_SINTER:
    case CMD_SUNION:
    case CMD_SDIFF:
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

/* Push a task with backpressure retry (the consumer is another thread and
 * always makes progress); kick the consumer when it may be asleep. */
static void mt_push_task(mt_spsc *q, mt_task *t, pal_wakeup *wake)
{
    int pr;
    while ((pr = mt_spsc_push(q, t)) < 0)
        pal_sleep_ms(1);
    if (pr == 1)
        (void)pal_wakeup_kick(wake);
}

/* Aggregate commands (DBSIZE sum, FLUSHDB broadcast): run the home part
 * inline, fan sub-tasks out to every other worker and finish when all
 * parts arrived. Runs on the home worker thread. */
static int mt_route_aggregate(worker *home, void *conn,
                              const resp_value *argv, size_t argc,
                              const char *raw, size_t rawlen, uint16_t cmd)
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
    if (agg == NULL) {
        /* OOM: degrade to the home-worker answer. */
        resp_buf_init(&local);
        command_execute_at(server_db(home->srv), argv, argc, &local,
                           pal_wall_ms());
        server_conn_out_append(home->srv, conn, local.data, local.len);
        resp_buf_free(&local);
        st->seq_write++;
        return 1;
    }
    agg->conn = conn;
    agg->home = home;
    agg->seq = seq;
    agg->cmd = cmd;
    agg->pending = home->ms->nworkers - 1;

    /* home part */
    resp_buf_init(&local);
    command_execute_at(server_db(home->srv), argv, argc, &local,
                       pal_wall_ms());
    mt_agg_accumulate(agg, &local);
    resp_buf_free(&local);

    /* fan out */
    for (i = 0; i < home->ms->nworkers; i++) {
        mt_task *t = NULL;
        mt_cmd_blob *blob;
        if (i == home->id)
            continue;
        blob = mt_blob_one(raw, rawlen);
        if (blob != NULL)
            t = mt_task_new(conn, home, seq, 1, 1, blob);
        if (t != NULL) {
            t->agg = agg;
            server_conn_mt_inc(conn);
            mt_push_task(&home->ms->workers[i].inbox[home->id], t,
                         &home->ms->workers[i].wakeup);
        } else {
            if (blob != NULL)
                mt_blobs_free(blob, 1);
            agg->pending--;
        }
    }
    if (agg->pending == 0)
        mt_agg_finish(home->srv, conn, st, agg);
    return 1;
}

/* Flush the open merge batch (if any) as one routed task. */
static void mt_batch_flush(worker *home, void *conn, mt_conn_state *st)
{
    int target = st->batch_target;
    mt_task *t;
    if (st->batch_n == 0)
        return;
    t = mt_task_new(conn, home, st->batch_seq, (uint32_t)st->batch_n,
                    (uint32_t)st->batch_n, st->batch);
    if (t == NULL) {
        mt_blobs_free(st->batch, st->batch_n);
    } else {
        server_conn_mt_inc(conn);
        mt_push_task(&home->ms->workers[target].inbox[home->id], t,
                     &home->ms->workers[target].wakeup);
    }
    st->batch = NULL;
    st->batch_n = 0;
    st->batch_cap = 0;
    st->batch_target = -1;
}

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
    b = &st->batch[st->batch_n];
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
        resp_buf_reserve(out, len);
        memcpy(out->data + out->len, data, len);
        out->len += len;
        st->seq_write++;
        return;
    }
    {
        mt_task *t = mt_task_new(conn, home, seq, 1, 0, NULL);
        if (t == NULL) {
            resp_buf_reserve(out, len);
            memcpy(out->data + out->len, data, len);
            out->len += len;
            st->seq_write++;
            return;
        }
        resp_buf_reserve(&t->reply, len);
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

/* Execute an EXEC bundle on db d: validate watches, then replay every
 * queued command, packing the replies into a RESP array. Watch references
 * are released on the owner (all entries map to this worker). */
static void mt_exec_on_db(db *d, mt_task *t, arena *ar)
{
    size_t i;
    int aborted = 0;
    for (i = 0; i < t->nexec_watches; i++) {
        mt_watch_entry *e = &t->exec_watches[i];
        if (db_key_version(d, e->key, e->klen) != e->version ||
            d->flush_epoch != e->epoch) {
            aborted = 1;
            break;
        }
    }
    if (d->watch_refs >= t->nexec_watches)
        d->watch_refs -= t->nexec_watches;
    else
        d->watch_refs = 0;
    if (aborted) {
        static const char nullarr[] = "*-1\r\n";
        resp_buf_reserve(&t->reply, sizeof(nullarr) - 1);
        memcpy(t->reply.data + t->reply.len, nullarr, sizeof(nullarr) - 1);
        t->reply.len += sizeof(nullarr) - 1;
        return;
    }
    resp_write_array_header(&t->reply, t->ncmds);
    for (i = 0; i < t->ncmds; i++) {
        resp_value v;
        ptrdiff_t used;
        arena_reset(ar);
        used = resp_parse(t->cmds[i].raw, t->cmds[i].len, &v, ar);
        if (used != (ptrdiff_t)t->cmds[i].len || v.type != RESP_ARRAY) {
            resp_write_error(&t->reply, "ERR Protocol error", 18);
            continue;
        }
        command_execute_at(d, v.items, v.count, &t->reply, pal_wall_ms());
    }
}

static int mt_txn_watch(worker *home, void *conn, mt_conn_state *st,
                        uint64_t seq, const resp_value *argv, size_t argc,
                        const char *raw, size_t rawlen, resp_buf *out)
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
        db *d = server_db(home->srv);
        size_t i;
        for (i = 1; i < argc; i++) {
            if (argv[i].str == NULL)
                continue;
            if (mt_watch_add(st, argv[i].str, argv[i].len,
                             db_key_version(d, argv[i].str, argv[i].len),
                             d->flush_epoch) != 0)
                break;
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
        server_conn_mt_inc(conn);
        mt_push_task(&home->ms->workers[target].inbox[home->id], t,
                     &home->ms->workers[target].wakeup);
        return 1;
    }
}

static int mt_txn_exec(worker *home, void *conn, mt_conn_state *st,
                       uint64_t seq, resp_buf *out)
{
    static const char empty[] = "*0\r\n";
    static const char err_execabort[] =
        "-EXECABORT Transaction discarded because of: keys hash to "
        "different slots\r\n";
    static const char err_oom[] = "-ERR out of memory\r\n";
    arena ar;
    int target = -1;
    int bad = 0;
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
        mt_reply_local(home, conn, st, seq, err_execabort,
                       sizeof(err_execabort) - 1, out);
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
        mt_exec_on_db(server_db(home->srv), t, &home->exec_arena);
        mt_reorder_insert(st, t);
        mt_drain_ready(home->srv, &home->exec_arena, conn, st, 1);
        return 1;
    }
    server_conn_mt_inc(conn);
    mt_push_task(&home->ms->workers[target].inbox[home->id], t,
                 &home->ms->workers[target].wakeup);
    return 1;
}

static int mt_route_txn(worker *home, void *conn, mt_conn_state *st,
                        const resp_value *argv, size_t argc, const char *raw,
                        size_t rawlen, uint16_t cmd, resp_buf *out)
{
    static const char queued[] = "+QUEUED\r\n";
    static const char ok[] = "+OK\r\n";
    static const char err_nested[] = "-ERR MULTI calls can not be nested\r\n";
    static const char err_no_exec[] = "-ERR EXEC without MULTI\r\n";
    static const char err_no_discard[] = "-ERR DISCARD without MULTI\r\n";
    static const char err_oom[] = "-ERR out of memory\r\n";
    uint64_t seq = st->seq_next++;

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
                            out);
    case CMD_EXEC:
        if (!st->in_multi) {
            mt_reply_local(home, conn, st, seq, err_no_exec,
                           sizeof(err_no_exec) - 1, out);
            return 1;
        }
        return mt_txn_exec(home, conn, st, seq, out);
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

/* Router installed on every worker's server. Runs on the home worker thread
 * inside conn_process_input. Returns non-zero when the command was handled
 * (locally, blocked, or forwarded). */
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
    if (st == NULL) {
        st = (mt_conn_state *)calloc(1, sizeof(*st));
        if (st == NULL)
            return 0;
        st->batch_target = -1;
        server_conn_set_mt_state(conn, st);
    }

    if (cmd == CMD_MULTI || cmd == CMD_EXEC || cmd == CMD_DISCARD ||
        cmd == CMD_WATCH || cmd == CMD_UNWATCH || st->in_multi) {
        mt_batch_flush(home, conn, st);
        return mt_route_txn(home, conn, st, argv, argc, raw, rawlen, cmd,
                            out);
    }

    if (cmd == CMD_DBSIZE || cmd == CMD_FLUSHDB) {
        mt_batch_flush(home, conn, st);
        return mt_route_aggregate(home, conn, argv, argc, raw, rawlen, cmd);
    }

    target = mt_classify(home->ms->nworkers, cmd, argv, argc);
    if (target == MT_PASS)
        return 0; /* legacy inline path (SINTER/SUNION/SDIFF for now) */

    seq = st->seq_next++;

    /* Forward to the owning worker: merge consecutive commands for the same
     * target into one task (flushed on target change / local command /
     * end of the parse loop). */
    if (target >= 0 && target != home->id) {
        if (st->batch_target >= 0 && st->batch_target != target)
            mt_batch_flush(home, conn, st);
        if (st->batch_target < 0) {
            st->batch_target = target;
            st->batch_seq = seq;
        }
        if (mt_batch_append(st, raw, rawlen) != 0) {
            mt_batch_flush(home, conn, st); /* keep the earlier commands */
            resp_write_error(out, "ERR out of memory", 17);
            st->seq_write++;
            return 1;
        }
        return 1;
    }

    /* Local / blocked / crossslot: any open batch must go out first to keep
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

static void worker_on_wakeup(void *ctx)
{
    worker *w = (worker *)ctx;
    int pi;
    (void)pal_wakeup_drain(&w->wakeup);

    /* 1. adopt accepted fds */
    for (;;) {
        void *p = mt_spsc_pop(&w->accepts);
        pal_socket_t fd;
        if (p == NULL)
            break;
        fd = (pal_socket_t)(uintptr_t)p;
        (void)server_adopt_fd(w->srv, fd);
    }

    /* 2. execute commands routed to this worker (merged tasks are re-parsed
     * per command into the worker's own arena: no argv deep copies) */
    for (pi = 0; pi < w->ms->nworkers; pi++) {
        for (;;) {
            mt_task *t = (mt_task *)mt_spsc_pop(&w->inbox[pi]);
            uint32_t ci;
            if (t == NULL)
                break;
            if (t->kind == MT_TASK_UNWATCH) {
                /* fire-and-forget watch_refs release (key bytes in cmds) */
                db *d = server_db(w->srv);
                if (d->watch_refs > 0)
                    d->watch_refs--;
                mt_task_free(t);
                continue;
            }
            if (t->kind == MT_TASK_WATCH) {
                /* read versions for every watched key; they ride back
                 * out-of-band with the +OK reply */
                resp_value v;
                ptrdiff_t used;
                size_t i;
                db *d = server_db(w->srv);
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
                    }
                }
                resp_write_simple_string(&t->reply, "OK", 2);
            } else if (t->kind == MT_TASK_EXEC) {
                mt_exec_on_db(server_db(w->srv), t, &w->exec_arena);
            } else {
                for (ci = 0; ci < t->ncmds; ci++) {
                    resp_value v;
                    ptrdiff_t used;
                    arena_reset(&w->exec_arena);
                    used = resp_parse(t->cmds[ci].raw, t->cmds[ci].len, &v,
                                      &w->exec_arena);
                    if (used != (ptrdiff_t)t->cmds[ci].len ||
                        v.type != RESP_ARRAY || v.is_null) {
                        resp_write_error(&t->reply, "ERR Protocol error",
                                         18);
                        continue;
                    }
                    command_execute_at(server_db(w->srv), v.items, v.count,
                                       &t->reply, pal_wall_ms());
                }
            }
            w->tasks_executed++;
            mt_push_task(&t->home->completions[w->id], t,
                         &t->home->wakeup);
        }
    }

    /* 3. deliver completed replies (home side) */
    for (pi = 0; pi < w->ms->nworkers; pi++) {
        for (;;) {
            mt_task *t = (mt_task *)mt_spsc_pop(&w->completions[pi]);
            void *conn;
            mt_conn_state *st;
            if (t == NULL)
                break;
            conn = t->conn;
            st = (mt_conn_state *)server_conn_mt_state(conn);
            if (t->agg != NULL) {
                /* broadcast part: accumulate and finish when complete */
                mt_agg *agg = t->agg;
                mt_agg_accumulate(agg, &t->reply);
                mt_task_free(t);
                agg->pending--;
                if (agg->pending == 0) {
                    if (st != NULL)
                        mt_agg_finish(w->srv, conn, st, agg);
                    else
                        free(agg);
                }
                server_conn_mt_dec(w->srv, conn);
                continue;
            }
            if (st != NULL) {
                mt_reorder_insert(st, t);
                if (server_conn_mt_is_zombie(conn)) {
                    mt_drain_ready(w->srv, &w->exec_arena, conn, st, 0);
                } else {
                    mt_drain_ready(w->srv, &w->exec_arena, conn, st, 1);
                    (void)server_conn_flush(w->srv, conn);
                }
            } else {
                mt_task_free(t);
            }
            server_conn_mt_dec(w->srv, conn);
        }
    }
}

static void *worker_main(void *arg)
{
    worker *w = (worker *)arg;
    while (w->running)
        (void)server_run_once(w->srv, 50);
    return NULL;
}

static void *acceptor_main(void *arg)
{
    mt_server *ms = (mt_server *)arg;
    pal_loop *l = pal_loop_create();
    int rr = 0;
    if (l == NULL)
        return NULL;
    if (pal_loop_add(l, ms->listen_fd, 1, 0, NULL) != 0) {
        pal_loop_free(l);
        return NULL;
    }
    while (ms->running) {
        pal_event evs[8];
        int n = pal_loop_wait(l, evs, 8, 50);
        int i;
        for (i = 0; i < n; i++) {
            if (evs[i].fd != ms->listen_fd || !evs[i].readable)
                continue;
            for (;;) {
                pal_socket_t fd = pal_accept(ms->listen_fd);
                worker *w;
                if (fd == PAL_SOCKET_INVALID)
                    break;
                w = &ms->workers[rr % ms->nworkers];
                rr++;
                {
                    int pr = mt_spsc_push(&w->accepts,
                                          (void *)(uintptr_t)fd);
                    if (pr < 0) {
                        pal_close(fd);
                        continue;
                    }
                    if (pr == 1)
                        (void)pal_wakeup_kick(&w->wakeup);
                }
            }
        }
    }
    pal_loop_free(l);
    return NULL;
}

mt_server *mt_server_create(const char *host, uint16_t port, int nworkers)
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
    ms->nworkers = nworkers;
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
        w->srv = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
        w->inbox = (mt_spsc *)calloc((size_t)nworkers, sizeof(mt_spsc));
        w->completions =
            (mt_spsc *)calloc((size_t)nworkers, sizeof(mt_spsc));
        if (w->srv == NULL || w->inbox == NULL || w->completions == NULL ||
            mt_spsc_init(&w->accepts, 256) != 0 ||
            pal_wakeup_create(&w->wakeup) != 0) {
            ms->nworkers = i; /* destroy only initialized workers */
            mt_server_destroy(ms);
            return NULL;
        }
        for (j = 0; j < nworkers; j++) {
            if (mt_spsc_init(&w->inbox[j], 1024) != 0 ||
                mt_spsc_init(&w->completions[j], 1024) != 0) {
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
    }
    return ms;
}

uint16_t mt_server_port(const mt_server *ms)
{
    return ms->port;
}

uint64_t mt_server_tasks_executed(const mt_server *ms)
{
    uint64_t total = 0;
    int i;
    for (i = 0; i < ms->nworkers; i++)
        total += ms->workers[i].tasks_executed;
    return total;
}

int mt_server_start(mt_server *ms)
{
    int i;
    ms->running = 1;
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        w->running = 1;
        if (pal_thread_create(&w->thread, worker_main, w) != 0) {
            w->running = 0;
            ms->running = 0;
            return -1;
        }
    }
    if (pal_thread_create(&ms->acceptor, acceptor_main, ms) != 0) {
        ms->running = 0;
        return -1;
    }
    return 0;
}

void mt_server_stop(mt_server *ms)
{
    int i;
    ms->running = 0;
    for (i = 0; i < ms->nworkers; i++) {
        ms->workers[i].running = 0;
        (void)pal_wakeup_kick(&ms->workers[i].wakeup);
    }
    (void)pal_thread_join(&ms->acceptor, NULL);
    for (i = 0; i < ms->nworkers; i++)
        (void)pal_thread_join(&ms->workers[i].thread, NULL);
}

void mt_server_destroy(mt_server *ms)
{
    int i;
    if (ms == NULL)
        return;
    pal_close(ms->listen_fd);
    for (i = 0; i < ms->nworkers; i++) {
        worker *w = &ms->workers[i];
        if (w->srv == NULL)
            continue;
        if (w->inbox != NULL) {
            int j;
            void *p;
            for (j = 0; j < ms->nworkers; j++) {
                while ((p = mt_spsc_pop(&w->inbox[j])) != NULL)
                    mt_task_free(p);
                mt_spsc_destroy(&w->inbox[j]);
                while ((p = mt_spsc_pop(&w->completions[j])) != NULL)
                    mt_task_free(p);
                mt_spsc_destroy(&w->completions[j]);
            }
            free(w->inbox);
            free(w->completions);
        }
        {
            void *p;
            while ((p = mt_spsc_pop(&w->accepts)) != NULL)
                pal_close((pal_socket_t)(uintptr_t)p);
            mt_spsc_destroy(&w->accepts);
        }
        pal_wakeup_destroy(&w->wakeup);
        arena_destroy(&w->exec_arena);
        server_destroy(w->srv);
    }
    free(ms->workers);
    free(ms);
}
