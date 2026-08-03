/* test_repl.c - replication backlog ring buffer (sub-step 1). */
#include <string.h>

#include "server/repl.h"
#include "test.h"

static void test_backlog_basic(void)
{
    repl_backlog b;
    char out[64];
    repl_backlog_init(&b, 16);

    repl_backlog_append(&b, "hello", 5);
    DD_CHECK_EQ_INT(5, (long long)b.offset);
    DD_CHECK_EQ_INT(5, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("hello", 5, out, 5);

    repl_backlog_append(&b, " world", 6);
    DD_CHECK_EQ_INT(11, (long long)b.offset);
    DD_CHECK_EQ_INT(11, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("hello world", 11, out, 11);

    repl_backlog_free(&b);
}

static void test_backlog_wrap(void)
{
    repl_backlog b;
    char out[64];
    repl_backlog_init(&b, 16);

    /* 10 + 10 bytes into a 16-byte ring: oldest 4 dropped */
    repl_backlog_append(&b, "0123456789", 10);
    repl_backlog_append(&b, "abcdefghij", 10);
    DD_CHECK_EQ_INT(20, (long long)b.offset); /* offset counts everything */
    DD_CHECK_EQ_INT(16, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("456789abcdefghij", 16, out, 16);

    /* a write larger than the ring keeps only its tail */
    repl_backlog_append(&b, "0123456789ABCDEFGHIJ", 20);
    DD_CHECK_EQ_INT(40, (long long)b.offset);
    DD_CHECK_EQ_INT(16, (long long)repl_backlog_read(&b, out, sizeof(out)));
    DD_CHECK_MEM("456789ABCDEFGHIJ", 16, out, 16);

    repl_backlog_free(&b);
}

int main(void)
{
    DD_RUN(test_backlog_basic);
    DD_RUN(test_backlog_wrap);
    return DD_TEST_SUMMARY();
}
