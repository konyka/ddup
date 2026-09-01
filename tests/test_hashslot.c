/* test_hashslot.c - CRC16-XMODEM and Redis hash slot computation. */
#include <string.h>

#include "core/hashslot.h"
#include "test.h"
#include "pal/pal_thread.h"

typedef struct crc_worker_ctx {
    int failures[8];
    int index;
} crc_worker_ctx;

static void *crc_worker(void *arg)
{
    crc_worker_ctx *ctx = (crc_worker_ctx *)arg;
    int i;
    for (i = 0; i < 100000; i++) {
        if (crc16("123456789", 9) != 0x31C3)
            ctx->failures[ctx->index]++;
    }
    return NULL;
}

static void test_crc16_vectors(void)
{
    /* CRC-16/XMODEM check value (poly 0x1021, init 0, MSB-first). */
    DD_CHECK_EQ_INT(0x31C3, (long long)crc16("123456789", 9));
    DD_CHECK_EQ_INT(0, (long long)crc16("", 0));
    /* relationship checks (not invented constants) */
    DD_CHECK(crc16("foo", 3) != crc16("bar", 3));
}

static void test_crc16_concurrent_initialization(void)
{
    enum { THREADS = 8 };
    pal_thread threads[THREADS];
    crc_worker_ctx ctx[THREADS];
    int i;

    memset(ctx, 0, sizeof(ctx));
    for (i = 0; i < THREADS; i++) {
        ctx[i].index = i;
    }
    for (i = 0; i < THREADS; i++) {
        DD_CHECK_EQ_INT(0, pal_thread_create(&threads[i], crc_worker, &ctx[i]));
    }
    for (i = 0; i < THREADS; i++) {
        DD_CHECK_EQ_INT(0, pal_thread_join(&threads[i], NULL));
    }
    for (i = 0; i < THREADS; i++)
        DD_CHECK_EQ_INT(0, ctx[i].failures[i]);
}

static uint16_t crc16_reference(const unsigned char *buf, size_t len)
{
    uint16_t crc = 0;
    size_t i;
    int bit;
    for (i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ ((uint16_t)buf[i] << 8));
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u)
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void test_crc16_table_matches_reference(void)
{
    unsigned char sample[64];
    size_t len;
    size_t i;
    for (i = 0; i < sizeof(sample); i++)
        sample[i] = (unsigned char)(i * 37u + 11u);
    for (len = 0; len <= sizeof(sample); len++)
        DD_CHECK_EQ_INT((long long)crc16_reference(sample, len),
                        (long long)crc16((const char *)sample, len));
}

static void test_hashtag_rule(void)
{
    char tag[64];
    size_t n;

    n = hash_tag("foo", 3, tag, sizeof(tag));
    DD_CHECK(n == 3 && memcmp(tag, "foo", 3) == 0);

    n = hash_tag("{user1000}.following", 20, tag, sizeof(tag));
    DD_CHECK(n == 8 && memcmp(tag, "user1000", 8) == 0);

    /* empty braces do NOT count as a hashtag */
    n = hash_tag("{}.x", 4, tag, sizeof(tag));
    DD_CHECK(n == 4 && memcmp(tag, "{}.x", 4) == 0);

    /* no closing brace -> whole key */
    n = hash_tag("{abc", 4, tag, sizeof(tag));
    DD_CHECK(n == 4 && memcmp(tag, "{abc", 4) == 0);

    /* brace later in key works */
    n = hash_tag("foo{bar}zap", 11, tag, sizeof(tag));
    DD_CHECK(n == 3 && memcmp(tag, "bar", 3) == 0);
}

static void test_slot_vectors(void)
{
    /* published Redis hash slot values */
    DD_CHECK_EQ_INT(12182, (long long)hash_slot("foo", 3));
    DD_CHECK_EQ_INT(5061, (long long)hash_slot("bar", 3));
    /* relationship checks */
    DD_CHECK_EQ_INT(hash_slot("{user1000}.following", 20),
                    hash_slot("{user1000}.followers", 20));
    DD_CHECK_EQ_INT((long long)hash_slot("user1000", 8),
                    (long long)hash_slot("{user1000}.following", 20));
    DD_CHECK_EQ_INT(0, (long long)hash_slot("", 0));
    DD_CHECK(hash_slot("a", 1) < 16384);
    DD_CHECK(hash_slot("zzzzzzzzzzzzzzzzzzzz", 20) < 16384);
}

int main(void)
{
    DD_RUN(test_crc16_vectors);
    DD_RUN(test_crc16_concurrent_initialization);
    DD_RUN(test_crc16_table_matches_reference);
    DD_RUN(test_hashtag_rule);
    DD_RUN(test_slot_vectors);
    return DD_TEST_SUMMARY();
}
