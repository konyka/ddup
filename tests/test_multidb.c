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

/* minimal two-db server-side stand-in for the selection hook (heap: db
 * structs are huge due to the embedded cluster tables) */
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
    dbset *ds = (dbset *)calloc(1, sizeof(dbset));
    DD_CHECK(ds != NULL);
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

static void test_flushall_all_dbs(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 3, "SET", "k0", "v0");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k1", "v1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "2");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k2", "v2");
    EXPECT(out, "+OK\r\n");

    exec_sess(s, T0, &out, 1, "FLUSHALL");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 1, "DBSIZE");
    EXPECT(out, ":0\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "0");
    exec_sess(s, T0, &out, 2, "GET", "k0");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 2, "GET", "k1");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "2");
    exec_sess(s, T0, &out, 2, "GET", "k2");
    EXPECT(out, "$-1\r\n");

    /* FLUSHALL also resets stats across dbs; the FLUSHALL call itself is
     * accounted for on the selected db after dispatch. */
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 1, "INFO");
    {
        resp_buf_reserve(&out, 1);
        out.data[out.len] = '\0';
        DD_CHECK(strstr(out.data, "cmdstat_set:calls=") == NULL);
    }

    exec_sess(s, T0, &out, 2, "FLUSHALL", "x");
    EXPECT(out,
           "-ERR wrong number of arguments for 'flushall' command\r\n");

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

static void test_info_keyspace_sections(void)
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
    exec_sess(s, T0, &out, 3, "EXPIRE", "b", "100");
    EXPECT(out, ":1\r\n");

    exec_sess(s, T0, &out, 1, "INFO");
    {
        resp_buf_reserve(&out, 1);
        out.data[out.len] = '\0';
        DD_CHECK(strstr(out.data, "dbsize:1\r\n") != NULL);
        DD_CHECK(strstr(out.data, "db0:keys=1,expires=0,avg_ttl=0\r\n") !=
                 NULL);
        DD_CHECK(strstr(out.data, "db1:keys=1,expires=1,avg_ttl=0\r\n") !=
                 NULL);
        DD_CHECK(strstr(out.data, "db2:keys=") == NULL);
    }

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

static void test_expiry_isolated_per_db(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 3, "SET", "k", "v0");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 3, "SET", "k", "v1");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "EXPIRE", "k", "100");
    EXPECT(out, ":1\r\n");

    /* db1's k expires; db0's k is unaffected */
    exec_sess(s, T0 + 100 * 1000 + 1, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0 + 100 * 1000 + 1, &out, 2, "SELECT", "0");
    exec_sess(s, T0 + 100 * 1000 + 1, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv0\r\n");

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

static void test_commandstats_in_info(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 1, "PING");
    EXPECT(out, "+PONG\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k", "v2");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$2\r\nv2\r\n");

    exec_sess(s, T0, &out, 1, "INFO");
    {
        resp_buf_reserve(&out, 1);
        out.data[out.len] = '\0';
        DD_CHECK(strstr(out.data, "# Commandstats\r\n") != NULL);
        DD_CHECK(strstr(out.data, "cmdstat_set:calls=2,") != NULL);
        DD_CHECK(strstr(out.data, "cmdstat_get:calls=1,") != NULL);
        DD_CHECK(strstr(out.data, "cmdstat_ping:calls=1,") != NULL);
        DD_CHECK(strstr(out.data, "usec_per_call=") != NULL);
    }

    /* FLUSHDB resets the counters */
    exec_sess(s, T0, &out, 1, "FLUSHDB");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 1, "INFO");
    {
        resp_buf_reserve(&out, 1);
        out.data[out.len] = '\0';
        DD_CHECK(strstr(out.data, "cmdstat_set:calls=2,") == NULL);
    }

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

/* INFO __STATS__ (internal, mt aggregation transport): machine-readable
 * per-worker snapshot - one "k:v" line per metric, db/cmd lines carry their
 * index/id so the home worker can sum parts without name lookups. */
