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
    if (s == NULL)
        return;
    memset(s, 0, sizeof(*s));
    s->d = d;
    s->authed = 1; /* no password configured: start authenticated */
    s->sel_ndbs = 1; /* no selection hook: only db 0 */
}

void session_queue_clear(session *s)
{
    size_t i, j;
    if (s == NULL)
        return;
    for (i = 0; i < s->queue_len; i++) {
        for (j = 0; j < s->queue[i].argc; j++)
            free((void *)s->queue[i].argv[j].str);
        free(s->queue[i].argv);
    }
    s->queue_len = 0;
}

void session_block_clear(session *s)
{
    size_t i;
    if (s == NULL)
        return;
    if (!s->blocked)
        return;
    for (i = 0; i < s->blocked_argc; i++)
        free((void *)s->blocked_argv[i].str);
    free(s->blocked_argv);
    s->blocked_argv = NULL;
    s->blocked_argc = 0;
    s->blocked_cmd = 0;
    s->blocked_deadline_ms = 0;
    s->blocked = 0;
}

int session_block_start(session *s, const resp_value *argv, size_t argc,
                        uint16_t cmd_id, uint64_t deadline_ms)
{
    resp_value *copy_argv;
    size_t i;
    if (s == NULL || (argv == NULL && argc != 0) ||
        argc > SIZE_MAX / sizeof(*copy_argv))
        return -1;
    copy_argv = argc == 0 ? NULL
                          : (resp_value *)malloc(argc * sizeof(*copy_argv));
    if (argc != 0 && copy_argv == NULL)
        return -1;
    for (i = 0; i < argc; i++) {
        if (argv[i].str == NULL && argv[i].len != 0) {
            while (i > 0)
                free((void *)copy_argv[--i].str);
            free(copy_argv);
            return -1;
        }
        char *copy = NULL;
        if (argv[i].len != 0) {
            copy = (char *)malloc(argv[i].len);
        }
        if (argv[i].len != 0 && copy == NULL) {
            while (i > 0)
                free((void *)copy_argv[--i].str);
            free(copy_argv);
            return -1;
        }
        if (argv[i].len != 0)
            memcpy(copy, argv[i].str, argv[i].len);
        copy_argv[i] = argv[i];
        copy_argv[i].str = copy;
        copy_argv[i].items = NULL;
    }
    session_block_clear(s);
    s->blocked = 1;
    s->blocked_cmd = cmd_id;
    s->blocked_deadline_ms = deadline_ms;
    s->blocked_argv = copy_argv;
    s->blocked_argc = argc;
    return 0;
}

void session_watch_clear(session *s)
{
    size_t i;
    if (s == NULL)
        return;
    for (i = 0; i < s->nwatch; i++)
        free(s->watches[i].key);
    if (s->d != NULL) {
        for (i = 0; i < s->nwatch; i++) {
            db *d = s->d;
            if (s->sel_fn != NULL)
                d = s->sel_fn(s->sel_ctx, s->watches[i].db_index);
            if (d != NULL && d->watch_refs > 0)
                d->watch_refs--;
        }
    }
    s->nwatch = 0;
}

void session_release(session *s)
{
    size_t i, j;
    if (s == NULL)
        return;
    session_block_clear(s);
    session_queue_clear(s);
    session_watch_clear(s);
    for (i = 0; i < s->himport_len; i++) {
        himport_fieldset *fs = &s->himport_sets[i];
        free(fs->name);
        for (j = 0; j < fs->count; j++)
            free(fs->fields[j]);
        free(fs->fields);
        free(fs->field_lens);
    }
    free(s->himport_sets);
    s->himport_sets = NULL;
    s->himport_len = 0;
    s->himport_cap = 0;
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

static int session_queue_bytes(size_t count, size_t *bytes)
{
    if (bytes == NULL)
        return -1;
    if (count > SIZE_MAX / sizeof(resp_value))
        return -1;
    *bytes = count * sizeof(resp_value);
    return 0;
}

static int session_queue_growth(size_t cap, size_t *new_cap)
{
    if (new_cap == NULL)
        return -1;
    if (cap == 0) {
        *new_cap = 8;
        return 0;
    }
    if (cap > SIZE_MAX / 2)
        return -1;
    *new_cap = cap * 2;
    return 0;
}

#ifdef DDUP_TESTING
int session_test_queue_bytes(size_t count, size_t *bytes)
{
    return session_queue_bytes(count, bytes);
}

int session_test_queue_growth(size_t cap, size_t *new_cap)
{
    return session_queue_growth(cap, new_cap);
}
#endif

int session_queue_push(session *s, const resp_value *argv, size_t argc)
{
    queued_cmd *qc;
    resp_value *copy_argv;
    size_t argv_bytes;
    size_t ncap;
    size_t i;
    if (s == NULL || (argv == NULL && argc != 0) ||
        session_queue_bytes(argc, &argv_bytes) != 0)
        return -1;
    ncap = s->queue_cap;
    if (s->queue_len == s->queue_cap &&
        (session_queue_growth(s->queue_cap, &ncap) != 0 ||
         ncap > SIZE_MAX / sizeof(*s->queue)))
        return -1;
    copy_argv = argv_bytes == 0 ? NULL : (resp_value *)xmalloc(argv_bytes);
    for (i = 0; i < argc; i++) {
        if (argv[i].str == NULL && argv[i].len != 0) {
            while (i > 0)
                free((void *)copy_argv[--i].str);
            free(copy_argv);
            return -1;
        }
        char *copy = NULL;
        if (argv[i].len != 0) {
            copy = (char *)xmalloc(argv[i].len);
            memcpy(copy, argv[i].str, argv[i].len);
        }
        copy_argv[i] = argv[i];
        copy_argv[i].str = copy;
        copy_argv[i].items = NULL;
    }
    if (s->queue_len == s->queue_cap) {
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
    qc->skip_log = 0;
    qc->argv = copy_argv;
    return 0;
}

void session_watch_add(session *s, const char *key, size_t klen,
                       uint64_t version, uint64_t epoch, int db_index)
{
    watch_entry *w;
    if (s == NULL || (key == NULL && klen != 0))
        return;
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
    w->db_index = db_index;
    if (s->d != NULL)
        s->d->watch_refs++;
}
