/* test_skiplist.c - unit + differential tests for src/ds/skiplist. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/skiplist.h"
#include "test.h"

/* deterministic xorshift for the differential test */
static uint32_t rng_state = 0x12345678u;
static uint32_t xrnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void test_insert_order(void)
{
    zskiplist *z = zsl_create();
    zsl_insert(z, 3.0, "c", 1);
    zsl_insert(z, 1.0, "a", 1);
    zsl_insert(z, 2.0, "b", 1);
    DD_CHECK_EQ_INT(3, (long long)z->length);

    /* level-0 walk is sorted */
    {
        zsl_node *n = z->header->level[0].forward;
        DD_CHECK(n != NULL && n->score == 1.0 && n->mlen == 1 &&
                 n->member[0] == 'a');
        n = n->level[0].forward;
        DD_CHECK(n != NULL && n->score == 2.0 && n->member[0] == 'b');
        n = n->level[0].forward;
        DD_CHECK(n != NULL && n->score == 3.0 && n->member[0] == 'c');
        n = n->level[0].forward;
        DD_CHECK(n == NULL);
    }
    /* tail + backward chain */
    DD_CHECK(z->tail != NULL && z->tail->score == 3.0);
    DD_CHECK(z->tail->backward != NULL && z->tail->backward->score == 2.0);
    zsl_free(z);
}

static void test_reject_unrepresentable_member_length(void)
{
#if SIZE_MAX > UINT32_MAX
    zskiplist *z = zsl_create();
    const char byte = 'x';
    uint64_t mem = z->mem;
    uint32_t rng_state = z->rng_state;
    DD_CHECK_EQ_INT(-1,
                    zsl_insert(z, 1.0, &byte, (size_t)UINT32_MAX + 1));
    DD_CHECK_EQ_INT(0, (long long)z->length);
    DD_CHECK(z->header->level[0].forward == NULL);
    DD_CHECK(z->mem == mem);
    DD_CHECK(z->rng_state == rng_state);
    zsl_free(z);
#endif
}

static void test_duplicate_score_tiebreak(void)
{
    zskiplist *z = zsl_create();
    /* equal scores: ordered by member bytes */
    zsl_insert(z, 1.0, "b", 1);
    zsl_insert(z, 1.0, "c", 1);
    zsl_insert(z, 1.0, "a", 1);
    zsl_insert(z, 1.0, "ab", 2); /* "a" < "ab" (prefix rule) */
    {
        zsl_node *n = z->header->level[0].forward;
        const char *want[4] = {"a", "ab", "b", "c"};
        int i;
        for (i = 0; i < 4; i++) {
            DD_CHECK(n != NULL);
            DD_CHECK(n->mlen == strlen(want[i]) &&
                     memcmp(n->member, want[i], n->mlen) == 0);
            n = n->level[0].forward;
        }
        DD_CHECK(n == NULL);
    }
    zsl_free(z);
}

static void test_delete_and_rank(void)
{
    zskiplist *z = zsl_create();
    int i;
    char m[8];
    for (i = 0; i < 10; i++) {
        snprintf(m, sizeof(m), "m%d", i);
        zsl_insert(z, (double)i, m, strlen(m));
    }
    DD_CHECK_EQ_INT(10, (long long)z->length);

    /* rank via walk */
    DD_CHECK_EQ_INT(0, zsl_rank(z, 0.0, "m0", 2));
    DD_CHECK_EQ_INT(5, zsl_rank(z, 5.0, "m5", 2));
    DD_CHECK_EQ_INT(9, zsl_rank(z, 9.0, "m9", 2));
    DD_CHECK_EQ_INT(-1, zsl_rank(z, 10.0, "mX", 2));

    /* delete middle, head, tail */
    DD_CHECK_EQ_INT(1, zsl_delete(z, 5.0, "m5", 2));
    DD_CHECK_EQ_INT(1, zsl_delete(z, 0.0, "m0", 2));
    DD_CHECK_EQ_INT(1, zsl_delete(z, 9.0, "m9", 2));
    DD_CHECK_EQ_INT(0, zsl_delete(z, 5.0, "m5", 2)); /* already gone */
    DD_CHECK_EQ_INT(7, (long long)z->length);
    DD_CHECK_EQ_INT(0, zsl_rank(z, 1.0, "m1", 2));
    DD_CHECK_EQ_INT(-1, zsl_rank(z, 5.0, "m5", 2));
    DD_CHECK(z->tail->score == 8.0);
    DD_CHECK(z->header->level[0].forward->score == 1.0);

    /* index walk */
    DD_CHECK(zsl_at(z, 0)->score == 1.0);
    DD_CHECK(zsl_at(z, 6)->score == 8.0);
    DD_CHECK(zsl_at(z, 7) == NULL);
    zsl_free(z);
}

