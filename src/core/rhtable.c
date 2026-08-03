/* rhtable.c - Robin Hood hash table with incremental rehash; see rhtable.h. */
#include "core/rhtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RH_INIT_CAP 16
#define RH_MAX_LOAD_NUM 85 /* grow when size*100 > cap*85 */
#define RH_MAX_LOAD_DEN 100
#define RH_MIGRATE_BATCH 16 /* occupied buckets migrated per operation */

/* ------------------------------------------------------------------ */
/* hash: FNV-1a 64 + murmur3 fmix64 finalizer (good low-bit spread).  */
/* wyhash/xxh3 is a documented future optimization (docs/roadmap).    */
/* ------------------------------------------------------------------ */
static uint64_t rh_hash(const char *key, size_t len)
{
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
}

static rh_entry *rh_alloc_slots(size_t cap)
{
    rh_entry *s = malloc(cap * sizeof(rh_entry));
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
static void rh_migrate_some(rh_table *t)
{
    if (!t->old_slots)
        return;
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

static void rh_maybe_grow(rh_table *t)
{
    if (t->old_slots)
        return; /* already migrating; new cap has headroom */
    if ((t->size + 1) * RH_MAX_LOAD_DEN <= t->cap * RH_MAX_LOAD_NUM)
        return;
    size_t new_cap = t->cap * 2;
    rh_entry *new_slots = rh_alloc_slots(new_cap);
    t->old_slots = t->slots;
    t->old_cap = t->cap;
    t->old_live = t->size;
    t->migrate_pos = 0;
    t->slots = new_slots;
    t->cap = new_cap;
}

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

void rh_set(rh_table *t, const char *key, size_t klen,
            const char *val, size_t vlen)
{
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
        return;
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
