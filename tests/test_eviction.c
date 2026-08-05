/* test_eviction.c - maxmemory accounting, allkeys-lru eviction, CONFIG/INFO.
 *
 * Deterministic: synthetic injected time controls the 24-bit LRU clocks and
 * db.rng_state (xorshift32) is fixed by db_init.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core/command.h"
#include "test.h"

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
#define LATER (T0 + 100000000ULL) /* +100000 s: newer LRU clock */

/* per-entry accounting: sizeof(rh_entry) + malloc overhead + key + value */
static uint64_t eb(size_t klen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + klen + vlen;
}

/* string values are stored as {1-byte type tag}{payload} since Phase 5.1 */
static uint64_t ebs(size_t klen, size_t slen)
{
    return eb(klen, slen + 1);
}

static void test_memory_accounting(void)
{
    db d;
    resp_buf out;
    db_init(&d);
    resp_buf_init(&out);

    DD_CHECK_EQ_INT(0, (long long)d.used_memory);
    exec_cmd(&d, T0, &out, 3, "SET", "k", "v");
    EXPECT(out, "+OK\r\n");
    DD_CHECK((long long)d.used_memory == (long long)ebs(1, 1));

    /* overwrite adjusts instead of accumulating */
    exec_cmd(&d, T0, &out, 3, "SET", "k", "vvvvv");
    EXPECT(out, "+OK\r\n");
    DD_CHECK((long long)d.used_memory == (long long)ebs(1, 5));

    /* expiry entry is accounted too */
    exec_cmd(&d, T0, &out, 3, "EXPIRE", "k", "100");
    EXPECT(out, ":1\r\n");
    DD_CHECK((long long)d.used_memory == (long long)(ebs(1, 5) + eb(1, 8)));

    /* touch (GET) must not change accounting */
    exec_cmd(&d, T0, &out, 2, "GET", "k");
    EXPECT(out, "$5\r\nvvvvv\r\n");
    DD_CHECK((long long)d.used_memory == (long long)(ebs(1, 5) + eb(1, 8)));

    exec_cmd(&d, T0, &out, 2, "PERSIST", "k");
    EXPECT(out, ":1\r\n");
    DD_CHECK((long long)d.used_memory == (long long)ebs(1, 5));

    exec_cmd(&d, T0, &out, 2, "DEL", "k");
    EXPECT(out, ":1\r\n");
    DD_CHECK_EQ_INT(0, (long long)d.used_memory);

    exec_cmd(&d, T0, &out, 3, "SET", "a", "1");
    exec_cmd(&d, T0, &out, 3, "SET", "b", "2");
    exec_cmd(&d, T0, &out, 1, "FLUSHDB");
    EXPECT(out, "+OK\r\n");
    DD_CHECK_EQ_INT(0, (long long)d.used_memory);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_allkeys_lru_eviction(void)
{
    db d;
    resp_buf out;
    char name[16];
    char maxmem[32];
    int i;
    db_init(&d);
    resp_buf_init(&out);

    /* victim gets the oldest LRU clock; the kXX keys are touched later */
    exec_cmd(&d, T0, &out, 3, "SET", "victim", "x");
    EXPECT(out, "+OK\r\n");
    for (i = 0; i < 9; i++) {
        snprintf(name, sizeof(name), "k%02d", i);
        exec_cmd(&d, LATER, &out, 3, "SET", name, "x");
    }
    /* 1 * eb(6,1) + 9 * eb(3,1) bytes in use; cap at ~2 entries so the
     * eviction loop must remove most of them */
    snprintf(maxmem, sizeof(maxmem), "%llu", (unsigned long long)(2 * ebs(3, 1)));
    exec_cmd(&d, LATER, &out, 3, "CONFIG", "SET", "maxmemory");
    EXPECT(out, "-ERR wrong number of arguments for 'config' command\r\n");
    exec_cmd(&d, LATER, &out, 4, "CONFIG", "SET", "maxmemory", maxmem);
    EXPECT(out, "+OK\r\n");

    /* eviction already ran at the end of CONFIG SET */
    DD_CHECK((long long)d.evicted_keys >= 5);
    DD_CHECK(d.used_memory <= d.maxmemory);
    DD_CHECK(d.used_memory > 0);

    /* the oldest-touched key was evicted */
    exec_cmd(&d, LATER, &out, 2, "GET", "victim");
    EXPECT(out, "$-1\r\n");
    exec_cmd(&d, LATER, &out, 2, "EXISTS", "victim");
    EXPECT(out, ":0\r\n");

    /* further writes keep the cap */
    exec_cmd(&d, LATER, &out, 3, "SET", "newkey", "x");
    EXPECT(out, "+OK\r\n");
    DD_CHECK(d.used_memory <= d.maxmemory);

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_noeviction_oom(void)
{
    db d;
    resp_buf out;
    char maxmem[32];
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory-policy",
             "noeviction");
    EXPECT(out, "+OK\r\n");
    /* room for exactly 2 entries */
    snprintf(maxmem, sizeof(maxmem), "%llu", (unsigned long long)(2 * ebs(1, 1)));
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory", maxmem);
    EXPECT(out, "+OK\r\n");

    exec_cmd(&d, T0, &out, 3, "SET", "a", "1");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "b", "2");
    EXPECT(out, "+OK\r\n");
    /* this write fits the check (used == max, not >), then exceeds it */
    exec_cmd(&d, T0, &out, 3, "SET", "c", "3");
    EXPECT(out, "+OK\r\n");
    DD_CHECK(d.used_memory > d.maxmemory);
    DD_CHECK_EQ_INT(0, (long long)d.evicted_keys);

    /* now over the cap: writes are rejected */
    exec_cmd(&d, T0, &out, 3, "SET", "d", "4");
    EXPECT(out,
           "-OOM command not allowed when used memory > 'maxmemory'.\r\n");
    exec_cmd(&d, T0, &out, 3, "APPEND", "a", "x");
    EXPECT(out,
           "-OOM command not allowed when used memory > 'maxmemory'.\r\n");
    exec_cmd(&d, T0, &out, 2, "INCR", "a");
    EXPECT(out,
           "-OOM command not allowed when used memory > 'maxmemory'.\r\n");
    exec_cmd(&d, T0, &out, 2, "GET", "d");
    EXPECT(out, "$-1\r\n");

    /* reads and deletes still work, and free space unblocks writes */
    exec_cmd(&d, T0, &out, 2, "GET", "a");
    EXPECT(out, "$1\r\n1\r\n");
    exec_cmd(&d, T0, &out, 2, "DEL", "a");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 2, "DEL", "b");
    EXPECT(out, ":1\r\n");
    exec_cmd(&d, T0, &out, 3, "SET", "d", "4");
    EXPECT(out, "+OK\r\n");

    resp_buf_free(&out);
    db_destroy(&d);
}

