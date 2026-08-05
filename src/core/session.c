/* session.c - session memory management; see session.h.
 *
 * session_execute_at() lives in command.c next to the dispatch it wraps.
 */
#include "core/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void session_init(session *s, db *d)
{
    memset(s, 0, sizeof(*s));
    s->d = d;
    s->authed = 1; /* no password configured: start authenticated */
    s->sel_ndbs = 1; /* no selection hook: only db 0 */
}

void session_queue_clear(session *s)
{
    size_t i, j;
    for (i = 0; i < s->queue_len; i++) {
        for (j = 0; j < s->queue[i].argc; j++)
            free((void *)s->queue[i].argv[j].str);
        free(s->queue[i].argv);
    }
    s->queue_len = 0;
}

void session_watch_clear(session *s)
{
    size_t i;
    for (i = 0; i < s->nwatch; i++)
        free(s->watches[i].key);
    if (s->d != NULL && s->d->watch_refs >= s->nwatch)
        s->d->watch_refs -= s->nwatch;
    s->nwatch = 0;
}

void session_release(session *s)
{
    session_queue_clear(s);
    session_watch_clear(s);
    free(s->queue);
    free(s->watches);
    s->queue = NULL;
    s->watches = NULL;
    s->queue_cap = 0;
    s->watch_cap = 0;
}

session *session_create(db *d)
{
    session *s = (session *)malloc(sizeof(*s));
    if (s == NULL)
        return NULL;
    session_init(s, d);
    return s;
}

void session_free(session *s)
{
    if (s == NULL)
        return;
    session_release(s);
    free(s);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    return p;
}

void session_queue_push(session *s, const resp_value *argv, size_t argc)
{
    queued_cmd *qc;
    size_t i;
    if (s->queue_len == s->queue_cap) {
        size_t ncap = s->queue_cap == 0 ? 8 : s->queue_cap * 2;
        queued_cmd *nq =
            (queued_cmd *)realloc(s->queue, ncap * sizeof(*nq));
        if (nq == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        s->queue = nq;
        s->queue_cap = ncap;
    }
    qc = &s->queue[s->queue_len++];
    qc->argc = argc;
    qc->argv = (resp_value *)xmalloc(argc * sizeof(resp_value));
    for (i = 0; i < argc; i++) {
        char *copy = (char *)xmalloc(argv[i].len);
        memcpy(copy, argv[i].str, argv[i].len);
        qc->argv[i] = argv[i];
        qc->argv[i].str = copy;
        qc->argv[i].items = NULL;
    }
}

void session_watch_add(session *s, const char *key, size_t klen,
                       uint64_t version, uint64_t epoch)
{
    watch_entry *w;
    if (s->nwatch == s->watch_cap) {
        size_t ncap = s->watch_cap == 0 ? 4 : s->watch_cap * 2;
        watch_entry *nw =
            (watch_entry *)realloc(s->watches, ncap * sizeof(*nw));
        if (nw == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        s->watches = nw;
        s->watch_cap = ncap;
    }
    w = &s->watches[s->nwatch++];
    w->key = (char *)xmalloc(klen);
    memcpy(w->key, key, klen);
    w->klen = klen;
    w->version = version;
    w->epoch = epoch;
    if (s->d != NULL)
        s->d->watch_refs++;
}
