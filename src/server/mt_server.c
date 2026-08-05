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

typedef struct mt_task {
    struct mt_task *next; /* reorder-buffer link (queue nodes wrap tasks) */
    void *conn;           /* home connection (opaque, home-thread only) */
    worker *home;         /* connection's home worker */
    uint64_t seq;         /* per-conn pipeline sequence base */
    uint32_t span;        /* commands covered (merged batch size, >= 1) */
    mt_cmd_blob *cmds;    /* span raw command copies (routed tasks only) */
    resp_buf reply;       /* filled by the executing worker */
    mt_agg *agg;          /* broadcast group this task is a part of (or NULL) */
} mt_task;

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
                            uint32_t span, mt_cmd_blob *cmds)
{
    mt_task *t = (mt_task *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;
    t->conn = conn;
    t->home = home;
    t->seq = seq;
    t->span = span == 0 ? 1 : span;
    t->cmds = cmds; /* ownership transferred */
    resp_buf_init(&t->reply);
    return t;
}

static void mt_task_free(void *ptr)
{
    mt_task *t = (mt_task *)ptr;
    if (t == NULL)
        return;
    mt_blobs_free(t->cmds, t->span);
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

/* Append every consecutive ready reply to conn->out (or drop them when the
 * conn is a zombie: the client is gone, only resource cleanup matters). */
static void mt_drain_ready(server *srv, void *conn, mt_conn_state *st,
                           int append)
{
    while (st->reorder != NULL && st->reorder->seq == st->seq_write) {
        mt_task *t = st->reorder;
        st->reorder = t->next;
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
    mt_task *fin = mt_task_new(conn, agg->home, agg->seq, 1, NULL);
    if (fin != NULL) {
        if (agg->cmd == CMD_DBSIZE)
            resp_write_integer(&fin->reply, agg->sum);
        else
            resp_write_simple_string(&fin->reply, "OK", 2);
        mt_reorder_insert(st, fin);
        if (server_conn_mt_is_zombie(conn)) {
            mt_drain_ready(srv, conn, st, 0);
        } else {
            mt_drain_ready(srv, conn, st, 1);
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
    case CMD_MULTI:
    case CMD_EXEC:
    case CMD_DISCARD:
    case CMD_WATCH:
    case CMD_UNWATCH:
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
            t = mt_task_new(conn, home, seq, 1, blob);
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
                    st->batch);
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
        mt_task *t = mt_task_new(conn, home, seq, 1, NULL);
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
        mt_drain_ready(home->srv, conn, st, 1);
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
            for (ci = 0; ci < t->span; ci++) {
                resp_value v;
                ptrdiff_t used;
                arena_reset(&w->exec_arena);
                used = resp_parse(t->cmds[ci].raw, t->cmds[ci].len, &v,
                                  &w->exec_arena);
                if (used != (ptrdiff_t)t->cmds[ci].len ||
                    v.type != RESP_ARRAY || v.is_null) {
                    resp_write_error(&t->reply, "ERR Protocol error", 18);
                    continue;
                }
                command_execute_at(server_db(w->srv), v.items, v.count,
                                   &t->reply, pal_wall_ms());
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
                    mt_drain_ready(w->srv, conn, st, 0);
                } else {
                    mt_drain_ready(w->srv, conn, st, 1);
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
