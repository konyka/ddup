/* test_tier_db.c - transparent tier offload/materialize integration. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "core/tier.h"
#include "pal/pal_file.h"
#include "test.h"

#define PATH "ddup_tier_db_test.log"

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

#define T0 1000000ULL
#define LATER (T0 + 100000000ULL)

static uint64_t eb(size_t klen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + klen + vlen;
}

static void test_offload_materialize_delete(void)
{
    db d;
    resp_buf out;
    tier_store *tier;
    char maxmem[32];
    uint64_t live_before;

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&tier, PATH, 0));
    db_init(&d);
    db_set_tier(&d, tier, 0);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "victim",
             "0123456789abcdef0123456789abcdef"
             "0123456789abcdef0123456789abcdef");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, LATER, &out, 3, "SET", "k01",
             "0123456789abcdef0123456789abcdef"
             "0123456789abcdef0123456789abcdef");
    exec_cmd(&d, LATER, &out, 3, "SET", "k02",
             "0123456789abcdef0123456789abcdef"
             "0123456789abcdef0123456789abcdef");
    exec_cmd(&d, LATER, &out, 3, "SET", "k03",
             "0123456789abcdef0123456789abcdef"
             "0123456789abcdef0123456789abcdef");
    snprintf(maxmem, sizeof(maxmem), "%llu",
             (unsigned long long)(eb(6, 17) + 3 * eb(3, 65)));
    exec_cmd(&d, LATER, &out, 4, "CONFIG", "SET", "maxmemory", maxmem);
    EXPECT(out, "+OK\r\n");
    DD_CHECK(d.evicted_keys > 0);
    DD_CHECK(d.used_memory <= d.maxmemory);
    DD_CHECK(tier_live_records(tier) >= 1);
    live_before = tier_live_records(tier);

    /* A read must transparently materialize the offloaded value. */
    exec_cmd(&d, LATER, &out, 2, "GET", "victim");
    EXPECT(out,
           "$64\r\n"
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef\r\n");
    DD_CHECK(tier_failed(tier) == 0);

    exec_cmd(&d, LATER, &out, 2, "EXISTS", "victim");
    EXPECT(out, ":1\r\n");

    exec_cmd(&d, LATER, &out, 2, "DEL", "victim");
    EXPECT(out, ":1\r\n");
    DD_CHECK(tier_live_records(tier) <= live_before);

    resp_buf_free(&out);
    db_destroy(&d);
    tier_close(tier);
}

static void test_offloaded_expire(void)
{
    db d;
    resp_buf out;
    tier_store *tier;
    char maxmem[32];

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&tier, PATH, 0));
    db_init(&d);
    db_set_tier(&d, tier, 0);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "exp",
             "0123456789abcdef0123456789abcdef");
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "exp", "100");
    EXPECT(out, ":1\r\n");
    snprintf(maxmem, sizeof(maxmem), "%llu",
             (unsigned long long)(eb(3, 17) + eb(3, 8)));
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory", maxmem);
    DD_CHECK(d.evicted_keys > 0);

    /* Lazy expiration must remove the offloaded record and report missing. */
    exec_cmd(&d, T0 + 101000, &out, 2, "GET", "exp");
    EXPECT(out, "$-1\r\n");
    DD_CHECK_EQ_INT(0, (long long)tier_live_records(tier));

    resp_buf_free(&out);
    db_destroy(&d);
    tier_close(tier);
}

static void test_offload_reopen(void)
{
    db d;
    resp_buf out;
    tier_store *tier;
    tier_store *reopened;
    char maxmem[32];

    pal_file_unlink(PATH);
    DD_CHECK_EQ_INT(0, tier_open(&tier, PATH, 0));
    db_init(&d);
    db_set_tier(&d, tier, 0);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 3, "SET", "cold",
             "0123456789abcdef0123456789abcdef"
             "0123456789abcdef0123456789abcdef");
    snprintf(maxmem, sizeof(maxmem), "%llu",
             (unsigned long long)eb(4, 17));
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory", maxmem);
    DD_CHECK(d.evicted_keys > 0);
    DD_CHECK(tier_live_records(tier) == 1);

    /* Replace the live tier store with a reopened one (replay test). */
    tier_close(tier);
    DD_CHECK_EQ_INT(0, tier_open(&reopened, PATH, 0));
    db_set_tier(&d, reopened, 0);
    exec_cmd(&d, T0, &out, 2, "GET", "cold");
    EXPECT(out,
           "$64\r\n"
           "0123456789abcdef0123456789abcdef"
           "0123456789abcdef0123456789abcdef\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
    tier_close(reopened);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    DD_RUN(test_offload_materialize_delete);
    DD_RUN(test_offloaded_expire);
    DD_RUN(test_offload_reopen);
    pal_file_unlink(PATH);
    return DD_TEST_SUMMARY();
}
