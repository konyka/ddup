/* test_aof.c - append-only file: logging, replay, corrupt-tail tolerance. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/session.h"
#include "server/aof.h"
#include "server/server.h"
#include "test.h"

#define TMP_AOF "test_aof_tmp.aof"

static void test_log(void *ctx, int db_index, const resp_value *argv,
                     size_t argc, const char *raw, size_t raw_len)
{
    (void)db_index;
    (void)raw;
    (void)raw_len;
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

static void write_file(const char *path, const char *bytes)
{
    FILE *f = fopen(path, "wb");
    size_t len = strlen(bytes);
    DD_CHECK(f != NULL);
    if (f == NULL)
        return;
    DD_CHECK(fwrite(bytes, 1, len, f) == len);
    DD_CHECK(fclose(f) == 0);
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

static void test_aof_api_rejects_null_inputs(void)
{
    DD_CHECK(aof_open(NULL) == NULL);
    DD_CHECK_EQ_INT(-1, aof_copy_delta(NULL, 0, "x"));
    DD_CHECK_EQ_INT(-1, aof_replay(NULL, "x"));
    DD_CHECK_EQ_INT(-1, aof_replay_session(NULL, "x"));
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

static void test_aof_incomplete_tail(void)
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

static void test_aof_malformed_replay_fails(void)
{
    db d;
    db_init(&d);
    write_file(TMP_AOF,
               "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
               "*x\r\n");

    DD_CHECK_EQ_INT(-1, aof_replay(&d, TMP_AOF));

    db_destroy(&d);
    remove(TMP_AOF);
}

static void check_late_corruption_does_not_mutate(int through_session)
{
    db d;
    session *s;
    resp_buf out;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);
    DD_CHECK(s != NULL);
    exec_sess(s, T0, &out, 3, "SET", "seed", "before");
    EXPECT(out, "+OK\r\n");
    write_file(TMP_AOF,
               "*3\r\n$3\r\nSET\r\n$3\r\nnew\r\n$5\r\nvalue\r\n"
               "*2\r\n$3\r\nSET\r\n:1\r\n");

    if (through_session)
        DD_CHECK_EQ_INT(-1, aof_replay_session(s, TMP_AOF));
    else
        DD_CHECK_EQ_INT(-1, aof_replay(&d, TMP_AOF));
    exec_sess(s, T0, &out, 2, "GET", "seed");
    EXPECT(out, "$6\r\nbefore\r\n");
    exec_sess(s, T0, &out, 2, "GET", "new");
    EXPECT(out, "$-1\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
    remove(TMP_AOF);
}

static void test_aof_late_corruption_does_not_mutate_db(void)
{
    check_late_corruption_does_not_mutate(0);
}

static void test_aof_session_late_corruption_does_not_mutate_db(void)
{
    check_late_corruption_does_not_mutate(1);
}

static void test_aof_invalid_command_frames_fail(void)
{
    static const char *frames[] = {
        "+PING\r\n",
        "*0\r\n",
        "*2\r\n$3\r\nSET\r\n:1\r\n",
        "*1\r\n$7\r\nUNKNOWN\r\n",
    };
    size_t i;

    for (i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
        db d;
        db_init(&d);
        write_file(TMP_AOF, frames[i]);
        DD_CHECK_EQ_INT(-1, aof_replay(&d, TMP_AOF));
        db_destroy(&d);
        remove(TMP_AOF);
    }
}

static void test_server_rejects_invalid_aof(void)
{
    server *s;
    write_file(TMP_AOF, "*x\r\n");
    s = server_create("127.0.0.1", 0);
    DD_CHECK(s != NULL);
    if (s != NULL) {
        DD_CHECK_EQ_INT(-1, server_enable_aof(s, TMP_AOF));
        server_destroy(s);
    }
    remove(TMP_AOF);
}

static size_t write_limit;
static int write_calls;
static int write_fail_call;
static int write_zero_call;

static ptrdiff_t limited_write(pal_file *f, const void *buf, size_t len)
{
    write_calls++;
    if (write_calls == write_fail_call)
        return -1;
    if (write_calls == write_zero_call)
        return 0;
    if (len > write_limit)
        len = write_limit;
    return pal_file_write(f, buf, len);
}

static void log_set(aof *a)
{
    resp_value argv[3];
    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING;
    argv[0].str = "SET";
    argv[0].len = 3;
    argv[1].type = RESP_BULK_STRING;
    argv[1].str = "key";
    argv[1].len = 3;
    argv[2].type = RESP_BULK_STRING;
    argv[2].str = "value";
    argv[2].len = 5;
    aof_log_cmd(a, argv, 3);
}

static void test_aof_flush_completes_short_writes(void)
{
    aof *a = aof_open(TMP_AOF);
    char *want;
    char *bytes;
    size_t want_len;
    size_t bytes_len;
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    log_set(a);
    want_len = a->pending.len;
    want = (char *)malloc(want_len);
    DD_CHECK(want != NULL);
    if (want == NULL) {
        aof_close(a);
        remove(TMP_AOF);
        return;
    }
    memcpy(want, a->pending.data, want_len);
    write_limit = 5;
    write_calls = 0;
    write_fail_call = -1;
    write_zero_call = -1;
    aof_test_set_write_fn(a, limited_write);

    DD_CHECK_EQ_INT(0, aof_flush(a));
    DD_CHECK(a->pending.len == 0);
    DD_CHECK(write_calls > 1);
    aof_close(a);

    bytes = read_file(TMP_AOF, &bytes_len);
    DD_CHECK_MEM(want, want_len, bytes, bytes_len);
    free(bytes);
    free(want);
    remove(TMP_AOF);
}

static void test_aof_flush_preserves_suffix_on_error(void)
{
    aof *a = aof_open(TMP_AOF);
    char *want;
    size_t want_len;
    int calls_after_failure;
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    log_set(a);
    want_len = a->pending.len;
    want = (char *)malloc(want_len);
    DD_CHECK(want != NULL);
    if (want == NULL) {
        aof_close(a);
        remove(TMP_AOF);
        return;
    }
    memcpy(want, a->pending.data, want_len);
    write_limit = 7;
    write_calls = 0;
    write_fail_call = 2;
    write_zero_call = -1;
    aof_test_set_write_fn(a, limited_write);

    DD_CHECK_EQ_INT(-1, aof_flush(a));
    DD_CHECK_EQ_INT((long long)(want_len - 7),
                    (long long)a->pending.len);
    DD_CHECK_MEM(want + 7, want_len - 7, a->pending.data, a->pending.len);
    calls_after_failure = write_calls;
    DD_CHECK_EQ_INT(-1, aof_flush(a));
    DD_CHECK_EQ_INT(calls_after_failure, write_calls);

    free(want);
    aof_close(a);
    remove(TMP_AOF);
}

static void test_aof_flush_latches_zero_progress(void)
{
    aof *a = aof_open(TMP_AOF);
    size_t pending_len;
    int calls_after_failure;
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    log_set(a);
    pending_len = a->pending.len;
    write_limit = pending_len;
    write_calls = 0;
    write_fail_call = -1;
    write_zero_call = 1;
    aof_test_set_write_fn(a, limited_write);

    DD_CHECK_EQ_INT(-1, aof_flush(a));
    DD_CHECK_EQ_INT((long long)pending_len, (long long)a->pending.len);
    calls_after_failure = write_calls;
    DD_CHECK_EQ_INT(-1, aof_flush(a));
    DD_CHECK_EQ_INT(calls_after_failure, write_calls);

    aof_close(a);
    remove(TMP_AOF);
}

static void test_aof_flush_preserves_pending_on_flush_failure(void)
{
    aof *a = aof_open(TMP_AOF);
    char *want;
    size_t want_len;

    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    log_set(a);
    want_len = a->pending.len;
    want = (char *)malloc(want_len);
    DD_CHECK(want != NULL);
    if (want == NULL) {
        aof_close(a);
        remove(TMP_AOF);
        return;
    }
    memcpy(want, a->pending.data, want_len);
    pal_file_test_fail_next_flush();

    DD_CHECK_EQ_INT(-1, aof_flush(a));
    DD_CHECK_EQ_INT((long long)want_len, (long long)a->pending.len);
    DD_CHECK_MEM(want, want_len, a->pending.data, a->pending.len);
    DD_CHECK_EQ_INT(-1, aof_flush(a));

    free(want);
    aof_close(a);
    remove(TMP_AOF);
}

/* ---- appendfsync: durability policy over pal_file_sync --------------- */

