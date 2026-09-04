/* test_tier.c - append-only cold log roundtrip, replay, flush, compaction. */
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/tier.h"
#include "pal/pal_file.h"

#define PATH "ddup_tier_test.log"

static void test_put_get_delete(void)
{
    tier_store *t;
    uint64_t rid;
    char *val = NULL;
    size_t vlen = 0;
    uint64_t expire = 0;
    const char payload[] = {'\x06', 'h', 'e', '\0', 'l', 'l', 'o'};

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(0, tier_put(t, 3, "k", 1, payload, sizeof(payload),
                                12345, &rid));
    DD_CHECK(rid != 0);
    DD_CHECK_EQ_INT(1, (long long)tier_live_records(t));
    DD_CHECK(tier_disk_bytes(t) >= sizeof(payload));

    DD_CHECK_EQ_INT(0, tier_get(t, rid, &val, &vlen, &expire));
    DD_CHECK_MEM(payload, sizeof(payload), val, vlen);
    DD_CHECK_EQ_INT(12345, (long long)expire);
    free(val);
    val = NULL;

    DD_CHECK_EQ_INT(0, tier_del(t, rid));
    DD_CHECK_EQ_INT(0, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(-1, tier_get(t, rid, &val, &vlen, NULL));
    tier_close(t);
}

static void test_flush_and_reopen(void)
{
    tier_store *t;
    uint64_t a, b;
    char *val = NULL;
    size_t vlen = 0;

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(0, tier_put(t, 0, "a", 1, "1", 1, 0, &a));
    DD_CHECK_EQ_INT(0, tier_put(t, 0, "b", 1, "2", 1, 0, &b));
    DD_CHECK_EQ_INT(0, tier_flush_db(t, 0));
    DD_CHECK_EQ_INT(0, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(-1, tier_get(t, a, &val, &vlen, NULL));
    tier_close(t);

    /* Reopening must replay the FLUSH marker. */
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(0, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(-1, tier_get(t, b, &val, &vlen, NULL));
    tier_close(t);
}

static void test_replay_put(void)
{
    tier_store *t;
    uint64_t rid;
    char *val = NULL;
    size_t vlen = 0;
    uint64_t expire = 0;

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(0, tier_put(t, 0, "key", 3, "\x00v1", 3, 999, &rid));
    tier_close(t);

    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(1, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(0, tier_get(t, rid, &val, &vlen, &expire));
    DD_CHECK_MEM("\x00v1", 3, val, vlen);
    DD_CHECK_EQ_INT(999, (long long)expire);
    free(val);
    tier_close(t);
}

static void test_flush_db_keeps_other_db(void)
{
    tier_store *t;
    uint64_t a, b;
    char *val = NULL;
    size_t vlen = 0;

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(0, tier_put(t, 0, "a", 1, "1", 1, 0, &a));
    DD_CHECK_EQ_INT(0, tier_put(t, 1, "b", 1, "2", 1, 0, &b));
    DD_CHECK_EQ_INT(0, tier_flush_db(t, 0));
    DD_CHECK_EQ_INT(1, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(-1, tier_get(t, a, &val, &vlen, NULL));
    DD_CHECK_EQ_INT(0, tier_get(t, b, &val, &vlen, NULL));
    DD_CHECK_MEM("2", 1, val, vlen);
    free(val);
    val = NULL;
    tier_close(t);

    /* Per-db flush must replay correctly. */
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(1, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(-1, tier_get(t, a, &val, &vlen, NULL));
    DD_CHECK_EQ_INT(0, tier_get(t, b, &val, &vlen, NULL));
    DD_CHECK_MEM("2", 1, val, vlen);
    free(val);
    tier_close(t);
}

static void test_compact_keeps_live(void)
{
    tier_store *t;
    uint64_t dead, live;
    char *val = NULL;
    size_t vlen = 0;
    uint64_t before;

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(0, tier_put(t, 0, "dead", 4, "gone", 4, 0, &dead));
    DD_CHECK_EQ_INT(0, tier_put(t, 0, "live", 4, "kept", 4, 0, &live));
    DD_CHECK_EQ_INT(0, tier_del(t, dead));
    before = tier_disk_bytes(t);
    DD_CHECK_EQ_INT(0, tier_compact(t));
    DD_CHECK(tier_disk_bytes(t) < before);
    DD_CHECK_EQ_INT(1, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(0, tier_get(t, live, &val, &vlen, NULL));
    DD_CHECK_MEM("kept", 4, val, vlen);
    free(val);
    DD_CHECK_EQ_INT(-1, tier_get(t, dead, &val, &vlen, NULL));
    tier_close(t);

    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 0));
    DD_CHECK_EQ_INT(1, (long long)tier_live_records(t));
    DD_CHECK_EQ_INT(0, tier_get(t, live, &val, &vlen, NULL));
    DD_CHECK_MEM("kept", 4, val, vlen);
    free(val);
    tier_close(t);
}

static void test_disk_limit(void)
{
    tier_store *t;
    uint64_t rid;
    const char val[64] = {0};

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&t, PATH, 64));
    /* Header + value exceeds the tiny cap. */
    DD_CHECK_EQ_INT(-1, tier_put(t, 0, "k", 1, val, sizeof(val), 0, &rid));
    DD_CHECK_EQ_INT(0, (long long)tier_live_records(t));
    tier_close(t);
}

static void test_tier_api_rejects_null_inputs(void)
{
    tier_store *t = NULL;
    uint64_t rid;
    char *val;
    size_t vlen;
    DD_CHECK_EQ_INT(-1, tier_open(NULL, PATH, 0));
    DD_CHECK_EQ_INT(-1, tier_open(&t, NULL, 0));
    DD_CHECK_EQ_INT(-1, tier_open(&t, "", 0));
    DD_CHECK_EQ_INT(-1, tier_put(NULL, 0, "k", 1, "v", 1, 0, &rid));
    DD_CHECK_EQ_INT(-1, tier_put(t, 0, NULL, 1, "v", 1, 0, &rid));
    DD_CHECK_EQ_INT(-1, tier_put(t, 0, "k", 1, NULL, 1, 0, &rid));
    DD_CHECK_EQ_INT(-1, tier_get(NULL, 1, &val, &vlen, NULL));
    DD_CHECK_EQ_INT(-1, tier_del(NULL, 1));
    DD_CHECK_EQ_INT(-1, tier_flush_db(NULL, 0));
    DD_CHECK_EQ_INT(-1, tier_compact(NULL));
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_put_get_delete);
    DD_RUN(test_flush_and_reopen);
    DD_RUN(test_replay_put);
    DD_RUN(test_flush_db_keeps_other_db);
    DD_RUN(test_compact_keeps_live);
    DD_RUN(test_disk_limit);
    DD_RUN(test_tier_api_rejects_null_inputs);
    pal_file_unlink(PATH);
    return DD_TEST_SUMMARY();
}
