/* test_spsc.c - unit tests for the single-producer/single-consumer ring. */
#include "test.h"

#include <stddef.h>

#include "pal/pal_thread.h"
#include "server/mt_spsc.h"

static void test_fifo_order(void)
{
    mt_spsc q;
    int a = 1, b = 2, c = 3;

    DD_CHECK_EQ_INT(0, mt_spsc_init(&q, 64));
    DD_CHECK_EQ_INT(1, mt_spsc_push(&q, &a)); /* empty -> kick */
    DD_CHECK_EQ_INT(0, mt_spsc_push(&q, &b));
    DD_CHECK_EQ_INT(0, mt_spsc_push(&q, &c));

    DD_CHECK(mt_spsc_pop(&q) == &a);
    DD_CHECK(mt_spsc_pop(&q) == &b);
    DD_CHECK(mt_spsc_pop(&q) == &c);
    DD_CHECK(mt_spsc_pop(&q) == NULL);

    /* drained: the next push reports empty again */
    DD_CHECK_EQ_INT(1, mt_spsc_push(&q, &a));
    DD_CHECK(mt_spsc_pop(&q) == &a);
    DD_CHECK(mt_spsc_pop(&q) == NULL);

    mt_spsc_destroy(&q);
}

static void test_full_condition(void)
{
    mt_spsc q;
    int items[32];
    int i;

    DD_CHECK_EQ_INT(0, mt_spsc_init(&q, 16)); /* capacity 16, usable 15 */
    for (i = 0; i < 15; i++)
        DD_CHECK(mt_spsc_push(&q, &items[i]) >= 0);
    DD_CHECK_EQ_INT(-1, mt_spsc_push(&q, &items[15])); /* full */

    DD_CHECK(mt_spsc_pop(&q) == &items[0]);
    DD_CHECK(mt_spsc_push(&q, &items[15]) >= 0); /* space again */

    for (i = 1; i < 16; i++)
        DD_CHECK(mt_spsc_pop(&q) == &items[i]);
    DD_CHECK(mt_spsc_pop(&q) == NULL);

    mt_spsc_destroy(&q);
}

typedef struct spsc_stress {
    mt_spsc q;
    long produced;
    long consumed;
    volatile int done;
} spsc_stress;

#define STRESS_N 200000

static void *spsc_producer(void *arg)
{
    spsc_stress *s = (spsc_stress *)arg;
    long i;
    for (i = 0; i < STRESS_N; i++) {
        while (mt_spsc_push(&s->q, (void *)(ptrdiff_t)(i + 1)) < 0)
            ; /* spin: consumer is draining */
        s->produced++;
    }
    return NULL;
}

static void *spsc_consumer(void *arg)
{
    spsc_stress *s = (spsc_stress *)arg;
    long expect = 1;
    while (expect <= STRESS_N) {
        void *p = mt_spsc_pop(&s->q);
        if (p == NULL)
            continue;
        if ((long)(ptrdiff_t)p != expect)
            break;
        expect++;
        s->consumed++;
    }
    s->done = 1;
    return NULL;
}

static void test_two_thread_stress(void)
{
    spsc_stress s;
    pal_thread pt, ct;

    s.produced = 0;
    s.consumed = 0;
    s.done = 0;
    DD_CHECK_EQ_INT(0, mt_spsc_init(&s.q, 1024));
    DD_CHECK_EQ_INT(0, pal_thread_create(&pt, spsc_producer, &s));
    DD_CHECK_EQ_INT(0, pal_thread_create(&ct, spsc_consumer, &s));
    DD_CHECK_EQ_INT(0, pal_thread_join(&pt, NULL));
    DD_CHECK_EQ_INT(0, pal_thread_join(&ct, NULL));

    DD_CHECK_EQ_INT(STRESS_N, s.produced);
    DD_CHECK_EQ_INT(STRESS_N, s.consumed);
    mt_spsc_destroy(&s.q);
}

int main(void)
{
    DD_RUN(test_fifo_order);
    DD_RUN(test_full_condition);
    DD_RUN(test_two_thread_stress);
    return DD_TEST_SUMMARY();
}
