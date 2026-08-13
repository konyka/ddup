/* skiplist.h - classic Redis-style skip list with per-level span.
 *
 * Ordering is (score, member bytes): scores ascending, equal scores ordered
 * lexicographically by member (same rule as Redis zsets). Per-level span
 * (number of level-0 steps each forward link covers) gives O(log N)
 * ZRANK/index access, same algorithm as Redis zsl. Member kept as a
 * separate owned allocation. Random levels use an internal xorshift32
 * (deterministic seed; reseed via rng_state for tests).
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
    char *member; /* owned copy (mlen bytes) */
    struct zsl_level {
        struct zsl_node *forward;
        uint32_t span; /* level-0 nodes this link skips over */
    } level[];        /* [level] entries */
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
/* Returns 0 on success or -1 when mlen cannot be represented safely. */
int zsl_insert(zskiplist *z, double score, const char *member, size_t mlen);

/* Delete by exact (score, member). Returns 1 if found. */
int zsl_delete(zskiplist *z, double score, const char *member, size_t mlen);

/* 0-based rank via span walk (O(log N)); -1 if absent. */
long zsl_rank(zskiplist *z, double score, const char *member, size_t mlen);

/* Node at 0-based index via span walk (O(log N)); NULL when out of range. */
zsl_node *zsl_at(zskiplist *z, size_t idx);

/* Range queries: first/last node inside spec (NULL if empty), and the
 * number of nodes inside spec. */
zsl_node *zsl_first_in_range(zskiplist *z, const zrangespec *r);
zsl_node *zsl_last_in_range(zskiplist *z, const zrangespec *r);
size_t zsl_count_in_range(zskiplist *z, const zrangespec *r);

/* Lexicographic (member-only) range bound, as in Redis zlexrangespec.
 * inf: -1 = "-" (negative infinity), +1 = "+" (positive infinity),
 * 0 = finite member bytes s/len; ex marks an open interval "(x". */
typedef struct zlexbound {
    const char *s;
    size_t len;
    int ex;
    int inf;
} zlexbound;

typedef struct zlexrangespec {
    zlexbound min;
    zlexbound max;
} zlexrangespec;

/* Lex range queries over the (score, member) order; only defined when
 * all scores are equal, same caveat as Redis. first/last node inside
 * spec (NULL if empty). Traverse from first to last (pointer equality)
 * to enumerate the range. */
zsl_node *zsl_first_in_lex_range(zskiplist *z, const zlexrangespec *r);
zsl_node *zsl_last_in_lex_range(zskiplist *z, const zlexrangespec *r);

#endif /* DDUP_SKIPLIST_H */
