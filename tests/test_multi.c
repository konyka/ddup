/* test_multi.c - MULTI/EXEC/DISCARD/WATCH/UNWATCH with two sessions. */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "core/session.h"
#include "test.h"

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

static void test_multi_exec_basic(void)
{
    db d;
    session *a, *b;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    a = session_create(&d);
    b = session_create(&d);

    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "-ERR EXEC without MULTI\r\n");
    exec_sess(a, T0, &out, 1, "DISCARD");
    EXPECT(out, "-ERR DISCARD without MULTI\r\n");

    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "-ERR MULTI calls can not be nested\r\n");

    /* queued commands are not applied until EXEC */
    exec_sess(a, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 2, "GET", "k");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(b, T0, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n");

    /* EXEC replays each command's reply as an array element */
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*2\r\n+OK\r\n$1\r\nv\r\n");
    exec_sess(b, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");

    /* state fully cleared */
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "-ERR EXEC without MULTI\r\n");

    session_free(b);
    session_free(a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_multi_discard(void)
{
    db d;
    session *a;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    a = session_create(&d);

    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "DISCARD");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "-ERR EXEC without MULTI\r\n");

    session_free(a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_multi_execabort(void)
{
    db d;
    session *a;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    a = session_create(&d);

    /* unknown command at queue time */
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 1, "BOGUS");
    EXPECT(out, "-ERR unknown command 'BOGUS'\r\n");
    exec_sess(a, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out,
           "-EXECABORT Transaction discarded because of previous errors.\r\n");
    /* queue discarded, state cleared */
    exec_sess(a, T0, &out, 2, "GET", "k");
    EXPECT(out, "$-1\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "-ERR EXEC without MULTI\r\n");

    /* wrong arg count at queue time */
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "SET", "k");
    EXPECT(out, "-ERR wrong number of arguments for 'set' command\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out,
           "-EXECABORT Transaction discarded because of previous errors.\r\n");
    exec_sess(a, T0, &out, 1, "PING");
    EXPECT(out, "+PONG\r\n");

    session_free(a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_watch_dirty_by_other_session(void)
{
    db d;
    session *a, *b;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    a = session_create(&d);
    b = session_create(&d);

    exec_sess(a, T0, &out, 3, "SET", "w", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "WATCH", "w");
    EXPECT(out, "+OK\r\n");
    /* another session bumps the key version */
    exec_sess(b, T0, &out, 3, "SET", "w", "2");
    EXPECT(out, "+OK\r\n");

    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 3, "SET", "other", "x");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*-1\r\n"); /* dirty: null array, nothing applied */
    exec_sess(b, T0, &out, 2, "GET", "other");
    EXPECT(out, "$-1\r\n");
    /* watches cleared by EXEC */
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 3, "SET", "other", "x");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*1\r\n+OK\r\n");

    session_free(b);
    session_free(a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_watch_variants(void)
{
    db d;
    session *a, *b;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    a = session_create(&d);
    b = session_create(&d);

    /* WATCH inside MULTI is rejected */
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "WATCH", "k");
    EXPECT(out, "-ERR WATCH inside MULTI is not allowed\r\n");
    exec_sess(a, T0, &out, 1, "DISCARD");
    EXPECT(out, "+OK\r\n");

    /* clean watch: EXEC succeeds */
    exec_sess(a, T0, &out, 3, "SET", "k", "1");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "WATCH", "k");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "GET", "k");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*1\r\n$1\r\n1\r\n");

    /* UNWATCH resets: modification after UNWATCH does not abort */
    exec_sess(a, T0, &out, 2, "WATCH", "k");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 1, "UNWATCH");
    EXPECT(out, "+OK\r\n");
    exec_sess(b, T0, &out, 3, "SET", "k", "2");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "GET", "k");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*1\r\n$1\r\n2\r\n");

    /* modifying the watched key inside the transaction itself is fine
     * (the dirty check runs before queued commands) */
    exec_sess(a, T0, &out, 2, "WATCH", "k");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 3, "SET", "k", "3");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*1\r\n+OK\r\n");
    exec_sess(b, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\n3\r\n");

    /* lazy expiry of a watched key dirties it */
    exec_sess(a, T0, &out, 5, "SET", "e", "1", "EX", "10");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 2, "WATCH", "e");
    EXPECT(out, "+OK\r\n");
    exec_sess(b, T0 + 10000, &out, 2, "GET", "e"); /* lazy-expires e */
    EXPECT(out, "$-1\r\n");
    exec_sess(a, T0 + 10000, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0 + 10000, &out, 1, "PING");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0 + 10000, &out, 1, "EXEC");
    EXPECT(out, "*-1\r\n");

    session_free(b);
    session_free(a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_multi_with_objects(void)
{
    db d;
    session *a, *b;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    a = session_create(&d);
    b = session_create(&d);

    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 4, "HSET", "h", "f", "v");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 4, "ZADD", "z", "1.5", "m");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 3, "LPUSH", "l", "x");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*3\r\n:1\r\n:1\r\n:1\r\n");

    exec_sess(b, T0, &out, 3, "HGET", "h", "f");
    EXPECT(out, "$1\r\nv\r\n");
    exec_sess(b, T0, &out, 3, "ZSCORE", "z", "m");
    EXPECT(out, "$3\r\n1.5\r\n");

    /* object mutations bump key versions (WATCH dirty via HSET) */
    exec_sess(a, T0, &out, 2, "WATCH", "h");
    EXPECT(out, "+OK\r\n");
    exec_sess(b, T0, &out, 4, "HSET", "h", "f2", "v2");
    EXPECT(out, ":1\r\n");
    exec_sess(a, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(a, T0, &out, 1, "PING");
    EXPECT(out, "+QUEUED\r\n");
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "*-1\r\n");

    session_free(b);
    session_free(a);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_dirty_watch_reserve_failure_cleans_transaction(void)
{
    db d;
    session *a, *b;
    resp_buf out;
    resp_value exec_arg;
    char *data;
    size_t cap;

    db_init(&d);
    resp_buf_init(&out);
    a = session_create(&d);
    b = session_create(&d);

    exec_sess(a, T0, &out, 2, "WATCH", "k");
    exec_sess(b, T0, &out, 3, "SET", "k", "v");
    exec_sess(a, T0, &out, 1, "MULTI");
    exec_sess(a, T0, &out, 1, "PING");

    memset(&exec_arg, 0, sizeof(exec_arg));
    exec_arg.type = RESP_BULK_STRING;
    exec_arg.str = "EXEC";
    exec_arg.len = 4;
    data = out.data;
    cap = out.cap;
    out.len = SIZE_MAX;
    session_execute_at(a, &exec_arg, 1, &out, T0);
    DD_CHECK(out.len == SIZE_MAX);

    out.data = data;
    out.len = 0;
    out.cap = cap;
    exec_sess(a, T0, &out, 1, "EXEC");
    EXPECT(out, "-ERR EXEC without MULTI\r\n");

    session_free(b);
    session_free(a);
    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_multi_exec_basic);
    DD_RUN(test_multi_discard);
    DD_RUN(test_multi_execabort);
    DD_RUN(test_watch_dirty_by_other_session);
    DD_RUN(test_watch_variants);
    DD_RUN(test_multi_with_objects);
    DD_RUN(test_dirty_watch_reserve_failure_cleans_transaction);
    return DD_TEST_SUMMARY();
}
