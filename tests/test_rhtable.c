/* test_rhtable.c - Robin Hood hash table tests (written before the impl). */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rhtable.h"
#include "pal/pal_time.h"

static void test_set_get(void)
{
    rh_table t;
    rh_init(&t);
    rh_set(&t, "foo", 3, "bar", 3);
    const char *v;
    size_t vl;
    DD_CHECK(rh_get(&t, "foo", 3, &v, &vl) == 1);
    DD_CHECK_MEM("bar", 3, v, vl);
    DD_CHECK(rh_get(&t, "nope", 4, &v, &vl) == 0);
    DD_CHECK_EQ_INT(1, rh_size(&t));
    rh_destroy(&t);
}

static void test_api_rejects_null_inputs(void)
{
    rh_table t;
    const char *v = NULL;
    size_t vl = 0;
    char *old = NULL;
    size_t old_len = 0;
    rh_init(&t);
    rh_init(NULL);
    rh_destroy(NULL);
    DD_CHECK_EQ_INT(0, rh_get(NULL, "k", 1, &v, &vl));
    DD_CHECK_EQ_INT(0, rh_get(&t, NULL, 1, &v, &vl));
    DD_CHECK_EQ_INT(-1, rh_set(NULL, "k", 1, "v", 1));
    DD_CHECK_EQ_INT(-1, rh_set(&t, NULL, 1, "v", 1));
    DD_CHECK_EQ_INT(-1, rh_set(&t, "k", 1, NULL, 1));
    DD_CHECK_EQ_INT(-1, rh_set_ex2(&t, "k", 1, NULL, 1, NULL, 0, 0,
                                   &old, &old_len));
    DD_CHECK_EQ_INT(0, rh_del(NULL, "k", 1));
    DD_CHECK_EQ_INT(0, rh_touch(NULL, "k", 1, 0));
    DD_CHECK_EQ_INT(0, rh_meta_of(NULL, "k", 1));
    DD_CHECK_EQ_INT(0, rh_size(NULL));
    rh_destroy(&t);
}

static void test_iteration_api_rejects_null_inputs(void)
{
    const char *key = NULL;
    const char *val = NULL;
    size_t klen = 0, vlen = 0;
    DD_CHECK_EQ_INT(0, (long long)rh_scan(NULL, 0, 1, NULL, NULL));
    rh_each(NULL, NULL, NULL);
    DD_CHECK_EQ_INT(0, rh_random_entry(NULL, 0, &key, &klen, &val, &vlen,
                                       NULL));
}

static void test_overwrite(void)
{
    rh_table t;
    rh_init(&t);
    rh_set(&t, "k", 1, "v1", 2);
    rh_set(&t, "k", 1, "v2-longer", 9);
    const char *v;
    size_t vl;
    DD_CHECK(rh_get(&t, "k", 1, &v, &vl) == 1);
    DD_CHECK_MEM("v2-longer", 9, v, vl);
    DD_CHECK_EQ_INT(1, rh_size(&t)); /* overwrite must not grow size */
    rh_destroy(&t);
}

static void test_set_ex(void)
{
    rh_table t;
    const char *v;
    size_t vl;
    char *old_kv;
    size_t old_vlen;

    rh_init(&t);
    /* insert: no old block, meta lands on the new entry */
    DD_CHECK_EQ_INT(0, rh_set_ex(&t, "k", 1, "v1", 2, 42, &old_kv,
                                 &old_vlen));
    DD_CHECK_EQ_INT(1, rh_size(&t));
    DD_CHECK(rh_get(&t, "k", 1, &v, &vl) == 1);
    DD_CHECK_MEM("v1", 2, v, vl);
    DD_CHECK_EQ_INT(42, (int)rh_meta_of(&t, "k", 1));

    /* overwrite: old block comes back unfreed, entry keeps the key */
    DD_CHECK_EQ_INT(1, rh_set_ex(&t, "k", 1, "v2-longer", 9, 77, &old_kv,
                                 &old_vlen));
    DD_CHECK_EQ_INT(1, rh_size(&t));
    DD_CHECK_EQ_INT(2, (long long)old_vlen);
    DD_CHECK_MEM("v1", 2, old_kv + 1, old_vlen);
    free(old_kv);
    DD_CHECK(rh_get(&t, "k", 1, &v, &vl) == 1);
    DD_CHECK_MEM("v2-longer", 9, v, vl);
    DD_CHECK_EQ_INT(77, (int)rh_meta_of(&t, "k", 1));

    /* table stays consistent across delete + reinsert */
    DD_CHECK_EQ_INT(1, rh_del(&t, "k", 1));
    DD_CHECK_EQ_INT(0, rh_set_ex(&t, "k", 1, "x", 1, 1, &old_kv, &old_vlen));
    DD_CHECK(rh_get(&t, "k", 1, &v, &vl) == 1);
    DD_CHECK_MEM("x", 1, v, vl);
    rh_destroy(&t);
}

