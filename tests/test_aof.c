/* test_aof.c - append-only file: logging, replay, corrupt-tail tolerance. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/session.h"
#include "server/aof.h"
#include "test.h"

#define TMP_AOF "test_aof_tmp.aof"

static void test_log(void *ctx, int db_index, const resp_value *argv,
                     size_t argc)
{
    (void)db_index;
    aof_log_cmd((aof *)ctx, argv, argc);
}

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[10];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *str = va_arg(ap, const char *);
        memset(&argv[i], 0, sizeof(argv[i]));
        argv[i].type = RESP_BULK_STRING;
        argv[i].str = str;
        argv[i].len = strlen(str);
    }
    va_end(ap);
    out->len = 0;
    session_execute_at(s, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

#define T0 1000000ULL

static session *aof_session(db *d, aof *a)
{
    session *s = session_create(d);
    s->aof_ctx = a;
    s->aof_log = test_log;
    return s;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    DD_CHECK(f != NULL);
    DD_CHECK(fseek(f, 0, SEEK_END) == 0);
    n = ftell(f);
    DD_CHECK(n >= 0);
    DD_CHECK(fseek(f, 0, SEEK_SET) == 0);
    buf = (char *)malloc((size_t)n + 1);
    DD_CHECK(buf != NULL);
    DD_CHECK(fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

static void test_aof_serialization(void)
{
    db d;
    aof *a;
    session *s;
    resp_buf out;
    char *bytes;
    size_t blen;
    db_init(&d);
    resp_buf_init(&out);

    a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    s = aof_session(&d, a);

    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k"); /* reads are not logged */
    EXPECT(out, "$1\r\nv\r\n");
    exec_sess(s, T0, &out, 4, "HSET", "h", "f", "v");
    EXPECT(out, ":1\r\n");
    aof_flush(a);
    aof_close(a);

    bytes = read_file(TMP_AOF, &blen);
    {
        const char *want = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
                           "*4\r\n$4\r\nHSET\r\n$1\r\nh\r\n$1\r\nf\r\n$1\r\nv\r\n";
        DD_CHECK_MEM(want, strlen(want), bytes, blen);
    }
    free(bytes);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
    remove(TMP_AOF);
}

static void test_aof_replay(void)
{
    db d, d2;
    aof *a;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    s = aof_session(&d, a);
    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    exec_sess(s, T0, &out, 4, "HSET", "h", "f1", "v1");
    exec_sess(s, T0, &out, 4, "ZADD", "z", "2.5", "m");
    exec_sess(s, T0, &out, 4, "LPUSH", "l", "a", "b");
    exec_sess(s, T0, &out, 3, "SADD", "st", "x");
    exec_sess(s, T0, &out, 2, "INCR", "n");
    aof_flush(a);
    aof_close(a);
    session_free(s);
    db_destroy(&d);

    /* replay into a fresh db */
    db_init(&d2);
    DD_CHECK_EQ_INT(0, aof_replay(&d2, TMP_AOF));
    {
        session *r = session_create(&d2);
        exec_sess(r, T0, &out, 2, "GET", "k");
        EXPECT(out, "$1\r\nv\r\n");
        exec_sess(r, T0, &out, 3, "HGET", "h", "f1");
        EXPECT(out, "$2\r\nv1\r\n");
        exec_sess(r, T0, &out, 3, "ZSCORE", "z", "m");
        EXPECT(out, "$3\r\n2.5\r\n");
        exec_sess(r, T0, &out, 4, "LRANGE", "l", "0", "-1");
        EXPECT(out, "*2\r\n$1\r\nb\r\n$1\r\na\r\n");
        exec_sess(r, T0, &out, 3, "SISMEMBER", "st", "x");
        EXPECT(out, ":1\r\n");
        exec_sess(r, T0, &out, 2, "GET", "n");
        EXPECT(out, "$1\r\n1\r\n");
        session_free(r);
    }
    db_destroy(&d2);
    resp_buf_free(&out);
    remove(TMP_AOF);
}

