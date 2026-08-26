/* test_eval.c - EVAL / EVALSHA / SCRIPT family (Lua scripting). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/script.h"
#include "core/session.h"
#include "core/sha1.h"
#include "server/aof.h"
#include "test.h"

#define T0 1000000ULL

static void exec_sess(session *s, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[16];
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

/* Extract the first RESP bulk-string payload from a reply buffer. */
static int resp_bulk_payload(resp_buf *out, const char **data, size_t *len)
{
    const char *p;
    size_t n;
    if (out->len < 5 || out->data[0] != '$')
        return -1;
    p = out->data + 1;
    n = 0;
    while ((size_t)(p - out->data) < out->len && *p >= '0' && *p <= '9') {
        if (n > (SIZE_MAX - 9) / 10)
            return -1;
        n = n * 10 + (size_t)(*p - '0');
        p++;
    }
    if ((size_t)(p - out->data) >= out->len || p[0] != '\r' || p[1] != '\n')
        return -1;
    p += 2;
    if (n > (size_t)(out->data + out->len - p))
        return -1;
    *data = p;
    *len = n;
    return 0;
}

static void exec_sess_restore(session *s, uint64_t now, resp_buf *out,
                              const char *payload, size_t payload_len,
                              const char *policy)
{
    resp_value argv[4];
    int argc = policy ? 4 : 3;
    int i;
    memset(argv, 0, sizeof(argv));
    for (i = 0; i < 3; i++) {
        argv[i].type = RESP_BULK_STRING;
    }
    argv[0].str = "FUNCTION";
    argv[0].len = 8;
    argv[1].str = "RESTORE";
    argv[1].len = 7;
    argv[2].str = payload;
    argv[2].len = payload_len;
    if (policy) {
        argv[3].type = RESP_BULK_STRING;
        argv[3].str = policy;
        argv[3].len = strlen(policy);
    }
    out->len = 0;
    session_execute_at(s, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

static void test_return_matrix(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "EVAL", "return 42", "0");
    EXPECT(out, ":42\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return 3.9", "0");
    EXPECT(out, ":3\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return 'hello'", "0");
    EXPECT(out, "$5\r\nhello\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return {1,'two',3}", "0");
    EXPECT(out, "*3\r\n:1\r\n$3\r\ntwo\r\n:3\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return nil", "0");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return true", "0");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return false", "0");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return {ok='fine'}", "0");
    EXPECT(out, "+fine\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return {err='bad thing'}", "0");
    EXPECT(out, "-bad thing\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return {{1,2},{3}}", "0");
    EXPECT(out, "*2\r\n*2\r\n:1\r\n:2\r\n*1\r\n:3\r\n");
    exec_sess(s, T0, &out, 3, "EVAL", "return {1,nil,3}", "0");
    EXPECT(out, "*1\r\n:1\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_redis_call_and_bindings(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 5, "EVAL",
              "redis.call('SET', KEYS[1], ARGV[1]) "
              "return redis.call('GET', KEYS[1])",
              "1", "k", "v");
    EXPECT(out, "$1\r\nv\r\n");
    exec_sess(s, T0, &out, 2, "GET", "k"); /* effect is real */
    EXPECT(out, "$1\r\nv\r\n");

    exec_sess(s, T0, &out, 4, "EVAL",
              "return redis.call('HSET', KEYS[1], 'f1', 'v1', 'f2', 'v2')",
              "1", "h");
    EXPECT(out, ":2\r\n");
    exec_sess(s, T0, &out, 4, "EVAL",
              "return redis.call('HGET', KEYS[1], 'f2')", "1", "h");
    EXPECT(out, "$2\r\nv2\r\n");

    exec_sess(s, T0, &out, 4, "EVAL", "return redis.call('INCR', KEYS[1])",
              "1", "ctr");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0, &out, 4, "EVAL", "return redis.call('INCR', KEYS[1])",
              "1", "ctr");
    EXPECT(out, ":2\r\n");

    /* KEYS / ARGV tables */
    exec_sess(s, T0, &out, 6, "EVAL", "return #KEYS", "2", "a", "b", "x");
    EXPECT(out, ":2\r\n");
    exec_sess(s, T0, &out, 6, "EVAL", "return #ARGV", "2", "a", "b", "x");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0, &out, 5, "EVAL", "return ARGV[1]", "1", "a", "payload");
    EXPECT(out, "$7\r\npayload\r\n");
    exec_sess(s, T0, &out, 5, "EVAL", "return KEYS[2]", "2", "k1", "k2");
    EXPECT(out, "$2\r\nk2\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_script_execution_limits(void)
{
    db d;
    session *s;
    resp_buf out;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "EVAL", "while true do end", "0");
    DD_CHECK(out.len > 0);
    DD_CHECK_MEM("-ERR Error running script", strlen("-ERR Error running script"),
                 out.data, strlen("-ERR Error running script"));

    exec_sess(s, T0, &out, 5, "EVAL",
               "redis.call('SET', KEYS[1], ARGV[1]) "
               "return redis.call('GET', KEYS[1])", "1", "limited", "ok");
    EXPECT(out, "$2\r\nok\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_evalsha_and_script_family(void)
{
    db d;
    session *s;
    resp_buf out;
    char sha[41], exp[128];
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    /* NOSCRIPT for an unknown sha */
    exec_sess(s, T0, &out, 3, "EVALSHA",
              "ffffffffffffffffffffffffffffffffffffffff", "0");
    EXPECT(out, "-NOSCRIPT No matching script. Please use EVAL.\r\n");

    /* EVAL then EVALSHA on the same source */
    exec_sess(s, T0, &out, 3, "EVAL", "return 1", "0");
    EXPECT(out, ":1\r\n");
    sha1_hex("return 1", 8, sha);
    exec_sess(s, T0, &out, 3, "EVALSHA", sha, "0");
    EXPECT(out, ":1\r\n");

    /* SCRIPT LOAD / EXISTS / FLUSH */
    exec_sess(s, T0, &out, 3, "SCRIPT", "LOAD", "return 42");
    sha1_hex("return 42", 9, sha);
    snprintf(exp, sizeof(exp), "$40\r\n%s\r\n", sha);
    EXPECT(out, exp);
    exec_sess(s, T0, &out, 4, "SCRIPT", "EXISTS", sha,
              "ffffffffffffffffffffffffffffffffffffffff");
    EXPECT(out, "*2\r\n:1\r\n:0\r\n");
    exec_sess(s, T0, &out, 2, "SCRIPT", "FLUSH");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "EVALSHA", sha, "0");
    EXPECT(out, "-NOSCRIPT No matching script. Please use EVAL.\r\n");

    /* SCRIPT DEBUG accepts the Redis modes; KILL has no running script */
    exec_sess(s, T0, &out, 3, "SCRIPT", "DEBUG", "YES");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 3, "SCRIPT", "DEBUG", "NO");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "SCRIPT", "KILL");
    EXPECT(out, "-NOTBUSY No scripts in execution right now\r\n");

    /* unknown subcommand */
    exec_sess(s, T0, &out, 2, "SCRIPT", "BOGUS");
    DD_CHECK(out.len > 5 && out.data[0] == '-');

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_eval_ro(void)
{
    db d;
    session *s;
    resp_buf out;
    char sha[41];
    const char *src;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "EVAL_RO", "return 1", "0");
    EXPECT(out, ":1\r\n");

    exec_sess(s, T0, &out, 4, "EVAL_RO",
              "return redis.call('GET', KEYS[1])", "1", "rokey");
    EXPECT(out, "$-1\r\n");

    exec_sess(s, T0, &out, 5, "EVAL_RO",
              "redis.call('SET', KEYS[1], ARGV[1]) return 1",
              "1", "rokey", "v");
    DD_CHECK(out.len > 5 && out.data[0] == '-');
    DD_CHECK(strstr(out.data, "read-only scripts") != NULL);
    exec_sess(s, T0, &out, 2, "GET", "rokey");
    EXPECT(out, "$-1\r\n");

    src = "return 7";
    sha1_hex(src, strlen(src), sha);
    exec_sess(s, T0, &out, 3, "EVAL_RO", src, "0");
    EXPECT(out, ":7\r\n");
    exec_sess(s, T0, &out, 3, "EVALSHA_RO", sha, "0");
    EXPECT(out, ":7\r\n");

    src = "redis.call('SET', KEYS[1], ARGV[1]) return 1";
    sha1_hex(src, strlen(src), sha);
    exec_sess(s, T0, &out, 3, "SCRIPT", "LOAD", src);
    exec_sess(s, T0, &out, 5, "EVALSHA_RO", sha, "1", "rokey", "v");
    DD_CHECK(out.len > 5 && out.data[0] == '-');
    DD_CHECK(strstr(out.data, "read-only scripts") != NULL);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_fcall_function(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "FUNCTION", "LOAD",
              "#!lua name=mylib\nreturn {KEYS[1], ARGV[1]}");
    EXPECT(out, "$5\r\nmylib\r\n");
    exec_sess(s, T0, &out, 5, "FCALL", "mylib", "1", "k", "arg");
    EXPECT(out, "*2\r\n$1\r\nk\r\n$3\r\narg\r\n");
    exec_sess(s, T0, &out, 3, "FCALL_RO", "mylib", "0");
    EXPECT(out, "*0\r\n");

    /* Redis-style libraries register multiple callable functions. */
    exec_sess(s, T0, &out, 3, "FUNCTION", "LOAD",
              "#!lua name=multi\n"
              "redis.register_function{function_name='bump', "
              "callback=function(keys,args) return redis.call('INCRBY', "
              "keys[1], args[1]) end}\n"
              "redis.register_function{function_name='pair', "
              "callback=function(keys,args) return {keys[1], args[1]} end}");
    EXPECT(out, "$5\r\nmulti\r\n");
    exec_sess(s, T0, &out, 5, "FCALL", "bump", "1", "counter", "3");
    EXPECT(out, ":3\r\n");
    exec_sess(s, T0, &out, 5, "FCALL", "pair", "1", "key-a", "arg-a");
    EXPECT(out, "*2\r\n$5\r\nkey-a\r\n$5\r\narg-a\r\n");

    exec_sess(s, T0, &out, 2, "FUNCTION", "LIST");
    DD_CHECK(strstr(out.data, "mylib") != NULL);
    DD_CHECK(strstr(out.data, "multi") != NULL);
    exec_sess(s, T0, &out, 3, "FUNCTION", "DELETE", "mylib");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 4, "FCALL", "mylib", "1", "k", "arg");
    EXPECT(out, "-ERR Function not found\r\n");

    exec_sess(s, T0, &out, 3, "FUNCTION", "LOAD",
              "#!lua name=lib2\nreturn 7");
    EXPECT(out, "$4\r\nlib2\r\n");
    exec_sess(s, T0, &out, 2, "FUNCTION", "FLUSH");
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "FUNCTION", "LIST");
    EXPECT(out, "*0\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_function_dump_restore(void)
{
    db d;
    session *s;
    resp_buf out;
    const char *p;
    size_t plen, first_len;
    char *payload;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "FUNCTION", "LOAD",
              "#!lua name=lib_a\nreturn 1");
    EXPECT(out, "$5\r\nlib_a\r\n");
    exec_sess(s, T0, &out, 2, "FUNCTION", "DUMP");
    DD_CHECK(resp_bulk_payload(&out, &p, &plen) == 0);
    first_len = plen;
    payload = (char *)malloc(plen + 1);
    DD_CHECK(payload != NULL);
    memcpy(payload, p, plen);
    payload[plen] = '\0';

    exec_sess(s, T0, &out, 2, "FUNCTION", "FLUSH");
    EXPECT(out, "+OK\r\n");
    exec_sess_restore(s, T0, &out, payload, first_len, NULL);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "FUNCTION", "LIST");
    DD_CHECK(strstr(out.data, "lib_a") != NULL);

    exec_sess(s, T0, &out, 4, "FUNCTION", "LOAD", "REPLACE",
              "#!lua name=lib_a\nreturn 99");
    exec_sess(s, T0, &out, 2, "FUNCTION", "DUMP");
    DD_CHECK(resp_bulk_payload(&out, &p, &plen) == 0);
    {
        char *v99 = (char *)malloc(plen + 1);
        DD_CHECK(v99 != NULL);
        memcpy(v99, p, plen);
        v99[plen] = '\0';

        exec_sess(s, T0, &out, 2, "FUNCTION", "FLUSH");
        exec_sess_restore(s, T0, &out, payload, first_len, NULL);
        EXPECT(out, "+OK\r\n");
        exec_sess(s, T0, &out, 3, "FCALL", "lib_a", "0");
        EXPECT(out, ":1\r\n");

        exec_sess_restore(s, T0, &out, v99, plen, "REPLACE");
        EXPECT(out, "+OK\r\n");
        exec_sess(s, T0, &out, 3, "FCALL", "lib_a", "0");
        EXPECT(out, ":99\r\n");

        exec_sess_restore(s, T0, &out, v99, plen, "APPEND");
        DD_CHECK(out.len > 0 && out.data[0] == '-');
        DD_CHECK(strstr(out.data, "already exists") != NULL);

        free(v99);
    }

    exec_sess_restore(s, T0, &out, "junk", 4, "REPLACE");
    DD_CHECK(out.len > 0 && out.data[0] == '-');
    exec_sess_restore(s, T0, &out, payload, first_len, "BOGUS");
    DD_CHECK(out.len > 0 && out.data[0] == '-');

    free(payload);
    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_error_texts(void)
{
    db d;
    session *s;
    resp_buf out;
    char sha[41], exp[512];
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    /* compile error */
    exec_sess(s, T0, &out, 3, "EVAL", "not valid lua !!", "0");
    DD_CHECK(out.len > 5 && memcmp(out.data, "-ERR ", 5) == 0);
    out.data[out.len] = '\0';
    DD_CHECK(strstr(out.data, "Error compiling script (new function):") !=
             NULL);

    /* Lua runtime error */
    {
        const char *script = "return undefined_fn()";
        sha1_hex(script, strlen(script), sha);
        snprintf(exp, sizeof(exp),
                 "-ERR Error running script (call to f_%s): script:1: "
                 "attempt to call global 'undefined_fn' (a nil value)\r\n",
                 sha);
        exec_sess(s, T0, &out, 3, "EVAL", script, "0");
        EXPECT(out, exp);
    }

    /* redis.call error (no position prefix on the message) */
    exec_sess(s, T0, &out, 3, "SET", "str", "x");
    {
        const char *script = "return redis.call('INCR', KEYS[1])";
        sha1_hex(script, strlen(script), sha);
        snprintf(exp, sizeof(exp),
                 "-ERR Error running script (call to f_%s): ERR value is "
                 "not an integer or out of range\r\n",
                 sha);
        exec_sess(s, T0, &out, 4, "EVAL", script, "1", "str");
        EXPECT(out, exp);
    }

    /* nested EVAL via redis.call is rejected */
    exec_sess(s, T0, &out, 3, "EVAL", "return redis.call('EVAL', 'return 1', '0')",
              "0");
    out.data[out.len] = '\0';
    DD_CHECK(strstr(out.data, "not allowed from scripts") != NULL);

    /* numkeys validation */
    exec_sess(s, T0, &out, 4, "EVAL", "return 1", "2", "onlyone");
    EXPECT(out,
           "-ERR Number of keys can't be greater than number of args\r\n");
    exec_sess(s, T0, &out, 4, "EVAL", "return 1", "-1", "k");
    EXPECT(out, "-ERR Number of keys can't be negative\r\n");
    exec_sess(s, T0, &out, 2, "EVAL", "return 1");
    DD_CHECK(out.len > 5 && memcmp(out.data, "-ERR ", 5) == 0);

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_pcall_captures(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "str", "x");
    /* pcall converts a command error into an {err=...} table; returning
     * that table yields the error reply */
    exec_sess(s, T0, &out, 4, "EVAL",
              "return redis.pcall('INCR', KEYS[1])", "1", "str");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    /* pcall success: missing GET -> false -> null bulk */
    exec_sess(s, T0, &out, 4, "EVAL", "return redis.pcall('GET', KEYS[1])",
              "1", "missing");
    EXPECT(out, "$-1\r\n");
    /* pcall lets the script continue after an error */
    exec_sess(s, T0, &out, 4, "EVAL",
              "local r = redis.pcall('INCR', KEYS[1]) "
              "if r.err then return 'caught' end return 'miss'",
              "1", "str");
    EXPECT(out, "$6\r\ncaught\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

/* ------------------------------------------------------------------ */
/* AOF: effects replication logs the effect commands, not the script  */
/* ------------------------------------------------------------------ */
#include "server/aof.h"

#define TMP_AOF "test_eval_tmp.aof"

static void test_log(void *ctx, int db_index, const resp_value *argv,
                     size_t argc, const char *raw, size_t raw_len)
{
    (void)db_index;
    (void)raw;
    (void)raw_len;
    aof_log_cmd((aof *)ctx, argv, argc);
}

static void test_aof_records_effects(void)
{
    db d;
    aof *a;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    a = aof_open(TMP_AOF);
    DD_CHECK(a != NULL);
    s = session_create(&d);
    s->aof_ctx = a;
    s->aof_log = test_log;

    exec_sess(s, T0, &out, 6, "EVAL",
              "redis.call('SET', KEYS[1], ARGV[1]) "
              "redis.call('INCR', KEYS[2]) return 1",
              "2", "k", "ctr", "v");
    EXPECT(out, ":1\r\n");
    /* read-only script logs nothing (and the EVAL itself never logs) */
    exec_sess(s, T0, &out, 3, "EVAL", "return 1", "0");
    EXPECT(out, ":1\r\n");
    aof_flush(a);
    aof_close(a);

    {
        FILE *f = fopen(TMP_AOF, "rb");
        char buf[4096];
        size_t n;
        DD_CHECK(f != NULL);
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        DD_CHECK(strstr(buf, "$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n") != NULL);
        DD_CHECK(strstr(buf, "$4\r\nINCR\r\n$3\r\nctr\r\n") != NULL);
        DD_CHECK(strstr(buf, "EVAL") == NULL);
        DD_CHECK(strstr(buf, "redis.call") == NULL);
    }

    exec_sess(s, T0, &out, 2, "GET", "k");
    EXPECT(out, "$1\r\nv\r\n");
    exec_sess(s, T0, &out, 2, "GET", "ctr");
    EXPECT(out, "$1\r\n1\r\n");

    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
    remove(TMP_AOF);
}

/* ------------------------------------------------------------------ */
/* mt: scripts run per worker (shared-nothing dbs)                    */
/* ------------------------------------------------------------------ */
#include "pal/pal_socket.h"
#include "pal/pal_time.h"
#include "server/mt_server.h"

static size_t mt_ask(pal_socket_t c, const char *req, char *buf, size_t cap)
{
    size_t got = 0;
    int iter = 0;
    DD_CHECK_EQ_INT((long long)strlen(req),
                    (long long)pal_send(c, req, strlen(req)));
    while (iter < 5000) {
        ptrdiff_t n;
        iter++;
        n = pal_recv(c, buf + got, cap - got - 1);
        if (n > 0) {
            got += (size_t)n;
            break;
        }
        pal_sleep_ms(1);
    }
    buf[got] = '\0';
    return got;
}

static void test_mt_eval(void)
{
    mt_server *ms;
    pal_socket_t c;
    char buf[512];

    DD_CHECK_EQ_INT(0, pal_socket_init());
    ms = mt_server_create("127.0.0.1", 0, 2);
    DD_CHECK(ms != NULL);
    DD_CHECK_EQ_INT(0, mt_server_start(ms));

    c = pal_tcp_connect("127.0.0.1", mt_server_port(ms));
    DD_CHECK(c != PAL_SOCKET_INVALID);
    DD_CHECK_EQ_INT(0, pal_set_nonblocking(c, 1));

    mt_ask(c, "*3\r\n$4\r\nEVAL\r\n$10\r\nreturn 1+1\r\n$1\r\n0\r\n", buf,
           sizeof(buf));
    DD_CHECK_STR(":2\r\n", buf);
    /* set+get inside one script: same worker's db, works under mt */
    mt_ask(c,
           "*5\r\n$4\r\nEVAL\r\n$69\r\nredis.call('SET', KEYS[1], ARGV[1]) "
           "return redis.call('GET', KEYS[1])\r\n$1\r\n1\r\n$1\r\nk\r\n"
           "$1\r\nv\r\n",
           buf, sizeof(buf));
    DD_CHECK_STR("$1\r\nv\r\n", buf);

    pal_close(c);
    mt_server_stop(ms);
    mt_server_destroy(ms);
    pal_socket_cleanup();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_return_matrix);
    DD_RUN(test_redis_call_and_bindings);
    DD_RUN(test_script_execution_limits);
    DD_RUN(test_evalsha_and_script_family);
    DD_RUN(test_eval_ro);
    DD_RUN(test_fcall_function);
    DD_RUN(test_function_dump_restore);
    DD_RUN(test_error_texts);
    DD_RUN(test_pcall_captures);
    DD_RUN(test_aof_records_effects);
    DD_RUN(test_mt_eval);
    return DD_TEST_SUMMARY();
}
