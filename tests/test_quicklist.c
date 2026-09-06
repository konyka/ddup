/* test_quicklist.c - unit + differential tests for src/ds/quicklist.
 * Written before the implementation (TDD). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/quicklist.h"
#include "test.h"

static size_t expected_node_mem(const quicklist *ql)
{
    size_t m = sizeof(quicklist);
    const ql_node *n;
    for (n = ql->head; n != NULL; n = n->next)
        m += sizeof(ql_node) + 16 + (size_t)n->bytes;
    return m;
}

static size_t count_nodes(const quicklist *ql)
{
    size_t c = 0;
    const ql_node *n;
    uint64_t total = 0;
    for (n = ql->head; n != NULL; n = n->next) {
        c++;
        total += n->count;
    }
    DD_CHECK_EQ_INT((long long)ql->len, (long long)total);
    return c;
}

/* push 0..n-1 at the tail; elements are "%d" strings */
static void fill_tail(quicklist *ql, int n)
{
    int i;
    char tmp[16];
    for (i = 0; i < n; i++) {
        int l = snprintf(tmp, sizeof(tmp), "%d", i);
        DD_CHECK_EQ_INT(0, ql_push(ql, 0, tmp, (size_t)l));
    }
}

static void expect_elem(ql_iter *it, const char *s)
{
    size_t len = 0;
    const char *v = ql_iter_value(it, &len);
    DD_CHECK(v != NULL);
    if (v != NULL)
        DD_CHECK_MEM(s, strlen(s), v, len);
}

static void test_new_empty(void)
{
    quicklist *ql = ql_new();
    char *data = (char *)0x1;
    size_t len = 0;
    ql_iter it;
    DD_CHECK(ql != NULL);
    DD_CHECK_EQ_INT(0, (long long)ql->len);
    DD_CHECK_EQ_INT((long long)sizeof(quicklist), (long long)ql_mem(ql));
    DD_CHECK_EQ_INT(0, ql_pop(ql, 1, &data, &len));
    DD_CHECK(data == (char *)0x1);
    DD_CHECK_EQ_INT(0, ql_seek(ql, 0, &it));
    DD_CHECK_EQ_INT(0, ql_first(ql, &it));
    DD_CHECK_EQ_INT(0, ql_last(ql, &it));
    ql_free(ql);
}

static void test_null_query_handles(void)
{
    ql_iter it;
    char *data = (char *)0x1;
    size_t len = 99;
    DD_CHECK_EQ_INT(0, (long long)ql_mem(NULL));
    DD_CHECK_EQ_INT(0, ql_seek(NULL, 0, &it));
    DD_CHECK_EQ_INT(0, ql_first(NULL, &it));
    DD_CHECK_EQ_INT(0, ql_last(NULL, &it));
    DD_CHECK_EQ_INT(0, ql_pop(NULL, 0, &data, &len));
    DD_CHECK(data == (char *)0x1);
    DD_CHECK_EQ_INT(0, (long long)len);
    len = 99;
    DD_CHECK(ql_iter_value(NULL, &len) == NULL);
    DD_CHECK_EQ_INT(0, (long long)len);
}

static void test_null_iterator_handles(void)
{
    DD_CHECK_EQ_INT(0, ql_iter_next(NULL));
    DD_CHECK_EQ_INT(0, ql_iter_prev(NULL));
    DD_CHECK_EQ_INT(-1, ql_set(NULL, "x", 1));
    DD_CHECK_EQ_INT(-1, ql_insert(NULL, 0, "x", 1));
    ql_remove(NULL);
}

static void test_push_rejects_null_inputs(void)
{
    quicklist *ql = ql_new();
    DD_CHECK_EQ_INT(-1, ql_push(NULL, 0, "x", 1));
    DD_CHECK_EQ_INT(-1, ql_push(ql, 0, NULL, 1));
    DD_CHECK_EQ_INT(0, ql_push(ql, 0, NULL, 0));
    DD_CHECK_EQ_INT(1, (long long)ql->len);
    ql_free(ql);
}