static int sync_calls;
static int sync_fail_call;
static uint64_t fake_now;

static int counting_sync(pal_file *f)
{
    sync_calls++;
    if (sync_calls == sync_fail_call)
        return -1;
    return pal_file_sync(f);
}

static uint64_t now_fake(void)
{
    return fake_now;
}

static void test_pal_file_sync_roundtrip(void)
{
    pal_file *f = pal_file_open_write(TMP_AOF);
    char buf[8];
    DD_CHECK(f != NULL);
    DD_CHECK_EQ_INT(4, pal_file_write(f, "sync", 4));
    DD_CHECK_EQ_INT(0, pal_file_flush(f));
    DD_CHECK_EQ_INT(0, pal_file_sync(f));
    DD_CHECK_EQ_INT(0, pal_file_close(f));
    f = pal_file_open_read(TMP_AOF);
    DD_CHECK(f != NULL);
    if (f != NULL) {
        DD_CHECK_EQ_INT(4, pal_file_read(f, buf, sizeof(buf)));
        DD_CHECK_MEM("sync", 4, buf, 4);
        DD_CHECK_EQ_INT(0, pal_file_close(f));
    }
    remove(TMP_AOF);
}

static void test_aof_fsync_always_syncs_every_flush(void)
{
    aof *a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    sync_calls = 0;
    sync_fail_call = -1;
    aof_test_set_sync_fn(a, counting_sync);
    aof_set_fsync_mode(a, AOF_FSYNC_ALWAYS);
    log_set(a);
    DD_CHECK_EQ_INT(0, aof_flush(a));
    DD_CHECK_EQ_INT(1, sync_calls);
    DD_CHECK_EQ_INT(0, aof_flush(a)); /* nothing pending: no extra sync */
    DD_CHECK_EQ_INT(1, sync_calls);
    log_set(a);
    DD_CHECK_EQ_INT(0, aof_flush(a));
    DD_CHECK_EQ_INT(2, sync_calls);
    aof_close(a);
    remove(TMP_AOF);
}

