/* test_keys.c - generic key command tests (written before the impl):
 * TYPE, KEYS, SCAN, RENAME, RENAMENX, TOUCH, RANDOMKEY,
 * EXPIRETIME, PEXPIRETIME. Synthetic wall time, no sleeps. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

#define T0 1000000ULL /* synthetic epoch base, ms */

/* Build an argv of bulk strings from varargs and execute with injected time.
 * out is reset first, so the reply is exactly out.data[0..out.len). */
static void exec_cmd(db *d, uint64_t now, resp_buf *out, int argc, ...)
{
    resp_value argv[8];
    va_list ap;
    int i;
    va_start(ap, argc);
    for (i = 0; i < argc; i++) {
        const char *s = va_arg(ap, const char *);
        memset(&argv[i], 0, sizeof(argv[i]));
        argv[i].type = RESP_BULK_STRING;
        argv[i].str = s;
        argv[i].len = strlen(s);
    }
    va_end(ap);
    out->len = 0;
    command_execute_at(d, argv, (size_t)argc, out, now);
}

#define EXPECT(out, s) DD_CHECK_MEM((s), strlen(s), (out).data, (out).len)

/* Header element count of a top-level array reply ("*N\r\n..."). */
static long reply_array_len(const resp_buf *out)
{
    if (out->len < 4 || out->data[0] != '*')
        return -1;
    return strtol(out->data + 1, NULL, 10);
}

/* 1 when the reply contains the bulk-encoded key ("\r\n<key>\r\n"). */
static int reply_has_key(const resp_buf *out, const char *key)
{
    char needle[300];
    size_t nl = strlen(key);
    size_t i;
    if (nl + 4 > sizeof(needle))
        return 0;
    needle[0] = '\r';
    needle[1] = '\n';
    memcpy(needle + 2, key, nl);
    needle[2 + nl] = '\r';
    needle[3 + nl] = '\n';
    nl += 4;
    if (out->len < nl)
        return 0;
    for (i = 0; i + nl <= out->len; i++)
        if (memcmp(out->data + i, needle, nl) == 0)
            return 1;
    return 0;
}

/* Walk a SCAN reply: "*2\r\n$<n>\r\n<cursor>\r\n*<m>\r\n" + m key bulks.
 * Copies the next cursor (NUL-terminated) and calls cb per key. */
static int scan_walk(const resp_buf *out, char *cursor, size_t ccap,
                     void (*cb)(const char *k, size_t kl, void *ctx),
                     void *ctx)
{
    const char *p = out->data;
    const char *end = out->data + out->len;
    char *np;
    long cl, n, i;
    if (out->len < 4 || memcmp(p, "*2\r\n", 4) != 0)
        return -1;
    p += 4;
    if (p >= end || *p != '$')
        return -1;
    cl = strtol(p + 1, &np, 10);
    p = np;
    if (end - p < 2 || memcmp(p, "\r\n", 2) != 0)
        return -1;
    p += 2;
    if (cl < 0 || end - p < cl + 2 || (size_t)cl >= ccap)
        return -1;
    memcpy(cursor, p, (size_t)cl);
    cursor[cl] = '\0';
    p += cl;
    if (memcmp(p, "\r\n", 2) != 0)
        return -1;
    p += 2;
    if (p >= end || *p != '*')
        return -1;
    n = strtol(p + 1, &np, 10);
    p = np;
    if (end - p < 2 || memcmp(p, "\r\n", 2) != 0)
        return -1;
    p += 2;
    for (i = 0; i < n; i++) {
        long kl;
        if (p >= end || *p != '$')
            return -1;
        kl = strtol(p + 1, &np, 10);
        p = np;
        if (end - p < 2 || memcmp(p, "\r\n", 2) != 0)
            return -1;
        p += 2;
        if (kl < 0 || end - p < kl + 2)
            return -1;
        cb(p, (size_t)kl, ctx);
        p += kl;
        if (memcmp(p, "\r\n", 2) != 0)
            return -1;
        p += 2;
    }
    return 0;
}

