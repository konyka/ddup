/* aof.c - append-only file persistence; see aof.h. */
#include "server/aof.h"

#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/snapshot.h"
#include "ds/obj.h"
#include "pal/pal_time.h"
#include "resp/resp_parser.h"

static void aof_free_value_cb(const char *key, size_t klen, const char *val,
                              size_t vlen, void *ctx)
{
    (void)key;
    (void)klen;
    (void)ctx;
    obj_free_value(val, vlen);
}

/* Transfer only replayed key data. Configuration, WATCH state, and command
 * statistics remain owned by the caller's database. */
static void aof_swap_db_data(db *d, db *tmp)
{
    rh_each(&d->table, aof_free_value_cb, NULL);
    rh_destroy(&d->table);
    rh_destroy(&d->expires);
    d->table = tmp->table;
    d->expires = tmp->expires;
    d->used_memory = tmp->used_memory;
    d->dirty += tmp->dirty;
    d->flush_epoch++;
    memset(&tmp->table, 0, sizeof(tmp->table));
    memset(&tmp->expires, 0, sizeof(tmp->expires));
}

typedef struct aof_replay_dbs {
    db *dbs;
    int ndbs;
} aof_replay_dbs;

static db *aof_replay_select(void *ctx, int idx)
{
    aof_replay_dbs *set = (aof_replay_dbs *)ctx;
    if (idx < 0 || idx >= set->ndbs)
        return NULL;
    return &set->dbs[idx];
}

static int aof_clone_db_data(db *dst, db *src)
{
    resp_buf snapshot;
    int rc;

    resp_buf_init(&snapshot);
    rc = snapshot_serialize(src, &snapshot);
    if (rc == 0)
        rc = snapshot_load_mem(dst, snapshot.data, snapshot.len, pal_wall_ms());
    resp_buf_free(&snapshot);
    return rc;
}

aof *aof_open(const char *path)
{
    aof *a = (aof *)calloc(1, sizeof(*a));
    if (a == NULL)
        return NULL;
    a->f = pal_file_open_append(path);
    if (a->f == NULL) {
        free(a);
        return NULL;
    }
    resp_buf_init(&a->pending);
    a->write_fn = pal_file_write;
    a->fsync_mode = AOF_FSYNC_EVERYSEC;
    a->sync_fn = pal_file_sync;
    a->now_fn = pal_wall_ms;
    return a;
}

void aof_log_cmd(aof *a, const resp_value *argv, size_t argc)
{
    size_t i;
    if (a->failed)
        return;
    resp_write_array_header(&a->pending, argc);
    for (i = 0; i < argc; i++) {
        if (argv[i].type == RESP_BULK_STRING ||
            argv[i].type == RESP_SIMPLE_STRING) {
            resp_write_bulk(&a->pending, argv[i].str, argv[i].len);
        } else {
            resp_write_bulk(&a->pending, "", 0);
        }
    }
}

void aof_set_fsync_mode(aof *a, int mode)
{
    a->fsync_mode = mode;
}

/* Durability sync after a successful flush, per the appendfsync policy.
 * 0 on success (or no sync due), -1 when the sync itself failed. */
static int aof_policy_sync(aof *a)
{
    uint64_t now;
    if (a->fsync_mode == AOF_FSYNC_NO)
        return 0;
    now = a->now_fn();
    if (a->fsync_mode == AOF_FSYNC_EVERYSEC &&
        now - a->last_sync_ms < 1000)
        return 0;
    if (a->sync_fn(a->f) != 0)
        return -1;
    a->last_sync_ms = now;
    return 0;
}

int aof_flush(aof *a)
{
    size_t written = 0;
    int had_pending;
    if (a->failed)
        return -1;
    had_pending = a->pending.len > 0;
    while (written < a->pending.len) {
        ptrdiff_t n = a->write_fn(a->f, a->pending.data + written,
                                  a->pending.len - written);
        if (n <= 0) {
            if (written > 0) {
                memmove(a->pending.data, a->pending.data + written,
                        a->pending.len - written);
                a->pending.len -= written;
            }
            a->failed = 1;
            return -1;
        }
        written += (size_t)n;
    }
    if (pal_file_flush(a->f) != 0) {
        a->failed = 1;
        return -1;
    }
    a->pending.len = 0;
    /* the bytes made it to the OS; durability sync failures latch the same
     * fail-closed state as write/flush failures */
    if (had_pending && aof_policy_sync(a) != 0) {
        a->failed = 1;
        return -1;
    }
    return 0;
}

void aof_test_set_write_fn(
    aof *a, ptrdiff_t (*write_fn)(pal_file *f, const void *buf, size_t n))
{
    a->write_fn = write_fn != NULL ? write_fn : pal_file_write;
}

void aof_test_set_sync_fn(aof *a, int (*sync_fn)(pal_file *f))
{
    a->sync_fn = sync_fn != NULL ? sync_fn : pal_file_sync;
}

void aof_test_set_now_fn(aof *a, uint64_t (*now_fn)(void))
{
    a->now_fn = now_fn != NULL ? now_fn : pal_wall_ms;
}

void aof_close(aof *a)
{
    if (a == NULL)
        return;
    aof_flush(a);
    /* graceful exit: one last durability sync, throttle window ignored
     * (best-effort -- the file is being closed either way) */
    if (!a->failed && a->fsync_mode != AOF_FSYNC_NO)
        (void)a->sync_fn(a->f);
    pal_file_close(a->f);
    resp_buf_free(&a->pending);
    free(a);
}

