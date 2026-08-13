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
    size_t level_bytes;
    if (mlen > UINT32_MAX ||
        (size_t)level > (SIZE_MAX - sizeof(zsl_node)) /
                             sizeof(struct zsl_level))
        return NULL;
    level_bytes = (size_t)level * sizeof(struct zsl_level);
    zsl_node *n = (zsl_node *)zsl_xmalloc(sizeof(zsl_node) + level_bytes);
    n->score = score;
    n->backward = NULL;
    n->mlen = (uint32_t)mlen;
    n->member = (char *)zsl_xmalloc(mlen ? mlen : 1);
    memcpy(n->member, member, mlen);
    return n;
}

static int zsl_node_bytes(int level, size_t mlen, uint64_t *bytes)
{
    uint64_t total = sizeof(zsl_node);
    if (mlen > UINT32_MAX || level < 0 ||
        (uint64_t)level > (UINT64_MAX - total) / sizeof(struct zsl_level))
        return -1;
    total += (uint64_t)level * sizeof(struct zsl_level);
    if (total > UINT64_MAX - 32 - (uint64_t)mlen)
        return -1;
    *bytes = total + 32 + (uint64_t)mlen;
    return 0;
}

zskiplist *zsl_create(void)
{
    zskiplist *z = (zskiplist *)zsl_xmalloc(sizeof(*z));
    int i;
    z->header = zsl_node_new(ZSL_MAX_LEVEL, 0.0, "", 0);
    for (i = 0; i < ZSL_MAX_LEVEL; i++) {
        z->header->level[i].forward = NULL;
        z->header->level[i].span = 0;
    }
    z->tail = NULL;
    z->length = 0;
    z->level = 1;
    {
        uint64_t header_bytes;
        if (zsl_node_bytes(ZSL_MAX_LEVEL, 0, &header_bytes) != 0)
            abort();
        z->mem = (uint64_t)sizeof(*z) + header_bytes;
    }
    z->rng_state = 0x2545F491u;
    return z;
}

void zsl_free(zskiplist *z)
{
    zsl_node *n;
    if (z == NULL)
        return;
    n = z->header->level[0].forward;
    while (n != NULL) {
        zsl_node *next = n->level[0].forward;
        free(n->member);
        free(n);
        n = next;
    }
    free(z->header->member);
    free(z->header);
    free(z);
}

int zsl_insert(zskiplist *z, double score, const char *member, size_t mlen)
{
    zsl_node *update[ZSL_MAX_LEVEL];
    uint32_t rank[ZSL_MAX_LEVEL];
    zsl_node *x;
    zsl_node *n;
    int level;
    int i;

    if (mlen > UINT32_MAX)
        return -1;
    x = z->header;
    for (i = z->level - 1; i >= 0; i--) {
        rank[i] = (i == z->level - 1) ? 0 : rank[i + 1];
        while (x->level[i].forward != NULL &&
               zsl_cmp(x->level[i].forward->score,
                       x->level[i].forward->member,
                       x->level[i].forward->mlen, score, member, mlen) < 0) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }
    level = zsl_random_level(z);
    if (level > z->level) {
        for (i = z->level; i < level; i++) {
            update[i] = z->header;
            update[i]->level[i].span = (uint32_t)z->length;
            rank[i] = 0;
        }
        z->level = level;
    }
    n = zsl_node_new(level, score, member, mlen);
    if (n == NULL)
        return -1;
    for (i = 0; i < level; i++) {
        n->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = n;
        /* rank[0]-rank[i] = level-0 nodes between update[i] and n */
        n->level[i].span =
            update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    for (i = level; i < z->level; i++)
        update[i]->level[i].span++;
    n->backward = (update[0] == z->header) ? NULL : update[0];
    if (n->level[0].forward != NULL)
        n->level[0].forward->backward = n;
    else
        z->tail = n;
    z->length++;
    {
        uint64_t node_bytes;
        if (zsl_node_bytes(level, mlen, &node_bytes) != 0)
            abort();
        z->mem += node_bytes;
    }
    return 0;
}

int zsl_delete(zskiplist *z, double score, const char *member, size_t mlen)
{
    zsl_node *update[ZSL_MAX_LEVEL];
    zsl_node *x;
    int i;
    int level = 0;

    x = z->header;
    for (i = z->level - 1; i >= 0; i--) {
        while (x->level[i].forward != NULL &&
               zsl_cmp(x->level[i].forward->score,
                       x->level[i].forward->member,
                       x->level[i].forward->mlen, score, member, mlen) < 0)
            x = x->level[i].forward;
        update[i] = x;
    }
    x = x->level[0].forward;
    if (x == NULL ||
        zsl_cmp(x->score, x->member, x->mlen, score, member, mlen) != 0)
        return 0;
    for (i = 0; i < z->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
            level = i + 1; /* highest level the node participated in */
        } else {
            update[i]->level[i].span -= 1;
        }
    }
    if (x->level[0].forward != NULL)
        x->level[0].forward->backward = x->backward;
    else
        z->tail = x->backward;
    while (z->level > 1 && z->header->level[z->level - 1].forward == NULL)
        z->level--;
    z->length--;
    {
        uint64_t node_bytes;
        if (zsl_node_bytes(level, x->mlen, &node_bytes) != 0)
            abort();
        z->mem -= node_bytes;
    }
    free(x->member);
    free(x);
    return 1;
}

long zsl_rank(zskiplist *z, double score, const char *member, size_t mlen)
{
    zsl_node *x = z->header;
    uint64_t r = 0;
    int i;
    for (i = z->level - 1; i >= 0; i--) {
        while (x->level[i].forward != NULL &&
               zsl_cmp(x->level[i].forward->score,
                       x->level[i].forward->member,
                       x->level[i].forward->mlen, score, member, mlen) <= 0) {
            r += x->level[i].span;
            x = x->level[i].forward;
        }
        if (x != z->header &&
            zsl_cmp(x->score, x->member, x->mlen, score, member, mlen) == 0)
            return (long)(r - 1);
    }
    return -1;
}

zsl_node *zsl_at(zskiplist *z, size_t idx)
{
    zsl_node *x = z->header;
    uint64_t traversed = 0;
    int i;
    /* zslGetElementByRank semantics are 1-based; idx is 0-based */
    for (i = z->level - 1; i >= 0; i--) {
        while (x->level[i].forward != NULL &&
               traversed + x->level[i].span <= (uint64_t)idx + 1) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == (uint64_t)idx + 1)
            return x;
    }
    return NULL;
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
        while (x->level[i].forward != NULL &&
               !zsl_gte_min(x->level[i].forward->score, r))
            x = x->level[i].forward;
    x = x->level[0].forward;
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
        while (x->level[i].forward != NULL && zsl_lte_max(x->level[i].forward->score, r))
            x = x->level[i].forward;
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
        n = n->level[0].forward;
    }
    return count;
}
