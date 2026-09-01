/* test_dump.c - DUMP/RESTORE per-key serialization + CRC64. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/crc64.h"
#include "core/session.h"
#include "core/snapshot.h"
#include "pal/pal_thread.h"
#include "test.h"

#define T0 1000000ULL

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

/* RESTORE with a binary payload (cannot go through the varargs helper). */
static void exec_restore_named(session *s, uint64_t now, resp_buf *out,
                               const char *cmd, const char *key,
                               const char *ttl, const char *payload,
                               size_t plen, int replace)
{
    resp_value argv[5];
    size_t argc = replace ? 5 : 4;
    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING;
    argv[0].str = cmd;
    argv[0].len = strlen(cmd);
    argv[1].type = RESP_BULK_STRING;
    argv[1].str = key;
    argv[1].len = strlen(key);
    argv[2].type = RESP_BULK_STRING;
    argv[2].str = ttl;
    argv[2].len = strlen(ttl);
    argv[3].type = RESP_BULK_STRING;
    argv[3].str = payload;
    argv[3].len = plen;
    if (replace) {
        argv[4].type = RESP_BULK_STRING;
        argv[4].str = "REPLACE";
        argv[4].len = 7;
    }
    out->len = 0;
    session_execute_at(s, argv, argc, out, now);
}

static void exec_restore(session *s, uint64_t now, resp_buf *out,
                         const char *key, const char *ttl,
                         const char *payload, size_t plen, int replace)
{
    exec_restore_named(s, now, out, "RESTORE", key, ttl, payload, plen,
                       replace);
}