static void test_aof_fsync_everysec_throttles(void)
{
    aof *a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    sync_calls = 0;
    sync_fail_call = -1;
    fake_now = 100000;
    aof_test_set_sync_fn(a, counting_sync);
    aof_test_set_now_fn(a, now_fake);
    aof_set_fsync_mode(a, AOF_FSYNC_EVERYSEC);
    log_set(a);
    DD_CHECK_EQ_INT(0, aof_flush(a)); /* first flush syncs */
    DD_CHECK_EQ_INT(1, sync_calls);
    fake_now += 999;
    log_set(a);
    DD_CHECK_EQ_INT(0, aof_flush(a)); /* inside the 1s window: skipped */
    DD_CHECK_EQ_INT(1, sync_calls);
    fake_now += 1;
    log_set(a);
    DD_CHECK_EQ_INT(0, aof_flush(a)); /* a full second later: syncs */
    DD_CHECK_EQ_INT(2, sync_calls);
    aof_close(a);
    remove(TMP_AOF);
}

static void test_aof_fsync_no_never_syncs(void)
{
    aof *a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    sync_calls = 0;
    sync_fail_call = -1;
    aof_test_set_sync_fn(a, counting_sync);
    aof_set_fsync_mode(a, AOF_FSYNC_NO);
    log_set(a);
    DD_CHECK_EQ_INT(0, aof_flush(a));
    DD_CHECK_EQ_INT(0, sync_calls);
    aof_close(a); /* even the closing flush must not sync in mode no */
    DD_CHECK_EQ_INT(0, sync_calls);
    remove(TMP_AOF);
}

static void test_aof_close_final_sync_everysec(void)
{
    aof *a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    sync_calls = 0;
    sync_fail_call = -1;
    fake_now = 100000;
    aof_test_set_sync_fn(a, counting_sync);
    aof_test_set_now_fn(a, now_fake);
    /* default mode is everysec (Redis default) */
    log_set(a);
    DD_CHECK_EQ_INT(0, aof_flush(a));
    DD_CHECK_EQ_INT(1, sync_calls);
    /* close inside the throttle window still syncs once at the end */
    aof_close(a);
    DD_CHECK_EQ_INT(2, sync_calls);
    remove(TMP_AOF);
}

