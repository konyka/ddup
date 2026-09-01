/* rhtable.c - Robin Hood hash table with incremental rehash; see rhtable.h. */
#include "core/rhtable.h"
#include "pal/pal_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RH_INIT_CAP 16
#define RH_MAX_LOAD_NUM 85 /* grow when size*100 > cap*85 */
#define RH_MAX_LOAD_DEN 100
#define RH_MIGRATE_BATCH 16 /* occupied buckets migrated per operation */

static int rh_slot_bytes(size_t cap, size_t *bytes)
{
    if (cap > SIZE_MAX / sizeof(rh_entry))
        return -1;
    *bytes = cap * sizeof(rh_entry);
    return 0;
}

static int rh_grow_capacity(size_t cap, size_t *new_cap)
{
    size_t doubled;
    if (cap > SIZE_MAX / 2)
        return -1;
    doubled = cap * 2;
    if (doubled > SIZE_MAX / sizeof(rh_entry))
        return -1;
    *new_cap = doubled;
    return 0;
}

/* ------------------------------------------------------------------ */
/* hash: wyhash (final v4, vendored in deps/wyhash) on platforms with  */
/* a 128-bit multiply; FNV-1a 64 + fmix64 fallback elsewhere.          */
/* ------------------------------------------------------------------ */
#if DDUP_HAS_WYHASH
#include "wyhash.h"
#endif

static uint64_t rh_hash(const char *key, size_t len)
{
#if DDUP_HAS_WYHASH
    return wyhash(key, len, 0, _wyp);
#else
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)key[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
#endif
}

/* Keep load-factor arithmetic off the insert hot path. */
static size_t rh_growth_limit(size_t cap)
{
    return (cap / RH_MAX_LOAD_DEN) * RH_MAX_LOAD_NUM +
           ((cap % RH_MAX_LOAD_DEN) * RH_MAX_LOAD_NUM) / RH_MAX_LOAD_DEN;
}

static rh_entry *rh_alloc_slots(size_t cap)
{
    size_t bytes;
    if (rh_slot_bytes(cap, &bytes) != 0) {
        fprintf(stderr, "ddup: hash table capacity overflow\n");
        exit(1);
    }
    rh_entry *s = malloc(bytes);
    if (!s) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    for (size_t i = 0; i < cap; i++)
        s[i].psl = -1;
    return s;
}

void rh_init(rh_table *t)
{
    t->slots = rh_alloc_slots(RH_INIT_CAP);
    t->cap = RH_INIT_CAP;
    t->size = 0;
    t->grow_at = rh_growth_limit(RH_INIT_CAP);
    t->old_slots = NULL;
    t->old_cap = 0;
    t->old_live = 0;
    t->migrate_pos = 0;
}

void rh_destroy(rh_table *t)
{
    for (size_t i = 0; i < t->cap; i++)
        if (t->slots[i].psl >= 0)
            free(t->slots[i].kv);
    for (size_t i = 0; i < t->old_cap; i++)
        if (t->old_slots[i].psl >= 0)
            free(t->old_slots[i].kv);
    free(t->slots);
    free(t->old_slots);
    t->slots = NULL;
    t->old_slots = NULL;
}

/* Robin Hood insert of an existing entry (takes ownership of kv). */
static void rh_insert_entry(rh_entry *slots, size_t cap, rh_entry e)
{
    size_t mask = cap - 1;
    size_t i = (size_t)e.hash & mask;
    int32_t dist = 0;
    for (;;) {
        if (slots[i].psl < 0) {
            e.psl = dist;
            slots[i] = e;
            return;
        }
        if (slots[i].psl < dist) {
            rh_entry tmp = slots[i];
            slots[i] = e;
            slots[i].psl = dist;
            e = tmp;
            dist = e.psl;
        }
        i = (i + 1) & mask;
        dist++;
    }
}

/* Find entry index in a slot array; -1 if absent. */
static long rh_find_in(const rh_entry *slots, size_t cap, uint64_t hash,
                       const char *key, size_t klen)
{
    size_t mask = cap - 1;
    size_t i = (size_t)hash & mask;
    int32_t dist = 0;
    for (;;) {
        const rh_entry *e = &slots[i];
        if (e->psl < 0 || e->psl < dist)
            return -1;
        if (e->hash == hash && e->klen == klen &&
            memcmp(e->kv, key, klen) == 0)
            return (long)i;
        i = (i + 1) & mask;
        dist++;
    }
}