static void test_reject_unrepresentable_lengths(void)
{
    rh_table t;
    const char byte = 'x';
    const char *v;
    size_t vl;
    char *old_kv = (char *)&byte;
    size_t old_vlen = 123;

    rh_init(&t);
    DD_CHECK_EQ_INT(0, rh_set(&t, "k", 1, "old", 3));

#if SIZE_MAX > UINT32_MAX
    DD_CHECK_EQ_INT(-1,
                    rh_set(&t, &byte, (size_t)UINT32_MAX + 1, &byte, 1));
    DD_CHECK_EQ_INT(-1,
                    rh_set(&t, &byte, 1, &byte, (size_t)UINT32_MAX + 1));
    DD_CHECK_EQ_INT(-1,
                    rh_set_ex(&t, &byte, (size_t)UINT32_MAX + 1, &byte, 1,
                              0, &old_kv, &old_vlen));
#endif
    DD_CHECK_EQ_INT(-1,
                    rh_set_ex2(&t, &byte, 1, &byte, SIZE_MAX, &byte, 1, 0,
                               &old_kv, &old_vlen));
    DD_CHECK_EQ_INT(-1,
                    rh_set_ex2(&t, &byte, 1, &byte, UINT32_MAX, &byte, 1, 0,
                               &old_kv, &old_vlen));
    DD_CHECK_EQ_INT(-1, rh_set(&t, &byte, SIZE_MAX, &byte, 1));

    DD_CHECK(old_kv == &byte);
    DD_CHECK_EQ_INT(123, (long long)old_vlen);
    DD_CHECK_EQ_INT(1, rh_size(&t));
    DD_CHECK(rh_get(&t, "k", 1, &v, &vl) == 1);
    DD_CHECK_MEM("old", 3, v, vl);
    rh_destroy(&t);
}

static void test_capacity_arithmetic_overflow(void)
{
    size_t bytes = 123;
    size_t cap = 123;
    DD_CHECK(rh_test_slot_bytes(SIZE_MAX, &bytes) == -1);
    DD_CHECK_EQ_INT(123, (long long)bytes);
    DD_CHECK(rh_test_grow_capacity(SIZE_MAX, &cap) == -1);
    DD_CHECK_EQ_INT(123, (long long)cap);
    DD_CHECK(rh_test_grow_capacity(16, &cap) == 0);
    DD_CHECK_EQ_INT(32, (long long)cap);
}

static void test_cached_growth_threshold(void)
{
    rh_table t;
    char key[16];
    int i;

    rh_init(&t);
    DD_CHECK_EQ_INT(13, (long long)t.grow_at);
    for (i = 0; i < 13; i++) {
        int n = snprintf(key, sizeof(key), "k%d", i);
        DD_CHECK_EQ_INT(0, rh_set(&t, key, (size_t)n, "v", 1));
    }
    DD_CHECK_EQ_INT(16, (long long)t.cap);
    DD_CHECK_EQ_INT(13, (long long)t.grow_at);
    DD_CHECK_EQ_INT(0, rh_set(&t, "k13", 3, "v", 1));
    DD_CHECK_EQ_INT(32, (long long)t.cap);
    DD_CHECK_EQ_INT(27, (long long)t.grow_at);
    rh_destroy(&t);
}

