/* mt_server.c - thread-per-core server skeleton; see mt_server.h.
 *
 * Acceptor thread: owns the public (non-blocking) listener, accepts pending
 * connections and pushes each fd onto one worker's accept queue, kicking the
 * worker's wakeup pipe. Worker threads run an ordinary server event loop;
 * the wakeup callback drains the accept queue and adopts the fds.
 */
#include "server/mt_server.h"

#include <stdlib.h>

#include "pal/pal_event.h"
#include "pal/pal_socket.h"
#include "pal/pal_thread.h"
#include "pal/pal_wakeup.h"
#include "server/server.h"

typedef struct fd_node {
    struct fd_node *next;
    pal_socket_t fd;
} fd_node;

typedef struct fd_queue {
    pal_mutex mu;
    fd_node *head;
    fd_node *tail;
} fd_queue;

typedef struct worker {
    int id;
    server *srv;
    pal_thread thread;
    pal_wakeup wakeup;
    fd_queue accepts;
    volatile int running;
} worker;

struct mt_server {
    pal_socket_t listen_fd;
    uint16_t port;
    int nworkers;
    worker *workers;
    pal_thread acceptor;
    volatile int running;
};

static int fd_queue_init(fd_queue *q)
{
    q->head = NULL;
    q->tail = NULL;
    return pal_mutex_init(&q->mu);
}

static void fd_queue_destroy(fd_queue *q)
{
    fd_node *n = q->head;
    while (n != NULL) {
        fd_node *next = n->next;
        pal_close(n->fd);
        free(n);
        n = next;
    }
    q->head = NULL;
    q->tail = NULL;
    pal_mutex_destroy(&q->mu);
}

static int fd_queue_push(fd_queue *q, pal_socket_t fd)
{
    fd_node *n = (fd_node *)malloc(sizeof(*n));
    if (n == NULL)
        return -1;
    n->fd = fd;
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

static pal_socket_t fd_queue_pop(fd_queue *q)
{
    fd_node *n;
    pal_socket_t fd;
    pal_mutex_lock(&q->mu);
    n = q->head;
    if (n == NULL) {
        pal_mutex_unlock(&q->mu);
        return PAL_SOCKET_INVALID;
    }
    q->head = n->next;
    if (q->head == NULL)
        q->tail = NULL;
    pal_mutex_unlock(&q->mu);
    fd = n->fd;
    free(n);
    return fd;
}

static void worker_on_wakeup(void *ctx)
{
    worker *w = (worker *)ctx;
    (void)pal_wakeup_drain(&w->wakeup);
    for (;;) {
        pal_socket_t fd = fd_queue_pop(&w->accepts);
        if (fd == PAL_SOCKET_INVALID)
            break;
        (void)server_adopt_fd(w->srv, fd);
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
                if (fd_queue_push(&w->accepts, fd) != 0) {
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
        w->srv = server_create_ex("127.0.0.1", 0, SERVER_BACKEND_SELECT);
        if (w->srv == NULL ||
            fd_queue_init(&w->accepts) != 0 ||
            pal_wakeup_create(&w->wakeup) != 0) {
            ms->nworkers = i; /* destroy only initialized workers */
            mt_server_destroy(ms);
            return NULL;
        }
        server_close_listener(w->srv);
        if (server_set_wakeup(w->srv, w->wakeup.wait_fd,
                              worker_on_wakeup, w) != 0) {
            ms->nworkers = i + 1;
            mt_server_destroy(ms);
            return NULL;
        }
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
            fd_queue_destroy(&w->accepts);
            pal_wakeup_destroy(&w->wakeup);
            server_destroy(w->srv);
        }
    }
    free(ms->workers);
    free(ms);
}