static int aof_replay_impl(session *s, db *d, const char *path)
{
    pal_file *f = pal_file_open_read(path);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    arena ar;
    resp_buf out;
    session replay;
    aof_replay_dbs replay_dbs;
    db *tmp_dbs = NULL;
    db **target_dbs = NULL;
    int ndbs = 1;
    size_t off = 0;
    int rc = 0;

    if (f == NULL)
        return -1;
    /* slurp the whole file (dev-scale AOFs) */
    for (;;) {
        ptrdiff_t n;
        if (len == cap) {
            size_t ncap = cap == 0 ? 65536 : cap * 2;
            char *nb = (char *)realloc(buf, ncap);
            if (nb == NULL) {
                free(buf);
                pal_file_close(f);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        n = pal_file_read(f, buf + len, cap - len);
        if (n < 0) {
            free(buf);
            pal_file_close(f);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    pal_file_close(f);

    if (s != NULL && s->sel_fn != NULL)
        ndbs = s->sel_ndbs;
    if (ndbs < 1) {
        free(buf);
        return -1;
    }
    tmp_dbs = (db *)calloc((size_t)ndbs, sizeof(*tmp_dbs));
    target_dbs = (db **)calloc((size_t)ndbs, sizeof(*target_dbs));
    if (tmp_dbs == NULL || target_dbs == NULL) {
        free(target_dbs);
        free(tmp_dbs);
        free(buf);
        return -1;
    }
    {
        int i;
        for (i = 0; i < ndbs; i++)
            db_init(&tmp_dbs[i]);
        for (i = 0; i < ndbs; i++) {
            target_dbs[i] = s != NULL ? s->d : d;
            if (s != NULL && s->sel_fn != NULL)
                target_dbs[i] = s->sel_fn(s->sel_ctx, i);
            if (target_dbs[i] == NULL) {
                rc = -1;
                break;
            }
            if (aof_clone_db_data(&tmp_dbs[i], target_dbs[i]) != 0) {
                rc = -1;
                break;
            }
            tmp_dbs[i].maxmemory = target_dbs[i]->maxmemory;
            tmp_dbs[i].maxmemory_policy = target_dbs[i]->maxmemory_policy;
        }
        if (rc != 0) {
            int j;
            for (j = 0; j < ndbs; j++)
                db_destroy(&tmp_dbs[j]);
            free(target_dbs);
            free(tmp_dbs);
            free(buf);
            return -1;
        }
    }
    replay_dbs.dbs = tmp_dbs;
    replay_dbs.ndbs = ndbs;
    session_init(&replay, &tmp_dbs[0]);
    if (s != NULL) {
        replay.authed = s->authed;
        replay.requirepass = s->requirepass;
        replay.db_index = s->db_index;
        if (s->sel_fn != NULL) {
            if (replay.db_index < 0 || replay.db_index >= ndbs) {
                session_release(&replay);
                {
                    int i;
                    for (i = 0; i < ndbs; i++)
                        db_destroy(&tmp_dbs[i]);
                }
                free(target_dbs);
                free(tmp_dbs);
                free(buf);
                return -1;
            }
            replay.sel_ctx = &replay_dbs;
            replay.sel_fn = aof_replay_select;
            replay.sel_ndbs = ndbs;
            replay.d = &tmp_dbs[replay.db_index];
        }
    }
    arena_init(&ar, 4096);
    resp_buf_init(&out);
    while (off < len) {
        resp_value v;
        size_t i;
        ptrdiff_t used = resp_parse(buf + off, len - off, &v, &ar);
        if (used == 0)
            break; /* an incomplete final command is the tolerated EOF tail */
        if (used < 0 || v.type != RESP_ARRAY || v.is_null || v.count == 0) {
            rc = -1;
            break;
        }
        for (i = 0; i < v.count; i++) {
            if ((v.items[i].type != RESP_BULK_STRING &&
                 v.items[i].type != RESP_SIMPLE_STRING) ||
                v.items[i].str == NULL) {
                rc = -1;
                break;
            }
        }
        if (rc != 0)
            break;
        out.len = 0;
        session_execute(&replay, v.items, v.count, &out);
        if (out.len > 0 && out.data[0] == '-') {
            rc = -1;
            break;
        }
        arena_reset(&ar);
        off += (size_t)used;
    }
    if (rc == 0) {
        int i;
        for (i = 0; i < ndbs; i++)
            aof_swap_db_data(target_dbs[i], &tmp_dbs[i]);
        if (s != NULL) {
            s->db_index = replay.db_index;
            s->d = target_dbs[replay.db_index];
        }
    }
    session_release(&replay);
    resp_buf_free(&out);
    arena_destroy(&ar);
    {
        int i;
        for (i = 0; i < ndbs; i++)
            db_destroy(&tmp_dbs[i]);
    }
    free(target_dbs);
    free(tmp_dbs);
    free(buf);
    return rc;
}

int aof_replay(db *d, const char *path)
{
    return aof_replay_impl(NULL, d, path);
}

int aof_replay_session(session *s, const char *path)
{
    return aof_replay_impl(s, NULL, path);
}