static void test_aof_sync_failure_latches_fail_closed(void)
{
    aof *a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    if (a == NULL)
        return;
    sync_calls = 0;
    sync_fail_call = 1;
    aof_test_set_sync_fn(a, counting_sync);
    aof_set_fsync_mode(a, AOF_FSYNC_ALWAYS);
    log_set(a);
    DD_CHECK_EQ_INT(-1, aof_flush(a)); /* sync failed: latched */
    DD_CHECK_EQ_INT(1, sync_calls);
    DD_CHECK_EQ_INT(-1, aof_flush(a)); /* fail-closed: no retry */
    DD_CHECK_EQ_INT(1, sync_calls);
    aof_close(a);
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

static void test_aof_session_late_corruption_does_not_mutate_dbs(void)
{
    dbset *ds;
    session *s;
    resp_buf out;
    int i;

    ds = (dbset *)calloc(1, sizeof(*ds));
    DD_CHECK(ds != NULL);
    for (i = 0; i < 4; i++)
        db_init(&ds->dbs[i]);
    resp_buf_init(&out);
    s = session_create(&ds->dbs[0]);
    DD_CHECK(s != NULL);
    s->sel_ctx = ds;
    s->sel_fn = mds_select;
    s->sel_ndbs = 4;
    exec_sess(s, T0, &out, 3, "SET", "zero", "before0");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 3, "SET", "one", "before1");
    exec_sess(s, T0, &out, 2, "SELECT", "0");
    write_file(TMP_AOF,
               "*3\r\n$3\r\nSET\r\n$4\r\nnew0\r\n$2\r\nv0\r\n"
               "*2\r\n$6\r\nSELECT\r\n$1\r\n1\r\n"
               "*3\r\n$3\r\nSET\r\n$4\r\nnew1\r\n$2\r\nv1\r\n"
               "*2\r\n$3\r\nSET\r\n:1\r\n");

    DD_CHECK_EQ_INT(-1, aof_replay_session(s, TMP_AOF));
    DD_CHECK_EQ_INT(0, s->db_index);
    exec_sess(s, T0, &out, 2, "GET", "zero");
    EXPECT(out, "$7\r\nbefore0\r\n");
    exec_sess(s, T0, &out, 2, "GET", "new0");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 2, "GET", "one");
    EXPECT(out, "$7\r\nbefore1\r\n");
    exec_sess(s, T0, &out, 2, "GET", "new1");
    EXPECT(out, "$-1\r\n");

    session_free(s);
    resp_buf_free(&out);
    for (i = 0; i < 4; i++)
        db_destroy(&ds->dbs[i]);
    free(ds);
    remove(TMP_AOF);
}

int main(void)
{
    DD_RUN(test_aof_serialization);
    DD_RUN(test_aof_api_rejects_null_inputs);
    DD_RUN(test_aof_replay);
    DD_RUN(test_aof_incomplete_tail);
    DD_RUN(test_aof_malformed_replay_fails);
    DD_RUN(test_aof_late_corruption_does_not_mutate_db);
    DD_RUN(test_aof_session_late_corruption_does_not_mutate_db);
    DD_RUN(test_aof_invalid_command_frames_fail);
    DD_RUN(test_server_rejects_invalid_aof);
    DD_RUN(test_aof_flush_completes_short_writes);
    DD_RUN(test_aof_flush_preserves_suffix_on_error);
    DD_RUN(test_aof_flush_latches_zero_progress);
    DD_RUN(test_aof_flush_preserves_pending_on_flush_failure);
    DD_RUN(test_pal_file_sync_roundtrip);
    DD_RUN(test_aof_fsync_always_syncs_every_flush);
    DD_RUN(test_aof_fsync_everysec_throttles);
    DD_RUN(test_aof_fsync_no_never_syncs);
    DD_RUN(test_aof_close_final_sync_everysec);
    DD_RUN(test_aof_sync_failure_latches_fail_closed);
    DD_RUN(test_aof_flushdb_logged);
    DD_RUN(test_aof_multidb_replay);
    DD_RUN(test_aof_session_late_corruption_does_not_mutate_dbs);
    return DD_TEST_SUMMARY();
}
