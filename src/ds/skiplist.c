/* skiplist.c - classic Redis-style skip list; see skiplist.h. */
#include "ds/skiplist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (score, member) three-way compare: score first, member bytes on tie. */
static int zsl_cmp(double s1, const char *m1, size_t l1, double s2,
                   const char *m2, size_t l2)
{
    size_t minl;
    int c;
    if (s1 < s2)
        return -1;
    if (s1 > s2)
        return 1;
    minl = l1 < l2 ? l1 : l2;
    c = minl > 0 ? memcmp(m1, m2, minl) : 0;
    if (c != 0)
        return c < 0 ? -1 : 1;
    if (l1 < l2)
        return -1;
    if (l1 > l2)
        return 1;
    return 0;
}

static void *zsl_xmalloc(size_t n)
{
    void *p = malloc(n);
    if (p == NULL) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    return p;
}

static uint32_t zsl_rand(zskiplist *z)
{
    uint32_t x = z->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    z->rng_state = x;
    return x;
}

static int zsl_random_level(zskiplist *z)
{
    int level = 1;
    while (level < ZSL_MAX_LEVEL && (zsl_rand(z) & 0xFFFFu) < 0x4000u)
        level++; /* p = 1/4 */
    return level;
}

static zsl_node *zsl_node_new(int level, double score, const char *member,
                              size_t mlen)
{
    zsl_node *n = (zsl_node *)zsl_xmalloc(sizeof(zsl_node) +
                                          (size_t)level * sizeof(zsl_node *));
    n->score = score;
    n->backward = NULL;
    n->mlen = (uint32_t)mlen;
    n->member = (char *)zsl_xmalloc(mlen);
    memcpy(n->member, member, mlen);
    return n;
}

static uint64_t zsl_node_bytes(int level, size_t mlen)
{
    return (uint64_t)sizeof(zsl_node) + (uint64_t)level * sizeof(void *) +
           16 + mlen + 16;
}

zskiplist *zsl_create(void)
{
    zskiplist *z = (zskiplist *)zsl_xmalloc(sizeof(*z));
    int i;
    z->header = zsl_node_new(ZSL_MAX_LEVEL, 0.0, "", 0);
    for (i = 0; i < ZSL_MAX_LEVEL; i++)
        z->header->forward[i] = NULL;
    z->tail = NULL;
    z->length = 0;
    z->level = 1;
    z->mem = (uint64_t)sizeof(*z) + zsl_node_bytes(ZSL_MAX_LEVEL, 0);
    z->rng_state = 0x2545F491u;
    return z;
}

void zsl_free(zskiplist *z)
{
    zsl_node *n;
    if (z == NULL)
        return;
    n = z->header->forward[0];
    while (n != NULL) {
        zsl_node *next = n->forward[0];
        free(n->member);
        free(n);
        n = next;
    }
    free(z->header->member);
    free(z->header);
    free(z);
}

void zsl_insert(zskiplist *z, double score, const char *member, size_t mlen)
{
    zsl_node *update[ZSL_MAX_LEVEL];
    zsl_node *x;
    zsl_node *n;
    int level;
    int i;

    x = z->header;
    for (i = z->level - 1; i >= 0; i--) {
        while (x->forward[i] != NULL &&
               zsl_cmp(x->forward[i]->score, x->forward[i]->member,
                       x->forward[i]->mlen, score, member, mlen) < 0)
            x = x->forward[i];
        update[i] = x;
    }
    level = zsl_random_level(z);
    if (level > z->level) {
        for (i = z->level; i < level; i++)
            update[i] = z->header;
        z->level = level;
    }
    n = zsl_node_new(level, score, member, mlen);
    for (i = 0; i < level; i++) {
        n->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = n;
    }
    n->backward = (update[0] == z->header) ? NULL : update[0];
    if (n->forward[0] != NULL)
        n->forward[0]->backward = n;
    else
        z->tail = n;
    z->length++;
    z->mem += zsl_node_bytes(level, mlen);
}

int zsl_delete(zskiplist *z, double score, const char *member, size_t mlen)
{
    zsl_node *update[ZSL_MAX_LEVEL];
    zsl_node *x;
    int i;
    int level = 0;

    x = z->header;
    for (i = z->level - 1; i >= 0; i--) {
        while (x->forward[i] != NULL &&
               zsl_cmp(x->forward[i]->score, x->forward[i]->member,
                       x->forward[i]->mlen, score, member, mlen) < 0)
            x = x->forward[i];
        update[i] = x;
    }
    x = x->forward[0];
    if (x == NULL ||
        zsl_cmp(x->score, x->member, x->mlen, score, member, mlen) != 0)
        return 0;
    for (i = 0; i < z->level; i++) {
        if (update[i]->forward[i] == x) {
            update[i]->forward[i] = x->forward[i];
            level = i + 1; /* highest level the node participated in */
        }
    }
    if (x->forward[0] != NULL)
        x->forward[0]->backward = x->backward;
    else
        z->tail = x->backward;
    while (z->level > 1 && z->header->forward[z->level - 1] == NULL)
        z->level--;
    z->length--;
    z->mem -= zsl_node_bytes(level, x->mlen);
    free(x->member);
    free(x);
    return 1;
}

long zsl_rank(zskiplist *z, double score, const char *member, size_t mlen)
{
    zsl_node *n = z->header->forward[0];
    long rank = 0;
    while (n != NULL) {
        int c = zsl_cmp(n->score, n->member, n->mlen, score, member, mlen);
        if (c == 0)
            return rank;
        if (c > 0)
            return -1;
        rank++;
        n = n->forward[0];
    }
    return -1;
}

zsl_node *zsl_at(zskiplist *z, size_t idx)
{
    zsl_node *n = z->header->forward[0];
    size_t i = 0;
    while (n != NULL && i < idx) {
        n = n->forward[0];
        i++;
    }
    return n;
}

static int zsl_gte_min(double score, const zrangespec *r)
{
    return r->minex ? score > r->min : score >= r->min;
}

static int zsl_lte_max(double score, const zrangespec *r)
{
    return r->maxex ? score < r->max : score <= r->max;
}

zsl_node *zsl_first_in_range(zskiplist *z, const zrangespec *r)
{
    zsl_node *x = z->header;
    int i;
    if (zsl_cmp(r->min, "", 0, r->max, "", 0) > 0)
        return NULL; /* min > max: empty */
    for (i = z->level - 1; i >= 0; i--)
        while (x->forward[i] != NULL &&
               !zsl_gte_min(x->forward[i]->score, r))
            x = x->forward[i];
    x = x->forward[0];
    if (x == NULL || !zsl_lte_max(x->score, r))
        return NULL;
    return x;
}

zsl_node *zsl_last_in_range(zskiplist *z, const zrangespec *r)
{
    zsl_node *x = z->header;
    int i;
    if (zsl_cmp(r->min, "", 0, r->max, "", 0) > 0)
        return NULL;
    for (i = z->level - 1; i >= 0; i--)
        while (x->forward[i] != NULL && zsl_lte_max(x->forward[i]->score, r))
            x = x->forward[i];
    if (x == z->header || !zsl_gte_min(x->score, r))
        return NULL;
    return x;
}

size_t zsl_count_in_range(zskiplist *z, const zrangespec *r)
{
    zsl_node *n = zsl_first_in_range(z, r);
    size_t count = 0;
    while (n != NULL && zsl_lte_max(n->score, r)) {
        count++;
        n = n->forward[0];
    }
    return count;
}