/* Backward-shift deletion at index i. Frees the removed entry's kv. */
static void rh_delete_at(rh_entry *slots, size_t cap, size_t i)
{
    size_t mask = cap - 1;
    free(slots[i].kv);
    size_t j = i;
    for (;;) {
        size_t next = (j + 1) & mask;
        if (slots[next].psl <= 0) { /* empty or home slot: stop */
            slots[j].psl = -1;
            slots[j].kv = NULL;
            return;
        }
        slots[j] = slots[next];
        slots[j].psl--;
        j = next;
    }
}

/* Migrate up to RH_MIGRATE_BATCH occupied buckets from old to new table. */
static void rh_migrate_some_slow(rh_table *t)
{
    size_t moved = 0;
    while (moved < RH_MIGRATE_BATCH && t->old_live > 0) {
        if (t->migrate_pos >= t->old_cap)
            break; /* defensive: old_live should hit 0 first */
        rh_entry *e = &t->old_slots[t->migrate_pos++];
        if (e->psl >= 0) {
            rh_insert_entry(t->slots, t->cap, *e);
            e->psl = -1;
            e->kv = NULL;
            t->old_live--;
            moved++;
        }
    }
    if (t->old_live == 0) {
        free(t->old_slots);
        t->old_slots = NULL;
        t->old_cap = 0;
        t->migrate_pos = 0;
    }
}

/* Hot-path wrapper (Phase 36): every table op pays exactly one inlined
 * load + predictable branch when no migration is in flight; the migration
 * loop itself stays out of line. */
static inline void rh_migrate_some(rh_table *t)
{
    if (t->old_slots != NULL)
        rh_migrate_some_slow(t);
}

static void rh_maybe_grow(rh_table *t)
{
    if (t->old_slots)
        return; /* already migrating; new cap has headroom */
    if (t->size < SIZE_MAX && t->size + 1 <= t->grow_at)
        return;
    size_t new_cap;
    if (rh_grow_capacity(t->cap, &new_cap) != 0) {
        fprintf(stderr, "ddup: hash table capacity overflow\n");
        exit(1);
    }
    rh_entry *new_slots = rh_alloc_slots(new_cap);
    t->old_slots = t->slots;
    t->old_cap = t->cap;
    t->old_live = t->size;
    t->migrate_pos = 0;
    t->slots = new_slots;
    t->cap = new_cap;
    t->grow_at = rh_growth_limit(new_cap);
}

#ifdef DDUP_TESTING
int rh_test_slot_bytes(size_t cap, size_t *bytes)
{
    return rh_slot_bytes(cap, bytes);
}

int rh_test_grow_capacity(size_t cap, size_t *new_cap)
{
    return rh_grow_capacity(cap, new_cap);
}
#endif

size_t rh_size(const rh_table *t)
{
    return t->size;
}

void rh_each(const rh_table *t, rh_iter_fn fn, void *ctx)
{
    size_t i;
    for (i = 0; i < t->cap; i++) {
        const rh_entry *e = &t->slots[i];
        if (e->psl >= 0)
            fn(e->kv, e->klen, e->kv + e->klen, e->vlen, ctx);
    }
    for (i = 0; i < t->old_cap; i++) {
        const rh_entry *e = &t->old_slots[i];
        if (e->psl >= 0)
            fn(e->kv, e->klen, e->kv + e->klen, e->vlen, ctx);
    }
}

size_t rh_scan(rh_table *t, size_t cursor, size_t count, rh_scan_cb cb,
               void *ctx)
{
    size_t visited = 0;
    while (visited < count) {
        rh_entry *e;
        if (cursor < t->cap) {
            e = &t->slots[cursor];
        } else if (t->old_slots != NULL && cursor - t->cap < t->old_cap) {
            e = &t->old_slots[cursor - t->cap];
        } else {
            return 0; /* past both tables: iteration complete */
        }
        cursor++;
        if (e->psl < 0)
            continue;
        /* The callback may delete entries (freeing kv) and advance the
         * incremental rehash (freeing old_slots), so copy the entry views
         * out first, never touch e afterwards, and re-read all table
         * fields on the next loop iteration. */
        {
            const char *key = e->kv;
            size_t klen = e->klen;
            const char *val = e->kv + e->klen;
            size_t vlen = e->vlen;
            if (cb(key, klen, val, vlen, ctx))
                break;
        }
        visited++;
    }
    if (cursor >= t->cap + t->old_cap)
        return 0;
    return cursor;
}