static void test_delete(void)
{
    rh_table t;
    rh_init(&t);
    rh_set(&t, "a", 1, "1", 1);
    rh_set(&t, "b", 1, "2", 1);
    DD_CHECK(rh_del(&t, "a", 1) == 1);
    DD_CHECK(rh_del(&t, "a", 1) == 0); /* already gone */
    const char *v;
    size_t vl;
    DD_CHECK(rh_get(&t, "a", 1, &v, &vl) == 0);
    DD_CHECK(rh_get(&t, "b", 1, &v, &vl) == 1);
    DD_CHECK_EQ_INT(1, rh_size(&t));
    rh_destroy(&t);
}

static void test_binary_keys_values(void)
{
    rh_table t;
    rh_init(&t);
    const char key[] = {'k', '\0', 'y'};
    const char val[] = {'\0', '\r', '\n', '\0'};
    rh_set(&t, key, sizeof(key), val, sizeof(val));
    const char *v;
    size_t vl;
    DD_CHECK(rh_get(&t, key, sizeof(key), &v, &vl) == 1);
    DD_CHECK_MEM(val, sizeof(val), v, vl);
    rh_destroy(&t);
}

static void test_many_keys_growth(void)
{
    /* Forces multiple doublings incl. incremental migration; all lookups must
     * succeed at every point (mid-migration included). */
    rh_table t;
    rh_init(&t);
    char key[32], val[32];
    const int N = 100000;
    for (int i = 0; i < N; i++) {
        int kl = snprintf(key, sizeof(key), "key:%d", i);
        int vl2 = snprintf(val, sizeof(val), "val:%d", i);
        rh_set(&t, key, (size_t)kl, val, (size_t)vl2);
        /* verify a recent and an old key mid-growth */
        const char *v;
        size_t vl;
        DD_CHECK(rh_get(&t, key, (size_t)kl, &v, &vl) == 1);
        DD_CHECK(rh_get(&t, "key:0", 5, &v, &vl) == 1);
    }
    DD_CHECK_EQ_INT(N, rh_size(&t));
    for (int i = 0; i < N; i++) {
        int kl = snprintf(key, sizeof(key), "key:%d", i);
        const char *v;
        size_t vl;
        if (rh_get(&t, key, (size_t)kl, &v, &vl) != 1) {
            DD_CHECK(0 && "missing key after growth");
            break;
        }
        char want[32];
        int wl = snprintf(want, sizeof(want), "val:%d", i);
        DD_CHECK_MEM(want, (size_t)wl, v, vl);
    }
    rh_destroy(&t);
}

static void test_delete_half_then_verify(void)
{
    rh_table t;
    rh_init(&t);
    char key[32];
    const int N = 20000;
    for (int i = 0; i < N; i++) {
        int kl = snprintf(key, sizeof(key), "k%d", i);
        rh_set(&t, key, (size_t)kl, "v", 1);
    }
    for (int i = 0; i < N; i += 2) {
        int kl = snprintf(key, sizeof(key), "k%d", i);
        DD_CHECK(rh_del(&t, key, (size_t)kl) == 1);
    }
    DD_CHECK_EQ_INT(N / 2, rh_size(&t));
    for (int i = 0; i < N; i++) {
        int kl = snprintf(key, sizeof(key), "k%d", i);
        const char *v;
        size_t vl;
        int found = rh_get(&t, key, (size_t)kl, &v, &vl);
        DD_CHECK(found == (i % 2));
    }
    rh_destroy(&t);
}

/* Naive reference model for the randomized differential test. */
typedef struct {
    char key[32];
    int klen;
    char val[32];
    int vlen;
    int present;
} model_entry;

static unsigned long long g_rng = 0x9E3779B97F4A7C15ULL;
static unsigned rng_next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (unsigned)(g_rng >> 32);
}

