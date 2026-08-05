/* test_multidb.c - SELECT/SWAPDB multi-database semantics. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "core/session.h"
#include "test.h"

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[8];
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

/* minimal two-db server-side stand-in for the selection hook */
typedef struct dbset {
    db dbs[4];
} dbset;

static db *set_select(void *ctx, int idx)
{
    dbset *ds = (dbset *)ctx;
    return &ds->dbs[idx];
}

static dbset *set_new(int n)
{
    int i;
    dbset *ds = (dbset *)malloc(sizeof(dbset));
    for (i = 0; i < 4; i++)
        db_init(&ds->dbs[i]);
    (void)n;
    return ds;
}

static void set_free(dbset *ds)
{
    int i;
    for (i = 0; i < 4; i++)
        db_destroy(&ds->dbs[i]);
    free(ds);
}

static session *set_session(dbset *ds)
{
    session *s = session_create(&ds->dbs[0]);
    s->sel_ctx = ds;
    s->sel_fn = set_select;
    s->sel_ndbs = 4;
    return s;
}

static void test_select_isolation(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 3, "SET", "k", "v0");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n"); /* db1 is empty */
    exec_sess(s, T0, &out, 3, "SET", "k", "v1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "0");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv0\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv1\r\n");

    /* DBSIZE is per selected db */
    exec_sess(s, T0, &out, 1, "DBSIZE");
    EXPECT(out, ":1\r\n");

    /* FLUSHDB only affects the selected db */
    exec_sess(s, T0, &out, 1, "FLUSHDB");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 1, "DBSIZE");
    EXPECT(out, ":0\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "0");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv0\r\n");

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

static void test_select_out_of_range(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 2, "SELECT", "4");
    EXPECT(out, "-ERR DB index is out of range\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "-1");
    EXPECT(out, "-ERR DB index is out of range\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "abc");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    /* a stack session (no hook) only has db 0 */
    {
        db d;
        resp_buf out2;
        resp_value argv[2];
        db_init(&d);
        resp_buf_init(&out2);
        memset(argv, 0, sizeof(argv));
        argv[0].type = RESP_BULK_STRING;
        argv[0].str = "SELECT";
        argv[0].len = 6;
        argv[1].type = RESP_BULK_STRING;
        argv[1].str = "1";
        argv[1].len = 1;
        command_execute(&d, argv, 2, &out2);
        EXPECT(out2, "-ERR DB index is out of range\r\n");
        resp_buf_free(&out2);
        db_destroy(&d);
    }

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

static void test_swapdb(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 3, "SET", "a", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 3, "SET", "b", "2");
    EXPECT(out, "+OK\r\n");

    exec_sess(s, T0, &out, 3, "SWAPDB", "0", "1");
    EXPECT(out, "+OK\r\n");

    exec_sess(s, T0, &out, 2, "GET", "a"); /* now in db1 */
    EXPECT(out, "$1\r\n1\r\n");
    exec_sess(s, T0, &out, 2, "GET", "b");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "0");
    exec_sess(s, T0, &out, 2, "GET", "b"); /* now in db0 */
    EXPECT(out, "$1\r\n2\r\n");
    exec_sess(s, T0, &out, 2, "GET", "a");
    EXPECT(out, "$-1\r\n");

    exec_sess(s, T0, &out, 3, "SWAPDB", "0", "9");
    EXPECT(out, "-ERR DB index is out of range\r\n");

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

static void test_swapdb_invalidates_watch(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    session *watcher = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(watcher, T0, &out, 2, "WATCH", "k");
    EXPECT(out, "+OK\r\n");
    /* SWAPDB replaces db contents: the watch must trip */
    exec_sess(s, T0, &out, 3, "SWAPDB", "0", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(watcher, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(watcher, T0, &out, 2, "GET", "k");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(watcher, T0, &out, 1, "EXEC");
    EXPECT(out, "*-1\r\n");

    session_free(watcher);
    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

int main(void)
{
    DD_RUN(test_select_isolation);
    DD_RUN(test_select_out_of_range);
    DD_RUN(test_swapdb);
    DD_RUN(test_swapdb_invalidates_watch);
    return DD_TEST_SUMMARY();
}