int rh_get(rh_table *t, const char *key, size_t klen,
           const char **val, size_t *vlen)
{
    rh_migrate_some(t);
    uint64_t h = rh_hash(key, klen);
    const rh_entry *e = NULL;
    long i = rh_find_in(t->slots, t->cap, h, key, klen);
    if (i >= 0) {
        e = &t->slots[i];
    } else if (t->old_slots) {
        long oi = rh_find_in(t->old_slots, t->old_cap, h, key, klen);
        if (oi >= 0)
            e = &t->old_slots[oi];
    }
    if (!e)
        return 0;
    *val = e->kv + e->klen;
    *vlen = e->vlen;
    return 1;
}

int rh_get_touch(rh_table *t, const char *key, size_t klen,
                 const char **val, size_t *vlen, uint32_t meta)
{
    rh_migrate_some(t);
    uint64_t h = rh_hash(key, klen);
    rh_entry *e = NULL;
    long i = rh_find_in(t->slots, t->cap, h, key, klen);
    if (i >= 0) {
        e = &t->slots[i];
    } else if (t->old_slots) {
        long oi = rh_find_in(t->old_slots, t->old_cap, h, key, klen);
        if (oi >= 0)
            e = &t->old_slots[oi];
    }
    if (!e)
        return 0;
    e->meta = meta;
    *val = e->kv + e->klen;
    *vlen = e->vlen;
    return 1;
}

int rh_set(rh_table *t, const char *key, size_t klen,
           const char *val, size_t vlen)
{
    if (klen > UINT32_MAX || vlen > UINT32_MAX || klen > SIZE_MAX - vlen)
        return -1;
    rh_migrate_some(t);
    uint64_t h = rh_hash(key, klen);

    long i = rh_find_in(t->slots, t->cap, h, key, klen);
    rh_entry *target = NULL;
    if (i >= 0) {
        target = &t->slots[i];
    } else if (t->old_slots) {
        long oi = rh_find_in(t->old_slots, t->old_cap, h, key, klen);
        if (oi >= 0)
            target = &t->old_slots[oi];
    }

    if (target) {
        /* overwrite: keep key, replace the whole owned block */
        char *kv = malloc(klen + vlen);
        if (!kv) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        memcpy(kv, key, klen);
        memcpy(kv + klen, val, vlen);
        free(target->kv);
        target->kv = kv;
        target->vlen = (uint32_t)vlen;
        return 1;
    }

    rh_maybe_grow(t);
    rh_entry e;
    e.hash = h;
    e.klen = (uint32_t)klen;
    e.vlen = (uint32_t)vlen;
    e.kv = malloc(klen + vlen);
    if (!e.kv) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    memcpy(e.kv, key, klen);
    memcpy(e.kv + klen, val, vlen);
    e.psl = 0;
    e.meta = 0;
    rh_insert_entry(t->slots, t->cap, e);
    t->size++;
    return 0;
}