/* RESTORE with a binary payload plus arbitrary string options. */
static void exec_restore_opts(session *s, uint64_t now, resp_buf *out,
                              const char *key, const char *ttl,
                              const char *payload, size_t plen, int nopts, ...)
{
    resp_value argv[10];
    size_t argc = 4;
    va_list ap;
    int i;

    memset(argv, 0, sizeof(argv));
    argv[0].type = RESP_BULK_STRING;
    argv[0].str = "RESTORE";
    argv[0].len = 7;
    argv[1].type = RESP_BULK_STRING;
    argv[1].str = key;
    argv[1].len = strlen(key);
    argv[2].type = RESP_BULK_STRING;
    argv[2].str = ttl;
    argv[2].len = strlen(ttl);
    argv[3].type = RESP_BULK_STRING;
    argv[3].str = payload;
    argv[3].len = plen;

    va_start(ap, nopts);
    for (i = 0; i < nopts; i++) {
        const char *opt = va_arg(ap, const char *);
        argv[argc].type = RESP_BULK_STRING;
        argv[argc].str = opt;
        argv[argc].len = strlen(opt);
        argc++;
    }
    va_end(ap);

    out->len = 0;
    session_execute_at(s, argv, argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

/* Parse a "$<len>\r\n<bytes>\r\n" reply; returns bulk length or -1 ($-1). */
static long long bulk_payload(const resp_buf *out, const char **p)
{
    long long n = 0;
    const char *s;
    if (out->len < 4 || out->data[0] != '$')
        return -2;
    s = out->data + 1;
    if (*s == '-')
        return -1;
    while (*s != '\r') {
        n = n * 10 + (*s - '0');
        s++;
    }
    *p = s + 2;
    return n;
}

/* DUMP key via the command layer; copies the payload into buf (cap bytes).
 * Returns payload length, or -1 when the key is missing. */
static long long dump_key(session *s, uint64_t now, resp_buf *out,
                          const char *key, char *buf, size_t cap)
{
    const char *p;
    long long n;
    exec_sess(s, now, out, 2, "DUMP", key);
    n = bulk_payload(out, &p);
    if (n >= 0 && (size_t)n <= cap)
        memcpy(buf, p, (size_t)n);
    return n;
}

static void test_crc64_vector(void)
{
    uint64_t whole = crc64(0, "123456789", 9);
    uint64_t split = crc64(crc64(0, "1234", 4), "56789", 5);
    DD_CHECK_EQ_INT(0x995DC9BBDF1939FALL, (long long)whole);
    DD_CHECK_EQ_INT((long long)whole, (long long)split);
}

typedef struct crc64_worker_ctx {
    int failures;
} crc64_worker_ctx;

static void *crc64_worker(void *arg)
{
    crc64_worker_ctx *ctx = (crc64_worker_ctx *)arg;
    int i;
    for (i = 0; i < 100000; i++) {
        if (crc64(0, "123456789", 9) != 0x995DC9BBDF1939FAULL)
            ctx->failures++;
    }
    return NULL;
}

static void test_crc64_concurrent_initialization(void)
{
    enum { THREADS = 8 };
    pal_thread threads[THREADS];
    crc64_worker_ctx ctx[THREADS];
    int i;

    memset(ctx, 0, sizeof(ctx));
    for (i = 0; i < THREADS; i++)
        DD_CHECK_EQ_INT(0, pal_thread_create(&threads[i], crc64_worker,
                                             &ctx[i]));
    for (i = 0; i < THREADS; i++)
        DD_CHECK_EQ_INT(0, pal_thread_join(&threads[i], NULL));
    for (i = 0; i < THREADS; i++)
        DD_CHECK_EQ_INT(0, ctx[i].failures);
}

static void test_dump_missing(void)
{
    db d;
    session *s;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 2, "DUMP", "nope");
    EXPECT(out, "$-1\r\n");
    exec_sess(s, T0, &out, 1, "DUMP");
    DD_CHECK(out.len > 5 && memcmp(out.data, "-ERR ", 5) == 0);

    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
}

static void test_payload_layout(void)
{
    db d;
    session *s;
    resp_buf out;
    char buf[256];
    long long n;
    uint64_t stored = 0, calc;
    int i;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "foo", "bar");
    n = dump_key(s, T0, &out, "foo", buf, sizeof(buf));
    /* u16 version + u8 type + u32 len + 3 bytes + u64 crc = 18 */
    DD_CHECK_EQ_INT(18, n);
    DD_CHECK_EQ_INT(1, buf[0]); /* version lo */
    DD_CHECK_EQ_INT(0, buf[1]); /* version hi */
    for (i = 0; i < 8; i++)
        stored |= (uint64_t)(uint8_t)buf[n - 8 + i] << (8 * i);
    calc = crc64(0, buf, (size_t)n - 8);
    DD_CHECK_EQ_INT((long long)calc, (long long)stored);

    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
}

static void test_roundtrip_all_types(void)
{
    db d, d2;
    session *s, *r;
    resp_buf out;
    char buf[65536];
    long long n;

    db_init(&d);
    db_init(&d2);
    resp_buf_init(&out);
    s = session_create(&d);
    r = session_create(&d2);

    exec_sess(s, T0, &out, 3, "SET", "str", "hello");
    exec_sess(s, T0, &out, 6, "HSET", "h", "f1", "v1", "f2", "v2");
    exec_sess(s, T0, &out, 4, "RPUSH", "l", "a", "b");
    exec_sess(s, T0, &out, 4, "SADD", "st", "x", "y");
    exec_sess(s, T0, &out, 4, "ZADD", "z", "1.5", "m1");

    n = dump_key(s, T0, &out, "str", buf, sizeof(buf));
    DD_CHECK(n > 0);
    exec_restore(r, T0, &out, "str", "0", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 2, "GET", "str");
    EXPECT(out, "$5\r\nhello\r\n");

    n = dump_key(s, T0, &out, "h", buf, sizeof(buf));
    DD_CHECK(n > 0);
    exec_restore(r, T0, &out, "h", "0", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 3, "HGET", "h", "f2");
    EXPECT(out, "$2\r\nv2\r\n");

    n = dump_key(s, T0, &out, "l", buf, sizeof(buf));
    DD_CHECK(n > 0);
    exec_restore(r, T0, &out, "l", "0", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 4, "LRANGE", "l", "0", "-1");
    EXPECT(out, "*2\r\n$1\r\na\r\n$1\r\nb\r\n");

    n = dump_key(s, T0, &out, "st", buf, sizeof(buf));
    DD_CHECK(n > 0);
    exec_restore(r, T0, &out, "st", "0", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 3, "SISMEMBER", "st", "y");
    EXPECT(out, ":1\r\n");

    n = dump_key(s, T0, &out, "z", buf, sizeof(buf));
    DD_CHECK(n > 0);
    exec_restore(r, T0, &out, "z", "0", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 3, "ZSCORE", "z", "m1");
    EXPECT(out, "$3\r\n1.5\r\n");

    /* keys are independent: source untouched, target had no prior data */
    exec_sess(s, T0, &out, 1, "DBSIZE");
    EXPECT(out, ":5\r\n");
    exec_sess(r, T0, &out, 1, "DBSIZE");
    EXPECT(out, ":5\r\n");

    session_free(s);
    session_free(r);
    db_destroy(&d);
    db_destroy(&d2);
    resp_buf_free(&out);
}

