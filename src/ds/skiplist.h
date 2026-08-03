/* skiplist.h - classic Redis-style skip list (simplified variant).
 *
 * Ordering is (score, member bytes): scores ascending, equal scores ordered
 * lexicographically by member (same rule as Redis zsets).
 *
 * Simplifications vs Redis zsl: no per-level span (ZRANK/index access walk
 * level 0 — documented in docs/architecture.md), member kept as a separate
 * owned allocation. Random levels use an internal xorshift32 (deterministic
 * seed; reseed via rng_state for tests).
 */
#ifndef DDUP_SKIPLIST_H
#define DDUP_SKIPLIST_H

#include <stddef.h>
#include <stdint.h>

#define ZSL_MAX_LEVEL 32

typedef struct zsl_node {
    double score;
    struct zsl_node *backward;
    uint32_t mlen;
    char *member;               /* owned copy (mlen bytes) */
    struct zsl_node *forward[]; /* [level] entries */
} zsl_node;

/* Score range: min/max with optional exclusive flags. Use -inf/+inf
 * doubles for open ends. Range semantics are score-only (all members
 * sharing a boundary score are in or out together). */
typedef struct zrangespec {
    double min;
    double max;
    int minex;
    int maxex;
} zrangespec;

typedef struct zskiplist {
    zsl_node *header;
    zsl_node *tail;
    size_t length;
    int level;
    uint64_t mem; /* header + nodes + members, incremental */
    uint32_t rng_state;
} zskiplist;

zskiplist *zsl_create(void);
void zsl_free(zskiplist *z);

/* Insert a NEW (score, member); the caller guarantees the member is not
 * present (the zset dict enforces this). */
void zsl_insert(zskiplist *z, double score, const char *member, size_t mlen);

/* Delete by exact (score, member). Returns 1 if found. */
int zsl_delete(zskiplist *z, double score, const char *member, size_t mlen);

/* 0-based rank via level-0 walk; -1 if absent. */
long zsl_rank(zskiplist *z, double score, const char *member, size_t mlen);

/* Node at 0-based index via level-0 walk; NULL when out of range. */
zsl_node *zsl_at(zskiplist *z, size_t idx);

/* Range queries: first/last node inside spec (NULL if empty), and the
 * number of nodes inside spec. */
zsl_node *zsl_first_in_range(zskiplist *z, const zrangespec *r);
zsl_node *zsl_last_in_range(zskiplist *z, const zrangespec *r);
size_t zsl_count_in_range(zskiplist *z, const zrangespec *r);

#endif /* DDUP_SKIPLIST_H */