static void test_randomized_vs_model(void)
{
    rh_table t;
    rh_init(&t);
    static model_entry model[512];
    memset(model, 0, sizeof(model));

    for (int iter = 0; iter < 60000; iter++) {
        unsigned slot = rng_next() % 512;
        model_entry *m = &model[slot];
        unsigned op = rng_next() % 100;
        const char *v;
        size_t vl;
        if (op < 45) { /* set */
            m->klen = snprintf(m->key, sizeof(m->key), "u%u", slot);
            m->vlen = snprintf(m->val, sizeof(m->val), "%u", iter);
            m->present = 1;
            rh_set(&t, m->key, (size_t)m->klen, m->val, (size_t)m->vlen);
        } else if (op < 70) { /* delete */
            m->klen = snprintf(m->key, sizeof(m->key), "u%u", slot);
            int got = rh_del(&t, m->key, (size_t)m->klen);
            DD_CHECK(got == m->present);
            m->present = 0;
        } else { /* get */
            m->klen = snprintf(m->key, sizeof(m->key), "u%u", slot);
            int got = rh_get(&t, m->key, (size_t)m->klen, &v, &vl);
            DD_CHECK(got == m->present);
            if (got && m->present)
                DD_CHECK_MEM(m->val, (size_t)m->vlen, v, vl);
        }
    }
    /* final size cross-check */
    size_t expect = 0;
    for (int i = 0; i < 512; i++)
        expect += model[i].present ? 1u : 0u;
    DD_CHECK_EQ_INT((long long)expect, (long long)rh_size(&t));
    rh_destroy(&t);
}

/* ------------------------------------------------------------------ */
/* rh_scan: cursor-based iteration over both tables                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int seen[512];
    size_t visited;
    int stop_after_first;
} scan_ctx;

static int scan_cb(const char *key, size_t klen, const char *val, size_t vlen,
                   void *ctx)
{
    scan_ctx *c = (scan_ctx *)ctx;
    int idx;
    (void)val;
    (void)vlen;
    c->visited++;
    if (klen >= 2 && key[0] == 's') {
        idx = atoi(key + 1);
        if (idx >= 0 && idx < 512)
            c->seen[idx]++;
    }
    return c->stop_after_first;
}

static int scan_all(rh_table *t, size_t count_hint, scan_ctx *c)
{
    size_t cursor = 0;
    int rounds = 0;
    memset(c, 0, sizeof(*c));
    do {
        cursor = rh_scan(t, cursor, count_hint, scan_cb, c);
        if (++rounds > 100000)
            return -1; /* no convergence */
    } while (cursor != 0);
    return rounds;
}

static void test_scan_basic(void)
{
    rh_table t;
    scan_ctx c;
    char key[32];
    int i;

    rh_init(&t);
    for (i = 0; i < 100; i++) {
        int kl = snprintf(key, sizeof(key), "s%d", i);
        rh_set(&t, key, (size_t)kl, "v", 1);
    }
    /* count >= size: a single call covers the table and reports done */
    c.stop_after_first = 0;
    memset(&c, 0, sizeof(c));
    DD_CHECK_EQ_INT(0, (long long)rh_scan(&t, 0, 1000, scan_cb, &c));
    DD_CHECK_EQ_INT(100, (long long)c.visited);
    for (i = 0; i < 100; i++)
        DD_CHECK_EQ_INT(1, c.seen[i]);
    rh_destroy(&t);
}

static void test_scan_batches(void)
{
    rh_table t;
    scan_ctx c;
    char key[32];
    int i;

    rh_init(&t);
    for (i = 0; i < 100; i++) {
        int kl = snprintf(key, sizeof(key), "s%d", i);
        rh_set(&t, key, (size_t)kl, "v", 1);
    }
    /* small batches: converges, no mutation -> every key exactly once */
    DD_CHECK(scan_all(&t, 7, &c) > 1);
    DD_CHECK_EQ_INT(100, (long long)c.visited);
    for (i = 0; i < 100; i++)
        DD_CHECK_EQ_INT(1, c.seen[i]);

    /* early stop via callback return value */
    memset(&c, 0, sizeof(c));
    c.stop_after_first = 1;
    DD_CHECK(rh_scan(&t, 0, 1000, scan_cb, &c) != 0);
    DD_CHECK_EQ_INT(1, (long long)c.visited);
    rh_destroy(&t);
}

static void test_scan_empty_table(void)
{
    rh_table t;
    scan_ctx c;
    rh_init(&t);
    memset(&c, 0, sizeof(c));
    DD_CHECK_EQ_INT(0, (long long)rh_scan(&t, 0, 10, scan_cb, &c));
    DD_CHECK_EQ_INT(0, (long long)c.visited);
    rh_destroy(&t);
}