typedef void (*key_cb)(const char *k, size_t kl, void *ctx);

/* Full SCAN iteration (optionally with MATCH) until the cursor returns to
 * "0". Returns the number of round trips, -1 on malformed reply/no
 * convergence. */
static int scan_collect(db *d, uint64_t now, const char *match, long count,
                        key_cb cb, void *ctx)
{
    char cursor[32] = "0";
    char countstr[24];
    resp_buf out;
    int rounds = 0;
    snprintf(countstr, sizeof(countstr), "%ld", count);
    resp_buf_init(&out);
    do {
        if (match != NULL)
            exec_cmd(d, now, &out, 6, "SCAN", cursor, "MATCH", match, "COUNT",
                     countstr);
        else
            exec_cmd(d, now, &out, 4, "SCAN", cursor, "COUNT", countstr);
        if (scan_walk(&out, cursor, sizeof(cursor), cb, ctx) != 0) {
            resp_buf_free(&out);
            return -1;
        }
        if (++rounds > 100000) {
            resp_buf_free(&out);
            return -1;
        }
    } while (strcmp(cursor, "0") != 0);
    resp_buf_free(&out);
    return rounds;
}

static void test_type(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "s", "v");
    exec_cmd(&d, T0, &out, 2, "TYPE", "s");
    EXPECT(out, "+string\r\n");
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 2, "TYPE", "h");
    EXPECT(out, "+hash\r\n");
    exec_cmd(&d, T0, &out, 3, "LPUSH", "l", "e");
    exec_cmd(&d, T0, &out, 2, "TYPE", "l");
    EXPECT(out, "+list\r\n");
    exec_cmd(&d, T0, &out, 3, "SADD", "st", "m");
    exec_cmd(&d, T0, &out, 2, "TYPE", "st");
    EXPECT(out, "+set\r\n");
    exec_cmd(&d, T0, &out, 4, "ZADD", "z", "1", "m");
    exec_cmd(&d, T0, &out, 2, "TYPE", "z");
    EXPECT(out, "+zset\r\n");
    exec_cmd(&d, T0, &out, 2, "TYPE", "missing");
    EXPECT(out, "+none\r\n");

    /* expired key reports none and is lazily collected */
    exec_cmd(&d, T0, &out, 3, "SET", "e", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "e", "1000");
    exec_cmd(&d, T0 + 2000, &out, 2, "TYPE", "e");
    EXPECT(out, "+none\r\n");
    exec_cmd(&d, T0 + 2000, &out, 2, "GET", "e");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, T0, &out, 1, "TYPE");
    EXPECT(out, "-ERR wrong number of arguments for 'type' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_keys(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "a", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "ab", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "abc", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "b", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "key:1", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "key:2", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "p*", "1");

    exec_cmd(&d, T0, &out, 2, "KEYS", "*");
    DD_CHECK_EQ_INT(7, reply_array_len(&out));
    DD_CHECK(reply_has_key(&out, "a"));
    DD_CHECK(reply_has_key(&out, "ab"));
    DD_CHECK(reply_has_key(&out, "abc"));
    DD_CHECK(reply_has_key(&out, "b"));
    DD_CHECK(reply_has_key(&out, "key:1"));
    DD_CHECK(reply_has_key(&out, "key:2"));
    DD_CHECK(reply_has_key(&out, "p*"));

    exec_cmd(&d, T0, &out, 2, "KEYS", "a?");
    DD_CHECK_EQ_INT(1, reply_array_len(&out));
    DD_CHECK(reply_has_key(&out, "ab"));

    exec_cmd(&d, T0, &out, 2, "KEYS", "a??");
    DD_CHECK_EQ_INT(1, reply_array_len(&out));
    DD_CHECK(reply_has_key(&out, "abc"));

    exec_cmd(&d, T0, &out, 2, "KEYS", "[ab]");
    DD_CHECK_EQ_INT(2, reply_array_len(&out));
    DD_CHECK(reply_has_key(&out, "a"));
    DD_CHECK(reply_has_key(&out, "b"));

    exec_cmd(&d, T0, &out, 2, "KEYS", "key:[12]");
    DD_CHECK_EQ_INT(2, reply_array_len(&out));
    DD_CHECK(reply_has_key(&out, "key:1"));
    DD_CHECK(reply_has_key(&out, "key:2"));

    exec_cmd(&d, T0, &out, 2, "KEYS", "key:[^1]");
    DD_CHECK_EQ_INT(1, reply_array_len(&out));
    DD_CHECK(reply_has_key(&out, "key:2"));

    exec_cmd(&d, T0, &out, 2, "KEYS", "p\\*");
    DD_CHECK_EQ_INT(1, reply_array_len(&out));
    DD_CHECK(reply_has_key(&out, "p*"));

    exec_cmd(&d, T0, &out, 2, "KEYS", "nomatch*");
    EXPECT(out, "*0\r\n");

    /* expired keys are excluded and lazily collected */
    exec_cmd(&d, T0, &out, 3, "SET", "gone", "1");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "gone", "1000");
    exec_cmd(&d, T0 + 2000, &out, 2, "KEYS", "*");
    DD_CHECK_EQ_INT(7, reply_array_len(&out));
    DD_CHECK(!reply_has_key(&out, "gone"));
    exec_cmd(&d, T0 + 2000, &out, 2, "GET", "gone");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, T0, &out, 1, "KEYS");
    EXPECT(out, "-ERR wrong number of arguments for 'keys' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static int seen[1024];

static void seen_cb(const char *k, size_t kl, void *ctx)
{
    long v = 0;
    size_t i;
    (void)ctx;
    if (kl <= 3 || memcmp(k, "sk:", 3) != 0)
        return;
    for (i = 3; i < kl; i++) {
        if (k[i] < '0' || k[i] > '9')
            return;
        v = v * 10 + (k[i] - '0');
    }
    if (v < 1024)
        seen[v]++;
}

static void test_scan_full_iteration(void)
{
    db d;
    resp_buf out;
    char key[32];
    int i;

    db_init(&d);
    resp_buf_init(&out);

    /* empty db: single round trip, empty key array */
    exec_cmd(&d, T0, &out, 2, "SCAN", "0");
    EXPECT(out, "*2\r\n$1\r\n0\r\n*0\r\n");

    /* 300 keys force several doublings; the full iteration must see every
     * key (mid-rehash rounds included) and converge. */
    for (i = 0; i < 300; i++) {
        snprintf(key, sizeof(key), "sk:%d", i);
        exec_cmd(&d, T0, &out, 3, "SET", key, "v");
    }
    memset(seen, 0, sizeof(seen));
    DD_CHECK(scan_collect(&d, T0, NULL, 10, seen_cb, NULL) > 1);
    for (i = 0; i < 300; i++)
        DD_CHECK(seen[i] >= 1);

    /* COUNT 1 also converges */
    memset(seen, 0, sizeof(seen));
    DD_CHECK(scan_collect(&d, T0, NULL, 1, seen_cb, NULL) > 1);
    for (i = 0; i < 300; i++)
        DD_CHECK(seen[i] >= 1);

    resp_buf_free(&out);
    db_destroy(&d);
}

static long match_count;
static long match_bad;

static void match_cb(const char *k, size_t kl, void *ctx)
{
    (void)ctx;
    match_count++;
    if (kl < 4 || memcmp(k, "sk:1", 4) != 0)
        match_bad++;
}

static void test_scan_match(void)
{
    db d;
    resp_buf out;
    char key[32];
    int i;

    db_init(&d);
    resp_buf_init(&out);
    for (i = 0; i < 300; i++) {
        snprintf(key, sizeof(key), "sk:%d", i);
        exec_cmd(&d, T0, &out, 3, "SET", key, "v");
    }
    /* sk:1, sk:10..19, sk:100..199 -> 111 keys */
    match_count = 0;
    match_bad = 0;
    DD_CHECK(scan_collect(&d, T0, "sk:1*", 10000, match_cb, NULL) > 0);
    DD_CHECK_EQ_INT(111, match_count);
    DD_CHECK_EQ_INT(0, match_bad);

    /* pattern matching nothing: converges with zero keys */
    match_count = 0;
    DD_CHECK(scan_collect(&d, T0, "zzz*", 10, match_cb, NULL) > 0);
    DD_CHECK_EQ_INT(0, match_count);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_scan_errors(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 2, "SCAN", "-1");
    EXPECT(out, "-ERR invalid cursor\r\n");
    exec_cmd(&d, T0, &out, 2, "SCAN", "abc");
    EXPECT(out, "-ERR invalid cursor\r\n");
    exec_cmd(&d, T0, &out, 2, "SCAN", "12x");
    EXPECT(out, "-ERR invalid cursor\r\n");
    exec_cmd(&d, T0, &out, 2, "SCAN", "");
    EXPECT(out, "-ERR invalid cursor\r\n");
    exec_cmd(&d, T0, &out, 3, "SCAN", "0", "MATCH");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 3, "SCAN", "0", "COUNT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "SCAN", "0", "COUNT", "0");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "SCAN", "0", "COUNT", "x");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 4, "SCAN", "0", "FOO", "bar");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 5, "SCAN", "0", "MATCH", "a", "COUNT");
    EXPECT(out, "-ERR syntax error\r\n");
    exec_cmd(&d, T0, &out, 1, "SCAN");
    EXPECT(out, "-ERR wrong number of arguments for 'scan' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_scan_skips_expired(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "sk:1", "v");
    exec_cmd(&d, T0, &out, 3, "SET", "sk:2", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "sk:2", "1000");
    memset(seen, 0, sizeof(seen));
    DD_CHECK(scan_collect(&d, T0 + 2000, NULL, 10, seen_cb, NULL) > 0);
    DD_CHECK_EQ_INT(1, seen[1]);
    DD_CHECK_EQ_INT(0, seen[2]);
    /* expired key was lazily collected */
    exec_cmd(&d, T0 + 2000, &out, 2, "GET", "sk:2");
    EXPECT(out, "$-1\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_rename(void)
{
    db d;
    resp_buf out;
    uint64_t vs, vd, dirty0;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "a", "1");
    exec_cmd(&d, T0, &out, 3, "RENAME", "a", "b");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "a");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "b");
    EXPECT(out, "$1\r\n1\r\n");

    /* TTL moves with the value */
    exec_cmd(&d, T0, &out, 3, "SET", "t", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "t", "100000");
    exec_cmd(&d, T0, &out, 3, "RENAME", "t", "t2");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "t2");
    EXPECT(out, ":100000\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "t");
    EXPECT(out, ":-2\r\n");

    /* dst is overwritten and its old TTL is gone */
    exec_cmd(&d, T0, &out, 3, "SET", "d", "dv");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "d", "100000");
    exec_cmd(&d, T0, &out, 3, "SET", "s", "sv");
    exec_cmd(&d, T0, &out, 3, "RENAME", "s", "d");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "d");
    EXPECT(out, "$2\r\nsv\r\n");
    exec_cmd(&d, T0, &out, 2, "PTTL", "d");
    EXPECT(out, ":-1\r\n");

    /* error cases */
    exec_cmd(&d, T0, &out, 3, "RENAME", "missing", "x");
    EXPECT(out, "-ERR no such key\r\n");
    exec_cmd(&d, T0, &out, 3, "RENAME", "b", "b");
    EXPECT(out,
           "-ERR source and destination objects are the same\r\n");
    /* src existence is checked before the same-key check (Redis order) */
    exec_cmd(&d, T0, &out, 3, "RENAME", "missing", "missing");
    EXPECT(out, "-ERR no such key\r\n");
    /* expired src counts as missing */
    exec_cmd(&d, T0, &out, 3, "SET", "e", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "e", "1000");
    exec_cmd(&d, T0 + 2000, &out, 3, "RENAME", "e", "f");
    EXPECT(out, "-ERR no such key\r\n");
    exec_cmd(&d, T0, &out, 2, "RENAME", "b");
    EXPECT(out, "-ERR wrong number of arguments for 'rename' command\r\n");

    /* value type is preserved (hash blob moved as-is) */
    exec_cmd(&d, T0, &out, 4, "HSET", "h", "f", "v");
    exec_cmd(&d, T0, &out, 3, "RENAME", "h", "h2");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "HGET", "h2", "f");
    EXPECT(out, "$1\r\nv\r\n");
    exec_cmd(&d, T0, &out, 2, "TYPE", "h");
    EXPECT(out, "+none\r\n");

    /* WATCH/version bookkeeping: both keys bumped, dirty +2 */
    exec_cmd(&d, T0, &out, 3, "SET", "ws", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "wd", "2");
    d.watch_refs = 1;
    vs = db_key_version(&d, "ws", 2);
    vd = db_key_version(&d, "wd", 2);
    dirty0 = d.dirty;
    exec_cmd(&d, T0, &out, 3, "RENAME", "ws", "wd");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT((long long)(vs + 1),
                    (long long)db_key_version(&d, "ws", 2));
    DD_CHECK_EQ_INT((long long)(vd + 1),
                    (long long)db_key_version(&d, "wd", 2));
    DD_CHECK_EQ_INT((long long)(dirty0 + 2), (long long)d.dirty);
    d.watch_refs = 0;

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_renamenx(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "a", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "b", "2");
    exec_cmd(&d, T0, &out, 3, "RENAMENX", "a", "b");
    EXPECT(out, ":0\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "a");
    EXPECT(out, "$1\r\n1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "b");
    EXPECT(out, "$1\r\n2\r\n");

    exec_cmd(&d, T0, &out, 3, "RENAMENX", "a", "c");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "a");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "c");
    EXPECT(out, "$1\r\n1\r\n");

    /* an expired dst counts as absent */
    exec_cmd(&d, T0, &out, 3, "SET", "d", "dv");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "d", "1000");
    exec_cmd(&d, T0 + 2000, &out, 3, "RENAMENX", "c", "d");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0 + 2000, &out, 2, "GET", "d");
    EXPECT(out, "$1\r\n1\r\n");

    exec_cmd(&d, T0, &out, 3, "RENAMENX", "missing", "x");
    EXPECT(out, "-ERR no such key\r\n");
    exec_cmd(&d, T0, &out, 3, "RENAMENX", "d", "d");
    EXPECT(out,
           "-ERR source and destination objects are the same\r\n");
    exec_cmd(&d, T0, &out, 2, "RENAMENX", "d");
    EXPECT(out,
           "-ERR wrong number of arguments for 'renamenx' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_touch(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "a", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "b", "2");
    exec_cmd(&d, T0, &out, 4, "TOUCH", "a", "b", "c");
    EXPECT(out, ":2\r\n");
    exec_cmd(&d, T0, &out, 2, "TOUCH", "c");
    EXPECT(out, ":0\r\n");

    /* expired keys are not counted */
    exec_cmd(&d, T0, &out, 3, "SET", "e", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "e", "1000");
    exec_cmd(&d, T0 + 2000, &out, 3, "TOUCH", "a", "e");
    EXPECT(out, ":1\r\n");

    exec_cmd(&d, T0, &out, 1, "TOUCH");
    EXPECT(out, "-ERR wrong number of arguments for 'touch' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_randomkey(void)
{
    db d;
    resp_buf out;
    char want[16];
    int i, found;
    db_init(&d);
    resp_buf_init(&out);

    /* empty db: null bulk */
    exec_cmd(&d, T0, &out, 1, "RANDOMKEY");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, T0, &out, 3, "SET", "only", "v");
    exec_cmd(&d, T0, &out, 1, "RANDOMKEY");
    EXPECT(out, "$4\r\nonly\r\n");

    /* with several keys the reply must be one of them */
    exec_cmd(&d, T0, &out, 3, "SET", "k1", "v");
    exec_cmd(&d, T0, &out, 3, "SET", "k2", "v");
    exec_cmd(&d, T0, &out, 3, "SET", "k3", "v");
    found = 0;
    for (i = 0; i < 20; i++) {
        int j;
        exec_cmd(&d, T0, &out, 1, "RANDOMKEY");
        for (j = 0; j < 4; j++) {
            if (j == 0)
                snprintf(want, sizeof(want), "$4\r\nonly\r\n");
            else
                snprintf(want, sizeof(want), "$2\r\nk%d\r\n", j);
            if (out.len == strlen(want) &&
                memcmp(out.data, want, out.len) == 0) {
                found = 1;
                break;
            }
        }
        DD_CHECK(found);
        if (!found)
            break;
    }

    /* a db whose only key is expired yields null (and collects the key) */
    exec_cmd(&d, T0, &out, 1, "FLUSHDB");
    exec_cmd(&d, T0, &out, 3, "SET", "x", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "x", "1000");
    exec_cmd(&d, T0 + 2000, &out, 1, "RANDOMKEY");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, T0 + 2000, &out, 2, "GET", "x");
    EXPECT(out, "$-1\r\n");

    exec_cmd(&d, T0, &out, 2, "RANDOMKEY", "x");
    EXPECT(out,
           "-ERR wrong number of arguments for 'randomkey' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_expiretime(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 2, "EXPIRETIME", "missing");
    EXPECT(out, ":-2\r\n");
    exec_cmd(&d, T0, &out, 2, "PEXPIRETIME", "missing");
    EXPECT(out, ":-2\r\n");

    exec_cmd(&d, T0, &out, 3, "SET", "k", "v");
    exec_cmd(&d, T0, &out, 2, "EXPIRETIME", "k");
    EXPECT(out, ":-1\r\n");
    exec_cmd(&d, T0, &out, 2, "PEXPIRETIME", "k");
    EXPECT(out, ":-1\r\n");

    exec_cmd(&d, T0, &out, 3, "PEXPIREAT", "k", "1500000");
    exec_cmd(&d, T0, &out, 2, "PEXPIRETIME", "k");
    EXPECT(out, ":1500000\r\n");
    exec_cmd(&d, T0, &out, 2, "EXPIRETIME", "k");
    EXPECT(out, ":1500\r\n");

    /* sub-second absolute times truncate (floor) for EXPIRETIME */
    exec_cmd(&d, T0, &out, 3, "PEXPIREAT", "k", "1500123");
    exec_cmd(&d, T0, &out, 2, "EXPIRETIME", "k");
    EXPECT(out, ":1500\r\n");

    /* expired key: -2 */
    exec_cmd(&d, T0, &out, 3, "SET", "e", "v");
    exec_cmd(&d, T0, &out, 3, "PEXPIRE", "e", "1000");
    exec_cmd(&d, T0 + 2000, &out, 2, "EXPIRETIME", "e");
    EXPECT(out, ":-2\r\n");

    exec_cmd(&d, T0, &out, 1, "EXPIRETIME");
    EXPECT(out,
           "-ERR wrong number of arguments for 'expiretime' command\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_type);
    DD_RUN(test_keys);
    DD_RUN(test_scan_full_iteration);
    DD_RUN(test_scan_match);
    DD_RUN(test_scan_errors);
    DD_RUN(test_scan_skips_expired);
    DD_RUN(test_rename);
    DD_RUN(test_renamenx);
    DD_RUN(test_touch);
    DD_RUN(test_randomkey);
    DD_RUN(test_expiretime);
    return DD_TEST_SUMMARY();
}