static void test_config_and_info(void)
{
    db d;
    resp_buf out;
    char nul[16384];
    db_init(&d);
    resp_buf_init(&out);

    exec_cmd(&d, T0, &out, 2, "CONFIG", "GET");
    EXPECT(out, "-ERR wrong number of arguments for 'config' command\r\n");
    exec_cmd(&d, T0, &out, 3, "CONFIG", "GET", "maxmemory");
    EXPECT(out, "*2\r\n$9\r\nmaxmemory\r\n$1\r\n0\r\n");
    exec_cmd(&d, T0, &out, 3, "CONFIG", "GET", "maxmemory-policy");
    EXPECT(out, "*2\r\n$16\r\nmaxmemory-policy\r\n$11\r\nallkeys-lru\r\n");
    exec_cmd(&d, T0, &out, 3, "CONFIG", "GET", "bogus");
    EXPECT(out, "*0\r\n");

    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory", "12345");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "CONFIG", "GET", "maxmemory");
    EXPECT(out, "*2\r\n$9\r\nmaxmemory\r\n$5\r\n12345\r\n");
    DD_CHECK_EQ_INT(12345, (long long)d.maxmemory);

    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory", "abc");
    EXPECT(out, "-ERR value is not an integer or out of range\r\n");
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory", "-5");
    EXPECT(out, "-ERR invalid argument for CONFIG SET 'maxmemory'\r\n");
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory-policy",
             "noeviction");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 3, "CONFIG", "GET", "maxmemory-policy");
    EXPECT(out, "*2\r\n$16\r\nmaxmemory-policy\r\n$10\r\nnoeviction\r\n");
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "maxmemory-policy", "bogus");
    EXPECT(out,
           "-ERR invalid argument for CONFIG SET 'maxmemory-policy'\r\n");
    exec_cmd(&d, T0, &out, 4, "CONFIG", "SET", "bogus", "1");
    EXPECT(out, "-ERR Unsupported CONFIG parameter: bogus\r\n");
    exec_cmd(&d, T0, &out, 3, "CONFIG", "BOGUS", "x");
    EXPECT(out, "-ERR unknown CONFIG subcommand\r\n");

    /* INFO: minimal sections, values as a bulk string */
    exec_cmd(&d, T0, &out, 3, "SET", "ik", "iv");
    EXPECT(out, "+OK\r\n");
    exec_cmd(&d, T0, &out, 1, "INFO");
    DD_CHECK(out.len > 0 && out.data[0] == '$');
    DD_CHECK(out.len < sizeof(nul) - 1);
    memcpy(nul, out.data, out.len);
    nul[out.len] = '\0';
    DD_CHECK(strstr(nul, "# Memory\r\n") != NULL);
    DD_CHECK(strstr(nul, "used_memory:") != NULL);
    DD_CHECK(strstr(nul, "used_memory_human:") != NULL);
    DD_CHECK(strstr(nul, "maxmemory:12345\r\n") != NULL);
    DD_CHECK(strstr(nul, "maxmemory_policy:noeviction\r\n") != NULL);
    DD_CHECK(strstr(nul, "# Stats\r\n") != NULL);
    DD_CHECK(strstr(nul, "expired_keys:0\r\n") != NULL);
    DD_CHECK(strstr(nul, "evicted_keys:0\r\n") != NULL);
    DD_CHECK(strstr(nul, "# Keyspace\r\n") != NULL);
    DD_CHECK(strstr(nul, "dbsize:1\r\n") != NULL);

    resp_buf_free(&out);
    db_destroy(&d);
}

int main(void)
{
    DD_RUN(test_memory_accounting);
    DD_RUN(test_allkeys_lru_eviction);
    DD_RUN(test_noeviction_oom);
    DD_RUN(test_config_and_info);
    return DD_TEST_SUMMARY();
}