static void test_scan_covers_both_tables_mid_rehash(void)
{
    /* Insert until an incremental rehash is in flight (old table live),
     * then verify a full scan sees every key exactly once. */
    rh_table t;
    scan_ctx c;
    char key[32];
    int n = 0;
    int i;

    rh_init(&t);
    for (i = 0; i < 400; i++) {
        int kl = snprintf(key, sizeof(key), "s%d", i);
        rh_set(&t, key, (size_t)kl, "v", 1);
        n++;
        if (t.old_slots != NULL && t.old_live > 0)
            break;
    }
    DD_CHECK(t.old_slots != NULL); /* rehash really in flight */
    DD_CHECK(n < 400);
    memset(&c, 0, sizeof(c));
    DD_CHECK_EQ_INT(0, (long long)rh_scan(&t, 0, 100000, scan_cb, &c));
    DD_CHECK_EQ_INT(n, (long long)c.visited);
    for (i = 0; i < n; i++)
        DD_CHECK_EQ_INT(1, c.seen[i]);
    rh_destroy(&t);
}

static int scan_del_cb(const char *key, size_t klen, const char *val,
                       size_t vlen, void *ctx)
{
    rh_table *t = (rh_table *)ctx;
    (void)val;
    (void)vlen;
    rh_del(t, key, klen); /* mutating callback: must stay memory-safe */
    return 0;
}

static void test_scan_with_deleting_callback(void)
{
    rh_table t;
    char key[32];
    int i;

    rh_init(&t);
    for (i = 0; i < 200; i++) {
        int kl = snprintf(key, sizeof(key), "s%d", i);
        rh_set(&t, key, (size_t)kl, "v", 1);
    }
    /* Deleting the visited entry backward-shifts neighbours, so some keys
     * are skipped; repeated full passes must converge to an empty table
     * without crashing or corrupting it. */
    for (int pass = 0; pass < 1000 && rh_size(&t) > 0; pass++) {
        size_t cursor = 0;
        do {
            cursor = rh_scan(&t, cursor, 8, scan_del_cb, &t);
        } while (cursor != 0);
    }
    DD_CHECK_EQ_INT(0, (long long)rh_size(&t));
    rh_destroy(&t);
}

static void bench_throughput(void)
{
    /* Not an assertion: prints ops/sec for docs/performance.md. */
    rh_table t;
    rh_init(&t);
    char key[32];
    const int N = 1000000;
    uint64_t t0 = pal_now_ms();
    for (int i = 0; i < N; i++) {
        int kl = snprintf(key, sizeof(key), "bench:%d", i);
        rh_set(&t, key, (size_t)kl, "v", 1);
    }
    uint64_t t1 = pal_now_ms();
    const char *v;
    size_t vl;
    for (int i = 0; i < N; i++) {
        int kl = snprintf(key, sizeof(key), "bench:%d", i);
        if (rh_get(&t, key, (size_t)kl, &v, &vl) != 1) {
            fprintf(stderr, "bench: missing key\n");
            break;
        }
    }
    uint64_t t2 = pal_now_ms();
    double set_s = (double)(t1 - t0) / 1000.0;
    double get_s = (double)(t2 - t1) / 1000.0;
    printf("bench: %d SET in %.3fs (%.0f ops/s), %d GET in %.3fs (%.0f ops/s)\n",
           N, set_s, set_s > 0 ? N / set_s : 0,
           N, get_s, get_s > 0 ? N / get_s : 0);
    dd_test_checks++;
    rh_destroy(&t);
}

int main(void)
{
    DD_RUN(test_capacity_arithmetic_overflow);
    DD_RUN(test_cached_growth_threshold);
    DD_RUN(test_set_get);
    DD_RUN(test_api_rejects_null_inputs);
    DD_RUN(test_iteration_api_rejects_null_inputs);
    DD_RUN(test_overwrite);
    DD_RUN(test_set_ex);
    DD_RUN(test_reject_unrepresentable_lengths);
    DD_RUN(test_delete);
    DD_RUN(test_binary_keys_values);
    DD_RUN(test_many_keys_growth);
    DD_RUN(test_delete_half_then_verify);
    DD_RUN(test_randomized_vs_model);
    DD_RUN(test_scan_basic);
    DD_RUN(test_scan_batches);
    DD_RUN(test_scan_empty_table);
    DD_RUN(test_scan_covers_both_tables_mid_rehash);
    DD_RUN(test_scan_with_deleting_callback);
    DD_RUN(bench_throughput);
    return DD_TEST_SUMMARY();
}