static void test_aof_corrupt_tail(void)
{
    db d;
    resp_buf out;
    FILE *f = fopen(TMP_AOF, "wb");
    db_init(&d);
    resp_buf_init(&out);

    /* one full command + a truncated second one */
    DD_CHECK(f != NULL);
    fputs("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n*3\r\n$3\r\nGE", f);
    fclose(f);

    DD_CHECK_EQ_INT(0, aof_replay(&d, TMP_AOF));
    {
        session *r = session_create(&d);
        exec_sess(r, T0, &out, 2, "GET", "k");
        EXPECT(out, "$1\r\nv\r\n");
        session_free(r);
    }
    resp_buf_free(&out);
    db_destroy(&d);
    remove(TMP_AOF);
}

static void test_aof_flushdb_logged(void)
{
    db d, d2;
    aof *a;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    s = aof_session(&d, a);
    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    exec_sess(s, T0, &out, 1, "FLUSHDB");
    aof_flush(a);
    aof_close(a);
    session_free(s);
    db_destroy(&d);

    db_init(&d2);
    DD_CHECK_EQ_INT(0, aof_replay(&d2, TMP_AOF));
    {
        session *r = session_create(&d2);
        exec_sess(r, T0, &out, 1, "DBSIZE");
        EXPECT(out, ":0\r\n");
        session_free(r);
    }
    db_destroy(&d2);
    resp_buf_free(&out);
    remove(TMP_AOF);
}

/* multi-db replay: embedded SELECT switches the target db via the
 * session selection hook */
typedef struct dbset {
    db dbs[4];
} dbset;

static db *mds_select(void *ctx, int idx)
{
    return &((dbset *)ctx)->dbs[idx];
}

static void test_aof_multidb_replay(void)
{
    aof *a;
    resp_buf out;
    resp_value sel[2], cmd3[3];
    dbset *ds;
    session *r;
    int i;

    resp_buf_init(&out);
    ds = (dbset *)calloc(1, sizeof(*ds));
    DD_CHECK(ds != NULL);
    a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);

    memset(sel, 0, sizeof(sel));
    sel[0].type = RESP_BULK_STRING;
    sel[0].str = "SELECT";
    sel[0].len = 6;
    sel[1].type = RESP_BULK_STRING;
    sel[1].str = "1";
    sel[1].len = 1;
    aof_log_cmd(a, sel, 2);

    memset(cmd3, 0, sizeof(cmd3));
    cmd3[0].type = RESP_BULK_STRING;
    cmd3[0].str = "SET";
    cmd3[0].len = 3;
    cmd3[1].type = RESP_BULK_STRING;
    cmd3[1].str = "k";
    cmd3[1].len = 1;
    cmd3[2].type = RESP_BULK_STRING;
    cmd3[2].str = "v1";
    cmd3[2].len = 2;
    aof_log_cmd(a, cmd3, 3);
    aof_flush(a);
    aof_close(a);

    for (i = 0; i < 4; i++)
        db_init(&ds->dbs[i]);
    r = session_create(&ds->dbs[0]);
    r->sel_ctx = ds;
    r->sel_fn = mds_select;
    r->sel_ndbs = 4;

    DD_CHECK_EQ_INT(0, aof_replay_session(r, TMP_AOF));

    exec_sess(r, T0, &out, 2, "SELECT", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv1\r\n");
    exec_sess(r, T0, &out, 2, "SELECT", "0");
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n");

    session_free(r);
    for (i = 0; i < 4; i++)
        db_destroy(&ds->dbs[i]);
    free(ds);
    resp_buf_free(&out);
    remove(TMP_AOF);
}

int main(void)
{
    DD_RUN(test_aof_serialization);
    DD_RUN(test_aof_replay);
    DD_RUN(test_aof_corrupt_tail);
    DD_RUN(test_aof_flushdb_logged);
    DD_RUN(test_aof_multidb_replay);
    return DD_TEST_SUMMARY();
}