static void test_restore_bad_payload(void)
{
    db d;
    session *s;
    resp_buf out;
    char buf[256];
    long long n;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    /* too short / garbage */
    exec_restore(s, T0, &out, "k", "0", "garbage", 7, 0);
    EXPECT(out, "-ERR Bad data format\r\n");

    /* truncated valid payload */
    exec_sess(s, T0, &out, 3, "SET", "foo", "bar");
    n = dump_key(s, T0, &out, "foo", buf, sizeof(buf));
    DD_CHECK_EQ_INT(18, n);
    exec_restore(s, T0, &out, "k", "0", buf, (size_t)n - 3, 0);
    EXPECT(out, "-ERR Bad data format\r\n");

    /* corrupted byte: crc must catch it */
    buf[5] ^= 0x01;
    exec_restore(s, T0, &out, "k", "0", buf, (size_t)n, 0);
    EXPECT(out, "-ERR Bad data format\r\n");

    /* nothing was installed */
    exec_sess(s, T0, &out, 2, "EXISTS", "k");
    EXPECT(out, ":0\r\n");

    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
}

static void test_restore_busykey_and_replace(void)
{
    db d;
    session *s;
    resp_buf out;
    char buf[256];
    long long n;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "foo", "bar");
    n = dump_key(s, T0, &out, "foo", buf, sizeof(buf));
    DD_CHECK(n > 0);

    exec_sess(s, T0, &out, 3, "SET", "dst", "old");
    exec_restore(s, T0, &out, "dst", "0", buf, (size_t)n, 0);
    EXPECT(out, "-BUSYKEY Target key name already exists.\r\n");
    exec_sess(s, T0, &out, 2, "GET", "dst");
    EXPECT(out, "$3\r\nold\r\n");

    exec_restore(s, T0, &out, "dst", "0", buf, (size_t)n, 1);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "dst");
    EXPECT(out, "$3\r\nbar\r\n");

    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
}

static void test_restore_ttl(void)
{
    db d;
    session *s;
    resp_buf out;
    char buf[256];
    long long n;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "foo", "bar");
    n = dump_key(s, T0, &out, "foo", buf, sizeof(buf));
    DD_CHECK(n > 0);

    /* relative ttl in ms */
    exec_restore(s, T0, &out, "k1", "100", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "PTTL", "k1");
    EXPECT(out, ":100\r\n");

    /* ttl 0: persist */
    exec_restore(s, T0, &out, "k2", "0", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "PTTL", "k2");
    EXPECT(out, ":-1\r\n");

    /* negative ttl rejected */
    exec_restore(s, T0, &out, "k3", "-5", buf, (size_t)n, 0);
    EXPECT(out, "-ERR Invalid TTL value, must be >= 0\r\n");

    /* expired keys are restored dead: not visible */
    exec_restore(s, T0 + 200, &out, "k4", "100", buf, (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0 + 200, &out, 2, "EXISTS", "k4");
    EXPECT(out, ":1\r\n");
    exec_sess(s, T0 + 301, &out, 2, "EXISTS", "k4");
    EXPECT(out, ":0\r\n");

    session_free(s);
    db_destroy(&d);
    resp_buf_free(&out);
}

