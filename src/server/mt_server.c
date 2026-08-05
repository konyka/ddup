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

#include "core/command.h"
#include "core/hashslot.h"
#include "core/session.h"
#include "pal/pal_event.h"
#include "pal/pal_socket.h"
#include "pal/pal_thread.h"
#include "pal/pal_time.h"
#include "pal/pal_wakeup.h"
#include "resp/resp_writer.h"
#include "server/server.h"

/* ------------------------------------------------------------------ */
/* generic mutex-protected pointer queue                               */
/* ------------------------------------------------------------------ */

typedef struct qnode {
    struct qnode *next;
    void *ptr;
} qnode;

typedef struct mt_queue {
    pal_mutex mu;
    qnode *head;
    qnode *tail;
} mt_queue;

static int mt_queue_init(mt_queue *q)
{
    q->head = NULL;
    q->tail = NULL;
    return pal_mutex_init(&q->mu);
}

static void mt_queue_destroy(mt_queue *q, void (*free_fn)(void *))
{
    qnode *n = q->head;
    while (n != NULL) {
        qnode *next = n->next;
        if (free_fn != NULL)
            free_fn(n->ptr);
        free(n);
        n = next;
    }
    q->head = NULL;
    q->tail = NULL;
    pal_mutex_destroy(&q->mu);
}

static int mt_queue_push(mt_queue *q, void *ptr)
{
    qnode *n = (qnode *)malloc(sizeof(*n));
    if (n == NULL)
        return -1;
    n->ptr = ptr;
    n->next = NULL;
    pal_mutex_lock(&q->mu);
    if (q->tail != NULL)
        q->tail->next = n;
    else
        q->head = n;
    q->tail = n;
    pal_mutex_unlock(&q->mu);
    return 0;
}

static void *mt_queue_pop(mt_queue *q)
{
    qnode *n;
    void *ptr;
    pal_mutex_lock(&q->mu);
    n = q->head;
    if (n == NULL) {
        pal_mutex_unlock(&q->mu);
        return NULL;
    }
    q->head = n->next;
    if (q->head == NULL)
        q->tail = NULL;
    pal_mutex_unlock(&q->mu);
    ptr = n->ptr;
    free(n);
    return ptr;
}

/* ------------------------------------------------------------------ */
/* routed tasks                                                        */
/* ------------------------------------------------------------------ */

typedef struct worker worker;

typedef struct mt_task {
    struct mt_task *next; /* reorder-buffer link (queue nodes wrap tasks) */
    void *conn;           /* home connection (opaque, home-thread only) */
    worker *home;         /* connection's home worker */
    uint64_t seq;         /* per-conn pipeline sequence number */
    resp_value *argv;     /* deep-copied command (routed tasks only) */
    size_t argc;
    resp_buf reply;       /* filled by the executing worker */
} mt_task;

/* Per-conn routing state (stored via server_conn_set_mt_state). */
typedef struct mt_conn_state {
    uint64_t seq_next;  /* next sequence number to assign */
    uint64_t seq_write; /* next sequence number to append to conn->out */
    mt_task *reorder;   /* ready replies waiting, sorted by seq */
} mt_conn_state;

static resp_value *mt_copy_argv(const resp_value *argv, size_t argc)
{
    resp_value *copy = (resp_value *)calloc(argc, sizeof(resp_value));
    size_t i;
    if (copy == NULL)
        return NULL;
    for (i = 0; i < argc; i++) {
        copy[i] = argv[i];
        copy[i].items = NULL;
        if (argv[i].str != NULL && argv[i].len > 0) {
            char *s = (char *)malloc(argv[i].len);
            if (s == NULL) {
                size_t j;
                for (j = 0; j < i; j++)
                    free((void *)copy[j].str);
                free(copy);
                return NULL;
            }
            memcpy(s, argv[i].str, argv[i].len);
            copy[i].str = s;
        }
    }
    return copy;
}

static mt_task *mt_task_new(void *conn, worker *home, uint64_t seq,
                            const resp_value *argv, size_t argc)
{
    mt_task *t = (mt_task *)calloc(1, sizeof(*t));
    if (t == NULL)
        return NULL;
    t->conn = conn;
    t->home = home;
    t->seq = seq;
    resp_buf_init(&t->reply);
    if (argv != NULL) {
        t->argv = mt_copy_argv(argv, argc);
        if (t->argv == NULL) {
            free(t);
            return NULL;
        }
        t->argc = argc;
    }
    return t;
}