static void test_info_machine_format(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 3, "SET", "x", "y");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "0");

    exec_sess(s, T0, &out, 2, "INFO", "__STATS__");
    {
        resp_buf_reserve(&out, 1);
        out.data[out.len] = '\0';
        DD_CHECK(out.data[0] == '$');
        DD_CHECK(strstr(out.data, "\r\nused_memory:") != NULL);
        DD_CHECK(strstr(out.data, "\r\nexpired_keys:") != NULL);
        DD_CHECK(strstr(out.data, "\r\nevicted_keys:") != NULL);
        DD_CHECK(strstr(out.data, "\r\ndbsize:1\r\n") != NULL);
        DD_CHECK(strstr(out.data, "\r\nndbs:4\r\n") != NULL);
        /* db:<idx>:<keys>:<expires> for every non-empty db */
        DD_CHECK(strstr(out.data, "\r\ndb:0:1:0\r\n") != NULL);
        DD_CHECK(strstr(out.data, "\r\ndb:1:1:0\r\n") != NULL);
        /* c:<cmd_id>:<calls>:<usec> lines for called commands */
        DD_CHECK(strstr(out.data, "\r\nc:") != NULL);
        /* no human sections in the machine format */
        DD_CHECK(strstr(out.data, "# Memory") == NULL);
        DD_CHECK(strstr(out.data, "cmdstat_") == NULL);
    }

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

static void test_copy_crossdb(void)
{
    dbset *ds = set_new(4);
    session *s = set_session(ds);
    resp_buf out;
    uint64_t before;
    resp_buf_init(&out);

    exec_sess(s, T0, &out, 3, "SET", "k", "v0");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "PEXPIRE", "k", "10000");

    /* copy into db 2: value + absolute expiry, source untouched; the
     * session db's dirty bumps so the AOF/propagation hook fires */
    before = s->d->dirty;
    exec_sess(s, T0 + 4000, &out, 5, "COPY", "k", "k2", "DB", "2");
    EXPECT(out, ":1\r\n");
    DD_CHECK(s->d->dirty > before);

    exec_sess(s, T0 + 4000, &out, 2, "SELECT", "2");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0 + 4000, &out, 2, "GET", "k2");
    EXPECT(out, "$2\r\nv0\r\n");
    exec_sess(s, T0 + 4000, &out, 2, "PTTL", "k2");
    EXPECT(out, ":6000\r\n");

    /* target exists in the target db: REPLACE is required */
    exec_sess(s, T0 + 4000, &out, 3, "SET", "k2", "other");
    exec_sess(s, T0 + 4000, &out, 2, "SELECT", "0");
    exec_sess(s, T0 + 4000, &out, 5, "COPY", "k", "k2", "DB", "2");
    EXPECT(out, "-ERR Target key already exists\r\n");
    exec_sess(s, T0 + 4000, &out, 6, "COPY", "k", "k2", "DB", "2", "REPLACE");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0 + 4000, &out, 2, "SELECT", "2");
    exec_sess(s, T0 + 4000, &out, 2, "GET", "k2");
    EXPECT(out, "$2\r\nv0\r\n");

    /* db indexes beyond the configured range are rejected */
    exec_sess(s, T0, &out, 2, "SELECT", "0");
    exec_sess(s, T0, &out, 5, "COPY", "k", "k9", "DB", "4");
    EXPECT(out, "-ERR DB index is out of range\r\n");
    exec_sess(s, T0, &out, 5, "COPY", "k", "k9", "DB", "99");
    EXPECT(out, "-ERR DB index is out of range\r\n");

    /* composite values are deep-copied across dbs */
    exec_sess(s, T0, &out, 4, "RPUSH", "l", "a", "b");
    EXPECT(out, ":2\r\n");
    exec_sess(s, T0, &out, 5, "COPY", "l", "l", "DB", "1");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0, &out, 3, "RPUSH", "l", "c");
    EXPECT(out, ":3\r\n");
    exec_sess(s, T0, &out, 2, "SELECT", "1");
    exec_sess(s, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    session_free(s);
    resp_buf_free(&out);
    set_free(ds);
}

int main(void)
{
    DD_RUN(test_select_isolation);
    DD_RUN(test_flushall_all_dbs);
    DD_RUN(test_select_out_of_range);
    DD_RUN(test_swapdb);
    DD_RUN(test_swapdb_invalidates_watch);
    DD_RUN(test_info_keyspace_sections);
    DD_RUN(test_expiry_isolated_per_db);
    DD_RUN(test_commandstats_in_info);
    DD_RUN(test_info_machine_format);
    DD_RUN(test_copy_crossdb);
    return DD_TEST_SUMMARY();
}