static void test_push_pop_ends(void)
{
    quicklist *ql = ql_new();
    ql_iter it;
    char *data = NULL;
    size_t len = 0;
    DD_CHECK_EQ_INT(0, ql_push(ql, 0, "b", 1));
    DD_CHECK_EQ_INT(0, ql_push(ql, 1, "a", 1));
    DD_CHECK_EQ_INT(0, ql_push(ql, 0, "c", 1));
    DD_CHECK_EQ_INT(3, (long long)ql->len);
    DD_CHECK_EQ_INT(1, count_nodes(ql));
    DD_CHECK(ql_mem(ql) == expected_node_mem(ql));

    DD_CHECK_EQ_INT(1, ql_first(ql, &it));
    expect_elem(&it, "a");
    DD_CHECK_EQ_INT(1, ql_iter_next(&it));
    expect_elem(&it, "b");
    DD_CHECK_EQ_INT(1, ql_iter_next(&it));
    expect_elem(&it, "c");
    DD_CHECK_EQ_INT(0, ql_iter_next(&it));

    DD_CHECK_EQ_INT(1, ql_pop(ql, 1, &data, &len));
    DD_CHECK_MEM("a", 1, data, len);
    free(data);
    DD_CHECK_EQ_INT(1, ql_pop(ql, 0, &data, &len));
    DD_CHECK_MEM("c", 1, data, len);
    free(data);
    DD_CHECK_EQ_INT(1, (long long)ql->len);
    DD_CHECK_EQ_INT(1, ql_pop(ql, 1, &data, &len));
    DD_CHECK_MEM("b", 1, data, len);
    free(data);
    DD_CHECK_EQ_INT(0, (long long)ql->len);
    DD_CHECK_EQ_INT(0, (long long)count_nodes(ql)); /* empty node unlinked */
    DD_CHECK(ql_mem(ql) == expected_node_mem(ql));
    ql_free(ql);
}

