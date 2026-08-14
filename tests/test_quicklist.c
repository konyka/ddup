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

int main(void)
{
    DD_RUN(test_new_empty);
    DD_RUN(test_push_pop_ends);
    DD_RUN(test_fill_split);
    DD_RUN(test_pop_across_nodes);
    DD_RUN(test_set);
    DD_RUN(test_remove_iter);
    DD_RUN(test_differential);
    DD_RUN(test_reject_huge_element);
    DD_RUN(test_configurable_fill);
    return DD_TEST_SUMMARY();
}