static void test_restore_options(void)
{
    db src, dst;
    session *s, *r;
    resp_buf out;
    char buf[256];
    long long n;

    db_init(&src);
    db_init(&dst);
    resp_buf_init(&out);
    s = session_create(&src);
    r = session_create(&dst);

    exec_sess(s, T0, &out, 3, "SET", "src", "value");
    EXPECT(out, "+OK\r\n");
    n = dump_key(s, T0, &out, "src", buf, sizeof(buf));
    DD_CHECK(n > 0);

    /* ABSTTL treats the TTL argument as an absolute Unix ms timestamp. */
    exec_restore_opts(r, T0, &out, "abs", "1005000", buf, (size_t)n, 1,
                      "ABSTTL");
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 2, "PTTL", "abs");
    EXPECT(out, ":5000\r\n");

    /* IDLETIME and FREQ are accepted and do not affect the restored value. */
    exec_restore_opts(r, T0, &out, "meta", "0", buf, (size_t)n, 4,
                      "IDLETIME", "42", "FREQ", "7");
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 2, "GET", "meta");
    EXPECT(out, "$5\r\nvalue\r\n");

    /* Options may be combined; REPLACE plus ABSTTL replaces an existing key. */
    exec_restore_opts(r, T0, &out, "abs", "1006000", buf, (size_t)n, 6,
                      "REPLACE", "ABSTTL", "IDLETIME", "1", "FREQ", "2");
    EXPECT(out, "+OK\r\n");
    exec_sess(r, T0, &out, 2, "PTTL", "abs");
    EXPECT(out, ":6000\r\n");

    /* Negative IDLETIME is rejected with a syntax error. */
    exec_restore_opts(r, T0, &out, "bad", "0", buf, (size_t)n, 2,
                      "IDLETIME", "-1");
    EXPECT(out, "-ERR syntax error\r\n");

    session_free(s);
    session_free(r);
    db_destroy(&src);
    db_destroy(&dst);
    resp_buf_free(&out);
}

static void test_restore_asking(void)
{
    db d;
    session *s;
    resp_buf out;
    char buf[256];
    long long n;

    db_init(&d);
    resp_buf_init(&out);
    s = session_create(&d);

    exec_sess(s, T0, &out, 3, "SET", "foo", "bar");
    n = dump_key(s, T0, &out, "foo", buf, sizeof(buf));
    DD_CHECK(n > 0);

    exec_restore_named(s, T0, &out, "RESTORE-ASKING", "dst", "0", buf,
                       (size_t)n, 0);
    EXPECT(out, "+OK\r\n");
    exec_sess(s, T0, &out, 2, "GET", "dst");
    EXPECT(out, "$3\r\nbar\r\n");

    exec_restore_named(s, T0, &out, "RESTORE-ASKING", "dst", "0", buf,
                       (size_t)n, 0);
    EXPECT(out, "-BUSYKEY Target key name already exists.\r\n");

    session_free(s);
    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_crc64_concurrent_initialization);
    DD_RUN(test_crc64_vector);
    DD_RUN(test_dump_missing);
    DD_RUN(test_payload_layout);
    DD_RUN(test_roundtrip_all_types);
    DD_RUN(test_restore_bad_payload);
    DD_RUN(test_restore_busykey_and_replace);
    DD_RUN(test_restore_ttl);
    DD_RUN(test_restore_options);
    DD_RUN(test_restore_asking);
    return DD_TEST_SUMMARY();
}