static void test_fill_split(void)
{
    /* 300 tail pushes -> 128 + 128 + 44 across three nodes */
    quicklist *ql = ql_new();
    ql_iter it;
    int i;
    fill_tail(ql, 300);
    DD_CHECK_EQ_INT(300, (long long)ql->len);
    DD_CHECK_EQ_INT(3, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(128, (long long)ql->head->count);
    DD_CHECK_EQ_INT(44, (long long)ql->tail->count);
    DD_CHECK(ql_mem(ql) == expected_node_mem(ql));

    /* forward iteration sees 0..299 in order */
    DD_CHECK_EQ_INT(1, ql_first(ql, &it));
    i = 0;
    do {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d", i);
        expect_elem(&it, tmp);
        i++;
    } while (ql_iter_next(&it));
    DD_CHECK_EQ_INT(300, i);

    /* backward iteration sees 299..0 */
    DD_CHECK_EQ_INT(1, ql_last(ql, &it));
    i = 299;
    do {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d", i);
        expect_elem(&it, tmp);
        i--;
    } while (ql_iter_prev(&it));
    DD_CHECK_EQ_INT(-1, i);

    /* seek hits node boundaries from the nearer end */
    DD_CHECK_EQ_INT(1, ql_seek(ql, 0, &it));
    expect_elem(&it, "0");
    DD_CHECK_EQ_INT(1, ql_seek(ql, 127, &it));
    expect_elem(&it, "127");
    DD_CHECK_EQ_INT(1, ql_seek(ql, 128, &it));
    expect_elem(&it, "128");
    DD_CHECK_EQ_INT(1, ql_seek(ql, 299, &it));
    expect_elem(&it, "299");
    DD_CHECK_EQ_INT(0, ql_seek(ql, 300, &it));
    ql_free(ql);
}

static void test_pop_across_nodes(void)
{
    quicklist *ql = ql_new();
    int i;
    fill_tail(ql, 300);
    /* pop 140 from the head: empties node 1 and 12 into node 2 */
    for (i = 0; i < 140; i++) {
        char *data = NULL;
        size_t len = 0;
        char tmp[16];
        int l = snprintf(tmp, sizeof(tmp), "%d", i);
        DD_CHECK_EQ_INT(1, ql_pop(ql, 1, &data, &len));
        DD_CHECK_MEM(tmp, (size_t)l, data, len);
        free(data);
    }
    DD_CHECK_EQ_INT(160, (long long)ql->len);
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(116, (long long)ql->head->count);
    /* head pops keep landing in the non-empty head node */
    for (i = 140; i < 300; i++) {
        char *data = NULL;
        size_t len = 0;
        DD_CHECK_EQ_INT(1, ql_pop(ql, 1, &data, &len));
        free(data);
    }
    DD_CHECK_EQ_INT(0, (long long)ql->len);
    DD_CHECK_EQ_INT(0, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT((long long)sizeof(quicklist), (long long)ql_mem(ql));
    ql_free(ql);
}

static void test_set(void)
{
    quicklist *ql = ql_new();
    ql_iter it;
    uint64_t before;
    fill_tail(ql, 200);
    before = ql->mem;
    /* same-size replace keeps mem; growing replace accounts the delta */
    DD_CHECK_EQ_INT(1, ql_seek(ql, 0, &it));
    DD_CHECK_EQ_INT(1, ql_set(&it, "XX", 2));
    DD_CHECK_EQ_INT(1, ql_seek(ql, 128, &it));
    DD_CHECK_EQ_INT(1, ql_set(&it, "a much longer replacement value", 31));
    DD_CHECK_EQ_INT(1, ql_seek(ql, 199, &it));
    DD_CHECK_EQ_INT(1, ql_set(&it, "-4096", 5));
    DD_CHECK(ql->mem > before);
    DD_CHECK(ql_mem(ql) == expected_node_mem(ql));
    DD_CHECK_EQ_INT(1, ql_seek(ql, 0, &it));
    expect_elem(&it, "XX");
    DD_CHECK_EQ_INT(1, ql_seek(ql, 128, &it));
    expect_elem(&it, "a much longer replacement value");
    DD_CHECK_EQ_INT(1, ql_seek(ql, 199, &it));
    expect_elem(&it, "-4096");
    ql_free(ql);
}

static void test_remove_iter(void)
{
    quicklist *ql = ql_new();
    ql_iter it;
    int i;
    fill_tail(ql, 300);

    /* remove the head element: iterator lands on the successor */
    DD_CHECK_EQ_INT(1, ql_first(ql, &it));
    ql_remove(&it);
    expect_elem(&it, "1");
    DD_CHECK_EQ_INT(299, (long long)ql->len);

    /* remove the tail element of the head node ("128"): the iterator must
     * cross into the next node */
    DD_CHECK_EQ_INT(1, ql_seek(ql, 127, &it));
    expect_elem(&it, "128");
    ql_remove(&it);
    expect_elem(&it, "129");
    DD_CHECK_EQ_INT(3, (long long)count_nodes(ql));

    /* remove the tail: iterator becomes invalid */
    DD_CHECK_EQ_INT(1, ql_last(ql, &it));
    ql_remove(&it);
    DD_CHECK_EQ_INT(0, ql_iter_next(&it));
    DD_CHECK_EQ_INT(297, (long long)ql->len);

    /* remove everything through one iterator: nodes unlink as they empty */
    DD_CHECK_EQ_INT(1, ql_first(ql, &it));
    i = 1;
    while (it.entry != NULL) {
        char tmp[16];
        /* expected sequence: 1,2,...,127,129,...,298 */
        if (i == 128)
            i++; /* "128" was removed above */
        snprintf(tmp, sizeof(tmp), "%d", i);
        expect_elem(&it, tmp);
        ql_remove(&it);
        i++;
    }
    DD_CHECK_EQ_INT(0, (long long)ql->len);
    DD_CHECK_EQ_INT(0, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT((long long)sizeof(quicklist), (long long)ql_mem(ql));
    ql_free(ql);
}

/* reference model: array of malloc'd strings */
typedef struct {
    char **v;
    size_t *l;
    size_t n;
    size_t cap;
} ref_list;

static void ref_insert(ref_list *r, size_t at, const char *s, size_t len)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->v = (char **)realloc(r->v, r->cap * sizeof(*r->v));
        r->l = (size_t *)realloc(r->l, r->cap * sizeof(*r->l));
    }
    memmove(r->v + at + 1, r->v + at, (r->n - at) * sizeof(*r->v));
    memmove(r->l + at + 1, r->l + at, (r->n - at) * sizeof(*r->l));
    r->v[at] = (char *)malloc(len);
    memcpy(r->v[at], s, len);
    r->l[at] = len;
    r->n++;
}

static void ref_remove(ref_list *r, size_t at)
{
    free(r->v[at]);
    memmove(r->v + at, r->v + at + 1, (r->n - at - 1) * sizeof(*r->v));
    memmove(r->l + at, r->l + at + 1, (r->n - at - 1) * sizeof(*r->l));
    r->n--;
}

static void ref_free(ref_list *r)
{
    size_t i;
    for (i = 0; i < r->n; i++)
        free(r->v[i]);
    free(r->v);
    free(r->l);
}

static uint32_t rng_state = 0xC0FFEEu;
static uint32_t xrnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void test_differential(void)
{
    quicklist *ql = ql_new();
    ref_list ref = {NULL, NULL, 0, 0};
    int op;
    for (op = 0; op < 4000; op++) {
        uint32_t r = xrnd() % 100;
        char tmp[24];
        int l = snprintf(tmp, sizeof(tmp), "v%u", xrnd() % 1000);
        if (r < 35) { /* push left */
            DD_CHECK_EQ_INT(0, ql_push(ql, 1, tmp, (size_t)l));
            ref_insert(&ref, 0, tmp, (size_t)l);
        } else if (r < 70) { /* push right */
            DD_CHECK_EQ_INT(0, ql_push(ql, 0, tmp, (size_t)l));
            ref_insert(&ref, ref.n, tmp, (size_t)l);
        } else if (r < 85) { /* pop */
            int left = (int)(xrnd() & 1);
            size_t at = left ? 0 : ref.n - 1;
            char *data = NULL;
            size_t len = 0;
            if (ref.n == 0) {
                DD_CHECK_EQ_INT(0, ql_pop(ql, left, &data, &len));
                continue;
            }
            DD_CHECK_EQ_INT(1, ql_pop(ql, left, &data, &len));
            DD_CHECK_MEM(ref.v[at], ref.l[at], data, len);
            free(data);
            ref_remove(&ref, at);
        } else if (r < 95) { /* set at random index */
            if (ref.n == 0)
                continue;
            {
                size_t at = xrnd() % ref.n;
                ql_iter it;
                DD_CHECK_EQ_INT(1, ql_seek(ql, at, &it));
                DD_CHECK_EQ_INT(1, ql_set(&it, tmp, (size_t)l));
                free(ref.v[at]);
                ref.v[at] = (char *)malloc((size_t)l);
                memcpy(ref.v[at], tmp, (size_t)l);
                ref.l[at] = (size_t)l;
            }
        } else { /* remove at random index */
            if (ref.n == 0)
                continue;
            {
                size_t at = xrnd() % ref.n;
                ql_iter it;
                DD_CHECK_EQ_INT(1, ql_seek(ql, at, &it));
                ql_remove(&it);
                ref_remove(&ref, at);
            }
        }
        if ((op & 63) == 0) {
            /* periodic full forward + backward comparison */
            size_t i;
            ql_iter it;
            DD_CHECK_EQ_INT((long long)ref.n, (long long)ql->len);
            DD_CHECK(ql_mem(ql) == expected_node_mem(ql));
            if (ref.n == 0) {
                DD_CHECK_EQ_INT(0, ql_first(ql, &it));
                continue;
            }
            DD_CHECK_EQ_INT(1, ql_first(ql, &it));
            i = 0;
            do {
                size_t len = 0;
                const char *v = ql_iter_value(&it, &len);
                DD_CHECK_MEM(ref.v[i], ref.l[i], v, len);
                i++;
            } while (ql_iter_next(&it));
            DD_CHECK_EQ_INT((long long)ref.n, (long long)i);
            DD_CHECK_EQ_INT(1, ql_last(ql, &it));
            i = ref.n - 1;
            for (;;) {
                size_t len = 0;
                const char *v = ql_iter_value(&it, &len);
                DD_CHECK_MEM(ref.v[i], ref.l[i], v, len);
                if (i == 0)
                    break;
                i--;
                DD_CHECK_EQ_INT(1, ql_iter_prev(&it));
            }
        }
    }
    ref_free(&ref);
    ql_free(ql);
}

static void test_reject_huge_element(void)
{
#if SIZE_MAX > UINT32_MAX
    quicklist *ql = ql_new();
    const char byte = 'x';
    uint64_t mem = ql->mem;
    DD_CHECK_EQ_INT(-1, ql_push(ql, 0, &byte, (size_t)UINT32_MAX + 1));
    DD_CHECK_EQ_INT(0, (long long)ql->len);
    DD_CHECK(ql->mem == mem);
    DD_CHECK(ql->head == NULL);
    {
        ql_iter it;
        DD_CHECK_EQ_INT(0, ql_push(ql, 0, "a", 1));
        DD_CHECK_EQ_INT(1, ql_first(ql, &it));
        DD_CHECK_EQ_INT(-1, ql_set(&it, &byte, (size_t)UINT32_MAX + 1));
        expect_elem(&it, "a"); /* unchanged */
    }
    ql_free(ql);
#endif
}

static void test_configurable_fill(void)
{
    quicklist *ql;

    /* lowered fill: nodes split at the new limit */
    quicklist_set_fill(4);
    ql = ql_new();
    fill_tail(ql, 10);
    DD_CHECK_EQ_INT(10, (long long)ql->len);
    DD_CHECK_EQ_INT(3, (long long)count_nodes(ql)); /* 4 + 4 + 2 */
    DD_CHECK_EQ_INT(4, (long long)ql->head->count);
    DD_CHECK_EQ_INT(2, (long long)ql->tail->count);
    ql_free(ql);

    /* fill 1: every element gets its own node */
    quicklist_set_fill(1);
    ql = ql_new();
    fill_tail(ql, 3);
    DD_CHECK_EQ_INT(3, (long long)count_nodes(ql));
    ql_free(ql);

    /* restore the default for the rest of the process */
    quicklist_set_fill((int)DDUP_QL_FILL);
    ql = ql_new();
    fill_tail(ql, 129);
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(128, (long long)ql->head->count);
    ql_free(ql);
}

static void expect_values(quicklist *ql, const int *vals, int n)
{
    ql_iter it;
    int i = 0;
    DD_CHECK_EQ_INT(1, ql_first(ql, &it));
    do {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%d", vals[i]);
        expect_elem(&it, tmp);
        i++;
    } while (ql_iter_next(&it));
    DD_CHECK_EQ_INT(n, i);
}

static void test_merge_basic(void)
{
    /* fill=8: [0..7][8..15][16..23]; deleting 8..14 leaves the middle
     * node at count 1 (< 8/4=2), so it absorbs next (1+8 <= 2*8) */
    quicklist *ql;
    ql_iter it;
    int i;
    int exp[17];
    quicklist_set_fill(8);
    ql = ql_new();
    fill_tail(ql, 24);
    DD_CHECK_EQ_INT(3, (long long)count_nodes(ql));

    DD_CHECK_EQ_INT(1, ql_seek(ql, 8, &it));
    for (i = 0; i < 7; i++)
        ql_remove(&it);
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(17, (long long)ql->len);
    DD_CHECK_EQ_INT(9, (long long)ql->head->next->count);

    /* the iterator survived the merge on the same logical element */
    expect_elem(&it, "15");
    DD_CHECK_EQ_INT(1, ql_iter_next(&it));
    expect_elem(&it, "16");
    DD_CHECK_EQ_INT(1, ql_iter_prev(&it));
    expect_elem(&it, "15");
    DD_CHECK_EQ_INT(1, ql_iter_prev(&it));
    expect_elem(&it, "7");

    for (i = 0; i < 8; i++)
        exp[i] = i; /* 0..7 */
    exp[8] = 15;
    for (i = 9; i < 17; i++)
        exp[i] = i + 7; /* 16..23 */
    expect_values(ql, exp, 17);
    quicklist_set_fill((int)DDUP_QL_FILL);
    ql_free(ql);
}

static void test_merge_into_prev(void)
{
    /* fill=8: [0..7][8..15][16..19]; deleting 17,18,19 leaves the tail
     * node at count 1 (< 2); next is NULL, so it folds into prev
     * (1+8 <= 16). Case B: the iterator falls off the end and the node
     * under it is freed by the merge. */
    quicklist *ql;
    ql_iter it;
    int i;
    int exp[17];
    quicklist_set_fill(8);
    ql = ql_new();
    fill_tail(ql, 20);
    DD_CHECK_EQ_INT(3, (long long)count_nodes(ql));

    DD_CHECK_EQ_INT(1, ql_seek(ql, 17, &it));
    ql_remove(&it); /* 17 */
    expect_elem(&it, "18");
    ql_remove(&it); /* 18 */
    ql_remove(&it); /* 19: tail entry, count 1 -> merge into prev */
    DD_CHECK(it.entry == NULL);
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(8, (long long)ql->head->count);
    DD_CHECK_EQ_INT(9, (long long)ql->tail->count);
    DD_CHECK_EQ_INT(17, (long long)ql->len);
    for (i = 0; i < 17; i++)
        exp[i] = i;
    expect_values(ql, exp, 17);
    quicklist_set_fill((int)DDUP_QL_FILL);
    ql_free(ql);
}

static void test_merge_size_cap(void)
{
    /* The 2*fill sum cap is unreachable through push/remove alone (nodes
     * never exceed fill and a sparse node is < fill/4, so a pair always
     * sums to < 1.5*fill); it becomes reachable when fill shrinks after
     * nodes were packed. fill 128 -> 8: sparse < 2, cap 16. */
    quicklist *ql;
    char *data = NULL;
    size_t len = 0;

    /* [0..127][128,129] at fill 128, then fill 8: popping the tail down
     * to 1 entry must NOT merge into the 128-entry prev (1+128 > 16) */
    quicklist_set_fill(128);
    ql = ql_new();
    fill_tail(ql, 130);
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    quicklist_set_fill(8);
    DD_CHECK_EQ_INT(1, ql_pop(ql, 0, &data, &len));
    DD_CHECK_MEM("129", 3, data, len);
    free(data);
    data = NULL;
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(128, (long long)ql->head->count);
    DD_CHECK_EQ_INT(1, (long long)ql->tail->count);
    ql_free(ql);

    /* head-push mirror: [129][128..1 -> wait, head pushes reverse]:
     * [x][128-entry node]; popping the head down to 1 entry must NOT
     * merge into the 128-entry next */
    quicklist_set_fill(128);
    ql = ql_new();
    {
        int i;
        char tmp[16];
        for (i = 0; i < 130; i++) {
            int l = snprintf(tmp, sizeof(tmp), "%d", i);
            DD_CHECK_EQ_INT(0, ql_push(ql, 1, tmp, (size_t)l));
        }
    }
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(2, (long long)ql->head->count);
    quicklist_set_fill(8);
    DD_CHECK_EQ_INT(1, ql_pop(ql, 1, &data, &len));
    free(data);
    data = NULL;
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql)); /* 1+128 > 16: no merge */
    DD_CHECK_EQ_INT(1, (long long)ql->head->count);
    DD_CHECK_EQ_INT(128, (long long)ql->tail->count);
    ql_free(ql);

    /* positive control: same layout, but cap 200 admits 1+128 */
    quicklist_set_fill(128);
    ql = ql_new();
    fill_tail(ql, 130);
    quicklist_set_fill(100); /* sparse < 25, cap 200 */
    DD_CHECK_EQ_INT(1, ql_pop(ql, 0, &data, &len));
    free(data);
    data = NULL;
    DD_CHECK_EQ_INT(1, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(129, (long long)ql->head->count);
    DD_CHECK_EQ_INT(129, (long long)ql->len);
    quicklist_set_fill((int)DDUP_QL_FILL);
    ql_free(ql);
}

