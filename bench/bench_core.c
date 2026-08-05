/* bench_core.c - in-process hot-path benchmark (no sockets).
 *
 * Times the server-side CPU path deterministically: pregenerated pipelined
 * RESP buffers are parsed and dispatched through a real session, exactly
 * like the conn loop in server.c. Used to A/B CPU-side optimizations.
 *
 * Usage: bench_core [n_commands]   (default 200000 per phase)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/buf_pool.h"
#include "core/command.h"
#include "core/session.h"
#include "pal/pal_time.h"
#include "resp/resp_parser.h"
#include "resp/resp_writer.h"

#define T0 1000000ULL

/* Append one RESP command to buf: *argc, all args as bulk strings. */
static void build_cmd(resp_buf *b, const char *c1, const char *c2,
                      const char *c3)
{
    int argc = c3 != NULL ? 3 : (c2 != NULL ? 2 : 1);
    char tmp[128];
    resp_write_array_header(b, (size_t)argc);
    snprintf(tmp, sizeof(tmp), "$%zu\r\n%s\r\n", strlen(c1), c1);
    resp_buf_reserve(b, strlen(tmp));
    memcpy(b->data + b->len, tmp, strlen(tmp));
    b->len += strlen(tmp);
    if (argc >= 2) {
        snprintf(tmp, sizeof(tmp), "$%zu\r\n%s\r\n", strlen(c2), c2);
        resp_buf_reserve(b, strlen(tmp));
        memcpy(b->data + b->len, tmp, strlen(tmp));
        b->len += strlen(tmp);
    }
    if (argc >= 3) {
        snprintf(tmp, sizeof(tmp), "$%zu\r\n%s\r\n", strlen(c3), c3);
        resp_buf_reserve(b, strlen(tmp));
        memcpy(b->data + b->len, tmp, strlen(tmp));
        b->len += strlen(tmp);
    }
}

/* Benchmark cmd_resolve() over a mixed command name workload. */
static double bench_cmd_resolve(long n)
{
    static const char *names[] = {"GET",   "SET",    "PING",   "HGET",
                                  "HSET",  "LPUSH",  "ZADD",   "CLUSTER",
                                  "FOOCMD"};
    const int nnames = (int)(sizeof(names) / sizeof(names[0]));
    size_t lens[9];
    int i;
    volatile uint16_t sink = 0;
    uint64_t t0, t1;
    long k;

    for (i = 0; i < nnames; i++)
        lens[i] = strlen(names[i]);

    t0 = pal_now_us();
    for (k = 0; k < n; k++) {
        int idx = (int)(k % nnames);
        sink += cmd_resolve(names[idx], lens[idx]);
    }
    t1 = pal_now_us();
    (void)sink;
    return (t1 > t0) ? (double)n / ((double)(t1 - t0) / 1000000.0)
                     : (double)n;
}

/* Benchmark buf_pool get/put for the 64 KiB tier. */
static double bench_buf_pool(long n)
{
    buf_pool pool;
    size_t sz;
    void *p;
    long k;
    uint64_t t0, t1;

    buf_pool_init(&pool);
    t0 = pal_now_us();
    for (k = 0; k < n; k++) {
        p = buf_pool_get(&pool, 64 * 1024, &sz);
        buf_pool_put(&pool, p, sz);
    }
    t1 = pal_now_us();
    buf_pool_destroy(&pool);
    return (t1 > t0) ? (double)n / ((double)(t1 - t0) / 1000000.0)
                     : (double)n;
}

/* Parse-only phase: isolate parser cost from command dispatch/storage. */
static double run_parse_phase(arena *ar, const char *buf, size_t len)
{
    size_t off = 0;
    long long n = 0;
    uint64_t t0 = pal_now_us();
    while (off < len) {
        resp_value v;
        ptrdiff_t used = resp_parse(buf + off, len - off, &v, ar);
        if (used <= 0) {
            fprintf(stderr, "parse error at %zu\n", off);
            exit(1);
        }
        arena_reset(ar);
        off += (size_t)used;
        n++;
    }
    {
        uint64_t t1 = pal_now_us();
        double secs = (double)(t1 - t0) / 1000000.0;
        return secs > 0.0 ? (double)n / secs : (double)n;
    }
}

/* Run every command in the buffer through parse+dispatch; return ops/sec. */
static double run_phase(session *s, arena *ar, resp_buf *out,
                        const char *buf, size_t len)
{
    size_t off = 0;
    long long n = 0;
    uint64_t t0 = pal_now_us();
    while (off < len) {
        resp_value v;
        ptrdiff_t used = resp_parse(buf + off, len - off, &v, ar);
        if (used <= 0) {
            fprintf(stderr, "parse error at %zu\n", off);
            exit(1);
        }
        out->len = 0;
        session_execute_at(s, v.items, v.count, out, T0);
        arena_reset(ar);
        off += (size_t)used;
        n++;
    }
    {
        uint64_t t1 = pal_now_us();
        double secs = (double)(t1 - t0) / 1000000.0;
        return secs > 0.0 ? (double)n / secs : (double)n;
    }
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 200000;
    resp_buf sets, gets, out;
    db d;
    session *s;
    arena ar;
    long i;
    double set_ops, get_ops, get2_ops;

    resp_buf_init(&sets);
    resp_buf_init(&gets);
    resp_buf_init(&out);
    db_init(&d);
    s = session_create(&d);
    arena_init(&ar, 4096);

    for (i = 0; i < n; i++) {
        char key[32];
        snprintf(key, sizeof(key), "bench:%08ld", i);
        build_cmd(&sets, "SET", key, "0123456789abcdef");
        build_cmd(&gets, "GET", key, NULL);
    }

    /* warmup: fills the db with n keys */
    set_ops = run_phase(s, &ar, &out, sets.data, sets.len);
    get_ops = run_phase(s, &ar, &out, gets.data, gets.len);
    get2_ops = run_phase(s, &ar, &out, gets.data, gets.len);

    printf("bench_core: %ld commands/phase, db=%zu keys\n", n,
           rh_size(&d.table));
    printf("SET (cold, includes inserts): %12.0f ops/s\n", set_ops);
    printf("GET (warm):                   %12.0f ops/s\n", get_ops);
    printf("GET (warm, run 2):            %12.0f ops/s\n", get2_ops);
    printf("parse-only SET:               %12.0f ops/s\n",
           run_parse_phase(&ar, sets.data, sets.len));
    printf("parse-only GET:               %12.0f ops/s\n",
           run_parse_phase(&ar, gets.data, gets.len));
    printf("cmd_resolve (mixed):          %12.0f ops/s\n",
           bench_cmd_resolve(n));
    printf("buf_pool get/put (64 KiB):    %12.0f ops/s\n",
           bench_buf_pool(n));

    session_free(s);
    db_destroy(&d);
    arena_destroy(&ar);
    resp_buf_free(&sets);
    resp_buf_free(&gets);
    resp_buf_free(&out);
    return 0;
}