int rh_set_ex2(rh_table *t, const char *key, size_t klen, const char *v1,
               size_t n1, const char *v2, size_t n2, uint32_t meta,
               char **old_kv, size_t *old_vlen)
{
    size_t vlen;

    if (klen > UINT32_MAX || n1 > UINT32_MAX || n2 > UINT32_MAX ||
        n1 > SIZE_MAX - n2)
        return -1;
    vlen = n1 + n2;
    if (vlen > UINT32_MAX || klen > SIZE_MAX - vlen)
        return -1;

    rh_migrate_some(t);
    uint64_t h = rh_hash(key, klen);

    long i = rh_find_in(t->slots, t->cap, h, key, klen);
    rh_entry *target = NULL;
    if (i >= 0) {
        target = &t->slots[i];
    } else if (t->old_slots) {
        long oi = rh_find_in(t->old_slots, t->old_cap, h, key, klen);
        if (oi >= 0)
            target = &t->old_slots[oi];
    }

    if (target) {
        /* overwrite: keep key, swap in the new owned block, hand the old
         * one back unfreed (caller tears it down) */
        char *kv = malloc(klen + vlen);
        if (!kv) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        memcpy(kv, key, klen);
        memcpy(kv + klen, v1, n1);
        if (n2 > 0)
            memcpy(kv + klen + n1, v2, n2);
        *old_kv = target->kv;
        *old_vlen = target->vlen;
        target->kv = kv;
        target->vlen = (uint32_t)vlen;
        target->meta = meta;
        return 1;
    }

    rh_maybe_grow(t);
    rh_entry e;
    e.hash = h;
    e.klen = (uint32_t)klen;
    e.vlen = (uint32_t)vlen;
    e.kv = malloc(klen + vlen);
    if (!e.kv) {
        fprintf(stderr, "ddup: out of memory\n");
        exit(1);
    }
    memcpy(e.kv, key, klen);
    memcpy(e.kv + klen, v1, n1);
    if (n2 > 0)
        memcpy(e.kv + klen + n1, v2, n2);
    e.psl = 0;
    e.meta = meta;
    rh_insert_entry(t->slots, t->cap, e);
    t->size++;
    return 0;
}

int rh_set_ex(rh_table *t, const char *key, size_t klen, const char *val,
              size_t vlen, uint32_t meta, char **old_kv, size_t *old_vlen)
{
    return rh_set_ex2(t, key, klen, val, vlen, NULL, 0, meta, old_kv,
                      old_vlen);
}

uint32_t rh_meta_of(rh_table *t, const char *key, size_t klen)
{
    uint64_t h = rh_hash(key, klen);
    long i = rh_find_in(t->slots, t->cap, h, key, klen);
    if (i >= 0)
        return t->slots[i].meta;
    if (t->old_slots) {
        long oi = rh_find_in(t->old_slots, t->old_cap, h, key, klen);
        if (oi >= 0)
            return t->old_slots[oi].meta;
    }
    return 0;
}

/* First occupied slot at or after start (wrapping), or NULL. */
static const rh_entry *rh_scan_occupied(const rh_entry *slots, size_t cap,
                                        size_t start)
{
    for (size_t k = 0; k < cap; k++) {
        const rh_entry *e = &slots[(start + k) & (cap - 1)];
        if (e->psl >= 0)
            return e;
    }
    return NULL;
}

int rh_random_entry(rh_table *t, uint32_t rand, const char **key, size_t *klen,
                    const char **val, size_t *vlen, uint32_t *meta)
{
    const rh_entry *e = NULL;
    if (t->size == 0)
        return 0;
    e = rh_scan_occupied(t->slots, t->cap, (size_t)rand & (t->cap - 1));
    if (e == NULL && t->old_slots != NULL)
        e = rh_scan_occupied(t->old_slots, t->old_cap,
                             (size_t)rand & (t->old_cap - 1));
    if (e == NULL)
        return 0;
    *key = e->kv;
    *klen = e->klen;
    *val = e->kv + e->klen;
    *vlen = e->vlen;
    if (meta != NULL)
        *meta = e->meta;
    return 1;
}

int rh_touch(rh_table *t, const char *key, size_t klen, uint32_t meta)
{
    uint64_t h = rh_hash(key, klen);
    rh_entry *e = NULL;
    long i = rh_find_in(t->slots, t->cap, h, key, klen);
    if (i >= 0) {
        e = &t->slots[i];
    } else if (t->old_slots) {
        long oi = rh_find_in(t->old_slots, t->old_cap, h, key, klen);
        if (oi >= 0)
            e = &t->old_slots[oi];
    }
    if (e == NULL)
        return 0;
    e->meta = meta;
    return 1;
}

int rh_del(rh_table *t, const char *key, size_t klen)
{
    rh_migrate_some(t);
    uint64_t h = rh_hash(key, klen);
    long i = rh_find_in(t->slots, t->cap, h, key, klen);
    if (i >= 0) {
        rh_delete_at(t->slots, t->cap, (size_t)i);
        t->size--;
        return 1;
    }
    if (t->old_slots) {
        long oi = rh_find_in(t->old_slots, t->old_cap, h, key, klen);
        if (oi >= 0) {
            rh_delete_at(t->old_slots, t->old_cap, (size_t)oi);
            t->old_live--;
            t->size--;
            return 1;
        }
    }
    return 0;
}