static void test_ranges(void)
{
    zskiplist *z = zsl_create();
    int i;
    char m[8];
    for (i = 1; i <= 10; i++) {
        snprintf(m, sizeof(m), "m%d", i);
        zsl_insert(z, (double)i, m, strlen(m));
    }
    {
        zrangespec r;
        r.min = 3.0; r.max = 7.0; r.minex = 0; r.maxex = 0;
        DD_CHECK_EQ_INT(5, (long long)zsl_count_in_range(z, &r));
        DD_CHECK(zsl_first_in_range(z, &r)->score == 3.0);
        DD_CHECK(zsl_last_in_range(z, &r)->score == 7.0);
        r.minex = 1;
        DD_CHECK_EQ_INT(4, (long long)zsl_count_in_range(z, &r));
        r.maxex = 1;
        DD_CHECK_EQ_INT(3, (long long)zsl_count_in_range(z, &r));
        r.minex = 0; /* [3,7) */
        DD_CHECK_EQ_INT(4, (long long)zsl_count_in_range(z, &r));
    }
    {
        zrangespec all = {-INFINITY, INFINITY, 0, 0};
        zrangespec hi = {8.0, INFINITY, 0, 0};
        zrangespec none = {20.0, 30.0, 0, 0};
        DD_CHECK_EQ_INT(10, (long long)zsl_count_in_range(z, &all));
        DD_CHECK_EQ_INT(3, (long long)zsl_count_in_range(z, &hi));
        DD_CHECK_EQ_INT(0, (long long)zsl_count_in_range(z, &none));
        DD_CHECK(zsl_first_in_range(z, &none) == NULL);
        DD_CHECK(zsl_last_in_range(z, &none) == NULL);
    }
    zsl_free(z);
}

/* model entry for the differential test */
typedef struct model_entry {
    double score;
    char member[8];
} model_entry;

static int model_cmp(const void *pa, const void *pb)
{
    const model_entry *a = (const model_entry *)pa;
    const model_entry *b = (const model_entry *)pb;
    if (a->score != b->score)
        return a->score < b->score ? -1 : 1;
    return strcmp(a->member, b->member);
}

static void test_differential(void)
{
    enum { N = 500 };
    zskiplist *z = zsl_create();
    model_entry *model = malloc(N * sizeof(*model));
    int i, j;
    rng_state = 0xABCDEF01u;

    /* insert N unique members with clustered scores (many duplicates) */
    for (i = 0; i < N; i++) {
        snprintf(model[i].member, sizeof(model[i].member), "m%04d", i);
        model[i].score = (double)(xrnd() % 50);
        zsl_insert(z, model[i].score, model[i].member,
                   strlen(model[i].member));
    }
    qsort(model, N, sizeof(*model), model_cmp);
    DD_CHECK_EQ_INT(N, (long long)z->length);

    /* full-order comparison */
    {
        zsl_node *n = z->header->level[0].forward;
        for (i = 0; i < N; i++) {
            DD_CHECK(n != NULL);
            DD_CHECK(n->score == model[i].score);
            DD_CHECK(n->mlen == strlen(model[i].member) &&
                    memcmp(n->member, model[i].member, n->mlen) == 0);
            n = n->level[0].forward;
        }
        DD_CHECK(n == NULL);
    }
    /* rank of 100 sampled members equals model position */
    for (j = 0; j < 100; j++) {
        i = (int)(xrnd() % N);
        DD_CHECK_EQ_INT(i, zsl_rank(z, model[i].score, model[i].member,
                                    strlen(model[i].member)));
    }
    /* index access: zsl_at(j) lands on model[j] */
    for (j = 0; j < 100; j++) {
        i = (int)(xrnd() % N);
        {
            zsl_node *n = zsl_at(z, (size_t)i);
            DD_CHECK(n != NULL);
            DD_CHECK(n->score == model[i].score);
            DD_CHECK(n->mlen == strlen(model[i].member) &&
                    memcmp(n->member, model[i].member, n->mlen) == 0);
        }
    }
    DD_CHECK(zsl_at(z, N) == NULL);
    /* delete half (even model indexes after re-sort is fine: membership
     * is what matters), then re-compare */
    {
        int remaining = 0;
        for (i = 0; i < N; i += 2)
            DD_CHECK_EQ_INT(1, zsl_delete(z, model[i].score, model[i].member,
                                          strlen(model[i].member)));
        for (i = 1; i < N; i += 2)
            model[remaining++] = model[i];
        {
            zsl_node *n = z->header->level[0].forward;
            for (i = 0; i < remaining; i++) {
                DD_CHECK(n != NULL);
                DD_CHECK(n->score == model[i].score);
                DD_CHECK(n->mlen == strlen(model[i].member) &&
                    memcmp(n->member, model[i].member, n->mlen) == 0);
                n = n->level[0].forward;
            }
            DD_CHECK(n == NULL);
            DD_CHECK_EQ_INT(remaining, (long long)z->length);
        }
        /* rank + index AFTER deletes (span bookkeeping differential) */
        for (j = 0; j < 200; j++) {
            i = (int)(xrnd() % (uint32_t)remaining);
            DD_CHECK_EQ_INT(i, zsl_rank(z, model[i].score, model[i].member,
                                        strlen(model[i].member)));
            {
                zsl_node *n = zsl_at(z, (size_t)i);
                DD_CHECK(n != NULL);
                DD_CHECK(n->score == model[i].score);
                DD_CHECK(n->mlen == strlen(model[i].member) &&
                        memcmp(n->member, model[i].member, n->mlen) == 0);
            }
        }
        DD_CHECK_EQ_INT(-1, zsl_rank(z, -999.0, "zzzz", 4));
        DD_CHECK(zsl_at(z, (size_t)remaining) == NULL);
    }
    free(model);
    zsl_free(z);
}

int main(void)
{
    DD_RUN(test_insert_order);
    DD_RUN(test_reject_unrepresentable_member_length);
    DD_RUN(test_duplicate_score_tiebreak);
    DD_RUN(test_delete_and_rank);
    DD_RUN(test_ranges);
    DD_RUN(test_differential);
    return DD_TEST_SUMMARY();
}
