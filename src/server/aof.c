/* aof.c - append-only file persistence; see aof.h. */
#include "server/aof.h"

#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "resp/resp_parser.h"

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
    return a;
}

void aof_log_cmd(aof *a, const resp_value *argv, size_t argc)
{
    size_t i;
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

int aof_flush(aof *a)
{
    if (a->pending.len > 0) {
        if (pal_file_write(a->f, a->pending.data, a->pending.len) < 0)
            return -1;
        a->pending.len = 0;
    }
    return pal_file_flush(a->f);
}

void aof_close(aof *a)
{
    if (a == NULL)
        return;
    aof_flush(a);
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

    arena_init(&ar, 4096);
    resp_buf_init(&out);
    while (off < len) {
        resp_value v;
        ptrdiff_t used = resp_parse(buf + off, len - off, &v, &ar);
        if (used <= 0)
            break; /* truncated tail (0) or corrupt bytes (-1): stop here */
        if (v.type == RESP_ARRAY && !v.is_null && v.count > 0) {
            out.len = 0;
            if (s != NULL)
                session_execute(s, v.items, v.count, &out);
            else
                command_execute(d, v.items, v.count, &out);
        }
        arena_reset(&ar);
        off += (size_t)used;
    }
    resp_buf_free(&out);
    arena_destroy(&ar);
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
