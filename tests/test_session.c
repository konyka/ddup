/* test_session.c - session context executes commands like the db wrapper. */
#include <stdarg.h>
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

static void test_queue_allocation_size_overflow(void)
{
    size_t bytes = 123;
    size_t cap = 123;
    DD_CHECK(session_test_queue_bytes(SIZE_MAX, &bytes) == -1);
    DD_CHECK_EQ_INT(123, (long long)bytes);
    DD_CHECK(session_test_queue_growth(SIZE_MAX, &cap) == -1);
    DD_CHECK_EQ_INT(123, (long long)cap);
    DD_CHECK(session_test_queue_growth(0, &cap) == 0);
    DD_CHECK_EQ_INT(8, (long long)cap);
}

static void test_session_basic(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s != NULL);

    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");
    exec_sess(s, T0, &out, 1, "PING");
    EXPECT(out, "+PONG\r\n");

    /* two sessions share one db */
    {
        session *s2 = session_create(&d);
        DD_CHECK(s2 != NULL);
        exec_sess(s2, T0, &out, 2, "GET", "k");
        EXPECT(out, "$1\r\nv\r\n");
        session_free(s2);
    }

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_client_no_touch_preserves_lru(void)
{
    db d;
    session *s;
    resp_buf out;
    uint32_t before, after;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);
    DD_CHECK(s != NULL);
    if (s == NULL)
        return;

    exec_sess(s, 1000000ULL, &out, 3, "SET", "k", "v");
    before = rh_meta_of(&d.table, "k", 1);
    exec_sess(s, 4000000ULL, &out, 3, "CLIENT", "NO-TOUCH", "ON");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, 9000000ULL, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");
    after = rh_meta_of(&d.table, "k", 1);
    DD_CHECK_EQ_INT((long long)before, (long long)after);

    exec_sess(s, 12000000ULL, &out, 3, "CLIENT", "NO-TOUCH", "OFF");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, 16000000ULL, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");
    DD_CHECK(rh_meta_of(&d.table, "k", 1) != after);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_auth_flow(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    s->requirepass = "s3cret";
    s->authed = 0;

    /* everything but AUTH/QUIT is rejected while unauthenticated */
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "-NOAUTH Authentication required.\r\n");
    exec_sess(s, T0, &out, 1, "PING");
    EXPECT(out, "-NOAUTH Authentication required.\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "-NOAUTH Authentication required.\r\n");

    /* wrong password */
    exec_sess(s, T0, &out, 2, "AUTH", "nope");
    EXPECT(out,
           "-WRONGPASS invalid username-password pair or user is "
           "disabled.\r\n");

    /* correct password unlocks the session */
    exec_sess(s, T0, &out, 2, "AUTH", "s3cret");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_auth_username_form(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    s->requirepass = "pw";
    s->authed = 0;

    /* AUTH <user> <pass> with the default user works */
    exec_sess(s, T0, &out, 3, "AUTH", "default", "pw");
    EXPECT(out, "+OK\r\n");

    /* a non-default user is rejected */
    s->authed = 0;
    exec_sess(s, T0, &out, 3, "AUTH", "admin", "pw");
    EXPECT(out,
           "-WRONGPASS invalid username-password pair or user is "
           "disabled.\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_auth_without_password_configured(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s->authed == 1); /* no password: sessions start authenticated */

    exec_sess(s, T0, &out, 2, "AUTH", "x");
    EXPECT(out, "-ERR Client sent AUTH, but no password is set\r\n");
    exec_sess(s, T0, &out, 1, "PING");
    EXPECT(out, "+PONG\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static const char *g_slaveof_host;
static int g_slaveof_port;
static int g_slaveof_calls;

static int slaveof_hook(void *ctx, const char *host, uint16_t port)
{
    (void)ctx;
    g_slaveof_calls++;
    g_slaveof_host = host;
    g_slaveof_port = (int)port;
    return 0;
}

static void test_slaveof_alias(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s != NULL);

    /* Without a hook the alias reports the same context error. */
    exec_sess(s, T0, &out, 3, "SLAVEOF", "NO", "ONE");
    EXPECT(out, "-ERR replicaof not supported in this context\r\n");

    s->replicaof_hook = slaveof_hook;
    s->replicaof_ctx = s;
    g_slaveof_calls = 0;
    g_slaveof_host = NULL;
    g_slaveof_port = -1;

    exec_sess(s, T0, &out, 3, "SLAVEOF", "NO", "ONE");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(1, g_slaveof_calls);
    DD_CHECK(g_slaveof_host == NULL);
    DD_CHECK_EQ_INT(0, g_slaveof_port);

    exec_sess(s, T0, &out, 3, "SLAVEOF", "127.0.0.1", "6379");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(2, g_slaveof_calls);
    DD_CHECK(g_slaveof_host != NULL);
    DD_CHECK_STR("127.0.0.1", g_slaveof_host);
    DD_CHECK_EQ_INT(6379, g_slaveof_port);

    exec_sess(s, T0, &out, 3, "SLAVEOF", "127.0.0.1", "bad");
    EXPECT(out,
           "-ERR value is not an integer or out of range\r\n");
    exec_sess(s, T0, &out, 3, "SLAVEOF", "127.0.0.1", "0");
    EXPECT(out,
           "-ERR value is not an integer or out of range\r\n");
    exec_sess(s, T0, &out, 2, "SLAVEOF", "NO");
    EXPECT(out,
           "-ERR wrong number of arguments for 'slaveof' command\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_role(void)
{
    db d;
    session *s;
    resp_buf out;
    repl_info ri;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s != NULL);

    /* no replication info: report a fresh master */
    exec_sess(s, T0, &out, 1, "ROLE");
    EXPECT(out, "*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n");

    memset(&ri, 0, sizeof(ri));
    ri.role = SESSION_ROLE_MASTER;
    ri.offset = 1234;
    s->repl = &ri;
    exec_sess(s, T0, &out, 1, "ROLE");
    EXPECT(out, "*3\r\n$6\r\nmaster\r\n:1234\r\n*0\r\n");

    memset(&ri, 0, sizeof(ri));
    ri.role = SESSION_ROLE_REPLICA;
    strcpy(ri.master_host, "127.0.0.1");
    ri.master_port = 6379;
    ri.link_up = 1;
    ri.master_offset = 42;
    exec_sess(s, T0, &out, 1, "ROLE");
    EXPECT(out,
           "*5\r\n$5\r\nslave\r\n$9\r\n127.0.0.1\r\n:6379\r\n"
           "$9\r\nconnected\r\n:42\r\n");

    exec_sess(s, T0, &out, 2, "ROLE", "x");
    EXPECT(out, "-ERR wrong number of arguments for 'role' command\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_reset(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s != NULL);

    exec_sess(s, T0, &out, 1, "MULTI");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+QUEUED\r\n");
    DD_CHECK_EQ_INT(1, (long long)s->queue_len);
    exec_sess(s, T0, &out, 1, "RESET");
    EXPECT(out, "+RESET\r\n");
    DD_CHECK_EQ_INT(0, (long long)s->queue_len);
    DD_CHECK(s->in_multi == 0);

    exec_sess(s, T0, &out, 2, "WATCH", "k");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(1, (long long)s->nwatch);
    s->read_only = 1;
    exec_sess(s, T0, &out, 1, "RESET");
    EXPECT(out, "+RESET\r\n");
    DD_CHECK_EQ_INT(0, (long long)s->nwatch);
    DD_CHECK(s->read_only == 0);

    exec_sess(s, T0, &out, 2, "RESET", "x");
    EXPECT(out, "-ERR wrong number of arguments for 'reset' command\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_hello(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s != NULL);

    exec_sess(s, T0, &out, 1, "HELLO");
    EXPECT(out,
           "*14\r\n"
           "$6\r\nserver\r\n$5\r\nredis\r\n"
           "$7\r\nversion\r\n$6\r\n7.2.15\r\n"
           "$5\r\nproto\r\n:2\r\n"
           "$2\r\nid\r\n:0\r\n"
           "$4\r\nmode\r\n$10\r\nstandalone\r\n"
           "$4\r\nrole\r\n$6\r\nmaster\r\n"
           "$7\r\nmodules\r\n*0\r\n");

    exec_sess(s, T0, &out, 2, "HELLO", "3");
    EXPECT(out,
           "%7\r\n"
           "$6\r\nserver\r\n$5\r\nredis\r\n"
           "$7\r\nversion\r\n$6\r\n7.2.15\r\n"
           "$5\r\nproto\r\n:3\r\n"
           "$2\r\nid\r\n:0\r\n"
           "$4\r\nmode\r\n$10\r\nstandalone\r\n"
           "$4\r\nrole\r\n$6\r\nmaster\r\n"
           "$7\r\nmodules\r\n*0\r\n");

    exec_sess(s, T0, &out, 2, "HELLO", "4");
    EXPECT(out, "-NOPROTO unsupported protocol version\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_readonly_readwrite(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    s = session_create(&d);
    DD_CHECK(s != NULL);
    DD_CHECK(s->read_only == 0);

    exec_sess(s, T0, &out, 1, "READONLY");
    EXPECT(out, "+OK\r\n");
    DD_CHECK(s->read_only == 1);

    exec_sess(s, T0, &out, 1, "READWRITE");
    EXPECT(out, "+OK\r\n");
    DD_CHECK(s->read_only == 0);

    exec_sess(s, T0, &out, 2, "READONLY", "x");
    EXPECT(out,
           "-ERR wrong number of arguments for 'readonly' command\r\n");
    exec_sess(s, T0, &out, 2, "READWRITE", "x");
    EXPECT(out,
           "-ERR wrong number of arguments for 'readwrite' command\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_queue_allocation_size_overflow);
    DD_RUN(test_session_basic);
    DD_RUN(test_client_no_touch_preserves_lru);
    DD_RUN(test_auth_flow);
    DD_RUN(test_auth_username_form);
    DD_RUN(test_auth_without_password_configured);
    DD_RUN(test_slaveof_alias);
    DD_RUN(test_role);
    DD_RUN(test_reset);
    DD_RUN(test_hello);
    DD_RUN(test_readonly_readwrite);
    return DD_TEST_SUMMARY();
}