static void mt_task_free(void *ptr)
{
    mt_task *t = (mt_task *)ptr;
    size_t i;
    if (t == NULL)
        return;
    if (t->argv != NULL) {
        for (i = 0; i < t->argc; i++)
            free((void *)t->argv[i].str);
        free(t->argv);
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
        st->seq_write++;
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
    free(st);
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
        return mt_multikey_target(nworkers, cmd, argv, argc);
    default:
        break;
    }
    if (mt_is_keyless(cmd))
        return MT_LOCAL;
    return MT_PASS; /* SINTER/SUNION/SDIFF stay on the legacy path for now */
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
    mt_queue accepts;     /* pal_socket_t values as void* */
    mt_queue inbox;       /* mt_task*: commands to execute on this worker */
    mt_queue completions; /* mt_task*: executed replies to deliver home */
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

/* Router installed on every worker's server. Runs on the home worker thread
 * inside conn_process_input. Returns non-zero when the command was handled
 * (locally, blocked, or forwarded). */
static int mt_route(void *ctx, void *conn, session *sess,
                    const resp_value *argv, size_t argc, resp_buf *out)
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
    target = mt_classify(home->ms->nworkers, cmd, argv, argc);
    if (target == MT_PASS)
        return 0; /* legacy inline path (SINTER/SUNION/SDIFF for now) */

    st = (mt_conn_state *)server_conn_mt_state(conn);
    if (st == NULL) {
        st = (mt_conn_state *)calloc(1, sizeof(*st));
        if (st == NULL)
            return 0;
        server_conn_set_mt_state(conn, st);
    }
    seq = st->seq_next++;

    /* Forward to the owning worker. */
    if (target >= 0 && target != home->id) {
        mt_task *t = mt_task_new(conn, home, seq, argv, argc);
        if (t == NULL) {
            resp_write_error(out, "ERR out of memory", 17);
            st->seq_write++;
            return 1;
        }
        server_conn_mt_inc(conn);
        if (mt_queue_push(&home->ms->workers[target].inbox, t) != 0) {
            server_conn_mt_dec(home->srv, conn);
            mt_task_free(t);
            resp_write_error(out, "ERR out of memory", 17);
            st->seq_write++;
            return 1;
        }
        (void)pal_wakeup_kick(&home->ms->workers[target].wakeup);
        return 1;
    }

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
        mt_task *t = mt_task_new(conn, home, seq, NULL, 0);
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
    (void)pal_wakeup_drain(&w->wakeup);

    /* 1. adopt accepted fds */
    for (;;) {
        void *p = mt_queue_pop(&w->accepts);
        pal_socket_t fd;
        if (p == NULL)
            break;
        fd = (pal_socket_t)(uintptr_t)p;
        (void)server_adopt_fd(w->srv, fd);
    }

    /* 2. execute commands routed to this worker */
    for (;;) {
        mt_task *t = (mt_task *)mt_queue_pop(&w->inbox);
        if (t == NULL)
            break;
        command_execute_at(server_db(w->srv), t->argv, t->argc, &t->reply,
                           pal_wall_ms());
        if (mt_queue_push(&t->home->completions, t) == 0)
            (void)pal_wakeup_kick(&t->home->wakeup);
        else
            mt_task_free(t); /* OOM: drop the reply (conn times out) */
    }

    /* 3. deliver completed replies (home side) */
    for (;;) {
        mt_task *t = (mt_task *)mt_queue_pop(&w->completions);
        void *conn;
        mt_conn_state *st;
        if (t == NULL)
            break;
        conn = t->conn;
        st = (mt_conn_state *)server_conn_mt_state(conn);
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
                if (mt_queue_push(&w->accepts,
                                  (void *)(uintptr_t)fd) != 0) {
                    pal_close(fd);
                    continue;
                }
                (void)pal_wakeup_kick(&w->wakeup);
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
        w->id = i;
        w->ms = ms;
        w->srv = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
        if (w->srv == NULL || mt_queue_init(&w->accepts) != 0 ||
            mt_queue_init(&w->inbox) != 0 ||
            mt_queue_init(&w->completions) != 0 ||
            pal_wakeup_create(&w->wakeup) != 0) {
            ms->nworkers = i; /* destroy only initialized workers */
            mt_server_destroy(ms);
            return NULL;
        }
        server_close_listener(w->srv);
        if (server_set_wakeup(w->srv, w->wakeup.wait_fd, worker_on_wakeup,
                              w) != 0) {
            ms->nworkers = i + 1;
            mt_server_destroy(ms);
            return NULL;
        }
        server_set_route(w->srv, mt_route, mt_state_free_cb, w);
    }
    return ms;
}

uint16_t mt_server_port(const mt_server *ms)
{
    return ms->port;
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
        if (w->srv != NULL) {
            mt_queue_destroy(&w->accepts, NULL);
            mt_queue_destroy(&w->inbox, mt_task_free);
            mt_queue_destroy(&w->completions, mt_task_free);
            pal_wakeup_destroy(&w->wakeup);
            server_destroy(w->srv);
        }
    }
    free(ms->workers);
    free(ms);
}