static void test_merge_pop_end(void)
{
    /* fill=8: [0..7][8..15][16..23]; 7 head pops leave the head node at
     * count 1 (< 2) -> it absorbs the next node (1+8 <= 16). The merge
     * must strictly shrink mem (one node overhead gone) and keep len. */
    quicklist *ql;
    uint64_t mem_before;
    int i;
    char *data = NULL;
    size_t len = 0;
    int exp[17];
    quicklist_set_fill(8);
    ql = ql_new();
    fill_tail(ql, 24);
    for (i = 0; i < 6; i++) {
        DD_CHECK_EQ_INT(1, ql_pop(ql, 1, &data, &len));
        free(data);
        data = NULL;
    }
    DD_CHECK_EQ_INT(2, (long long)ql->head->count); /* 6,7: not sparse yet */
    mem_before = ql_mem(ql);
    DD_CHECK_EQ_INT(1, ql_pop(ql, 1, &data, &len));
    DD_CHECK_MEM("6", 1, data, len);
    free(data);
    data = NULL;
    DD_CHECK_EQ_INT(2, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(9, (long long)ql->head->count); /* 7,8..15 */
    DD_CHECK_EQ_INT(17, (long long)ql->len);
    DD_CHECK(ql_mem(ql) < mem_before);
    DD_CHECK(ql_mem(ql) == expected_node_mem(ql));
    for (i = 0; i < 17; i++)
        exp[i] = i + 7; /* 7..23 */
    expect_values(ql, exp, 17);
    quicklist_set_fill((int)DDUP_QL_FILL);
    ql_free(ql);
}

static void test_merge_disabled_tiny_fill(void)
{
    /* fill < 4 makes the sparse threshold fill/4 == 0: never triggers */
    quicklist *ql;
    ql_iter it;
    int exp[9] = {0, 1, 2, 5, 6, 7, 8, -1, -1};
    quicklist_set_fill(3);
    ql = ql_new();
    fill_tail(ql, 9); /* [0,1,2][3,4,5][6,7,8] */
    DD_CHECK_EQ_INT(1, ql_seek(ql, 3, &it));
    ql_remove(&it); /* 3 */
    ql_remove(&it); /* 4: middle node at count 1, still no merge */
    DD_CHECK_EQ_INT(3, (long long)count_nodes(ql));
    DD_CHECK_EQ_INT(1, (long long)ql->head->next->count);
    expect_values(ql, exp, 7);
    quicklist_set_fill((int)DDUP_QL_FILL);
    ql_free(ql);
}

int main(void)
{
    DD_RUN(test_null_query_handles);
    DD_RUN(test_null_iterator_handles);
    DD_RUN(test_push_rejects_null_inputs);
    DD_RUN(test_new_empty);
    DD_RUN(test_push_pop_ends);
    DD_RUN(test_fill_split);
    DD_RUN(test_pop_across_nodes);
    DD_RUN(test_set);
    DD_RUN(test_remove_iter);
    DD_RUN(test_differential);
    DD_RUN(test_reject_huge_element);
    DD_RUN(test_configurable_fill);
    DD_RUN(test_merge_basic);
    DD_RUN(test_merge_into_prev);
    DD_RUN(test_merge_size_cap);
    DD_RUN(test_merge_pop_end);
    DD_RUN(test_merge_disabled_tiny_fill);
    return DD_TEST_SUMMARY();
}
