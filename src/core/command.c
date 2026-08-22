/* command.c - RESP command dispatch; see command.h.
 *
 * Expiration model: `db.expires` maps key -> 8-byte absolute wall-ms expiry.
 * All key lookups go through db_get()/db_expire_if_needed() so expired keys
 * are treated as missing (lazy expiration). Overwriting or deleting a key
 * clears its expiry (SET/INCR/APPEND/MSET/DEL semantics here).
 */
#include "core/command.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds/obj.h"
#include "ds/glob.h"
#include "pal/pal_time.h"

#include <math.h>

#include "core/session.h"
#include "core/hashslot.h"
#include "core/snapshot.h"
#include "core/tier.h"
#include "core/migrate.h"
#include "core/script.h"

static void free_obj_cb(const char *key, size_t klen, const char *val,
                        size_t vlen, void *ctx);

/* Unified command metadata (defined with CMD_TABLE at the bottom; this
 * early declaration makes the table usable file-wide, e.g. in INFO). */
typedef struct cmd_entry {
    const char *name;
    uint16_t    id;
    int         min_argc;
    int         max_argc;   /* -1 = unbounded */
    int         parity;     /* 0 any, 1 odd, 2 even */
    uint8_t     flags;
} cmd_entry;

/* table row for a command id (defined next to CMD_TABLE at the bottom) */
static const cmd_entry *cmd_table_entry(uint16_t id);

/* string view of an argv item (defined with the reply helpers below) */
static int arg_str(const resp_value *v, const char **s, size_t *len);

static void command_dispatch(session *s, const resp_value *argv, size_t argc,
                             resp_buf *out, uint64_t now_ms);
static int db_tier_materialize(db *d, const char *key, size_t klen,
                               const char **val, size_t *vlen,
                               uint64_t now_ms);
static int blocking_pop_try(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out, uint64_t now_ms,
                            uint64_t *deadline_ms);
static void blocking_timeout_reply(resp_buf *out, uint16_t cmd_id);

void db_init(db *d)
{
    script_set_command_fn(command_dispatch);
    rh_init(&d->table);
    rh_init(&d->expires);
    rh_init(&d->keyvers);
    d->watch_refs = 0;
    d->flush_epoch = 0;
    d->expired_keys = 0;
    d->evicted_keys = 0;
    d->used_memory = 0;
    d->dirty = 0;
    d->maxmemory = 0;
    d->maxmemory_policy = DB_POLICY_ALLKEYS_LRU;
    d->cluster_enabled = 0;
    d->node_id[0] = '\0';
    cluster_nodes_init(d);
    memset(d->slot_owner, 0xFF, sizeof(d->slot_owner));
    memset(d->slot_migrating, 0xFF, sizeof(d->slot_migrating));
    memset(d->slot_importing, 0xFF, sizeof(d->slot_importing));
    d->slot_owner_dirty = 1;
    d->cluster_changes = 0;
    d->cluster_current_epoch = 1;
    d->cluster_node_timeout_ms = 15000; /* Redis default node-timeout */
    d->fail_broadcast_id[0] = '\0';
    strcpy(d->cluster_ip, "0.0.0.0");
    d->cluster_port = 0;
    d->snapshot_path = NULL;
    d->last_save = 0;
    d->rng_state = 0x9E3779B9u; /* nonzero xorshift seed */
    rh_init(&d->scripts);
    rh_init(&d->function_libs);
    d->tier = NULL;
    d->tier_db_index = 0;
    d->tier_enabled = 0;
    d->tier_dir[0] = '\0';
    d->tier_max_disk_bytes = 0;
    d->tier_io_error = 0;
    d->lua_state = NULL;
}

void db_set_tier(db *d, tier_store *tier, int db_index)
{
    d->tier = tier;
    d->tier_db_index = db_index;
    d->tier_enabled = tier != NULL;
}

void db_destroy(db *d)
{
    script_cleanup(d);
    rh_each(&d->table, free_obj_cb, NULL);
    rh_destroy(&d->table);
    rh_destroy(&d->expires);
    rh_destroy(&d->keyvers);
}

void db_flush(db *d)
{
    rh_each(&d->table, free_obj_cb, NULL);
    rh_destroy(&d->table);
    rh_destroy(&d->expires);
    rh_init(&d->table);
    rh_init(&d->expires);
    d->used_memory = 0;
    if (d->tier != NULL)
        (void)tier_flush_db(d->tier, (unsigned int)d->tier_db_index);
    memset(d->cmd_calls, 0, sizeof(d->cmd_calls));
    memset(d->cmd_usecs, 0, sizeof(d->cmd_usecs));
}

/* ------------------------------------------------------------------ */
/* db-level helpers                                                   */
/* ------------------------------------------------------------------ */

static void put_u64(char buf[8], uint64_t v)
{
    memcpy(buf, &v, 8);
}

static uint64_t get_u64(const char *buf)
{
    uint64_t v;
    memcpy(&v, buf, 8);
    return v;
}

int db_touch_key(db *d, const char *key, size_t klen)
{
    const char *v;
    size_t vl;
    uint64_t ver = 0;
    char b[8];
    if (klen > UINT32_MAX)
        return -1;
    /* version bumps are only needed while at least one WATCH is active */
    if (d->watch_refs == 0) {
        d->dirty++;
        return 0;
    }
    if (rh_get(&d->keyvers, key, klen, &v, &vl) && vl == 8)
        ver = get_u64(v);
    put_u64(b, ver + 1);
    if (rh_set(&d->keyvers, key, klen, b, 8) < 0)
        return -1;
    d->dirty++;
    return 0;
}

uint64_t db_key_version(db *d, const char *key, size_t klen)
{
    const char *v;
    size_t vl;
    if (!rh_get(&d->keyvers, key, klen, &v, &vl) || vl != 8)
        return 0;
    return get_u64(v);
}

/* Per-entry memory estimate: slot + malloc overhead + key + value bytes. */
static uint64_t entry_bytes(size_t klen, size_t vlen)
{
    return (uint64_t)sizeof(rh_entry) + 16 + klen + vlen;
}

/* 24-bit LRU clock in seconds resolution (like Redis LRU_CLOCK). */
static uint32_t lru_clock(uint64_t now_ms)
{
    return (uint32_t)((now_ms / 1000ULL) & 0xFFFFFFu);
}

int db_expire_if_needed(db *d, const char *key, size_t klen, uint64_t now_ms)
{
    const char *v;
    size_t vl;
    if (klen > UINT32_MAX)
        return 0;
    /* fast path: nothing has a TTL (O(1), skips the expires-table lookup) */
    if (rh_size(&d->expires) == 0)
        return 0;
    if (!rh_get(&d->expires, key, klen, &v, &vl) || vl != 8)
        return 0;
    if (get_u64(v) > now_ms)
        return 0;
    rh_del(&d->expires, key, klen);
    d->used_memory -= entry_bytes(klen, 8);
    if (rh_get(&d->table, key, klen, &v, &vl)) {
        if (d->tier != NULL && obj_is_tier(v, vl)) {
            uint64_t rid = 0, ignored = 0;
            obj_tier_unpack(v, vl, &rid, &ignored);
            if (tier_del(d->tier, rid) != 0)
                d->tier_io_error = 1;
        }
        d->used_memory -= entry_bytes(klen, vl) + obj_extra_mem(v, vl);
        obj_free_value(v, vl);
        rh_del(&d->table, key, klen);
    }
    db_touch_key(d, key, klen);
    d->expired_keys++;
    return 1;
}

/* Expire-aware lookup; a hit also refreshes the LRU clock (single probe). */
static int db_get(db *d, const char *key, size_t klen, const char **val,
                  size_t *vlen, uint64_t now_ms)
{
    db_expire_if_needed(d, key, klen, now_ms);
    if (!rh_get_touch(&d->table, key, klen, val, vlen, lru_clock(now_ms)))
        return 0;
    if (obj_is_tier(*val, *vlen))
        return db_tier_materialize(d, key, klen, val, vlen, now_ms);
    return 1;
}

/* Overwrite a value blob (tagged; see ds/obj.h); clears any expiry;
 * refreshes the LRU clock; frees/adjusts a replaced object. */
static int db_set_kv(db *d, const char *key, size_t klen, const char *val,
                     size_t vlen, uint64_t now_ms)
{
    char *old_kv;
    size_t old_vlen;
    int set_rc;
    if (klen > UINT32_MAX)
        return -1;
    set_rc = rh_set_ex(&d->table, key, klen, val, vlen, lru_clock(now_ms),
                       &old_kv, &old_vlen);
    if (set_rc < 0)
        return -1;
    /* expires probe only when the table is non-empty (common case: no
     * TTLs at all, skip the hash+probe entirely) */
    if (rh_size(&d->expires) > 0) {
        const char *old;
        size_t oldl;
        if (rh_get(&d->expires, key, klen, &old, &oldl)) {
            rh_del(&d->expires, key, klen);
            d->used_memory -= entry_bytes(klen, 8);
        }
    }
    if (set_rc == 1) {
        const char *oldv = old_kv + klen;
        if (d->tier != NULL && obj_is_tier(oldv, old_vlen)) {
            uint64_t rid = 0, ignored = 0;
            obj_tier_unpack(oldv, old_vlen, &rid, &ignored);
            if (tier_del(d->tier, rid) != 0)
                d->tier_io_error = 1;
        }
        d->used_memory -=
            entry_bytes(klen, old_vlen) + obj_extra_mem(oldv, old_vlen);
        obj_free_value(oldv, old_vlen);
        free(old_kv);
    }
    d->used_memory += entry_bytes(klen, vlen) + obj_extra_mem(val, vlen);
    (void)db_touch_key(d, key, klen);
    return 0;
}

/* String store with the tag built into the kv block directly (Phase 28):
 * one allocation, no temporary concatenation. Strings have no extra
 * object memory, so accounting skips obj_extra_mem. */
static int db_set_kv_string(db *d, const char *key, size_t klen,
                            const char *val, size_t vlen, uint64_t now_ms)
{
    char *old_kv;
    size_t old_vlen;
    const char tag = (char)DDUP_OBJ_STRING;
    int set_rc;
    if (klen > UINT32_MAX)
        return -1;
    set_rc = rh_set_ex2(&d->table, key, klen, &tag, 1, val, vlen,
                        lru_clock(now_ms), &old_kv, &old_vlen);
    if (set_rc < 0)
        return -1;
    if (rh_size(&d->expires) > 0) {
        const char *old;
        size_t oldl;
        if (rh_get(&d->expires, key, klen, &old, &oldl)) {
            rh_del(&d->expires, key, klen);
            d->used_memory -= entry_bytes(klen, 8);
        }
    }
    if (set_rc == 1) {
        const char *oldv = old_kv + klen;
        if (d->tier != NULL && obj_is_tier(oldv, old_vlen)) {
            uint64_t rid = 0, ignored = 0;
            obj_tier_unpack(oldv, old_vlen, &rid, &ignored);
            if (tier_del(d->tier, rid) != 0)
                d->tier_io_error = 1;
        }
        d->used_memory -=
            entry_bytes(klen, old_vlen) + obj_extra_mem(oldv, old_vlen);
        obj_free_value(oldv, old_vlen);
        free(old_kv);
    }
    d->used_memory += entry_bytes(klen, vlen + 1);
    (void)db_touch_key(d, key, klen);
    return 0;
}

/* Store a string payload (fused tag+value write, no temporary blob). */
static int db_set_string(db *d, const char *key, size_t klen,
                         const char *val, size_t vlen, uint64_t now_ms)
{
    return db_set_kv_string(d, key, klen, val, vlen, now_ms);
}

/* Delete key and expiry (and any owned object). Returns 1 if existed. */
int db_del_kv(db *d, const char *key, size_t klen)
{
    const char *old;
    size_t oldl;
    int existed;
    if (klen > UINT32_MAX)
        return 0;
    if (rh_get(&d->expires, key, klen, &old, &oldl)) {
        rh_del(&d->expires, key, klen);
        d->used_memory -= entry_bytes(klen, 8);
    }
    existed = rh_get(&d->table, key, klen, &old, &oldl);
    if (existed) {
        if (d->tier != NULL && obj_is_tier(old, oldl)) {
            uint64_t rid = 0, ignored = 0;
            obj_tier_unpack(old, oldl, &rid, &ignored);
            if (tier_del(d->tier, rid) != 0)
                d->tier_io_error = 1;
        }
        d->used_memory -= entry_bytes(klen, oldl) + obj_extra_mem(old, oldl);
        obj_free_value(old, oldl);
        rh_del(&d->table, key, klen);
        db_touch_key(d, key, klen);
    }
    return existed;
}

/* Delete key and expiry WITHOUT freeing the owned object: RENAME/RENAMENX
 * move the value blob to the destination key first, and for pointer-backed
 * types (hash/list/set/zset) the blob copy shares the object pointer, so
 * the source teardown must not free it. Object extra memory stays
 * accounted once, under the dst entry added by db_set_kv. Returns 1 if
 * the key existed. */
static int db_del_kv_keep_obj(db *d, const char *key, size_t klen)
{
    const char *old;
    size_t oldl;
    int existed;
    if (klen > UINT32_MAX)
        return 0;
    if (rh_get(&d->expires, key, klen, &old, &oldl)) {
        rh_del(&d->expires, key, klen);
        d->used_memory -= entry_bytes(klen, 8);
    }
    existed = rh_get(&d->table, key, klen, &old, &oldl);
    if (existed) {
        d->used_memory -= entry_bytes(klen, oldl);
        rh_del(&d->table, key, klen);
        db_touch_key(d, key, klen);
    }
    return existed;
}

static int db_set_expiry(db *d, const char *key, size_t klen, uint64_t when_ms)
{
    char b[8];
    const char *old;
    size_t oldl;
    if (klen > UINT32_MAX)
        return -1;
    put_u64(b, when_ms);
    int is_new = !rh_get(&d->expires, key, klen, &old, &oldl);
    if (rh_set(&d->expires, key, klen, b, 8) < 0)
        return -1;
    if (is_new)
        d->used_memory += entry_bytes(klen, 8);
    return 0;
}

int db_install_blob(db *d, const char *key, size_t klen, const char *blob,
                    size_t bloblen, uint64_t now_ms)
{
    return db_set_kv(d, key, klen, blob, bloblen, now_ms);
}

int db_install_expiry(db *d, const char *key, size_t klen, uint64_t when_ms)
{
    return db_set_expiry(d, key, klen, when_ms);
}

static int db_tier_materialize(db *d, const char *key, size_t klen,
                               const char **val, size_t *vlen,
                               uint64_t now_ms)
{
    uint64_t rid = 0;
    uint64_t expire = 0;
    uint64_t cold_expire = 0;
    char *payload = NULL;
    size_t plen = 0;
    int rc;

    if (d->tier == NULL) {
        d->tier_io_error = 1;
        return 0;
    }
    obj_tier_unpack(*val, *vlen, &rid, &expire);
    if (tier_get(d->tier, rid, &payload, &plen, &cold_expire) != 0) {
        d->tier_io_error = 1;
        return 0;
    }
    rc = snapshot_restore_key(d, key, klen, payload, plen, cold_expire, 1,
                              now_ms);
    free(payload);
    if (rc != 0) {
        d->tier_io_error = 1;
        return 0;
    }
    if (tier_del(d->tier, rid) != 0)
        d->tier_io_error = 1;
    if (!rh_get_touch(&d->table, key, klen, val, vlen, lru_clock(now_ms))) {
        d->tier_io_error = 1;
        return 0;
    }
    return 1;
}

/* rh_each callback: free any owned object value (FLUSHDB teardown). */
static void free_obj_cb(const char *key, size_t klen, const char *val,
                        size_t vlen, void *ctx)
{
    (void)key;
    (void)klen;
    (void)ctx;
    obj_free_value(val, vlen);
}

/* xorshift32 over db->rng_state (deterministic; tests may reseed). */
static uint32_t db_rand(db *d)
{
    uint32_t x = d->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    d->rng_state = x;
    return x;
}

size_t db_active_expire(db *d, uint64_t now_ms, int max_samples)
{
    size_t expired_total = 0;
    int iter;
    if (max_samples <= 0)
        return 0;
    for (iter = 0; iter < 10; iter++) {
        int sampled = 0, expired = 0;
        int i;
        for (i = 0; i < max_samples; i++) {
            const char *key, *val;
            size_t klen, vlen;
            if (!rh_random_entry(&d->expires, db_rand(d), &key, &klen, &val,
                                 &vlen, NULL))
                break; /* expires table empty */
            sampled++;
            if (vlen == 8 && get_u64(val) <= now_ms) {
                /* the entry is freed on delete: copy the key first */
                char stackbuf[256];
                char *kb = stackbuf;
                if (klen > sizeof(stackbuf))
                    kb = (char *)malloc(klen);
                memcpy(kb, key, klen);
                db_expire_if_needed(d, kb, klen, now_ms);
                if (kb != stackbuf)
                    free(kb);
                expired++;
            }
        }
        expired_total += (size_t)expired;
        if (sampled == 0 || expired * 4 <= sampled)
            break; /* <= 25% expired: stop */
    }
    return expired_total;
}

/* ------------------------------------------------------------------ */
/* eviction                                                           */
/* ------------------------------------------------------------------ */

#define DB_EVICT_SAMPLES 5

/* Serialize the current hot value into the cold layer and replace the table
 * entry with a compact tier-ref. The caller owns the key bytes. Returns 1 on
 * offload, 0 when offload does not apply, -1 on tier failure (fail-closed). */
static int db_tier_offload(db *d, const char *key, size_t klen,
                           const char *val, size_t vlen, uint64_t now_ms)
{
    resp_buf sb;
    uint64_t expire_ms = 0;
    uint64_t rid = 0;
    unsigned char ref[17];
    const char *ev;
    size_t el;
    char *old_kv = NULL;
    size_t old_vlen = 0;
    int set_rc;

    if (d->tier == NULL || !d->tier_enabled || obj_is_tier(val, vlen))
        return 0;
    resp_buf_init(&sb);
    if (snapshot_dump_key(d, key, klen, &sb) != 0) {
        resp_buf_free(&sb);
        d->tier_io_error = 1;
        return -1;
    }
    if (rh_get(&d->expires, key, klen, &ev, &el) && el == 8)
        expire_ms = get_u64(ev);
    if (tier_put(d->tier, (unsigned int)d->tier_db_index, key, klen,
                 sb.data, sb.len, expire_ms, &rid) != 0) {
        resp_buf_free(&sb);
        d->tier_io_error = 1;
        return -1;
    }
    obj_tier_pack((char *)ref, rid, expire_ms);
    set_rc = rh_set_ex(&d->table, key, klen, (const char *)ref, sizeof(ref),
                       lru_clock(now_ms), &old_kv, &old_vlen);
    if (set_rc < 0) {
        (void)tier_del(d->tier, rid);
        resp_buf_free(&sb);
        d->tier_io_error = 1;
        return -1;
    }
    if (set_rc == 1) {
        const char *oldv = old_kv + klen;
        d->used_memory -=
            entry_bytes(klen, old_vlen) + obj_extra_mem(oldv, old_vlen);
        obj_free_value(oldv, old_vlen);
        free(old_kv);
    }
    d->used_memory += entry_bytes(klen, sizeof(ref));
    resp_buf_free(&sb);
    return 1;
}

/* Sample up to DB_EVICT_SAMPLES keys and evict the one with the oldest
 * 24-bit LRU clock. Returns 1 if a key was evicted. */
static int db_evict_one(db *d, uint64_t now_ms)
{
    const char *cand_key[DB_EVICT_SAMPLES];
    size_t cand_klen[DB_EVICT_SAMPLES];
    uint32_t cand_meta[DB_EVICT_SAMPLES];
    int n = 0;
    int oldest = 0;
    int i;

    for (i = 0; i < DB_EVICT_SAMPLES; i++) {
        const char *key, *val;
        size_t klen, vlen;
        uint32_t meta;
        if (!rh_random_entry(&d->table, db_rand(d), &key, &klen, &val, &vlen,
                             &meta))
            break;
        cand_key[n] = key;
        cand_klen[n] = klen;
        cand_meta[n] = meta;
        n++;
    }
    if (n == 0)
        return 0;
    for (i = 1; i < n; i++)
        if (cand_meta[i] < cand_meta[oldest])
            oldest = i;

    /* the sampled views dangle once we mutate: copy the victim key first */
    {
        char stackbuf[256];
        char *kb = stackbuf;
        size_t kl = cand_klen[oldest];
        if (kl > sizeof(stackbuf))
            kb = (char *)malloc(kl);
        memcpy(kb, cand_key[oldest], kl);
        if (d->tier != NULL) {
            const char *v;
            size_t vl;
            int rc;
            if (rh_get(&d->table, kb, kl, &v, &vl))
                rc = db_tier_offload(d, kb, kl, v, vl, now_ms);
            else
                rc = 0;
            if (rc > 0) {
                d->evicted_keys++;
                if (kb != stackbuf)
                    free(kb);
                return 1;
            }
            if (rc < 0) {
                /* tier failure: keep the hot value and stop eviction */
                if (kb != stackbuf)
                    free(kb);
                return 0;
            }
        }
        db_del_kv(d, kb, kl);
        if (kb != stackbuf)
            free(kb);
    }
    d->evicted_keys++;
    return 1;
}

static void db_evict_if_needed(db *d, uint64_t now_ms)
{
    if (d->maxmemory == 0)
        return;
    while (d->used_memory > d->maxmemory && rh_size(&d->table) > 0) {
        if (!db_evict_one(d, now_ms))
            break;
    }
}

static const char OOM_MSG[] =
    "OOM command not allowed when used memory > 'maxmemory'.";
static const char TIER_IO_MSG[] = "ERR tiered storage I/O error";

static void tier_io_reply(resp_buf *out)
{
    out->len = 0;
    resp_write_error(out, TIER_IO_MSG, sizeof(TIER_IO_MSG) - 1);
}

/* noeviction policy: reject writes while over the cap. */
static int oom_blocked(db *d, resp_buf *out)
{
    if (d->maxmemory != 0 && d->maxmemory_policy == DB_POLICY_NOEVICTION &&
        d->used_memory > d->maxmemory) {
        resp_write_error(out, OOM_MSG, sizeof(OOM_MSG) - 1);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* object fetch helpers (hash/list commands)                          */
/* ------------------------------------------------------------------ */

static const char WRONGTYPE_MSG[] =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

static void wrongtype(resp_buf *out)
{
    resp_write_error(out, WRONGTYPE_MSG, sizeof(WRONGTYPE_MSG) - 1);
}

/* String payload of a value blob; writes WRONGTYPE and returns 0 if the
 * value is not a string. */
static int as_string(resp_buf *out, const char *val, size_t vlen,
                     const char **s, size_t *len)
{
    if (obj_tag_of(val, vlen) != DDUP_OBJ_STRING) {
        wrongtype(out);
        return 0;
    }
    obj_str(val, vlen, s, len);
    return 1;
}

/* rh_each callback for CLUSTER COUNTKEYSINSLOT/GETKEYSINSLOT. */
typedef struct slot_scan_ctx {
    uint32_t slot;
    long long limit;   /* -1 = count only */
    long long emitted;
    resp_buf *out;
} slot_scan_ctx;

static void slot_scan_cb(const char *key, size_t klen, const char *v,
                         size_t vlen, void *c)
{
    slot_scan_ctx *ctx = (slot_scan_ctx *)c;
    (void)v;
    (void)vlen;
    if (hash_slot(key, klen) != ctx->slot)
        return;
    if (ctx->limit < 0) {
        ctx->emitted++;
        return;
    }
    if (ctx->emitted < ctx->limit) {
        resp_write_bulk(ctx->out, key, klen);
        ctx->emitted++;
    }
}

static void storage_length_error(resp_buf *out)
{
    static const char E[] = "ERR key or value length is not representable";
    resp_write_error(out, E, sizeof(E) - 1);
}

static int parse_i64(const char *s, size_t len, long long *out);

static int storage_key_ok(size_t klen)
{
    return klen <= UINT32_MAX;
}

static int storage_string_ok(size_t klen, size_t vlen)
{
    return storage_key_ok(klen) && vlen < UINT32_MAX;
}

static int bitmap_offset(const char *s, size_t len, size_t *out)
{
    long long n;
    if (!parse_i64(s, len, &n) || n < 0 ||
        (unsigned long long)n > (unsigned long long)SIZE_MAX)
        return 0;
    *out = (size_t)n;
    return 1;
}

static unsigned bitmap_popcount_byte(unsigned char byte)
{
    byte = (unsigned char)(byte - ((byte >> 1) & 0x55U));
    byte = (unsigned char)((byte & 0x33U) + ((byte >> 2) & 0x33U));
    return (unsigned)((byte + (byte >> 4)) & 0x0fU);
}

static long long bitmap_pos_byte(unsigned char byte, int bit)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (((byte >> (7 - i)) & 1U) == (unsigned)bit)
            return i;
    }
    return -1;
}

/* Fetch the hash object for key; create when missing && create != 0.
 * Returns 1 (*h set), 0 missing, -1 WRONGTYPE (reply written). */
static int get_hash(db *d, resp_buf *out, const char *k, size_t kl, int create,
                    uint64_t now, obj_hash **h)
{
    const char *v;
    size_t vl;
    if (!db_get(d, k, kl, &v, &vl, now)) {
        char blob[9];
        obj_hash *nh;
        if (!create)
            return 0;
        nh = obj_hash_new();
        obj_pack_ptr(blob, DDUP_OBJ_HASH, nh);
        if (db_set_kv(d, k, kl, blob, 9, now) != 0) {
            obj_hash_free(nh);
            storage_length_error(out);
            return -1;
        }
        *h = nh;
        return 1;
    }
    if (obj_tag_of(v, vl) != DDUP_OBJ_HASH) {
        wrongtype(out);
        return -1;
    }
    *h = (obj_hash *)obj_unpack_ptr(v, vl);
    return 1;
}

/* Fetch the list object for key; create when missing && create != 0.
 * Returns 1 (*l set), 0 missing, -1 WRONGTYPE (reply written). */
static int get_list(db *d, resp_buf *out, const char *k, size_t kl, int create,
                    uint64_t now, obj_list **l)
{
    const char *v;
    size_t vl;
    if (!db_get(d, k, kl, &v, &vl, now)) {
        char blob[9];
        obj_list *nl;
        if (!create)
            return 0;
        nl = obj_list_new();
        obj_pack_ptr(blob, DDUP_OBJ_LIST, nl);
        if (db_set_kv(d, k, kl, blob, 9, now) != 0) {
            obj_list_free(nl);
            storage_length_error(out);
            return -1;
        }
        *l = nl;
        return 1;
    }
    if (obj_tag_of(v, vl) != DDUP_OBJ_LIST) {
        wrongtype(out);
        return -1;
    }
    *l = (obj_list *)obj_unpack_ptr(v, vl);
    return 1;
}

/* Fetch the set object for key; create when missing && create != 0.
 * Returns 1 (*s set), 0 missing, -1 WRONGTYPE (reply written). */
static int get_set(db *d, resp_buf *out, const char *k, size_t kl, int create,
                   uint64_t now, obj_set **s)
{
    const char *v;
    size_t vl;
    if (!db_get(d, k, kl, &v, &vl, now)) {
        char blob[9];
        obj_set *ns;
        if (!create)
            return 0;
        ns = obj_set_new();
        obj_pack_ptr(blob, DDUP_OBJ_SET, ns);
        if (db_set_kv(d, k, kl, blob, 9, now) != 0) {
            obj_set_free(ns);
            storage_length_error(out);
            return -1;
        }
        *s = ns;
        return 1;
    }
    if (obj_tag_of(v, vl) != DDUP_OBJ_SET) {
        wrongtype(out);
        return -1;
    }
    *s = (obj_set *)obj_unpack_ptr(v, vl);
    return 1;
}

/* Fetch the zset object for key; create when missing && create != 0.
 * Returns 1 (*z set), 0 missing, -1 WRONGTYPE (reply written). */
static int get_zset(db *d, resp_buf *out, const char *k, size_t kl, int create,
                    uint64_t now, obj_zset **z)
{
    const char *v;
    size_t vl;
    if (!db_get(d, k, kl, &v, &vl, now)) {
        char blob[9];
        obj_zset *nz;
        if (!create)
            return 0;
        nz = obj_zset_new();
        obj_pack_ptr(blob, DDUP_OBJ_ZSET, nz);
        if (db_set_kv(d, k, kl, blob, 9, now) != 0) {
            obj_zset_free(nz);
            storage_length_error(out);
            return -1;
        }
        *z = nz;
        return 1;
    }
    if (obj_tag_of(v, vl) != DDUP_OBJ_ZSET) {
        wrongtype(out);
        return -1;
    }
    *z = (obj_zset *)obj_unpack_ptr(v, vl);
    return 1;
}

/* Fetch the stream object for key; create when missing && create != 0.
 * Returns 1 (*st set), 0 missing, -1 WRONGTYPE (reply written). */
static int get_stream(db *d, resp_buf *out, const char *k, size_t kl,
                      int create, uint64_t now, obj_stream **st)
{
    const char *v;
    size_t vl;
    if (!db_get(d, k, kl, &v, &vl, now)) {
        char blob[9];
        obj_stream *ns;
        if (!create)
            return 0;
        ns = obj_stream_new();
        obj_pack_ptr(blob, DDUP_OBJ_STREAM, ns);
        if (db_set_kv(d, k, kl, blob, 9, now) != 0) {
            obj_stream_free(ns);
            storage_length_error(out);
            return -1;
        }
        *st = ns;
        return 1;
    }
    if (obj_tag_of(v, vl) != DDUP_OBJ_STREAM) {
        wrongtype(out);
        return -1;
    }
    *st = (obj_stream *)obj_unpack_ptr(v, vl);
    return 1;
}

/* Strict double parse (strtod, full consumption, NaN rejected). */
static int parse_double(const char *s, size_t len, double *out)
{
    char buf[128];
    char *end;
    double v;
    if (len == 0 || len >= sizeof(buf) || s[0] == ' ')
        return 0;
    memcpy(buf, s, len);
    buf[len] = '\0';
    v = strtod(buf, &end);
    if (end != buf + len || v != v)
        return 0;
    *out = v;
    return 1;
}

/* Redis-compatible score formatting (%.17g; inf/-inf). */
static int fmt_score(char *buf, size_t cap, double v)
{
    return snprintf(buf, cap, "%.17g", v);
}

/* Strict long double parse (strtold, full consumption; NaN/Inf and a
 * leading '+' rejected, matching the observable side of Redis string2ld). */
static int parse_ld(const char *s, size_t len, long double *out)
{
    char buf[5120];
    char *end;
    long double v;
    if (len == 0 || len >= sizeof(buf) || s[0] == ' ' || s[0] == '+')
        return 0;
    memcpy(buf, s, len);
    buf[len] = '\0';
    v = strtold(buf, &end);
    if (end != buf + len || v != v || isinf(v))
        return 0;
    *out = v;
    return 1;
}

/* Range bound: optional '(' exclusive prefix, inf/+inf/-inf, or a double. */
static int parse_bound(const char *s, size_t len, double *v, int *ex)
{
    *ex = 0;
    if (len > 0 && s[0] == '(') {
        *ex = 1;
        s++;
        len--;
    }
    if (len == 3 && (s[0] == 'i' || s[0] == 'I') &&
        (s[1] == 'n' || s[1] == 'N') && (s[2] == 'f' || s[2] == 'F')) {
        *v = HUGE_VAL;
        return 1;
    }
    if (len == 4 && s[0] == '+' && (s[1] == 'i' || s[1] == 'I')) {
        *v = HUGE_VAL;
        return 1;
    }
    if (len == 4 && s[0] == '-' && (s[1] == 'i' || s[1] == 'I')) {
        *v = -HUGE_VAL;
        return 1;
    }
    return parse_double(s, len, v);
}

/* Lex range bound (ZRANGEBYLEX family): "-" / "+" for the infinities,
 * "[x" closed / "(x" open finite member. Returns 0 on anything else. */
static int parse_lex_bound(const char *s, size_t len, zlexbound *b)
{
    b->s = NULL;
    b->len = 0;
    b->ex = 0;
    b->inf = 0;
    if (len == 1 && s[0] == '-') {
        b->inf = -1;
        return 1;
    }
    if (len == 1 && s[0] == '+') {
        b->inf = 1;
        return 1;
    }
    if (len >= 1 && s[0] == '[') {
        b->s = s + 1;
        b->len = len - 1;
        return 1;
    }
    if (len >= 1 && s[0] == '(') {
        b->s = s + 1;
        b->len = len - 1;
        b->ex = 1;
        return 1;
    }
    return 0;
}

/* Write one member (and optionally its score) from a zset iterator. */
static void zset_emit_member(resp_buf *out, obj_zset_iter *it, int withscores)
{
    size_t ml = 0;
    const char *mv = obj_zset_iter_member(it, &ml);
    resp_write_bulk(out, mv, ml);
    if (withscores) {
        char num[40];
        int nl = fmt_score(num, sizeof(num), obj_zset_iter_score(it));
        resp_write_bulk(out, num, (size_t)nl);
    }
}

/* Fold an object's mem delta (mutations done in place) into used_memory,
 * and bump the key's WATCH version. */
static void mem_sync(db *d, const char *key, size_t klen, uint64_t before,
                     uint64_t after)
{
    d->used_memory =
        (uint64_t)((int64_t)d->used_memory + ((int64_t)after - (int64_t)before));
    db_touch_key(d, key, klen);
}

typedef struct hdump_ctx {
    resp_buf *out;
    int keys;
    int vals;
} hdump_ctx;

/* RESP2 null array (*-1), e.g. LPOP/RPOP with count on a missing key. */
static void write_null_array(resp_buf *out)
{
    static const char null_arr[] = "*-1\r\n";
    if (resp_buf_reserve(out, sizeof(null_arr) - 1) != 0)
        return;
    memcpy(out->data + out->len, null_arr, sizeof(null_arr) - 1);
    out->len += sizeof(null_arr) - 1;
}

static void hdump_cb(const char *f, size_t flen, const char *v, size_t vlen,
                     void *c)
{
    hdump_ctx *ctx = (hdump_ctx *)c;
    if (ctx->keys)
        resp_write_bulk(ctx->out, f, flen);
    if (ctx->vals)
        resp_write_bulk(ctx->out, v, vlen);
}

/* Collect views of all members of an rh_table-backed set (valid until the
 * next mutation of that table). */
typedef struct collect_ctx {
    const char **keys;
    size_t *lens;
    size_t n;
    size_t cap;
} collect_ctx;

static void collect_cb(const char *k, size_t klen, const char *v, size_t vlen,
                       void *c)
{
    collect_ctx *ctx = (collect_ctx *)c;
    (void)v;
    (void)vlen;
    if (ctx->n == ctx->cap) {
        size_t ncap = ctx->cap == 0 ? 16 : ctx->cap * 2;
        const char **nk =
            (const char **)realloc(ctx->keys, ncap * sizeof(*nk));
        size_t *nl = (size_t *)realloc(ctx->lens, ncap * sizeof(*nl));
        if (nk == NULL || nl == NULL) {
            free(nk);
            free(nl);
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        ctx->keys = nk;
        ctx->lens = nl;
        ctx->cap = ncap;
    }
    ctx->keys[ctx->n] = k;
    ctx->lens[ctx->n] = klen;
    ctx->n++;
}

/* Fisher-Yates shuffle of the first k entries of a collected view array. */
static void collect_shuffle(db *d, collect_ctx *ctx, size_t k)
{
    size_t i;
    if (k > ctx->n)
        k = ctx->n;
    for (i = 0; i + 1 < k; i++) {
        size_t j = i + (size_t)(db_rand(d) % (uint32_t)(ctx->n - i));
        const char *tk = ctx->keys[i];
        size_t tl = ctx->lens[i];
        ctx->keys[i] = ctx->keys[j];
        ctx->lens[i] = ctx->lens[j];
        ctx->keys[j] = tk;
        ctx->lens[j] = tl;
    }
}

/* SINTER/SDIFF member filter over sets[1..n). */
typedef struct setop_ctx {
    obj_set **sets;
    size_t n;
    rh_table *result;
    int inter;
} setop_ctx;

static void setop_cb(const char *m, size_t mlen, const char *v, size_t vlen,
                     void *c)
{
    setop_ctx *ctx = (setop_ctx *)c;
    size_t j;
    int include = 1;
    (void)v;
    (void)vlen;
    for (j = 1; j < ctx->n && include; j++) {
        if (ctx->inter)
            include = ctx->sets[j] != NULL &&
                      obj_set_has(ctx->sets[j], m, mlen);
        else
            include = ctx->sets[j] == NULL ||
                      !obj_set_has(ctx->sets[j], m, mlen);
    }
    if (include)
        rh_set(ctx->result, m, mlen, "", 0);
}

/* SUNION member collector (dedupe via the result table). */
static void set_union_cb(const char *m, size_t mlen, const char *v,
                         size_t vlen, void *c)
{
    (void)v;
    (void)vlen;
    rh_set((rh_table *)c, m, mlen, "", 0);
}

/* Resolve nkeys set operands (NULL view = missing key) and evaluate the
 * SINTER/SUNION/SDIFF into result (caller rh_init/rh_destroy's it).
 * Returns 0 on success, -1 when a reply was already written. */
static int setop_eval(db *d, resp_buf *out, const resp_value *kargv,
                      size_t nkeys, int inter, int sunion, uint64_t now_ms,
                      rh_table *result)
{
    obj_set **sets = (obj_set **)malloc(nkeys * sizeof(*sets));
    size_t i;
    /* resolve operands with type checks first */
    for (i = 0; i < nkeys; i++) {
        const char *k;
        size_t kl;
        obj_set *s = NULL;
        int rc;
        if (!arg_str(&kargv[i], &k, &kl)) {
            free(sets);
            resp_write_error(out, "ERR invalid argument type", 24);
            return -1;
        }
        rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0) {
            free(sets);
            return -1;
        }
        sets[i] = rc == 1 ? s : NULL;
    }
    if (sunion) {
        for (i = 0; i < nkeys; i++)
            if (sets[i] != NULL)
                obj_set_each(sets[i], set_union_cb, result);
    } else if (sets[0] != NULL) {
        setop_ctx ctx;
        ctx.sets = sets;
        ctx.n = nkeys;
        ctx.result = result;
        ctx.inter = inter;
        obj_set_each(sets[0], setop_cb, &ctx);
    }
    free(sets);
    return 0;
}

/* SINTERCARD counter: membership in sets[1..n), early stop at limit. */
typedef struct sintercard_ctx {
    obj_set **sets;
    size_t n;
    long long count;
    long long limit; /* 0 = unlimited */
} sintercard_ctx;

static int sintercard_cb(const char *m, size_t mlen, const char *v,
                         size_t vlen, void *c)
{
    sintercard_ctx *ctx = (sintercard_ctx *)c;
    size_t j;
    (void)v;
    (void)vlen;
    for (j = 1; j < ctx->n; j++)
        if (ctx->sets[j] == NULL || !obj_set_has(ctx->sets[j], m, mlen))
            return 0;
    ctx->count++;
    return ctx->limit > 0 && ctx->count >= ctx->limit;
}

/* obj_set_each adapter for listpack sets: void callback, so the early
 * stop requested by sintercard_cb is remembered in the wrapper. */
typedef struct sintercard_lp_ctx {
    sintercard_ctx ic;
    int stop;
} sintercard_lp_ctx;

static void sintercard_lp_cb(const char *m, size_t mlen, const char *v,
                             size_t vlen, void *c)
{
    sintercard_lp_ctx *w = (sintercard_lp_ctx *)c;
    if (w->stop)
        return;
    w->stop = sintercard_cb(m, mlen, v, vlen, &w->ic);
}

typedef struct setcard_ctx {
    rh_table *seen;
    long long limit;
    long long count;
    int stop;
} setcard_ctx;

static void setcard_union_cb(const char *m, size_t mlen, const char *v,
                             size_t vlen, void *c)
{
    setcard_ctx *sc = (setcard_ctx *)c;
    const char *old;
    size_t oldl;
    (void)v;
    (void)vlen;
    if (sc->stop)
        return;
    if (sc->limit > 0 && sc->count >= sc->limit) {
        sc->stop = 1;
        return;
    }
    if (!rh_get(sc->seen, m, mlen, &old, &oldl)) {
        if (rh_set(sc->seen, m, mlen, "", 0) == 0)
            sc->count++;
    }
}

static void setcard_collect_cb(const char *m, size_t mlen, const char *v,
                               size_t vlen, void *c)
{
    rh_table *seen = (rh_table *)c;
    (void)v;
    (void)vlen;
    (void)rh_set(seen, m, mlen, "", 0);
}

static void setcard_diff_cb(const char *m, size_t mlen, const char *v,
                            size_t vlen, void *c)
{
    setcard_ctx *sc = (setcard_ctx *)c;
    const char *old;
    size_t oldl;
    (void)v;
    (void)vlen;
    if (sc->stop)
        return;
    if (sc->limit > 0 && sc->count >= sc->limit) {
        sc->stop = 1;
        return;
    }
    if (!rh_get(sc->seen, m, mlen, &old, &oldl))
        sc->count++;
}

/* Copy one result member into a fresh obj_set (STORE materialization). */
static void set_store_cb(const char *m, size_t mlen, const char *v,
                         size_t vlen, void *c)
{
    (void)v;
    (void)vlen;
    (void)obj_set_add((obj_set *)c, m, mlen);
}

/* RESP2 push triple: [kind, channel-or-nil, count]. */
static void write_sub_reply(resp_buf *out, const char *kind, size_t klen,
                            const char *ch, size_t chlen, long long count)
{
    resp_write_array_header(out, 3);
    resp_write_bulk(out, kind, klen);
    if (ch != NULL)
        resp_write_bulk(out, ch, chlen);
    else
        resp_write_bulk(out, NULL, 0);
    resp_write_integer(out, count);
}

/* Collect owned copies of channel names (UNSUBSCRIBE without args). */
typedef struct unsub_ctx {
    char **names;
    size_t *lens;
    size_t n;
    size_t cap;
} unsub_ctx;

static void unsub_collect_cb(const char *ch, size_t len, void *arg)
{
    unsub_ctx *u = (unsub_ctx *)arg;
    if (u->n == u->cap) {
        size_t ncap = u->cap == 0 ? 8 : u->cap * 2;
        char **nn = (char **)realloc(u->names, ncap * sizeof(*nn));
        size_t *nl = (size_t *)realloc(u->lens, ncap * sizeof(*nl));
        if (nn == NULL || nl == NULL) {
            free(nn);
            free(nl);
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        u->names = nn;
        u->lens = nl;
        u->cap = ncap;
    }
    u->names[u->n] = (char *)malloc(len);
    memcpy(u->names[u->n], ch, len);
    u->lens[u->n] = len;
    u->n++;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int ci_equal(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen)
        return 0;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z')
            ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z')
            cb = (char)(cb - 'a' + 'A');
        if (ca != cb)
            return 0;
    }
    return 1;
}

/* Extract a string argument; returns 0 if the value is not string-typed. */
/* Strict long-long parse of a string argument (optional '-', digits only). */
static int cmd_parse_ll(const resp_value *v, long long *out)
{
    const char *p;
    const char *end;
    int neg = 0;
    long long x = 0;
    if (v->str == NULL)
        return 0;
    p = v->str;
    end = v->str + v->len;
    if (p == end)
        return 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (p == end)
        return 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9')
            return 0;
        if (x > 1000000000LL)
            return 0;
        x = x * 10 + (*p - '0');
    }
    *out = neg ? -x : x;
    return 1;
}

static int arg_str(const resp_value *v, const char **s, size_t *len)
{
    if (v->type != RESP_BULK_STRING && v->type != RESP_SIMPLE_STRING)
        return 0;
    *s = v->str;
    *len = v->len;
    return 1;
}

static void wrong_args(resp_buf *out, const char *name)
{
    char msg[96];
    int n = snprintf(msg, sizeof(msg),
                     "ERR wrong number of arguments for '%s' command", name);
    resp_write_error(out, msg, (size_t)n);
}

/* Strict signed 64-bit parse (leading '-', digits only, no overflow). */
static int parse_i64(const char *s, size_t len, long long *out)
{
    if (len == 0)
        return 0;
    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (len == 1)
            return 0;
    }
    unsigned long long v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        unsigned digit = (unsigned)(s[i] - '0');
        if (v > (unsigned long long)LLONG_MAX / 10 ||
            (v == (unsigned long long)LLONG_MAX / 10 &&
             digit > (unsigned)(LLONG_MAX % 10) + (neg ? 1u : 0u)))
            return 0;
        v = v * 10 + digit;
    }
    if (neg)
        *out = (v == (unsigned long long)LLONG_MAX + 1ULL)
                   ? LLONG_MIN
                   : -(long long)v;
    else
        *out = (long long)v;
    return 1;
}

/* Strict unsigned 64-bit parse (digits only, no overflow). */
static int parse_u64(const char *s, size_t len, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    if (len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned digit;
        if (s[i] < '0' || s[i] > '9')
            return 0;
        digit = (unsigned)(s[i] - '0');
        if (v > (UINT64_MAX - digit) / 10)
            return 0;
        v = v * 10 + digit;
    }
    *out = v;
    return 1;
}

static const char ERR_NOT_INT[] = "ERR value is not an integer or out of range";
static const char ERR_OVERFLOW[] = "ERR increment or decrement would overflow";
static const char ERR_SYNTAX[] = "ERR syntax error";
static const char ERR_NOT_FLOAT[] = "ERR value is not a valid float";

/* ------------------------------------------------------------------ */
/* sorted-set aggregate command helpers (ZUNION/ZINTER/ZDIFF family)   */
/* ------------------------------------------------------------------ */

enum { ZSET_AGG_SUM = 0, ZSET_AGG_MIN = 1, ZSET_AGG_MAX = 2 };

typedef struct zsetop_args {
    obj_zset **sets;
    size_t nkeys;
    double *weights;
    int aggregate;
    int withscores;
} zsetop_args;

static double zset_aggregate(double a, double b, int aggregate)
{
    if (aggregate == ZSET_AGG_MIN)
        return a < b ? a : b;
    if (aggregate == ZSET_AGG_MAX)
        return a > b ? a : b;
    return a + b;
}

static int zset_resolve_operands(db *d, resp_buf *out, const resp_value *kargv,
                                 size_t nkeys, uint64_t now_ms,
                                 obj_zset ***sets_out)
{
    obj_zset **sets;
    size_t i;

    if (nkeys == 0) {
        *sets_out = NULL;
        return 0;
    }
    sets = (obj_zset **)malloc(nkeys * sizeof(*sets));
    if (sets == NULL) {
        oom_blocked(d, out);
        return -1;
    }
    for (i = 0; i < nkeys; i++) {
        const char *k;
        size_t kl;
        obj_zset *z = NULL;
        int rc;
        if (!arg_str(&kargv[i], &k, &kl)) {
            free(sets);
            resp_write_error(out, "ERR invalid argument type", 24);
            return -1;
        }
        rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0) {
            free(sets);
            return -1;
        }
        sets[i] = rc == 1 ? z : NULL;
    }
    *sets_out = sets;
    return 0;
}

static void zsetop_args_free(zsetop_args *args)
{
    free(args->sets);
    free(args->weights);
}

/* Parse "numkeys key [key ...] [options]" for the Z*STORE/Z* commands.
 * `numkeys_idx` is 2 for STORE (argv[1] is the destination), 1 otherwise.
 * On failure a reply has already been written and the caller returns. */
static int zsetop_parse(db *d, resp_buf *out, const resp_value *argv,
                        size_t argc, size_t numkeys_idx, int has_weights,
                        int has_agg, int has_withscores, int has_limit,
                        long long *limit, uint64_t now_ms, zsetop_args *args)
{
    const char *nv;
    size_t nvl;
    long long nk;
    size_t nkeys, first_key, opts, i;
    int seen_weights = 0, seen_agg = 0, seen_withscores = 0, seen_limit = 0;

    memset(args, 0, sizeof(*args));
    if (limit != NULL)
        *limit = 0;
    if (numkeys_idx >= argc || !arg_str(&argv[numkeys_idx], &nv, &nvl)) {
        resp_write_error(out, "ERR invalid argument type", 24);
        return -1;
    }
    if (!parse_i64(nv, nvl, &nk)) {
        resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
        return -1;
    }
    if (nk <= 0 ||
        (unsigned long long)nk > (unsigned long long)(argc - numkeys_idx - 1)) {
        static const char E[] =
            "ERR Number of keys can't be greater than number of args";
        resp_write_error(out, E, strlen(E));
        return -1;
    }
    nkeys = (size_t)nk;
    first_key = numkeys_idx + 1;
    opts = first_key + nkeys;

    args->weights = (double *)malloc(nkeys * sizeof(*args->weights));
    if (args->weights == NULL) {
        oom_blocked(d, out);
        return -1;
    }
    for (i = 0; i < nkeys; i++)
        args->weights[i] = 1.0;
    args->nkeys = nkeys;
    args->aggregate = ZSET_AGG_SUM;

    for (i = opts; i < argc; i++) {
        const char *tok;
        size_t tokl;
        if (!arg_str(&argv[i], &tok, &tokl)) {
            zsetop_args_free(args);
            resp_write_error(out, "ERR invalid argument type", 24);
            return -1;
        }
        if (has_withscores && ci_equal(tok, tokl, "WITHSCORES")) {
            if (seen_withscores) {
                zsetop_args_free(args);
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return -1;
            }
            seen_withscores = 1;
            args->withscores = 1;
        } else if (has_weights && ci_equal(tok, tokl, "WEIGHTS")) {
            size_t j;
            if (seen_weights || i + nkeys >= argc) {
                zsetop_args_free(args);
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return -1;
            }
            seen_weights = 1;
            for (j = 0; j < nkeys; j++) {
                const char *w;
                size_t wl;
                if (!arg_str(&argv[i + 1 + j], &w, &wl) ||
                    !parse_double(w, wl, &args->weights[j])) {
                    zsetop_args_free(args);
                    resp_write_error(out, ERR_NOT_FLOAT,
                                     sizeof(ERR_NOT_FLOAT) - 1);
                    return -1;
                }
            }
            i += nkeys;
        } else if (has_agg && ci_equal(tok, tokl, "AGGREGATE")) {
            const char *ag;
            size_t agl;
            if (seen_agg || i + 1 >= argc ||
                !arg_str(&argv[i + 1], &ag, &agl)) {
                zsetop_args_free(args);
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return -1;
            }
            seen_agg = 1;
            if (ci_equal(ag, agl, "SUM"))
                args->aggregate = ZSET_AGG_SUM;
            else if (ci_equal(ag, agl, "MIN"))
                args->aggregate = ZSET_AGG_MIN;
            else if (ci_equal(ag, agl, "MAX"))
                args->aggregate = ZSET_AGG_MAX;
            else {
                zsetop_args_free(args);
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return -1;
            }
            i++;
        } else if (has_limit && ci_equal(tok, tokl, "LIMIT")) {
            long long lv;
            const char *lvstr;
            size_t lvlen;
            if (seen_limit || i + 1 >= argc ||
                !arg_str(&argv[i + 1], &lvstr, &lvlen) ||
                !parse_i64(lvstr, lvlen, &lv)) {
                zsetop_args_free(args);
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return -1;
            }
            if (lv < 0) {
                static const char E[] = "ERR LIMIT can't be negative";
                zsetop_args_free(args);
                resp_write_error(out, E, sizeof(E) - 1);
                return -1;
            }
            seen_limit = 1;
            if (limit != NULL)
                *limit = lv;
            i++;
        } else {
            zsetop_args_free(args);
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return -1;
        }
    }

    if (zset_resolve_operands(d, out, argv + first_key, nkeys, now_ms,
                              &args->sets) != 0) {
        zsetop_args_free(args);
        return -1;
    }
    return 0;
}

static void zset_emit_all(resp_buf *out, obj_zset *z, int withscores)
{
    obj_zset_iter it;
    size_t n = obj_zset_len(z);
    size_t i;
    resp_write_array_header(out, n * (withscores ? 2u : 1u));
    if (obj_zset_first(z, &it)) {
        for (i = 0; i < n; i++) {
            zset_emit_member(out, &it, withscores);
            if (i + 1 < n && !obj_zset_iter_next(&it))
                break;
        }
    }
}

static int zset_store_result(db *d, resp_buf *out, const char *dst,
                             size_t dstl, obj_zset *result, uint64_t now_ms)
{
    if (obj_zset_len(result) == 0) {
        obj_zset_free(result);
        db_del_kv(d, dst, dstl);
        resp_write_integer(out, 0);
        return 0;
    }
    {
        char blob[9];
        obj_pack_ptr(blob, DDUP_OBJ_ZSET, result);
        if (db_set_kv(d, dst, dstl, blob, 9, now_ms) != 0) {
            obj_zset_free(result);
            storage_length_error(out);
            return -1;
        }
    }
    resp_write_integer(out, (long long)obj_zset_len(result));
    return 0;
}

static int zset_union_build(obj_zset **sets, size_t nkeys, double *weights,
                            int aggregate, obj_zset *result)
{
    size_t i;
    for (i = 0; i < nkeys; i++) {
        obj_zset_iter it;
        if (sets[i] == NULL || !obj_zset_first(sets[i], &it))
            continue;
        for (;;) {
            size_t ml = 0;
            const char *mv = obj_zset_iter_member(&it, &ml);
            double sc = obj_zset_iter_score(&it) * weights[i];
            double cur;
            if (obj_zset_score(result, mv, ml, &cur))
                sc = zset_aggregate(cur, sc, aggregate);
            if (sc != sc || obj_zset_add(result, mv, ml, sc) < 0)
                return -1;
            if (!obj_zset_iter_next(&it))
                break;
        }
    }
    return 0;
}

static int zset_inter_build(obj_zset **sets, size_t nkeys, double *weights,
                            int aggregate, obj_zset *result)
{
    size_t min_idx = 0;
    uint64_t min_len = UINT64_MAX;
    size_t i;

    for (i = 0; i < nkeys; i++) {
        if (sets[i] == NULL)
            return 0; /* a missing operand empties the intersection */
        if (obj_zset_len(sets[i]) < min_len) {
            min_len = obj_zset_len(sets[i]);
            min_idx = i;
        }
    }
    {
        obj_zset_iter it;
        if (!obj_zset_first(sets[min_idx], &it))
            return 0;
        for (;;) {
            size_t ml = 0;
            const char *mv = obj_zset_iter_member(&it, &ml);
            double acc = obj_zset_iter_score(&it) * weights[min_idx];
            int present = 1;
            for (i = 0; i < nkeys; i++) {
                double sc;
                if (i == min_idx)
                    continue;
                if (!obj_zset_score(sets[i], mv, ml, &sc)) {
                    present = 0;
                    break;
                }
                acc = zset_aggregate(acc, sc * weights[i], aggregate);
            }
            if (present && (acc != acc || obj_zset_add(result, mv, ml, acc) < 0))
                return -1;
            if (!obj_zset_iter_next(&it))
                break;
        }
    }
    return 0;
}

static int zset_diff_build(obj_zset **sets, size_t nkeys, obj_zset *result)
{
    obj_zset_iter it;
    size_t i;
    if (sets[0] == NULL || !obj_zset_first(sets[0], &it))
        return 0;
    for (;;) {
        size_t ml = 0;
        const char *mv = obj_zset_iter_member(&it, &ml);
        int present = 0;
        for (i = 1; i < nkeys; i++) {
            double sc;
            if (sets[i] != NULL && obj_zset_score(sets[i], mv, ml, &sc)) {
                present = 1;
                break;
            }
        }
        if (!present &&
            obj_zset_add(result, mv, ml, obj_zset_iter_score(&it)) < 0)
            return -1;
        if (!obj_zset_iter_next(&it))
            break;
    }
    return 0;
}

static long long zset_intercard(obj_zset **sets, size_t nkeys, long long limit)
{
    size_t min_idx = 0;
    uint64_t min_len = UINT64_MAX;
    size_t i;
    long long count = 0;

    if (limit == 0)
        return 0;
    for (i = 0; i < nkeys; i++) {
        if (sets[i] == NULL)
            return 0;
        if (obj_zset_len(sets[i]) < min_len) {
            min_len = obj_zset_len(sets[i]);
            min_idx = i;
        }
    }
    {
        obj_zset_iter it;
        if (!obj_zset_first(sets[min_idx], &it))
            return 0;
        for (;;) {
            size_t ml = 0;
            const char *mv = obj_zset_iter_member(&it, &ml);
            int present = 1;
            for (i = 0; i < nkeys; i++) {
                double sc;
                if (i == min_idx)
                    continue;
                if (!obj_zset_score(sets[i], mv, ml, &sc)) {
                    present = 0;
                    break;
                }
            }
            if (present) {
                count++;
                if (limit > 0 && count >= limit)
                    break;
            }
            if (!obj_zset_iter_next(&it))
                break;
        }
    }
    return count;
}

/* Resulting-string ceiling for SETRANGE/BITFIELD (proto-max-bulk-len). */
#define STRING_MAX_BYTES (512ULL * 1024ULL * 1024ULL)

/* ------------------------------------------------------------------ */
/* bitmap batch helpers (BITOP / BITFIELD)                            */
/* ------------------------------------------------------------------ */

enum {
    BITOP_AND = 0,
    BITOP_OR,
    BITOP_XOR,
    BITOP_NOT
};

static int bitmap_op_parse(const char *s, size_t len, int *op)
{
    if (ci_equal(s, len, "AND")) {
        *op = BITOP_AND;
        return 1;
    }
    if (ci_equal(s, len, "OR")) {
        *op = BITOP_OR;
        return 1;
    }
    if (ci_equal(s, len, "XOR")) {
        *op = BITOP_XOR;
        return 1;
    }
    if (ci_equal(s, len, "NOT")) {
        *op = BITOP_NOT;
        return 1;
    }
    return 0;
}

static void cmd_bitop(db *d, const resp_value *argv, size_t argc,
                      resp_buf *out, uint64_t now_ms)
{
    const char *ops, *dk;
    size_t opsl, dkl;
    const char **srcs;
    size_t *srclens;
    size_t nsrc, i, maxlen = 0, full_words, tail, wi;
    char *result = NULL;
    int op;

    if (argc < 4) {
        wrong_args(out, "bitop");
        return;
    }
    if (!arg_str(&argv[1], &ops, &opsl) || !arg_str(&argv[2], &dk, &dkl))
        goto bitop_badtype;
    if (!bitmap_op_parse(ops, opsl, &op)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    if (op == BITOP_NOT && argc != 4) {
        static const char E[] =
            "ERR BITOP NOT must be called with a single source key.";
        resp_write_error(out, E, strlen(E));
        return;
    }
    if (!storage_key_ok(dkl)) {
        storage_length_error(out);
        return;
    }

    nsrc = argc - 3;
    srcs = (const char **)malloc(nsrc * sizeof(*srcs));
    srclens = (size_t *)malloc(nsrc * sizeof(*srclens));
    if (srcs == NULL || srclens == NULL) {
        free(srcs);
        free(srclens);
        storage_length_error(out);
        return;
    }

    /* First pass: resolve every source once and find the longest input.
     * Missing keys are treated as empty strings, matching Redis BITOP. */
    for (i = 0; i < nsrc; i++) {
        const char *k;
        size_t kl;
        const char *v;
        size_t vl;
        if (!arg_str(&argv[3 + i], &k, &kl)) {
            free(srcs);
            free(srclens);
            goto bitop_badtype;
        }
        srcs[i] = NULL;
        srclens[i] = 0;
        if (db_get(d, k, kl, &v, &vl, now_ms)) {
            const char *s;
            size_t sl;
            if (!as_string(out, v, vl, &s, &sl)) {
                free(srcs);
                free(srclens);
                return;
            }
            srcs[i] = s;
            srclens[i] = sl;
        }
        if (srclens[i] > maxlen)
            maxlen = srclens[i];
    }
    if (maxlen == 0) {
        free(srcs);
        free(srclens);
        db_del_kv(d, dk, dkl);
        resp_write_integer(out, 0);
        return;
    }
    if (!storage_string_ok(dkl, maxlen)) {
        free(srcs);
        free(srclens);
        storage_length_error(out);
        return;
    }
    if (oom_blocked(d, out)) {
        free(srcs);
        free(srclens);
        return;
    }

    result = (char *)malloc(maxlen);
    if (result == NULL) {
        free(srcs);
        free(srclens);
        storage_length_error(out);
        return;
    }

    /* Word-at-a-time core: 8-byte chunks avoid per-byte branches on the
     * hot path; the tail is handled bytewise for unaligned ends. */
    full_words = maxlen / 8;
    tail = maxlen % 8;
    for (wi = 0; wi < full_words; wi++) {
        uint64_t acc = op == BITOP_AND ? ~0ULL : 0ULL;
        for (i = 0; i < nsrc; i++) {
            uint64_t w = 0;
            if (srclens[i] >= (wi + 1) * 8)
                memcpy(&w, srcs[i] + wi * 8, 8);
            if (op == BITOP_AND)
                acc &= w;
            else if (op == BITOP_OR)
                acc |= w;
            else if (op == BITOP_XOR)
                acc ^= w;
            else
                acc = ~w;
        }
        memcpy(result + wi * 8, &acc, 8);
    }
    for (i = 0; i < tail; i++) {
        unsigned char byte = op == BITOP_AND ? 0xFFU : 0U;
        size_t bytepos = full_words * 8 + i;
        size_t j;
        for (j = 0; j < nsrc; j++) {
            unsigned char w =
                bytepos < srclens[j] ? (unsigned char)srcs[j][bytepos] : 0U;
            if (op == BITOP_AND)
                byte &= w;
            else if (op == BITOP_OR)
                byte |= w;
            else if (op == BITOP_XOR)
                byte ^= w;
            else
                byte = (unsigned char)~w;
        }
        result[bytepos] = (char)byte;
    }

    if (db_set_string(d, dk, dkl, result, maxlen, now_ms) != 0) {
        free(result);
        free(srcs);
        free(srclens);
        storage_length_error(out);
        return;
    }
    free(result);
    free(srcs);
    free(srclens);
    resp_write_integer(out, (long long)maxlen);
    return;

bitop_badtype:
    resp_write_error(out, "ERR invalid argument type", 24);
}

#define BF_GET 0
#define BF_SET 1
#define BF_INCRBY 2

#define BF_OVERFLOW_WRAP 0
#define BF_OVERFLOW_SAT 1
#define BF_OVERFLOW_FAIL 2

typedef struct bf_op {
    int kind;
    int is_signed;
    unsigned width;
    long long raw_off;
    long long value;
    int mode;
} bf_op;

static int bf_parse_type(const char *s, size_t len, int *signedp,
                         unsigned *width)
{
    unsigned w = 0;
    size_t i;
    int is_signed;
    if (len < 2)
        return 0;
    if (s[0] == 'u') {
        is_signed = 0;
    } else if (s[0] == 'i') {
        is_signed = 1;
    } else {
        return 0;
    }
    for (i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        if (w > 64)
            return 0;
        w = w * 10 + (unsigned)(s[i] - '0');
    }
    if (w == 0 || w > 64)
        return 0;
    if (!is_signed && w == 64)
        return 0; /* Redis: u64 is not a supported BITFIELD type */
    *signedp = is_signed;
    *width = w;
    return 1;
}

static uint64_t bf_read_raw(const unsigned char *p, size_t len, size_t bitoff,
                            unsigned width)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < width; i++) {
        size_t pos = bitoff + i;
        size_t byte = pos / 8;
        unsigned bit = (unsigned)(7 - (pos & 7));
        v <<= 1;
        if (byte < len && (p[byte] & (1U << bit)))
            v |= 1ULL;
    }
    return v;
}

static void bf_write_raw(unsigned char *p, size_t len, size_t bitoff,
                         unsigned width, uint64_t v)
{
    unsigned i;
    (void)len;
    if (width < 64)
        v &= (1ULL << width) - 1;
    for (i = 0; i < width; i++) {
        size_t pos = bitoff + i;
        size_t byte = pos / 8;
        unsigned bit = (unsigned)(7 - (pos & 7));
        unsigned set = (unsigned)((v >> (width - 1 - i)) & 1ULL);
        if (set)
            p[byte] |= (unsigned char)(1U << bit);
        else
            p[byte] &= (unsigned char)~(1U << bit);
    }
}

static long long bf_raw_to_signed(uint64_t v, unsigned width)
{
    if (width == 64) {
        long long out;
        memcpy(&out, &v, 8);
        return out;
    }
    if (v & (1ULL << (width - 1)))
        v |= ~((1ULL << width) - 1);
    return (long long)v;
}

static uint64_t bf_signed_to_raw(long long v, unsigned width)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    if (width < 64)
        u &= (1ULL << width) - 1;
    return u;
}

static int bf_incr_value(long long old, unsigned width, int is_signed,
                         long long inc, int mode, long long *out)
{
    if (is_signed) {
        long long maxv = width == 64
                             ? LLONG_MAX
                             : (long long)((1ULL << (width - 1)) - 1);
        long long minv = width == 64
                             ? LLONG_MIN
                             : (long long)(0ULL - (1ULL << (width - 1)));
        if (mode == BF_OVERFLOW_WRAP) {
            uint64_t sum = bf_signed_to_raw(old, width) +
                           bf_signed_to_raw(inc, width);
            *out = bf_raw_to_signed(sum, width);
            return 0;
        }
        if (mode == BF_OVERFLOW_SAT) {
            if (inc > 0 && old > maxv - inc)
                *out = maxv;
            else if (inc < 0 && old < minv - inc)
                *out = minv;
            else
                *out = old + inc;
            return 0;
        }
        if ((inc > 0 && old > maxv - inc) ||
            (inc < 0 && old < minv - inc))
            return 1;
        *out = old + inc;
        return 0;
    }
    {
        long long maxv = (long long)((1ULL << width) - 1);
        if (mode == BF_OVERFLOW_WRAP) {
            uint64_t sum = (uint64_t)old + (uint64_t)inc;
            if (width < 64)
                sum &= (1ULL << width) - 1;
            *out = (long long)sum;
            return 0;
        }
        if (mode == BF_OVERFLOW_SAT) {
            if (inc > 0 && old > maxv - inc)
                *out = maxv;
            else if (inc < 0) {
                uint64_t mag = (uint64_t)(-(inc + 1)) + 1ULL;
                *out = (uint64_t)old < mag ? 0 : old + inc;
            } else {
                *out = old;
            }
            return 0;
        }
        if (inc > 0 && old > maxv - inc)
            return 1;
        if (inc < 0) {
            uint64_t mag = (uint64_t)(-(inc + 1)) + 1ULL;
            if ((uint64_t)old < mag)
                return 1;
        }
        *out = old + inc;
        return 0;
    }
}

static int bf_resolve_offset(long long raw, size_t cur_bytes, unsigned width,
                             size_t *out)
{
    uint64_t base = (uint64_t)cur_bytes * 8;
    uint64_t abs;
    if (raw < 0) {
        uint64_t mag = (uint64_t)(-(raw + 1)) + 1ULL;
        if (mag > base)
            return 0;
        abs = base - mag;
    } else {
        abs = (uint64_t)raw;
    }
    if (abs > (uint64_t)SIZE_MAX || abs + width > STRING_MAX_BYTES * 8)
        return 0;
    *out = (size_t)abs;
    return 1;
}

static void cmd_bitfield(db *d, const resp_value *argv, size_t argc,
                         resp_buf *out, uint64_t now_ms, int ro)
{
    const char *k;
    size_t kl;
    bf_op *ops = NULL;
    size_t nops = 0, cap = 0;
    size_t i;
    int mode = BF_OVERFLOW_WRAP;
    resp_buf tmp, reply;

    if (argc < 3) {
        wrong_args(out, ro ? "bitfield_ro" : "bitfield");
        return;
    }
    if (!arg_str(&argv[1], &k, &kl)) {
        resp_write_error(out, "ERR invalid argument type", 24);
        return;
    }
    if (!storage_key_ok(kl)) {
        storage_length_error(out);
        return;
    }

    i = 2;
    while (i < argc) {
        const char *tok;
        size_t tokl;
        bf_op op;
        if (!arg_str(&argv[i], &tok, &tokl)) {
            resp_write_error(out, "ERR invalid argument type", 24);
            goto bitfield_fail;
        }
        if (ci_equal(tok, tokl, "OVERFLOW")) {
            const char *m;
            size_t ml;
            if (ro) {
                static const char E[] =
                    "ERR BITFIELD_RO only supports the GET subcommand";
                resp_write_error(out, E, sizeof(E) - 1);
                goto bitfield_fail;
            }
            if (i + 1 >= argc || !arg_str(&argv[i + 1], &m, &ml)) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                goto bitfield_fail;
            }
            if (ci_equal(m, ml, "WRAP")) {
                mode = BF_OVERFLOW_WRAP;
            } else if (ci_equal(m, ml, "SAT")) {
                mode = BF_OVERFLOW_SAT;
            } else if (ci_equal(m, ml, "FAIL")) {
                mode = BF_OVERFLOW_FAIL;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                goto bitfield_fail;
            }
            i += 2;
            continue;
        }

        memset(&op, 0, sizeof(op));
        op.mode = mode;
        if (ci_equal(tok, tokl, "GET")) {
            op.kind = BF_GET;
        } else if (ci_equal(tok, tokl, "SET")) {
            op.kind = BF_SET;
        } else if (ci_equal(tok, tokl, "INCRBY")) {
            op.kind = BF_INCRBY;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            goto bitfield_fail;
        }
        if (ro && op.kind != BF_GET) {
            static const char E[] =
                "ERR BITFIELD_RO only supports the GET subcommand";
            resp_write_error(out, E, sizeof(E) - 1);
            goto bitfield_fail;
        }
        if (i + 2 >= argc ||
            !arg_str(&argv[i + 1], &tok, &tokl) ||
            !bf_parse_type(tok, tokl, &op.is_signed, &op.width)) {
            static const char E[] =
                "ERR Invalid bitfield type. Use something like i16 u8. "
                "Note that u64 is not supported but i64 is.";
            resp_write_error(out, E, sizeof(E) - 1);
            goto bitfield_fail;
        }
        {
            const char *off;
            size_t offl;
            if (!arg_str(&argv[i + 2], &off, &offl) ||
                !parse_i64(off, offl, &op.raw_off)) {
                resp_write_error(out,
                                 "ERR bit offset is not an integer or out of "
                                 "range",
                                 48);
                goto bitfield_fail;
            }
        }
        if (op.kind != BF_GET) {
            const char *val;
            size_t vall;
            if (i + 3 >= argc || !arg_str(&argv[i + 3], &val, &vall) ||
                !parse_i64(val, vall, &op.value)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                goto bitfield_fail;
            }
        }
        if (nops == cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            bf_op *n = (bf_op *)realloc(ops, ncap * sizeof(*n));
            if (n == NULL) {
                storage_length_error(out);
                goto bitfield_fail;
            }
            ops = n;
            cap = ncap;
        }
        ops[nops++] = op;
        i += op.kind == BF_GET ? 3 : 4;
    }

    if (nops == 0) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        goto bitfield_fail;
    }

    resp_buf_init(&tmp);
    {
        const char *v;
        size_t vl;
        if (db_get(d, k, kl, &v, &vl, now_ms)) {
            const char *s;
            size_t sl;
            if (!as_string(out, v, vl, &s, &sl)) {
                resp_buf_free(&tmp);
                free(ops);
                return;
            }
            if (sl > 0 && resp_buf_reserve(&tmp, sl) != 0) {
                resp_buf_free(&tmp);
                free(ops);
                storage_length_error(out);
                return;
            }
            memcpy(tmp.data, s, sl);
            tmp.len = sl;
        }
    }

    resp_buf_init(&reply);
    resp_write_array_header(&reply, nops);
    for (i = 0; i < nops; i++) {
        bf_op *op = &ops[i];
        size_t bitoff;
        size_t need;
        if (!bf_resolve_offset(op->raw_off, tmp.len, op->width, &bitoff)) {
            resp_buf_free(&tmp);
            resp_buf_free(&reply);
            free(ops);
            resp_write_error(out,
                             "ERR bit offset is not an integer or out of range",
                             48);
            return;
        }
        need = (bitoff + op->width + 7) / 8;
        if (need > tmp.len) {
            if (resp_buf_reserve(&tmp, need) != 0) {
                resp_buf_free(&tmp);
                resp_buf_free(&reply);
                free(ops);
                storage_length_error(out);
                return;
            }
            memset(tmp.data + tmp.len, 0, need - tmp.len);
            tmp.len = need;
        }

        {
            uint64_t oldraw =
                bf_read_raw((unsigned char *)tmp.data, tmp.len, bitoff,
                            op->width);
            if (op->kind == BF_GET) {
                long long v = op->is_signed
                                  ? bf_raw_to_signed(oldraw, op->width)
                                  : (long long)oldraw;
                resp_write_integer(&reply, v);
            } else if (op->kind == BF_SET) {
                long long old = op->is_signed
                                    ? bf_raw_to_signed(oldraw, op->width)
                                    : (long long)oldraw;
                bf_write_raw((unsigned char *)tmp.data, tmp.len, bitoff,
                             op->width,
                             op->is_signed
                                 ? bf_signed_to_raw(op->value, op->width)
                                 : (uint64_t)op->value);
                resp_write_integer(&reply, old);
            } else {
                long long old = op->is_signed
                                    ? bf_raw_to_signed(oldraw, op->width)
                                    : (long long)oldraw;
                long long next = 0;
                if (bf_incr_value(old, op->width, op->is_signed, op->value,
                                  op->mode, &next)) {
                    resp_write_bulk(&reply, NULL, 0);
                } else {
                    bf_write_raw((unsigned char *)tmp.data, tmp.len, bitoff,
                                 op->width,
                                 op->is_signed
                                     ? bf_signed_to_raw(next, op->width)
                                     : (uint64_t)next);
                    resp_write_integer(&reply, next);
                }
            }
        }
    }

    if (!ro) {
        if (tmp.len == 0) {
            db_del_kv(d, k, kl);
        } else if (db_set_string(d, k, kl, tmp.data, tmp.len, now_ms) != 0) {
            resp_buf_free(&tmp);
            resp_buf_free(&reply);
            free(ops);
            storage_length_error(out);
            return;
        }
    }

    if (resp_buf_reserve(out, reply.len) != 0) {
        resp_buf_free(&tmp);
        resp_buf_free(&reply);
        free(ops);
        storage_length_error(out);
        return;
    }
    memcpy(out->data + out->len, reply.data, reply.len);
    out->len += reply.len;
    resp_buf_free(&tmp);
    resp_buf_free(&reply);
    free(ops);
    return;

bitfield_fail:
    free(ops);
}

static const char *policy_name(int policy)
{
    return policy == DB_POLICY_NOEVICTION ? "noeviction" : "allkeys-lru";
}

static void human_bytes(uint64_t b, char *buf, size_t cap)
{
    if (b < 1024ULL)
        snprintf(buf, cap, "%lluB", (unsigned long long)b);
    else if (b < 1024ULL * 1024ULL)
        snprintf(buf, cap, "%.2fK", (double)b / 1024.0);
    else if (b < 1024ULL * 1024ULL * 1024ULL)
        snprintf(buf, cap, "%.2fM", (double)b / (1024.0 * 1024.0));
    else
        snprintf(buf, cap, "%.2fG", (double)b / (1024.0 * 1024.0 * 1024.0));
}

/* ------------------------------------------------------------------ */
/* INFO                                                                 */
/* ------------------------------------------------------------------ */

/* Collect the numeric INFO snapshot for a session: per-process scalars and
 * commandstats are summed across every logical db when a selection hook is
 * installed. */
static void info_fill(const session *s, info_stats *st)
{
    db *d = s->d;
    uint16_t id;
    memset(st, 0, sizeof(*st));
    st->tier_disk_bytes =
        d->tier != NULL ? tier_disk_bytes(d->tier) : 0;
    st->tier_live_records =
        d->tier != NULL ? tier_live_records(d->tier) : 0;
    st->tier_failed =
        d->tier != NULL ? (uint64_t)tier_failed(d->tier) : 0;
    st->dbsize = rh_size(&d->table);
    if (s->sel_fn != NULL) {
        int i;
        st->ndbs = s->sel_ndbs < INFO_STATS_MAX_DBS ? s->sel_ndbs
                                                    : INFO_STATS_MAX_DBS;
        for (i = 0; i < st->ndbs; i++) {
            db *di = s->sel_fn(s->sel_ctx, i);
            st->used_memory += di->used_memory;
            st->expired_keys += di->expired_keys;
            st->evicted_keys += di->evicted_keys;
            st->db_keys[i] = rh_size(&di->table);
            st->db_expires[i] = rh_size(&di->expires);
            for (id = 1; id <= CMD_MAX; id++) {
                st->cmd_calls[id] += di->cmd_calls[id];
                st->cmd_usecs[id] += di->cmd_usecs[id];
            }
        }
    } else {
        st->ndbs = 1;
        st->used_memory = d->used_memory;
        st->expired_keys = d->expired_keys;
        st->evicted_keys = d->evicted_keys;
        st->db_keys[0] = rh_size(&d->table);
        st->db_expires[0] = rh_size(&d->expires);
        for (id = 1; id <= CMD_MAX; id++) {
            st->cmd_calls[id] = d->cmd_calls[id];
            st->cmd_usecs[id] = d->cmd_usecs[id];
        }
    }
    /* server IO counters are per-process, not per-db: copy once */
    if (s->io != NULL)
        st->io = *s->io;
}

/* Machine-readable snapshot (INFO __STATS__, internal): the mt aggregation
 * transport. One "k:v" line per scalar, "db:<i>:<keys>:<expires>" per
 * non-empty db and "c:<id>:<calls>:<usec>" per called command, so the home
 * worker can sum parts without name lookups. */
static void info_format_stats(const info_stats *st, resp_buf *out)
{
    char buf[8192];
    int n2;
    int i;
    uint16_t id;
    n2 = snprintf(buf, sizeof(buf),
                  "used_memory:%llu\r\n"
                  "expired_keys:%llu\r\n"
                  "evicted_keys:%llu\r\n"
                  "tier_disk_bytes:%llu\r\n"
                  "tier_live_records:%llu\r\n"
                  "tier_failed:%llu\r\n"
                  "dbsize:%llu\r\n"
                  "ndbs:%d\r\n"
                  "io_loops:%llu\r\n"
                  "io_events:%llu\r\n"
                  "io_reads:%llu\r\n"
                  "io_writes:%llu\r\n"
                  "io_bytes_read:%llu\r\n"
                  "io_bytes_written:%llu\r\n",
                  (unsigned long long)st->used_memory,
                  (unsigned long long)st->expired_keys,
                  (unsigned long long)st->evicted_keys,
                  (unsigned long long)st->tier_disk_bytes,
                  (unsigned long long)st->tier_live_records,
                  (unsigned long long)st->tier_failed,
                  (unsigned long long)st->dbsize, st->ndbs,
                  (unsigned long long)st->io.loops,
                  (unsigned long long)st->io.events,
                  (unsigned long long)st->io.reads,
                  (unsigned long long)st->io.writes,
                  (unsigned long long)st->io.bytes_read,
                  (unsigned long long)st->io.bytes_written);
    for (i = 0; i < st->ndbs; i++) {
        if (st->db_keys[i] > 0)
            n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                           "db:%d:%llu:%llu\r\n", i,
                           (unsigned long long)st->db_keys[i],
                           (unsigned long long)st->db_expires[i]);
    }
    for (id = 1; id <= CMD_MAX; id++) {
        if (st->cmd_calls[id] > 0)
            n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                           "c:%u:%llu:%llu\r\n", (unsigned)id,
                           (unsigned long long)st->cmd_calls[id],
                           (unsigned long long)st->cmd_usecs[id]);
    }
    resp_write_bulk(out, buf, (size_t)n2);
}

void command_info_render(const db *home, const repl_info *repl,
                         const info_stats *st, resp_buf *out)
{
    char human[32];
    char buf[16384];
    int n2;
    int i;
    uint16_t id;
    int any = 0;
    human_bytes(st->used_memory, human, sizeof(human));
    n2 = snprintf(buf, sizeof(buf),
                  "# Memory\r\n"
                  "used_memory:%llu\r\n"
                  "used_memory_human:%s\r\n"
                  "maxmemory:%llu\r\n"
                  "maxmemory_policy:%s\r\n"
                  "# Tiering\r\n"
                  "tiered_storage:%s\r\n"
                  "tiered_storage_dir:%s\r\n"
                  "tiered_storage_max_disk_bytes:%llu\r\n"
                  "tier_disk_bytes:%llu\r\n"
                  "tier_live_records:%llu\r\n"
                  "tier_failed:%llu\r\n"
                  "# Stats\r\n"
                  "expired_keys:%llu\r\n"
                  "evicted_keys:%llu\r\n"
                  "# Keyspace\r\n"
                  "dbsize:%llu\r\n",
                  (unsigned long long)st->used_memory, human,
                  (unsigned long long)home->maxmemory,
                  policy_name(home->maxmemory_policy),
                  home->tier_enabled ? "yes" : "no",
                  home->tier_dir,
                  (unsigned long long)home->tier_max_disk_bytes,
                  (unsigned long long)st->tier_disk_bytes,
                  (unsigned long long)st->tier_live_records,
                  (unsigned long long)st->tier_failed,
                  (unsigned long long)st->expired_keys,
                  (unsigned long long)st->evicted_keys,
                  (unsigned long long)st->dbsize);
    /* Redis-style per-db keyspace sections for non-empty dbs */
    for (i = 0; i < st->ndbs; i++) {
        if (st->db_keys[i] > 0)
            n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                           "db%d:keys=%llu,expires=%llu,avg_ttl=0\r\n", i,
                           (unsigned long long)st->db_keys[i],
                           (unsigned long long)st->db_expires[i]);
    }
    /* server IO counters (Phase 27) + derived total command count */
    {
        uint64_t total_cmds = 0;
        for (id = 1; id <= CMD_MAX; id++)
            total_cmds += st->cmd_calls[id];
        n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                       "# IO\r\n"
                       "io_loops:%llu\r\n"
                       "io_events:%llu\r\n"
                       "io_reads:%llu\r\n"
                       "io_writes:%llu\r\n"
                       "io_bytes_read:%llu\r\n"
                       "io_bytes_written:%llu\r\n"
                       "total_commands:%llu\r\n",
                       (unsigned long long)st->io.loops,
                       (unsigned long long)st->io.events,
                       (unsigned long long)st->io.reads,
                       (unsigned long long)st->io.writes,
                       (unsigned long long)st->io.bytes_read,
                       (unsigned long long)st->io.bytes_written,
                       (unsigned long long)total_cmds);
    }
    n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                   "# Cluster\r\n"
                   "cluster_enabled:%d\r\n",
                   home->cluster_enabled);
    /* Redis-style per-command statistics */
    for (id = 1; id <= CMD_MAX; id++) {
        if (st->cmd_calls[id] > 0) {
            const cmd_entry *e = cmd_table_entry(id);
            if (!any) {
                n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                               "# Commandstats\r\n");
                any = 1;
            }
            n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                           "cmdstat_%s:calls=%llu,usec=%llu,"
                           "usec_per_call=%.2f\r\n",
                           e->name,
                           (unsigned long long)st->cmd_calls[id],
                           (unsigned long long)st->cmd_usecs[id],
                           (double)st->cmd_usecs[id] /
                               (double)st->cmd_calls[id]);
        }
    }
    if (repl != NULL) {
        n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                       "# Replication\r\n"
                       "role:%s\r\n"
                       "connected_slaves:%llu\r\n"
                       "master_replid:%s\r\n"
                       "master_repl_offset:%llu\r\n",
                       repl->role == SESSION_ROLE_REPLICA ? "slave"
                                                          : "master",
                       (unsigned long long)repl->connected_slaves,
                       repl->role == SESSION_ROLE_REPLICA
                           ? repl->master_replid
                           : repl->replid,
                       (unsigned long long)repl->offset);
        if (repl->role == SESSION_ROLE_REPLICA)
            n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                           "master_host:%s\r\n"
                           "master_port:%u\r\n"
                           "master_link_status:%s\r\n",
                           repl->master_host, (unsigned)repl->master_port,
                           repl->link_up ? "up" : "down");
    }
    resp_write_bulk(out, buf, (size_t)n2);
}

/* ------------------------------------------------------------------ */
/* expiration commands                                                */
/* ------------------------------------------------------------------ */

/* EXPIRE/PEXPIRE (relative) and EXPIREAT/PEXPIREAT (absolute).
 * scale is 1000 for the second-based variants, 1 for ms. */
static void cmd_expire(db *d, const resp_value *argv, size_t argc,
                       resp_buf *out, uint64_t now, long long scale,
                       int absolute, const char *cmdname)
{
    const char *k, *t;
    size_t kl, tl;
    long long tv, base, exp;
    const char *v;
    size_t vl;
    int nx = 0, xx = 0, gt = 0, lt = 0;
    size_t i;

    if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &t, &tl)) {
        resp_write_error(out, "ERR invalid argument type", 24);
        return;
    }
    if (!storage_key_ok(kl)) {
        storage_length_error(out);
        return;
    }
    if (!parse_i64(t, tl, &tv)) {
        resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
        return;
    }
    for (i = 3; i < argc; i++) {
        const char *o;
        size_t ol;
        if (!arg_str(&argv[i], &o, &ol)) {
            resp_write_error(out, "ERR invalid argument type", 24);
            return;
        }
        if (ci_equal(o, ol, "NX") && !nx && !xx) {
            nx = 1;
        } else if (ci_equal(o, ol, "XX") && !nx && !xx) {
            xx = 1;
        } else if (ci_equal(o, ol, "GT") && !gt && !lt) {
            gt = 1;
        } else if (ci_equal(o, ol, "LT") && !gt && !lt) {
            lt = 1;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
    }
    if (!db_get(d, k, kl, &v, &vl, now)) {
        resp_write_integer(out, 0);
        return;
    }
    if (nx || xx || gt || lt) {
        const char *ev;
        size_t evl;
        int has_ttl = rh_get(&d->expires, k, kl, &ev, &evl) && evl == 8;
        long long cur = has_ttl ? (long long)get_u64(ev) : 0;
        base = absolute ? 0 : (long long)now;
        if (tv > 0 && tv > (LLONG_MAX - base) / scale) {
            char msg[96];
            int n = snprintf(msg, sizeof(msg),
                             "ERR invalid expire time in '%s' command", cmdname);
            resp_write_error(out, msg, (size_t)n);
            return;
        }
        exp = base + tv * scale;
        if (nx && has_ttl) {
            resp_write_integer(out, 0);
            return;
        }
        if (xx && !has_ttl) {
            resp_write_integer(out, 0);
            return;
        }
        if (gt && has_ttl && exp <= cur) {
            resp_write_integer(out, 0);
            return;
        }
        if (lt && (!has_ttl || exp >= cur)) {
            resp_write_integer(out, 0);
            return;
        }
        if (exp <= (long long)now) {
            db_del_kv(d, k, kl);
            resp_write_integer(out, 1);
            return;
        }
        if (db_set_expiry(d, k, kl, (uint64_t)exp) != 0) {
            storage_length_error(out);
            return;
        }
        resp_write_integer(out, 1);
        return;
    }
    base = absolute ? 0 : (long long)now;
    if (tv > 0 && tv > (LLONG_MAX - base) / scale) {
        char msg[96];
        int n = snprintf(msg, sizeof(msg),
                         "ERR invalid expire time in '%s' command", cmdname);
        resp_write_error(out, msg, (size_t)n);
        return;
    }
    exp = base + tv * scale;
    if (exp <= (long long)now) {
        /* negative/past ttl: delete the key, report success */
        db_del_kv(d, k, kl);
        resp_write_integer(out, 1);
        return;
    }
    if (db_set_expiry(d, k, kl, (uint64_t)exp) != 0) {
        storage_length_error(out);
        return;
    }
    resp_write_integer(out, 1);
}

static void cmd_ttl(db *d, const resp_value *argv, resp_buf *out, uint64_t now,
                    int pttl)
{
    const char *k;
    size_t kl;
    const char *v;
    size_t vl;

    if (!arg_str(&argv[1], &k, &kl)) {
        resp_write_error(out, "ERR invalid argument type", 24);
        return;
    }
    if (!db_get(d, k, kl, &v, &vl, now)) {
        resp_write_integer(out, -2); /* no such key */
        return;
    }
    if (!rh_get(&d->expires, k, kl, &v, &vl) || vl != 8) {
        resp_write_integer(out, -1); /* no expiry */
        return;
    }
    {
        long long rem = (long long)get_u64(v) - (long long)now;
        if (rem < 0)
            rem = 0;
        resp_write_integer(out, pttl ? rem : (rem + 500) / 1000);
    }
}

/* ------------------------------------------------------------------ */
/* generic key command helpers (TYPE/KEYS/SCAN/RANDOMKEY)             */
/* ------------------------------------------------------------------ */

/* Redis type name for a value blob tag. */
static const char *obj_type_name(int tag)
{
    switch (tag) {
    case DDUP_OBJ_STRING: return "string";
    case DDUP_OBJ_HASH:   return "hash";
    case DDUP_OBJ_LIST:   return "list";
    case DDUP_OBJ_SET:    return "set";
    case DDUP_OBJ_ZSET:   return "zset";
    case DDUP_OBJ_STREAM: return "stream";
    default:              return "none";
}
}

/* SCAN cursor: decimal non-negative integer, digits only. */
static int parse_cursor(const char *s, size_t len, size_t *out)
{
    size_t v = 0;
    size_t i;
    if (len == 0)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned digit;
        if (s[i] < '0' || s[i] > '9')
            return 0;
        digit = (unsigned)(s[i] - '0');
        if (v > (SIZE_MAX - digit) / 10)
            return 0;
        v = v * 10 + digit;
    }
    *out = v;
    return 1;
}

/* Insert a "*N\r\n" array header at byte position pos (KEYS appends its
 * elements first; the count is only known at the end). */
static void resp_insert_array_header(resp_buf *b, size_t pos, size_t n)
{
    char hdr[24];
    int hl = snprintf(hdr, sizeof(hdr), "*%llu\r\n", (unsigned long long)n);
    size_t tail = b->len - pos;
    if (hl <= 0 || resp_buf_reserve(b, (size_t)hl) != 0)
        return;
    memmove(b->data + pos + (size_t)hl, b->data + pos, tail);
    memcpy(b->data + pos, hdr, (size_t)hl);
    b->len += (size_t)hl;
}

typedef struct keys_ctx {
    db *d;
    uint64_t now_ms;
    const char *pat;
    size_t plen;
    resp_buf *out;
    size_t nmatch;
} keys_ctx;

/* KEYS visitor: expired keys are lazily collected, survivors glob-filtered
 * and appended straight into the reply (no intermediate container). */
static int keys_cb(const char *key, size_t klen, const char *val,
                   size_t vlen, void *ctx)
{
    keys_ctx *c = (keys_ctx *)ctx;
    (void)val;
    (void)vlen;
    if (db_expire_if_needed(c->d, key, klen, c->now_ms))
        return 0;
    if (!ddup_glob_match(c->pat, c->plen, key, klen))
        return 0;
    resp_write_bulk(c->out, key, klen);
    c->nmatch++;
    return 0;
}

#define SCAN_BATCH 32 /* matching keys collected per rh_scan call */

typedef struct scan_ctx {
    db *d;
    uint64_t now_ms;
    const char *pat; /* NULL = no MATCH filter */
    size_t plen;
    const char *keys[SCAN_BATCH]; /* entry views; valid until the batch is
                                   * flushed (no mutation in between) */
    size_t klens[SCAN_BATCH];
    size_t n;
} scan_ctx;

static int scan_cb(const char *key, size_t klen, const char *val,
                   size_t vlen, void *ctx)
{
    scan_ctx *c = (scan_ctx *)ctx;
    (void)val;
    (void)vlen;
    if (db_expire_if_needed(c->d, key, klen, c->now_ms))
        return 0;
    if (c->pat != NULL && !ddup_glob_match(c->pat, c->plen, key, klen))
        return 0;
    c->keys[c->n] = key;
    c->klens[c->n] = klen;
    c->n++;
    return c->n == SCAN_BATCH; /* batch full: stop early */
}

typedef struct randomkey_ctx {
    db *d;
    uint64_t now_ms;
    const char *key;
    size_t klen;
} randomkey_ctx;

/* RANDOMKEY fallback: first live key in bucket order. */
static int randomkey_cb(const char *key, size_t klen, const char *val,
                        size_t vlen, void *ctx)
{
    randomkey_ctx *c = (randomkey_ctx *)ctx;
    (void)val;
    (void)vlen;
    if (db_expire_if_needed(c->d, key, klen, c->now_ms))
        return 0;
    c->key = key;
    c->klen = klen;
    return 1;
}

typedef struct scan_object_opt {
    size_t cursor;
    const char *pat;
    size_t plen;
    long long count;
} scan_object_opt;

static int parse_object_scan(const resp_value *argv, size_t argc,
                             resp_buf *out, scan_object_opt *opt)
{
    const char *cur;
    size_t curl;
    size_t i;

    if (!arg_str(&argv[2], &cur, &curl)) {
        resp_write_error(out, "ERR invalid argument type", 24);
        return -1;
    }
    if (!parse_cursor(cur, curl, &opt->cursor)) {
        resp_write_error(out, "ERR invalid cursor", 18);
        return -1;
    }
    opt->pat = NULL;
    opt->plen = 0;
    opt->count = 10;
    for (i = 3; i < argc; i += 2) {
        const char *name, *val;
        size_t namel, vall;
        if (i + 1 >= argc) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return -1;
        }
        if (!arg_str(&argv[i], &name, &namel) ||
            !arg_str(&argv[i + 1], &val, &vall)) {
            resp_write_error(out, "ERR invalid argument type", 24);
            return -1;
        }
        if (ci_equal(name, namel, "MATCH")) {
            opt->pat = val;
            opt->plen = vall;
        } else if (ci_equal(name, namel, "COUNT")) {
            if (!parse_i64(val, vall, &opt->count) || opt->count <= 0) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return -1;
            }
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return -1;
        }
    }
    return 0;
}

typedef struct hscan_ctx {
    const char *fields[SCAN_BATCH];
    size_t flens[SCAN_BATCH];
    const char *vals[SCAN_BATCH];
    size_t vlens[SCAN_BATCH];
    size_t n;
    const char *pat;
    size_t plen;
} hscan_ctx;

static void hscan_collect(hscan_ctx *c, const char *f, size_t flen,
                          const char *v, size_t vlen)
{
    if (c->n >= SCAN_BATCH)
        return;
    if (c->pat != NULL && !ddup_glob_match(c->pat, c->plen, f, flen))
        return;
    c->fields[c->n] = f;
    c->flens[c->n] = flen;
    c->vals[c->n] = v;
    c->vlens[c->n] = vlen;
    c->n++;
}

static int hscan_ht_cb(const char *f, size_t flen, const char *v,
                       size_t vlen, void *ctx)
{
    hscan_ctx *c = (hscan_ctx *)ctx;
    hscan_collect(c, f, flen, v, vlen);
    return c->n == SCAN_BATCH;
}

static size_t hscan_ht_run(obj_hash *h, size_t cursor, size_t count,
                            hscan_ctx *c)
{
    c->n = 0;
    return rh_scan(&h->fields, cursor, count, hscan_ht_cb, c);
}

static size_t hscan_lp_next(obj_hash *h, size_t cursor, size_t count,
                            const char *pat, size_t plen, size_t *n_out)
{
    uint64_t len = obj_hash_len(h);
    size_t idx = cursor;
    size_t visited = 0;
    size_t n = 0;
    while (idx < len && visited < count && n < SCAN_BATCH) {
        const char *f, *v;
        size_t fl, vl;
        idx++;
        visited++;
        if (!obj_hash_pair_at(h, (uint64_t)(idx - 1), &f, &fl, &v, &vl))
            break;
        if (pat == NULL || ddup_glob_match(pat, plen, f, fl))
            n++;
    }
    *n_out = n;
    return idx >= len ? 0 : idx;
}

static void hscan_lp_emit(obj_hash *h, size_t cursor, size_t count,
                          const char *pat, size_t plen, resp_buf *out)
{
    uint64_t len = obj_hash_len(h);
    size_t idx = cursor;
    size_t visited = 0;
    size_t emitted = 0;
    while (idx < len && visited < count && emitted < SCAN_BATCH) {
        const char *f, *v;
        size_t fl, vl;
        idx++;
        visited++;
        if (!obj_hash_pair_at(h, (uint64_t)(idx - 1), &f, &fl, &v, &vl))
            break;
        if (pat != NULL && !ddup_glob_match(pat, plen, f, fl))
            continue;
        resp_write_bulk(out, f, fl);
        resp_write_bulk(out, v, vl);
        emitted++;
    }
}

typedef struct sscan_ctx {
    const char *members[SCAN_BATCH];
    size_t mlens[SCAN_BATCH];
    size_t n;
    const char *pat;
    size_t plen;
} sscan_ctx;

static void sscan_collect(sscan_ctx *c, const char *m, size_t mlen)
{
    if (c->n >= SCAN_BATCH)
        return;
    if (c->pat != NULL && !ddup_glob_match(c->pat, c->plen, m, mlen))
        return;
    c->members[c->n] = m;
    c->mlens[c->n] = mlen;
    c->n++;
}

static int sscan_ht_cb(const char *m, size_t mlen, const char *v,
                       size_t vlen, void *ctx)
{
    sscan_ctx *c = (sscan_ctx *)ctx;
    (void)v;
    (void)vlen;
    sscan_collect(c, m, mlen);
    return c->n == SCAN_BATCH;
}

static size_t sscan_ht_run(obj_set *s, size_t cursor, size_t count,
                            sscan_ctx *c)
{
    c->n = 0;
    return rh_scan(&s->members, cursor, count, sscan_ht_cb, c);
}

static size_t sscan_lp_next(obj_set *s, size_t cursor, size_t count,
                            const char *pat, size_t plen, size_t *n_out)
{
    uint64_t len = obj_set_len(s);
    size_t idx = cursor;
    size_t visited = 0;
    size_t n = 0;
    while (idx < len && visited < count && n < SCAN_BATCH) {
        const char *m;
        size_t ml;
        idx++;
        visited++;
        if (!obj_set_member_at(s, (uint64_t)(idx - 1), &m, &ml))
            break;
        if (pat == NULL || ddup_glob_match(pat, plen, m, ml))
            n++;
    }
    *n_out = n;
    return idx >= len ? 0 : idx;
}

static void sscan_lp_emit(obj_set *s, size_t cursor, size_t count,
                          const char *pat, size_t plen, resp_buf *out)
{
    uint64_t len = obj_set_len(s);
    size_t idx = cursor;
    size_t visited = 0;
    size_t emitted = 0;
    while (idx < len && visited < count && emitted < SCAN_BATCH) {
        const char *m;
        size_t ml;
        idx++;
        visited++;
        if (!obj_set_member_at(s, (uint64_t)(idx - 1), &m, &ml))
            break;
        if (pat != NULL && !ddup_glob_match(pat, plen, m, ml))
            continue;
        resp_write_bulk(out, m, ml);
        emitted++;
    }
}

typedef struct zscan_ctx {
    obj_zset *z;
    const char *members[SCAN_BATCH];
    size_t mlens[SCAN_BATCH];
    double scores[SCAN_BATCH];
    size_t n;
    const char *pat;
    size_t plen;
} zscan_ctx;

static void zscan_collect(zscan_ctx *c, const char *m, size_t mlen, double sc)
{
    if (c->n >= SCAN_BATCH)
        return;
    if (c->pat != NULL && !ddup_glob_match(c->pat, c->plen, m, mlen))
        return;
    c->members[c->n] = m;
    c->mlens[c->n] = mlen;
    c->scores[c->n] = sc;
    c->n++;
}

static int zscan_ht_cb(const char *m, size_t mlen, const char *v,
                       size_t vlen, void *ctx)
{
    zscan_ctx *c = (zscan_ctx *)ctx;
    double sc;
    (void)v;
    (void)vlen;
    if (!obj_zset_score(c->z, m, mlen, &sc))
        return 0;
    zscan_collect(c, m, mlen, sc);
    return c->n == SCAN_BATCH;
}

static size_t zscan_ht_run(obj_zset *z, size_t cursor, size_t count,
                            zscan_ctx *c)
{
    c->z = z;
    c->n = 0;
    return rh_scan(&z->dict, cursor, count, zscan_ht_cb, c);
}

static size_t zscan_lp_next(obj_zset *z, size_t cursor, size_t count,
                            const char *pat, size_t plen, size_t *n_out)
{
    uint64_t len = obj_zset_len(z);
    size_t idx = cursor;
    size_t visited = 0;
    size_t n = 0;
    while (idx < len && visited < count && n < SCAN_BATCH) {
        obj_zset_iter it;
        const char *m;
        size_t ml;
        idx++;
        visited++;
        if (!obj_zset_seek(z, (size_t)(idx - 1), &it))
            break;
        m = obj_zset_iter_member(&it, &ml);
        if (pat == NULL || ddup_glob_match(pat, plen, m, ml))
            n++;
    }
    *n_out = n;
    return idx >= len ? 0 : idx;
}

static void zscan_lp_emit(obj_zset *z, size_t cursor, size_t count,
                          const char *pat, size_t plen, resp_buf *out)
{
    uint64_t len = obj_zset_len(z);
    size_t idx = cursor;
    size_t visited = 0;
    size_t emitted = 0;
    while (idx < len && visited < count && emitted < SCAN_BATCH) {
        obj_zset_iter it;
        const char *m;
        size_t ml;
        double sc;
        char num[40];
        int nl;
        idx++;
        visited++;
        if (!obj_zset_seek(z, (size_t)(idx - 1), &it))
            break;
        m = obj_zset_iter_member(&it, &ml);
        sc = obj_zset_iter_score(&it);
        if (pat != NULL && !ddup_glob_match(pat, plen, m, ml))
            continue;
        nl = fmt_score(num, sizeof(num), sc);
        resp_write_bulk(out, m, ml);
        resp_write_bulk(out, num, (size_t)nl);
        emitted++;
    }
}


/* ------------------------------------------------------------------ */
/* HyperLogLog (dense-only Redis 7 compatible encoding)               */
/* ------------------------------------------------------------------ */

#define HLL_P 14
#define HLL_REGISTERS 16384u
#define HLL_BITS 6
#define HLL_DENSE_REG_BYTES ((HLL_REGISTERS * HLL_BITS + 7u) / 8u)
#define HLL_DENSE_HEADER 16u
#define HLL_DENSE_SIZE (HLL_DENSE_HEADER + HLL_DENSE_REG_BYTES)
#define HLL_DENSE_ENC 0
#define HLL_CARD_INVALID UINT64_MAX

static const char HLL_INVALID_VALUE[] =
    "WRONGTYPE Key is not a valid HyperLogLog string value.";

static uint64_t hll_murmur64(const char *key, size_t len)
{
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;
    uint64_t h = 0xadc83b19ULL ^ ((uint64_t)len * m);
    const unsigned char *p = (const unsigned char *)key;
    size_t nwords = len / 8;
    size_t i;

    for (i = 0; i < nwords; i++) {
        uint64_t k;
        memcpy(&k, p + i * 8, 8);
        k *= m;
        k ^= k >> r;
        k *= m;
        h ^= k;
        h *= m;
    }

    p += nwords * 8;
    switch (len & 7u) {
    case 7: h ^= (uint64_t)p[6] << 48; /* fall through */
    case 6: h ^= (uint64_t)p[5] << 40; /* fall through */
    case 5: h ^= (uint64_t)p[4] << 32; /* fall through */
    case 4: h ^= (uint64_t)p[3] << 24; /* fall through */
    case 3: h ^= (uint64_t)p[2] << 16; /* fall through */
    case 2: h ^= (uint64_t)p[1] << 8;  /* fall through */
    case 1: h ^= (uint64_t)p[0];       /* fall through */
    default: break;
    }
    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

static int hll_pat_len(const char *ele, size_t len, size_t *reg)
{
    uint64_t hash = hll_murmur64(ele, len);
    uint64_t bit = 1;
    int count = 1;

    *reg = (size_t)(hash & (HLL_REGISTERS - 1u));
    hash >>= HLL_P;
    hash |= (1ULL << (64 - HLL_P));
    while ((hash & bit) == 0 && count < 64) {
        count++;
        bit <<= 1;
    }
    return count;
}

static int hll_dense_valid(const char *s, size_t len)
{
    return len == HLL_DENSE_SIZE && memcmp(s, "HYLL", 4) == 0 &&
           (unsigned char)s[4] == HLL_DENSE_ENC;
}

static unsigned char *hll_regs_mut(unsigned char *h)
{
    return h + HLL_DENSE_HEADER;
}

static const unsigned char *hll_regs(const unsigned char *h)
{
    return h + HLL_DENSE_HEADER;
}

static void hll_card_invalidate(unsigned char *h)
{
    memset(h + 8, 0xFF, 8);
}

static void hll_dense_init(unsigned char *h)
{
    memset(h, 0, HLL_DENSE_SIZE);
    memcpy(h, "HYLL", 4);
    h[4] = HLL_DENSE_ENC;
    hll_card_invalidate(h);
}

static uint8_t hll_dense_get(const unsigned char *regs, size_t idx)
{
    size_t byte = (idx * HLL_BITS) / 8u;
    unsigned fb = (unsigned)((idx * HLL_BITS) % 8u);
    uint8_t lo = regs[byte];
    uint8_t hi = regs[byte + 1];

    return (uint8_t)(((uint16_t)lo >> fb) |
                     ((uint16_t)hi << (8u - fb))) &
           (uint8_t)((1u << HLL_BITS) - 1u);
}

static void hll_dense_set(unsigned char *regs, size_t idx, uint8_t val)
{
    size_t byte = (idx * HLL_BITS) / 8u;
    unsigned fb = (unsigned)((idx * HLL_BITS) % 8u);
    uint8_t low_mask = (uint8_t)(((1u << HLL_BITS) - 1u) << fb);
    uint8_t high_mask =
        fb == 0 ? 0 : (uint8_t)(((1u << HLL_BITS) - 1u) >> (8u - fb));

    regs[byte] = (uint8_t)((regs[byte] & (uint8_t)~low_mask) |
                           ((uint8_t)(val << fb) & low_mask));
    if (fb != 0) {
        regs[byte + 1] =
            (uint8_t)((regs[byte + 1] & (uint8_t)~high_mask) |
                      ((uint8_t)(val >> (8u - fb)) & high_mask));
    }
}

static double hll_dense_estimate(const unsigned char *h)
{
    uint64_t histo[64] = {0};
    const unsigned char *regs = hll_regs(h);
    double m = (double)HLL_REGISTERS;
    double alpha;
    double sum = 0.0;
    double e;
    size_t i;
    unsigned v;

    for (i = 0; i < HLL_REGISTERS; i++)
        histo[hll_dense_get(regs, i)]++;
    if (histo[0] == HLL_REGISTERS)
        return 0.0;

    for (v = 1; v < 64; v++) {
        if (histo[v] != 0)
            sum += (double)histo[v] * pow(2.0, -(double)v);
    }

    alpha = 0.7213 / (1.0 + 1.079 / m);
    e = alpha * m * m / sum;
    {
        double lc = m * log(m / (double)histo[0]);
        if (lc <= 2.5 * m)
            e = lc;
    }
    return e;
}

static int hll_write_reply_value(resp_buf *out, const char *v, size_t vl,
                                 const unsigned char **regs_out)
{
    const char *s;
    size_t sl;
    const unsigned char *regs;

    if (!as_string(out, v, vl, &s, &sl))
        return -1;
    if (!hll_dense_valid(s, sl)) {
        resp_write_error(out, HLL_INVALID_VALUE,
                         sizeof(HLL_INVALID_VALUE) - 1);
        return -1;
    }
    regs = hll_regs((const unsigned char *)s);
    if (regs_out != NULL)
        *regs_out = regs;
    return 0;
}

static void cmd_pfadd(db *d, const resp_value *argv, size_t argc,
                      resp_buf *out, uint64_t now_ms)
{
    const char *key;
    size_t keyl;
    const char *v;
    size_t vl;
    unsigned char *h;
    unsigned char *regs;
    int exists;
    int changed = 0;
    size_t i;

    if (argc < 3) {
        wrong_args(out, "pfadd");
        return;
    }
    if (!arg_str(&argv[1], &key, &keyl))
        goto bad_type;
    if (!storage_string_ok(keyl, HLL_DENSE_SIZE)) {
        storage_length_error(out);
        return;
    }

    h = (unsigned char *)malloc(HLL_DENSE_SIZE);
    if (h == NULL) {
        oom_blocked(d, out);
        return;
    }

    exists = db_get(d, key, keyl, &v, &vl, now_ms);
    if (exists) {
        const char *s;
        size_t sl;
        if (!as_string(out, v, vl, &s, &sl)) {
            free(h);
            return;
        }
        if (!hll_dense_valid(s, sl)) {
            free(h);
            resp_write_error(out, HLL_INVALID_VALUE,
                             sizeof(HLL_INVALID_VALUE) - 1);
            return;
        }
        memcpy(h, s, HLL_DENSE_SIZE);
    } else {
        hll_dense_init(h);
    }
    regs = hll_regs_mut(h);

    for (i = 2; i < argc; i++) {
        const char *ele;
        size_t elel;
        size_t reg = 0;
        int val;
        uint8_t old;
        if (!arg_str(&argv[i], &ele, &elel)) {
            free(h);
            goto bad_type;
        }
        val = hll_pat_len(ele, elel, &reg);
        old = hll_dense_get(regs, reg);
        if ((uint8_t)val > old) {
            hll_dense_set(regs, reg, (uint8_t)val);
            changed = 1;
        }
    }

    if (changed) {
        hll_card_invalidate(h);
        if (db_set_string(d, key, keyl, (const char *)h, HLL_DENSE_SIZE,
                          now_ms) != 0) {
            free(h);
            storage_length_error(out);
            return;
        }
    }
    free(h);
    resp_write_integer(out, changed ? 1 : 0);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_pfcount(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    unsigned char *merged;
    int any = 0;
    uint64_t card;
    size_t i;

    if (argc < 2) {
        wrong_args(out, "pfcount");
        return;
    }

    if (argc == 2) {
        const char *key;
        size_t keyl;
        const char *v;
        size_t vl;
        if (!arg_str(&argv[1], &key, &keyl))
            goto bad_type;
        if (!db_get(d, key, keyl, &v, &vl, now_ms)) {
            resp_write_integer(out, 0);
            return;
        }
        {
            const char *s;
            size_t sl;
            if (!as_string(out, v, vl, &s, &sl))
                return;
            if (!hll_dense_valid(s, sl)) {
                resp_write_error(out, HLL_INVALID_VALUE,
                                 sizeof(HLL_INVALID_VALUE) - 1);
                return;
            }
            card = (uint64_t)hll_dense_estimate((const unsigned char *)s);
        }
        resp_write_integer(out, (long long)card);
        return;
    }

    merged = (unsigned char *)malloc(HLL_DENSE_SIZE);
    if (merged == NULL) {
        oom_blocked(d, out);
        return;
    }
    hll_dense_init(merged);

    for (i = 1; i < argc; i++) {
        const char *key;
        size_t keyl;
        const char *v;
        size_t vl;
        const unsigned char *regs;
        unsigned char *dst = hll_regs_mut(merged);
        size_t r;

        if (!arg_str(&argv[i], &key, &keyl)) {
            free(merged);
            goto bad_type;
        }
        if (!db_get(d, key, keyl, &v, &vl, now_ms))
            continue;
        if (hll_write_reply_value(out, v, vl, &regs) != 0) {
            free(merged);
            return;
        }
        any = 1;
        for (r = 0; r < HLL_REGISTERS; r++) {
            uint8_t a = hll_dense_get(dst, r);
            uint8_t b = hll_dense_get(regs, r);
            if (b > a)
                hll_dense_set(dst, r, b);
        }
    }

    card = any ? (uint64_t)hll_dense_estimate(merged) : 0;
    free(merged);
    resp_write_integer(out, (long long)card);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_pfmerge(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    const char *dst;
    size_t dstl;
    unsigned char *merged;
    unsigned char *dst_regs;
    size_t i;

    if (argc < 3) {
        wrong_args(out, "pfmerge");
        return;
    }
    if (!arg_str(&argv[1], &dst, &dstl))
        goto bad_type;
    if (!storage_string_ok(dstl, HLL_DENSE_SIZE)) {
        storage_length_error(out);
        return;
    }

    merged = (unsigned char *)malloc(HLL_DENSE_SIZE);
    if (merged == NULL) {
        oom_blocked(d, out);
        return;
    }
    hll_dense_init(merged);
    dst_regs = hll_regs_mut(merged);

    for (i = 2; i < argc; i++) {
        const char *key;
        size_t keyl;
        const char *v;
        size_t vl;
        const unsigned char *regs;
        size_t r;

        if (!arg_str(&argv[i], &key, &keyl)) {
            free(merged);
            goto bad_type;
        }
        if (!db_get(d, key, keyl, &v, &vl, now_ms))
            continue;
        if (hll_write_reply_value(out, v, vl, &regs) != 0) {
            free(merged);
            return;
        }
        for (r = 0; r < HLL_REGISTERS; r++) {
            uint8_t a = hll_dense_get(dst_regs, r);
            uint8_t b = hll_dense_get(regs, r);
            if (b > a)
                hll_dense_set(dst_regs, r, b);
        }
    }

    hll_card_invalidate(merged);
    if (db_set_string(d, dst, dstl, (const char *)merged, HLL_DENSE_SIZE,
                      now_ms) != 0) {
        free(merged);
        storage_length_error(out);
        return;
    }
    free(merged);
    resp_write_simple_string(out, "OK", 2);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_pfdebug(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    const char *sub;
    size_t subl;
    const char *key;
    size_t keyl;
    const char *v;
    size_t vl;
    const unsigned char *regs;

    if (argc < 2) {
        wrong_args(out, "pfdebug");
        return;
    }
    if (!arg_str(&argv[1], &sub, &subl))
        goto bad_type;

    if (argc != 3 || !arg_str(&argv[2], &key, &keyl))
        goto bad_type;
    if (!db_get(d, key, keyl, &v, &vl, now_ms)) {
        resp_write_error(out, HLL_INVALID_VALUE,
                         sizeof(HLL_INVALID_VALUE) - 1);
        return;
    }
    if (hll_write_reply_value(out, v, vl, &regs) != 0)
        return;

    if (ci_equal(sub, subl, "ENCODING")) {
        resp_write_bulk(out, "dense", 5);
        return;
    }
    if (ci_equal(sub, subl, "GETREG")) {
        size_t i;
        resp_write_array_header(out, HLL_REGISTERS);
        for (i = 0; i < HLL_REGISTERS; i++)
            resp_write_integer(out, (long long)hll_dense_get(regs, i));
        return;
    }
    if (ci_equal(sub, subl, "DECODE")) {
        size_t i;
        resp_write_array_header(out, 2);
        resp_write_bulk(out, "dense", 5);
        resp_write_array_header(out, HLL_REGISTERS);
        for (i = 0; i < HLL_REGISTERS; i++)
            resp_write_integer(out, (long long)hll_dense_get(regs, i));
        return;
    }
    if (ci_equal(sub, subl, "TODENSE")) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    resp_write_error(out, "ERR Unknown PFDEBUG subcommand", 29);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_pfselftest(db *d, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    (void)d;
    (void)argv;
    (void)now_ms;
    if (argc != 1) {
        wrong_args(out, "pfselftest");
        return;
    }
    resp_write_simple_string(out, "OK", 2);
}

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

static const char CROSSSLOT_MSG[] =
    "CROSSSLOT Keys in request don't hash to the same slot";

/* Accumulate a key into a single-slot set: 1 ok, 0 on mismatch. */
static int slot_accum(const char *k, size_t klen, int *have, uint32_t *slot)
{
    uint32_t s = hash_slot(k, klen);
    if (!*have) {
        *have = 1;
        *slot = s;
        return 1;
    }
    return *slot == s;
}

/* Legacy GEORADIUS/GEORADIUSBYMEMBER STORE/STOREDIST destination key. */
static int geo_radius_store_key(const resp_value *argv, size_t argc,
                                size_t start, const char **store,
                                size_t *storelen)
{
    size_t i;

    for (i = start; i + 1 < argc; i++) {
        const char *tok;
        size_t tokl;

        if (!arg_str(&argv[i], &tok, &tokl))
            continue;
        if (ci_equal(tok, tokl, "STORE") ||
            ci_equal(tok, tokl, "STOREDIST")) {
            if (arg_str(&argv[i + 1], store, storelen))
                return 1;
            return 0;
        }
    }
    return 0;
}

/* Single-slot accumulation for the keys of one command (cluster mode).
 * Multi-key commands check all their key positions; single-key commands
 * check argv[1]; keyless commands always pass. The {have,slot} accumulator
 * can be shared across a MULTI queue to compare commands with each other. */
static int cmd_keys_accum(const resp_value *argv, size_t argc, int *have,
                          uint32_t *slot)
{
    const char *name;
    size_t nlen;
    size_t i;
    uint16_t cmd_id;
    if (argc == 0 || !arg_str(&argv[0], &name, &nlen))
        return 1;
    if (argc < 2)
        return 1;
    cmd_id = cmd_resolve(name, nlen);
    if (cmd_id == CMD_MGET || cmd_id == CMD_DEL ||
        cmd_id == CMD_UNLINK || cmd_id == CMD_EXISTS ||
        cmd_id == CMD_TOUCH ||
        cmd_id == CMD_SINTER || cmd_id == CMD_SUNION ||
        cmd_id == CMD_SDIFF || cmd_id == CMD_WATCH ||
        cmd_id == CMD_SINTERSTORE || cmd_id == CMD_SUNIONSTORE ||
        cmd_id == CMD_SDIFFSTORE || cmd_id == CMD_BITOP) {
        for (i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_MSET || cmd_id == CMD_MSETNX) {
        for (i = 1; i + 1 < argc; i += 2) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_SMOVE || cmd_id == CMD_RENAME ||
        cmd_id == CMD_RENAMENX || cmd_id == CMD_RPOPLPUSH ||
        cmd_id == CMD_LMOVE || cmd_id == CMD_LMOVEM ||
        cmd_id == CMD_COPY || cmd_id == CMD_LCS ||
        cmd_id == CMD_BRPOPLPUSH || cmd_id == CMD_BLMOVE ||
        cmd_id == CMD_BLMOVEM) {
        for (i = 1; i < argc && i < 3; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_BLPOP || cmd_id == CMD_BRPOP ||
        cmd_id == CMD_BZPOPMIN || cmd_id == CMD_BZPOPMAX) {
        for (i = 1; i + 1 < argc; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_BLMPOP || cmd_id == CMD_BZMPOP) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 4 || !arg_str(&argv[2], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 3 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 3; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }

    if (cmd_id == CMD_SINTERCARD) {
        /* keys are argv[2..2+numkeys); numkeys validated by the dispatch */
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 3 || !arg_str(&argv[1], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 2 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 2; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_SUNIONCARD || cmd_id == CMD_SDIFFCARD) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 3 || !arg_str(&argv[1], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 2 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 2; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_ZUNIONSTORE || cmd_id == CMD_ZINTERSTORE ||
        cmd_id == CMD_ZDIFFSTORE) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        const char *k;
        size_t kl;
        if (argc < 4 || !arg_str(&argv[1], &k, &kl) ||
            !arg_str(&argv[2], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        if (!slot_accum(k, kl, have, slot))
            return 0;
        end = 3 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 3; i < end; i++) {
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_ZUNION || cmd_id == CMD_ZINTER ||
        cmd_id == CMD_ZDIFF || cmd_id == CMD_ZINTERCARD ||
        cmd_id == CMD_ZMPOP || cmd_id == CMD_LMPOP) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 3 || !arg_str(&argv[1], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 2 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 2; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_ZRANGESTORE) {
        for (i = 1; i < argc && i < 3; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_PFCOUNT || cmd_id == CMD_PFMERGE) {
        for (i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_PFDEBUG) {
        const char *k;
        size_t kl;
        if (argc < 3 || !arg_str(&argv[2], &k, &kl))
            return 1;
        return slot_accum(k, kl, have, slot);
    }
    if (cmd_id == CMD_GEOSEARCHSTORE) {
        for (i = 1; i < argc && i < 3; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_GEORADIUS || cmd_id == CMD_GEORADIUSBYMEMBER) {
        const char *k;
        const char *store;
        size_t kl;
        size_t sl;
        if (arg_str(&argv[1], &k, &kl) &&
            !slot_accum(k, kl, have, slot))
            return 0;
        if (geo_radius_store_key(argv, argc,
                                 cmd_id == CMD_GEORADIUS ? 6u : 5u,
                                 &store, &sl) &&
            !slot_accum(store, sl, have, slot))
            return 0;
        return 1;
    }
    if (cmd_id == CMD_EVAL || cmd_id == CMD_EVALSHA) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 4 || !arg_str(&argv[2], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 3 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 3; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_XGROUP || cmd_id == CMD_XINFO) {
        const char *k;
        size_t kl;
        if (argc < 3 || !arg_str(&argv[2], &k, &kl))
            return 1;
        return slot_accum(k, kl, have, slot);
    }
    if (cmd_id == CMD_XREAD || cmd_id == CMD_XREADGROUP) {
        size_t streams = 1;
        size_t nkeys;
        size_t start;
        while (streams < argc) {
            const char *tok;
            size_t tokl;
            if (arg_str(&argv[streams], &tok, &tokl) &&
                ci_equal(tok, tokl, "STREAMS"))
                break;
            streams++;
        }
        if (streams >= argc || (argc - streams - 1) < 2 ||
            ((argc - streams - 1) & 1u) != 0)
            return 1;
        nkeys = (argc - streams - 1) / 2;
        start = streams + 1;
        for (i = start; i < start + nkeys; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    /* everything else: single key at argv[1] */
    {
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            return 1;
        return slot_accum(k, kl, have, slot);
    }
}

static int cmd_keys_one_slot(const resp_value *argv, size_t argc)
{
    int have = 0;
    uint32_t slot = 0;
    return cmd_keys_accum(argv, argc, &have, &slot);
}

/* Write the CROSSSLOT error when cluster mode rejects a command. */
static int crossslot_reject(db *d, resp_buf *out, const resp_value *argv,
                            size_t argc)
{
    if (d->cluster_enabled && !cmd_keys_one_slot(argv, argc)) {
        resp_write_error(out, CROSSSLOT_MSG, sizeof(CROSSSLOT_MSG) - 1);
        return 1;
    }
    return 0;
}

/* Rebuild the slot->owner-node-index cache after any bitmap change. */
static void db_rebuild_slot_owner(db *d)
{
    int i, s;
    memset(d->slot_owner, 0xFF, sizeof(d->slot_owner));
    for (i = 0; i < d->nnodes; i++)
        for (s = 0; s < 16384; s++)
            if (cluster_slots_get(d->nodes[i].slots, (uint32_t)s))
                d->slot_owner[s] = (uint16_t)i;
    d->slot_owner_dirty = 0;
}

/* Cluster ownership for one key. Returns 1 when the key is served here
 * (myself owns its slot, or cluster mode is off, or a one-shot ASKING
 * bypass applies to an importing slot); otherwise writes the MOVED
 * (assigned to a peer), CLUSTERDOWN (unassigned) or ASK (migrating away,
 * key already gone) reply and returns 0. `asking` is the session's
 * one-shot ASKING flag. */
static int db_key_served(db *d, const char *key, size_t klen, resp_buf *out,
                         uint64_t now_ms, int asking)
{
    uint32_t slot, idx;
    if (!d->cluster_enabled)
        return 1;
    slot = hash_slot(key, klen);
    if (d->slot_owner_dirty)
        db_rebuild_slot_owner(d);
    idx = d->slot_owner[slot];
    if (idx == 0xFFFFu) {
        if (asking && d->slot_importing[slot] != 0xFFFFu)
            return 1;
        {
            static const char E[] = "CLUSTERDOWN Hash slot not served";
            resp_write_error(out, E, sizeof(E) - 1);
            return 0;
        }
    }
    if (d->nodes[idx].flags & CLUSTER_NODE_MYSELF) {
        uint16_t mt = d->slot_migrating[slot];
        if (mt != 0xFFFFu) {
            const char *v;
            size_t vl;
            db_expire_if_needed(d, key, klen, now_ms);
            if (!rh_get(&d->table, key, klen, &v, &vl)) {
                char msg[128];
                int n = snprintf(msg, sizeof(msg), "ASK %u %s:%u", slot,
                                 d->nodes[mt].ip,
                                 (unsigned)d->nodes[mt].port);
                resp_write_error(out, msg, (size_t)n);
                return 0;
            }
        }
        return 1;
    }
    if (asking && d->slot_importing[slot] == idx)
        return 1;
    {
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "MOVED %u %s:%u", slot,
                         d->nodes[idx].ip, (unsigned)d->nodes[idx].port);
        resp_write_error(out, msg, (size_t)n);
        return 0;
    }
}

static int cluster_keyless_id(uint16_t cmd_id)
{
    switch (cmd_id) {
    case CMD_PING:       case CMD_ECHO:       case CMD_CONFIG:
    case CMD_INFO:       case CMD_SAVE:       case CMD_LASTSAVE:
    case CMD_SHUTDOWN:   case CMD_SYNC:       case CMD_REPLICAOF:
    case CMD_SLAVEOF:    case CMD_PSYNC:      case CMD_AUTH:
    case CMD_SELECT:
    case CMD_SWAPDB:
    case CMD_SUBSCRIBE:  case CMD_UNSUBSCRIBE:case CMD_PUBLISH:
    case CMD_SSUBSCRIBE: case CMD_SUNSUBSCRIBE: case CMD_PUBSUB:
    case CMD_QUIT:       case CMD_MULTI:      case CMD_EXEC:
    case CMD_DISCARD:    case CMD_UNWATCH:    case CMD_DBSIZE:
    case CMD_FLUSHDB:    case CMD_CLUSTER:    case CMD_PERSIST:
    case CMD_MIGRATE:    case CMD_ASKING:    case CMD_SCRIPT:
    case CMD_KEYS:       case CMD_SCAN:       case CMD_RANDOMKEY:
    case CMD_FLUSHALL:   case CMD_TIME:       case CMD_READONLY:
    case CMD_READWRITE:  case CMD_ROLE:       case CMD_RESET:
    case CMD_HELLO:       case CMD_PFSELFTEST:
    case CMD_COMMAND:    case CMD_CLIENT:    case CMD_MEMORY:
    case CMD_SLOWLOG:    case CMD_BGSAVE:    case CMD_BGREWRITEAOF:
    case CMD_LOLWUT:     case CMD_WAIT:       case CMD_WAITAOF:
    case CMD_REPLCONF:   case CMD_FAILOVER:   case CMD_MONITOR:
    case CMD_ACL:        case CMD_DEBUG:      case CMD_LATENCY:
    case CMD_MODULE:     case CMD_SENTINEL:
        return 1;
    default:
        return 0;
    }
}

/* -MOVED/-CLUSTERDOWN/-ASK check for one command (cluster mode only):
 * extracts the command's key positions and verifies ownership of each.
 * Consumes the session's one-shot ASKING flag. Returns 1 to proceed,
 * 0 when a reply was already written. */
static int cluster_check_ownership(session *s, const resp_value *argv,
                                   size_t argc, resp_buf *out,
                                   uint64_t now_ms)
{
    db *d = s->d;
    const char *name;
    size_t nlen;
    size_t i;
    int asking;
    uint16_t cmd_id;
    if (!d->cluster_enabled)
        return 1;
    asking = s->asking;
    s->asking = 0;
    if (argc == 0 || !arg_str(&argv[0], &name, &nlen))
        return 1;
    if (argc < 2)
        return 1;
    cmd_id = cmd_resolve(name, nlen);
    if (cmd_id == CMD_MOVE) {
        static const char E[] = "ERR MOVE is not allowed in cluster mode";
        resp_write_error(out, E, sizeof(E) - 1);
        return 0;
    }
    if (cmd_id == CMD_RESTORE_ASKING)
        asking = 1; /* RESTORE-ASKING implies a one-shot ASKING */
    if (cluster_keyless_id(cmd_id))
        return 1;
    if (cmd_id == CMD_MGET || cmd_id == CMD_DEL ||
        cmd_id == CMD_UNLINK || cmd_id == CMD_EXISTS ||
        cmd_id == CMD_TOUCH ||
        cmd_id == CMD_SINTER || cmd_id == CMD_SUNION ||
        cmd_id == CMD_SDIFF || cmd_id == CMD_WATCH ||
        cmd_id == CMD_SINTERSTORE || cmd_id == CMD_SUNIONSTORE ||
        cmd_id == CMD_SDIFFSTORE || cmd_id == CMD_BITOP) {
        for (i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_MSET) {
        for (i = 1; i + 1 < argc; i += 2) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_SMOVE || cmd_id == CMD_RENAME ||
        cmd_id == CMD_RENAMENX || cmd_id == CMD_RPOPLPUSH ||
        cmd_id == CMD_LMOVE || cmd_id == CMD_LMOVEM ||
        cmd_id == CMD_COPY || cmd_id == CMD_LCS ||
        cmd_id == CMD_BRPOPLPUSH || cmd_id == CMD_BLMOVE ||
        cmd_id == CMD_BLMOVEM) {
        for (i = 1; i < argc && i < 3; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_BLPOP || cmd_id == CMD_BRPOP ||
        cmd_id == CMD_BZPOPMIN || cmd_id == CMD_BZPOPMAX) {
        for (i = 1; i + 1 < argc; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_BLMPOP || cmd_id == CMD_BZMPOP) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 4 || !arg_str(&argv[2], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 3 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 3; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_SINTERCARD) {
        /* keys are argv[2..2+numkeys); numkeys validated by the dispatch */
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 3 || !arg_str(&argv[1], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 2 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 2; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_PFCOUNT || cmd_id == CMD_PFMERGE) {
        for (i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_PFDEBUG) {
        const char *k;
        size_t kl;
        if (argc < 3 || !arg_str(&argv[2], &k, &kl))
            return 1;
        return db_key_served(d, k, kl, out, now_ms, asking);
    }
    if (cmd_id == CMD_GEOSEARCHSTORE) {
        for (i = 1; i < argc && i < 3; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_GEORADIUS || cmd_id == CMD_GEORADIUSBYMEMBER) {
        const char *k;
        const char *store;
        size_t kl;
        size_t sl;
        if (arg_str(&argv[1], &k, &kl) &&
            !db_key_served(d, k, kl, out, now_ms, asking))
            return 0;
        if (geo_radius_store_key(argv, argc,
                                 cmd_id == CMD_GEORADIUS ? 6u : 5u,
                                 &store, &sl) &&
            !db_key_served(d, store, sl, out, now_ms, asking))
            return 0;
        return 1;
    }
    if (cmd_id == CMD_EVAL || cmd_id == CMD_EVALSHA) {
        /* keys are argv[3..3+numkeys); numkeys validated by the dispatch */
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 4 || !arg_str(&argv[2], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 3 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 3; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_ZUNIONSTORE || cmd_id == CMD_ZINTERSTORE ||
        cmd_id == CMD_ZDIFFSTORE) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        const char *k;
        size_t kl;
        if (argc < 4 || !arg_str(&argv[1], &k, &kl) ||
            !arg_str(&argv[2], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        if (!db_key_served(d, k, kl, out, now_ms, asking))
            return 0;
        end = 3 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 3; i < end; i++) {
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_ZUNION || cmd_id == CMD_ZINTER ||
        cmd_id == CMD_ZDIFF || cmd_id == CMD_ZINTERCARD ||
        cmd_id == CMD_ZMPOP || cmd_id == CMD_LMPOP) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 3 || !arg_str(&argv[1], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            return 1;
        end = 2 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 2; i < end; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_ZRANGESTORE) {
        for (i = 1; i < argc && i < 3; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !db_key_served(d, k, kl, out, now_ms, asking))
                return 0;
        }
        return 1;
    }
    /* everything else: single key at argv[1] */
    {
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            return 1;
        return db_key_served(d, k, kl, out, now_ms, asking);
    }
}

static int is_write_command(const char *name, size_t nlen)
{
    return cmd_is_write(cmd_resolve(name, nlen));
}

/* Drop every key and expiry from one logical db without disturbing its
 * configuration (maxmemory, policy, cluster metadata, ...). Used by both
 * FLUSHDB and FLUSHALL. */
static void flush_db_contents(db *d)
{
    rh_each(&d->table, free_obj_cb, NULL);
    rh_destroy(&d->table);
    rh_destroy(&d->expires);
    rh_destroy(&d->keyvers);
    rh_init(&d->table);
    rh_init(&d->expires);
    rh_init(&d->keyvers);
    d->used_memory = 0;
    memset(d->cmd_calls, 0, sizeof(d->cmd_calls));
    memset(d->cmd_usecs, 0, sizeof(d->cmd_usecs));
    d->flush_epoch++; /* invalidates all WATCHes */
    d->dirty++;
}

static void hello_reply(resp_buf *out, int proto)
{
    if (proto == 3)
        resp_write_map_header(out, 7);
    else
        resp_write_array_header(out, 14);
    resp_write_bulk(out, "server", 6);
    resp_write_bulk(out, "redis", 5);
    resp_write_bulk(out, "version", 7);
    resp_write_bulk(out, "7.2.15", 6);
    resp_write_bulk(out, "proto", 5);
    resp_write_integer(out, proto);
    resp_write_bulk(out, "id", 2);
    resp_write_integer(out, 0);
    resp_write_bulk(out, "mode", 4);
    resp_write_bulk(out, "standalone", 10);
    resp_write_bulk(out, "role", 4);
    resp_write_bulk(out, "master", 6);
    resp_write_bulk(out, "modules", 7);
    resp_write_array_header(out, 0);
}

static size_t lcs_length_linear(const char *a, size_t alen, const char *b,
                                size_t blen)
{
    uint32_t *prev;
    uint32_t *curr;
    size_t i, j;
    uint32_t answer = 0;

    if (alen == 0 || blen == 0)
        return 0;
    if (alen > blen) {
        const char *tmp = a;
        size_t tl = alen;
        a = b;
        b = tmp;
        alen = blen;
        blen = tl;
    }
    prev = (uint32_t *)malloc((blen + 1) * sizeof(*prev));
    curr = (uint32_t *)malloc((blen + 1) * sizeof(*curr));
    if (prev == NULL || curr == NULL) {
        free(prev);
        free(curr);
        return SIZE_MAX;
    }
    memset(prev, 0, (blen + 1) * sizeof(*prev));
    for (i = 1; i <= alen; i++) {
        curr[0] = 0;
        for (j = 1; j <= blen; j++) {
            if (a[i - 1] == b[j - 1])
                curr[j] = prev[j - 1] + 1;
            else
                curr[j] = prev[j] > curr[j - 1] ? prev[j] : curr[j - 1];
        }
        {
            uint32_t *swap = prev;
            prev = curr;
            curr = swap;
        }
    }
    answer = prev[blen];
    free(prev);
    free(curr);
    return answer;
}

typedef struct lcs_match_range {
    uint32_t a_start;
    uint32_t a_end;
    uint32_t b_start;
    uint32_t b_end;
    uint32_t len;
} lcs_match_range;

typedef struct sort_elem {
    char *val;      /* NUL-terminated copy of the element */
    size_t vlen;
    double score;
    char *cmp;      /* ALPHA BY lookup copy; NULL when missing/not used */
    size_t cmplen;
    int has_cmp;
} sort_elem;

typedef struct sort_vec {
    sort_elem *v;
    size_t n;
    size_t cap;
    int oom;
} sort_vec;

typedef struct sort_get {
    char *pattern;
    size_t plen;
} sort_get;

static int sort_vec_add(sort_vec *vec, const char *s, size_t len)
{
    sort_elem *e;
    char *copy;
    if (vec->oom)
        return -1;
    if (vec->n == vec->cap) {
        size_t ncap = vec->cap == 0 ? 16 : vec->cap * 2;
        sort_elem *nv =
            (sort_elem *)realloc(vec->v, ncap * sizeof(*nv));
        if (nv == NULL) {
            vec->oom = 1;
            return -1;
        }
        vec->v = nv;
        vec->cap = ncap;
    }
    if (len == SIZE_MAX) {
        vec->oom = 1;
        return -1;
    }
    copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        vec->oom = 1;
        return -1;
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
    e = &vec->v[vec->n++];
    e->val = copy;
    e->vlen = len;
    e->score = 0.0;
    e->cmp = NULL;
    e->cmplen = 0;
    e->has_cmp = 0;
    return 0;
}

static void sort_vec_free(sort_vec *vec)
{
    size_t i;
    for (i = 0; i < vec->n; i++) {
        free(vec->v[i].val);
        free(vec->v[i].cmp);
    }
    free(vec->v);
    memset(vec, 0, sizeof(*vec));
}

static void sort_set_collect_cb(const char *member, size_t mlen,
                                const char *value, size_t vlen, void *ctx)
{
    sort_vec *vec = (sort_vec *)ctx;
    (void)value;
    (void)vlen;
    if (sort_vec_add(vec, member, mlen) != 0)
        vec->oom = 1;
}

static int sort_vec_add_list(sort_vec *vec, obj_list *l)
{
    ql_iter it;
    size_t len;
    if (obj_list_first(l, &it)) {
        do {
            const char *v = obj_list_iter_value(&it, &len);
            if (sort_vec_add(vec, v, len) != 0)
                return -1;
        } while (obj_list_iter_next(&it));
    }
    return vec->oom ? -1 : 0;
}

static int sort_vec_add_zset(sort_vec *vec, obj_zset *z)
{
    obj_zset_iter it;
    size_t len;
    if (obj_zset_first(z, &it)) {
        do {
            const char *m = obj_zset_iter_member(&it, &len);
            if (sort_vec_add(vec, m, len) != 0)
                return -1;
        } while (obj_zset_iter_next(&it));
    }
    return vec->oom ? -1 : 0;
}

static char *sort_pattern_key(const char *pat, size_t plen, const char *elem,
                              size_t elen, size_t *outlen)
{
    const char *star;
    size_t pre, post, total;
    char *key;
    star = (const char *)memchr(pat, '*', plen);
    if (star == NULL)
        return NULL;
    pre = (size_t)(star - pat);
    post = plen - pre - 1;
    if (pre > SIZE_MAX - elen || pre + elen > SIZE_MAX - post) {
        *outlen = 0;
        return NULL;
    }
    total = pre + elen + post;
    key = (char *)malloc(total + 1);
    if (key == NULL) {
        *outlen = 0;
        return NULL;
    }
    if (pre > 0)
        memcpy(key, pat, pre);
    if (elen > 0)
        memcpy(key + pre, elem, elen);
    if (post > 0)
        memcpy(key + pre + elen, star + 1, post);
    key[total] = '\0';
    *outlen = total;
    return key;
}

/* Look up a SORT BY/GET pattern. Returns 1 on a string hit, 0 on miss,
 * -1 when the key exists with a non-string type (reply already written). */
static int sort_pattern_lookup(db *d, resp_buf *out, const char *pat,
                               size_t plen, const char *elem, size_t elen,
                               uint64_t now_ms, const char **s, size_t *sl)
{
    char *key;
    size_t klen;
    const char *v;
    size_t vl;
    int rc;
    key = sort_pattern_key(pat, plen, elem, elen, &klen);
    if (key == NULL)
        return 0;
    rc = db_get(d, key, klen, &v, &vl, now_ms);
    if (rc) {
        if (obj_tag_of(v, vl) != DDUP_OBJ_STRING) {
            wrongtype(out);
            free(key);
            return -1;
        }
        obj_str(v, vl, s, sl);
    }
    free(key);
    return rc;
}

static int sort_cmp_bytes(const char *a, size_t alen, const char *b,
                          size_t blen)
{
    size_t n = alen < blen ? alen : blen;
    int c = n > 0 ? memcmp(a, b, n) : 0;
    if (c != 0)
        return c;
    return (alen > blen) - (alen < blen);
}

static int sort_elem_compare(const sort_elem *a, const sort_elem *b, int alpha,
                             int bypattern, int desc)
{
    int cmp;
    if (alpha) {
        if (bypattern) {
            if (a->has_cmp && b->has_cmp) {
                cmp = sort_cmp_bytes(a->cmp, a->cmplen, b->cmp, b->cmplen);
            } else if (a->has_cmp == b->has_cmp) {
                cmp = 0;
            } else {
                cmp = a->has_cmp ? 1 : -1;
            }
        } else {
            cmp = sort_cmp_bytes(a->val, a->vlen, b->val, b->vlen);
        }
    } else {
        if (a->score < b->score)
            cmp = -1;
        else if (a->score > b->score)
            cmp = 1;
        else
            cmp = sort_cmp_bytes(a->val, a->vlen, b->val, b->vlen);
    }
    return desc ? -cmp : cmp;
}

static void sort_merge(sort_elem *dst, sort_elem *src, size_t lo, size_t mid,
                       size_t hi, int alpha, int bypattern, int desc)
{
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (sort_elem_compare(&src[i], &src[j], alpha, bypattern, desc) <= 0)
            dst[k++] = src[i++];
        else
            dst[k++] = src[j++];
    }
    while (i < mid)
        dst[k++] = src[i++];
    while (j < hi)
        dst[k++] = src[j++];
}

static void sort_merge_sort(sort_elem *v, sort_elem *tmp, size_t n, int alpha,
                            int bypattern, int desc)
{
    size_t width;
    sort_elem *src = v;
    sort_elem *dst = tmp;
    if (n < 2)
        return;
    for (width = 1; width < n; width *= 2) {
        size_t lo;
        for (lo = 0; lo < n; lo += width * 2) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + width * 2 < n ? lo + width * 2 : n;
            sort_merge(dst, src, lo, mid, hi, alpha, bypattern, desc);
        }
        {
            sort_elem *swap = src;
            src = dst;
            dst = swap;
        }
    }
    if (src != v) {
        size_t i;
        for (i = 0; i < n; i++)
            v[i] = src[i];
    }
}


/* ------------------------------------------------------------------ */
/* Geospatial commands (zset-backed Redis-compatible WGS84 encoding)  */
/* ------------------------------------------------------------------ */

#define GEO_LAT_MIN -85.05112878
#define GEO_LAT_MAX 85.05112878
#define GEO_LON_MIN -180.0
#define GEO_LON_MAX 180.0
#define GEO_EARTH_RADIUS_M 6372797.560856
#define GEO_STEP_MAX 26u
#define GEO_SCALE (1ULL << GEO_STEP_MAX)
#define GEO_BITS_MASK ((1ULL << 52) - 1ULL)
#define GEO_PI 3.14159265358979323846

static const char GEO_BASE32[] = "0123456789bcdefghjkmnpqrstuvwxyz";

typedef struct geo_hit {
    char *member;
    size_t mlen;
    double score;
    double dist;
} geo_hit;

typedef struct geo_vec {
    geo_hit *v;
    size_t n;
    size_t cap;
    int oom;
} geo_vec;

typedef struct geo_opts {
    int withcoord;
    int withdist;
    int withhash;
    int asc;
    int desc;
    long long count;
    int has_count;
    int any;
    const char *store;
    size_t storelen;
    int storedist;
} geo_opts;

static uint64_t geo_interleave(uint32_t xlo, uint32_t ylo)
{
    uint64_t x = xlo;
    uint64_t y = ylo;
    uint64_t r = 0;
    unsigned i;

    for (i = 0; i < 32; i++) {
        r |= ((x >> i) & 1ULL) << (2u * i);
        r |= ((y >> i) & 1ULL) << (2u * i + 1u);
    }
    return r;
}

static void geo_deinterleave(uint64_t bits, uint32_t *x, uint32_t *y)
{
    uint32_t xo = 0;
    uint32_t yo = 0;
    unsigned i;

    for (i = 0; i < 32; i++) {
        xo |= (uint32_t)((bits >> (2u * i)) & 1ULL) << i;
        yo |= (uint32_t)((bits >> (2u * i + 1u)) & 1ULL) << i;
    }
    *x = xo;
    *y = yo;
}

static uint64_t geo_bits(double lon, double lat)
{
    double latn = (lat - GEO_LAT_MIN) / (GEO_LAT_MAX - GEO_LAT_MIN);
    double lonn = (lon - GEO_LON_MIN) / (GEO_LON_MAX - GEO_LON_MIN);
    uint32_t ilat = latn >= 1.0 ? (uint32_t)(GEO_SCALE - 1u)
                                : (uint32_t)(latn * (double)GEO_SCALE);
    uint32_t ilon = lonn >= 1.0 ? (uint32_t)(GEO_SCALE - 1u)
                                : (uint32_t)(lonn * (double)GEO_SCALE);

    return geo_interleave(ilat, ilon) & GEO_BITS_MASK;
}

static void geo_decode_bits(uint64_t bits, double *lon, double *lat)
{
    uint32_t xlo = 0;
    uint32_t ylo = 0;
    double lat_scale = GEO_LAT_MAX - GEO_LAT_MIN;
    double lon_scale = GEO_LON_MAX - GEO_LON_MIN;
    double lat_min;
    double lat_max;
    double lon_min;
    double lon_max;

    geo_deinterleave(bits, &xlo, &ylo);
    lat_min = GEO_LAT_MIN +
              ((double)xlo / (double)GEO_SCALE) * lat_scale;
    lat_max = GEO_LAT_MIN +
              ((double)(xlo + 1u) / (double)GEO_SCALE) * lat_scale;
    lon_min = GEO_LON_MIN +
              ((double)ylo / (double)GEO_SCALE) * lon_scale;
    lon_max = GEO_LON_MIN +
              ((double)(ylo + 1u) / (double)GEO_SCALE) * lon_scale;
    *lat = (lat_min + lat_max) / 2.0;
    *lon = (lon_min + lon_max) / 2.0;
    if (*lat > GEO_LAT_MAX)
        *lat = GEO_LAT_MAX;
    if (*lat < GEO_LAT_MIN)
        *lat = GEO_LAT_MIN;
    if (*lon > GEO_LON_MAX)
        *lon = GEO_LON_MAX;
    if (*lon < GEO_LON_MIN)
        *lon = GEO_LON_MIN;
}

static void geo_hash_string(double lon, double lat, char out[12])
{
    double lat_min = -90.0;
    double lat_max = 90.0;
    double lon_min = -180.0;
    double lon_max = 180.0;
    int even = 1;
    int i;

    for (i = 0; i < 11; i++) {
        int j;
        int val = 0;
        for (j = 0; j < 5; j++) {
            val <<= 1;
            if (even) {
                double mid = (lon_min + lon_max) / 2.0;
                if (lon >= mid) {
                    val |= 1;
                    lon_min = mid;
                } else {
                    lon_max = mid;
                }
            } else {
                double mid = (lat_min + lat_max) / 2.0;
                if (lat >= mid) {
                    val |= 1;
                    lat_min = mid;
                } else {
                    lat_max = mid;
                }
            }
            even = !even;
        }
        out[i] = GEO_BASE32[val];
    }
    out[10] = GEO_BASE32[0];
    out[11] = '\0';
}

static double geo_distance_m(double lon1, double lat1, double lon2,
                             double lat2)
{
    double lat1r = lat1 * GEO_PI / 180.0;
    double lat2r = lat2 * GEO_PI / 180.0;
    double dlat = (lat2 - lat1) * GEO_PI / 180.0;
    double dlon = (lon2 - lon1) * GEO_PI / 180.0;
    double u = sin(dlat / 2.0);
    double v = sin(dlon / 2.0);
    double a = u * u + cos(lat1r) * cos(lat2r) * v * v;

    return 2.0 * GEO_EARTH_RADIUS_M * asin(sqrt(a));
}

static int geo_unit(const char *s, size_t len, double *meters)
{
    if (ci_equal(s, len, "m")) {
        *meters = 1.0;
        return 1;
    }
    if (ci_equal(s, len, "km")) {
        *meters = 1000.0;
        return 1;
    }
    if (ci_equal(s, len, "mi")) {
        *meters = 1609.34;
        return 1;
    }
    if (ci_equal(s, len, "ft")) {
        *meters = 0.3048;
        return 1;
    }
    return 0;
}

static void geo_write_distance(resp_buf *out, double meters, double unit)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.4f", meters / unit);

    resp_write_bulk(out, buf, (size_t)n);
}

static void geo_write_coord(resp_buf *out, double v)
{
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.17Lf", (long double)v);
    char *p;

    if (n <= 0)
        return;
    if (strchr(buf, '.') != NULL) {
        p = buf + n - 1;
        while (n > 1 && *p == '0') {
            p--;
            n--;
        }
        if (n > 0 && *p == '.')
            n--;
    }
    if (n == 2 && buf[0] == '-' && buf[1] == '0') {
        buf[0] = '0';
        n = 1;
    }
    resp_write_bulk(out, buf, (size_t)n);
}

static int geo_member_coords(obj_zset *z, const char *member, size_t mlen,
                             double *lon, double *lat)
{
    double score;

    if (!obj_zset_score(z, member, mlen, &score))
        return 0;
    geo_decode_bits((uint64_t)score, lon, lat);
    return 1;
}

static int geo_vec_add(geo_vec *vec, const char *member, size_t mlen,
                       double score, double dist)
{
    geo_hit *h;
    char *copy;

    if (vec->oom)
        return -1;
    if (vec->n == vec->cap) {
        size_t ncap = vec->cap == 0 ? 16 : vec->cap * 2;
        geo_hit *nv = (geo_hit *)realloc(vec->v, ncap * sizeof(*nv));

        if (nv == NULL) {
            vec->oom = 1;
            return -1;
        }
        vec->v = nv;
        vec->cap = ncap;
    }
    if (mlen == SIZE_MAX) {
        vec->oom = 1;
        return -1;
    }
    copy = (char *)malloc(mlen + 1);
    if (copy == NULL) {
        vec->oom = 1;
        return -1;
    }
    memcpy(copy, member, mlen);
    copy[mlen] = '\0';
    h = &vec->v[vec->n++];
    h->member = copy;
    h->mlen = mlen;
    h->score = score;
    h->dist = dist;
    return 0;
}

static void geo_vec_free(geo_vec *vec)
{
    size_t i;

    for (i = 0; i < vec->n; i++)
        free(vec->v[i].member);
    free(vec->v);
    memset(vec, 0, sizeof(*vec));
}

static int geo_hit_compare(const geo_hit *a, const geo_hit *b, int desc)
{
    int cmp;

    if (a->dist < b->dist)
        cmp = -1;
    else if (a->dist > b->dist)
        cmp = 1;
    else {
        size_t n = a->mlen < b->mlen ? a->mlen : b->mlen;
        cmp = n > 0 ? memcmp(a->member, b->member, n) : 0;
        if (cmp == 0)
            cmp = (a->mlen > b->mlen) - (a->mlen < b->mlen);
    }
    return desc ? -cmp : cmp;
}

static void geo_vec_sort(geo_vec *vec, int desc)
{
    geo_hit *tmp;
    size_t n = vec->n;
    size_t width;
    geo_hit *src = vec->v;
    geo_hit *dst;

    if (n < 2)
        return;
    tmp = (geo_hit *)malloc(n * sizeof(*tmp));
    if (tmp == NULL) {
        vec->oom = 1;
        return;
    }
    dst = tmp;
    for (width = 1; width < n; width *= 2) {
        size_t lo;
        for (lo = 0; lo < n; lo += width * 2) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + width * 2 < n ? lo + width * 2 : n;
            size_t i = lo;
            size_t j = mid;
            size_t k = lo;
            while (i < mid && j < hi) {
                if (geo_hit_compare(&src[i], &src[j], desc) <= 0)
                    dst[k++] = src[i++];
                else
                    dst[k++] = src[j++];
            }
            while (i < mid)
                dst[k++] = src[i++];
            while (j < hi)
                dst[k++] = src[j++];
        }
        {
            geo_hit *swap = src;
            src = dst;
            dst = swap;
        }
    }
    if (src != vec->v) {
        size_t i;
        for (i = 0; i < n; i++)
            vec->v[i] = src[i];
    }
    free(tmp);
}

static int geo_collect_radius(obj_zset *z, double center_lon,
                              double center_lat, double radius_m,
                              geo_vec *vec)
{
    obj_zset_iter it;

    if (!obj_zset_first(z, &it))
        return 0;
    do {
        size_t mlen = 0;
        const char *member = obj_zset_iter_member(&it, &mlen);
        double score = obj_zset_iter_score(&it);
        double lon;
        double lat;
        double dist;

        geo_decode_bits((uint64_t)score, &lon, &lat);
        dist = geo_distance_m(center_lon, center_lat, lon, lat);
        if (dist <= radius_m && geo_vec_add(vec, member, mlen, score, dist) != 0)
            return -1;
    } while (obj_zset_iter_next(&it));
    return vec->oom ? -1 : 0;
}

static int geo_collect_box(obj_zset *z, double center_lon, double center_lat,
                           double width_m, double height_m, geo_vec *vec)
{
    double half_w = width_m / 2.0;
    double half_h = height_m / 2.0;
    obj_zset_iter it;

    if (!obj_zset_first(z, &it))
        return 0;
    do {
        size_t mlen = 0;
        const char *member = obj_zset_iter_member(&it, &mlen);
        double score = obj_zset_iter_score(&it);
        double lon;
        double lat;

        geo_decode_bits((uint64_t)score, &lon, &lat);
        {
            double lat_dist = GEO_EARTH_RADIUS_M *
                              fabs((lat - center_lat) * GEO_PI / 180.0);
            double lon_dist = geo_distance_m(center_lon, lat, lon, lat);
            double dist;

            if (lat_dist > half_h || lon_dist > half_w)
                continue;
            dist = geo_distance_m(center_lon, center_lat, lon, lat);
            if (geo_vec_add(vec, member, mlen, score, dist) != 0)
                return -1;
        }
    } while (obj_zset_iter_next(&it));
    return vec->oom ? -1 : 0;
}

static int geo_parse_opts(const resp_value *argv, size_t argc, size_t start,
                          int store_kind, geo_opts *opts, resp_buf *out)
{
    size_t i;

    memset(opts, 0, sizeof(*opts));
    for (i = start; i < argc; i++) {
        const char *tok;
        size_t tokl;

        if (!arg_str(&argv[i], &tok, &tokl))
            goto bad_type;
        if (ci_equal(tok, tokl, "WITHCOORD") && !opts->withcoord) {
            opts->withcoord = 1;
        } else if (ci_equal(tok, tokl, "WITHDIST") && !opts->withdist) {
            opts->withdist = 1;
        } else if (ci_equal(tok, tokl, "WITHHASH") && !opts->withhash) {
            opts->withhash = 1;
        } else if (ci_equal(tok, tokl, "ASC")) {
            opts->asc = 1;
            opts->desc = 0;
        } else if (ci_equal(tok, tokl, "DESC")) {
            opts->desc = 1;
            opts->asc = 0;
        } else if (ci_equal(tok, tokl, "ANY")) {
            opts->any = 1;
        } else if (ci_equal(tok, tokl, "COUNT") && i + 1 < argc) {
            const char *cv;
            size_t cvl;
            long long count;

            if (!arg_str(&argv[i + 1], &cv, &cvl) ||
                !parse_i64(cv, cvl, &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return -1;
            }
            if (count <= 0) {
                static const char E[] = "ERR COUNT must be > 0";
                resp_write_error(out, E, sizeof(E) - 1);
                return -1;
            }
            opts->has_count = 1;
            opts->count = count;
            i++;
        } else if (store_kind == 1 && ci_equal(tok, tokl, "STORE") &&
                   i + 1 < argc) {
            if (!arg_str(&argv[i + 1], &opts->store, &opts->storelen))
                goto bad_type;
            opts->storedist = 0;
            i++;
        } else if (store_kind == 1 && ci_equal(tok, tokl, "STOREDIST") &&
                   i + 1 < argc) {
            if (!arg_str(&argv[i + 1], &opts->store, &opts->storelen))
                goto bad_type;
            opts->storedist = 1;
            i++;
        } else if (store_kind == 2 && ci_equal(tok, tokl, "STOREDIST")) {
            opts->storedist = 1;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return -1;
        }
    }
    if (opts->any && !opts->has_count) {
        static const char E[] = "ERR the ANY argument requires COUNT argument";
        resp_write_error(out, E, sizeof(E) - 1);
        return -1;
    }
    if ((opts->store != NULL || store_kind == 2) &&
        (opts->withcoord || opts->withdist || opts->withhash)) {
        static const char E_GEORADIUS[] =
            "ERR STORE option in GEORADIUS is not compatible with "
            "WITHDIST, WITHHASH and WITHCOORD options";
        static const char E_GEOSEARCHSTORE[] =
            "ERR GEOSEARCHSTORE is not compatible with "
            "WITHDIST, WITHHASH and WITHCOORD options";
        const char *E = store_kind == 2 ? E_GEOSEARCHSTORE : E_GEORADIUS;
        resp_write_error(out, E, sizeof(E) - 1);
        return -1;
    }
    return 0;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
    return -1;
}

static int geo_parse_radius(const char *s, size_t len, double unit,
                            double *radius_m)
{
    double v;

    if (!parse_double(s, len, &v) || v < 0.0)
        return 0;
    *radius_m = v * unit;
    return 1;
}

static int geo_parse_coords(const char *lons, size_t lonl, const char *lats,
                            size_t latl, double *lon, double *lat)
{
    *lon = 0.0;
    *lat = 0.0;
    if (!parse_double(lons, lonl, lon))
        return -1;
    if (!parse_double(lats, latl, lat))
        return -1;
    if (*lon < GEO_LON_MIN || *lon > GEO_LON_MAX ||
        *lat < GEO_LAT_MIN || *lat > GEO_LAT_MAX)
        return 0;
    return 1;
}

static void geo_write_coords_error(resp_buf *out, int rc, double lon,
                                   double lat)
{
    if (rc < 0) {
        resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
        return;
    }
    {
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "ERR invalid longitude,latitude pair %.6f,%.6f",
                         lon, lat);
        if (n < 0)
            return;
        resp_write_error(out, buf, (size_t)n);
    }
}

static void geo_emit_member(resp_buf *out, const geo_hit *hit,
                            const geo_opts *opts, double unit)
{
    size_t nfields = 1 + (opts->withdist ? 1u : 0u) +
                     (opts->withhash ? 1u : 0u) +
                     (opts->withcoord ? 1u : 0u);

    if (nfields == 1) {
        resp_write_bulk(out, hit->member, hit->mlen);
        return;
    }
    resp_write_array_header(out, nfields);
    resp_write_bulk(out, hit->member, hit->mlen);
    if (opts->withdist)
        geo_write_distance(out, hit->dist, unit);
    if (opts->withhash)
        resp_write_integer(out, (long long)(uint64_t)hit->score);
    if (opts->withcoord) {
        double lon;
        double lat;
        geo_decode_bits((uint64_t)hit->score, &lon, &lat);
        resp_write_array_header(out, 2);
        geo_write_coord(out, lon);
        geo_write_coord(out, lat);
    }
}

static void geo_emit_results(resp_buf *out, const geo_vec *vec,
                             const geo_opts *opts, double unit)
{
    size_t i;

    resp_write_array_header(out, vec->n);
    for (i = 0; i < vec->n; i++)
        geo_emit_member(out, &vec->v[i], opts, unit);
}

static int geo_store_results(db *d, resp_buf *out, const char *dst,
                             size_t dstl, geo_vec *vec, int storedist,
                             double unit, uint64_t now_ms)
{
    obj_zset *z;
    int rc;
    size_t i;

    if (vec->n == 0) {
        db_del_kv(d, dst, dstl);
        resp_write_integer(out, 0);
        return 0;
    }
    db_del_kv(d, dst, dstl);
    rc = get_zset(d, out, dst, dstl, 1, now_ms, &z);
    if (rc <= 0)
        return rc == 0 ? 0 : -1;
    for (i = 0; i < vec->n; i++) {
        double score = storedist ? vec->v[i].dist / unit : vec->v[i].score;
        if (obj_zset_add(z, vec->v[i].member, vec->v[i].mlen, score) < 0) {
            storage_length_error(out);
            return -1;
        }
    }
    resp_write_integer(out, (long long)vec->n);
    return 0;
}

static int geo_execute_radius(db *d, resp_buf *out, obj_zset *z,
                              double center_lon, double center_lat,
                              double radius_m, double unit,
                              const geo_opts *opts, const char *dst,
                              size_t dstl, uint64_t now_ms)
{
    geo_vec vec;
    int rc;

    memset(&vec, 0, sizeof(vec));
    if (opts->any && opts->has_count) {
        obj_zset_iter it;
        if (obj_zset_first(z, &it)) {
            do {
                size_t mlen = 0;
                const char *member = obj_zset_iter_member(&it, &mlen);
                double score = obj_zset_iter_score(&it);
                double lon;
                double lat;
                double dist;

                geo_decode_bits((uint64_t)score, &lon, &lat);
                dist = geo_distance_m(center_lon, center_lat, lon, lat);
                if (dist <= radius_m) {
                    if (geo_vec_add(&vec, member, mlen, score, dist) != 0)
                        break;
                    if ((long long)vec.n == opts->count)
                        break;
                }
            } while (obj_zset_iter_next(&it));
        }
    } else {
        rc = geo_collect_radius(z, center_lon, center_lat, radius_m, &vec);
        if (rc != 0) {
            geo_vec_free(&vec);
            oom_blocked(d, out);
            return -1;
        }
        if (opts->has_count || opts->asc || opts->desc)
            geo_vec_sort(&vec, opts->desc);
        if (vec.oom) {
            geo_vec_free(&vec);
            oom_blocked(d, out);
            return -1;
        }
        if (opts->has_count && !opts->any && (size_t)opts->count < vec.n) {
            size_t i;
            for (i = (size_t)opts->count; i < vec.n; i++)
                free(vec.v[i].member);
            vec.n = (size_t)opts->count;
        }
    }

    if (dst != NULL) {
        rc = geo_store_results(d, out, dst, dstl, &vec, opts->storedist,
                               unit, now_ms);
        geo_vec_free(&vec);
        return rc;
    }
    geo_emit_results(out, &vec, opts, unit);
    geo_vec_free(&vec);
    return 0;
}

static void cmd_geoadd(db *d, const resp_value *argv, size_t argc,
                       resp_buf *out, uint64_t now_ms)
{
    const char *key;
    size_t keyl;
    size_t i = 2;
    int nx = 0;
    int xx = 0;
    int ch = 0;
    obj_zset *z;
    int rc;
    long long added = 0;

    if (argc < 5) {
        wrong_args(out, "geoadd");
        return;
    }
    if (!arg_str(&argv[1], &key, &keyl))
        goto bad_type;
    while (i < argc) {
        const char *tok;
        size_t tokl;
        if (!arg_str(&argv[i], &tok, &tokl))
            goto bad_type;
        if (ci_equal(tok, tokl, "NX")) {
            nx = 1;
            i++;
        } else if (ci_equal(tok, tokl, "XX")) {
            xx = 1;
            i++;
        } else if (ci_equal(tok, tokl, "CH")) {
            ch = 1;
            i++;
        } else {
            break;
        }
    }
    if ((argc - i) % 3 != 0 || (nx && xx)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }

    {
        size_t first = i;
        size_t j;

        for (j = i; j < argc; j += 3) {
            const char *lons;
            const char *lats;
            const char *member;
            size_t lonl;
            size_t latl;
            size_t mlen;
            double lon;
            double lat;

            if (!arg_str(&argv[j], &lons, &lonl) ||
                !arg_str(&argv[j + 1], &lats, &latl) ||
                !arg_str(&argv[j + 2], &member, &mlen))
                goto bad_type;
            {
                int crc = geo_parse_coords(lons, lonl, lats, latl, &lon, &lat);
                if (crc <= 0) {
                    geo_write_coords_error(out, crc, lon, lat);
                    return;
                }
            }
        }
        i = first;
    }

    rc = get_zset(d, out, key, keyl, 1, now_ms, &z);
    if (rc <= 0)
        return;

    for (; i < argc; i += 3) {
        const char *lons;
        const char *lats;
        const char *member;
        size_t lonl;
        size_t latl;
        size_t mlen;
        double lon;
        double lat;
        double old;
        int present;

        if (!arg_str(&argv[i], &lons, &lonl) ||
            !arg_str(&argv[i + 1], &lats, &latl) ||
            !arg_str(&argv[i + 2], &member, &mlen))
            goto bad_type;
        {
            int crc = geo_parse_coords(lons, lonl, lats, latl, &lon, &lat);
            if (crc <= 0) {
                geo_write_coords_error(out, crc, lon, lat);
                return;
            }
        }
        present = obj_zset_score(z, member, mlen, &old);
        if ((nx && present) || (xx && !present))
            continue;
        if (obj_zset_add(z, member, mlen,
                         (double)geo_bits(lon, lat)) < 0) {
            storage_length_error(out);
            return;
        }
        if (!present || ch || old != (double)geo_bits(lon, lat))
            added++;
    }
    resp_write_integer(out, added);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_geopos(db *d, const resp_value *argv, size_t argc,
                       resp_buf *out, uint64_t now_ms)
{
    const char *key;
    size_t keyl;
    obj_zset *z;
    int rc;
    size_t i;

    if (argc < 2) {
        wrong_args(out, "geopos");
        return;
    }
    if (!arg_str(&argv[1], &key, &keyl))
        goto bad_type;
    rc = get_zset(d, out, key, keyl, 0, now_ms, &z);
    if (rc < 0)
        return;

    resp_write_array_header(out, argc - 2);
    for (i = 2; i < argc; i++) {
        const char *member;
        size_t mlen;
        double lon;
        double lat;

        if (!arg_str(&argv[i], &member, &mlen))
            goto bad_type;
        if (rc == 0 || !geo_member_coords(z, member, mlen, &lon, &lat)) {
            write_null_array(out);
            continue;
        }
        resp_write_array_header(out, 2);
        geo_write_coord(out, lon);
        geo_write_coord(out, lat);
    }
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_geohash(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    const char *key;
    size_t keyl;
    obj_zset *z;
    int rc;
    size_t i;

    if (argc < 2) {
        wrong_args(out, "geohash");
        return;
    }
    if (!arg_str(&argv[1], &key, &keyl))
        goto bad_type;
    rc = get_zset(d, out, key, keyl, 0, now_ms, &z);
    if (rc < 0)
        return;

    resp_write_array_header(out, argc - 2);
    for (i = 2; i < argc; i++) {
        const char *member;
        size_t mlen;
        double lon;
        double lat;

        if (!arg_str(&argv[i], &member, &mlen))
            goto bad_type;
        if (rc == 0 || !geo_member_coords(z, member, mlen, &lon, &lat)) {
            resp_write_bulk(out, NULL, 0);
            continue;
        }
        {
            char hash[12];
            geo_hash_string(lon, lat, hash);
            resp_write_bulk(out, hash, 11);
        }
    }
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_geodist(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    const char *key;
    const char *m1;
    const char *m2;
    size_t keyl;
    size_t m1l;
    size_t m2l;
    double unit = 1.0;
    obj_zset *z;
    int rc;
    double lon1;
    double lat1;
    double lon2;
    double lat2;

    if (argc < 4 || argc > 5) {
        wrong_args(out, "geodist");
        return;
    }
    if (!arg_str(&argv[1], &key, &keyl) ||
        !arg_str(&argv[2], &m1, &m1l) ||
        !arg_str(&argv[3], &m2, &m2l))
        goto bad_type;
    if (argc == 5) {
        const char *u;
        size_t ul;
        if (!arg_str(&argv[4], &u, &ul) || !geo_unit(u, ul, &unit)) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
    }
    rc = get_zset(d, out, key, keyl, 0, now_ms, &z);
    if (rc <= 0) {
        if (rc == 0)
            resp_write_bulk(out, NULL, 0);
        return;
    }
    if (!geo_member_coords(z, m1, m1l, &lon1, &lat1) ||
        !geo_member_coords(z, m2, m2l, &lon2, &lat2)) {
        resp_write_bulk(out, NULL, 0);
        return;
    }
    geo_write_distance(out, geo_distance_m(lon1, lat1, lon2, lat2), unit);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}


static int geo_execute_box(db *d, resp_buf *out, obj_zset *z,
                           double center_lon, double center_lat,
                           double width_m, double height_m, double unit,
                           const geo_opts *opts, const char *dst,
                           size_t dstl, uint64_t now_ms)
{
    geo_vec vec;
    int rc;

    memset(&vec, 0, sizeof(vec));
    rc = geo_collect_box(z, center_lon, center_lat, width_m, height_m, &vec);
    if (rc != 0) {
        geo_vec_free(&vec);
        oom_blocked(d, out);
        return -1;
    }
    if (opts->has_count || opts->asc || opts->desc)
        geo_vec_sort(&vec, opts->desc);
    if (vec.oom) {
        geo_vec_free(&vec);
        oom_blocked(d, out);
        return -1;
    }
    if (opts->has_count && !opts->any && (size_t)opts->count < vec.n) {
        size_t i;
        for (i = (size_t)opts->count; i < vec.n; i++)
            free(vec.v[i].member);
        vec.n = (size_t)opts->count;
    }

    if (dst != NULL) {
        rc = geo_store_results(d, out, dst, dstl, &vec, opts->storedist,
                               unit, now_ms);
        geo_vec_free(&vec);
        return rc;
    }
    geo_emit_results(out, &vec, opts, unit);
    geo_vec_free(&vec);
    return 0;
}

static void geo_radius_command(db *d, const resp_value *argv, size_t argc,
                               resp_buf *out, uint64_t now_ms,
                               int from_member, int allow_store,
                               const char *cmdname)
{
    const char *key;
    size_t keyl;
    size_t opt_start;
    double unit = 1.0;
    double radius_m = 0.0;
    double center_lon = 0.0;
    double center_lat = 0.0;
    obj_zset *z;
    int rc;
    geo_opts opts;

    if (argc < (from_member ? 5u : 6u)) {
        wrong_args(out, cmdname);
        return;
    }
    if (!arg_str(&argv[1], &key, &keyl))
        goto bad_type;

    if (from_member) {
        const char *member;
        const char *rs;
        const char *us;
        size_t memberl;
        size_t rsl;
        size_t usl;

        if (!arg_str(&argv[2], &member, &memberl) ||
            !arg_str(&argv[3], &rs, &rsl) ||
            !arg_str(&argv[4], &us, &usl) ||
            !geo_unit(us, usl, &unit) ||
            !geo_parse_radius(rs, rsl, unit, &radius_m)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        opt_start = 5;
        rc = get_zset(d, out, key, keyl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            if (geo_parse_opts(argv, argc, opt_start, allow_store ? 1 : 0, &opts,
                               out) != 0)
                return;
            if (opts.store != NULL) {
                geo_vec empty;
                memset(&empty, 0, sizeof(empty));
                (void)geo_store_results(d, out, opts.store, opts.storelen,
                                        &empty, opts.storedist, unit, now_ms);
            } else {
                resp_write_array_header(out, 0);
            }
            return;
        }
        if (!geo_member_coords(z, member, memberl, &center_lon,
                               &center_lat)) {
            static const char E[] =
                "ERR could not decode requested zset member";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
    } else {
        const char *lons;
        const char *lats;
        const char *rs;
        const char *us;
        size_t lonl;
        size_t latl;
        size_t rsl;
        size_t usl;

        if (!arg_str(&argv[2], &lons, &lonl) ||
            !arg_str(&argv[3], &lats, &latl) ||
            !arg_str(&argv[4], &rs, &rsl) ||
            !arg_str(&argv[5], &us, &usl) ||
            !geo_unit(us, usl, &unit) ||
            !geo_parse_radius(rs, rsl, unit, &radius_m)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        {
            int crc = geo_parse_coords(lons, lonl, lats, latl, &center_lon,
                                       &center_lat);
            if (crc <= 0) {
                geo_write_coords_error(out, crc, center_lon, center_lat);
                return;
            }
        }
        opt_start = 6;
        rc = get_zset(d, out, key, keyl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            if (geo_parse_opts(argv, argc, opt_start, allow_store ? 1 : 0, &opts,
                               out) != 0)
                return;
            if (opts.store != NULL) {
                geo_vec empty;
                memset(&empty, 0, sizeof(empty));
                (void)geo_store_results(d, out, opts.store, opts.storelen,
                                        &empty, opts.storedist, unit, now_ms);
            } else {
                resp_write_array_header(out, 0);
            }
            return;
        }
    }

    if (geo_parse_opts(argv, argc, opt_start, allow_store ? 1 : 0, &opts, out) != 0)
        return;
    (void)geo_execute_radius(d, out, z, center_lon, center_lat, radius_m,
                             unit, &opts, opts.store, opts.storelen, now_ms);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_georadius(db *d, const resp_value *argv, size_t argc,
                          resp_buf *out, uint64_t now_ms, int allow_store,
                          int from_member, const char *cmdname)
{
    geo_radius_command(d, argv, argc, out, now_ms, from_member, allow_store,
                       cmdname);
}

static void geo_search_command(db *d, const resp_value *argv, size_t argc,
                               resp_buf *out, uint64_t now_ms, int store_mode)
{
    const char *dst = NULL;
    const char *src;
    size_t dstl = 0;
    size_t srcl;
    size_t key_idx = store_mode ? 2u : 1u;
    size_t cursor;
    int from_member = 0;
    int from_loc = 0;
    int by_radius = 0;
    int by_box = 0;
    const char *center_arg = NULL;
    size_t center_argl = 0;
    double center_lon = 0.0;
    double center_lat = 0.0;
    double unit = 1.0;
    double radius_m = 0.0;
    double width_m = 0.0;
    double height_m = 0.0;
    obj_zset *z;
    int rc;
    geo_opts opts;

    if (store_mode) {
        if (argc < 8) {
            wrong_args(out, "geosearchstore");
            return;
        }
        if (!arg_str(&argv[1], &dst, &dstl))
            goto bad_type;
    } else if (argc < 7) {
        wrong_args(out, "geosearch");
        return;
    }
    if (!arg_str(&argv[key_idx], &src, &srcl))
        goto bad_type;

    cursor = key_idx + 1;
    if (cursor >= argc) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    {
        const char *mode;
        size_t model;

        if (!arg_str(&argv[cursor], &mode, &model))
            goto bad_type;
        if (ci_equal(mode, model, "FROMMEMBER")) {
            from_member = 1;
            if (cursor + 1 >= argc ||
                !arg_str(&argv[cursor + 1], &center_arg, &center_argl))
                goto bad_type;
            cursor += 2;
        } else if (ci_equal(mode, model, "FROMLONLAT")) {
            const char *lons;
            const char *lats;
            size_t lonl;
            size_t latl;

            if (cursor + 2 >= argc ||
                !arg_str(&argv[cursor + 1], &lons, &lonl) ||
                !arg_str(&argv[cursor + 2], &lats, &latl)) {
                resp_write_error(out, ERR_NOT_FLOAT,
                                 sizeof(ERR_NOT_FLOAT) - 1);
                return;
            }
            {
                int crc = geo_parse_coords(lons, lonl, lats, latl,
                                           &center_lon, &center_lat);
                if (crc <= 0) {
                    geo_write_coords_error(out, crc, center_lon, center_lat);
                    return;
                }
            }
            from_loc = 1;
            cursor += 3;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
    }

    if (cursor >= argc) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    {
        const char *shape;
        size_t shapel;

        if (!arg_str(&argv[cursor], &shape, &shapel))
            goto bad_type;
        if (ci_equal(shape, shapel, "BYRADIUS")) {
            const char *rs;
            const char *us;
            size_t rsl;
            size_t usl;

            if (cursor + 2 >= argc ||
                !arg_str(&argv[cursor + 1], &rs, &rsl) ||
                !arg_str(&argv[cursor + 2], &us, &usl) ||
                !geo_unit(us, usl, &unit) ||
                !geo_parse_radius(rs, rsl, unit, &radius_m)) {
                resp_write_error(out, ERR_NOT_FLOAT,
                                 sizeof(ERR_NOT_FLOAT) - 1);
                return;
            }
            by_radius = 1;
            cursor += 3;
        } else if (ci_equal(shape, shapel, "BYBOX")) {
            const char *ws;
            const char *hs;
            const char *us;
            size_t wsl;
            size_t hsl;
            size_t usl;

            if (cursor + 3 >= argc ||
                !arg_str(&argv[cursor + 1], &ws, &wsl) ||
                !arg_str(&argv[cursor + 2], &hs, &hsl) ||
                !arg_str(&argv[cursor + 3], &us, &usl) ||
                !geo_unit(us, usl, &unit) ||
                !parse_double(ws, wsl, &width_m) || width_m < 0.0 ||
                !parse_double(hs, hsl, &height_m) || height_m < 0.0) {
                resp_write_error(out, ERR_NOT_FLOAT,
                                 sizeof(ERR_NOT_FLOAT) - 1);
                return;
            }
            by_box = 1;
            width_m *= unit;
            height_m *= unit;
            cursor += 4;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
    }

    if ((from_member && from_loc) || (!from_member && !from_loc)) {
        static const char E[] =
            "ERR exactly one of FROMMEMBER or FROMLONLAT can be specified";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if ((by_radius && by_box) || (!by_radius && !by_box)) {
        static const char E[] =
            "ERR exactly one of BYRADIUS and BYBOX can be specified";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    if (geo_parse_opts(argv, argc, cursor, store_mode ? 2 : 0, &opts, out) != 0)
        return;

    rc = get_zset(d, out, src, srcl, 0, now_ms, &z);
    if (rc < 0)
        return;
    if (rc == 0) {
        if (store_mode) {
            geo_vec empty;
            memset(&empty, 0, sizeof(empty));
            (void)geo_store_results(d, out, dst, dstl, &empty, opts.storedist,
                                    unit, now_ms);
        } else {
            resp_write_array_header(out, 0);
        }
        return;
    }
    if (from_member &&
        !geo_member_coords(z, center_arg, center_argl, &center_lon,
                           &center_lat)) {
        static const char E[] =
            "ERR could not decode requested zset member";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    if (by_box) {
        (void)geo_execute_box(d, out, z, center_lon, center_lat, width_m,
                              height_m, unit, &opts,
                              store_mode ? dst : NULL,
                              store_mode ? dstl : 0, now_ms);
    } else {
        (void)geo_execute_radius(d, out, z, center_lon, center_lat, radius_m,
                                 unit, &opts, store_mode ? dst : NULL,
                                 store_mode ? dstl : 0, now_ms);
    }
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void cmd_geosearch(db *d, const resp_value *argv, size_t argc,
                          resp_buf *out, uint64_t now_ms)
{
    geo_search_command(d, argv, argc, out, now_ms, 0);
}

static void cmd_geosearchstore(db *d, const resp_value *argv, size_t argc,
                               resp_buf *out, uint64_t now_ms)
{
    geo_search_command(d, argv, argc, out, now_ms, 1);
}


/* ------------------------------------------------------------------ */
/* server ops / introspection commands                                */
/* ------------------------------------------------------------------ */

static void command_write_flags(resp_buf *out, uint16_t id)
{
    resp_write_array_header(out, 1);
    resp_write_bulk(out, cmd_is_write(id) ? "write" : "readonly",
                    cmd_is_write(id) ? 5 : 8);
}

static void command_info_one(resp_buf *out, uint16_t id)
{
    const cmd_entry *e = cmd_table_entry(id);
    int arity = e->min_argc;
    if (e->max_argc != e->min_argc)
        arity = -e->min_argc;

    resp_write_array_header(out, 6);
    resp_write_bulk(out, e->name, strlen(e->name));
    resp_write_integer(out, arity);
    command_write_flags(out, id);
    if (e->min_argc <= 1) {
        resp_write_integer(out, 0);
        resp_write_integer(out, 0);
        resp_write_integer(out, 0);
    } else {
        resp_write_integer(out, 1);
        resp_write_integer(out, 1);
        resp_write_integer(out, 1);
    }
}

static void command_list_reply(resp_buf *out)
{
    uint16_t id;
    int count = 0;
    for (id = 1; id <= CMD_MAX; id++)
        count++;
    resp_write_array_header(out, (size_t)count);
    for (id = 1; id <= CMD_MAX; id++)
        resp_write_bulk(out, cmd_table_entry(id)->name,
                        strlen(cmd_table_entry(id)->name));
}

static int command_count_reply(resp_buf *out)
{
    uint16_t id;
    int count = 0;
    for (id = 1; id <= CMD_MAX; id++)
        count++;
    resp_write_integer(out, count);
    return 0;
}

/* Small key-position table used by COMMAND GETKEYS/GETKEYSANDFLAGS. It
 * intentionally mirrors the cluster/mt key extraction logic for the common
 * commands. When with_flags is nonzero each key is emitted as
 * [key, flags]; otherwise it is emitted as a flat bulk string. */
static void command_emit_keys_mode(const resp_value *argv, size_t argc,
                                   resp_buf *out, int with_flags)
{
    const char *name;
    size_t nlen;
    uint16_t cmd_id;
    int write;
    const resp_value **keys = NULL;
    size_t nkeys = 0, cap = 0, i;
    int oom = 0;

    /* argv = COMMAND GETKEYS <cmd> [arg ...] */
    if (argc < 3 || !arg_str(&argv[2], &name, &nlen))
        goto done;
    cmd_id = cmd_resolve(name, nlen);
    if (cmd_id == CMD_ID_UNKNOWN)
        goto done;
    write = cmd_is_write(cmd_id);

#define EMIT_KEY(idx_) do { \
        const resp_value *_v = (idx_); \
        if (_v->str != NULL && nkeys == cap) { \
            size_t _nc = cap == 0 ? 8 : cap * 2; \
            const resp_value **_nk = (const resp_value **)realloc(keys, _nc * sizeof(*keys)); \
            if (_nk == NULL) { oom = 1; goto done; } \
            keys = _nk; cap = _nc; \
        } \
        if (_v->str != NULL) keys[nkeys++] = _v; \
    } while (0)

    if (cmd_id == CMD_MGET || cmd_id == CMD_DEL ||
        cmd_id == CMD_UNLINK || cmd_id == CMD_EXISTS ||
        cmd_id == CMD_TOUCH || cmd_id == CMD_SINTER ||
        cmd_id == CMD_SUNION || cmd_id == CMD_SDIFF ||
        cmd_id == CMD_WATCH || cmd_id == CMD_SINTERSTORE ||
        cmd_id == CMD_SUNIONSTORE || cmd_id == CMD_SDIFFSTORE ||
        cmd_id == CMD_BITOP) {
        for (i = 3; i < argc; i++)
            EMIT_KEY(&argv[i]);
        goto done;
    }
    if (cmd_id == CMD_MSET || cmd_id == CMD_MSETNX) {
        for (i = 3; i + 1 < argc; i += 2)
            EMIT_KEY(&argv[i]);
        goto done;
    }
    if (cmd_id == CMD_SMOVE || cmd_id == CMD_RENAME ||
        cmd_id == CMD_RENAMENX || cmd_id == CMD_RPOPLPUSH ||
        cmd_id == CMD_LMOVE || cmd_id == CMD_LMOVEM ||
        cmd_id == CMD_COPY || cmd_id == CMD_LCS ||
        cmd_id == CMD_BRPOPLPUSH || cmd_id == CMD_BLMOVE ||
        cmd_id == CMD_BLMOVEM) {
        for (i = 3; i < argc && i < 5; i++)
            EMIT_KEY(&argv[i]);
        goto done;
    }
    if (cmd_id == CMD_BLPOP || cmd_id == CMD_BRPOP ||
        cmd_id == CMD_BZPOPMIN || cmd_id == CMD_BZPOPMAX) {
        for (i = 3; i + 1 < argc; i++)
            EMIT_KEY(&argv[i]);
        goto done;
    }
    if (cmd_id == CMD_BLMPOP || cmd_id == CMD_BZMPOP) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 5 || !arg_str(&argv[4], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            goto done;
        end = 5 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 5; i < end; i++)
            EMIT_KEY(&argv[i]);
        goto done;
    }
    if (cmd_id == CMD_SINTERCARD || cmd_id == CMD_ZUNION ||
        cmd_id == CMD_ZINTER || cmd_id == CMD_ZDIFF ||
        cmd_id == CMD_ZINTERCARD || cmd_id == CMD_ZMPOP ||
        cmd_id == CMD_LMPOP) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 4 || !arg_str(&argv[3], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            goto done;
        end = 4 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 4; i < end; i++)
            EMIT_KEY(&argv[i]);
        goto done;
    }
    if (cmd_id == CMD_ZUNIONSTORE || cmd_id == CMD_ZINTERSTORE ||
        cmd_id == CMD_ZDIFFSTORE) {
        const char *nv;
        size_t nvl;
        long long nk = 0;
        size_t end;
        if (argc < 5 || !arg_str(&argv[4], &nv, &nvl) ||
            !parse_i64(nv, nvl, &nk) || nk <= 0)
            goto done;
        EMIT_KEY(&argv[3]);
        end = 5 + (size_t)nk;
        if (end > argc)
            end = argc;
        for (i = 5; i < end; i++)
            EMIT_KEY(&argv[i]);
        goto done;
    }
    if (cmd_id == CMD_ZRANGESTORE) {
        EMIT_KEY(&argv[3]);
        EMIT_KEY(&argv[4]);
        goto done;
    }
    if (cmd_id == CMD_GEORADIUS || cmd_id == CMD_GEORADIUSBYMEMBER) {
        size_t start = cmd_id == CMD_GEORADIUS ? 3 : 3;
        (void)start;
        EMIT_KEY(&argv[3]);
        for (i = cmd_id == CMD_GEORADIUS ? 7 : 6; i + 1 < argc; i++) {
            const char *tok;
            size_t tokl;
            if (!arg_str(&argv[i], &tok, &tokl))
                continue;
            if (ci_equal(tok, tokl, "STORE") ||
                ci_equal(tok, tokl, "STOREDIST"))
                EMIT_KEY(&argv[i + 1]);
        }
        goto done;
    }
    if (cmd_id == CMD_GEOSEARCHSTORE) {
        EMIT_KEY(&argv[3]);
        EMIT_KEY(&argv[4]);
        goto done;
    }
    if (cmd_id == CMD_PFDEBUG) {
        EMIT_KEY(&argv[3]);
        goto done;
    }
    if (cmd_id == CMD_XGROUP || cmd_id == CMD_XINFO) {
        if (argc > 4)
            EMIT_KEY(&argv[4]);
        goto done;
    }
    if (cmd_id == CMD_XREAD || cmd_id == CMD_XREADGROUP) {
        size_t streams = 3;
        size_t start, end;
        while (streams < argc) {
            const char *tok;
            size_t tokl;
            if (arg_str(&argv[streams], &tok, &tokl) &&
                ci_equal(tok, tokl, "STREAMS"))
                break;
            streams++;
        }
        if (streams >= argc || (argc - streams - 1) < 2 ||
            ((argc - streams - 1) & 1u) != 0)
            goto done;
        start = streams + 1;
        end = start + (argc - streams - 1) / 2;
        for (i = start; i < end; i++)
            EMIT_KEY(&argv[i]);
        goto done;
    }

    /* Default single-key command: first argument after the command name. */
    if (argc > 3)
        EMIT_KEY(&argv[3]);

#undef EMIT_KEY

done:
    if (oom) {
        free(keys);
        resp_write_error(out, "ERR out of memory", 17);
        return;
    }
    resp_write_array_header(out, nkeys);
    for (i = 0; i < nkeys; i++) {
        const char *flags = write ? "RW" : "RO";
        if (with_flags) {
            resp_write_array_header(out, 2);
            resp_write_bulk(out, keys[i]->str, keys[i]->len);
            resp_write_bulk(out, flags, 2);
        } else {
            resp_write_bulk(out, keys[i]->str, keys[i]->len);
        }
    }
    free(keys);
}

static void command_emit_keys(const resp_value *argv, size_t argc,
                              resp_buf *out)
{
    command_emit_keys_mode(argv, argc, out, 0);
}

static void command_command(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out)
{
    const char *sub;
    size_t sl;
    (void)s;
    if (argc < 2) {
        wrong_args(out, "command");
        return;
    }
    if (!arg_str(&argv[1], &sub, &sl))
        goto bad_type;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "COUNT",
            "LIST",
            "INFO [command-name ...]",
            "GETKEYS command key [key ...]",
            "GETKEYSANDFLAGS command key [key ...]",
            "DOCS"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }

    if (ci_equal(sub, sl, "COUNT") && argc == 2) {
        command_count_reply(out);
        return;
    }
    if (ci_equal(sub, sl, "LIST") && argc == 2) {
        command_list_reply(out);
        return;
    }
    if (ci_equal(sub, sl, "DOCS") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "INFO") && argc >= 3) {
        size_t i;
        resp_write_array_header(out, argc - 2);
        for (i = 2; i < argc; i++) {
            const char *cn;
            size_t cl;
            uint16_t id;
            if (!arg_str(&argv[i], &cn, &cl))
                goto bad_type;
            id = cmd_resolve(cn, cl);
            if (id == CMD_ID_UNKNOWN) {
                resp_write_bulk(out, NULL, 0);
                continue;
            }
            command_info_one(out, id);
        }
        return;
    }
    if (ci_equal(sub, sl, "GETKEYS") && argc >= 3) {
        command_emit_keys(argv, argc, out);
        return;
    }
    if (ci_equal(sub, sl, "GETKEYSANDFLAGS") && argc >= 3) {
        command_emit_keys_mode(argv, argc, out, 1);
        return;
    }
    resp_write_error(out, "ERR unknown COMMAND subcommand",
                        sizeof("ERR unknown COMMAND subcommand") - 1);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_client(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out)
{
    const char *sub;
    size_t sl;
    if (argc < 2) {
        wrong_args(out, "client");
        return;
    }
    if (!arg_str(&argv[1], &sub, &sl))
        goto bad_type;
    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "ID",
            "SETNAME <name>",
            "GETNAME",
            "LIST",
            "INFO",
            "SETINFO <lib-name|lib-ver> <value>",
            "GETREDIR",
            "NO-EVICT ON|OFF",
            "NO-TOUCH ON|OFF",
            "PAUSE <timeout> [WRITE|ALL]",
            "UNPAUSE",
            "REPLY ON|OFF|SKIP",
            "CACHING YES|NO",
            "TRACKING ON|OFF [OPTIONS]",
            "TRACKINGINFO",
            "UNBLOCK <id> [TIMEOUT|ERROR]",
            "KILL <filter ...>"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }
    if (s->client_ctx == NULL) {
        resp_write_error(out, "ERR CLIENT is not supported in this context",
                         sizeof("ERR CLIENT is not supported in this context") - 1);
        return;
    }
    if (ci_equal(sub, sl, "ID") && argc == 2) {
        resp_write_integer(out, s->client_id(s->client_ctx, s));
        return;
    }
    if (ci_equal(sub, sl, "SETNAME") && argc == 3) {
        const char *name;
        size_t nl;
        if (!arg_str(&argv[2], &name, &nl))
            goto bad_type;
        if (s->client_setname(s->client_ctx, s, name, nl) != 0) {
            resp_write_error(out, "ERR client name is too long", 29);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "GETNAME") && argc == 2) {
        const char *name;
        size_t nl = 0;
        name = s->client_getname(s->client_ctx, s, &nl);
        if (name == NULL)
            name = "";
        resp_write_bulk(out, name, nl);
        return;
    }
    if (ci_equal(sub, sl, "LIST") && argc == 2) {
        s->client_list(s->client_ctx, out);
        return;
    }
    if (ci_equal(sub, sl, "INFO") && argc == 2) {
        char buf[192];
        long long id = s->client_id(s->client_ctx, s);
        const char *name;
        size_t nl = 0;
        int n;
        name = s->client_getname(s->client_ctx, s, &nl);
        n = snprintf(buf, sizeof(buf), "id=%lld name=%.*s", id,
                     (int)nl, name == NULL ? "" : name);
        resp_write_bulk(out, buf, (size_t)n);
        return;
    }
    if (ci_equal(sub, sl, "SETINFO") && argc == 4) {
        const char *attr, *val;
        size_t al, vl;
        if (!arg_str(&argv[2], &attr, &al) ||
            !arg_str(&argv[3], &val, &vl))
            goto bad_type;
        (void)val;
        (void)vl;
        if (!ci_equal(attr, al, "lib-name") &&
            !ci_equal(attr, al, "lib-ver")) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "GETREDIR") && argc == 2) {
        resp_write_integer(out, -1);
        return;
    }
    if ((ci_equal(sub, sl, "NO-EVICT") || ci_equal(sub, sl, "NO-TOUCH")) &&
        argc == 3) {
        const char *on;
        size_t onl;
        if (!arg_str(&argv[2], &on, &onl) ||
            (!ci_equal(on, onl, "ON") && !ci_equal(on, onl, "OFF"))) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "PAUSE") && argc >= 3) {
        const char *msv;
        size_t msl;
        long long ms;
        if (!arg_str(&argv[2], &msv, &msl) ||
            !parse_i64(msv, msl, &ms) || ms < 0) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (argc == 4) {
            const char *mode;
            size_t mdl;
            if (!arg_str(&argv[3], &mode, &mdl) ||
                (!ci_equal(mode, mdl, "WRITE") &&
                 !ci_equal(mode, mdl, "ALL"))) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "UNPAUSE") && argc == 2) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "REPLY") && argc == 3) {
        const char *mode;
        size_t mdl;
        if (!arg_str(&argv[2], &mode, &mdl) ||
            (!ci_equal(mode, mdl, "ON") &&
             !ci_equal(mode, mdl, "OFF") &&
             !ci_equal(mode, mdl, "SKIP"))) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "CACHING") && argc == 3) {
        const char *mode;
        size_t mdl;
        if (!arg_str(&argv[2], &mode, &mdl) ||
            (!ci_equal(mode, mdl, "YES") && !ci_equal(mode, mdl, "NO"))) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "TRACKING") && argc >= 3) {
        const char *mode;
        size_t mdl;
        if (!arg_str(&argv[2], &mode, &mdl) ||
            (!ci_equal(mode, mdl, "ON") && !ci_equal(mode, mdl, "OFF"))) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "TRACKINGINFO") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "UNBLOCK") && argc >= 3) {
        const char *idv;
        size_t idl;
        long long id;
        if (!arg_str(&argv[2], &idv, &idl) ||
            !parse_i64(idv, idl, &id)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (argc == 4) {
            const char *opt;
            size_t opl;
            if (!arg_str(&argv[3], &opt, &opl) ||
                (!ci_equal(opt, opl, "TIMEOUT") &&
                 !ci_equal(opt, opl, "ERROR"))) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        resp_write_integer(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "KILL") && argc >= 3) {
        size_t i;
        char filter[128];
        size_t fl = 0;
        for (i = 2; i < argc; i++) {
            const char *tok;
            size_t tl;
            if (!arg_str(&argv[i], &tok, &tl))
                goto bad_type;
            if (fl + tl + (fl != 0) >= sizeof(filter)) {
                resp_write_error(out, "ERR CLIENT KILL filter is too long",
                                 34);
                return;
            }
            if (fl != 0)
                filter[fl++] = ' ';
            memcpy(filter + fl, tok, tl);
            fl += tl;
        }
        if (s->client_kill(s->client_ctx, filter, fl, out))
            return;
        resp_write_integer(out, 0);
        return;
    }
    resp_write_error(out, "ERR unknown CLIENT subcommand",
                        sizeof("ERR unknown CLIENT subcommand") - 1);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static uint64_t command_memory_usage(db *d, const char *key, size_t klen,
                                     uint64_t now_ms)
{
    const char *v;
    size_t vl;
    if (!db_get(d, key, klen, &v, &vl, now_ms))
        return 0;
    return entry_bytes(klen, vl) + obj_extra_mem(v, vl);
}

static void command_memory(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    const char *sub;
    size_t sl;
    if (argc < 2) {
        wrong_args(out, "memory");
        return;
    }
    if (!arg_str(&argv[1], &sub, &sl))
        goto bad_type;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "USAGE <key> [SAMPLES count]",
            "STATS",
            "DOCTOR",
            "PURGE",
            "MALLOC-STATS"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }

    if (ci_equal(sub, sl, "USAGE") && (argc == 3 || argc == 4)) {
        const char *key;
        size_t kl;
        uint64_t usage;
        if (!arg_str(&argv[2], &key, &kl))
            goto bad_type;
        if (argc == 4) {
            /* SAMPLES is accepted and ignored for the deterministic
             * single-thread estimate; Redis clients send it as a hint. */
            const char *opt;
            size_t ol;
            if (!arg_str(&argv[3], &opt, &ol) ||
                !ci_equal(opt, ol, "SAMPLES")) {
                resp_write_error(out, "ERR syntax error", 16);
                return;
            }
        }
        usage = command_memory_usage(s->d, key, kl, now_ms);
        if (usage == 0)
            resp_write_bulk(out, NULL, 0);
        else
            resp_write_integer(out, (long long)usage);
        return;
    }
    if (ci_equal(sub, sl, "STATS") && argc == 2) {
        resp_write_array_header(out, 2);
        resp_write_bulk(out, "used_memory", 11);
        resp_write_integer(out, (long long)s->d->used_memory);
        return;
    }
    if (ci_equal(sub, sl, "DOCTOR") && argc == 2) {
        resp_write_simple_string(out, "Everything is ok", 17);
        return;
    }
    if ((ci_equal(sub, sl, "PURGE") || ci_equal(sub, sl, "MALLOC-STATS")) &&
        argc == 2) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    resp_write_error(out, "ERR unknown MEMORY subcommand",
                        sizeof("ERR unknown MEMORY subcommand") - 1);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_slowlog(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out)
{
    const char *sub;
    size_t sl;
    if (argc < 2) {
        wrong_args(out, "slowlog");
        return;
    }
    if (!arg_str(&argv[1], &sub, &sl))
        goto bad_type;
    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "GET [count]",
            "LEN",
            "RESET"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }
    if (s->slowlog_ctx == NULL) {
        resp_write_error(out, "ERR SLOWLOG is not supported in this context",
                         sizeof("ERR SLOWLOG is not supported in this context") - 1);
        return;
    }
    if (ci_equal(sub, sl, "LEN") && argc == 2) {
        resp_write_integer(out, (long long)s->slowlog_len(s->slowlog_ctx));
        return;
    }
    if (ci_equal(sub, sl, "GET") && (argc == 2 || argc == 3)) {
        long long count = 10;
        if (argc == 3) {
            const char *cv;
            size_t cl;
            if (!arg_str(&argv[2], &cv, &cl) || !parse_i64(cv, cl, &count) ||
                count < 0) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        s->slowlog_get(s->slowlog_ctx, count, out);
        return;
    }
    if (ci_equal(sub, sl, "RESET") && argc == 2) {
        s->slowlog_reset(s->slowlog_ctx);
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    resp_write_error(out, "ERR unknown SLOWLOG subcommand",
                        sizeof("ERR unknown SLOWLOG subcommand") - 1);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_bgsave(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    (void)argv;
    if (argc != 1) {
        wrong_args(out, "bgsave");
        return;
    }
    if (d->snapshot_path == NULL) {
        static const char E[] = "ERR snapshot path not configured";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (s->sel_fn != NULL) {
        int i;
        if (snapshot_save_multi(s->sel_ctx, (snapshot_db_get)s->sel_fn,
                                s->sel_ndbs, d->snapshot_path) != 0) {
            static const char E[] = "ERR snapshot save failed";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        for (i = 0; i < s->sel_ndbs; i++)
            s->sel_fn(s->sel_ctx, i)->last_save = now_ms / 1000;
    } else {
        if (snapshot_save(d, d->snapshot_path) != 0) {
            static const char E[] = "ERR snapshot save failed";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        d->last_save = now_ms / 1000;
    }
    resp_write_simple_string(out, "Background saving started", 25);
}

static void command_bgrewriteaof(session *s, const resp_value *argv,
                                 size_t argc, resp_buf *out)
{
    (void)argv;
    if (argc != 1) {
        wrong_args(out, "bgrewriteaof");
        return;
    }
    if (s->bgrewriteaof == NULL) {
        resp_write_error(out,
                         "ERR BGREWRITEAOF is not supported in this context",
                         sizeof("ERR BGREWRITEAOF is not supported in this context") - 1);
        return;
    }
    s->bgrewriteaof(s->bgrewriteaof_ctx, out);
}

/* ------------------------------------------------------------------ */
/* stream core command helpers                                        */
/* ------------------------------------------------------------------ */

static int stream_parse_full_id(const char *s, size_t len, uint64_t *ms,
                                uint64_t *seq)
{
    size_t dash = 0;
    if (len < 3)
        return 0;
    while (dash < len && s[dash] != '-')
        dash++;
    if (dash == 0 || dash + 1 >= len)
        return 0;
    if (!parse_u64(s, dash, ms) ||
        !parse_u64(s + dash + 1, len - dash - 1, seq))
        return 0;
    return 1;
}

static int stream_parse_range_bound(const char *s, size_t len, int *ex,
                                    uint64_t *ms, uint64_t *seq)
{
    size_t off = 0;
    *ex = 0;
    if (len == 1 && s[0] == '-') {
        *ms = 0;
        *seq = 0;
        return 1;
    }
    if (len == 1 && s[0] == '+') {
        *ms = UINT64_MAX;
        *seq = UINT64_MAX;
        return 1;
    }
    if (len > 1 && s[0] == '(') {
        *ex = 1;
        off = 1;
        len--;
    }
    return stream_parse_full_id(s + off, len, ms, seq);
}

/* Parse an XADD id. Returns 1 valid, 0 malformed, -1 not greater than the
 * stream's last id. */
static int stream_parse_xadd_id(const char *s, size_t len, uint64_t last_ms,
                                uint64_t last_seq, uint64_t now_ms,
                                uint64_t *ms, uint64_t *seq)
{
    size_t dash;
    if (len == 1 && s[0] == '*') {
        *ms = now_ms < last_ms ? last_ms : now_ms;
        *seq = (*ms == last_ms) ? last_seq + 1 : 0;
        return 1;
    }
    dash = 0;
    while (dash < len && s[dash] != '-')
        dash++;
    if (dash > 0 && dash + 1 < len && s[dash + 1] == '*' &&
        dash + 2 == len) {
        if (!parse_u64(s, dash, ms))
            return 0;
        if (*ms < last_ms)
            return -1;
        *seq = (*ms == last_ms) ? last_seq + 1 : 0;
        return 1;
    }
    if (!stream_parse_full_id(s, len, ms, seq))
        return 0;
    if (*ms < last_ms || (*ms == last_ms && *seq <= last_seq))
        return -1;
    return 1;
}

static void stream_emit_entry(resp_buf *out, const stream_entry *e)
{
    char id[64];
    const char *p = e->data;
    int n = snprintf(id, sizeof(id), "%llu-%llu",
                     (unsigned long long)e->ms,
                     (unsigned long long)e->seq);
    uint32_t i;
    resp_write_array_header(out, 2);
    resp_write_bulk(out, id, (size_t)n);
    resp_write_array_header(out, (size_t)e->nfields * 2);
    for (i = 0; i < e->nfields; i++) {
        uint32_t fl = e->lens[2 * i];
        uint32_t vl = e->lens[2 * i + 1];
        resp_write_bulk(out, p, fl);
        p += fl;
        resp_write_bulk(out, p, vl);
        p += vl;
    }
}

static int stream_parse_trim_option(const resp_value *argv, size_t argc,
                                    size_t *pos, int *maxlen,
                                    uint64_t *threshold, uint64_t *min_ms,
                                    uint64_t *min_seq, uint64_t *limit,
                                    resp_buf *out)
{
    const char *kind, *tok, *limv;
    size_t kl, tl, ll;
    size_t i = *pos;
    int max = 0;
    if (i >= argc || !arg_str(&argv[i], &kind, &kl))
        return 0;
    if (ci_equal(kind, kl, "MAXLEN"))
        max = 1;
    else if (!ci_equal(kind, kl, "MINID"))
        return 0;
    i++;
    if (i < argc && arg_str(&argv[i], &tok, &tl) &&
        (ci_equal(tok, tl, "=") || ci_equal(tok, tl, "~")))
        i++;
    if (i >= argc || !arg_str(&argv[i], &tok, &tl))
        goto syntax;
    if (max) {
        if (!parse_u64(tok, tl, threshold))
            goto syntax;
    } else {
        if (!stream_parse_full_id(tok, tl, min_ms, min_seq))
            goto syntax;
    }
    i++;
    if (i + 1 < argc) {
        if (!arg_str(&argv[i], &tok, &tl) || !ci_equal(tok, tl, "LIMIT"))
            goto syntax;
        i++;
        if (!arg_str(&argv[i], &limv, &ll) || !parse_u64(limv, ll, limit))
            goto syntax;
        i++;
    }
    *pos = i;
    *maxlen = max;
    return 1;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return -1;
}

static void command_xadd(session *s, const resp_value *argv, size_t argc,
                         resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *idv;
    size_t kl, idl;
    size_t i = 2, id_idx;
    int nomkstream = 0;
    int trim_set = 0, trim_maxlen = 0;
    uint64_t trim_threshold = 0, trim_min_ms = 0, trim_min_seq = 0;
    uint64_t trim_limit = UINT64_MAX;
    obj_stream *st;
    uint64_t ms, seq, before;
    char idbuf[64];
    int idn;
    int rc;
    const char **fields = NULL;
    const char **values = NULL;
    size_t *flens = NULL, *vlens = NULL;
    size_t nfields = 0;

    if (argc < 5) {
        wrong_args(out, "xadd");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl))
        goto bad_type;
    if (!storage_key_ok(kl)) {
        storage_length_error(out);
        return;
    }
    for (;;) {
        const char *tok;
        size_t tl;
        if (i >= argc || !arg_str(&argv[i], &tok, &tl))
            break;
        if (ci_equal(tok, tl, "NOMKSTREAM") && !nomkstream) {
            nomkstream = 1;
            i++;
        } else if (ci_equal(tok, tl, "MAXLEN") ||
                   ci_equal(tok, tl, "MINID")) {
            int rc2;
            if (trim_set)
                goto syntax;
            rc2 = stream_parse_trim_option(argv, argc, &i, &trim_maxlen,
                                           &trim_threshold, &trim_min_ms,
                                           &trim_min_seq, &trim_limit, out);
            if (rc2 < 0)
                return;
            trim_set = 1;
        } else {
            break;
        }
    }
    id_idx = i;
    if (id_idx >= argc || !arg_str(&argv[id_idx], &idv, &idl))
        goto syntax;
    if ((argc - id_idx - 1) < 2 || ((argc - id_idx - 1) & 1u) != 0)
        goto syntax;

    rc = get_stream(d, out, key, kl, !nomkstream, now_ms, &st);
    if (rc <= 0) {
        if (rc == 0)
            resp_write_bulk(out, NULL, 0);
        return;
    }
    rc = stream_parse_xadd_id(idv, idl, st->last_ms, st->last_seq, now_ms,
                              &ms, &seq);
    if (rc == 0) {
        static const char E[] =
            "ERR Invalid stream ID specified as stream command argument";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (rc < 0) {
        static const char E[] =
            "ERR The ID specified in XADD is equal or smaller than the "
            "target stream top item";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    nfields = (size_t)((argc - id_idx - 1) / 2);
    if (nfields > 0) {
        size_t j;
        fields = (const char **)malloc(nfields * sizeof(*fields));
        values = (const char **)malloc(nfields * sizeof(*values));
        flens = (size_t *)malloc(nfields * sizeof(*flens));
        vlens = (size_t *)malloc(nfields * sizeof(*vlens));
        if (fields == NULL || values == NULL || flens == NULL ||
            vlens == NULL) {
            fprintf(stderr, "ddup: out of memory\n");
            exit(1);
        }
        for (j = 0; j < nfields; j++) {
            if (!arg_str(&argv[id_idx + 1 + 2 * j], &fields[j], &flens[j]) ||
                !arg_str(&argv[id_idx + 2 + 2 * j], &values[j], &vlens[j])) {
                free(fields);
                free(values);
                free(flens);
                free(vlens);
                goto bad_type;
            }
        }
    }

    before = obj_stream_mem(st);
    if (obj_stream_append(st, ms, seq, fields, flens, values, vlens,
                          nfields) != OBJ_STREAM_ADD_OK) {
        static const char E[] = "ERR stream entry is not representable";
        resp_write_error(out, E, sizeof(E) - 1);
        goto cleanup;
    }
    if (trim_set) {
        if (trim_maxlen)
            (void)obj_stream_trim_maxlen(st, trim_threshold, trim_limit);
        else
            (void)obj_stream_trim_minid(st, trim_min_ms, trim_min_seq,
                                        trim_limit);
    }
    mem_sync(d, key, kl, before, obj_stream_mem(st));
    idn = snprintf(idbuf, sizeof(idbuf), "%llu-%llu",
                   (unsigned long long)ms, (unsigned long long)seq);
    resp_write_bulk(out, idbuf, (size_t)idn);

cleanup:
    free(fields);
    free(values);
    free(flens);
    free(vlens);
    return;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xlen(session *s, const resp_value *argv, size_t argc,
                         resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key;
    size_t kl;
    obj_stream *st;
    int rc;
    if (argc != 2) {
        wrong_args(out, "xlen");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl))
        goto bad_type;
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    resp_write_integer(out, rc == 0 ? 0 : (long long)obj_stream_len(st));
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xrange_rev(session *s, const resp_value *argv, size_t argc,
                               resp_buf *out, uint64_t now_ms, int rev)
{
    db *d = s->d;
    const char *key, *startv, *endv;
    size_t kl, startl, endl;
    uint64_t start_ms, start_seq, end_ms, end_seq;
    int start_ex = 0, end_ex = 0;
    long long count = -1;
    obj_stream *st;
    size_t first, last, emitted = 0;
    int rc;
    if (argc != 4 && argc != 6) {
        wrong_args(out, rev ? "xrevrange" : "xrange");
        return;
    }
    if (argc == 6) {
        const char *opt, *cv;
        size_t optl, cvl;
        if (!arg_str(&argv[4], &opt, &optl) ||
            !arg_str(&argv[5], &cv, &cvl) ||
            !ci_equal(opt, optl, "COUNT") || !parse_i64(cv, cvl, &count) ||
            count <= 0) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &startv, &startl) ||
        !arg_str(&argv[3], &endv, &endl))
        goto bad_type;
    if (rev) {
        const char *tmp = startv;
        size_t tmp_len = startl;
        startv = endv;
        startl = endl;
        endv = tmp;
        endl = tmp_len;
    }
    if (!stream_parse_range_bound(startv, startl, &start_ex, &start_ms,
                                  &start_seq) ||
        !stream_parse_range_bound(endv, endl, &end_ex, &end_ms, &end_seq)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_array_header(out, 0);
        return;
    }

    first = obj_stream_lower_bound(st, start_ms, start_seq);
    if (start_ex && first < obj_stream_len(st)) {
        const stream_entry *e = obj_stream_at(st, first);
        if (e->ms == start_ms && e->seq == start_seq)
            first++;
    }
    if (end_ms == UINT64_MAX && end_seq == UINT64_MAX) {
        last = obj_stream_len(st) > 0 ? obj_stream_len(st) - 1 : 0;
    } else {
        size_t idx = obj_stream_lower_bound(st, end_ms, end_seq);
        if (idx < obj_stream_len(st)) {
            const stream_entry *e = obj_stream_at(st, idx);
            if (e->ms == end_ms && e->seq == end_seq && !end_ex)
                last = idx;
            else
                last = idx > 0 ? idx - 1 : SIZE_MAX;
        } else if (idx > 0) {
            last = idx - 1;
        } else {
            last = SIZE_MAX;
        }
    }
    if (first >= obj_stream_len(st) || last == SIZE_MAX ||
        first > last) {
        resp_write_array_header(out, 0);
        return;
    }
    if (count < 0 || (uint64_t)(last - first + 1) <= (uint64_t)count)
        emitted = last - first + 1;
    else
        emitted = (size_t)count;
    resp_write_array_header(out, emitted);
    if (rev) {
        size_t n = emitted;
        size_t idx = last;
        while (n > 0) {
            stream_emit_entry(out, obj_stream_at(st, idx));
            if (idx == first)
                break;
            idx--;
            n--;
        }
    } else {
        size_t idx;
        for (idx = first; idx < first + emitted; idx++)
            stream_emit_entry(out, obj_stream_at(st, idx));
    }
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

/* Delete strategy for XDELEX/XACKDEL. */
#define STREAM_EX_KEEPREF 1
#define STREAM_EX_DELREF  2
#define STREAM_EX_ACKED   3

static int stream_parse_ex_ids(const resp_value *argv, size_t argc,
                               size_t start, int *strategy,
                               size_t *ids_start, size_t *numids,
                               resp_buf *out);
static int stream_entry_referenced(const obj_stream *st, uint64_t ms,
                                   uint64_t seq);
static void stream_entry_remove_refs(obj_stream *st, uint64_t ms,
                                     uint64_t seq);

static void command_xdel(session *s, const resp_value *argv, size_t argc,
                         resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key;
    size_t kl;
    obj_stream *st;
    uint64_t before;
    long long deleted = 0;
    size_t i;
    int rc;
    if (argc < 3) {
        wrong_args(out, "xdel");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl))
        goto bad_type;
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_integer(out, 0);
        return;
    }
    before = obj_stream_mem(st);
    for (i = 2; i < argc; i++) {
        const char *idv;
        size_t idl;
        uint64_t ms, seq;
        if (!arg_str(&argv[i], &idv, &idl) ||
            !stream_parse_full_id(idv, idl, &ms, &seq)) {
            static const char E[] =
                "ERR Invalid stream ID specified as stream command argument";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (obj_stream_delete(st, ms, seq))
            deleted++;
    }
    if (deleted > 0)
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    resp_write_integer(out, deleted);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xdelex(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key;
    size_t kl;
    obj_stream *st;
    uint64_t before;
    uint64_t *ids = NULL;
    size_t ids_start = 0, numids = 0, i;
    int strategy = 0;
    int rc;
    int changed = 0;

    if (argc < 5) {
        wrong_args(out, "xdelex");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl))
        goto bad_type;
    if (stream_parse_ex_ids(argv, argc, 2, &strategy, &ids_start,
                            &numids, out) != 0)
        return;

    ids = (uint64_t *)malloc(numids * 2 * sizeof(*ids));
    if (ids == NULL) {
        resp_write_error(out, OOM_MSG, sizeof(OOM_MSG) - 1);
        return;
    }
    for (i = 0; i < numids; i++) {
        const char *idv;
        size_t idl;
        if (!arg_str(&argv[ids_start + i], &idv, &idl) ||
            !stream_parse_full_id(idv, idl, &ids[2 * i], &ids[2 * i + 1])) {
            static const char E[] =
                "ERR Invalid stream ID specified as stream command argument";
            resp_write_error(out, E, sizeof(E) - 1);
            free(ids);
            return;
        }
    }

    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0) {
        free(ids);
        return;
    }
    resp_write_array_header(out, numids);
    if (rc == 0) {
        for (i = 0; i < numids; i++)
            resp_write_integer(out, -1);
        free(ids);
        return;
    }

    before = obj_stream_mem(st);
    for (i = 0; i < numids; i++) {
        uint64_t ms = ids[2 * i];
        uint64_t seq = ids[2 * i + 1];
        int result = -1;
        if (strategy == STREAM_EX_ACKED &&
            stream_entry_referenced(st, ms, seq)) {
            result = 2;
        } else {
            if (strategy == STREAM_EX_DELREF) {
                if (stream_entry_referenced(st, ms, seq))
                    changed = 1;
                stream_entry_remove_refs(st, ms, seq);
            }
            if (obj_stream_delete(st, ms, seq)) {
                result = 1;
                changed = 1;
            }
        }
        resp_write_integer(out, result);
    }
    if (changed)
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    free(ids);
    return;

bad_type:
    free(ids);
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xtrim(session *s, const resp_value *argv, size_t argc,
                          resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key;
    size_t kl;
    obj_stream *st;
    uint64_t before;
    size_t removed = 0;
    int maxlen = 0, rc;
    uint64_t threshold = 0, min_ms = 0, min_seq = 0, limit = UINT64_MAX;
    size_t pos = 2;
    if (argc < 4) {
        wrong_args(out, "xtrim");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl))
        goto bad_type;
    if (stream_parse_trim_option(argv, argc, &pos, &maxlen, &threshold,
                                 &min_ms, &min_seq, &limit, out) < 0)
        return;
    if (pos != argc) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_integer(out, 0);
        return;
    }
    before = obj_stream_mem(st);
    if (maxlen)
        removed = obj_stream_trim_maxlen(st, threshold, limit);
    else
        removed = obj_stream_trim_minid(st, min_ms, min_seq, limit);
    if (removed > 0)
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    resp_write_integer(out, (long long)removed);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xsetid(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *idv;
    size_t kl, idl;
    obj_stream *st;
    uint64_t ms, seq, entries_added = UINT64_MAX;
    uint64_t del_ms = UINT64_MAX, del_seq = UINT64_MAX;
    size_t i = 3;
    int rc;
    if (argc < 3) {
        wrong_args(out, "xsetid");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) || !arg_str(&argv[2], &idv, &idl))
        goto bad_type;
    if (!stream_parse_full_id(idv, idl, &ms, &seq)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    for (; i + 1 < argc; i += 2) {
        const char *opt, *val;
        size_t optl, vall;
        if (!arg_str(&argv[i], &opt, &optl) ||
            !arg_str(&argv[i + 1], &val, &vall))
            goto bad_type;
        if (ci_equal(opt, optl, "ENTRIESADDED")) {
            if (!parse_u64(val, vall, &entries_added))
                goto syntax;
        } else if (ci_equal(opt, optl, "MAXDELETEDID")) {
            if (!stream_parse_full_id(val, vall, &del_ms, &del_seq))
                goto syntax;
        } else {
            goto syntax;
        }
    }
    if (i != argc)
        goto syntax;
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_error(out, "ERR no such key", 15);
        return;
    }
    if (ms < st->last_ms || (ms == st->last_ms && seq < st->last_seq)) {
        static const char E[] =
            "ERR The ID specified in XSETID is smaller than the target "
            "stream top item";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    {
        uint64_t before = obj_stream_mem(st);
        st->last_ms = ms;
        st->last_seq = seq;
        if (entries_added != UINT64_MAX)
            st->entries_added = entries_added;
        if (del_ms != UINT64_MAX) {
            st->max_deleted_ms = del_ms;
            st->max_deleted_seq = del_seq;
        }
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    }
    resp_write_simple_string(out, "OK", 2);
    return;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static int stream_entry_index(const obj_stream *st, uint64_t ms, uint64_t seq,
                              size_t *idx);

static void command_xcfgset(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key;
    size_t kl;
    obj_stream *st;
    int rc;
    int have_duration = 0;
    int have_maxsize = 0;
    size_t i = 2;

    if (argc < 2) {
        wrong_args(out, "xcfgset");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl))
        goto bad_type;
    while (i < argc) {
        const char *opt, *val;
        size_t optl, vall;
        long long n;
        if (i + 1 >= argc)
            goto syntax;
        if (!arg_str(&argv[i], &opt, &optl) ||
            !arg_str(&argv[i + 1], &val, &vall))
            goto bad_type;
        if (!parse_i64(val, vall, &n) || n < 0)
            goto syntax;
        if (ci_equal(opt, optl, "IDMP-DURATION")) {
            if (have_duration)
                goto syntax;
            have_duration = 1;
        } else if (ci_equal(opt, optl, "IDMP-MAXSIZE")) {
            if (have_maxsize)
                goto syntax;
            have_maxsize = 1;
        } else {
            goto syntax;
        }
        i += 2;
    }
    if (!have_duration && !have_maxsize)
        goto syntax;

    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_error(out, "ERR no such key", 15);
        return;
    }
    (void)st;
    resp_write_simple_string(out, "OK", 2);
    return;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xidmprecord(session *s, const resp_value *argv,
                                size_t argc, resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *pid, *iid, *idv;
    size_t kl, pl, il, idl;
    obj_stream *st;
    uint64_t ms, seq;
    size_t eidx;
    int rc;

    if (argc != 5) {
        wrong_args(out, "xidmprecord");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &pid, &pl) ||
        !arg_str(&argv[3], &iid, &il) ||
        !arg_str(&argv[4], &idv, &idl))
        goto bad_type;
    if (pl == 0) {
        static const char E[] = "ERR producer ID must be non-empty";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (il == 0) {
        static const char E[] = "ERR idempotent ID must be non-empty";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (!stream_parse_full_id(idv, idl, &ms, &seq)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_error(out, "ERR no such key", 15);
        return;
    }
    if (!stream_entry_index(st, ms, seq, &eidx)) {
        static const char E[] = "ERR No such message in stream";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    resp_write_simple_string(out, "OK", 2);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

/* ------------------------------------------------------------------ */
/* stream consumer-group command family                               */
/* ------------------------------------------------------------------ */

static int stream_id_gt(uint64_t a_ms, uint64_t a_seq, uint64_t b_ms,
                        uint64_t b_seq)
{
    return a_ms > b_ms || (a_ms == b_ms && a_seq > b_seq);
}

static int stream_name_eq(const char *a, size_t alen, const char *b,
                          size_t blen)
{
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

static void stream_write_id(resp_buf *out, uint64_t ms, uint64_t seq)
{
    char id[64];
    int n = snprintf(id, sizeof(id), "%llu-%llu",
                     (unsigned long long)ms, (unsigned long long)seq);
    resp_write_bulk(out, id, (size_t)n);
}

static int stream_entry_index(const obj_stream *st, uint64_t ms, uint64_t seq,
                              size_t *idx)
{
    size_t i = obj_stream_lower_bound(st, ms, seq);
    if (i < obj_stream_len(st)) {
        const stream_entry *e = obj_stream_at(st, i);
        if (e->ms == ms && e->seq == seq) {
            *idx = i;
            return 1;
        }
    }
    return 0;
}

static stream_pending *stream_group_pel_find(stream_group *g, uint64_t ms,
                                             uint64_t seq,
                                             stream_consumer **owner)
{
    size_t i;
    for (i = 0; i < g->nconsumers; i++) {
        stream_consumer *c = &g->consumers[i];
        stream_pending *p = obj_stream_consumer_pel_find(c, ms, seq);
        if (p != NULL) {
            if (owner != NULL)
                *owner = c;
            return p;
        }
    }
    return NULL;
}

/* Returns 1 when any consumer group still references this stream entry. */
static int stream_entry_referenced(const obj_stream *st, uint64_t ms,
                                   uint64_t seq)
{
    size_t i;
    for (i = 0; i < st->ngroups; i++) {
        if (stream_group_pel_find(&st->groups[i], ms, seq, NULL) != NULL)
            return 1;
    }
    return 0;
}

/* Remove all PEL references to an entry across every consumer group. */
static void stream_entry_remove_refs(obj_stream *st, uint64_t ms,
                                     uint64_t seq)
{
    size_t i;
    for (i = 0; i < st->ngroups; i++)
        (void)obj_stream_group_pel_remove(&st->groups[i], ms, seq);
}

/* Delete strategy for XDELEX/XACKDEL. */
#define STREAM_EX_KEEPREF 1
#define STREAM_EX_DELREF  2
#define STREAM_EX_ACKED   3

/* Parse [KEEPREF|DELREF|ACKED] IDS <numids> <id ...> with no trailing
 * options. start is the first argument after the fixed key/group fields. */
static int stream_parse_ex_ids(const resp_value *argv, size_t argc,
                               size_t start, int *strategy,
                               size_t *ids_start, size_t *numids,
                               resp_buf *out)
{
    size_t i = start;
    long long n = 0;
    *strategy = 0;
    while (i < argc) {
        const char *tok;
        size_t tl;
        if (!arg_str(&argv[i], &tok, &tl))
            goto ex_bad_arg;
        if (ci_equal(tok, tl, "KEEPREF") && *strategy == 0) {
            *strategy = STREAM_EX_KEEPREF;
            i++;
        } else if (ci_equal(tok, tl, "DELREF") && *strategy == 0) {
            *strategy = STREAM_EX_DELREF;
            i++;
        } else if (ci_equal(tok, tl, "ACKED") && *strategy == 0) {
            *strategy = STREAM_EX_ACKED;
            i++;
        } else if (ci_equal(tok, tl, "IDS") && i + 1 < argc) {
            const char *nv;
            size_t nvl;
            if (!arg_str(&argv[i + 1], &nv, &nvl) ||
                !parse_i64(nv, nvl, &n) || n <= 0)
                goto ex_syntax;
            *ids_start = i + 2;
            *numids = (size_t)n;
            if (*numids > argc - *ids_start) {
                static const char E[] =
                    "ERR number of IDs doesn't match numids";
                resp_write_error(out, E, sizeof(E) - 1);
                return -1;
            }
            if (*ids_start + *numids != argc) {
                static const char E[] =
                    "ERR number of IDs doesn't match numids";
                resp_write_error(out, E, sizeof(E) - 1);
                return -1;
            }
            if (*strategy == 0)
                *strategy = STREAM_EX_KEEPREF;
            return 0;
        } else {
            goto ex_syntax;
        }
    }
    static const char MISSING[] = "ERR IDS option is required";
    resp_write_error(out, MISSING, sizeof(MISSING) - 1);
    return -1;

ex_syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return -1;
ex_bad_arg:
    resp_write_error(out, "ERR invalid argument type", 24);
    return -1;
}

static void command_xgroup(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *sub, *key, *group, *consumer, *idv, *opt, *val;
    size_t subl, kl, gl, cl, idl, optl, vall;
    obj_stream *st;
    stream_group *g;
    int rc;

    if (argc < 2) {
        wrong_args(out, "xgroup");
        return;
    }
    if (!arg_str(&argv[1], &sub, &subl))
        goto bad_type;

    if (ci_equal(sub, subl, "HELP")) {
        static const char *help[] = {
            "CREATE <key> <group> <id|$> [MKSTREAM] [ENTRIESREAD entries-read]",
            "SETID <key> <group> <id|$> [ENTRIESREAD entries-read]",
            "DESTROY <key> <group>",
            "CREATECONSUMER <key> <group> <consumer>",
            "DELCONSUMER <key> <group> <consumer>"
        };
        size_t i;
        if (argc != 2) {
            wrong_args(out, "xgroup|HELP");
            return;
        }
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }

    if (argc < 3) {
        wrong_args(out, "xgroup");
        return;
    }
    if (!arg_str(&argv[2], &key, &kl))
        goto bad_type;

    if (ci_equal(sub, subl, "CREATE") || ci_equal(sub, subl, "SETID")) {
        uint64_t ms = 0, seq = 0, entries_read = UINT64_MAX;
        int create = ci_equal(sub, subl, "CREATE");
        int mkstream = 0, created = 0, dollar = 0;
        size_t i;
        if (argc < 5) {
            wrong_args(out, "xgroup");
            return;
        }
        if (!arg_str(&argv[3], &group, &gl) ||
            !arg_str(&argv[4], &idv, &idl))
            goto bad_type;
        i = 5;
        while (i < argc) {
            if (!arg_str(&argv[i], &opt, &optl))
                goto bad_type;
            if (ci_equal(opt, optl, "MKSTREAM")) {
                if (!create || mkstream)
                    goto syntax;
                mkstream = 1;
                i++;
            } else if (ci_equal(opt, optl, "ENTRIESREAD")) {
                if (i + 1 >= argc)
                    goto syntax;
                if (!arg_str(&argv[i + 1], &val, &vall) ||
                    !parse_u64(val, vall, &entries_read))
                    goto syntax;
                i += 2;
            } else {
                goto syntax;
            }
        }
        dollar = ci_equal(idv, idl, "$");
        if (!dollar && !stream_parse_full_id(idv, idl, &ms, &seq))
            goto syntax;
        rc = get_stream(d, out, key, kl, create && mkstream, now_ms, &st);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_error(out, "ERR no such key", 15);
            return;
        }
        if (dollar) {
            ms = st->last_ms;
            seq = st->last_seq;
        }
        g = obj_stream_group_create(st, group, gl, ms, seq, &created);
        if (g == NULL) {
            storage_length_error(out);
            return;
        }
        if (create && created == 0) {
            static const char E[] =
                "BUSYGROUP Consumer Group name already exists";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (entries_read != UINT64_MAX)
            g->entries_read = entries_read;
        else if (create)
            g->entries_read = 0;
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (argc < 4) {
        wrong_args(out, "xgroup");
        return;
    }
    if (!arg_str(&argv[3], &group, &gl))
        goto bad_type;
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_integer(out, 0);
        return;
    }
    g = obj_stream_group_get(st, group, gl);
    if (ci_equal(sub, subl, "DESTROY")) {
        if (argc != 4) {
            wrong_args(out, "xgroup|DESTROY");
            return;
        }
        resp_write_integer(out, obj_stream_group_destroy(st, group, gl));
        return;
    }
    if (g == NULL) {
        static const char E[] = "NOGROUP No such consumer group";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (ci_equal(sub, subl, "CREATECONSUMER") ||
        ci_equal(sub, subl, "DELCONSUMER")) {
        stream_consumer *c;
        uint64_t pending = 0;
        uint64_t before = obj_stream_mem(st);
        int is_create = ci_equal(sub, subl, "CREATECONSUMER");
        if (argc != 5) {
            wrong_args(out, is_create ? "xgroup|CREATECONSUMER"
                                      : "xgroup|DELCONSUMER");
            return;
        }
        if (!arg_str(&argv[4], &consumer, &cl))
            goto bad_type;
        c = obj_stream_consumer_get(g, consumer, cl);
        if (is_create) {
            int existed = c != NULL;
            c = obj_stream_consumer_create(g, consumer, cl);
            if (c == NULL) {
                storage_length_error(out);
                return;
            }
            (void)before;
            mem_sync(d, key, kl, before, obj_stream_mem(st));
            resp_write_integer(out, existed ? 0 : 1);
            return;
        }
        if (c != NULL)
            pending = (uint64_t)c->pel_len;
        if (!obj_stream_consumer_destroy(g, consumer, cl)) {
            resp_write_integer(out, 0);
            return;
        }
        mem_sync(d, key, kl, before, obj_stream_mem(st));
        resp_write_integer(out, (long long)pending);
        return;
    }
    goto syntax;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xack(session *s, const resp_value *argv, size_t argc,
                         resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *group, *idv;
    size_t kl, gl, idl;
    obj_stream *st;
    stream_group *g;
    uint64_t before, acked = 0;
    size_t i;
    int rc;
    if (argc < 4) {
        wrong_args(out, "xack");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &group, &gl))
        goto bad_type;
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0 || obj_stream_group_get(st, group, gl) == NULL) {
        resp_write_integer(out, 0);
        return;
    }
    g = obj_stream_group_get(st, group, gl);
    before = obj_stream_mem(st);
    for (i = 3; i < argc; i++) {
        uint64_t ms, seq;
        if (!arg_str(&argv[i], &idv, &idl) ||
            !stream_parse_full_id(idv, idl, &ms, &seq)) {
            static const char E[] =
                "ERR Invalid stream ID specified as stream command argument";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (obj_stream_group_pel_remove(g, ms, seq))
            acked++;
    }
    if (acked > 0)
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    resp_write_integer(out, (long long)acked);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xackdel(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *group;
    size_t kl, gl;
    obj_stream *st;
    stream_group *g;
    uint64_t before;
    uint64_t *ids = NULL;
    size_t ids_start = 0, numids = 0, i;
    int strategy = 0;
    int rc;
    int changed = 0;

    if (argc < 6) {
        wrong_args(out, "xackdel");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &group, &gl))
        goto bad_type;
    if (stream_parse_ex_ids(argv, argc, 3, &strategy, &ids_start,
                            &numids, out) != 0)
        return;

    ids = (uint64_t *)malloc(numids * 2 * sizeof(*ids));
    if (ids == NULL) {
        resp_write_error(out, OOM_MSG, sizeof(OOM_MSG) - 1);
        return;
    }
    for (i = 0; i < numids; i++) {
        const char *idv;
        size_t idl;
        if (!arg_str(&argv[ids_start + i], &idv, &idl) ||
            !stream_parse_full_id(idv, idl, &ids[2 * i], &ids[2 * i + 1])) {
            static const char E[] =
                "ERR Invalid stream ID specified as stream command argument";
            resp_write_error(out, E, sizeof(E) - 1);
            free(ids);
            return;
        }
    }

    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0) {
        free(ids);
        return;
    }
    resp_write_array_header(out, numids);
    if (rc == 0 || (g = obj_stream_group_get(st, group, gl)) == NULL) {
        for (i = 0; i < numids; i++)
            resp_write_integer(out, -1);
        free(ids);
        return;
    }

    before = obj_stream_mem(st);
    for (i = 0; i < numids; i++) {
        uint64_t ms = ids[2 * i];
        uint64_t seq = ids[2 * i + 1];
        int result = -1;
        if (obj_stream_group_pel_remove(g, ms, seq)) {
            changed = 1;
            if (strategy == STREAM_EX_ACKED &&
                stream_entry_referenced(st, ms, seq)) {
                result = 2;
            } else {
                if (strategy == STREAM_EX_DELREF) {
                    stream_entry_remove_refs(st, ms, seq);
                    changed = 1;
                }
                (void)obj_stream_delete(st, ms, seq);
                result = 1;
            }
        }
        resp_write_integer(out, result);
    }
    if (changed)
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    free(ids);
    return;

bad_type:
    free(ids);
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xpending(session *s, const resp_value *argv, size_t argc,
                             resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *group, *consumer = NULL;
    size_t kl, gl, cl = 0;
    obj_stream *st;
    stream_group *g;
    int rc;
    if (argc != 3 && argc != 6 && argc != 7) {
        wrong_args(out, "xpending");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &group, &gl))
        goto bad_type;
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0 || (g = obj_stream_group_get(st, group, gl)) == NULL) {
        resp_write_integer(out, 0);
        return;
    }
    if (argc == 3) {
        uint64_t pending = obj_stream_group_pending_count(g);
        uint64_t min_ms = 0, min_seq = 0, max_ms = 0, max_seq = 0;
        int have = 0;
        size_t i, j;
        for (i = 0; i < g->nconsumers; i++) {
            for (j = 0; j < g->consumers[i].pel_len; j++) {
                stream_pending *p = &g->consumers[i].pel[j];
                if (!have || stream_id_gt(min_ms, min_seq, p->ms, p->seq)) {
                    min_ms = p->ms;
                    min_seq = p->seq;
                }
                if (!have || stream_id_gt(p->ms, p->seq, max_ms, max_seq)) {
                    max_ms = p->ms;
                    max_seq = p->seq;
                }
                have = 1;
            }
        }
        resp_write_array_header(out, 4);
        resp_write_integer(out, (long long)pending);
        if (have)
            stream_write_id(out, min_ms, min_seq);
        else
            resp_write_bulk(out, NULL, 0);
        if (have)
            stream_write_id(out, max_ms, max_seq);
        else
            resp_write_bulk(out, NULL, 0);
        resp_write_array_header(out, g->nconsumers);
        for (i = 0; i < g->nconsumers; i++) {
            resp_write_array_header(out, 2);
            resp_write_bulk(out, g->consumers[i].name,
                            g->consumers[i].name_len);
            resp_write_integer(out, (long long)g->consumers[i].pel_len);
        }
        return;
    }

    {
        const char *sv, *ev, *cv;
        size_t svl, evl, cvl;
        uint64_t sms, sseq, ems, eseq, count, emitted = 0;
        size_t i, j;
        if (!arg_str(&argv[3], &sv, &svl) ||
            !arg_str(&argv[4], &ev, &evl) ||
            !arg_str(&argv[5], &cv, &cvl))
            goto bad_type;
        if (!stream_parse_full_id(sv, svl, &sms, &sseq) ||
            !stream_parse_full_id(ev, evl, &ems, &eseq) ||
            !parse_u64(cv, cvl, &count))
            goto syntax;
        if (argc == 7) {
            if (!arg_str(&argv[6], &consumer, &cl))
                goto bad_type;
            if (cl == 0)
                goto syntax;
        }
        /* First pass counts matching entries; second pass emits. */
        for (i = 0; i < g->nconsumers; i++) {
            stream_consumer *c = &g->consumers[i];
            if (consumer != NULL &&
                !stream_name_eq(c->name, c->name_len, consumer, cl))
                continue;
            for (j = 0; j < c->pel_len; j++) {
                stream_pending *p = &c->pel[j];
                if (p->ms < sms || (p->ms == sms && p->seq < sseq) ||
                    p->ms > ems || (p->ms == ems && p->seq > eseq))
                    continue;
                if (emitted < count)
                    emitted++;
            }
        }
        resp_write_array_header(out, emitted);
        emitted = 0;
        for (i = 0; i < g->nconsumers && emitted < count; i++) {
            stream_consumer *c = &g->consumers[i];
            if (consumer != NULL &&
                !stream_name_eq(c->name, c->name_len, consumer, cl))
                continue;
            for (j = 0; j < c->pel_len && emitted < count; j++) {
                stream_pending *p = &c->pel[j];
                if (p->ms < sms || (p->ms == sms && p->seq < sseq) ||
                    p->ms > ems || (p->ms == ems && p->seq > eseq))
                    continue;
                resp_write_array_header(out, 4);
                stream_write_id(out, p->ms, p->seq);
                resp_write_bulk(out, c->name, c->name_len);
                resp_write_integer(out,
                    (long long)(p->idle > now_ms ? 0 : now_ms - p->idle));
                resp_write_integer(out, (long long)p->delivery_count);
                emitted++;
            }
        }
        return;
    }

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xclaim(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *group, *consumer;
    size_t kl, gl, cl;
    uint64_t min_idle, idle_ms = UINT64_MAX, time_ms = now_ms;
    int force = 0, justid = 0;
    uint64_t last_ms = UINT64_MAX, last_seq = UINT64_MAX;
    obj_stream *st;
    stream_group *g;
    stream_consumer *target = NULL;
    uint64_t before, count = 0;
    size_t pos, id_start, id_count;
    int rc;

    if (argc < 6) {
        wrong_args(out, "xclaim");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &group, &gl) ||
        !arg_str(&argv[3], &consumer, &cl))
        goto bad_type;
    {
        const char *mv;
        size_t mvl;
        if (!arg_str(&argv[4], &mv, &mvl) || !parse_u64(mv, mvl, &min_idle))
            goto syntax;
    }

    id_start = 5;
    id_count = 0;
    for (pos = id_start; pos < argc; pos++) {
        const char *idv;
        size_t idl;
        uint64_t ms, seq;
        if (!arg_str(&argv[pos], &idv, &idl))
            goto bad_type;
        if (!stream_parse_full_id(idv, idl, &ms, &seq))
            break;
        id_count++;
    }
    if (id_count == 0)
        goto syntax;

    while (pos < argc) {
        const char *opt, *val;
        size_t optl, vall;
        if (!arg_str(&argv[pos], &opt, &optl))
            goto bad_type;
        if (ci_equal(opt, optl, "FORCE")) {
            force = 1;
            pos++;
        } else if (ci_equal(opt, optl, "JUSTID")) {
            justid = 1;
            pos++;
        } else if (ci_equal(opt, optl, "IDLE") ||
                   ci_equal(opt, optl, "TIME") ||
                   ci_equal(opt, optl, "LASTID") ||
                   ci_equal(opt, optl, "RETRYCOUNT")) {
            if (pos + 1 >= argc)
                goto syntax;
            if (!arg_str(&argv[pos + 1], &val, &vall))
                goto bad_type;
            if (ci_equal(opt, optl, "RETRYCOUNT")) {
                static const char E[] =
                    "ERR XCLAIM RETRYCOUNT is not supported by this build";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            if (ci_equal(opt, optl, "IDLE")) {
                if (!parse_u64(val, vall, &idle_ms))
                    goto syntax;
            } else if (ci_equal(opt, optl, "TIME")) {
                if (!parse_u64(val, vall, &time_ms))
                    goto syntax;
            } else {
                if (!stream_parse_full_id(val, vall, &last_ms, &last_seq))
                    goto syntax;
            }
            pos += 2;
        } else {
            goto syntax;
        }
    }

    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0 || (g = obj_stream_group_get(st, group, gl)) == NULL) {
        static const char E[] = "NOGROUP No such consumer group";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (!justid) {
        target = obj_stream_consumer_create(g, consumer, cl);
        if (target == NULL) {
            storage_length_error(out);
            return;
        }
    }

    for (pos = id_start; pos < id_start + id_count; pos++) {
        const char *idv;
        size_t idl;
        uint64_t ms, seq;
        stream_pending *p;
        if (!arg_str(&argv[pos], &idv, &idl) ||
            !stream_parse_full_id(idv, idl, &ms, &seq)) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        p = stream_group_pel_find(g, ms, seq, NULL);
        if (force || (p != NULL &&
                      (p->idle > now_ms ? 0 : now_ms - p->idle) >= min_idle))
            count++;
    }

    before = obj_stream_mem(st);
    resp_write_array_header(out, count);
    for (pos = id_start; pos < id_start + id_count; pos++) {
        const char *idv;
        size_t idl;
        uint64_t ms, seq, delivery = 1, new_idle;
        stream_pending *p;
        size_t eidx;
        if (!arg_str(&argv[pos], &idv, &idl) ||
            !stream_parse_full_id(idv, idl, &ms, &seq)) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        p = stream_group_pel_find(g, ms, seq, NULL);
        if (!force && (p == NULL ||
                       (p->idle > now_ms ? 0 : now_ms - p->idle) < min_idle))
            continue;
        if (p != NULL) {
            delivery = p->delivery_count + (justid ? 0 : 1);
            if (!justid)
                (void)obj_stream_group_pel_remove(g, ms, seq);
        }
        if (justid) {
            stream_write_id(out, ms, seq);
            continue;
        }
        new_idle = idle_ms != UINT64_MAX ? idle_ms : time_ms;
        if (obj_stream_consumer_pel_add(g, target, ms, seq, new_idle,
                                        delivery) == NULL) {
            storage_length_error(out);
            return;
        }
        target->seen_time = now_ms;
        if (stream_entry_index(st, ms, seq, &eidx))
            stream_emit_entry(out, obj_stream_at(st, eidx));
        else
            resp_write_bulk(out, NULL, 0);
    }
    if (!justid && last_ms != UINT64_MAX) {
        g->last_ms = last_ms;
        g->last_seq = last_seq;
    }
    mem_sync(d, key, kl, before, obj_stream_mem(st));
    return;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xautoclaim(session *s, const resp_value *argv, size_t argc,
                               resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *group, *consumer, *sv;
    size_t kl, gl, cl, svl;
    uint64_t min_idle, start_ms, start_seq, count = 100;
    int justid = 0;
    obj_stream *st;
    stream_group *g;
    stream_consumer *target;
    uint64_t before, eligible_count = 0, next_ms = 0, next_seq = 0;
    uint64_t to_emit;
    size_t pos, i, j;
    int rc;

    if (argc < 6) {
        wrong_args(out, "xautoclaim");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &group, &gl) ||
        !arg_str(&argv[3], &consumer, &cl) ||
        !arg_str(&argv[4], &sv, &svl) ||
        !parse_u64(sv, svl, &min_idle) ||
        !arg_str(&argv[5], &sv, &svl) ||
        !stream_parse_full_id(sv, svl, &start_ms, &start_seq))
        goto syntax;
    pos = 6;
    while (pos < argc) {
        const char *opt, *val;
        size_t optl, vall;
        if (!arg_str(&argv[pos], &opt, &optl))
            goto bad_type;
        if (ci_equal(opt, optl, "JUSTID")) {
            justid = 1;
            pos++;
        } else if (ci_equal(opt, optl, "COUNT")) {
            if (pos + 1 >= argc)
                goto syntax;
            if (!arg_str(&argv[pos + 1], &val, &vall) ||
                !parse_u64(val, vall, &count))
                goto syntax;
            pos += 2;
        } else {
            goto syntax;
        }
    }
    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0 || (g = obj_stream_group_get(st, group, gl)) == NULL) {
        resp_write_array_header(out, 2);
        stream_write_id(out, 0, 0);
        resp_write_array_header(out, 0);
        return;
    }
    if (!justid) {
        target = obj_stream_consumer_create(g, consumer, cl);
        if (target == NULL) {
            storage_length_error(out);
            return;
        }
    } else {
        target = NULL;
    }

    /* First pass: count eligible entries and pick the resume cursor after
     * COUNT claims. The cursor is the next PEL entry seen after the last
     * claimed entry, or 0-0 when the whole PEL has been scanned. */
    for (i = 0; i < g->nconsumers; i++) {
        stream_consumer *c = &g->consumers[i];
        for (j = 0; j < c->pel_len; j++) {
            stream_pending *p = &c->pel[j];
            if (!stream_id_gt(p->ms, p->seq, start_ms, start_seq))
                continue;
            if (eligible_count < count) {
                if ((p->idle > now_ms ? 0 : now_ms - p->idle) >= min_idle)
                    eligible_count++;
                continue;
            }
            next_ms = p->ms;
            next_seq = p->seq;
            goto cursor_done;
        }
    }
cursor_done:

    before = obj_stream_mem(st);
    resp_write_array_header(out, 2);
    stream_write_id(out, next_ms, next_seq);
    resp_write_array_header(out, eligible_count);
    to_emit = eligible_count;

    if (justid) {
        for (i = 0; i < g->nconsumers && to_emit > 0; i++) {
            stream_consumer *c = &g->consumers[i];
            for (j = 0; j < c->pel_len && to_emit > 0; j++) {
                stream_pending *p = &c->pel[j];
                if (!stream_id_gt(p->ms, p->seq, start_ms, start_seq))
                    continue;
                if ((p->idle > now_ms ? 0 : now_ms - p->idle) < min_idle)
                    continue;
                stream_write_id(out, p->ms, p->seq);
                to_emit--;
            }
        }
    } else {
        i = 0;
        while (i < g->nconsumers && to_emit > 0) {
            stream_consumer *c = &g->consumers[i];
            j = 0;
            while (j < c->pel_len && to_emit > 0) {
                stream_pending *p = &c->pel[j];
                if (!stream_id_gt(p->ms, p->seq, start_ms, start_seq) ||
                    (p->idle > now_ms ? 0 : now_ms - p->idle) < min_idle) {
                    j++;
                    continue;
                }
                {
                    uint64_t delivery = p->delivery_count + 1;
                    size_t eidx;
                    uint64_t p_ms = p->ms;
                    uint64_t p_seq = p->seq;
                    (void)obj_stream_group_pel_remove(g, p_ms, p_seq);
                    if (obj_stream_consumer_pel_add(g, target, p_ms, p_seq,
                                                    now_ms, delivery) == NULL) {
                        storage_length_error(out);
                        return;
                    }
                    target->seen_time = now_ms;
                    if (stream_entry_index(st, p_ms, p_seq, &eidx))
                        stream_emit_entry(out, obj_stream_at(st, eidx));
                    else
                        resp_write_bulk(out, NULL, 0);
                }
                to_emit--;
            }
            i++;
        }
    }
    mem_sync(d, key, kl, before, obj_stream_mem(st));
    return;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xnack(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *group, *mode;
    size_t kl, gl, ml;
    obj_stream *st;
    stream_group *g;
    uint64_t before;
    uint64_t *ids = NULL;
    size_t ids_start = 0, numids = 0, i;
    int mode_id = -1;
    int force = 0;
    long long retrycount = -1;
    int retry_set = 0;
    long long nacked = 0;
    int rc;

    if (argc < 7) {
        wrong_args(out, "xnack");
        return;
    }
    if (!arg_str(&argv[1], &key, &kl) ||
        !arg_str(&argv[2], &group, &gl) ||
        !arg_str(&argv[3], &mode, &ml))
        goto bad_type;
    if (ci_equal(mode, ml, "SILENT")) {
        mode_id = 0;
    } else if (ci_equal(mode, ml, "FAIL")) {
        mode_id = 1;
    } else if (ci_equal(mode, ml, "FATAL")) {
        mode_id = 2;
    } else {
        static const char E[] = "ERR mode must be SILENT, FAIL, or FATAL";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    for (i = 4; i < argc; i++) {
        const char *tok;
        size_t tl;
        if (!arg_str(&argv[i], &tok, &tl))
            goto bad_type;
        if (ci_equal(tok, tl, "IDS") && i + 1 < argc) {
            const char *nv;
            size_t nvl;
            long long n;
            if (!arg_str(&argv[i + 1], &nv, &nvl) ||
                !parse_i64(nv, nvl, &n) || n <= 0) {
                static const char E[] =
                    "ERR numids must be a positive integer";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            ids_start = i + 2;
            numids = (size_t)n;
            if (numids > argc - ids_start) {
                static const char E[] =
                    "ERR number of IDs doesn't match numids";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            i = ids_start + numids - 1;
        } else if (ci_equal(tok, tl, "FORCE")) {
            force = 1;
        } else if (ci_equal(tok, tl, "RETRYCOUNT") && i + 1 < argc) {
            const char *nv;
            size_t nvl;
            i++;
            if (!arg_str(&argv[i], &nv, &nvl) ||
                !parse_i64(nv, nvl, &retrycount) || retrycount < 0) {
                static const char E[] =
                    "ERR Invalid RETRYCOUNT value, must be >= 0";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            retry_set = 1;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
    }
    if (ids_start == 0) {
        static const char E[] = "ERR syntax error, expected IDS keyword";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    ids = (uint64_t *)malloc(numids * 2 * sizeof(*ids));
    if (ids == NULL) {
        resp_write_error(out, OOM_MSG, sizeof(OOM_MSG) - 1);
        return;
    }
    for (i = 0; i < numids; i++) {
        const char *idv;
        size_t idl;
        if (!arg_str(&argv[ids_start + i], &idv, &idl) ||
            !stream_parse_full_id(idv, idl, &ids[2 * i], &ids[2 * i + 1])) {
            static const char E[] =
                "ERR Invalid stream ID specified as stream command argument";
            resp_write_error(out, E, sizeof(E) - 1);
            free(ids);
            return;
        }
    }

    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0) {
        free(ids);
        return;
    }
    if (rc == 0 || (g = obj_stream_group_get(st, group, gl)) == NULL) {
        static const char E[] = "NOGROUP No such consumer group";
        resp_write_error(out, E, sizeof(E) - 1);
        free(ids);
        return;
    }

    before = obj_stream_mem(st);
    for (i = 0; i < numids; i++) {
        uint64_t ms = ids[2 * i];
        uint64_t seq = ids[2 * i + 1];
        stream_pending *p = stream_group_pel_find(g, ms, seq, NULL);
        if (p != NULL) {
            if (retry_set) {
                p->delivery_count = (uint64_t)retrycount;
            } else if (mode_id == 0) {
                if (p->delivery_count > 0)
                    p->delivery_count--;
            } else if (mode_id == 2) {
                p->delivery_count = (uint64_t)INT64_MAX;
            }
            p->idle = 0;
            nacked++;
            continue;
        }
        if (force) {
            size_t eidx;
            stream_consumer *c;
            uint64_t delivery = 0;
            if (!stream_entry_index(st, ms, seq, &eidx))
                continue;
            c = obj_stream_consumer_create(g, "", 1);
            if (c == NULL) {
                storage_length_error(out);
                free(ids);
                return;
            }
            if (retry_set) {
                delivery = (uint64_t)retrycount;
            } else if (mode_id == 2) {
                delivery = (uint64_t)INT64_MAX;
            }
            if (obj_stream_consumer_pel_add(g, c, ms, seq, 0, delivery) ==
                NULL) {
                storage_length_error(out);
                free(ids);
                return;
            }
            nacked++;
        }
    }
    if (nacked > 0)
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    resp_write_integer(out, nacked);
    free(ids);
    return;

bad_type:
    free(ids);
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xread(session *s, const resp_value *argv, size_t argc,
                          resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    size_t pos = 1, nkeys, ids_start, k;
    long long count = -1;
    int have_streams = 0;

    if (argc < 4) {
        wrong_args(out, "xread");
        return;
    }
    while (pos < argc) {
        const char *tok, *val;
        size_t tl, vl;
        if (!arg_str(&argv[pos], &tok, &tl))
            goto bad_type;
        if (ci_equal(tok, tl, "STREAMS")) {
            have_streams = 1;
            pos++;
            break;
        }
        if (ci_equal(tok, tl, "COUNT")) {
            if (pos + 1 >= argc || !arg_str(&argv[pos + 1], &val, &vl) ||
                !parse_i64(val, vl, &count) || count <= 0)
                goto syntax;
            pos += 2;
        } else if (ci_equal(tok, tl, "BLOCK")) {
            uint64_t block;
            if (pos + 1 >= argc || !arg_str(&argv[pos + 1], &val, &vl) ||
                !parse_u64(val, vl, &block))
                goto syntax;
            pos += 2;
        } else {
            goto syntax;
        }
    }
    if (!have_streams || pos >= argc || ((argc - pos) & 1u) != 0)
        goto syntax;
    nkeys = (argc - pos) / 2;
    ids_start = pos + nkeys;

    for (k = 0; k < nkeys; k++) {
        const char *idv;
        size_t idl;
        uint64_t ms, seq;
        if (!arg_str(&argv[pos + k], &idv, &idl) ||
            !arg_str(&argv[ids_start + k], &idv, &idl))
            goto bad_type;
        if (!ci_equal(idv, idl, "$") &&
            !stream_parse_full_id(idv, idl, &ms, &seq))
            goto syntax;
    }

    resp_write_array_header(out, nkeys);
    for (k = 0; k < nkeys; k++) {
        const char *key, *idv;
        size_t kl, idl;
        obj_stream *st;
        uint64_t ms, seq, emitted = 0;
        size_t first, i;
        int rc;
        if (!arg_str(&argv[pos + k], &key, &kl) ||
            !arg_str(&argv[ids_start + k], &idv, &idl))
            goto bad_type;
        rc = get_stream(d, out, key, kl, 0, now_ms, &st);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_bulk(out, NULL, 0);
            continue;
        }
        if (ci_equal(idv, idl, "$")) {
            ms = st->last_ms;
            seq = st->last_seq;
        } else {
            (void)stream_parse_full_id(idv, idl, &ms, &seq);
        }
        first = obj_stream_lower_bound(st, ms, seq);
        while (first < obj_stream_len(st)) {
            const stream_entry *e = obj_stream_at(st, first);
            if (stream_id_gt(e->ms, e->seq, ms, seq))
                break;
            first++;
        }
        for (i = first; i < obj_stream_len(st); i++) {
            if (count >= 0 && emitted >= (uint64_t)count)
                break;
            emitted++;
        }
        resp_write_array_header(out, emitted);
        for (i = 0; i < emitted; i++)
            stream_emit_entry(out, obj_stream_at(st, first + i));
    }
    return;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xreadgroup(session *s, const resp_value *argv, size_t argc,
                               resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *group = NULL, *consumer = NULL;
    size_t gl = 0, cl = 0, pos, nkeys, ids_start, k;
    long long count = -1;
    int noack = 0, have_streams = 0;

    if (argc < 7) {
        wrong_args(out, "xreadgroup");
        return;
    }
    pos = 1;
    while (pos < argc) {
        const char *tok, *val;
        size_t tl, vl;
        if (!arg_str(&argv[pos], &tok, &tl))
            goto bad_type;
        if (ci_equal(tok, tl, "GROUP")) {
            if (pos + 2 >= argc)
                goto syntax;
            if (!arg_str(&argv[pos + 1], &group, &gl) ||
                !arg_str(&argv[pos + 2], &consumer, &cl))
                goto bad_type;
            pos += 3;
        } else if (ci_equal(tok, tl, "COUNT")) {
            if (pos + 1 >= argc || !arg_str(&argv[pos + 1], &val, &vl) ||
                !parse_i64(val, vl, &count) || count <= 0)
                goto syntax;
            pos += 2;
        } else if (ci_equal(tok, tl, "BLOCK")) {
            uint64_t block;
            if (pos + 1 >= argc || !arg_str(&argv[pos + 1], &val, &vl) ||
                !parse_u64(val, vl, &block))
                goto syntax;
            pos += 2;
        } else if (ci_equal(tok, tl, "NOACK")) {
            noack = 1;
            pos++;
        } else if (ci_equal(tok, tl, "STREAMS")) {
            have_streams = 1;
            pos++;
            break;
        } else {
            goto syntax;
        }
    }
    if (!have_streams || group == NULL || consumer == NULL ||
        pos >= argc || ((argc - pos) & 1u) != 0)
        goto syntax;
    nkeys = (argc - pos) / 2;
    ids_start = pos + nkeys;

    /* Validate ids/groups before writing the outer array. */
    for (k = 0; k < nkeys; k++) {
        const char *key, *idv;
        size_t kl, idl;
        obj_stream *st;
        stream_group *g;
        int rc;
        if (!arg_str(&argv[pos + k], &key, &kl) ||
            !arg_str(&argv[ids_start + k], &idv, &idl))
            goto bad_type;
        if (!ci_equal(idv, idl, ">") &&
            !stream_parse_full_id(idv, idl, &(uint64_t){0},
                                  &(uint64_t){0}))
            goto syntax;
        rc = get_stream(d, out, key, kl, 0, now_ms, &st);
        if (rc < 0)
            return;
        if (rc == 0 || (g = obj_stream_group_get(st, group, gl)) == NULL) {
            static const char E[] = "NOGROUP No such consumer group";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (ci_equal(idv, idl, ">") &&
            obj_stream_consumer_create(g, consumer, cl) == NULL) {
            storage_length_error(out);
            return;
        }
    }

    resp_write_array_header(out, nkeys);
    for (k = 0; k < nkeys; k++) {
        const char *key, *idv;
        size_t kl, idl;
        obj_stream *st;
        stream_group *g;
        uint64_t before;
        uint64_t ms, seq, emitted = 0, last_ms, last_seq;
        size_t first, i;
        int is_new;
        int rc;
        if (!arg_str(&argv[pos + k], &key, &kl) ||
            !arg_str(&argv[ids_start + k], &idv, &idl))
            goto bad_type;
        rc = get_stream(d, out, key, kl, 0, now_ms, &st);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_bulk(out, NULL, 0);
            continue;
        }
        g = obj_stream_group_get(st, group, gl);
        if (g == NULL) {
            static const char E[] = "NOGROUP No such consumer group";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        is_new = ci_equal(idv, idl, ">");
        if (is_new) {
            ms = g->last_ms;
            seq = g->last_seq;
        } else {
            (void)stream_parse_full_id(idv, idl, &ms, &seq);
        }
        first = obj_stream_lower_bound(st, ms, seq);
        while (first < obj_stream_len(st)) {
            const stream_entry *e = obj_stream_at(st, first);
            if (stream_id_gt(e->ms, e->seq, ms, seq))
                break;
            first++;
        }
        for (i = first; i < obj_stream_len(st); i++) {
            if (count >= 0 && emitted >= (uint64_t)count)
                break;
            emitted++;
        }
        before = obj_stream_mem(st);
        resp_write_array_header(out, emitted);
        last_ms = ms;
        last_seq = seq;
        for (i = 0; i < emitted; i++) {
            const stream_entry *e = obj_stream_at(st, first + i);
            if (is_new && !noack) {
                stream_consumer *c = obj_stream_consumer_get(g, consumer, cl);
                if (c == NULL) {
                    storage_length_error(out);
                    return;
                }
                if (obj_stream_consumer_pel_add(g, c, e->ms, e->seq, now_ms,
                                                1) == NULL) {
                    storage_length_error(out);
                    return;
                }
            }
            stream_emit_entry(out, e);
            last_ms = e->ms;
            last_seq = e->seq;
        }
        if (is_new && emitted > 0) {
            stream_consumer *c = obj_stream_consumer_get(g, consumer, cl);
            if (c != NULL)
                c->seen_time = now_ms;
            g->last_ms = last_ms;
            g->last_seq = last_seq;
            g->entries_read += emitted;
        }
        mem_sync(d, key, kl, before, obj_stream_mem(st));
    }
    return;

syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_xinfo(session *s, const resp_value *argv, size_t argc,
                          resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *key, *sub;
    size_t kl, subl;
    obj_stream *st;
    int rc;
    if (argc < 2) {
        wrong_args(out, "xinfo");
        return;
    }
    if (!arg_str(&argv[1], &sub, &subl))
        goto bad_type;

    if (ci_equal(sub, subl, "HELP")) {
        static const char *help[] = {
            "STREAM <key>",
            "GROUPS <key>",
            "CONSUMERS <key> <group>"
        };
        size_t i;
        if (argc != 2) {
            wrong_args(out, "xinfo|HELP");
            return;
        }
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }

    if (argc < 3) {
        wrong_args(out, "xinfo");
        return;
    }
    if (!arg_str(&argv[2], &key, &kl))
        goto bad_type;

    rc = get_stream(d, out, key, kl, 0, now_ms, &st);
    if (rc < 0)
        return;
    if (rc == 0) {
        resp_write_error(out, "ERR no such key", 15);
        return;
    }

    if (ci_equal(sub, subl, "STREAM")) {
        if (argc != 3) {
            wrong_args(out, "xinfo|STREAM");
            return;
        }
        resp_write_array_header(out, 16);
        resp_write_bulk(out, "length", 6);
        resp_write_integer(out, (long long)obj_stream_len(st));
        resp_write_bulk(out, "last-generated-id", 17);
        stream_write_id(out, st->last_ms, st->last_seq);
        resp_write_bulk(out, "entries-added", 13);
        resp_write_integer(out, (long long)st->entries_added);
        resp_write_bulk(out, "max-deleted-entry-id", 20);
        stream_write_id(out, st->max_deleted_ms, st->max_deleted_seq);
        resp_write_bulk(out, "entries", 7);
        resp_write_integer(out, (long long)obj_stream_len(st));
        resp_write_bulk(out, "groups", 6);
        resp_write_integer(out, (long long)st->ngroups);
        resp_write_bulk(out, "first-entry", 11);
        if (obj_stream_len(st) > 0)
            stream_emit_entry(out, obj_stream_at(st, 0));
        else
            resp_write_bulk(out, NULL, 0);
        resp_write_bulk(out, "last-entry", 10);
        if (obj_stream_len(st) > 0)
            stream_emit_entry(out, obj_stream_at(st, obj_stream_len(st) - 1));
        else
            resp_write_bulk(out, NULL, 0);
        return;
    }

    if (ci_equal(sub, subl, "GROUPS")) {
        size_t i;
        if (argc != 3) {
            wrong_args(out, "xinfo|GROUPS");
            return;
        }
        resp_write_array_header(out, st->ngroups);
        for (i = 0; i < st->ngroups; i++) {
            stream_group *g = &st->groups[i];
            resp_write_array_header(out, 12);
            resp_write_bulk(out, "name", 4);
            resp_write_bulk(out, g->name, g->name_len);
            resp_write_bulk(out, "consumers", 9);
            resp_write_integer(out, (long long)g->nconsumers);
            resp_write_bulk(out, "pending", 7);
            resp_write_integer(out, (long long)obj_stream_group_pending_count(g));
            resp_write_bulk(out, "last-delivered-id", 17);
            stream_write_id(out, g->last_ms, g->last_seq);
            resp_write_bulk(out, "entries-read", 12);
            resp_write_integer(out, (long long)g->entries_read);
            resp_write_bulk(out, "lag", 3);
            resp_write_integer(out, 0);
        }
        return;
    }

    if (ci_equal(sub, subl, "CONSUMERS")) {
        const char *group;
        size_t glen;
        stream_group *g;
        size_t i;
        if (argc != 4) {
            wrong_args(out, "xinfo|CONSUMERS");
            return;
        }
        if (!arg_str(&argv[3], &group, &glen))
            goto bad_type;
        g = obj_stream_group_get(st, group, glen);
        if (g == NULL) {
            static const char E[] = "NOGROUP No such consumer group";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        resp_write_array_header(out, g->nconsumers);
        for (i = 0; i < g->nconsumers; i++) {
            stream_consumer *c = &g->consumers[i];
            resp_write_array_header(out, 8);
            resp_write_bulk(out, "name", 4);
            resp_write_bulk(out, c->name, c->name_len);
            resp_write_bulk(out, "pending", 7);
            resp_write_integer(out, (long long)c->pel_len);
            resp_write_bulk(out, "idle", 4);
            resp_write_integer(out,
                (long long)(c->seen_time > now_ms ? 0 : now_ms - c->seen_time));
            resp_write_bulk(out, "inactive", 8);
            resp_write_integer(out,
                (long long)(c->active_time > now_ms ? 0
                                                   : now_ms - c->active_time));
        }
        return;
    }

    goto syntax;
syntax:
    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
    return;
bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}



static void blocking_timeout_reply(resp_buf *out, uint16_t cmd_id)
{
    if (cmd_id == CMD_BLPOP || cmd_id == CMD_BRPOP ||
        cmd_id == CMD_BRPOPLPUSH || cmd_id == CMD_BLMOVE)
        resp_write_bulk(out, NULL, 0);
    else
        write_null_array(out);
}

static int blocking_parse_timeout(const resp_value *v, uint64_t now_ms,
                                  uint64_t *deadline_ms, resp_buf *out)
{
    const char *s;
    size_t len;
    double sec;
    long double ms;
    uint64_t ums;
    if (!arg_str(v, &s, &len) || !parse_double(s, len, &sec) ||
        isinf(sec)) {
        static const char E[] =
            "ERR timeout is not a float or out of range";
        resp_write_error(out, E, sizeof(E) - 1);
        return -1;
    }
    if (sec < 0) {
        static const char E[] = "ERR timeout is negative";
        resp_write_error(out, E, sizeof(E) - 1);
        return -1;
    }
    if (sec == 0.0) {
        *deadline_ms = 0; /* block forever */
        return 0;
    }
    ms = (long double)sec * 1000.0L;
    if (ms >= (long double)UINT64_MAX) {
        *deadline_ms = UINT64_MAX;
        return 0;
    }
    ums = (uint64_t)ms;
    *deadline_ms =
        ums > UINT64_MAX - now_ms ? UINT64_MAX : now_ms + ums;
    return 0;
}

/* Find the first non-empty list among argv[start..end); WRONGTYPE on any
 * existing non-list is reported immediately (Redis semantics). Returns 1
 * and sets key/kl/l on success, 0 when every list is missing/empty, -1
 * when a reply was already written. */
static int blocking_first_list(db *d, resp_buf *out,
                               const resp_value *argv, size_t start,
                               size_t end, uint64_t now_ms, const char **key,
                               size_t *klen, obj_list **l)
{
    size_t i;
    for (i = start; i < end; i++) {
        const char *k;
        size_t kl;
        obj_list *cur;
        int rc;
        if (!arg_str(&argv[i], &k, &kl)) {
            resp_write_error(out, "ERR invalid argument type", 24);
            return -1;
        }
        rc = get_list(d, out, k, kl, 0, now_ms, &cur);
        if (rc < 0)
            return -1;
        if (rc == 1 && obj_list_len(cur) > 0) {
            *key = k;
            *klen = kl;
            *l = cur;
            return 1;
        }
    }
    return 0;
}

/* Same as blocking_first_list, for sorted sets. */
static int blocking_first_zset(db *d, resp_buf *out,
                               const resp_value *argv, size_t start,
                               size_t end, uint64_t now_ms, const char **key,
                               size_t *klen, obj_zset **z)
{
    size_t i;
    for (i = start; i < end; i++) {
        const char *k;
        size_t kl;
        obj_zset *cur;
        int rc;
        if (!arg_str(&argv[i], &k, &kl)) {
            resp_write_error(out, "ERR invalid argument type", 24);
            return -1;
        }
        rc = get_zset(d, out, k, kl, 0, now_ms, &cur);
        if (rc < 0)
            return -1;
        if (rc == 1 && obj_zset_len(cur) > 0) {
            *key = k;
            *klen = kl;
            *z = cur;
            return 1;
        }
    }
    return 0;
}

/* Execute a ready LMOVE/BRPOPLPUSH move after the source has been checked
 * non-empty. Dest type is validated before the source is mutated. */
static void blocking_list_move_ready(db *d, resp_buf *out, const char *sk,
                                     size_t skl, const char *dk, size_t dkl,
                                     int src_left, int dst_left,
                                     uint64_t now_ms)
{
    obj_list *src;
    obj_list *dst;
    int same = skl == dkl && memcmp(sk, dk, skl) == 0;
    int created_dst = 0;
    int rcs = get_list(d, out, sk, skl, 0, now_ms, &src);
    if (rcs < 0)
        return;
    if (!same) {
        int rcd = get_list(d, out, dk, dkl, 0, now_ms, &dst);
        if (rcd < 0)
            return;
        if (rcd == 0) {
            if (oom_blocked(d, out))
                return;
            rcd = get_list(d, out, dk, dkl, 1, now_ms, &dst);
            if (rcd < 0)
                return;
            created_dst = 1;
        }
    } else {
        dst = src;
    }
    {
        char *data = NULL;
        size_t dlen = 0;
        uint64_t sbefore = obj_list_mem(src);
        if (!obj_list_pop(src, src_left, &data, &dlen)) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        {
            uint64_t dbefore = same ? 0 : obj_list_mem(dst);
            if (obj_list_push(dst, dst_left, data, dlen) != 0) {
                if (created_dst)
                    db_del_kv(d, dk, dkl);
                (void)obj_list_push(src, src_left, data, dlen);
                free(data);
                storage_length_error(out);
                return;
            }
            if (same) {
                mem_sync(d, sk, skl, sbefore, obj_list_mem(src));
            } else {
                mem_sync(d, dk, dkl, dbefore, obj_list_mem(dst));
                mem_sync(d, sk, skl, sbefore, obj_list_mem(src));
            }
        }
        if (obj_list_len(src) == 0)
            db_del_kv(d, sk, skl);
        resp_write_bulk(out, data, dlen);
        free(data);
    }
}

/* ------------------------------------------------------------------ */
/* LMOVEM/BLMOVEM multi-element list moves                              */
/* ------------------------------------------------------------------ */

#define LMOVEM_MODE_DEFAULT 0
#define LMOVEM_MODE_UPTO    1
#define LMOVEM_MODE_EXACTLY 2
#define LMOVEM_ORDER_OBO    0
#define LMOVEM_ORDER_BULK   1

/* Parse the optional trailer at argv[opt_idx]: [<COUNT|EXACTLY> n <OBO|BULK>].
 * On success returns 0 and fills the three output parameters. On failure a
 * reply is written and -1 is returned. */
static int lmovem_parse_options(const resp_value *argv, size_t argc,
                                size_t opt_idx, int *mode, long long *count,
                                int *ordering, resp_buf *out)
{
    const char *sel;
    size_t sel_len;
    const char *ord;
    size_t ord_len;
    if (opt_idx == argc) {
        *mode = LMOVEM_MODE_DEFAULT;
        *count = 1;
        *ordering = LMOVEM_ORDER_BULK;
        return 0;
    }
    if (argc - opt_idx != 3) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return -1;
    }
    if (!arg_str(&argv[opt_idx], &sel, &sel_len))
        goto lmovem_bad_arg;
    if (ci_equal(sel, sel_len, "COUNT")) {
        *mode = LMOVEM_MODE_UPTO;
    } else if (ci_equal(sel, sel_len, "EXACTLY")) {
        *mode = LMOVEM_MODE_EXACTLY;
    } else {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return -1;
    }
    if (cmd_parse_ll(&argv[opt_idx + 1], count) == 0 || *count <= 0) {
        static const char E[] = "ERR count should be greater than 0";
        resp_write_error(out, E, sizeof(E) - 1);
        return -1;
    }
    if (!arg_str(&argv[opt_idx + 2], &ord, &ord_len))
        goto lmovem_bad_arg;
    if (ci_equal(ord, ord_len, "OBO")) {
        *ordering = LMOVEM_ORDER_OBO;
    } else if (ci_equal(ord, ord_len, "BULK")) {
        *ordering = LMOVEM_ORDER_BULK;
    } else {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return -1;
    }
    return 0;

lmovem_bad_arg:
    resp_write_error(out, "ERR invalid argument type", 24);
    return -1;
}

/* Pop tomove elements from src and push them into dst, replying with the
 * moved elements in destination order. The caller has already verified that
 * src is a list with at least tomove elements. Destination type is validated
 * before any source mutation. */
static void lmovem_move_ready(db *d, resp_buf *out, const char *sk,
                              size_t skl, const char *dk, size_t dkl,
                              int src_left, int dst_left, long long tomove,
                              int ordering, uint64_t now_ms)
{
    obj_list *src;
    obj_list *dst = NULL;
    char **vals = NULL;
    size_t *lens = NULL;
    size_t n = (size_t)tomove;
    size_t i;
    int same;
    uint64_t sbefore;
    uint64_t dbefore = 0;
    int rev;

    if (get_list(d, out, sk, skl, 0, now_ms, &src) < 0)
        return;
    same = skl == dkl && memcmp(sk, dk, skl) == 0;
    if (!same) {
        int rc = get_list(d, out, dk, dkl, 0, now_ms, &dst);
        if (rc < 0)
            return;
        if (rc == 0) {
            if (oom_blocked(d, out))
                return;
            rc = get_list(d, out, dk, dkl, 1, now_ms, &dst);
            if (rc < 0)
                return;
        }
    } else {
        dst = src;
    }

    /* Prevalidate destination push lengths before mutating the source. */
    {
        obj_list_iter it;
        int valid = src_left ? obj_list_first(src, &it)
                             : obj_list_last(src, &it);
        for (i = 0; i < n && valid; i++) {
            size_t el = 0;
            (void)obj_list_iter_value(&it, &el);
            if (el > UINT32_MAX) {
                storage_length_error(out);
                return;
            }
            valid = src_left ? obj_list_iter_next(&it)
                             : obj_list_iter_prev(&it);
        }
    }

    vals = (char **)malloc(n * sizeof(*vals));
    lens = (size_t *)malloc(n * sizeof(*lens));
    if (vals == NULL || lens == NULL) {
        free(vals);
        free(lens);
        resp_write_error(out, OOM_MSG, sizeof(OOM_MSG) - 1);
        return;
    }

    sbefore = obj_list_mem(src);
    if (!same)
        dbefore = obj_list_mem(dst);
    for (i = 0; i < n; i++) {
        if (!obj_list_pop(src, src_left, &vals[i], &lens[i])) {
            storage_length_error(out);
            goto lmovem_cleanup_vals;
        }
    }

    rev = (ordering == LMOVEM_ORDER_OBO) ? dst_left : !src_left;
    resp_write_array_header(out, n);
    for (i = 0; i < n; i++) {
        size_t di = rev ? n - 1 - i : i;
        resp_write_bulk(out, vals[di], lens[di]);
    }

    if (dst_left) {
        for (i = 0; i < n; i++) {
            size_t si = rev ? i : n - 1 - i;
            if (obj_list_push(dst, 1, vals[si], lens[si]) != 0) {
                storage_length_error(out);
                goto lmovem_cleanup_vals;
            }
        }
    } else {
        for (i = 0; i < n; i++) {
            size_t si = rev ? n - 1 - i : i;
            if (obj_list_push(dst, 0, vals[si], lens[si]) != 0) {
                storage_length_error(out);
                goto lmovem_cleanup_vals;
            }
        }
    }

    if (same) {
        mem_sync(d, sk, skl, sbefore, obj_list_mem(src));
    } else {
        mem_sync(d, dk, dkl, dbefore, obj_list_mem(dst));
        mem_sync(d, sk, skl, sbefore, obj_list_mem(src));
    }

    if (!same && obj_list_len(src) == 0)
        db_del_kv(d, sk, skl);

lmovem_cleanup_vals:
    for (i = 0; i < n; i++) {
        if (vals[i] != NULL)
            free(vals[i]);
    }
    free(vals);
    free(lens);
}

/* Non-blocking LMOVEM. It validates source sufficiency before destination
 * type, matching Redis: EXACTLY failures do not touch either key. */
static void lmovem_execute(db *d, resp_buf *out, const resp_value *argv,
                           size_t argc, uint64_t now_ms)
{
    const char *sk, *dk, *sw, *dw;
    size_t skl, dkl, swl, dwl;
    int src_left, dst_left;
    int mode;
    long long count;
    int ordering;
    obj_list *src;
    int rc;
    long long srclen;
    long long tomove;

    if (argc < 5) {
        wrong_args(out, "lmovem");
        return;
    }
    if (!arg_str(&argv[1], &sk, &skl) ||
        !arg_str(&argv[2], &dk, &dkl) ||
        !arg_str(&argv[3], &sw, &swl) ||
        !arg_str(&argv[4], &dw, &dwl))
        goto lmovem_bad_type;
    if (ci_equal(sw, swl, "LEFT")) {
        src_left = 1;
    } else if (ci_equal(sw, swl, "RIGHT")) {
        src_left = 0;
    } else {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    if (ci_equal(dw, dwl, "LEFT")) {
        dst_left = 1;
    } else if (ci_equal(dw, dwl, "RIGHT")) {
        dst_left = 0;
    } else {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    if (!storage_key_ok(skl) || !storage_key_ok(dkl)) {
        storage_length_error(out);
        return;
    }
    if (lmovem_parse_options(argv, argc, 5, &mode, &count, &ordering,
                             out) != 0)
        return;

    rc = get_list(d, out, sk, skl, 0, now_ms, &src);
    if (rc < 0)
        return;
    srclen = rc == 0 ? 0 : (long long)obj_list_len(src);

    if (mode == LMOVEM_MODE_EXACTLY) {
        if (srclen < count) {
            write_null_array(out);
            return;
        }
        tomove = count;
    } else {
        tomove = count < srclen ? count : srclen;
    }
    if (tomove == 0) {
        write_null_array(out);
        return;
    }
    lmovem_move_ready(d, out, sk, skl, dk, dkl, src_left, dst_left,
                      tomove, ordering, now_ms);
    return;

lmovem_bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

/* The blocking-command executor shared by first dispatch and server retry.
 * Returns 1 when a reply (or error) has been written and the session should
 * be unblocked; 0 when no key is ready and the session must block. */
static int blocking_pop_try(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out, uint64_t now_ms,
                            uint64_t *deadline_ms)
{
    db *d = s->d;
    const char *name;
    size_t nlen;
    uint16_t cmd_id;
    uint64_t deadline;
    if (argc == 0 || !arg_str(&argv[0], &name, &nlen))
        return 1;
    cmd_id = cmd_resolve(name, nlen);

    if (cmd_id == CMD_BLPOP || cmd_id == CMD_BRPOP) {
        int left = cmd_id == CMD_BLPOP;
        const char *k;
        size_t kl;
        obj_list *l;
        int rc;
        if (argc < 3) {
            wrong_args(out, left ? "blpop" : "brpop");
            return 1;
        }
        if (blocking_parse_timeout(&argv[argc - 1], now_ms, &deadline,
                                   out) != 0)
            return 1;
        rc = blocking_first_list(d, out, argv, 1, argc - 1, now_ms, &k,
                                 &kl, &l);
        if (rc < 0)
            return 1;
        if (rc == 0) {
            *deadline_ms = deadline;
            return 0;
        }
        {
            char *data = NULL;
            size_t dlen = 0;
            uint64_t before = obj_list_mem(l);
            if (!obj_list_pop(l, left, &data, &dlen)) {
                resp_write_bulk(out, NULL, 0);
                return 1;
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
            if (obj_list_len(l) == 0)
                db_del_kv(d, k, kl);
            resp_write_array_header(out, 2);
            resp_write_bulk(out, k, kl);
            resp_write_bulk(out, data, dlen);
            free(data);
        }
        return 1;
    }

    if (cmd_id == CMD_BRPOPLPUSH || cmd_id == CMD_BLMOVE) {
        const char *sk, *dk, *sw, *dw;
        size_t skl, dkl, swl, dwl;
        int src_left, dst_left;
        obj_list *src;
        int rcs;
        if (cmd_id == CMD_BRPOPLPUSH) {
            if (argc != 4) {
                wrong_args(out, "brpoplpush");
                return 1;
            }
            if (!arg_str(&argv[1], &sk, &skl) ||
                !arg_str(&argv[2], &dk, &dkl))
                goto bad_blocking_arg;
            src_left = 0;
            dst_left = 1;
        } else {
            if (argc != 6) {
                wrong_args(out, "blmove");
                return 1;
            }
            if (!arg_str(&argv[1], &sk, &skl) ||
                !arg_str(&argv[2], &dk, &dkl) ||
                !arg_str(&argv[3], &sw, &swl) ||
                !arg_str(&argv[4], &dw, &dwl))
                goto bad_blocking_arg;
            if (ci_equal(sw, swl, "LEFT")) {
                src_left = 1;
            } else if (ci_equal(sw, swl, "RIGHT")) {
                src_left = 0;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return 1;
            }
            if (ci_equal(dw, dwl, "LEFT")) {
                dst_left = 1;
            } else if (ci_equal(dw, dwl, "RIGHT")) {
                dst_left = 0;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return 1;
            }
        }
        if (!storage_key_ok(skl) || !storage_key_ok(dkl)) {
            storage_length_error(out);
            return 1;
        }
        if (blocking_parse_timeout(&argv[argc - 1], now_ms, &deadline,
                                   out) != 0)
            return 1;
        rcs = get_list(d, out, sk, skl, 0, now_ms, &src);
        if (rcs < 0)
            return 1;
        if (rcs == 0 || obj_list_len(src) == 0) {
            *deadline_ms = deadline;
            return 0;
        }
        blocking_list_move_ready(d, out, sk, skl, dk, dkl, src_left,
                                 dst_left, now_ms);
        return 1;
    }

    if (cmd_id == CMD_BLMOVEM) {
        const char *sk, *dk, *sw, *dw;
        size_t skl, dkl, swl, dwl;
        int src_left, dst_left;
        int mode;
        long long count;
        int ordering;
        obj_list *src;
        int rc;
        uint64_t srclen;
        uint64_t needed;
        long long tomove;

        if (argc < 6) {
            wrong_args(out, "blmovem");
            return 1;
        }
        if (!arg_str(&argv[1], &sk, &skl) ||
            !arg_str(&argv[2], &dk, &dkl) ||
            !arg_str(&argv[3], &sw, &swl) ||
            !arg_str(&argv[4], &dw, &dwl))
            goto bad_blocking_arg;
        if (ci_equal(sw, swl, "LEFT")) {
            src_left = 1;
        } else if (ci_equal(sw, swl, "RIGHT")) {
            src_left = 0;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return 1;
        }
        if (ci_equal(dw, dwl, "LEFT")) {
            dst_left = 1;
        } else if (ci_equal(dw, dwl, "RIGHT")) {
            dst_left = 0;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return 1;
        }
        if (!storage_key_ok(skl) || !storage_key_ok(dkl)) {
            storage_length_error(out);
            return 1;
        }
        if (blocking_parse_timeout(&argv[5], now_ms, &deadline, out) != 0)
            return 1;
        if (lmovem_parse_options(argv, argc, 6, &mode, &count, &ordering,
                                 out) != 0)
            return 1;

        rc = get_list(d, out, sk, skl, 0, now_ms, &src);
        if (rc < 0)
            return 1;
        srclen = rc == 0 ? 0 : obj_list_len(src);
        needed = mode == LMOVEM_MODE_EXACTLY ? (uint64_t)count : 1;
        if (srclen < needed) {
            *deadline_ms = deadline;
            return 0;
        }
        tomove = mode == LMOVEM_MODE_EXACTLY
                     ? count
                     : (srclen < (uint64_t)count ? (long long)srclen : count);
        lmovem_move_ready(d, out, sk, skl, dk, dkl, src_left, dst_left,
                          tomove, ordering, now_ms);
        return 1;
    }

    if (cmd_id == CMD_BLMPOP) {
        long long nk;
        long long count = 1;
        size_t dir_idx;
        const char *where;
        size_t wl;
        int left;
        const char *k;
        size_t kl;
        obj_list *l;
        int rc;
        if (argc < 4) {
            wrong_args(out, "blmpop");
            return 1;
        }
        if (blocking_parse_timeout(&argv[1], now_ms, &deadline, out) != 0)
            return 1;
        if (!cmd_parse_ll(&argv[2], &nk)) {
            resp_write_error(out,
                             "ERR value is not an integer or out of range",
                             43);
            return 1;
        }
        if (nk <= 0 ||
            (unsigned long long)nk > (unsigned long long)(argc - 4)) {
            static const char E[] =
                "ERR Number of keys can't be greater than number of args";
            resp_write_error(out, E, sizeof(E) - 1);
            return 1;
        }
        dir_idx = 3 + (size_t)nk;
        if (!arg_str(&argv[dir_idx], &where, &wl))
            goto bad_blocking_arg;
        if (ci_equal(where, wl, "LEFT")) {
            left = 1;
        } else if (ci_equal(where, wl, "RIGHT")) {
            left = 0;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return 1;
        }
        if (argc > dir_idx + 1) {
            const char *opt;
            size_t ol;
            if (argc != dir_idx + 3) {
                wrong_args(out, "blmpop");
                return 1;
            }
            if (!arg_str(&argv[dir_idx + 1], &opt, &ol))
                goto bad_blocking_arg;
            if (!ci_equal(opt, ol, "COUNT")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return 1;
            }
            if (!cmd_parse_ll(&argv[dir_idx + 2], &count)) {
                resp_write_error(out,
                                 "ERR value is not an integer or out of "
                                 "range",
                                 43);
                return 1;
            }
            if (count <= 0) {
                static const char E[] =
                    "ERR value is out of range, must be positive";
                resp_write_error(out, E, sizeof(E) - 1);
                return 1;
            }
        }
        rc = blocking_first_list(d, out, argv, 3, dir_idx, now_ms, &k,
                                 &kl, &l);
        if (rc < 0)
            return 1;
        if (rc == 0) {
            *deadline_ms = deadline;
            return 0;
        }
        {
            size_t n = (unsigned long long)count < obj_list_len(l)
                           ? (size_t)count
                           : (size_t)obj_list_len(l);
            uint64_t before = obj_list_mem(l);
            size_t j;
            resp_write_array_header(out, 2);
            resp_write_bulk(out, k, kl);
            resp_write_array_header(out, n);
            for (j = 0; j < n; j++) {
                char *data = NULL;
                size_t dlen = 0;
                if (!obj_list_pop(l, left, &data, &dlen))
                    break;
                resp_write_bulk(out, data, dlen);
                free(data);
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
            if (obj_list_len(l) == 0)
                db_del_kv(d, k, kl);
        }
        return 1;
    }

    if (cmd_id == CMD_BZPOPMIN || cmd_id == CMD_BZPOPMAX) {
        int min_side = cmd_id == CMD_BZPOPMIN;
        const char *k;
        size_t kl;
        obj_zset *z;
        int rc;
        if (argc < 3) {
            wrong_args(out, min_side ? "bzpopmin" : "bzpopmax");
            return 1;
        }
        if (blocking_parse_timeout(&argv[argc - 1], now_ms, &deadline,
                                   out) != 0)
            return 1;
        rc = blocking_first_zset(d, out, argv, 1, argc - 1, now_ms, &k,
                                 &kl, &z);
        if (rc < 0)
            return 1;
        if (rc == 0) {
            *deadline_ms = deadline;
            return 0;
        }
        {
            char *mv = NULL;
            size_t ml = 0;
            double sc = 0.0;
            char num[40];
            int nl;
            uint64_t before = obj_zset_mem(z);
            if (!obj_zset_pop(z, min_side, &mv, &ml, &sc)) {
                write_null_array(out);
                return 1;
            }
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            if (obj_zset_len(z) == 0)
                db_del_kv(d, k, kl);
            nl = fmt_score(num, sizeof(num), sc);
            resp_write_array_header(out, 3);
            resp_write_bulk(out, k, kl);
            resp_write_bulk(out, mv, ml);
            resp_write_bulk(out, num, (size_t)nl);
            free(mv);
        }
        return 1;
    }

    if (cmd_id == CMD_BZMPOP) {
        long long nk;
        long long count = 1;
        size_t side_idx;
        int min_side;
        const char *k;
        size_t kl;
        obj_zset *z;
        int rc;
        if (argc < 4) {
            wrong_args(out, "bzmpop");
            return 1;
        }
        if (blocking_parse_timeout(&argv[1], now_ms, &deadline, out) != 0)
            return 1;
        if (!cmd_parse_ll(&argv[2], &nk)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return 1;
        }
        if (nk <= 0 ||
            (unsigned long long)nk > (unsigned long long)(argc - 4)) {
            static const char E[] =
                "ERR Number of keys can't be greater than number of args";
            resp_write_error(out, E, sizeof(E) - 1);
            return 1;
        }
        side_idx = 3 + (size_t)nk;
        {
            const char *side;
            size_t sidel;
            if (!arg_str(&argv[side_idx], &side, &sidel))
                goto bad_blocking_arg;
            if (ci_equal(side, sidel, "MIN"))
                min_side = 1;
            else if (ci_equal(side, sidel, "MAX"))
                min_side = 0;
            else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return 1;
            }
        }
        if (argc > side_idx + 1) {
            const char *opt;
            size_t ol;
            if (argc != side_idx + 3) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return 1;
            }
            if (!arg_str(&argv[side_idx + 1], &opt, &ol))
                goto bad_blocking_arg;
            if (!ci_equal(opt, ol, "COUNT")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return 1;
            }
            if (!cmd_parse_ll(&argv[side_idx + 2], &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return 1;
            }
            if (count <= 0) {
                static const char E[] =
                    "ERR value is out of range, must be positive";
                resp_write_error(out, E, sizeof(E) - 1);
                return 1;
            }
        }
        rc = blocking_first_zset(d, out, argv, 3, side_idx, now_ms, &k,
                                 &kl, &z);
        if (rc < 0)
            return 1;
        if (rc == 0) {
            *deadline_ms = deadline;
            return 0;
        }
        {
            size_t avail = obj_zset_len(z);
            size_t n = (uint64_t)count < (uint64_t)avail ? (size_t)count
                                                         : avail;
            uint64_t before = obj_zset_mem(z);
            size_t p;
            resp_write_array_header(out, 2);
            resp_write_bulk(out, k, kl);
            resp_write_array_header(out, n);
            for (p = 0; p < n; p++) {
                char *mv = NULL;
                size_t ml = 0;
                double sc = 0.0;
                char num[40];
                int nl;
                if (!obj_zset_pop(z, min_side, &mv, &ml, &sc))
                    break;
                resp_write_array_header(out, 2);
                resp_write_bulk(out, mv, ml);
                nl = fmt_score(num, sizeof(num), sc);
                resp_write_bulk(out, num, (size_t)nl);
                free(mv);
            }
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            if (obj_zset_len(z) == 0)
                db_del_kv(d, k, kl);
        }
        return 1;
    }

bad_blocking_arg:
    resp_write_error(out, "ERR invalid argument type", 24);
    return 1;
}

int command_blocked_try(session *s, resp_buf *out, uint64_t now_ms)
{
    uint64_t deadline;
    if (!s->blocked)
        return 0;
    if (s->blocked_deadline_ms != 0 && now_ms >= s->blocked_deadline_ms) {
        blocking_timeout_reply(out, s->blocked_cmd);
        session_block_clear(s);
        return 1;
    }
    if (!blocking_pop_try(s, s->blocked_argv, s->blocked_argc, out, now_ms,
                          &deadline)) {
        /* The stored argv is the same request: leave the existing deadline
         * untouched; a retry never shortens or extends the original wait. */
        return 0;
    }
    session_block_clear(s);
    return 1;
}

/* Minimal FUNCTION library code is `#!lua name=<lib>` followed by a plain
 * Lua chunk (EVAL-style KEYS/ARGV globals). Redis's `redis.register_function`
 * multi-function libraries are documented as outside this compatibility
 * pass; FCALL runs the named library chunk with the supplied keys/args. */
static int function_parse_lib(const char *code, size_t codelen,
                              const char **body, size_t *bodylen,
                              const char **lib, size_t *liblen)
{
    const char *p = code;
    const char *end = code + codelen;
    if (codelen < 8 || memcmp(p, "#!lua ", 6) != 0)
        return -1;
    p += 6;
    if ((size_t)(end - p) < 5 || memcmp(p, "name=", 5) != 0)
        return -1;
    p += 5;
    *lib = p;
    while (p < end && *p != '\r' && *p != '\n' && *p != ' ')
        p++;
    if (p == *lib)
        return -1;
    *liblen = (size_t)(p - *lib);
    while (p < end && (*p == '\r' || *p == '\n' || *p == ' '))
        p++;
    *body = p;
    *bodylen = (size_t)(end - p);
    return 0;
}

typedef struct function_lib_list_ctx {
    resp_buf *out;
    const char *pat;
    size_t patlen;
    int withcode;
    size_t count;
    int write;
} function_lib_list_ctx;

#define FUNCTION_PAYLOAD_MAGIC "DDUPFN1"
#define FUNCTION_PAYLOAD_MAGIC_LEN 7

static void hex32(char *out, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--) {
        out[i] = digits[v & 0xfU];
        v >>= 4;
    }
}

static int hex32_parse(const char *s, size_t len, uint32_t *out)
{
    uint32_t v = 0;
    size_t i;
    if (len != 8)
        return -1;
    for (i = 0; i < 8; i++) {
        unsigned int c = (unsigned char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= 10 + c - 'a';
        else if (c >= 'A' && c <= 'F')
            v |= 10 + c - 'A';
        else
            return -1;
    }
    *out = v;
    return 0;
}

typedef struct function_dump_len_ctx {
    size_t total;
    int overflow;
} function_dump_len_ctx;

static void function_dump_len_cb(const char *key, size_t klen,
                                 const char *val, size_t vlen, void *arg)
{
    function_dump_len_ctx *c = (function_dump_len_ctx *)arg;
    size_t add = 16;
    (void)key;
    (void)val;
    if (c->overflow)
        return;
    if (klen > UINT32_MAX || vlen > UINT32_MAX ||
        klen > SIZE_MAX - add || vlen > SIZE_MAX - add - klen) {
        c->overflow = 1;
        return;
    }
    add += klen + vlen;
    if (c->total > SIZE_MAX - add) {
        c->overflow = 1;
        return;
    }
    c->total += add;
}

typedef struct function_dump_write_ctx {
    char *dst;
    size_t pos;
} function_dump_write_ctx;

static void function_dump_write_cb(const char *key, size_t klen,
                                   const char *val, size_t vlen, void *arg)
{
    function_dump_write_ctx *c = (function_dump_write_ctx *)arg;
    hex32(c->dst + c->pos, (uint32_t)klen);
    c->pos += 8;
    memcpy(c->dst + c->pos, key, klen);
    c->pos += klen;
    hex32(c->dst + c->pos, (uint32_t)vlen);
    c->pos += 8;
    memcpy(c->dst + c->pos, val, vlen);
    c->pos += vlen;
}

static int function_dump_build(db *d, char **out, size_t *outlen)
{
    function_dump_len_ctx lc;
    function_dump_write_ctx wc;
    char *buf;
    size_t total;

    lc.total = FUNCTION_PAYLOAD_MAGIC_LEN;
    lc.overflow = 0;
    rh_each(&d->function_libs, function_dump_len_cb, &lc);
    if (lc.overflow) {
        *out = NULL;
        *outlen = 0;
        return -1;
    }
    total = lc.total;
    buf = (char *)malloc(total);
    if (buf == NULL) {
        *out = NULL;
        *outlen = 0;
        return -1;
    }
    memcpy(buf, FUNCTION_PAYLOAD_MAGIC, FUNCTION_PAYLOAD_MAGIC_LEN);
    wc.dst = buf;
    wc.pos = FUNCTION_PAYLOAD_MAGIC_LEN;
    rh_each(&d->function_libs, function_dump_write_cb, &wc);
    *out = buf;
    *outlen = total;
    return 0;
}

static int function_restore_parse(const char *payload, size_t payload_len,
                                  rh_table *tmp)
{
    size_t pos;
    if (payload_len < FUNCTION_PAYLOAD_MAGIC_LEN ||
        memcmp(payload, FUNCTION_PAYLOAD_MAGIC, FUNCTION_PAYLOAD_MAGIC_LEN) != 0)
        return -1;
    pos = FUNCTION_PAYLOAD_MAGIC_LEN;
    while (pos < payload_len) {
        uint32_t klen, vlen;
        if (payload_len - pos < 16)
            return -1;
        if (hex32_parse(payload + pos, 8, &klen) != 0)
            return -1;
        pos += 8;
        if ((size_t)klen > payload_len - pos)
            return -1;
        {
            const char *key = payload + pos;
            pos += klen;
            if (payload_len - pos < 8)
                return -1;
            if (hex32_parse(payload + pos, 8, &vlen) != 0)
                return -1;
            pos += 8;
            if ((size_t)vlen > payload_len - pos)
                return -1;
            if (rh_set(tmp, key, klen, payload + pos, vlen) < 0)
                return -1;
            pos += vlen;
        }
    }
    return pos == payload_len ? 0 : -1;
}

typedef struct function_restore_join_ctx {
    rh_table *dst;
    int replace;
    int fail;
} function_restore_join_ctx;

static void function_restore_join_cb(const char *key, size_t klen,
                                     const char *val, size_t vlen, void *arg)
{
    function_restore_join_ctx *c = (function_restore_join_ctx *)arg;
    const char *old;
    size_t oldlen;
    if (c->fail)
        return;
    if (!c->replace && rh_get(c->dst, key, klen, &old, &oldlen)) {
        c->fail = 1;
        return;
    }
    if (rh_set(c->dst, key, klen, val, vlen) < 0)
        c->fail = 1;
}

static void function_lib_list_cb(const char *key, size_t klen,
                                 const char *val, size_t vlen, void *arg)
{
    function_lib_list_ctx *c = (function_lib_list_ctx *)arg;
    if (c->pat != NULL &&
        !ddup_glob_match(c->pat, c->patlen, key, klen))
        return;
    if (c->write) {
        if (c->withcode) {
            resp_write_array_header(c->out, 2);
            resp_write_bulk(c->out, key, klen);
            resp_write_bulk(c->out, val, vlen);
        } else {
            resp_write_bulk(c->out, key, klen);
        }
    }
    c->count++;
}

static void command_function(session *s, const resp_value *argv, size_t argc,
                             resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    const char *sub;
    size_t sl;
    (void)now_ms;
    if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
        goto bad;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "LOAD [REPLACE] <function-code>",
            "DELETE <library-name>",
            "LIST [LIBRARYNAME pattern] [WITHCODE]",
            "FLUSH [ASYNC|SYNC]",
            "DUMP",
            "RESTORE <serialized-value> [FLUSH|APPEND|REPLACE]",
            "STATS",
            "KILL"
        };
        size_t i;
        resp_write_array_header(out,
                                sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }

    if (ci_equal(sub, sl, "LOAD")) {
        int replace = 0;
        const char *code;
        size_t codelen;
        const char *body, *lib;
        size_t bodylen, liblen;
        if (argc == 4) {
            const char *opt;
            size_t optl;
            if (!arg_str(&argv[2], &opt, &optl))
                goto bad;
            if (!ci_equal(opt, optl, "REPLACE")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            replace = 1;
            code = argv[3].str;
            codelen = argv[3].len;
        } else if (argc == 3) {
            code = argv[2].str;
            codelen = argv[2].len;
        } else {
            wrong_args(out, "function|load");
            return;
        }
        if (argv[argc - 1].str == NULL)
            goto bad;
        if (function_parse_lib(code, codelen, &body, &bodylen, &lib,
                               &liblen) != 0) {
            static const char E[] =
                "ERR Missing library metadata";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        (void)body;
        (void)bodylen;
        if (!replace && rh_get(&d->function_libs, lib, liblen, &body,
                               &bodylen)) {
            char msg[96];
            int n = snprintf(msg, sizeof(msg),
                             "ERR Library '%.*s' already exists",
                             (int)liblen, lib);
            resp_write_error(out, msg, (size_t)n);
            return;
        }
        if (rh_set(&d->function_libs, lib, liblen, code, codelen) < 0) {
            static const char E[] = "ERR out of memory";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        d->dirty++;
        resp_write_bulk(out, lib, liblen);
        return;
    }

    if (ci_equal(sub, sl, "DELETE") && argc == 3) {
        const char *lib;
        size_t liblen;
        const char *dummy;
        size_t dummylen;
        if (!arg_str(&argv[2], &lib, &liblen))
            goto bad;
        if (!rh_get(&d->function_libs, lib, liblen, &dummy, &dummylen)) {
            static const char E[] = "ERR Library not found";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        rh_del(&d->function_libs, lib, liblen);
        d->dirty++;
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (ci_equal(sub, sl, "FLUSH") && (argc == 2 || argc == 3)) {
        rh_destroy(&d->function_libs);
        rh_init(&d->function_libs);
        d->dirty++;
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (ci_equal(sub, sl, "LIST") && argc >= 2 && argc <= 5) {
        function_lib_list_ctx c;
        const char *pat = NULL;
        size_t patlen = 0;
        int withcode = 0;
        size_t i = 2;
        while (i < argc) {
            const char *tok;
            size_t toklen;
            if (!arg_str(&argv[i], &tok, &toklen))
                goto bad;
            if (ci_equal(tok, toklen, "WITHCODE")) {
                withcode = 1;
                i++;
            } else if (ci_equal(tok, toklen, "LIBRARYNAME") && i + 1 < argc) {
                if (!arg_str(&argv[i + 1], &pat, &patlen))
                    goto bad;
                i += 2;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        c.out = out;
        c.pat = pat;
        c.patlen = patlen;
        c.withcode = withcode;
        c.count = 0;
        c.write = 0;
        rh_each(&d->function_libs, function_lib_list_cb, &c);
        resp_write_array_header(out, c.count);
        c.count = 0;
        c.write = 1;
        rh_each(&d->function_libs, function_lib_list_cb, &c);
        return;
    }

    if (ci_equal(sub, sl, "STATS") && argc == 2) {
        resp_write_array_header(out, 4);
        resp_write_bulk(out, "running_script", 14);
        resp_write_bulk(out, NULL, 0);
        resp_write_bulk(out, "engines", 7);
        resp_write_array_header(out, 0);
        return;
    }

    if (ci_equal(sub, sl, "DUMP") && argc == 2) {
        char *payload;
        size_t payloadlen;
        if (function_dump_build(d, &payload, &payloadlen) != 0) {
            static const char E[] = "ERR out of memory";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        resp_write_bulk(out, payload, payloadlen);
        free(payload);
        return;
    }

    if (ci_equal(sub, sl, "RESTORE") && argc >= 3 && argc <= 4) {
        const char *payload;
        size_t payloadlen;
        const char *policy = NULL;
        size_t policylen = 0;
        int mode = 0; /* 0 append, 1 replace, 2 flush */
        rh_table tmp;
        function_restore_join_ctx jc;

        if (argv[2].str == NULL)
            goto bad;
        if (!arg_str(&argv[2], &payload, &payloadlen))
            goto bad;
        if (argc == 4) {
            if (!arg_str(&argv[3], &policy, &policylen))
                goto bad;
            if (ci_equal(policy, policylen, "APPEND")) {
                mode = 0;
            } else if (ci_equal(policy, policylen, "REPLACE")) {
                mode = 1;
            } else if (ci_equal(policy, policylen, "FLUSH")) {
                mode = 2;
            } else {
                static const char E[] =
                    "ERR Wrong restore policy given, value should be either "
                    "FLUSH, APPEND or REPLACE.";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
        }

        rh_init(&tmp);
        if (function_restore_parse(payload, payloadlen, &tmp) != 0) {
            rh_destroy(&tmp);
            static const char E[] = "ERR invalid function payload";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }

        if (mode == 2) {
            rh_destroy(&d->function_libs);
            d->function_libs = tmp;
        } else {
            jc.dst = &d->function_libs;
            jc.replace = (mode == 1);
            jc.fail = 0;
            rh_each(&tmp, function_restore_join_cb, &jc);
            rh_destroy(&tmp);
            if (jc.fail) {
                char msg[96];
                int n;
                if (jc.replace) {
                    static const char E[] = "ERR out of memory";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                /* APPEND collision: report the first existing library name
                 * in the same wording as FUNCTION LOAD. */
                n = snprintf(msg, sizeof(msg),
                             "ERR Library already exists");
                resp_write_error(out, msg, (size_t)n);
                return;
            }
        }
        d->dirty++;
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (ci_equal(sub, sl, "KILL") && argc == 2) {
        static const char E[] = "NOTBUSY No scripts in execution right now";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    {
        char lc[32];
        char msg[160];
        size_t i;
        int n;
        for (i = 0; i < sl && i < sizeof(lc) - 1; i++)
            lc[i] = (sub[i] >= 'A' && sub[i] <= 'Z')
                        ? (char)(sub[i] + ('a' - 'A'))
                        : sub[i];
        lc[i] = '\0';
        n = snprintf(msg, sizeof(msg),
                     "ERR Unknown FUNCTION subcommand or wrong number of "
                     "arguments for '%s'",
                     lc);
        resp_write_error(out, msg, (size_t)n);
    }
    return;

bad:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_wait(session *s, const resp_value *argv, size_t argc,
                        resp_buf *out)
{
    const char *nv, *tv;
    size_t nl, tl;
    long long numreplicas, timeout;
    (void)s;
    if (argc != 3 || !arg_str(&argv[1], &nv, &nl) ||
        !arg_str(&argv[2], &tv, &tl) || !parse_i64(nv, nl, &numreplicas) ||
        !parse_i64(tv, tl, &timeout)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    if (numreplicas < 0) {
        resp_write_error(out, "ERR Number of replicas can't be negative", 41);
        return;
    }
    (void)timeout;
    /* Shared-nothing single node has no synchronous replicas to await. */
    resp_write_integer(out, 0);
}

static void command_waitaof(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out)
{
    const char *n1, *n2, *tv;
    size_t l1, l2, tl;
    long long local, replicas, timeout;
    (void)s;
    if (argc != 4 || !arg_str(&argv[1], &n1, &l1) ||
        !arg_str(&argv[2], &n2, &l2) || !arg_str(&argv[3], &tv, &tl) ||
        !parse_i64(n1, l1, &local) || !parse_i64(n2, l2, &replicas) ||
        !parse_i64(tv, tl, &timeout)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    if (local < 0 || replicas < 0) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    (void)timeout;
    resp_write_array_header(out, 2);
    resp_write_integer(out, 0);
    resp_write_integer(out, 0);
}

static void command_replconf(session *s, const resp_value *argv, size_t argc,
                             resp_buf *out)
{
    const char *sub;
    size_t sl;
    (void)s;
    if (argc < 2 || !arg_str(&argv[1], &sub, &sl)) {
        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
        return;
    }
    if (ci_equal(sub, sl, "GETACK") && argc == 3) {
        resp_write_array_header(out, 3);
        resp_write_bulk(out, "REPLCONF", 8);
        resp_write_bulk(out, "ACK", 3);
        resp_write_integer(out, 0);
        return;
    }
    resp_write_simple_string(out, "OK", 2);
}

static void command_failover(session *s, const resp_value *argv, size_t argc,
                             resp_buf *out)
{
    (void)s;
    if (argc == 1) {
        static const char E[] = "ERR FAILOVER requires connected replicas.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    /* Optional TO/ABORT/TIMEOUT arguments are accepted syntactically. The
     * standalone cache has no coordinated failover state machine. */
    if (argc == 2) {
        const char *ab;
        size_t abl;
        if (arg_str(&argv[1], &ab, &abl) && ci_equal(ab, abl, "ABORT")) {
            resp_write_simple_string(out, "OK", 2);
            return;
        }
    }
    {
        static const char E[] = "ERR FAILOVER requires connected replicas.";
        resp_write_error(out, E, sizeof(E) - 1);
    }
}

static void command_monitor(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out)
{
    (void)s;
    if (argc != 1) {
        wrong_args(out, "monitor");
        return;
    }
    (void)argv;
    static const char E[] = "ERR MONITOR is not supported in this build";
    resp_write_error(out, E, sizeof(E) - 1);
}

static void command_acl(session *s, const resp_value *argv, size_t argc,
                        resp_buf *out)
{
    const char *sub;
    size_t sl;
    (void)s;
    if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
        goto bad;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "CAT [category]", "DELUSER <username> [username ...]",
            "DRYRUN <username> <command> [arg ...]", "GENPASS [bits]",
            "GETUSER <username>", "LIST", "LOAD", "LOG [count|RESET]",
            "SAVE", "SETUSER <username> [rules]", "USERS", "WHOAMI"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }
    if (ci_equal(sub, sl, "LIST") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "USERS") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "WHOAMI") && argc == 2) {
        resp_write_bulk(out, "default", 7);
        return;
    }
    if (ci_equal(sub, sl, "CAT") && (argc == 2 || argc == 3)) {
        static const char *cats[] = {"keyspace", "read", "write", "connection"};
        size_t i;
        resp_write_array_header(out, sizeof(cats) / sizeof(cats[0]));
        for (i = 0; i < sizeof(cats) / sizeof(cats[0]); i++)
            resp_write_bulk(out, cats[i], strlen(cats[i]));
        return;
    }
    if (ci_equal(sub, sl, "GENPASS") && (argc == 2 || argc == 3)) {
        static const char zeros[] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        resp_write_bulk(out, zeros, 64);
        return;
    }
    if (ci_equal(sub, sl, "SETUSER") && argc >= 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "GETUSER") && argc == 3) {
        resp_write_bulk(out, NULL, 0);
        return;
    }
    if (ci_equal(sub, sl, "DELUSER") && argc >= 3) {
        resp_write_integer(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "DRYRUN") && argc >= 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "LOAD") && argc == 2) {
        static const char E[] =
            "ERR This Redis instance is not configured to use an ACL file.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (ci_equal(sub, sl, "SAVE") && argc == 2) {
        static const char E[] =
            "ERR This Redis instance is not configured to use an ACL file.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (ci_equal(sub, sl, "LOG") && (argc == 2 || argc == 3)) {
        if (argc == 3) {
            const char *opt;
            size_t opl;
            if (!arg_str(&argv[2], &opt, &opl))
                goto bad;
            if (ci_equal(opt, opl, "RESET")) {
                resp_write_simple_string(out, "OK", 2);
                return;
            }
        }
        resp_write_array_header(out, 0);
        return;
    }
    {
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "ERR Unknown ACL subcommand or wrong number of "
                         "arguments for '%.*s'",
                         (int)sl, sub);
        resp_write_error(out, msg, (size_t)n);
    }
    return;

bad:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_latency(session *s, const resp_value *argv, size_t argc,
                            resp_buf *out)
{
    const char *sub;
    size_t sl;
    (void)s;
    if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
        goto bad;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "DOCTOR", "GRAPH <event>", "HISTOGRAM [command ...]",
            "HISTORY <event>", "LATEST", "RESET [event ...]"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }
    if (ci_equal(sub, sl, "LATEST") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "HISTORY") && argc == 3) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "RESET") && argc >= 2) {
        resp_write_integer(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "GRAPH") && argc == 3) {
        const char *event;
        size_t el;
        char buf[128];
        int n;
        if (!arg_str(&argv[2], &event, &el))
            goto bad;
        n = snprintf(buf, sizeof(buf), "latency graph for event: %.*s",
                     (int)el, event);
        resp_write_bulk(out, buf, (size_t)n);
        return;
    }
    if (ci_equal(sub, sl, "DOCTOR") && argc == 2) {
        static const char E[] = "Dave, I have a bad feeling about this.\n";
        resp_write_bulk(out, E, sizeof(E) - 1);
        return;
    }
    if (ci_equal(sub, sl, "HISTOGRAM") && argc >= 2) {
        resp_write_array_header(out, 0);
        return;
    }
    {
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "ERR Unknown LATENCY subcommand or wrong number of "
                         "arguments for '%.*s'",
                         (int)sl, sub);
        resp_write_error(out, msg, (size_t)n);
    }
    return;

bad:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_module(session *s, const resp_value *argv, size_t argc,
                           resp_buf *out)
{
    const char *sub;
    size_t sl;
    (void)s;
    if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
        goto bad;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "LIST", "LOAD <path> [arg ...]",
            "LOADEX <path> [CONFIG name value ...] [ARGS arg ...]",
            "UNLOAD <name>"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }
    if (ci_equal(sub, sl, "LIST") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if ((ci_equal(sub, sl, "LOAD") || ci_equal(sub, sl, "LOADEX")) &&
        argc >= 3) {
        static const char E[] =
            "ERR Error loading the extension. Please check the server logs.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    if (ci_equal(sub, sl, "UNLOAD") && argc == 3) {
        static const char E[] =
            "ERR Error unloading module: no such module with that name";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    {
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "ERR Unknown MODULE subcommand or wrong number of "
                         "arguments for '%.*s'",
                         (int)sl, sub);
        resp_write_error(out, msg, (size_t)n);
    }
    return;

bad:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_sentinel(session *s, const resp_value *argv, size_t argc,
                             resp_buf *out)
{
    const char *sub;
    size_t sl;
    (void)s;
    if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
        goto bad;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "CKQUORUM <master>", "CONFIG GET|SET <master> [param] [value]",
            "DEBUG", "FAILOVER <master>", "FLUSHCONFIG",
            "GET-MASTER-ADDR-BY-NAME <master>", "INFO-CACHE <master>",
            "IS-MASTER-DOWN-BY-ADDR <ip> <port> <epoch> <runid>",
            "MASTER <master>", "MASTERS", "MONITOR <name> <ip> <port> <quorum>",
            "MYID", "PENDING-SCRIPTS", "REMOVE <master>", "REPLICAS <master>",
            "RESET <pattern>", "SENTINELS <master>", "SET <master> <option> <value>",
            "SIMULATE-FAILURE <mode>", "SLAVES <master>"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }
    if (ci_equal(sub, sl, "MASTERS") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "MASTER") && argc == 3) {
        resp_write_bulk(out, NULL, 0);
        return;
    }
    if ((ci_equal(sub, sl, "REPLICAS") || ci_equal(sub, sl, "SLAVES")) &&
        argc == 3) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "SENTINELS") && argc == 3) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "GET-MASTER-ADDR-BY-NAME") && argc == 3) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "RESET") && argc == 3) {
        resp_write_integer(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "FAILOVER") && argc == 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "CKQUORUM") && argc == 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "FLUSHCONFIG") && argc == 2) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "MONITOR") && argc == 6) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "REMOVE") && argc == 3) {
        resp_write_integer(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "SET") && argc == 5) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "IS-MASTER-DOWN-BY-ADDR") && argc == 6) {
        resp_write_array_header(out, 2);
        resp_write_integer(out, 0);
        resp_write_bulk(out, "*", 1);
        return;
    }
    if (ci_equal(sub, sl, "MYID") && argc == 2) {
        static const char zeros[] = "0000000000000000000000000000000000000000";
        resp_write_bulk(out, zeros, 40);
        return;
    }
    if (ci_equal(sub, sl, "CONFIG") && argc >= 4) {
        const char *op;
        size_t opl;
        if (!arg_str(&argv[2], &op, &opl))
            goto bad;
        if (ci_equal(op, opl, "GET")) {
            resp_write_array_header(out, 0);
            return;
        }
        if (ci_equal(op, opl, "SET") && argc == 6) {
            resp_write_simple_string(out, "OK", 2);
            return;
        }
        goto bad;
    }
    if (ci_equal(sub, sl, "DEBUG") && argc == 2) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "INFO-CACHE") && argc == 3) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "PENDING-SCRIPTS") && argc == 2) {
        resp_write_array_header(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "SIMULATE-FAILURE") && argc == 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    {
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "ERR Unknown SENTINEL subcommand or wrong number of "
                         "arguments for '%.*s'",
                         (int)sl, sub);
        resp_write_error(out, msg, (size_t)n);
    }
    return;

bad:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static void command_debug(session *s, const resp_value *argv, size_t argc,
                          resp_buf *out)
{
    const char *sub;
    size_t sl;
    (void)s;
    if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
        goto bad;

    if (ci_equal(sub, sl, "HELP") && argc == 2) {
        static const char *help[] = {
            "CHANGE-REPL-ID", "JMAP", "LISTPACK <key>", "LOG-MESSAGE <msg>",
            "OBJECT <key>", "QUICKLIST-PACKED-THRESHOLD <size>",
            "SET-ACTIVE-EXPIRE <0|1>", "SLEEP <seconds>",
            "STRINGMATCH <value> <pattern>", "STRINGMATCH-LEN"
        };
        size_t i;
        resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
        for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
            resp_write_bulk(out, help[i], strlen(help[i]));
        return;
    }
    if (ci_equal(sub, sl, "SLEEP") && argc == 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "SET-ACTIVE-EXPIRE") && argc == 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "CHANGE-REPL-ID") && argc == 2) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "QUICKLIST-PACKED-THRESHOLD") && argc == 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (ci_equal(sub, sl, "STRINGMATCH-LEN") && argc == 2) {
        resp_write_integer(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "STRINGMATCH") && argc == 4) {
        resp_write_integer(out, 0);
        return;
    }
    if (ci_equal(sub, sl, "LOG-MESSAGE") && argc == 3) {
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if ((ci_equal(sub, sl, "JMAP") || ci_equal(sub, sl, "OBJECT") ||
         ci_equal(sub, sl, "LISTPACK")) && argc >= 2) {
        static const char E[] = "ERR DEBUG subcommand is not supported in this build";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }
    {
        char msg[128];
        int n = snprintf(msg, sizeof(msg),
                         "ERR Unknown DEBUG subcommand or wrong number of "
                         "arguments for '%.*s'",
                         (int)sl, sub);
        resp_write_error(out, msg, (size_t)n);
    }
    return;

bad:
    resp_write_error(out, "ERR invalid argument type", 24);
}

static int hash_parse_field_list(const resp_value *argv, size_t argc,
                                 size_t fields_idx, size_t args_per_field,
                                 size_t *num_fields, size_t *first_field,
                                 resp_buf *out)
{
    const char *p;
    size_t pl;
    long long nf;
    static const char NEED_FIELDS[] =
        "ERR Mandatory argument FIELDS is missing or not at the right "
        "position";
    static const char NEED_COUNT[] =
        "ERR The `numfields` parameter must match the number of arguments";
    if (fields_idx == 0 || fields_idx + 2 >= argc) {
        resp_write_error(out, NEED_FIELDS, sizeof(NEED_FIELDS) - 1);
        return -1;
    }
    if (!arg_str(&argv[fields_idx + 1], &p, &pl) ||
        !parse_i64(p, pl, &nf) || nf <= 0) {
        static const char E[] = "ERR Number of fields must be a positive integer";
        resp_write_error(out, E, sizeof(E) - 1);
        return -1;
    }
    if ((unsigned long long)nf !=
            (argc - fields_idx - 2) / args_per_field ||
        (argc - fields_idx - 2) % args_per_field != 0) {
        resp_write_error(out, NEED_COUNT, sizeof(NEED_COUNT) - 1);
        return -1;
    }
    *num_fields = (size_t)nf;
    *first_field = fields_idx + 2;
    return 0;
}

static int hash_parse_expire(const resp_value *argv, size_t argc, size_t *pos,
                             int is_hsetex, uint64_t now_ms,
                             uint64_t *expire_ms, int *expire_opt,
                             resp_buf *out)
{
    int flag = 0;
    *expire_opt = 0;
    *expire_ms = 0;
    while (*pos < argc) {
        const char *opt;
        size_t optl;
        if (!arg_str(&argv[*pos], &opt, &optl))
            return -1;
        if (ci_equal(opt, optl, "FIELDS"))
            return 0;
        if (is_hsetex && ci_equal(opt, optl, "KEEPTTL")) {
            if (flag)
                goto err_expire;
            flag = 1;
            *expire_opt = 1; /* KEEPTTL */
            (*pos)++;
            continue;
        }
        if (!is_hsetex && ci_equal(opt, optl, "PERSIST")) {
            if (flag)
                goto err_expire;
            flag = 1;
            *expire_opt = 2; /* PERSIST */
            (*pos)++;
            continue;
        }
        if (ci_equal(opt, optl, "EX") || ci_equal(opt, optl, "PX") ||
            ci_equal(opt, optl, "EXAT") || ci_equal(opt, optl, "PXAT")) {
            const char *tv;
            size_t tvl;
            long long val;
            int unit_ms = ci_equal(opt, optl, "PX") ||
                          ci_equal(opt, optl, "PXAT");
            int relative = ci_equal(opt, optl, "EX") ||
                           ci_equal(opt, optl, "PX");
            if (flag)
                goto err_expire;
            if (*pos + 1 >= argc) {
                static const char E[] = "ERR missing expire time";
                resp_write_error(out, E, sizeof(E) - 1);
                return -1;
            }
            if (!arg_str(&argv[++(*pos)], &tv, &tvl) ||
                !parse_i64(tv, tvl, &val) || val < 0) {
                static const char E[] = "ERR invalid expire time";
                resp_write_error(out, E, sizeof(E) - 1);
                return -1;
            }
            if (unit_ms)
                *expire_ms = relative ? now_ms + (uint64_t)val
                                      : (uint64_t)val;
            else
                *expire_ms = relative ? now_ms + (uint64_t)val * 1000ULL
                                      : (uint64_t)val * 1000ULL;
            flag = 1;
            *expire_opt = 3; /* absolute expiry set */
            (*pos)++;
            continue;
        }
        if (is_hsetex && (ci_equal(opt, optl, "FNX") ||
                          ci_equal(opt, optl, "FXX"))) {
            /* handled by the caller through its own scan */
            (*pos)++;
            continue;
        }
        {
            char msg[128];
            int n = snprintf(msg, sizeof(msg), "ERR unknown argument: %.*s",
                             (int)optl, opt);
            resp_write_error(out, msg, (size_t)n);
        }
        return -1;
    }
    static const char MISSING[] = "ERR missing FIELDS argument";
    resp_write_error(out, MISSING, sizeof(MISSING) - 1);
    return -1;
err_expire:
    {
        static const char E[] =
            "ERR Only one of EX, PX, EXAT, PXAT or KEEPTTL/PERSIST arguments "
            "can be specified";
        resp_write_error(out, E, sizeof(E) - 1);
    }
    return -1;
}

static void command_dispatch(session *s, const resp_value *argv, size_t argc,
                             resp_buf *out, uint64_t now_ms)
{
    db *d = s->d;
    if (argc == 0) {
        resp_write_error(out, "ERR empty command", 17);
        return;
    }

    const char *name;
    size_t nlen;
    if (!arg_str(&argv[0], &name, &nlen)) {
        resp_write_error(out, "ERR invalid command name", 23);
        return;
    }
    const uint16_t cmd_id = cmd_resolve(name, nlen);

    /* EVAL_RO/EVALSHA_RO scripts may call read-only commands only. */
    if (s->in_script && s->in_ro_script && cmd_is_write(cmd_id)) {
        static const char E[] =
            "ERR Write commands are not allowed from read-only scripts.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    /* replicas are read-only for client writes (replication link bypasses) */
    {
        int write_blocked = is_write_command(name, nlen);
        if (write_blocked && cmd_id == CMD_SORT) {
            size_t ai;
            write_blocked = 0;
            for (ai = 2; ai < argc; ai++) {
                const char *tok;
                size_t tl;
                if (arg_str(&argv[ai], &tok, &tl) &&
                    ci_equal(tok, tl, "STORE")) {
                    write_blocked = 1;
                    break;
                }
            }
        }
        if (s->role != NULL && *s->role == SESSION_ROLE_REPLICA &&
            !s->repl_link && write_blocked) {
        static const char E[] =
            "READONLY You can't write against a read only replica.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
        }
    }

    /* cluster mode: ownership enforcement (-MOVED / -CLUSTERDOWN / -ASK) */
    if (!cluster_check_ownership(s, argv, argc, out, now_ms))
        return;

    if (cmd_id == CMD_BLPOP || cmd_id == CMD_BRPOP ||
        cmd_id == CMD_BRPOPLPUSH || cmd_id == CMD_BLMOVE ||
        cmd_id == CMD_BLMOVEM || cmd_id == CMD_BLMPOP ||
        cmd_id == CMD_BZPOPMIN || cmd_id == CMD_BZPOPMAX ||
        cmd_id == CMD_BZMPOP) {
        uint64_t deadline_ms = 0;
        if (blocking_pop_try(s, argv, argc, out, now_ms, &deadline_ms))
            return;
        if (session_block_start(s, argv, argc, cmd_id, deadline_ms) != 0) {
            static const char E[] = "ERR out of memory";
            resp_write_error(out, E, sizeof(E) - 1);
        }
        return;
    }

    if (cmd_id == CMD_PING) {
        if (argc == 1) {
            resp_write_simple_string(out, "PONG", 4);
        } else if (argc == 2) {
            const char *s;
            size_t l;
            if (!arg_str(&argv[1], &s, &l))
                goto bad_type;
            resp_write_bulk(out, s, l);
        } else {
            wrong_args(out, "ping");
        }
        return;
    }

    if (cmd_id == CMD_READONLY || cmd_id == CMD_READWRITE) {
        if (argc != 1) {
            wrong_args(out, cmd_id == CMD_READONLY ? "readonly"
                                                   : "readwrite");
            return;
        }
        s->read_only = (cmd_id == CMD_READONLY);
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_ROLE) {
        const repl_info *r = s->repl;
        if (argc != 1) {
            wrong_args(out, "role");
            return;
        }
        if (r == NULL || r->role == SESSION_ROLE_MASTER) {
            resp_write_array_header(out, 3);
            resp_write_bulk(out, "master", 6);
            resp_write_integer(out, (long long)(r == NULL ? 0 : r->offset));
            resp_write_array_header(out, 0);
        } else {
            resp_write_array_header(out, 5);
            resp_write_bulk(out, "slave", 5);
            resp_write_bulk(out, r->master_host, strlen(r->master_host));
            resp_write_integer(out, (long long)r->master_port);
            resp_write_bulk(out, r->link_up ? "connected" : "connecting",
                            r->link_up ? 9 : 10);
            resp_write_integer(out, (long long)r->master_offset);
        }
        return;
    }

    if (cmd_id == CMD_WAIT) {
        command_wait(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_WAITAOF) {
        command_waitaof(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_REPLCONF) {
        command_replconf(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_FAILOVER) {
        command_failover(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_MONITOR) {
        command_monitor(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_ACL) {
        command_acl(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_DEBUG) {
        command_debug(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_LATENCY) {
        command_latency(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_MODULE) {
        command_module(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_SENTINEL) {
        command_sentinel(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_HELLO) {
        long long proto = 2;
        if (argc == 1) {
            hello_reply(out, (int)proto);
        } else if (argc == 2) {
            const char *pv;
            size_t pl;
            if (!arg_str(&argv[1], &pv, &pl) || !parse_i64(pv, pl, &proto)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (proto != 2 && proto != 3) {
                static const char E[] = "NOPROTO unsupported protocol version";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            hello_reply(out, (int)proto);
        } else {
            wrong_args(out, "hello");
        }
        return;
    }

    if (cmd_id == CMD_PFADD) {
        cmd_pfadd(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_PFCOUNT) {
        cmd_pfcount(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_PFMERGE) {
        cmd_pfmerge(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_PFDEBUG) {
        cmd_pfdebug(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_PFSELFTEST) {
        cmd_pfselftest(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_GEOADD) {
        cmd_geoadd(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_GEOPOS) {
        cmd_geopos(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_GEOHASH) {
        cmd_geohash(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_GEODIST) {
        cmd_geodist(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_GEORADIUS || cmd_id == CMD_GEORADIUS_RO) {
        cmd_georadius(d, argv, argc, out, now_ms,
                      cmd_id == CMD_GEORADIUS, 0,
                      cmd_id == CMD_GEORADIUS ? "georadius"
                                              : "georadius_ro");
        return;
    }

    if (cmd_id == CMD_GEORADIUSBYMEMBER ||
        cmd_id == CMD_GEORADIUSBYMEMBER_RO) {
        cmd_georadius(d, argv, argc, out, now_ms,
                      cmd_id == CMD_GEORADIUSBYMEMBER, 1,
                      cmd_id == CMD_GEORADIUSBYMEMBER
                          ? "georadiusbymember"
                          : "georadiusbymember_ro");
        return;
    }

    if (cmd_id == CMD_GEOSEARCH) {
        cmd_geosearch(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_GEOSEARCHSTORE) {
        cmd_geosearchstore(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_AUTH) {
        const char *pw = NULL;
        size_t pwl = 0;
        const char *rp = s->requirepass;
        if (rp == NULL || rp[0] == '\0') {
            static const char E[] =
                "ERR Client sent AUTH, but no password is set";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (argc == 2) {
            if (!arg_str(&argv[1], &pw, &pwl))
                goto bad_type;
        } else if (argc == 3) {
            const char *user;
            size_t ul;
            if (!arg_str(&argv[1], &user, &ul) ||
                !arg_str(&argv[2], &pw, &pwl))
                goto bad_type;
            if (ul != 7 || memcmp(user, "default", 7) != 0) {
                static const char E[] =
                    "WRONGPASS invalid username-password pair or user is "
                    "disabled.";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
        } else {
            wrong_args(out, "auth");
            return;
        }
        if (pwl == strlen(rp) && memcmp(pw, rp, pwl) == 0) {
            s->authed = 1;
            resp_write_simple_string(out, "OK", 2);
        } else {
            static const char E[] =
                "WRONGPASS invalid username-password pair or user is "
                "disabled.";
            resp_write_error(out, E, sizeof(E) - 1);
        }
        return;
    }

    if (cmd_id == CMD_SELECT) {
        long long idx;
        if (argc != 2) {
            wrong_args(out, "select");
            return;
        }
        if (!cmd_parse_ll(&argv[1], &idx)) {
            resp_write_error(out,
                             "ERR value is not an integer or out of range",
                             43);
            return;
        }
        if (idx < 0 || idx >= s->sel_ndbs || s->sel_fn == NULL) {
            static const char E[] = "ERR DB index is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        s->d = s->sel_fn(s->sel_ctx, (int)idx);
        s->db_index = (int)idx;
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_SWAPDB) {
        long long a, b;
        if (argc != 3) {
            wrong_args(out, "swapdb");
            return;
        }
        if (!cmd_parse_ll(&argv[1], &a) || !cmd_parse_ll(&argv[2], &b)) {
            resp_write_error(out,
                             "ERR value is not an integer or out of range",
                             43);
            return;
        }
        if (a < 0 || a >= s->sel_ndbs || b < 0 || b >= s->sel_ndbs ||
            s->sel_fn == NULL) {
            static const char E[] = "ERR DB index is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (a != b) {
            db *da = s->sel_fn(s->sel_ctx, (int)a);
            db *dbb = s->sel_fn(s->sel_ctx, (int)b);
            db tmp = *da;
            *da = *dbb;
            *dbb = tmp;
            /* watches must trip: swapped contents invalidate versions */
            da->flush_epoch++;
            dbb->flush_epoch++;
            s->d->dirty++; /* AOF/propagation: log SWAPDB itself */
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_MOVE) {
        long long tdb;
        const char *k;
        size_t kl;
        const char *v;
        size_t vl;
        const char *e;
        size_t el;
        db *td;
        if (argc != 3) {
            wrong_args(out, "move");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!cmd_parse_ll(&argv[2], &tdb)) {
            resp_write_error(out,
                             "ERR value is not an integer or out of range",
                             43);
            return;
        }
        if (tdb < 0 || tdb >= s->sel_ndbs || s->sel_fn == NULL) {
            static const char E[] = "ERR DB index is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (tdb == s->db_index) {
            static const char E[] =
                "ERR source and destination objects are the same";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        td = s->sel_fn(s->sel_ctx, (int)tdb);
        if (td == NULL) {
            static const char E[] = "ERR DB index is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        db_expire_if_needed(d, k, kl, now_ms);
        db_expire_if_needed(td, k, kl, now_ms);
        if (!rh_get(&d->table, k, kl, &v, &vl)) {
            resp_write_integer(out, 0);
            return;
        }
        if (rh_get(&td->table, k, kl, &e, &el)) {
            resp_write_integer(out, 0);
            return;
        }
        if (oom_blocked(td, out))
            return;
        if (db_set_kv(td, k, kl, v, vl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        if (rh_get(&d->expires, k, kl, &e, &el) && el == 8)
            (void)db_set_expiry(td, k, kl, get_u64(e));
        db_del_kv_keep_obj(d, k, kl);
        resp_write_integer(out, 1);
        return;
    }

    if (cmd_id == CMD_ECHO) {
        if (argc != 2) {
            wrong_args(out, "echo");
            return;
        }
        const char *s;
        size_t l;
        if (!arg_str(&argv[1], &s, &l))
            goto bad_type;
        resp_write_bulk(out, s, l);
        return;
    }

    if (cmd_id == CMD_GET) {
        if (argc != 2) {
            wrong_args(out, "get");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        const char *v;
        size_t vl;
        if (db_get(d, k, kl, &v, &vl, now_ms)) {
            const char *s;
            size_t sl2;
            if (!as_string(out, v, vl, &s, &sl2))
                return;
            resp_write_bulk(out, s, sl2);
        } else {
            resp_write_bulk(out, NULL, 0);
        }
        return;
    }

    if (cmd_id == CMD_SET) {
        if (argc < 3) {
            wrong_args(out, "set");
            return;
        }
        const char *k, *v;
        size_t kl, vl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &v, &vl))
            goto bad_type;
        if (!storage_string_ok(kl, vl)) {
            storage_length_error(out);
            return;
        }
        int nx = 0, xx = 0, has_ttl = 0, keepttl = 0, get_old = 0;
        uint64_t ttl_ms = 0;
        for (size_t i = 3; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if ((ci_equal(o, ol, "NX") || ci_equal(o, ol, "XX")) && !nx &&
                !xx) {
                nx = ci_equal(o, ol, "NX");
                xx = !nx;
            } else if ((ci_equal(o, ol, "EX") || ci_equal(o, ol, "PX")) &&
                       !has_ttl && !keepttl && i + 1 < argc) {
                const char *t;
                size_t tl;
                long long tv;
                if (!arg_str(&argv[i + 1], &t, &tl))
                    goto bad_type;
                if (!parse_i64(t, tl, &tv)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (tv <= 0 ||
                    (ci_equal(o, ol, "EX") && tv > LLONG_MAX / 1000)) {
                    resp_write_error(out,
                                     "ERR invalid expire time in 'set' command",
                                     40);
                    return;
                }
                ttl_ms = ci_equal(o, ol, "EX") ? (uint64_t)tv * 1000ULL
                                               : (uint64_t)tv;
                has_ttl = 1;
                i++;
            } else if (ci_equal(o, ol, "KEEPTTL") && !keepttl && !has_ttl) {
                keepttl = 1;
            } else if (ci_equal(o, ol, "GET") && !get_old) {
                get_old = 1;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        {
            const char *old;
            size_t oldl;
            int exists = db_get(d, k, kl, &old, &oldl, now_ms);
            if (exists && obj_tag_of(old, oldl) != DDUP_OBJ_STRING) {
                wrongtype(out);
                return;
            }
            if ((nx && exists) || (xx && !exists)) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            if (get_old) {
                /* replied before the overwrite frees the old payload */
                if (exists) {
                    const char *sv;
                    size_t sl2;
                    if (!as_string(out, old, oldl, &sv, &sl2))
                        return;
                    resp_write_bulk(out, sv, sl2);
                } else {
                    resp_write_bulk(out, NULL, 0);
                }
            }
        }
        if (oom_blocked(d, out))
            return;
        {
            /* KEEPTTL: read the absolute expiry before db_set_string
             * clears it, restore it after the overwrite */
            uint64_t keep_when = 0;
            int have_keep = 0;
            if (keepttl) {
                const char *e;
                size_t el;
                if (rh_get(&d->expires, k, kl, &e, &el) && el == 8) {
                    keep_when = get_u64(e);
                    have_keep = 1;
                }
            }
            if (db_set_string(d, k, kl, v, vl, now_ms) != 0) {
                storage_length_error(out);
                return;
            }
            if (have_keep && db_set_expiry(d, k, kl, keep_when) != 0) {
                storage_length_error(out);
                return;
            }
        }
        if (has_ttl && db_set_expiry(d, k, kl, now_ms + ttl_ms) != 0) {
            storage_length_error(out);
            return;
        }
        if (!get_old)
            resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_ASKING) {
        if (argc != 1) {
            wrong_args(out, "asking");
            return;
        }
        /* one-shot: the next keyed command may pass an importing slot */
        s->asking = 1;
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_DUMP) {
        if (argc != 2) {
            wrong_args(out, "dump");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        db_expire_if_needed(d, k, kl, now_ms);
        {
            resp_buf payload;
            resp_buf_init(&payload);
            if (snapshot_dump_key(d, k, kl, &payload) == 0)
                resp_write_bulk(out, payload.data, payload.len);
            else
                resp_write_bulk(out, NULL, 0);
            resp_buf_free(&payload);
        }
        return;
    }

    if (cmd_id == CMD_RESTORE || cmd_id == CMD_RESTORE_ASKING) {
        const char *restore_name = cmd_id == CMD_RESTORE_ASKING
                                     ? "restore-asking"
                                     : "restore";
        if (argc < 4) {
            wrong_args(out, restore_name);
            return;
        }
        const char *k, *t, *p;
        size_t kl, tl, pl;
        long long ttl;
        int replace = 0;
        int absttl = 0;
        int rc;
        size_t i;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &t, &tl) ||
            !arg_str(&argv[3], &p, &pl))
            goto bad_type;
        if (!parse_i64(t, tl, &ttl)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (ttl < 0) {
            static const char E[] = "ERR Invalid TTL value, must be >= 0";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        for (i = 4; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "REPLACE") && !replace) {
                replace = 1;
            } else if (ci_equal(o, ol, "ABSTTL") && !absttl) {
                absttl = 1;
            } else if ((ci_equal(o, ol, "IDLETIME") ||
                        ci_equal(o, ol, "FREQ")) &&
                       i + 1 < argc) {
                const char *nv;
                size_t nvl;
                long long nv64;
                if (!arg_str(&argv[i + 1], &nv, &nvl) ||
                    !parse_i64(nv, nvl, &nv64) || nv64 < 0) {
                    resp_write_error(out, ERR_SYNTAX,
                                     sizeof(ERR_SYNTAX) - 1);
                    return;
                }
                i++;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (oom_blocked(d, out))
            return;
        rc = snapshot_restore_key(
            d, k, kl, p, pl,
            ttl > 0 ? (absttl ? (uint64_t)ttl : now_ms + (uint64_t)ttl) : 0,
            replace, now_ms);
        if (rc == 1) {
            static const char E[] = "BUSYKEY Target key name already exists.";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (rc != 0) {
            static const char E[] = "ERR Bad data format";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_MIGRATE) {
        if (argc < 6) {
            wrong_args(out, "migrate");
            return;
        }
        const char *host, *ports, *key, *dbids, *tos;
        size_t hostl, portl, keyl, dbidl, tosl;
        long long port, dbid, timeout;
        int copy = 0, replace = 0;
        size_t first_key = 0; /* argv index of the KEYS list, 0 = single */
        size_t i;
        const resp_value *keys;
        size_t nkeys;
        char hostbuf[256];
        if (!arg_str(&argv[1], &host, &hostl) ||
            !arg_str(&argv[2], &ports, &portl) ||
            !arg_str(&argv[3], &key, &keyl) ||
            !arg_str(&argv[4], &dbids, &dbidl) ||
            !arg_str(&argv[5], &tos, &tosl))
            goto bad_type;
        if (!parse_i64(ports, portl, &port) || port <= 0 || port > 65535 ||
            !parse_i64(dbids, dbidl, &dbid) ||
            !parse_i64(tos, tosl, &timeout) || timeout < 0) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (dbid != 0) {
            static const char E[] = "ERR DB index is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (hostl >= sizeof(hostbuf)) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        memcpy(hostbuf, host, hostl);
        hostbuf[hostl] = '\0';
        if (timeout == 0)
            timeout = 1000;
        for (i = 6; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "COPY") && !copy) {
                copy = 1;
            } else if (ci_equal(o, ol, "REPLACE") && !replace) {
                replace = 1;
            } else if (ci_equal(o, ol, "KEYS") && first_key == 0 &&
                       i + 1 < argc) {
                first_key = i + 1;
                break;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (first_key != 0) {
            if (keyl != 0) {
                static const char E[] =
                    "ERR When using MIGRATE KEYS option, the key argument "
                    "must be set to the empty string";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            keys = argv + first_key;
            nkeys = argc - first_key;
        } else {
            const char *v;
            size_t vl;
            if (!db_get(d, key, keyl, &v, &vl, now_ms)) {
                static const char E[] = "NOKEY No such key";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            keys = &argv[3];
            nkeys = 1;
        }
        if (migrate_run(d, hostbuf, (uint16_t)port, keys, nkeys,
                        (uint64_t)timeout, copy, replace, now_ms) !=
            MIGRATE_OK) {
            static const char E[] =
                "IOERR error or timeout writing to target instance";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_EVAL || cmd_id == CMD_EVALSHA ||
        cmd_id == CMD_EVAL_RO || cmd_id == CMD_EVALSHA_RO) {
        int is_sha = cmd_id == CMD_EVALSHA || cmd_id == CMD_EVALSHA_RO;
        int is_ro = cmd_id == CMD_EVAL_RO || cmd_id == CMD_EVALSHA_RO;
        long long numkeys;
        const char *kv, *nv;
        size_t kvl, nvl;
        char sha[41];
        if (argc < 3) {
            const char *cn = cmd_id == CMD_EVALSHA ? "evalsha"
                           : cmd_id == CMD_EVAL_RO ? "eval_ro"
                           : cmd_id == CMD_EVALSHA_RO ? "evalsha_ro"
                           : "eval";
            wrong_args(out, cn);
            return;
        }
        if (s->in_script) {
            static const char E[] =
                "ERR This Redis command is not allowed from scripts";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (!arg_str(&argv[2], &nv, &nvl) || !parse_i64(nv, nvl, &numkeys)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (numkeys > (long long)(argc - 3)) {
            static const char E[] =
                "ERR Number of keys can't be greater than number of args";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (numkeys < 0) {
            static const char E[] = "ERR Number of keys can't be negative";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (!arg_str(&argv[1], &kv, &kvl))
            goto bad_type;
        if (is_sha) {
            if (script_ref(d, kv, kvl) < 0) {
                static const char E[] =
                    "NOSCRIPT No matching script. Please use EVAL.";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            memcpy(sha, kv, 40);
            sha[40] = '\0';
        } else {
            char err[256];
            if (script_load(d, kv, kvl, sha, err, sizeof(err)) != 0) {
                char ebuf[384];
                int n = snprintf(ebuf, sizeof(ebuf),
                                 "ERR Error compiling script (new function): "
                                 "%s",
                                 err);
                resp_write_error(out, ebuf, (size_t)n);
                return;
            }
        }
        /* effects replication: redis.call logs effect commands itself */
        s->aof_skip = 1;
        s->in_ro_script += is_ro;
        script_exec(s, sha, argv + 3, (size_t)numkeys,
                    argc - 3 - (size_t)numkeys, out, now_ms);
        s->in_ro_script -= is_ro;
        return;
    }

    if (cmd_id == CMD_FCALL || cmd_id == CMD_FCALL_RO) {
        int is_ro = cmd_id == CMD_FCALL_RO;
        const char *fname, *nv;
        size_t fnamel, nvl;
        const char *code;
        size_t codelen;
        const char *body, *lib;
        size_t bodylen, liblen;
        long long numkeys;
        char sha[41];
        char err[256];
        if (argc < 3) {
            wrong_args(out, is_ro ? "fcall_ro" : "fcall");
            return;
        }
        if (s->in_script) {
            static const char E[] =
                "ERR This Redis command is not allowed from scripts";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (!arg_str(&argv[1], &fname, &fnamel) ||
            !arg_str(&argv[2], &nv, &nvl))
            goto bad_type;
        if (!parse_i64(nv, nvl, &numkeys)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (numkeys < 0) {
            static const char E[] = "ERR Number of keys can't be negative";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (numkeys > (long long)(argc - 3)) {
            static const char E[] =
                "ERR Number of keys can't be greater than number of args";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (!rh_get(&d->function_libs, fname, fnamel, &code, &codelen)) {
            static const char E[] = "ERR Function not found";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (function_parse_lib(code, codelen, &body, &bodylen, &lib,
                               &liblen) != 0) {
            static const char E[] = "ERR Missing library metadata";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        (void)lib;
        (void)liblen;
        if (script_load(d, body, bodylen, sha, err, sizeof(err)) != 0) {
            char ebuf[384];
            int n = snprintf(ebuf, sizeof(ebuf),
                             "ERR Error compiling function: %s", err);
            resp_write_error(out, ebuf, (size_t)n);
            return;
        }
        s->aof_skip = 1;
        s->in_ro_script += is_ro;
        script_exec(s, sha, argv + 3, (size_t)numkeys,
                    argc - 3 - (size_t)numkeys, out, now_ms);
        s->in_ro_script -= is_ro;
        return;
    }

    if (cmd_id == CMD_FUNCTION) {
        command_function(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_SCRIPT) {
        const char *sub;
        size_t sl;
        if (s->in_script) {
            static const char E[] =
                "ERR This Redis command is not allowed from scripts";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
            goto bad_type;
        if (ci_equal(sub, sl, "HELP") && argc == 2) {
            static const char *help[] = {
                "LOAD <script>",
                "EXISTS <sha1> [sha1 ...]",
                "FLUSH",
                "DEBUG YES|SYNC|NO",
                "KILL"
            };
            size_t i;
            resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
            for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
                resp_write_bulk(out, help[i], strlen(help[i]));
            return;
        }
        if (ci_equal(sub, sl, "LOAD") && argc == 3) {
            const char *src;
            size_t srcl;
            char sha[41], err[256];
            if (!arg_str(&argv[2], &src, &srcl))
                goto bad_type;
            if (script_load(d, src, srcl, sha, err, sizeof(err)) != 0) {
                char ebuf[384];
                int n = snprintf(ebuf, sizeof(ebuf),
                                 "ERR Error compiling script (new function): "
                                 "%s",
                                 err);
                resp_write_error(out, ebuf, (size_t)n);
                return;
            }
            resp_write_bulk(out, sha, 40);
            return;
        }
        if (ci_equal(sub, sl, "EXISTS") && argc >= 3) {
            size_t i;
            resp_write_array_header(out, argc - 2);
            for (i = 2; i < argc; i++) {
                const char *k;
                size_t kl;
                if (!arg_str(&argv[i], &k, &kl))
                    goto bad_type;
                resp_write_integer(out,
                        script_cached(d, k, kl) ? 1 : 0);
            }
            return;
        }
        if (ci_equal(sub, sl, "FLUSH") && argc == 2) {
            script_flush(d);
            resp_write_simple_string(out, "OK", 2);
            return;
        }
        if (ci_equal(sub, sl, "DEBUG") && argc == 3) {
            const char *mode;
            size_t ml;
            if (!arg_str(&argv[2], &mode, &ml))
                goto bad_type;
            if (!ci_equal(mode, ml, "YES") &&
                !ci_equal(mode, ml, "SYNC") &&
                !ci_equal(mode, ml, "NO")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            /* Lua debug hooks are not exposed in this build; the mode is
             * accepted for compatibility and has no effect. */
            resp_write_simple_string(out, "OK", 2);
            return;
        }
        if (ci_equal(sub, sl, "KILL") && argc == 2) {
            static const char E[] = "NOTBUSY No scripts in execution right now";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        {
            char msg[96];
            int n = snprintf(msg, sizeof(msg),
                             "ERR Unknown SCRIPT subcommand or wrong number "
                             "of arguments for '%.*s'",
                             (int)sl, sub);
            resp_write_error(out, msg, (size_t)n);
        }
        return;
    }

    if (cmd_id == CMD_DEL || cmd_id == CMD_UNLINK) {
        if (argc < 2) {
            wrong_args(out, "del");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        long long deleted = 0;
        for (size_t i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
            db_expire_if_needed(d, k, kl, now_ms);
            deleted += db_del_kv(d, k, kl);
        }
        resp_write_integer(out, deleted);
        return;
    }

    if (cmd_id == CMD_EXISTS) {
        if (argc < 2) {
            wrong_args(out, "exists");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        long long found = 0;
        for (size_t i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
            const char *v;
            size_t vl;
            found += db_get(d, k, kl, &v, &vl, now_ms);
        }
        resp_write_integer(out, found);
        return;
    }

    if (cmd_id == CMD_INCR || cmd_id == CMD_DECR ||
        cmd_id == CMD_INCRBY || cmd_id == CMD_DECRBY) {
        int by = cmd_id == CMD_INCRBY || cmd_id == CMD_DECRBY;
        if (argc != (by ? 3 : 2)) {
            wrong_args(out, cmd_id == CMD_INCR    ? "incr"
                            : cmd_id == CMD_DECR  ? "decr"
                            : cmd_id == CMD_INCRBY ? "incrby"
                                                   : "decrby");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        long long delta;
        if (cmd_id == CMD_INCR) {
            delta = 1;
        } else if (cmd_id == CMD_DECR) {
            delta = -1;
        } else {
            const char *dv;
            size_t dvl;
            if (!arg_str(&argv[2], &dv, &dvl))
                goto bad_type;
            if (!parse_i64(dv, dvl, &delta)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (cmd_id == CMD_DECRBY) {
                if (delta == LLONG_MIN) {
                    resp_write_error(out, ERR_OVERFLOW,
                                     sizeof(ERR_OVERFLOW) - 1);
                    return;
                }
                delta = -delta;
            }
        }
        long long cur = 0;
        const char *v;
        size_t vl;
        if (db_get(d, k, kl, &v, &vl, now_ms)) {
            const char *s;
            size_t sl2;
            if (!as_string(out, v, vl, &s, &sl2))
                return;
            if (!parse_i64(s, sl2, &cur)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        if ((delta > 0 && cur > LLONG_MAX - delta) ||
            (delta < 0 && cur < LLONG_MIN - delta)) {
            resp_write_error(out, ERR_OVERFLOW, sizeof(ERR_OVERFLOW) - 1);
            return;
        }
        cur += delta;
        char num[24];
        int nl = snprintf(num, sizeof(num), "%lld", cur);
        if (oom_blocked(d, out))
            return;
        if (db_set_string(d, k, kl, num, (size_t)nl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        resp_write_integer(out, cur);
        return;
    }

    if (cmd_id == CMD_APPEND) {
        if (argc != 3) {
            wrong_args(out, "append");
            return;
        }
        const char *k, *v;
        size_t kl, vl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &v, &vl))
            goto bad_type;
        if (!storage_string_ok(kl, vl)) {
            storage_length_error(out);
            return;
        }
        const char *old;
        size_t oldl;
        if (oom_blocked(d, out))
            return;
        if (db_get(d, k, kl, &old, &oldl, now_ms)) {
            const char *os;
            size_t osl;
            resp_buf tmp;
            if (!as_string(out, old, oldl, &os, &osl))
                return;
            resp_buf_init(&tmp);
            if (vl > SIZE_MAX - osl || osl + vl >= UINT32_MAX ||
                resp_buf_reserve(&tmp, osl + vl) != 0) {
                resp_buf_free(&tmp);
                storage_length_error(out);
                return;
            }
            memcpy(tmp.data, os, osl);
            memcpy(tmp.data + osl, v, vl);
            tmp.len = osl + vl;
            if (db_set_string(d, k, kl, tmp.data, tmp.len, now_ms) != 0) {
                resp_buf_free(&tmp);
                storage_length_error(out);
                return;
            }
            resp_write_integer(out, (long long)tmp.len);
            resp_buf_free(&tmp);
        } else {
            if (db_set_string(d, k, kl, v, vl, now_ms) != 0) {
                storage_length_error(out);
                return;
            }
            resp_write_integer(out, (long long)vl);
        }
        return;
    }

    if (cmd_id == CMD_STRLEN) {
        if (argc != 2) {
            wrong_args(out, "strlen");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        const char *v;
        size_t vl;
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_integer(out, 0);
            return;
        }
        {
            const char *s;
            size_t sl2;
            if (!as_string(out, v, vl, &s, &sl2))
                return;
            resp_write_integer(out, (long long)sl2);
        }
        return;
    }

    if (cmd_id == CMD_LCS) {
        static const char LCS_TYPE[] =
            "ERR The specified keys must contain string values";
        static const char LCS_LONG[] = "ERR String too long for LCS";
        static const char LCS_MEM[] =
            "ERR Insufficient memory, failed allocating transient memory "
            "for LCS";
        static const char LCS_AMBIG[] =
            "ERR If you want both the length and indexes, please just use "
            "IDX.";
        const char *k1, *k2;
        size_t kl1, kl2;
        const char *a = "";
        const char *b = "";
        size_t alen = 0, blen = 0;
        const char *v;
        size_t vl;
        size_t j;
        int getlen = 0, getidx = 0, withmatchlen = 0;
        long long minmatchlen = 0;

        if (argc < 3) {
            wrong_args(out, "lcs");
            return;
        }
        if (!arg_str(&argv[1], &k1, &kl1) || !arg_str(&argv[2], &k2, &kl2))
            goto bad_type;

        for (j = 3; j < argc; j++) {
            const char *opt;
            size_t ol;
            if (!arg_str(&argv[j], &opt, &ol))
                goto bad_type;
            if (ci_equal(opt, ol, "IDX")) {
                getidx = 1;
            } else if (ci_equal(opt, ol, "LEN")) {
                getlen = 1;
            } else if (ci_equal(opt, ol, "WITHMATCHLEN")) {
                withmatchlen = 1;
            } else if (ci_equal(opt, ol, "MINMATCHLEN") && j + 1 < argc) {
                const char *mv;
                size_t mvl;
                if (!arg_str(&argv[j + 1], &mv, &mvl))
                    goto bad_type;
                if (!parse_i64(mv, mvl, &minmatchlen)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (minmatchlen < 0)
                    minmatchlen = 0;
                j++;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }

        if (getlen && getidx) {
            resp_write_error(out, LCS_AMBIG, sizeof(LCS_AMBIG) - 1);
            return;
        }

        if (db_get(d, k1, kl1, &v, &vl, now_ms)) {
            if (obj_tag_of(v, vl) != DDUP_OBJ_STRING) {
                resp_write_error(out, LCS_TYPE, sizeof(LCS_TYPE) - 1);
                return;
            }
            obj_str(v, vl, &a, &alen);
        }
        if (db_get(d, k2, kl2, &v, &vl, now_ms)) {
            if (obj_tag_of(v, vl) != DDUP_OBJ_STRING) {
                resp_write_error(out, LCS_TYPE, sizeof(LCS_TYPE) - 1);
                return;
            }
            obj_str(v, vl, &b, &blen);
        }
        if (alen >= UINT32_MAX - 1 || blen >= UINT32_MAX - 1) {
            resp_write_error(out, LCS_LONG, sizeof(LCS_LONG) - 1);
            return;
        }

        if (getlen) {
            size_t lcslen = lcs_length_linear(a, alen, b, blen);
            if (lcslen == SIZE_MAX) {
                resp_write_error(out, LCS_MEM, sizeof(LCS_MEM) - 1);
                return;
            }
            resp_write_integer(out, (long long)lcslen);
            return;
        }

        {
            unsigned long long cells =
                ((unsigned long long)alen + 1) * ((unsigned long long)blen + 1);
            size_t stride = blen + 1;
            size_t alloc;
            uint32_t *dp;
            size_t i, ii, jj;
            size_t lcslen;
            char *result = NULL;
            lcs_match_range *ranges = NULL;
            size_t nmatch = 0;
            size_t ridx;
            size_t arange_start, arange_end = 0, brange_start = 0,
                   brange_end = 0;

            if (cells > (unsigned long long)SIZE_MAX / sizeof(uint32_t)) {
                resp_write_error(out, LCS_LONG, sizeof(LCS_LONG) - 1);
                return;
            }
            alloc = (size_t)cells * sizeof(uint32_t);
            dp = (uint32_t *)malloc(alloc);
            if (dp == NULL) {
                resp_write_error(out, LCS_MEM, sizeof(LCS_MEM) - 1);
                return;
            }

            for (i = 0; i <= alen; i++) {
                for (jj = 0; jj <= blen; jj++) {
                    uint32_t cur;
                    if (i == 0 || jj == 0) {
                        cur = 0;
                    } else if (a[i - 1] == b[jj - 1]) {
                        cur = dp[(jj - 1) + stride * (i - 1)] + 1;
                    } else {
                        uint32_t up = dp[jj + stride * (i - 1)];
                        uint32_t left = dp[(jj - 1) + stride * i];
                        cur = up > left ? up : left;
                    }
                    dp[jj + stride * i] = cur;
                }
            }
            lcslen = dp[blen + stride * alen];

            if (lcslen > 0) {
                result = (char *)malloc(lcslen);
                if (result == NULL) {
                    free(dp);
                    resp_write_error(out, LCS_MEM, sizeof(LCS_MEM) - 1);
                    return;
                }
                if (getidx) {
                    ranges = (lcs_match_range *)malloc(lcslen *
                                                       sizeof(*ranges));
                    if (ranges == NULL) {
                        free(result);
                        free(dp);
                        resp_write_error(out, LCS_MEM,
                                         sizeof(LCS_MEM) - 1);
                        return;
                    }
                }
            }

            ii = alen;
            jj = blen;
            ridx = lcslen;
            arange_start = alen;
            while (ii > 0 && jj > 0) {
                int emit_range = 0;
                if (a[ii - 1] == b[jj - 1]) {
                    result[ridx - 1] = a[ii - 1];
                    if (arange_start == alen) {
                        arange_start = ii - 1;
                        arange_end = ii - 1;
                        brange_start = jj - 1;
                        brange_end = jj - 1;
                    } else if (arange_start == ii && brange_start == jj) {
                        arange_start--;
                        brange_start--;
                    } else {
                        emit_range = 1;
                    }
                    if (arange_start == 0 || brange_start == 0)
                        emit_range = 1;
                    ridx--;
                    ii--;
                    jj--;
                } else {
                    uint32_t up = dp[jj + stride * (ii - 1)];
                    uint32_t left = dp[(jj - 1) + stride * ii];
                    if (up > left)
                        ii--;
                    else
                        jj--;
                    if (arange_start != alen)
                        emit_range = 1;
                }
                if (emit_range) {
                    size_t match_len = arange_end - arange_start + 1;
                    if (minmatchlen == 0 ||
                        (long long)match_len >= minmatchlen) {
                        if (getidx) {
                            ranges[nmatch].a_start = (uint32_t)arange_start;
                            ranges[nmatch].a_end = (uint32_t)arange_end;
                            ranges[nmatch].b_start = (uint32_t)brange_start;
                            ranges[nmatch].b_end = (uint32_t)brange_end;
                            ranges[nmatch].len = (uint32_t)match_len;
                            nmatch++;
                        }
                    }
                    arange_start = alen;
                }
            }

            if (getidx) {
                resp_write_array_header(out, 4);
                resp_write_bulk(out, "matches", 7);
                resp_write_array_header(out, nmatch);
                for (i = 0; i < nmatch; i++) {
                    resp_write_array_header(out, withmatchlen ? 3 : 2);
                    resp_write_array_header(out, 2);
                    resp_write_integer(out, (long long)ranges[i].a_start);
                    resp_write_integer(out, (long long)ranges[i].a_end);
                    resp_write_array_header(out, 2);
                    resp_write_integer(out, (long long)ranges[i].b_start);
                    resp_write_integer(out, (long long)ranges[i].b_end);
                    if (withmatchlen)
                        resp_write_integer(out, (long long)ranges[i].len);
                }
                resp_write_bulk(out, "len", 3);
                resp_write_integer(out, (long long)lcslen);
            } else {
                resp_write_bulk(out, lcslen > 0 ? result : "", lcslen);
            }

            free(ranges);
            free(result);
            free(dp);
            return;
        }
    }

    if (cmd_id == CMD_SORT || cmd_id == CMD_SORT_RO) {
        static const char SORT_CONV[] =
            "ERR One or more scores can't be converted into double";
        static const char SORT_OOM[] = "ERR out of memory";
        int ro = cmd_id == CMD_SORT_RO;
        int desc = 0, alpha = 0, dontsort = 0, by_has_star = 0;
        int int_conversion_error = 0;
        long long limit_start = 0, limit_count = -1;
        const char *srckey, *bypat = NULL, *storekey = NULL;
        size_t srcklen, bylen = 0, storelen = 0;
        sort_get *gets = NULL;
        size_t nget = 0, getcap = 0;
        sort_vec vec;
        sort_elem *tmp = NULL;
        size_t j, out_start = 0, out_count = 0, output_total;
        obj_list *stored = NULL;

        memset(&vec, 0, sizeof(vec));
        if (argc < 2) {
            wrong_args(out, ro ? "sort_ro" : "sort");
            return;
        }
        if (!arg_str(&argv[1], &srckey, &srcklen))
            goto bad_type;

        j = 2;
        while (j < argc) {
            const char *tok;
            size_t tl;
            size_t leftargs = argc - j - 1;
            if (!arg_str(&argv[j], &tok, &tl))
                goto bad_type;
            if (ci_equal(tok, tl, "ASC")) {
                desc = 0;
            } else if (ci_equal(tok, tl, "DESC")) {
                desc = 1;
            } else if (ci_equal(tok, tl, "ALPHA")) {
                alpha = 1;
            } else if (ci_equal(tok, tl, "LIMIT") && leftargs >= 2) {
                const char *ov, *cv;
                size_t ol, cl;
                if (!arg_str(&argv[j + 1], &ov, &ol) ||
                    !arg_str(&argv[j + 2], &cv, &cl))
                    goto bad_type;
                if (!parse_i64(ov, ol, &limit_start) ||
                    !parse_i64(cv, cl, &limit_count)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    goto sort_cleanup;
                }
                j += 2;
            } else if (!ro && ci_equal(tok, tl, "STORE") && leftargs >= 1) {
                if (!arg_str(&argv[j + 1], &storekey, &storelen))
                    goto bad_type;
                j++;
            } else if (ci_equal(tok, tl, "BY") && leftargs >= 1) {
                if (!arg_str(&argv[j + 1], &bypat, &bylen))
                    goto bad_type;
                by_has_star = memchr(bypat, '*', bylen) != NULL;
                if (!by_has_star)
                    dontsort = 1;
                j++;
            } else if (ci_equal(tok, tl, "GET") && leftargs >= 1) {
                const char *pat;
                size_t pl;
                char *copy;
                if (!arg_str(&argv[j + 1], &pat, &pl))
                    goto bad_type;
                if (nget == getcap) {
                    size_t ncap = getcap == 0 ? 4 : getcap * 2;
                    sort_get *ng = (sort_get *)realloc(
                        gets, ncap * sizeof(*ng));
                    if (ng == NULL) {
                        resp_write_error(out, SORT_OOM,
                                         sizeof(SORT_OOM) - 1);
                        goto sort_cleanup;
                    }
                    gets = ng;
                    getcap = ncap;
                }
                copy = (char *)malloc(pl + 1);
                if (copy == NULL) {
                    resp_write_error(out, SORT_OOM, sizeof(SORT_OOM) - 1);
                    goto sort_cleanup;
                }
                memcpy(copy, pat, pl);
                copy[pl] = '\0';
                gets[nget].pattern = copy;
                gets[nget].plen = pl;
                nget++;
                j++;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                goto sort_cleanup;
            }
            j++;
        }

        {
            const char *srcv;
            size_t srcvl;
            if (db_get(d, srckey, srcklen, &srcv, &srcvl, now_ms)) {
                int tag = obj_tag_of(srcv, srcvl);
                if (tag == DDUP_OBJ_LIST) {
                    obj_list *l = (obj_list *)obj_unpack_ptr(srcv, srcvl);
                    sort_vec_add_list(&vec, l);
                } else if (tag == DDUP_OBJ_SET) {
                    obj_set *st = (obj_set *)obj_unpack_ptr(srcv, srcvl);
                    obj_set_each(st, sort_set_collect_cb, &vec);
                } else if (tag == DDUP_OBJ_ZSET) {
                    obj_zset *z = (obj_zset *)obj_unpack_ptr(srcv, srcvl);
                    sort_vec_add_zset(&vec, z);
                } else {
                    wrongtype(out);
                    goto sort_cleanup;
                }
            }
        }
        if (vec.oom) {
            resp_write_error(out, SORT_OOM, sizeof(SORT_OOM) - 1);
            goto sort_cleanup;
        }

        if (!dontsort) {
            for (j = 0; j < vec.n; j++) {
                sort_elem *e = &vec.v[j];
                if (bypat != NULL && by_has_star) {
                    const char *s;
                    size_t sl;
                    int rc = sort_pattern_lookup(d, out, bypat, bylen,
                                                 e->val, e->vlen, now_ms,
                                                 &s, &sl);
                    if (rc < 0)
                        goto sort_cleanup;
                    if (rc > 0) {
                        if (alpha) {
                            e->cmp = (char *)malloc(sl + 1);
                            if (e->cmp == NULL) {
                                resp_write_error(out, SORT_OOM,
                                                 sizeof(SORT_OOM) - 1);
                                goto sort_cleanup;
                            }
                            memcpy(e->cmp, s, sl);
                            e->cmp[sl] = '\0';
                            e->cmplen = sl;
                            e->has_cmp = 1;
                        } else if (!parse_double(s, sl, &e->score)) {
                            int_conversion_error = 1;
                        }
                    }
                } else if (!alpha && !parse_double(e->val, e->vlen,
                                                   &e->score)) {
                    int_conversion_error = 1;
                }
            }
        }

        if (!dontsort && vec.n > 1) {
            tmp = (sort_elem *)malloc(vec.n * sizeof(*tmp));
            if (tmp == NULL) {
                resp_write_error(out, SORT_OOM, sizeof(SORT_OOM) - 1);
                goto sort_cleanup;
            }
            sort_merge_sort(vec.v, tmp, vec.n, alpha,
                            bypat != NULL && by_has_star, desc);
        } else if (dontsort && desc && vec.n > 1) {
            size_t lo = 0, hi = vec.n - 1;
            while (lo < hi) {
                sort_elem swap = vec.v[lo];
                vec.v[lo] = vec.v[hi];
                vec.v[hi] = swap;
                lo++;
                hi--;
            }
        }

        if (vec.n > 0) {
            size_t start = limit_start > 0 ? (size_t)limit_start : 0;
            size_t remaining;
            if (start < vec.n) {
                remaining = vec.n - start;
                if (limit_count < 0 ||
                    (unsigned long long)limit_count >= remaining) {
                    out_start = start;
                    out_count = remaining;
                } else if (limit_count > 0) {
                    out_start = start;
                    out_count = (size_t)limit_count;
                }
            }
        }
        output_total = nget > 0 ? out_count * nget : out_count;

        if (int_conversion_error) {
            resp_write_error(out, SORT_CONV, sizeof(SORT_CONV) - 1);
            goto sort_cleanup;
        }

        if (storekey != NULL) {
            if (out_count == 0) {
                db_del_kv(d, storekey, storelen);
                resp_write_integer(out, 0);
            } else {
                stored = obj_list_new();
                if (stored == NULL) {
                    resp_write_error(out, SORT_OOM, sizeof(SORT_OOM) - 1);
                    goto sort_cleanup;
                }
                for (j = 0; j < out_count; j++) {
                    sort_elem *e = &vec.v[out_start + j];
                    if (nget == 0) {
                        if (obj_list_push(stored, 0, e->val, e->vlen) != 0) {
                            resp_write_error(out, SORT_OOM,
                                             sizeof(SORT_OOM) - 1);
                            goto sort_cleanup;
                        }
                    } else {
                        size_t gi;
                        for (gi = 0; gi < nget; gi++) {
                            const char *s = e->val;
                            size_t sl = e->vlen;
                            if (gets[gi].plen != 1 ||
                                gets[gi].pattern[0] != '#') {
                                int rc = sort_pattern_lookup(
                                    d, out, gets[gi].pattern, gets[gi].plen,
                                    e->val, e->vlen, now_ms, &s, &sl);
                                if (rc < 0)
                                    goto sort_cleanup;
                                if (rc == 0) {
                                    s = "";
                                    sl = 0;
                                }
                            }
                            if (obj_list_push(stored, 0, s, sl) != 0) {
                                resp_write_error(out, SORT_OOM,
                                                 sizeof(SORT_OOM) - 1);
                                goto sort_cleanup;
                            }
                        }
                    }
                }
                {
                    char blob[9];
                    obj_pack_ptr(blob, DDUP_OBJ_LIST, stored);
                    if (db_set_kv(d, storekey, storelen, blob, 9, now_ms) !=
                        0) {
                        resp_write_error(out, SORT_OOM,
                                         sizeof(SORT_OOM) - 1);
                        goto sort_cleanup;
                    }
                    stored = NULL;
                }
                resp_write_integer(out, (long long)output_total);
            }
        } else {
            resp_write_array_header(out, output_total);
            for (j = 0; j < out_count; j++) {
                sort_elem *e = &vec.v[out_start + j];
                if (nget == 0) {
                    resp_write_bulk(out, e->val, e->vlen);
                } else {
                    size_t gi;
                    for (gi = 0; gi < nget; gi++) {
                        const char *s = e->val;
                        size_t sl = e->vlen;
                        if (gets[gi].plen != 1 ||
                            gets[gi].pattern[0] != '#') {
                            int rc = sort_pattern_lookup(
                                d, out, gets[gi].pattern, gets[gi].plen,
                                e->val, e->vlen, now_ms, &s, &sl);
                            if (rc < 0)
                                goto sort_cleanup;
                            if (rc == 0) {
                                s = NULL;
                                sl = 0;
                            }
                        }
                        resp_write_bulk(out, s, sl);
                    }
                }
            }
        }

    sort_cleanup:
        if (stored != NULL)
            obj_list_free(stored);
        for (j = 0; j < nget; j++)
            free(gets[j].pattern);
        free(gets);
        free(tmp);
        sort_vec_free(&vec);
        return;
    }

    if (cmd_id == CMD_GETDEL) {
        if (argc != 2) {
            wrong_args(out, "getdel");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        const char *v;
        size_t vl;
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        {
            const char *s;
            size_t sl2;
            if (!as_string(out, v, vl, &s, &sl2))
                return;
            /* the reply copies the payload, so the delete may free it */
            resp_write_bulk(out, s, sl2);
        }
        db_del_kv(d, k, kl);
        return;
    }

    if (cmd_id == CMD_GETEX) {
        if (argc < 2) {
            wrong_args(out, "getex");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        int has_opt = 0, persist = 0;
        uint64_t when = 0; /* absolute expiry ms (has_opt && !persist) */
        for (size_t i = 2; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "PERSIST") && !has_opt) {
                has_opt = 1;
                persist = 1;
            } else if ((ci_equal(o, ol, "EX") || ci_equal(o, ol, "PX") ||
                        ci_equal(o, ol, "EXAT") || ci_equal(o, ol, "PXAT")) &&
                       !has_opt && i + 1 < argc) {
                const char *t;
                size_t tl;
                long long tv;
                int secs = ci_equal(o, ol, "EX") || ci_equal(o, ol, "EXAT");
                int absolute = ci_equal(o, ol, "EXAT") ||
                               ci_equal(o, ol, "PXAT");
                if (!arg_str(&argv[i + 1], &t, &tl))
                    goto bad_type;
                if (!parse_i64(t, tl, &tv)) {
                    resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (tv <= 0 || (secs && tv > LLONG_MAX / 1000)) {
                    static const char E[] =
                        "ERR invalid expire time in 'getex' command";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                when = (absolute ? 0 : now_ms) +
                       (uint64_t)tv * (secs ? 1000ULL : 1ULL);
                has_opt = 1;
                i++;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        const char *v;
        size_t vl;
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        {
            const char *s;
            size_t sl2;
            if (!as_string(out, v, vl, &s, &sl2))
                return;
            /* replied before any mutation: the payload is safely copied */
            resp_write_bulk(out, s, sl2);
        }
        if (!has_opt)
            return;
        if (persist) {
            const char *e;
            size_t el;
            if (rh_get(&d->expires, k, kl, &e, &el)) {
                rh_del(&d->expires, k, kl);
                d->used_memory -= entry_bytes(kl, 8);
                db_touch_key(d, k, kl);
            }
        } else if (when <= now_ms) {
            /* past expiry: immediate-delete semantics (see cmd_expire) */
            db_del_kv(d, k, kl);
        } else {
            (void)db_set_expiry(d, k, kl, when); /* klen pre-checked */
            db_touch_key(d, k, kl);
        }
        return;
    }

    if (cmd_id == CMD_SETEX || cmd_id == CMD_PSETEX) {
        if (argc != 4) {
            wrong_args(out, cmd_id == CMD_SETEX ? "setex" : "psetex");
            return;
        }
        const char *k, *t, *v;
        size_t kl, tl, vl;
        long long tv;
        int secs = cmd_id == CMD_SETEX;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &t, &tl) ||
            !arg_str(&argv[3], &v, &vl))
            goto bad_type;
        if (!storage_string_ok(kl, vl)) {
            storage_length_error(out);
            return;
        }
        if (!parse_i64(t, tl, &tv)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (tv <= 0 || (secs && tv > LLONG_MAX / 1000)) {
            char msg[96];
            int n = snprintf(msg, sizeof(msg),
                             "ERR invalid expire time in '%s' command",
                             secs ? "setex" : "psetex");
            resp_write_error(out, msg, (size_t)n);
            return;
        }
        {
            const char *old;
            size_t oldl;
            int exists = db_get(d, k, kl, &old, &oldl, now_ms);
            if (exists && obj_tag_of(old, oldl) != DDUP_OBJ_STRING) {
                wrongtype(out);
                return;
            }
        }
        if (oom_blocked(d, out))
            return;
        if (db_set_string(d, k, kl, v, vl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        if (db_set_expiry(d, k, kl,
                          now_ms + (uint64_t)tv * (secs ? 1000ULL : 1ULL)) !=
            0) {
            storage_length_error(out);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_GETSET) {
        if (argc != 3) {
            wrong_args(out, "getset");
            return;
        }
        const char *k, *v;
        size_t kl, vl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &v, &vl))
            goto bad_type;
        if (!storage_string_ok(kl, vl)) {
            storage_length_error(out);
            return;
        }
        if (oom_blocked(d, out))
            return;
        {
            const char *old;
            size_t oldl;
            if (db_get(d, k, kl, &old, &oldl, now_ms)) {
                const char *s;
                size_t sl2;
                if (!as_string(out, old, oldl, &s, &sl2))
                    return;
                /* replied before the overwrite frees the old payload */
                resp_write_bulk(out, s, sl2);
            } else {
                resp_write_bulk(out, NULL, 0);
            }
        }
        /* db_set_string discards any TTL (SET semantics) */
        if (db_set_string(d, k, kl, v, vl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        return;
    }

    if (cmd_id == CMD_SETRANGE) {
        if (argc != 4) {
            wrong_args(out, "setrange");
            return;
        }
        const char *k, *v;
        size_t kl, vl;
        long long off;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[3], &v, &vl))
            goto bad_type;
        {
            const char *ov;
            size_t ovl;
            if (!arg_str(&argv[2], &ov, &ovl))
                goto bad_type;
            if (!parse_i64(ov, ovl, &off)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        if (off < 0) {
            static const char E[] = "ERR offset is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        if ((uint64_t)off + vl > STRING_MAX_BYTES) {
            static const char E[] = "ERR string exceeds maximum allowed size";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        const char *old;
        size_t oldl;
        if (!db_get(d, k, kl, &old, &oldl, now_ms)) {
            if (vl == 0) {
                /* empty write on a missing key: no-op, length 0 */
                resp_write_integer(out, 0);
                return;
            }
            if (oom_blocked(d, out))
                return;
            size_t nl = (size_t)off + vl;
            resp_buf tmp;
            resp_buf_init(&tmp);
            if (nl >= UINT32_MAX || resp_buf_reserve(&tmp, nl) != 0) {
                resp_buf_free(&tmp);
                storage_length_error(out);
                return;
            }
            memset(tmp.data, 0, (size_t)off);
            memcpy(tmp.data + (size_t)off, v, vl);
            tmp.len = nl;
            if (db_set_string(d, k, kl, tmp.data, tmp.len, now_ms) != 0) {
                resp_buf_free(&tmp);
                storage_length_error(out);
                return;
            }
            resp_write_integer(out, (long long)tmp.len);
            resp_buf_free(&tmp);
            return;
        }
        {
            const char *s;
            size_t sl2;
            size_t nl;
            resp_buf tmp;
            if (!as_string(out, old, oldl, &s, &sl2))
                return;
            nl = (size_t)off + vl;
            if (vl == 0 && nl <= sl2) {
                /* nothing changes; report the current length */
                resp_write_integer(out, (long long)sl2);
                return;
            }
            if (nl < sl2)
                nl = sl2;
            if (oom_blocked(d, out))
                return;
            resp_buf_init(&tmp);
            if (nl >= UINT32_MAX || resp_buf_reserve(&tmp, nl) != 0) {
                resp_buf_free(&tmp);
                storage_length_error(out);
                return;
            }
            memcpy(tmp.data, s, sl2);
            if ((size_t)off > sl2)
                memset(tmp.data + sl2, 0, (size_t)off - sl2);
            memcpy(tmp.data + (size_t)off, v, vl);
            tmp.len = nl;
            if (db_set_string(d, k, kl, tmp.data, tmp.len, now_ms) != 0) {
                resp_buf_free(&tmp);
                storage_length_error(out);
                return;
            }
            resp_write_integer(out, (long long)tmp.len);
            resp_buf_free(&tmp);
        }
        return;
    }

    if (cmd_id == CMD_GETRANGE || cmd_id == CMD_SUBSTR) {
        if (argc != 4) {
            wrong_args(out, cmd_id == CMD_SUBSTR ? "substr" : "getrange");
            return;
        }
        const char *k;
        size_t kl;
        long long start, end;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        {
            const char *sv, *ev;
            size_t svl, evl;
            if (!arg_str(&argv[2], &sv, &svl) || !arg_str(&argv[3], &ev, &evl))
                goto bad_type;
            if (!parse_i64(sv, svl, &start) || !parse_i64(ev, evl, &end)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        const char *v;
        size_t vl;
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_bulk(out, "", 0);
            return;
        }
        {
            const char *s;
            size_t sl2;
            long long len;
            if (!as_string(out, v, vl, &s, &sl2))
                return;
            len = (long long)sl2;
            /* Redis bound normalization: negatives from the tail, then clamp */
            if (start < 0 && end < 0 && start > end) {
                resp_write_bulk(out, "", 0);
                return;
            }
            if (start < 0) start += len;
            if (end < 0) end += len;
            if (start < 0) start = 0;
            if (end < 0) end = 0;
            if (end >= len) end = len - 1;
            if (start > end || len == 0) {
                resp_write_bulk(out, "", 0);
                return;
            }
            resp_write_bulk(out, s + start, (size_t)(end - start + 1));
        }
        return;
    }

    if (cmd_id == CMD_INCRBYFLOAT) {
        if (argc != 3) {
            wrong_args(out, "incrbyfloat");
            return;
        }
        const char *k, *dv;
        size_t kl, dvl;
        long double delta, cur = 0, res;
        char buf[5120];
        int nl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &dv, &dvl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        if (!parse_ld(dv, dvl, &delta)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        {
            const char *v;
            size_t vl;
            if (db_get(d, k, kl, &v, &vl, now_ms)) {
                const char *s;
                size_t sl2;
                if (!as_string(out, v, vl, &s, &sl2))
                    return;
                if (!parse_ld(s, sl2, &cur)) {
                    resp_write_error(out, ERR_NOT_FLOAT,
                                     sizeof(ERR_NOT_FLOAT) - 1);
                    return;
                }
            }
        }
        res = cur + delta;
        if (res != res || isinf(res)) {
            static const char E[] =
                "ERR increment would produce NaN or Infinity";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        nl = snprintf(buf, sizeof(buf), "%.17Lg", res);
        if (oom_blocked(d, out))
            return;
        if (db_set_string(d, k, kl, buf, (size_t)nl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        resp_write_bulk(out, buf, (size_t)nl);
        return;
    }

    if (cmd_id == CMD_SETNX) {
        const char *k, *v, *old;
        size_t kl, vl, oldl;
        if (argc != 3) {
            wrong_args(out, "setnx");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &v, &vl))
            goto bad_type;
        if (!storage_string_ok(kl, vl)) {
            storage_length_error(out);
            return;
        }
        if (db_get(d, k, kl, &old, &oldl, now_ms)) {
            resp_write_integer(out, 0);
            return;
        }
        if (oom_blocked(d, out))
            return;
        if (db_set_string(d, k, kl, v, vl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        resp_write_integer(out, 1);
        return;
    }

    if (cmd_id == CMD_GETBIT) {
        const char *k, *off, *v, *sv;
        size_t kl, offl, bit, vl, sl;
        if (argc != 3) {
            wrong_args(out, "getbit");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &off, &offl))
            goto bad_type;
        if (!bitmap_offset(off, offl, &bit)) {
            resp_write_error(out, "ERR bit offset is not an integer or out of range",
                             48);
            return;
        }
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_integer(out, 0);
            return;
        }
        if (!as_string(out, v, vl, &sv, &sl))
            return;
        if (bit / 8 >= sl)
            resp_write_integer(out, 0);
        else
            resp_write_integer(out,
                ((unsigned char)sv[bit / 8] >> (7 - (bit % 8))) & 1U);
        return;
    }

    if (cmd_id == CMD_SETBIT) {
        const char *k, *off, *bv, *v, *sv;
        size_t kl, offl, bvl, bit, vl, sl, bytes, oldlen;
        long long b;
        resp_buf tmp;
        if (argc != 4) {
            wrong_args(out, "setbit");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &off, &offl) ||
            !arg_str(&argv[3], &bv, &bvl))
            goto bad_type;
        if (!bitmap_offset(off, offl, &bit)) {
            resp_write_error(out, "ERR bit offset is not an integer or out of range",
                             48);
            return;
        }
        if (!parse_i64(bv, bvl, &b) || (b != 0 && b != 1)) {
            resp_write_error(out, "ERR bit is not an integer or out of range", 41);
            return;
        }
        if (bit == SIZE_MAX || bit / 8 >= UINT32_MAX) {
            storage_length_error(out);
            return;
        }
        bytes = bit / 8 + 1;
        sv = NULL;
        sl = 0;
        if (db_get(d, k, kl, &v, &vl, now_ms) && !as_string(out, v, vl, &sv, &sl))
            return;
        oldlen = sl;
        if (bytes < oldlen)
            bytes = oldlen;
        if (!storage_string_ok(kl, bytes)) {
            storage_length_error(out);
            return;
        }
        if (oom_blocked(d, out))
            return;
        resp_buf_init(&tmp);
        if (resp_buf_reserve(&tmp, bytes) != 0) {
            resp_buf_free(&tmp);
            storage_length_error(out);
            return;
        }
        if (oldlen != 0)
            memcpy(tmp.data, sv, oldlen);
        if (bytes > oldlen)
            memset(tmp.data + oldlen, 0, bytes - oldlen);
        tmp.len = bytes;
        {
            unsigned char mask = (unsigned char)(1U << (7 - (bit % 8)));
            int old = (tmp.data[bit / 8] & mask) != 0;
            if (b)
                tmp.data[bit / 8] |= mask;
            else
                tmp.data[bit / 8] &= (unsigned char)~mask;
            if (db_set_string(d, k, kl, tmp.data, tmp.len, now_ms) != 0) {
                resp_buf_free(&tmp);
                storage_length_error(out);
                return;
            }
            resp_buf_free(&tmp);
            resp_write_integer(out, old);
        }
        return;
    }

    if (cmd_id == CMD_BITCOUNT) {
        const char *k, *v, *sv;
        size_t kl, vl, sl, begin = 0, end;
        long long start, stop, count = 0;
        if (argc != 2 && argc != 4) {
            wrong_args(out, "bitcount");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_integer(out, 0);
            return;
        }
        if (!as_string(out, v, vl, &sv, &sl))
            return;
        if (sl == 0) {
            resp_write_integer(out, 0);
            return;
        }
        end = sl - 1;
        if (argc == 4) {
            const char *a, *z;
            size_t al, zl;
            if (!arg_str(&argv[2], &a, &al) || !arg_str(&argv[3], &z, &zl))
                goto bad_type;
            if (!parse_i64(a, al, &start) || !parse_i64(z, zl, &stop)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (start < 0)
                start += (long long)sl;
            if (stop < 0)
                stop += (long long)sl;
            if (start < 0)
                start = 0;
            if (stop < 0 || start >= (long long)sl || start > stop) {
                resp_write_integer(out, 0);
                return;
            }
            begin = (size_t)start;
            end = (size_t)(stop >= (long long)sl ? (long long)sl - 1 : stop);
        }
        while (begin <= end)
            count += (long long)bitmap_popcount_byte((unsigned char)sv[begin++]);
        resp_write_integer(out, count);
        return;
    }

    if (cmd_id == CMD_BITPOS) {
        const char *k, *bv, *v, *sv;
        size_t kl, bvl, vl, sl, begin = 0, end;
        long long b, start, stop;
        int have_end = 0;
        if (argc < 3 || argc > 5) {
            wrong_args(out, "bitpos");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &bv, &bvl))
            goto bad_type;
        if (!parse_i64(bv, bvl, &b) || (b != 0 && b != 1)) {
            resp_write_error(out, "ERR The bit argument must be 1 or 0", 35);
            return;
        }
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_integer(out, b == 0 ? 0 : -1);
            return;
        }
        if (!as_string(out, v, vl, &sv, &sl))
            return;
        if (sl == 0) {
            resp_write_integer(out, b == 0 ? 0 : -1);
            return;
        }
        end = sl - 1;
        if (argc >= 4) {
            const char *a;
            size_t al;
            if (!arg_str(&argv[3], &a, &al) || !parse_i64(a, al, &start)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (start < 0)
                start += (long long)sl;
            if (start < 0)
                start = 0;
            if (start >= (long long)sl) {
                resp_write_integer(out, -1);
                return;
            }
            begin = (size_t)start;
        }
        if (argc == 5) {
            const char *z;
            size_t zl;
            have_end = 1;
            if (!arg_str(&argv[4], &z, &zl) || !parse_i64(z, zl, &stop)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (stop < 0)
                stop += (long long)sl;
            if (stop < 0 || begin > (size_t)stop) {
                resp_write_integer(out, -1);
                return;
            }
            end = (size_t)(stop >= (long long)sl ? (long long)sl - 1 : stop);
        }
        for (; begin <= end; begin++) {
            long long p = bitmap_pos_byte((unsigned char)sv[begin], (int)b);
            if (p >= 0) {
                resp_write_integer(out, (long long)begin * 8 + p);
                return;
            }
        }
        resp_write_integer(out, b == 0 && !have_end ? (long long)sl * 8 : -1);
        return;
    }

    if (cmd_id == CMD_BITOP) {
        cmd_bitop(d, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_BITFIELD || cmd_id == CMD_BITFIELD_RO) {
        cmd_bitfield(d, argv, argc, out, now_ms, cmd_id == CMD_BITFIELD_RO);
        return;
    }

    if (cmd_id == CMD_MSETNX) {
        size_t i;
        if (argc < 3 || argc % 2 == 0) {
            wrong_args(out, "msetnx");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        /* First pass validates and probes every key. No mutation happens
         * before the condition is known, preserving Redis all-or-nothing. */
        for (i = 1; i + 1 < argc; i += 2) {
            const char *k, *v, *old;
            size_t kl, vl, oldl;
            if (!arg_str(&argv[i], &k, &kl) ||
                !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            if (!storage_string_ok(kl, vl)) {
                storage_length_error(out);
                return;
            }
            if (db_get(d, k, kl, &old, &oldl, now_ms)) {
                resp_write_integer(out, 0);
                return;
            }
        }
        if (oom_blocked(d, out))
            return;
        for (i = 1; i + 1 < argc; i += 2) {
            const char *k, *v;
            size_t kl, vl;
            (void)arg_str(&argv[i], &k, &kl);
            (void)arg_str(&argv[i + 1], &v, &vl);
            if (db_set_string(d, k, kl, v, vl, now_ms) != 0) {
                storage_length_error(out);
                return;
            }
        }
        resp_write_integer(out, 1);
        return;
    }
    if (cmd_id == CMD_MGET) {
        if (argc < 2) {
            wrong_args(out, "mget");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        /* validate types first: the reply must be a single RESP value */
        for (size_t i = 1; i < argc; i++) {
            const char *k, *v;
            size_t kl, vl;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
            if (db_get(d, k, kl, &v, &vl, now_ms) &&
                obj_tag_of(v, vl) != DDUP_OBJ_STRING) {
                wrongtype(out);
                return;
            }
        }
        resp_write_array_header(out, argc - 1);
        for (size_t i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
            const char *v;
            size_t vl;
            if (!db_get(d, k, kl, &v, &vl, now_ms)) {
                resp_write_bulk(out, NULL, 0);
                continue;
            }
            {
                const char *s;
                size_t sl2;
                obj_str(v, vl, &s, &sl2);
                resp_write_bulk(out, s, sl2);
            }
        }
        return;
    }

    if (cmd_id == CMD_MSET) {
        if (argc < 3 || argc % 2 == 0) {
            wrong_args(out, "mset");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (oom_blocked(d, out))
            return;
        for (size_t i = 1; i + 1 < argc; i += 2) {
            const char *k, *v;
            size_t kl, vl;
            if (!arg_str(&argv[i], &k, &kl) ||
                !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            if (!storage_string_ok(kl, vl)) {
                storage_length_error(out);
                return;
            }
        }
        for (size_t i = 1; i + 1 < argc; i += 2) {
            const char *k, *v;
            size_t kl, vl;
            if (!arg_str(&argv[i], &k, &kl) || !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            if (db_set_string(d, k, kl, v, vl, now_ms) != 0) {
                storage_length_error(out);
                return;
            }
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_EXPIRE || cmd_id == CMD_PEXPIRE ||
        cmd_id == CMD_EXPIREAT || cmd_id == CMD_PEXPIREAT) {
        int seconds = cmd_id == CMD_EXPIRE ||
                      cmd_id == CMD_EXPIREAT;
        int absolute = cmd_id == CMD_EXPIREAT ||
                       cmd_id == CMD_PEXPIREAT;
        const char *cname = seconds ? (absolute ? "expireat" : "expire")
                                    : (absolute ? "pexpireat" : "pexpire");
        if (argc < 3 || argc > 7) {
            wrong_args(out, cname);
            return;
        }
        cmd_expire(d, argv, argc, out, now_ms, seconds ? 1000 : 1,
                   absolute, cname);
        return;
    }

    if (cmd_id == CMD_TTL || cmd_id == CMD_PTTL) {
        if (argc != 2) {
            wrong_args(out, "ttl");
            return;
        }
        cmd_ttl(d, argv, out, now_ms, cmd_id == CMD_PTTL);
        return;
    }

    if (cmd_id == CMD_PERSIST) {
        if (argc != 2) {
            wrong_args(out, "persist");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        db_expire_if_needed(d, k, kl, now_ms);
        {
            const char *v;
            size_t vl;
            if (rh_get(&d->expires, k, kl, &v, &vl)) {
                rh_del(&d->expires, k, kl);
                d->used_memory -= entry_bytes(kl, 8);
                resp_write_integer(out, 1);
            } else {
                resp_write_integer(out, 0);
            }
        }
        return;
    }

    if (cmd_id == CMD_TYPE) {
        const char *k, *v;
        size_t kl, vl;
        if (argc != 2) {
            wrong_args(out, "type");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_simple_string(out, "none", 4);
            return;
        }
        {
            const char *tn = obj_type_name(obj_tag_of(v, vl));
            resp_write_simple_string(out, tn, strlen(tn));
        }
        return;
    }

    if (cmd_id == CMD_OBJECT) {
        /* only the ENCODING subcommand (Redis 7 encoding names) */
        const char *sub, *k, *v;
        size_t subl, kl, vl;
        if (argc < 2) {
            wrong_args(out, "object");
            return;
        }
        if (!arg_str(&argv[1], &sub, &subl))
            goto bad_type;
        if (ci_equal(sub, subl, "HELP") && argc == 2) {
            static const char *help[] = {
                "ENCODING <key>",
                "FREQ <key>",
                "IDLETIME <key>",
                "REFCOUNT <key>"
            };
            size_t i;
            resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
            for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
                resp_write_bulk(out, help[i], strlen(help[i]));
            return;
        }
        if (argc != 3) {
            wrong_args(out, "object");
            return;
        }
        if (!arg_str(&argv[2], &k, &kl))
            goto bad_type;
        db_expire_if_needed(d, k, kl, now_ms);
        if (!rh_get(&d->table, k, kl, &v, &vl)) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        if (ci_equal(sub, subl, "REFCOUNT")) {
            resp_write_integer(out, 1);
            return;
        }
        if (ci_equal(sub, subl, "FREQ")) {
            resp_write_integer(out, 0);
            return;
        }
        if (ci_equal(sub, subl, "IDLETIME")) {
            uint32_t age = lru_clock(now_ms) - rh_meta_of(&d->table, k, kl);
            resp_write_integer(out, age);
            return;
        }
        if (!ci_equal(sub, subl, "ENCODING")) {
            resp_write_error(out, "ERR unknown OBJECT subcommand", 29);
            return;
        }
        {
            const char *enc;
            switch (obj_tag_of(v, vl)) {
            case DDUP_OBJ_HASH:
                enc = obj_hash_is_listpack((obj_hash *)obj_unpack_ptr(v, vl))
                          ? "listpack"
                          : "hashtable";
                break;
            case DDUP_OBJ_LIST:
                enc = "quicklist";
                break;
            case DDUP_OBJ_SET:
                enc = obj_set_is_listpack((obj_set *)obj_unpack_ptr(v, vl))
                          ? "listpack"
                          : "hashtable";
                break;
            case DDUP_OBJ_ZSET:
                enc = obj_zset_is_listpack((obj_zset *)obj_unpack_ptr(v, vl))
                          ? "listpack"
                          : "skiplist";
                break;
            default:
                enc = "raw"; /* strings carry no int/embstr optimization */
                break;
            }
            resp_write_bulk(out, enc, strlen(enc));
        }
        return;
    }

    if (cmd_id == CMD_KEYS) {
        const char *pat;
        size_t plen;
        keys_ctx kc;
        size_t hdr_pos;
        if (argc != 2) {
            wrong_args(out, "keys");
            return;
        }
        if (!arg_str(&argv[1], &pat, &plen))
            goto bad_type;
        kc.d = d;
        kc.now_ms = now_ms;
        kc.pat = pat;
        kc.plen = plen;
        kc.out = out;
        kc.nmatch = 0;
        hdr_pos = out->len;
        rh_scan(&d->table, 0, SIZE_MAX, keys_cb, &kc);
        resp_insert_array_header(out, hdr_pos, kc.nmatch);
        return;
    }

    if (cmd_id == CMD_SCAN) {
        const char *cur, *pat = NULL;
        size_t curl, plen = 0;
        size_t cursor = 0, i;
        long long count = 10; /* COUNT is a hint, like Redis */
        scan_ctx sc;
        char next[24];
        int nextlen;
        if (argc < 2) {
            wrong_args(out, "scan");
            return;
        }
        if (!arg_str(&argv[1], &cur, &curl))
            goto bad_type;
        if (!parse_cursor(cur, curl, &cursor)) {
            resp_write_error(out, "ERR invalid cursor", 18);
            return;
        }
        for (i = 2; i < argc; i += 2) {
            const char *opt, *ov;
            size_t optl, ovl;
            if (i + 1 >= argc)
                goto scan_syntax;
            if (!arg_str(&argv[i], &opt, &optl) ||
                !arg_str(&argv[i + 1], &ov, &ovl))
                goto bad_type;
            if (ci_equal(opt, optl, "MATCH")) {
                pat = ov;
                plen = ovl;
            } else if (ci_equal(opt, optl, "COUNT")) {
                if (!parse_i64(ov, ovl, &count) || count <= 0)
                    goto scan_syntax;
            } else {
                goto scan_syntax;
            }
        }
        sc.d = d;
        sc.now_ms = now_ms;
        sc.pat = pat;
        sc.plen = plen;
        sc.n = 0;
        cursor = rh_scan(&d->table, cursor, (size_t)count, scan_cb, &sc);
        nextlen = snprintf(next, sizeof(next), "%llu",
                           (unsigned long long)cursor);
        resp_write_array_header(out, 2);
        resp_write_bulk(out, next, (size_t)nextlen);
        resp_write_array_header(out, sc.n);
        for (i = 0; i < sc.n; i++)
            resp_write_bulk(out, sc.keys[i], sc.klens[i]);
        return;

    scan_syntax:
        resp_write_error(out, "ERR syntax error", 16);
        return;
    }

    if (cmd_id == CMD_HSCAN) {
        const char *k;
        size_t kl;
        obj_hash *h;
        int rc;
        scan_object_opt opt;
        hscan_ctx hc;
        size_t cursor;
        size_t i;
        char next[24];
        int nextlen;
        if (argc < 3) {
            wrong_args(out, "hscan");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (parse_object_scan(argv, argc, out, &opt) != 0)
            return;
        rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 2);
            resp_write_bulk(out, "0", 1);
            resp_write_array_header(out, 0);
            return;
        }
        if (obj_hash_is_listpack(h)) {
            size_t n = 0;
            cursor = hscan_lp_next(h, opt.cursor, (size_t)opt.count,
                                   opt.pat, opt.plen, &n);
            nextlen = snprintf(next, sizeof(next), "%llu",
                               (unsigned long long)cursor);
            resp_write_array_header(out, 2);
            resp_write_bulk(out, next, (size_t)nextlen);
            resp_write_array_header(out, n * 2);
            hscan_lp_emit(h, opt.cursor, (size_t)opt.count, opt.pat,
                          opt.plen, out);
        } else {
            hc.pat = opt.pat;
            hc.plen = opt.plen;
            cursor = hscan_ht_run(h, opt.cursor, (size_t)opt.count, &hc);
            nextlen = snprintf(next, sizeof(next), "%llu",
                               (unsigned long long)cursor);
            resp_write_array_header(out, 2);
            resp_write_bulk(out, next, (size_t)nextlen);
            resp_write_array_header(out, hc.n * 2);
            for (i = 0; i < hc.n; i++) {
                resp_write_bulk(out, hc.fields[i], hc.flens[i]);
                resp_write_bulk(out, hc.vals[i], hc.vlens[i]);
            }
        }
        return;
    }

    if (cmd_id == CMD_SSCAN) {
        const char *k;
        size_t kl;
        obj_set *s;
        int rc;
        scan_object_opt opt;
        sscan_ctx sc;
        size_t cursor;
        size_t i;
        char next[24];
        int nextlen;
        if (argc < 3) {
            wrong_args(out, "sscan");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (parse_object_scan(argv, argc, out, &opt) != 0)
            return;
        rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 2);
            resp_write_bulk(out, "0", 1);
            resp_write_array_header(out, 0);
            return;
        }
        if (obj_set_is_listpack(s)) {
            size_t n = 0;
            cursor = sscan_lp_next(s, opt.cursor, (size_t)opt.count,
                                   opt.pat, opt.plen, &n);
            nextlen = snprintf(next, sizeof(next), "%llu",
                               (unsigned long long)cursor);
            resp_write_array_header(out, 2);
            resp_write_bulk(out, next, (size_t)nextlen);
            resp_write_array_header(out, n);
            sscan_lp_emit(s, opt.cursor, (size_t)opt.count, opt.pat,
                          opt.plen, out);
        } else {
            sc.pat = opt.pat;
            sc.plen = opt.plen;
            cursor = sscan_ht_run(s, opt.cursor, (size_t)opt.count, &sc);
            nextlen = snprintf(next, sizeof(next), "%llu",
                               (unsigned long long)cursor);
            resp_write_array_header(out, 2);
            resp_write_bulk(out, next, (size_t)nextlen);
            resp_write_array_header(out, sc.n);
            for (i = 0; i < sc.n; i++)
                resp_write_bulk(out, sc.members[i], sc.mlens[i]);
        }
        return;
    }

    if (cmd_id == CMD_ZSCAN) {
        const char *k;
        size_t kl;
        obj_zset *z;
        int rc;
        scan_object_opt opt;
        zscan_ctx zc;
        size_t cursor;
        size_t i;
        char next[24];
        int nextlen;
        if (argc < 3) {
            wrong_args(out, "zscan");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (parse_object_scan(argv, argc, out, &opt) != 0)
            return;
        rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 2);
            resp_write_bulk(out, "0", 1);
            resp_write_array_header(out, 0);
            return;
        }
        if (obj_zset_is_listpack(z)) {
            size_t n = 0;
            cursor = zscan_lp_next(z, opt.cursor, (size_t)opt.count,
                                   opt.pat, opt.plen, &n);
            nextlen = snprintf(next, sizeof(next), "%llu",
                               (unsigned long long)cursor);
            resp_write_array_header(out, 2);
            resp_write_bulk(out, next, (size_t)nextlen);
            resp_write_array_header(out, n * 2);
            zscan_lp_emit(z, opt.cursor, (size_t)opt.count, opt.pat,
                          opt.plen, out);
        } else {
            zc.pat = opt.pat;
            zc.plen = opt.plen;
            cursor = zscan_ht_run(z, opt.cursor, (size_t)opt.count, &zc);
            nextlen = snprintf(next, sizeof(next), "%llu",
                               (unsigned long long)cursor);
            resp_write_array_header(out, 2);
            resp_write_bulk(out, next, (size_t)nextlen);
            resp_write_array_header(out, zc.n * 2);
            for (i = 0; i < zc.n; i++) {
                char num[40];
                int nl = fmt_score(num, sizeof(num), zc.scores[i]);
                resp_write_bulk(out, zc.members[i], zc.mlens[i]);
                resp_write_bulk(out, num, (size_t)nl);
            }
        }
        return;
    }

    if (cmd_id == CMD_RENAME || cmd_id == CMD_RENAMENX) {
        const char *sk, *dk, *v, *e;
        size_t skl, dkl, vl, el;
        int nx = cmd_id == CMD_RENAMENX;
        if (argc != 3) {
            wrong_args(out, nx ? "renamenx" : "rename");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (!arg_str(&argv[1], &sk, &skl) || !arg_str(&argv[2], &dk, &dkl))
            goto bad_type;
        if (!storage_key_ok(skl) || !storage_key_ok(dkl)) {
            storage_length_error(out);
            return;
        }
        db_expire_if_needed(d, sk, skl, now_ms);
        if (skl != dkl || memcmp(sk, dk, skl) != 0)
            db_expire_if_needed(d, dk, dkl, now_ms);
        /* src existence is checked before the same-key check (Redis order) */
        if (!rh_get(&d->table, sk, skl, &v, &vl)) {
            resp_write_error(out, "ERR no such key", 15);
            return;
        }
        if (skl == dkl && memcmp(sk, dk, skl) == 0) {
            static const char E[] =
                "ERR source and destination objects are the same";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (nx && rh_get(&d->table, dk, dkl, &e, &el)) {
            resp_write_integer(out, 0);
            return;
        }
        if (oom_blocked(d, out))
            return;
        /* move value (db_set_kv copies the blob and clears any dst TTL),
         * then carry over the src TTL, then drop src; the version/dirty
         * bookkeeping for both keys comes from db_set_kv/db_del_kv_keep_obj */
        if (db_set_kv(d, dk, dkl, v, vl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        if (rh_get(&d->expires, sk, skl, &e, &el) && el == 8)
            (void)db_set_expiry(d, dk, dkl, get_u64(e));
        db_del_kv_keep_obj(d, sk, skl);
        if (nx)
            resp_write_integer(out, 1);
        else
            resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_COPY) {
        const char *sk, *dk, *e;
        size_t skl, dkl, el;
        int replace = 0, has_db = 0;
        long long tdb = 0;
        size_t i;
        db *td;
        uint64_t expire_ms = 0;
        resp_buf payload;
        int rrc;
        if (argc < 3) {
            wrong_args(out, "copy");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (!arg_str(&argv[1], &sk, &skl) || !arg_str(&argv[2], &dk, &dkl))
            goto bad_type;
        for (i = 3; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "REPLACE") && !replace) {
                replace = 1;
            } else if (ci_equal(o, ol, "DB") && !has_db && i + 1 < argc) {
                const char *t;
                size_t tl;
                if (!arg_str(&argv[i + 1], &t, &tl))
                    goto bad_type;
                if (!parse_i64(t, tl, &tdb)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                has_db = 1;
                i++;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (!has_db)
            tdb = s->db_index;
        if (tdb < 0 || (tdb != s->db_index &&
                        (s->sel_fn == NULL || tdb >= s->sel_ndbs))) {
            static const char E[] = "ERR DB index is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (!storage_key_ok(skl) || !storage_key_ok(dkl)) {
            storage_length_error(out);
            return;
        }
        td = tdb == s->db_index ? d : s->sel_fn(s->sel_ctx, (int)tdb);
        if (td == NULL) {
            static const char E[] = "ERR DB index is out of range";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        /* serialize-then-install (DUMP/RESTORE path): pointer-backed objects
         * (hash/list/set/zset) share the object pointer inside a value blob,
         * so a raw blob copy would alias it (double free on the first
         * delete); the serialized payload is also alias-safe for
         * src == dst REPLACE. The absolute expiry instant is carried over. */
        db_expire_if_needed(d, sk, skl, now_ms);
        resp_buf_init(&payload);
        if (snapshot_dump_key(d, sk, skl, &payload) != 0) {
            resp_buf_free(&payload);
            resp_write_error(out, "ERR no such key", 15);
            return;
        }
        if (rh_get(&d->expires, sk, skl, &e, &el) && el == 8)
            expire_ms = get_u64(e);
        if (oom_blocked(td, out)) {
            resp_buf_free(&payload);
            return;
        }
        rrc = snapshot_restore_key(td, dk, dkl, payload.data, payload.len,
                                   expire_ms, replace, now_ms);
        resp_buf_free(&payload);
        if (rrc == 1) {
            static const char E[] = "ERR Target key already exists";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (rrc != 0) {
            storage_length_error(out);
            return;
        }
        if (td != d)
            /* cross-db: the target db's dirty/watch bookkeeping came from
             * the install; bump the session db too so the AOF/propagation
             * hook logs COPY itself (SWAPDB precedent). Replay is
             * self-contained: argv carries the DB option. */
            s->d->dirty++;
        resp_write_integer(out, 1);
        return;
    }

    if (cmd_id == CMD_TOUCH) {
        long long found = 0;
        size_t i;
        if (argc < 2) {
            wrong_args(out, "touch");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        for (i = 1; i < argc; i++) {
            const char *k, *v;
            size_t kl, vl;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
            found += db_get(d, k, kl, &v, &vl, now_ms);
        }
        resp_write_integer(out, found);
        return;
    }

    if (cmd_id == CMD_RANDOMKEY) {
        const char *k, *v;
        size_t kl, vl;
        int attempt;
        if (argc != 1) {
            wrong_args(out, "randomkey");
            return;
        }
        /* random probes first; expired samples are collected on the spot */
        for (attempt = 0; attempt < 100 && rh_size(&d->table) > 0;
             attempt++) {
            if (!rh_random_entry(&d->table, db_rand(d), &k, &kl, &v, &vl,
                                 NULL))
                break;
            if (db_expire_if_needed(d, k, kl, now_ms))
                continue;
            resp_write_bulk(out, k, kl);
            return;
        }
        /* deterministic fallback: first live key in bucket order */
        if (rh_size(&d->table) > 0) {
            randomkey_ctx rc;
            rc.d = d;
            rc.now_ms = now_ms;
            rc.key = NULL;
            rc.klen = 0;
            rh_scan(&d->table, 0, SIZE_MAX, randomkey_cb, &rc);
            if (rc.key != NULL) {
                resp_write_bulk(out, rc.key, rc.klen);
                return;
            }
        }
        resp_write_bulk(out, NULL, 0);
        return;
    }

    if (cmd_id == CMD_EXPIRETIME || cmd_id == CMD_PEXPIRETIME) {
        const char *k, *v;
        size_t kl, vl;
        if (argc != 2) {
            wrong_args(out, cmd_id == CMD_PEXPIRETIME ? "pexpiretime"
                                                      : "expiretime");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_integer(out, -2); /* no such key */
            return;
        }
        if (!rh_get(&d->expires, k, kl, &v, &vl) || vl != 8) {
            resp_write_integer(out, -1); /* no expiry */
            return;
        }
        {
            uint64_t when = get_u64(v);
            resp_write_integer(out, cmd_id == CMD_PEXPIRETIME
                                        ? (long long)when
                                        : (long long)(when / 1000));
        }
        return;
    }

    if (cmd_id == CMD_DBSIZE) {
        if (argc != 1) {
            wrong_args(out, "dbsize");
            return;
        }
        /* O(1) size; may include expired keys not yet collected. */
        resp_write_integer(out, (long long)rh_size(&d->table));
        return;
    }

    if (cmd_id == CMD_FLUSHDB) {
        if (argc != 1) {
            wrong_args(out, "flushdb");
            return;
        }
        flush_db_contents(d);
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_FLUSHALL) {
        int i;
        if (argc != 1) {
            wrong_args(out, "flushall");
            return;
        }
        if (s->sel_fn != NULL) {
            for (i = 0; i < s->sel_ndbs; i++)
                flush_db_contents(s->sel_fn(s->sel_ctx, i));
        } else {
            flush_db_contents(d);
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_TIME) {
        char sec[32], usec[32];
        int slen, ulen;
        if (argc != 1) {
            wrong_args(out, "time");
            return;
        }
        slen = snprintf(sec, sizeof(sec), "%llu",
                        (unsigned long long)(now_ms / 1000ULL));
        ulen = snprintf(usec, sizeof(usec), "%llu",
                        (unsigned long long)((now_ms % 1000ULL) * 1000ULL));
        resp_write_array_header(out, 2);
        resp_write_bulk(out, sec, (size_t)slen);
        resp_write_bulk(out, usec, (size_t)ulen);
        return;
    }

    if (cmd_id == CMD_LOLWUT) {
        static const char art[] =
            "Redis ver. 7.2.15\n"
            "   /\\_/\\\n"
            "  ( o.o )\n"
            "   > ^ <\n";
        long long version = 6;
        if (argc == 3) {
            const char *opt, *vv;
            size_t ol, vl;
            if (!arg_str(&argv[1], &opt, &ol) ||
                !arg_str(&argv[2], &vv, &vl))
                goto bad_type;
            if (!ci_equal(opt, ol, "VERSION")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            if (!parse_i64(vv, vl, &version)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        } else if (argc != 1) {
            wrong_args(out, "lolwut");
            return;
        }
        if (version != 5 && version != 6) {
            resp_write_error(out, "ERR Invalid version specified", 29);
            return;
        }
        resp_write_bulk(out, art, strlen(art));
        return;
    }

    if (cmd_id == CMD_CONFIG) {
        if (argc < 2) {
            wrong_args(out, "config");
            return;
        }
        const char *sub;
        size_t sl;
        if (!arg_str(&argv[1], &sub, &sl))
            goto bad_type;
        if (ci_equal(sub, sl, "HELP") && argc == 2) {
            static const char *help[] = {
                "GET <parameter>",
                "SET <parameter> <value>",
                "RESETSTAT",
                "REWRITE"
            };
            size_t i;
            resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
            for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
                resp_write_bulk(out, help[i], strlen(help[i]));
            return;
        }
        if (ci_equal(sub, sl, "GET")) {
            const char *p;
            size_t pl;
            if (argc != 3) {
                wrong_args(out, "config");
                return;
            }
            if (!arg_str(&argv[2], &p, &pl))
                goto bad_type;
            if (ci_equal(p, pl, "maxmemory")) {
                char num[24];
                int nl2 = snprintf(num, sizeof(num), "%llu",
                                   (unsigned long long)d->maxmemory);
                resp_write_array_header(out, 2);
                resp_write_bulk(out, "maxmemory", 9);
                resp_write_bulk(out, num, (size_t)nl2);
            } else if (ci_equal(p, pl, "maxmemory-policy")) {
                const char *pn = policy_name(d->maxmemory_policy);
                resp_write_array_header(out, 2);
                resp_write_bulk(out, "maxmemory-policy", 16);
                resp_write_bulk(out, pn, strlen(pn));
            } else if (ci_equal(p, pl, "tiered-storage")) {
                resp_write_array_header(out, 2);
                resp_write_bulk(out, "tiered-storage", 14);
                resp_write_bulk(out, d->tier_enabled ? "yes" : "no",
                                d->tier_enabled ? 3 : 2);
            } else if (ci_equal(p, pl, "tiered-storage-dir")) {
                resp_write_array_header(out, 2);
                resp_write_bulk(out, "tiered-storage-dir",
                                sizeof("tiered-storage-dir") - 1);
                resp_write_bulk(out, d->tier_dir, strlen(d->tier_dir));
            } else if (ci_equal(p, pl, "tiered-storage-max-disk-bytes")) {
                char num[24];
                int nl2 = snprintf(num, sizeof(num), "%llu",
                                   (unsigned long long)d->tier_max_disk_bytes);
                resp_write_array_header(out, 2);
                resp_write_bulk(out, "tiered-storage-max-disk-bytes",
                                sizeof("tiered-storage-max-disk-bytes") - 1);
                resp_write_bulk(out, num, (size_t)nl2);
            } else {
                resp_write_array_header(out, 0);
            }
            return;
        }
        if (ci_equal(sub, sl, "RESETSTAT") && argc == 2) {
            if (s->sel_fn != NULL) {
                int i;
                for (i = 0; i < s->sel_ndbs; i++) {
                    db *di = s->sel_fn(s->sel_ctx, i);
                    memset(di->cmd_calls, 0, sizeof(di->cmd_calls));
                    memset(di->cmd_usecs, 0, sizeof(di->cmd_usecs));
                }
            } else {
                memset(d->cmd_calls, 0, sizeof(d->cmd_calls));
                memset(d->cmd_usecs, 0, sizeof(d->cmd_usecs));
            }
            resp_write_simple_string(out, "OK", 2);
            return;
        }
        if (ci_equal(sub, sl, "REWRITE") && argc == 2) {
            static const char E[] =
                "ERR The server is running without a config file";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (ci_equal(sub, sl, "SET")) {
            const char *p, *v;
            size_t pl, vl2;
            if (argc != 4) {
                wrong_args(out, "config");
                return;
            }
            if (!arg_str(&argv[2], &p, &pl) || !arg_str(&argv[3], &v, &vl2))
                goto bad_type;
            if (ci_equal(p, pl, "maxmemory")) {
                long long mv;
                if (!parse_i64(v, vl2, &mv)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (mv < 0) {
                    static const char E[] =
                        "ERR invalid argument for CONFIG SET 'maxmemory'";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                /* Redis semantics: global limit, applied to every db */
                if (s->sel_fn != NULL) {
                    int i;
                    for (i = 0; i < s->sel_ndbs; i++)
                        s->sel_fn(s->sel_ctx, i)->maxmemory = (uint64_t)mv;
                } else {
                    d->maxmemory = (uint64_t)mv;
                }
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(p, pl, "maxmemory-policy")) {
                int pol;
                if (ci_equal(v, vl2, "allkeys-lru")) {
                    pol = DB_POLICY_ALLKEYS_LRU;
                } else if (ci_equal(v, vl2, "noeviction")) {
                    pol = DB_POLICY_NOEVICTION;
                } else {
                    static const char E[] =
                        "ERR invalid argument for CONFIG SET "
                        "'maxmemory-policy'";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                if (s->sel_fn != NULL) {
                    int i;
                    for (i = 0; i < s->sel_ndbs; i++)
                        s->sel_fn(s->sel_ctx, i)->maxmemory_policy = pol;
                } else {
                    d->maxmemory_policy = pol;
                }
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(p, pl, "tiered-storage")) {
                int enable;
                if (ci_equal(v, vl2, "yes")) {
                    enable = 1;
                } else if (ci_equal(v, vl2, "no")) {
                    enable = 0;
                } else {
                    static const char E[] =
                        "ERR invalid argument for CONFIG SET "
                        "'tiered-storage'";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                if (s->sel_fn != NULL) {
                    int i;
                    for (i = 0; i < s->sel_ndbs; i++)
                        s->sel_fn(s->sel_ctx, i)->tier_enabled = enable;
                } else {
                    d->tier_enabled = enable;
                }
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            {
                char msg[128];
                int n2 = snprintf(msg, sizeof(msg),
                                  "ERR Unsupported CONFIG parameter: %.*s",
                                  (int)pl, p);
                resp_write_error(out, msg, (size_t)n2);
            }
            return;
        }
        resp_write_error(out, "ERR unknown CONFIG subcommand", 29);
        return;
    }

    if (cmd_id == CMD_INFO) {
        int machine = 0;
        if (argc == 2) {
            const char *sec;
            size_t seclen;
            if (!arg_str(&argv[1], &sec, &seclen) ||
                !ci_equal(sec, seclen, "__STATS__")) {
                static const char E[] = "ERR unsupported INFO section";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            machine = 1;
        } else if (argc != 1) {
            wrong_args(out, "info");
            return;
        }
        {
            info_stats st;
            info_fill(s, &st);
            if (machine)
                info_format_stats(&st, out);
            else
                command_info_render(d, s->repl, &st, out);
        }
        return;
    }

    /* ---------------- hash commands ---------------- */

    if (cmd_id == CMD_HSET || cmd_id == CMD_HMSET) {
        int mset = cmd_id == CMD_HMSET;
        if (argc < 4 || argc % 2 != 0) {
            wrong_args(out, mset ? "hmset" : "hset");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        if (oom_blocked(d, out))
            return;
        for (size_t i = 2; i + 1 < argc; i += 2) {
            const char *f, *v;
            size_t fl, vl;
            if (!arg_str(&argv[i], &f, &fl) ||
                !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            if (!storage_string_ok(fl, vl)) {
                storage_length_error(out);
                return;
            }
        }
        obj_hash *h;
        if (get_hash(d, out, k, kl, 1, now_ms, &h) <= 0)
            return;
        long long added = 0;
        uint64_t before = obj_hash_mem(h);
        for (size_t i = 2; i + 1 < argc; i += 2) {
            const char *f, *v;
            size_t fl, vl;
            if (!arg_str(&argv[i], &f, &fl) || !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            {
                int set_rc = obj_hash_set_at(h, f, fl, v, vl, now_ms, 0);
                if (set_rc < 0) {
                    storage_length_error(out);
                    return;
                }
                added += set_rc;
            }
        }
        mem_sync(d, k, kl, before, obj_hash_mem(h));
        if (mset)
            resp_write_simple_string(out, "OK", 2);
        else
            resp_write_integer(out, added);
        return;
    }


    if (cmd_id == CMD_HSETEX || cmd_id == CMD_HGETEX) {
        const char *k;
        size_t kl;
        size_t pos = 2, fields_idx, num_fields, first_field;
        uint64_t expire_ms = 0;
        int expire_opt = 0;
        int fnx = 0, fxx = 0;
        obj_hash *h;
        int rc;
        uint64_t before;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        if (hash_parse_expire(argv, argc, &pos, cmd_id == CMD_HSETEX,
                              now_ms, &expire_ms, &expire_opt, out) != 0)
            return;
        fields_idx = pos;
        for (size_t i = 2; i < fields_idx; i++) {
            const char *opt;
            size_t optl;
            if (!arg_str(&argv[i], &opt, &optl))
                goto bad_type;
            if (ci_equal(opt, optl, "FNX"))
                fnx = 1;
            else if (ci_equal(opt, optl, "FXX"))
                fxx = 1;
        }
        if (fnx && fxx) {
            static const char E[] =
                "ERR Only one of FNX or FXX arguments can be specified";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (hash_parse_field_list(argv, argc, fields_idx,
                                  cmd_id == CMD_HSETEX ? 2 : 1,
                                  &num_fields, &first_field,
                                  out) != 0)
            return;

        if (cmd_id == CMD_HSETEX) {
            for (size_t i = first_field; i + 1 < first_field + num_fields * 2;
                 i += 2) {
                const char *f, *v;
                size_t fl, vl;
                if (!arg_str(&argv[i], &f, &fl) ||
                    !arg_str(&argv[i + 1], &v, &vl))
                    goto bad_type;
                if (!storage_string_ok(fl, vl)) {
                    storage_length_error(out);
                    return;
                }
            }
            rc = get_hash(d, out, k, kl, 0, now_ms, &h);
            if (rc < 0)
                return;
            if (rc == 0) {
                if (fxx) {
                    resp_write_integer(out, 0);
                    return;
                }
                rc = get_hash(d, out, k, kl, 1, now_ms, &h);
                if (rc <= 0)
                    return;
            }
            if (fxx || fnx) {
                size_t found = 0;
                for (size_t i = first_field; i < first_field + num_fields * 2;
                     i += 2) {
                    const char *f, *v;
                    size_t fl, vl;
                    if (!arg_str(&argv[i], &f, &fl))
                        goto bad_type;
                    if (obj_hash_get_at(h, f, fl, now_ms, &v, &vl))
                        found++;
                }
                if ((fxx && found != num_fields) ||
                    (fnx && found != 0)) {
                    resp_write_integer(out, 0);
                    return;
                }
            }
            before = obj_hash_mem(h);
            for (size_t i = first_field; i + 1 < first_field + num_fields * 2;
                 i += 2) {
                const char *f, *v;
                size_t fl, vl;
                if (!arg_str(&argv[i], &f, &fl) ||
                    !arg_str(&argv[i + 1], &v, &vl))
                    goto bad_type;
                if (obj_hash_set_at(h, f, fl, v, vl, now_ms,
                                    expire_opt != 0) < 0) {
                    storage_length_error(out);
                    return;
                }
                if (expire_opt == 3)
                    (void)obj_hash_expire_set(h, f, fl, expire_ms, now_ms);
            }
            mem_sync(d, k, kl, before, obj_hash_mem(h));
            obj_hash_purge_expired(h, now_ms);
            if (obj_hash_len_at(h, now_ms) == 0)
                db_del_kv(d, k, kl);
            resp_write_integer(out, 1);
            return;
        }

        rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        resp_write_array_header(out, num_fields);
        before = rc == 1 ? obj_hash_mem(h) : 0;
        for (size_t i = first_field; i < first_field + num_fields; i++) {
            const char *f, *v;
            size_t fl, vl;
            if (!arg_str(&argv[i], &f, &fl))
                goto bad_type;
            if (rc == 1 && obj_hash_get_at(h, f, fl, now_ms, &v, &vl)) {
                resp_write_bulk(out, v, vl);
                if (expire_opt == 3)
                    (void)obj_hash_expire_set(h, f, fl, expire_ms, now_ms);
                else if (expire_opt == 2)
                    (void)obj_hash_expire_persist(h, f, fl, now_ms);
            } else {
                resp_write_bulk(out, NULL, 0);
            }
        }
        if (rc == 1) {
            mem_sync(d, k, kl, before, obj_hash_mem(h));
            obj_hash_purge_expired(h, now_ms);
            if (obj_hash_len_at(h, now_ms) == 0)
                db_del_kv(d, k, kl);
        }
        return;
    }


    if (cmd_id == CMD_HEXPIRE || cmd_id == CMD_HPEXPIRE ||
        cmd_id == CMD_HEXPIREAT || cmd_id == CMD_HPEXPIREAT) {
        const char *k, *tv;
        size_t kl, tvl;
        long long val;
        uint64_t expire_ms;
        int cond = 0;
        size_t pos = 3, fields_idx, num_fields, first_field;
        obj_hash *h;
        int rc;
        uint64_t before;
        if (!arg_str(&argv[1], &k, &kl) ||
            !arg_str(&argv[2], &tv, &tvl))
            goto bad_type;
        if (!parse_i64(tv, tvl, &val) || val < 0) {
            static const char E[] = "ERR invalid expire time";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (cmd_id == CMD_HEXPIRE)
            expire_ms = now_ms + (uint64_t)val * 1000ULL;
        else if (cmd_id == CMD_HPEXPIRE)
            expire_ms = now_ms + (uint64_t)val;
        else if (cmd_id == CMD_HEXPIREAT)
            expire_ms = (uint64_t)val * 1000ULL;
        else
            expire_ms = (uint64_t)val;

        fields_idx = 0;
        for (; pos < argc; pos++) {
            const char *opt;
            size_t optl;
            if (!arg_str(&argv[pos], &opt, &optl))
                goto bad_type;
            if (ci_equal(opt, optl, "FIELDS")) {
                fields_idx = pos;
                break;
            }
            if (ci_equal(opt, optl, "NX")) {
                if (cond) {
                    static const char E[] =
                        "ERR Only one of NX, XX, GT or LT can be specified";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                cond = 1;
            } else if (ci_equal(opt, optl, "XX")) {
                if (cond) {
                    static const char E[] =
                        "ERR Only one of NX, XX, GT or LT can be specified";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                cond = 2;
            } else if (ci_equal(opt, optl, "GT")) {
                if (cond) {
                    static const char E[] =
                        "ERR Only one of NX, XX, GT or LT can be specified";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                cond = 3;
            } else if (ci_equal(opt, optl, "LT")) {
                if (cond) {
                    static const char E[] =
                        "ERR Only one of NX, XX, GT or LT can be specified";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                cond = 4;
            } else {
                char msg[128];
                int n = snprintf(msg, sizeof(msg),
                                 "ERR unknown argument: %.*s",
                                 (int)optl, opt);
                resp_write_error(out, msg, (size_t)n);
                return;
            }
        }
        if (fields_idx == 0) {
            static const char E[] = "ERR missing FIELDS argument";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (hash_parse_field_list(argv, argc, fields_idx, 1, &num_fields,
                                  &first_field, out) != 0)
            return;
        rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        resp_write_array_header(out, num_fields);
        if (rc == 0) {
            for (size_t i = 0; i < num_fields; i++)
                resp_write_integer(out, -2);
            return;
        }
        before = obj_hash_mem(h);
        for (size_t i = first_field; i < first_field + num_fields; i++) {
            const char *f, *v;
            size_t fl, vl;
            uint64_t cur = 0;
            int has_ttl, satisfied = 1;
            if (!arg_str(&argv[i], &f, &fl))
                goto bad_type;
            if (!obj_hash_get_at(h, f, fl, now_ms, &v, &vl)) {
                resp_write_integer(out, -2);
                continue;
            }
            has_ttl = obj_hash_expire_get(h, f, fl, &cur) == 1;
            if (cond == 1 && has_ttl)
                satisfied = 0;
            else if (cond == 2 && !has_ttl)
                satisfied = 0;
            else if (cond == 3 && (!has_ttl || expire_ms <= cur))
                satisfied = 0;
            else if (cond == 4 && (!has_ttl || expire_ms >= cur))
                satisfied = 0;
            if (!satisfied) {
                resp_write_integer(out, 0);
                continue;
            }
            if (expire_ms <= now_ms) {
                (void)obj_hash_del_at(h, f, fl, now_ms);
                resp_write_integer(out, 2);
            } else if (obj_hash_expire_set(h, f, fl, expire_ms, now_ms) == 1) {
                resp_write_integer(out, 1);
            } else {
                resp_write_integer(out, -2);
            }
        }
        mem_sync(d, k, kl, before, obj_hash_mem(h));
        obj_hash_purge_expired(h, now_ms);
        if (obj_hash_len_at(h, now_ms) == 0)
            db_del_kv(d, k, kl);
        return;
    }

    if (cmd_id == CMD_HTTL || cmd_id == CMD_HPTTL ||
        cmd_id == CMD_HEXPIRETIME || cmd_id == CMD_HPEXPIRETIME ||
        cmd_id == CMD_HPERSIST) {
        const char *k;
        size_t kl;
        size_t num_fields = 0, first_field = 0;
        obj_hash *h;
        int rc;
        uint64_t before;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (hash_parse_field_list(argv, argc, 2, 1, &num_fields,
                                  &first_field, out) != 0)
            return;
        rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        resp_write_array_header(out, num_fields);
        if (rc == 0) {
            for (size_t i = 0; i < num_fields; i++)
                resp_write_integer(out, -2);
            return;
        }
        before = obj_hash_mem(h);
        for (size_t i = first_field; i < first_field + num_fields; i++) {
            const char *f, *v;
            size_t fl, vl;
            uint64_t ttl_ms = 0, expire = 0;
            if (!arg_str(&argv[i], &f, &fl))
                goto bad_type;
            if (!obj_hash_get_at(h, f, fl, now_ms, &v, &vl)) {
                resp_write_integer(out, -2);
                continue;
            }
            if (cmd_id == CMD_HPERSIST) {
                int pr = obj_hash_expire_persist(h, f, fl, now_ms);
                resp_write_integer(out, pr == 0 ? -1 : pr);
                continue;
            }
            if (obj_hash_ttl(h, f, fl, now_ms, &ttl_ms) != 1) {
                resp_write_integer(out, -1);
                continue;
            }
            if (cmd_id == CMD_HTTL)
                resp_write_integer(out, (long long)((ttl_ms + 999) / 1000));
            else if (cmd_id == CMD_HPTTL)
                resp_write_integer(out, (long long)ttl_ms);
            else if (cmd_id == CMD_HEXPIRETIME) {
                (void)obj_hash_expire_get(h, f, fl, &expire);
                resp_write_integer(out, (long long)(expire / 1000));
            } else {
                (void)obj_hash_expire_get(h, f, fl, &expire);
                resp_write_integer(out, (long long)expire);
            }
        }
        mem_sync(d, k, kl, before, obj_hash_mem(h));
        obj_hash_purge_expired(h, now_ms);
        if (obj_hash_len_at(h, now_ms) == 0)
            db_del_kv(d, k, kl);
        return;
    }

    if (cmd_id == CMD_HGETDEL) {
        const char *k;
        size_t kl;
        size_t num_fields = 0, first_field = 0;
        obj_hash *h;
        int rc;
        uint64_t before;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (hash_parse_field_list(argv, argc, 2, 1, &num_fields,
                                  &first_field, out) != 0)
            return;
        rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        before = rc == 1 ? obj_hash_mem(h) : 0;
        resp_write_array_header(out, num_fields);
        for (size_t i = first_field; i < first_field + num_fields; i++) {
            const char *f, *v;
            size_t fl, vl;
            if (!arg_str(&argv[i], &f, &fl))
                goto bad_type;
            if (rc == 1 && obj_hash_get_at(h, f, fl, now_ms, &v, &vl)) {
                resp_write_bulk(out, v, vl);
                (void)obj_hash_del_at(h, f, fl, now_ms);
            } else {
                resp_write_bulk(out, NULL, 0);
            }
        }
        if (rc == 1) {
            mem_sync(d, k, kl, before, obj_hash_mem(h));
            if (obj_hash_len_at(h, now_ms) == 0)
                db_del_kv(d, k, kl);
        }
        return;
    }

    if (cmd_id == CMD_HGET) {
        if (argc != 3) {
            wrong_args(out, "hget");
            return;
        }
        const char *k, *f;
        size_t kl, fl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &f, &fl))
            goto bad_type;
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        const char *v;
        size_t vl;
        if (rc == 1 && obj_hash_get_at(h, f, fl, now_ms, &v, &vl))
            resp_write_bulk(out, v, vl);
        else
            resp_write_bulk(out, NULL, 0);
        return;
    }

    if (cmd_id == CMD_HDEL) {
        if (argc < 3) {
            wrong_args(out, "hdel");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        long long deleted = 0;
        uint64_t before = obj_hash_mem(h);
        for (size_t i = 2; i < argc; i++) {
            const char *f;
            size_t fl;
            if (!arg_str(&argv[i], &f, &fl))
                goto bad_type;
            deleted += obj_hash_del(h, f, fl);
        }
        mem_sync(d, k, kl, before, obj_hash_mem(h));
        if (obj_hash_len(h) == 0)
            db_del_kv(d, k, kl); /* empty hash: the key goes away */
        resp_write_integer(out, deleted);
        return;
    }

    if (cmd_id == CMD_HEXISTS) {
        if (argc != 3) {
            wrong_args(out, "hexists");
            return;
        }
        const char *k, *f;
        size_t kl, fl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &f, &fl))
            goto bad_type;
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        const char *v;
        size_t vl;
        resp_write_integer(out,
                           rc == 1 && obj_hash_get_at(h, f, fl, now_ms,
                                                      &v, &vl) ? 1 : 0);
        return;
    }

    if (cmd_id == CMD_HLEN) {
        if (argc != 2) {
            wrong_args(out, "hlen");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        resp_write_integer(out, rc == 1 ? (long long)obj_hash_len_at(h, now_ms)
                                       : 0);
        return;
    }

    if (cmd_id == CMD_HGETALL || cmd_id == CMD_HKEYS ||
        cmd_id == CMD_HVALS) {
        if (argc != 2) {
            wrong_args(out, cmd_id == CMD_HGETALL ? "hgetall"
                           : cmd_id == CMD_HKEYS  ? "hkeys"
                                                            : "hvals");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        hdump_ctx ctx;
        ctx.out = out;
        ctx.keys = cmd_id != CMD_HVALS;
        ctx.vals = cmd_id != CMD_HKEYS;
        {
            uint64_t len = obj_hash_len_at(h, now_ms);
            resp_write_array_header(out, cmd_id == CMD_HGETALL ? len * 2
                                                                : len);
        }
        obj_hash_each_at(h, hdump_cb, &ctx, now_ms);
        return;
    }

    if (cmd_id == CMD_HMGET) {
        if (argc < 3) {
            wrong_args(out, "hmget");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        resp_write_array_header(out, argc - 2);
        for (size_t i = 2; i < argc; i++) {
            const char *f, *v;
            size_t fl, vl;
            if (!arg_str(&argv[i], &f, &fl))
                goto bad_type;
            if (rc == 1 && obj_hash_get_at(h, f, fl, now_ms, &v, &vl))
                resp_write_bulk(out, v, vl);
            else
                resp_write_bulk(out, NULL, 0);
        }
        return;
    }

    if (cmd_id == CMD_HINCRBY) {
        if (argc != 4) {
            wrong_args(out, "hincrby");
            return;
        }
        const char *k, *f, *iv;
        size_t kl, fl, ivl;
        long long inc, cur = 0;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &f, &fl) ||
            !arg_str(&argv[3], &iv, &ivl))
            goto bad_type;
        if (!storage_key_ok(kl) || !storage_string_ok(fl, 24)) {
            storage_length_error(out);
            return;
        }
        if (!parse_i64(iv, ivl, &inc)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (oom_blocked(d, out))
            return;
        obj_hash *h;
        if (get_hash(d, out, k, kl, 1, now_ms, &h) <= 0)
            return;
        {
            const char *v;
            size_t vl;
            if (obj_hash_get_at(h, f, fl, now_ms, &v, &vl) &&
                !parse_i64(v, vl, &cur)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        if ((inc > 0 && cur > LLONG_MAX - inc) ||
            (inc < 0 && cur < LLONG_MIN - inc)) {
            resp_write_error(out, ERR_OVERFLOW, sizeof(ERR_OVERFLOW) - 1);
            return;
        }
        cur += inc;
        {
            char num[24];
            int nl = snprintf(num, sizeof(num), "%lld", cur);
            uint64_t before = obj_hash_mem(h);
            if (obj_hash_set_at(h, f, fl, num, (size_t)nl, now_ms, 0) < 0) {
                storage_length_error(out);
                return;
            }
            mem_sync(d, k, kl, before, obj_hash_mem(h));
        }
        resp_write_integer(out, cur);
        return;
    }

    if (cmd_id == CMD_HINCRBYFLOAT) {
        if (argc != 4) {
            wrong_args(out, "hincrbyfloat");
            return;
        }
        const char *k, *f, *dv;
        size_t kl, fl, dvl;
        long double delta, cur = 0, res;
        char buf[5120];
        int nl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &f, &fl) ||
            !arg_str(&argv[3], &dv, &dvl))
            goto bad_type;
        if (!storage_key_ok(kl) || !storage_key_ok(fl)) {
            storage_length_error(out);
            return;
        }
        if (!parse_ld(dv, dvl, &delta)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        if (oom_blocked(d, out))
            return;
        obj_hash *h;
        if (get_hash(d, out, k, kl, 1, now_ms, &h) <= 0)
            return;
        {
            const char *v;
            size_t vl;
            if (obj_hash_get_at(h, f, fl, now_ms, &v, &vl) &&
                !parse_ld(v, vl, &cur)) {
                static const char E[] = "ERR hash value is not a float";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
        }
        res = cur + delta;
        if (res != res || isinf(res)) {
            static const char E[] =
                "ERR increment would produce NaN or Infinity";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        nl = snprintf(buf, sizeof(buf), "%.17Lg", res);
        {
            uint64_t before = obj_hash_mem(h);
            if (obj_hash_set_at(h, f, fl, buf, (size_t)nl, now_ms, 0) < 0) {
                storage_length_error(out);
                return;
            }
            mem_sync(d, k, kl, before, obj_hash_mem(h));
        }
        resp_write_bulk(out, buf, (size_t)nl);
        return;
    }

    if (cmd_id == CMD_HSETNX) {
        if (argc != 4) {
            wrong_args(out, "hsetnx");
            return;
        }
        const char *k, *f, *v;
        size_t kl, fl, vl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &f, &fl) ||
            !arg_str(&argv[3], &v, &vl))
            goto bad_type;
        if (!storage_key_ok(kl) || !storage_string_ok(fl, vl)) {
            storage_length_error(out);
            return;
        }
        if (oom_blocked(d, out))
            return;
        obj_hash *h;
        if (get_hash(d, out, k, kl, 1, now_ms, &h) <= 0)
            return;
        {
            const char *old;
            size_t oldl;
            if (obj_hash_get_at(h, f, fl, now_ms, &old, &oldl)) {
                resp_write_integer(out, 0);
                return;
            }
        }
        {
            uint64_t before = obj_hash_mem(h);
            if (obj_hash_set_at(h, f, fl, v, vl, now_ms, 0) < 0) {
                storage_length_error(out);
                return;
            }
            mem_sync(d, k, kl, before, obj_hash_mem(h));
        }
        resp_write_integer(out, 1);
        return;
    }

    if (cmd_id == CMD_HSTRLEN) {
        if (argc != 3) {
            wrong_args(out, "hstrlen");
            return;
        }
        const char *k, *f;
        size_t kl, fl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &f, &fl))
            goto bad_type;
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        const char *v;
        size_t vl;
        resp_write_integer(out, rc == 1 && obj_hash_get(h, f, fl, &v, &vl)
                                      ? (long long)vl
                                      : 0);
        return;
    }

    if (cmd_id == CMD_HRANDFIELD) {
        if (argc < 2 || argc > 4) {
            wrong_args(out, "hrandfield");
            return;
        }
        const char *k;
        size_t kl;
        long long count = 0;
        int withvalues = 0;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (argc >= 3) {
            const char *cv;
            size_t cvl;
            if (!arg_str(&argv[2], &cv, &cvl))
                goto bad_type;
            if (!parse_i64(cv, cvl, &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        if (argc == 4) {
            const char *ov;
            size_t ovl;
            if (!arg_str(&argv[3], &ov, &ovl))
                goto bad_type;
            if (!ci_equal(ov, ovl, "WITHVALUES")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            withvalues = 1;
        }
        obj_hash *h;
        int rc = get_hash(d, out, k, kl, 0, now_ms, &h);
        if (rc < 0)
            return;
        if (argc == 2) {
            /* single field form: bulk reply */
            if (rc == 0) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            if (obj_hash_is_listpack(h)) {
                /* small hash: pick a random pair index directly */
                const char *f;
                size_t fl;
                uint64_t idx = db_rand(d) % (uint32_t)obj_hash_len(h);
                if (obj_hash_pair_at(h, idx, &f, &fl, NULL, NULL))
                    resp_write_bulk(out, f, fl);
                return;
            }
            {
                collect_ctx cc = {0};
                size_t idx;
                rh_each(&h->fields, collect_cb, &cc);
                idx = (size_t)(db_rand(d) % (uint32_t)cc.n);
                resp_write_bulk(out, cc.keys[idx], cc.lens[idx]);
                free(cc.keys);
                free(cc.lens);
                return;
            }
        }
        /* count form: array reply */
        if (rc == 0 || count == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        if (obj_hash_is_listpack(h)) {
            /* small hash: random pair indices, no collection pass */
            size_t n = (size_t)obj_hash_len(h);
            size_t i;
            if (count < 0) {
                /* with repeats */
                size_t total = (size_t)-count;
                resp_write_array_header(out, withvalues ? total * 2 : total);
                for (i = 0; i < total; i++) {
                    const char *f, *v;
                    size_t fl, vl;
                    uint64_t idx = db_rand(d) % (uint32_t)n;
                    if (!obj_hash_pair_at(h, idx, &f, &fl, &v, &vl))
                        continue;
                    resp_write_bulk(out, f, fl);
                    if (withvalues)
                        resp_write_bulk(out, v, vl);
                }
            } else {
                /* distinct: partial Fisher-Yates over pair indices */
                size_t k2 = (size_t)count < n ? (size_t)count : n;
                uint32_t *idxs =
                    (uint32_t *)malloc(n * sizeof(*idxs));
                if (idxs == NULL) {
                    fprintf(stderr, "ddup: out of memory\n");
                    exit(1);
                }
                for (i = 0; i < n; i++)
                    idxs[i] = (uint32_t)i;
                for (i = 0; i < k2; i++) {
                    size_t j = i + (size_t)(db_rand(d) % (uint32_t)(n - i));
                    uint32_t tmp = idxs[i];
                    idxs[i] = idxs[j];
                    idxs[j] = tmp;
                }
                resp_write_array_header(out, withvalues ? k2 * 2 : k2);
                for (i = 0; i < k2; i++) {
                    const char *f, *v;
                    size_t fl, vl;
                    if (!obj_hash_pair_at(h, idxs[i], &f, &fl, &v, &vl))
                        continue;
                    resp_write_bulk(out, f, fl);
                    if (withvalues)
                        resp_write_bulk(out, v, vl);
                }
                free(idxs);
            }
            return;
        }
        {
            collect_ctx cc = {0};
            size_t n, i, k2;
            rh_each(&h->fields, collect_cb, &cc);
            n = cc.n;
            if (count < 0) {
                /* with repeats */
                size_t total = (size_t)-count;
                resp_write_array_header(out, withvalues ? total * 2 : total);
                for (i = 0; i < total; i++) {
                    size_t idx = (size_t)(db_rand(d) % (uint32_t)n);
                    resp_write_bulk(out, cc.keys[idx], cc.lens[idx]);
                    if (withvalues) {
                        const char *v;
                        size_t vl;
                        if (obj_hash_get(h, cc.keys[idx], cc.lens[idx], &v,
                                         &vl))
                            resp_write_bulk(out, v, vl);
                    }
                }
            } else {
                k2 = (size_t)count < n ? (size_t)count : n;
                collect_shuffle(d, &cc, k2);
                resp_write_array_header(out, withvalues ? k2 * 2 : k2);
                for (i = 0; i < k2; i++) {
                    resp_write_bulk(out, cc.keys[i], cc.lens[i]);
                    if (withvalues) {
                        const char *v;
                        size_t vl;
                        if (obj_hash_get(h, cc.keys[i], cc.lens[i], &v, &vl))
                            resp_write_bulk(out, v, vl);
                    }
                }
            }
            free(cc.keys);
            free(cc.lens);
        }
        return;
    }

    /* ---------------- list commands ---------------- */

    if (cmd_id == CMD_LPUSH || cmd_id == CMD_RPUSH ||
        cmd_id == CMD_LPUSHX || cmd_id == CMD_RPUSHX) {
        int left = cmd_id == CMD_LPUSH || cmd_id == CMD_LPUSHX;
        int only_if_exists =
            cmd_id == CMD_LPUSHX || cmd_id == CMD_RPUSHX;
        if (argc < 3) {
            wrong_args(out, left ? (only_if_exists ? "lpushx" : "lpush")
                                 : (only_if_exists ? "rpushx" : "rpush"));
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        {
            size_t count = argc - 2;
            const char *vals[count];
            size_t lens[count];
            size_t i;
            for (i = 0; i < count; i++) {
                if (!arg_str(&argv[i + 2], &vals[i], &lens[i]))
                    goto bad_type;
                if (lens[i] > UINT32_MAX) {
                    storage_length_error(out);
                    return;
                }
            }
            if (oom_blocked(d, out))
                return;
            {
                obj_list *l;
                int rc = get_list(d, out, k, kl, !only_if_exists, now_ms, &l);
                if (rc < 0)
                    return;
                if (rc == 0) {
                    resp_write_integer(out, 0);
                    return;
                }
                {
                    uint64_t before = obj_list_mem(l);
                    if (obj_list_push_many(l, left, vals, lens, count) != 0) {
                        storage_length_error(out);
                        return;
                    }
                    mem_sync(d, k, kl, before, obj_list_mem(l));
                }
                resp_write_integer(out, (long long)obj_list_len(l));
                return;
            }
        }
    }

    if (cmd_id == CMD_LPOP || cmd_id == CMD_RPOP) {
        int left = cmd_id == CMD_LPOP;
        long long count = 0;
        if (argc != 2 && argc != 3) {
            wrong_args(out, left ? "lpop" : "rpop");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (argc == 3) {
            const char *cv;
            size_t cvl;
            if (!arg_str(&argv[2], &cv, &cvl))
                goto bad_type;
            if (!parse_i64(cv, cvl, &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (count <= 0) {
                static const char E[] =
                    "ERR value is out of range, must be positive";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
        }
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (argc == 3) {
            /* count form: null when the key is missing (Redis), else an
             * array of min(count, llen) elements */
            size_t n, i;
            uint64_t before;
            if (rc == 0) {
                write_null_array(out);
                return;
            }
            n = (unsigned long long)count < obj_list_len(l)
                    ? (size_t)count
                    : (size_t)obj_list_len(l);
            before = obj_list_mem(l);
            resp_write_array_header(out, n);
            for (i = 0; i < n; i++) {
                char *data = NULL;
                size_t dlen = 0;
                if (!obj_list_pop(l, left, &data, &dlen))
                    break;
                resp_write_bulk(out, data, dlen);
                free(data);
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
            if (obj_list_len(l) == 0)
                db_del_kv(d, k, kl); /* empty list: the key goes away */
            return;
        }
        if (rc == 0) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        {
            char *data = NULL;
            size_t dlen = 0;
            uint64_t before = obj_list_mem(l);
            if (!obj_list_pop(l, left, &data, &dlen)) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
            if (obj_list_len(l) == 0)
                db_del_kv(d, k, kl); /* empty list: the key goes away */
            resp_write_bulk(out, data, dlen);
            free(data);
        }
        return;
    }

    if (cmd_id == CMD_LLEN) {
        if (argc != 2) {
            wrong_args(out, "llen");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        resp_write_integer(out, rc == 1 ? (long long)obj_list_len(l) : 0);
        return;
    }

    if (cmd_id == CMD_LRANGE) {
        if (argc != 4) {
            wrong_args(out, "lrange");
            return;
        }
        const char *k, *sv, *ev;
        size_t kl, svl, evl;
        long long start, stop;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &sv, &svl) ||
            !arg_str(&argv[3], &ev, &evl))
            goto bad_type;
        if (!parse_i64(sv, svl, &start) || !parse_i64(ev, evl, &stop)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        {
            long long len = (long long)obj_list_len(l);
            long long i;
            obj_list_iter it;
            if (start < 0)
                start += len;
            if (start < 0)
                start = 0;
            if (stop < 0)
                stop += len;
            if (stop >= len)
                stop = len - 1;
            if (start > stop || start >= len || stop < 0) {
                resp_write_array_header(out, 0);
                return;
            }
            resp_write_array_header(out, (size_t)(stop - start + 1));
            if (obj_list_seek(l, (size_t)start, &it)) {
                for (i = start; i <= stop; i++) {
                    size_t vl = 0;
                    const char *v = obj_list_iter_value(&it, &vl);
                    resp_write_bulk(out, v, vl);
                    if (i < stop)
                        obj_list_iter_next(&it);
                }
            }
        }
        return;
    }

    if (cmd_id == CMD_LINDEX) {
        if (argc != 3) {
            wrong_args(out, "lindex");
            return;
        }
        const char *k, *iv;
        size_t kl, ivl;
        long long idx;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &iv, &ivl))
            goto bad_type;
        if (!parse_i64(iv, ivl, &idx)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        if (idx < 0)
            idx += (long long)obj_list_len(l);
        {
            obj_list_iter it;
            if (idx >= 0 && obj_list_seek(l, (size_t)idx, &it)) {
                size_t vl = 0;
                const char *v = obj_list_iter_value(&it, &vl);
                resp_write_bulk(out, v, vl);
            } else {
                resp_write_bulk(out, NULL, 0);
            }
        }
        return;
    }

    if (cmd_id == CMD_LSET) {
        if (argc != 4) {
            wrong_args(out, "lset");
            return;
        }
        const char *k, *iv, *v;
        size_t kl, ivl, vl;
        long long idx;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &iv, &ivl) ||
            !arg_str(&argv[3], &v, &vl))
            goto bad_type;
        if (!storage_key_ok(kl) || vl > UINT32_MAX) {
            storage_length_error(out);
            return;
        }
        if (!parse_i64(iv, ivl, &idx)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (oom_blocked(d, out))
            return;
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 0) {
            static const char E_NOKEY[] = "ERR no such key";
            resp_write_error(out, E_NOKEY, sizeof(E_NOKEY) - 1);
            return;
        }
        if (idx < 0)
            idx += (long long)obj_list_len(l);
        if (idx < 0) {
            static const char E_RANGE[] = "ERR index out of range";
            resp_write_error(out, E_RANGE, sizeof(E_RANGE) - 1);
            return;
        }
        {
            uint64_t before = obj_list_mem(l);
            int set_rc = obj_list_set_at(l, (size_t)idx, v, vl);
            if (set_rc < 0) {
                storage_length_error(out);
                return;
            }
            if (set_rc == 0) {
                static const char E_RANGE[] = "ERR index out of range";
                resp_write_error(out, E_RANGE, sizeof(E_RANGE) - 1);
                return;
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_LPOS) {
        if (argc < 3) {
            wrong_args(out, "lpos");
            return;
        }
        const char *k, *ele;
        size_t kl, elel;
        long long rank = 1, count = 0, maxlen = 0;
        int count_given = 0;
        size_t i;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &ele, &elel))
            goto bad_type;
        if (((argc - 3) % 2) != 0) {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        for (i = 3; i + 1 < argc; i += 2) {
            const char *opt, *val;
            size_t optl, vall;
            if (!arg_str(&argv[i], &opt, &optl) ||
                !arg_str(&argv[i + 1], &val, &vall))
                goto bad_type;
            if (ci_equal(opt, optl, "RANK")) {
                if (!parse_i64(val, vall, &rank)) {
                    resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (rank == 0) {
                    static const char E[] = "ERR RANK can't be zero";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
            } else if (ci_equal(opt, optl, "COUNT")) {
                if (!parse_i64(val, vall, &count)) {
                    resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (count < 0) {
                    static const char E[] = "ERR COUNT can't be negative";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                count_given = 1;
            } else if (ci_equal(opt, optl, "MAXLEN")) {
                if (!parse_i64(val, vall, &maxlen)) {
                    resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (maxlen < 0) {
                    static const char E[] = "ERR MAXLEN can't be negative";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 0) {
            if (count_given)
                resp_write_array_header(out, 0);
            else
                resp_write_bulk(out, NULL, 0);
            return;
        }
        /* Scan from the head (rank > 0) or the tail (rank < 0); maxlen caps
         * the number of comparisons (0 = unlimited). Two passes over the
         * match set: count first (array header), then emit -- no per-element
         * allocation. */
        {
            long long want = rank > 0 ? rank : -rank;
            long long from_head = rank > 0;
            long long matches = 0, seen = 0, compared = 0;
            long long idx = from_head ? 0 : (long long)obj_list_len(l) - 1;
            long long first_idx = 0;
            obj_list_iter it;
            int valid = from_head ? obj_list_first(l, &it)
                                  : obj_list_last(l, &it);
            for (; valid;
                 valid = from_head ? obj_list_iter_next(&it)
                                   : obj_list_iter_prev(&it)) {
                size_t vl = 0;
                const char *v;
                if (maxlen > 0 && compared >= maxlen)
                    break;
                compared++;
                v = obj_list_iter_value(&it, &vl);
                if (vl == elel && memcmp(v, ele, elel) == 0) {
                    seen++;
                    if (seen >= want) {
                        if (matches == 0)
                            first_idx = idx;
                        matches++;
                        if (count_given && count > 0 && matches >= count)
                            break;
                    }
                }
                idx += from_head ? 1 : -1;
            }
            if (!count_given) {
                if (matches == 0)
                    resp_write_bulk(out, NULL, 0);
                else
                    resp_write_integer(out, first_idx);
                return;
            }
            resp_write_array_header(out, (size_t)matches);
            {
                long long emitted = 0;
                long long idx = from_head ? 0 : (long long)obj_list_len(l) - 1;
                seen = 0;
                compared = 0;
                valid = from_head ? obj_list_first(l, &it)
                                  : obj_list_last(l, &it);
                for (; valid && emitted < matches;
                     valid = from_head ? obj_list_iter_next(&it)
                                       : obj_list_iter_prev(&it)) {
                    size_t vl = 0;
                    const char *v;
                    if (maxlen > 0 && compared >= maxlen)
                        break;
                    compared++;
                    v = obj_list_iter_value(&it, &vl);
                    if (vl == elel && memcmp(v, ele, elel) == 0) {
                        seen++;
                        if (seen >= want) {
                            resp_write_integer(out, idx);
                            emitted++;
                        }
                    }
                    idx += from_head ? 1 : -1;
                }
            }
        }
        return;
    }

    if (cmd_id == CMD_LREM) {
        if (argc != 4) {
            wrong_args(out, "lrem");
            return;
        }
        const char *k, *cv, *ele;
        size_t kl, cvl, elel;
        long long count;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &cv, &cvl) ||
            !arg_str(&argv[3], &ele, &elel))
            goto bad_type;
        if (!parse_i64(cv, cvl, &count)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        {
            /* count > 0 from the head, < 0 from the tail, 0 = all */
            unsigned long long limit =
                count < 0 ? (unsigned long long)(-(count + 1)) + 1ULL
                          : (unsigned long long)count;
            long long removed = 0;
            uint64_t before = obj_list_mem(l);
            obj_list_iter it;
            int from_head = count >= 0;
            int valid = from_head ? obj_list_first(l, &it)
                                  : obj_list_last(l, &it);
            while (valid &&
                   (limit == 0 || (unsigned long long)removed < limit)) {
                size_t vl = 0;
                const char *v = obj_list_iter_value(&it, &vl);
                if (vl == elel && memcmp(v, ele, elel) == 0) {
                    removed++;
                    valid = obj_list_remove_at(&it); /* lands on successor */
                    if (!from_head) {
                        /* backward scan: continue at the element before the
                         * removed one (predecessor of the successor) */
                        valid = valid ? obj_list_iter_prev(&it)
                                      : obj_list_last(l, &it);
                    }
                } else {
                    valid = from_head ? obj_list_iter_next(&it)
                                      : obj_list_iter_prev(&it);
                }
            }
            if (removed > 0) {
                mem_sync(d, k, kl, before, obj_list_mem(l));
                if (obj_list_len(l) == 0)
                    db_del_kv(d, k, kl); /* empty list: the key goes away */
            }
            resp_write_integer(out, removed);
        }
        return;
    }

    if (cmd_id == CMD_LTRIM) {
        if (argc != 4) {
            wrong_args(out, "ltrim");
            return;
        }
        const char *k, *sv, *ev;
        size_t kl, svl, evl;
        long long start, stop;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &sv, &svl) ||
            !arg_str(&argv[3], &ev, &evl))
            goto bad_type;
        if (!parse_i64(sv, svl, &start) || !parse_i64(ev, evl, &stop)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        obj_list *l;
        int rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 1) {
            /* same index math as LRANGE */
            long long len = (long long)obj_list_len(l);
            if (start < 0)
                start += len;
            if (start < 0)
                start = 0;
            if (stop < 0)
                stop += len;
            if (stop >= len)
                stop = len - 1;
            if (start > stop || start >= len || stop < 0) {
                db_del_kv(d, k, kl); /* nothing in range: drop the key */
            } else if (start > 0 || stop < len - 1) {
                long long idx = 0;
                obj_list_iter it;
                uint64_t before = obj_list_mem(l);
                int valid = obj_list_first(l, &it);
                while (valid) {
                    if (idx < start || idx > stop)
                        valid = obj_list_remove_at(&it); /* next successor */
                    else
                        valid = obj_list_iter_next(&it);
                    idx++;
                }
                mem_sync(d, k, kl, before, obj_list_mem(l));
            }
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_RPOPLPUSH) {
        if (argc != 3) {
            wrong_args(out, "rpoplpush");
            return;
        }
        const char *sk, *dk;
        size_t skl, dkl;
        if (!arg_str(&argv[1], &sk, &skl) || !arg_str(&argv[2], &dk, &dkl))
            goto bad_type;
        if (!storage_key_ok(skl) || !storage_key_ok(dkl)) {
            storage_length_error(out);
            return;
        }
        obj_list *src, *dst;
        int rcs = get_list(d, out, sk, skl, 0, now_ms, &src);
        if (rcs < 0)
            return;
        if (rcs == 0) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        {
            int same = skl == dkl && memcmp(sk, dk, skl) == 0;
            int created_dst = 0;
            /* type-check dst before mutating src */
            if (!same) {
                int rcd = get_list(d, out, dk, dkl, 0, now_ms, &dst);
                if (rcd < 0)
                    return;
                if (rcd == 0) {
                    if (oom_blocked(d, out))
                        return;
                    rcd = get_list(d, out, dk, dkl, 1, now_ms, &dst);
                    if (rcd < 0)
                        return;
                    created_dst = 1;
                }
            } else {
                dst = src;
            }
            /* push a copy of the tail element to dst's head first, then
             * remove the source tail (SMOVE ordering: a failed push leaves
             * src untouched) */
            {
                obj_list_iter sit;
                size_t tvl = 0;
                const char *tv;
                uint64_t dbefore = obj_list_mem(dst);
                obj_list_last(src, &sit); /* src is non-empty (rcs == 1) */
                tv = obj_list_iter_value(&sit, &tvl);
                if (obj_list_push(dst, 1, tv, tvl) != 0) {
                    if (created_dst)
                        db_del_kv(d, dk, dkl);
                    storage_length_error(out);
                    return;
                }
                mem_sync(d, dk, dkl, dbefore, obj_list_mem(dst));
            }
            {
                obj_list_iter rit;
                uint64_t sbefore = obj_list_mem(src);
                obj_list_last(src, &rit);
                obj_list_remove_at(&rit);
                mem_sync(d, sk, skl, sbefore, obj_list_mem(src));
            }
            if (obj_list_len(src) == 0)
                db_del_kv(d, sk, skl); /* empty list: the key goes away */
            {
                obj_list_iter hit;
                size_t hvl = 0;
                const char *hv;
                obj_list_first(dst, &hit); /* the just-pushed element */
                hv = obj_list_iter_value(&hit, &hvl);
                resp_write_bulk(out, hv, hvl);
            }
        }
        return;
    }

    if (cmd_id == CMD_LINSERT) {
        const char *k, *where, *piv, *val;
        size_t kl, wl, pl, vl;
        int after;
        obj_list *l;
        int rc;
        obj_list_iter it;
        int found = 0;
        int valid;
        if (argc != 5) {
            wrong_args(out, "linsert");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl) ||
            !arg_str(&argv[2], &where, &wl) ||
            !arg_str(&argv[3], &piv, &pl) ||
            !arg_str(&argv[4], &val, &vl))
            goto bad_type;
        if (ci_equal(where, wl, "BEFORE")) {
            after = 0;
        } else if (ci_equal(where, wl, "AFTER")) {
            after = 1;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        if (!storage_key_ok(kl) || vl > UINT32_MAX) {
            storage_length_error(out);
            return;
        }
        rc = get_list(d, out, k, kl, 0, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        valid = obj_list_first(l, &it);
        while (valid) {
            size_t el = 0;
            const char *ev = obj_list_iter_value(&it, &el);
            if (el == pl && memcmp(ev, piv, pl) == 0) {
                found = 1;
                break;
            }
            valid = obj_list_iter_next(&it);
        }
        if (!found) {
            resp_write_integer(out, -1);
            return;
        }
        if (oom_blocked(d, out))
            return;
        {
            uint64_t before = obj_list_mem(l);
            if (obj_list_insert(&it, after, val, vl) != 1) {
                storage_length_error(out);
                return;
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
        }
        resp_write_integer(out, (long long)obj_list_len(l));
        return;
    }

    if (cmd_id == CMD_LMOVEM) {
        lmovem_execute(d, out, argv, argc, now_ms);
        return;
    }

    if (cmd_id == CMD_LMOVE) {
        const char *sk, *dk, *sw, *dw;
        size_t skl, dkl, swl, dwl;
        int src_left, dst_left;
        obj_list *src, *dst;
        int rcs;
        if (argc != 5) {
            wrong_args(out, "lmove");
            return;
        }
        if (!arg_str(&argv[1], &sk, &skl) ||
            !arg_str(&argv[2], &dk, &dkl) ||
            !arg_str(&argv[3], &sw, &swl) ||
            !arg_str(&argv[4], &dw, &dwl))
            goto bad_type;
        if (ci_equal(sw, swl, "LEFT")) {
            src_left = 1;
        } else if (ci_equal(sw, swl, "RIGHT")) {
            src_left = 0;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        if (ci_equal(dw, dwl, "LEFT")) {
            dst_left = 1;
        } else if (ci_equal(dw, dwl, "RIGHT")) {
            dst_left = 0;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        if (!storage_key_ok(skl) || !storage_key_ok(dkl)) {
            storage_length_error(out);
            return;
        }
        rcs = get_list(d, out, sk, skl, 0, now_ms, &src);
        if (rcs < 0)
            return;
        if (rcs == 0) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        {
            int same = skl == dkl && memcmp(sk, dk, skl) == 0;
            int created_dst = 0;
            if (!same) {
                int rcd = get_list(d, out, dk, dkl, 0, now_ms, &dst);
                if (rcd < 0)
                    return;
                if (rcd == 0) {
                    if (oom_blocked(d, out))
                        return;
                    rcd = get_list(d, out, dk, dkl, 1, now_ms, &dst);
                    if (rcd < 0)
                        return;
                    created_dst = 1;
                }
            } else {
                dst = src;
            }
            {
                char *data = NULL;
                size_t dlen = 0;
                uint64_t sbefore = obj_list_mem(src);
                if (!obj_list_pop(src, src_left, &data, &dlen)) {
                    resp_write_bulk(out, NULL, 0);
                    return;
                }
                {
                    uint64_t dbefore = same ? 0 : obj_list_mem(dst);
                    if (obj_list_push(dst, dst_left, data, dlen) != 0) {
                        if (created_dst)
                            db_del_kv(d, dk, dkl);
                        (void)obj_list_push(src, src_left, data, dlen);
                        free(data);
                        storage_length_error(out);
                        return;
                    }
                    if (same) {
                        mem_sync(d, sk, skl, sbefore, obj_list_mem(src));
                    } else {
                        mem_sync(d, dk, dkl, dbefore, obj_list_mem(dst));
                        mem_sync(d, sk, skl, sbefore, obj_list_mem(src));
                    }
                }
                if (obj_list_len(src) == 0)
                    db_del_kv(d, sk, skl); /* empty list: the key goes away */
                resp_write_bulk(out, data, dlen);
                free(data);
            }
        }
        return;
    }

    if (cmd_id == CMD_LMPOP) {
        long long nk;
        long long count = 1;
        size_t dir_idx;
        const char *where;
        size_t wl;
        int left;
        size_t i;
        if (argc < 4) {
            wrong_args(out, "lmpop");
            return;
        }
        if (!cmd_parse_ll(&argv[1], &nk)) {
            resp_write_error(out,
                             "ERR value is not an integer or out of range",
                             43);
            return;
        }
        if (nk <= 0) {
            static const char E[] =
                "ERR numkeys should be greater than 0";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if ((unsigned long long)nk > (unsigned long long)(argc - 3)) {
            wrong_args(out, "lmpop");
            return;
        }
        dir_idx = 2 + (size_t)nk;
        if (!arg_str(&argv[dir_idx], &where, &wl))
            goto bad_type;
        if (ci_equal(where, wl, "LEFT")) {
            left = 1;
        } else if (ci_equal(where, wl, "RIGHT")) {
            left = 0;
        } else {
            resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
            return;
        }
        if (argc > dir_idx + 1) {
            const char *opt;
            size_t ol;
            if (argc != dir_idx + 3) {
                wrong_args(out, "lmpop");
                return;
            }
            if (!arg_str(&argv[dir_idx + 1], &opt, &ol))
                goto bad_type;
            if (!ci_equal(opt, ol, "COUNT")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            if (!cmd_parse_ll(&argv[dir_idx + 2], &count)) {
                resp_write_error(out,
                                 "ERR value is not an integer or out of "
                                 "range",
                                 43);
                return;
            }
            if (count <= 0) {
                static const char E[] =
                    "ERR value is out of range, must be positive";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
        }
        for (i = 2; i < 2 + (size_t)nk; i++) {
            const char *k;
            size_t kl;
            obj_list *l;
            int rc;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
            rc = get_list(d, out, k, kl, 0, now_ms, &l);
            if (rc < 0)
                return;
            if (rc == 0)
                continue;
            {
                size_t n = (unsigned long long)count < obj_list_len(l)
                               ? (size_t)count
                               : (size_t)obj_list_len(l);
                uint64_t before = obj_list_mem(l);
                size_t j;
                resp_write_array_header(out, 2);
                resp_write_bulk(out, k, kl);
                resp_write_array_header(out, n);
                for (j = 0; j < n; j++) {
                    char *data = NULL;
                    size_t dlen = 0;
                    if (!obj_list_pop(l, left, &data, &dlen))
                        break;
                    resp_write_bulk(out, data, dlen);
                    free(data);
                }
                mem_sync(d, k, kl, before, obj_list_mem(l));
                if (obj_list_len(l) == 0)
                    db_del_kv(d, k, kl); /* empty list: the key goes away */
            }
            return;
        }
        write_null_array(out);
        return;
    }

    /* ---------------- set commands ---------------- */

    if (cmd_id == CMD_SADD) {
        if (argc < 3) {
            wrong_args(out, "sadd");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        if (oom_blocked(d, out))
            return;
        for (size_t i = 2; i < argc; i++) {
            const char *m;
            size_t ml;
            if (!arg_str(&argv[i], &m, &ml))
                goto bad_type;
            if (!storage_key_ok(ml)) {
                storage_length_error(out);
                return;
            }
        }
        obj_set *s;
        if (get_set(d, out, k, kl, 1, now_ms, &s) <= 0)
            return;
        long long added = 0;
        uint64_t before = obj_set_mem(s);
        for (size_t i = 2; i < argc; i++) {
            const char *m;
            size_t ml;
            if (!arg_str(&argv[i], &m, &ml))
                goto bad_type;
            {
                int add_rc = obj_set_add(s, m, ml);
                if (add_rc < 0) {
                    storage_length_error(out);
                    return;
                }
                added += add_rc;
            }
        }
        mem_sync(d, k, kl, before, obj_set_mem(s));
        resp_write_integer(out, added);
        return;
    }

    if (cmd_id == CMD_SREM) {
        if (argc < 3) {
            wrong_args(out, "srem");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_set *s;
        int rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        long long removed = 0;
        uint64_t before = obj_set_mem(s);
        for (size_t i = 2; i < argc; i++) {
            const char *m;
            size_t ml;
            if (!arg_str(&argv[i], &m, &ml))
                goto bad_type;
            removed += obj_set_rem(s, m, ml);
        }
        mem_sync(d, k, kl, before, obj_set_mem(s));
        if (obj_set_len(s) == 0)
            db_del_kv(d, k, kl); /* empty set: the key goes away */
        resp_write_integer(out, removed);
        return;
    }

    if (cmd_id == CMD_SISMEMBER) {
        if (argc != 3) {
            wrong_args(out, "sismember");
            return;
        }
        const char *k, *m;
        size_t kl, ml;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &m, &ml))
            goto bad_type;
        obj_set *s;
        int rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0)
            return;
        resp_write_integer(out, rc == 1 && obj_set_has(s, m, ml) ? 1 : 0);
        return;
    }

    if (cmd_id == CMD_SMISMEMBER) {
        if (argc < 3) {
            wrong_args(out, "smismember");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_set *s;
        int rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0)
            return;
        resp_write_array_header(out, argc - 2);
        for (size_t i = 2; i < argc; i++) {
            const char *m;
            size_t ml;
            if (!arg_str(&argv[i], &m, &ml))
                goto bad_type;
            resp_write_integer(out, rc == 1 && obj_set_has(s, m, ml) ? 1 : 0);
        }
        return;
    }

    if (cmd_id == CMD_SCARD) {
        if (argc != 2) {
            wrong_args(out, "scard");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_set *s;
        int rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0)
            return;
        resp_write_integer(out,
                           rc == 1 ? (long long)obj_set_len(s) : 0);
        return;
    }

    if (cmd_id == CMD_SMEMBERS) {
        if (argc != 2) {
            wrong_args(out, "smembers");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_set *s;
        int rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        {
            hdump_ctx ctx;
            ctx.out = out;
            ctx.keys = 1;
            ctx.vals = 0;
            resp_write_array_header(out, (size_t)obj_set_len(s));
            obj_set_each(s, hdump_cb, &ctx);
        }
        return;
    }

    if (cmd_id == CMD_SPOP || cmd_id == CMD_SRANDMEMBER) {
        int pop = cmd_id == CMD_SPOP;
        if (argc != 2 && argc != 3) {
            wrong_args(out, pop ? "spop" : "srandmember");
            return;
        }
        const char *k;
        size_t kl;
        long long count = 0;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (argc == 3) {
            const char *cv;
            size_t cvl;
            if (!arg_str(&argv[2], &cv, &cvl))
                goto bad_type;
            if (!parse_i64(cv, cvl, &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        if (pop && oom_blocked(d, out))
            return;
        obj_set *s;
        int rc = get_set(d, out, k, kl, 0, now_ms, &s);
        if (rc < 0)
            return;
        if (argc == 2) {
            /* single member form: bulk reply */
            if (rc == 0) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            if (obj_set_is_listpack(s)) {
                /* small set: pick a random member index directly */
                const char *mv;
                size_t ml = 0;
                uint64_t idx = db_rand(d) % (uint32_t)obj_set_len(s);
                if (obj_set_member_at(s, idx, &mv, &ml)) {
                    if (pop) {
                        /* copy before rem: the listpack reallocs */
                        char copy[OBJ_SET_MAX_LISTPACK_VALUE];
                        uint64_t before;
                        memcpy(copy, mv, ml);
                        before = obj_set_mem(s);
                        obj_set_rem(s, copy, ml);
                        mem_sync(d, k, kl, before, obj_set_mem(s));
                        if (obj_set_len(s) == 0)
                            db_del_kv(d, k, kl);
                        resp_write_bulk(out, copy, ml);
                    } else {
                        resp_write_bulk(out, mv, ml);
                    }
                }
                return;
            }
            {
                collect_ctx cc = {0};
                size_t idx;
                rh_each(&s->members, collect_cb, &cc);
                idx = (size_t)(db_rand(d) % (uint32_t)cc.n);
                if (pop) {
                    char *copy = (char *)malloc(cc.lens[idx] + 1);
                    uint64_t before;
                    memcpy(copy, cc.keys[idx], cc.lens[idx]);
                    before = obj_set_mem(s);
                    obj_set_rem(s, copy, cc.lens[idx]);
                    mem_sync(d, k, kl, before, obj_set_mem(s));
                    if (obj_set_len(s) == 0)
                        db_del_kv(d, k, kl);
                    resp_write_bulk(out, copy, cc.lens[idx]);
                    free(copy);
                } else {
                    resp_write_bulk(out, cc.keys[idx], cc.lens[idx]);
                }
                free(cc.keys);
                free(cc.lens);
                return;
            }
        }
        /* count form: array reply */
        if (rc == 0 || (count == 0 && pop) || (count == 0 && !pop)) {
            resp_write_array_header(out, 0);
            return;
        }
        if (obj_set_is_listpack(s)) {
            /* small set: random member indices, no collection pass */
            size_t n = (size_t)obj_set_len(s);
            size_t i;
            uint64_t before = obj_set_mem(s);
            if (count < 0) {
                /* with repeats */
                resp_write_array_header(out, (size_t)-count);
                for (i = 0; i < (size_t)-count; i++) {
                    const char *mv;
                    size_t ml = 0;
                    uint64_t idx = db_rand(d) % (uint32_t)n;
                    if (!obj_set_member_at(s, idx, &mv, &ml))
                        continue;
                    resp_write_bulk(out, mv, ml);
                }
            } else if (pop) {
                /* pop one random member at a time: indices shift as the
                 * listpack shrinks, so no upfront shuffle is possible */
                size_t k2 = (size_t)count < n ? (size_t)count : n;
                resp_write_array_header(out, k2);
                for (i = 0; i < k2; i++) {
                    const char *mv;
                    size_t ml = 0;
                    char copy[OBJ_SET_MAX_LISTPACK_VALUE];
                    uint64_t idx = db_rand(d) % (uint32_t)obj_set_len(s);
                    if (!obj_set_member_at(s, idx, &mv, &ml))
                        continue;
                    memcpy(copy, mv, ml);
                    obj_set_rem(s, copy, ml);
                    resp_write_bulk(out, copy, ml);
                }
                mem_sync(d, k, kl, before, obj_set_mem(s));
                if (obj_set_len(s) == 0)
                    db_del_kv(d, k, kl);
            } else {
                /* distinct: partial Fisher-Yates over member indices */
                size_t k2 = (size_t)count < n ? (size_t)count : n;
                uint32_t *idxs = (uint32_t *)malloc(n * sizeof(*idxs));
                if (idxs == NULL) {
                    fprintf(stderr, "ddup: out of memory\n");
                    exit(1);
                }
                for (i = 0; i < n; i++)
                    idxs[i] = (uint32_t)i;
                for (i = 0; i < k2; i++) {
                    size_t j = i + (size_t)(db_rand(d) % (uint32_t)(n - i));
                    uint32_t tmp = idxs[i];
                    idxs[i] = idxs[j];
                    idxs[j] = tmp;
                }
                resp_write_array_header(out, k2);
                for (i = 0; i < k2; i++) {
                    const char *mv;
                    size_t ml = 0;
                    if (!obj_set_member_at(s, idxs[i], &mv, &ml))
                        continue;
                    resp_write_bulk(out, mv, ml);
                }
                free(idxs);
            }
            return;
        }
        {
            collect_ctx cc = {0};
            size_t n, i, k2;
            uint64_t before = obj_set_mem(s);
            rh_each(&s->members, collect_cb, &cc);
            n = cc.n;
            if (count < 0) {
                /* with repeats */
                resp_write_array_header(out, (size_t)-count);
                for (i = 0; i < (size_t)-count; i++) {
                    size_t idx = (size_t)(db_rand(d) % (uint32_t)n);
                    resp_write_bulk(out, cc.keys[idx], cc.lens[idx]);
                }
            } else {
                k2 = (size_t)count < n ? (size_t)count : n;
                collect_shuffle(d, &cc, k2);
                resp_write_array_header(out, k2);
                for (i = 0; i < k2; i++) {
                    if (pop) {
                        char *copy = (char *)malloc(cc.lens[i] + 1);
                        memcpy(copy, cc.keys[i], cc.lens[i]);
                        obj_set_rem(s, copy, cc.lens[i]);
                        resp_write_bulk(out, copy, cc.lens[i]);
                        free(copy);
                    } else {
                        resp_write_bulk(out, cc.keys[i], cc.lens[i]);
                    }
                }
            }
            if (pop) {
                mem_sync(d, k, kl, before, obj_set_mem(s));
                if (obj_set_len(s) == 0)
                    db_del_kv(d, k, kl);
            }
            free(cc.keys);
            free(cc.lens);
        }
        return;
    }

    if (cmd_id == CMD_SMOVE) {
        if (argc != 4) {
            wrong_args(out, "smove");
            return;
        }
        const char *sk, *dk, *m;
        size_t skl, dkl, ml;
        if (!arg_str(&argv[1], &sk, &skl) || !arg_str(&argv[2], &dk, &dkl) ||
            !arg_str(&argv[3], &m, &ml))
            goto bad_type;
        if (!storage_key_ok(skl) || !storage_key_ok(dkl) ||
            !storage_key_ok(ml)) {
            storage_length_error(out);
            return;
        }
        obj_set *src, *dst;
        int rcs = get_set(d, out, sk, skl, 0, now_ms, &src);
        if (rcs < 0)
            return;
        if (rcs == 0 || !obj_set_has(src, m, ml)) {
            resp_write_integer(out, 0);
            return;
        }
        if (skl == dkl && memcmp(sk, dk, skl) == 0) {
            resp_write_integer(out, 1); /* same key: no-op */
            return;
        }
        /* type-check dst before mutating src */
        int rcd;
        int created_dst = 0;
        {
            rcd = get_set(d, out, dk, dkl, 0, now_ms, &dst);
            if (rcd < 0)
                return;
        }
        if (oom_blocked(d, out))
            return;
        if (rcd == 0) {
            rcd = get_set(d, out, dk, dkl, 1, now_ms, &dst);
            if (rcd < 0)
                return;
            created_dst = 1;
        }
        {
            uint64_t before = obj_set_mem(dst);
            if (obj_set_add(dst, m, ml) < 0) {
                if (created_dst)
                    db_del_kv(d, dk, dkl);
                storage_length_error(out);
                return;
            }
            mem_sync(d, dk, dkl, before, obj_set_mem(dst));
        }
        {
            uint64_t before = obj_set_mem(src);
            obj_set_rem(src, m, ml);
            mem_sync(d, sk, skl, before, obj_set_mem(src));
        }
        if (obj_set_len(src) == 0)
            db_del_kv(d, sk, skl);
        resp_write_integer(out, 1);
        return;
    }

    if (cmd_id == CMD_SINTER || cmd_id == CMD_SUNION ||
        cmd_id == CMD_SDIFF) {
        int inter = cmd_id == CMD_SINTER;
        int sunion = cmd_id == CMD_SUNION;
        if (argc < 2) {
            wrong_args(out, inter ? "sinter" : sunion ? "sunion" : "sdiff");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        {
            rh_table result;
            rh_init(&result);
            if (setop_eval(d, out, argv + 1, argc - 1, inter, sunion, now_ms,
                           &result) != 0) {
                rh_destroy(&result);
                return;
            }
            {
                hdump_ctx dctx;
                dctx.out = out;
                dctx.keys = 1;
                dctx.vals = 0;
                resp_write_array_header(out, rh_size(&result));
                rh_each(&result, hdump_cb, &dctx);
            }
            rh_destroy(&result);
        }
        return;
    }


    if (cmd_id == CMD_SUNIONCARD || cmd_id == CMD_SDIFFCARD) {
        int is_union = cmd_id == CMD_SUNIONCARD;
        const char *nv;
        size_t nvl;
        long long nk, limit = 0;
        size_t nkeys, i, j;
        obj_set **sets;
        rh_table seen;
        long long card = 0;
        if (!arg_str(&argv[1], &nv, &nvl))
            goto bad_type;
        if (!parse_i64(nv, nvl, &nk)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (nk <= 0) {
            static const char E[] = "ERR numkeys should be greater than 0";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if ((unsigned long long)nk > argc - 2) {
            static const char E[] =
                "ERR Number of keys can't be greater than number of args";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        nkeys = (size_t)nk;
        for (j = 2 + nkeys; j < argc; j++) {
            const char *opt;
            size_t optl;
            if (!arg_str(&argv[j], &opt, &optl))
                goto bad_type;
            if (ci_equal(opt, optl, "LIMIT") && j + 1 < argc) {
                const char *lv;
                size_t lvl;
                if (!arg_str(&argv[++j], &lv, &lvl) ||
                    !parse_i64(lv, lvl, &limit) || limit < 0) {
                    static const char E[] = "ERR LIMIT can't be negative";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
            } else if (is_union && ci_equal(opt, optl, "APPROX")) {
                continue;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        sets = (obj_set **)malloc(nkeys * sizeof(*sets));
        for (i = 0; i < nkeys; i++) {
            const char *k;
            size_t kl;
            obj_set *set = NULL;
            int rc;
            if (!arg_str(&argv[2 + i], &k, &kl)) {
                free(sets);
                goto bad_type;
            }
            rc = get_set(d, out, k, kl, 0, now_ms, &set);
            if (rc < 0) {
                free(sets);
                return;
            }
            sets[i] = rc == 1 ? set : NULL;
        }
        rh_init(&seen);
        if (is_union) {
            setcard_ctx ctx;
            ctx.seen = &seen;
            ctx.limit = limit;
            ctx.count = 0;
            ctx.stop = 0;
            for (i = 0; i < nkeys && !ctx.stop; i++) {
                if (sets[i] == NULL)
                    continue;
                if (obj_set_is_listpack(sets[i]))
                    obj_set_each(sets[i], setcard_union_cb, &ctx);
                else
                    rh_each(&sets[i]->members, setcard_union_cb, &ctx);
            }
            card = ctx.count;
        } else {
            for (i = 1; i < nkeys; i++) {
                if (sets[i] == NULL)
                    continue;
                if (obj_set_is_listpack(sets[i]))
                    obj_set_each(sets[i], setcard_collect_cb, &seen);
                else
                    rh_each(&sets[i]->members, setcard_collect_cb, &seen);
            }
            if (sets[0] != NULL) {
                setcard_ctx ctx;
                ctx.seen = &seen;
                ctx.limit = limit;
                ctx.count = 0;
                ctx.stop = 0;
                if (obj_set_is_listpack(sets[0]))
                    obj_set_each(sets[0], setcard_diff_cb, &ctx);
                else
                    rh_each(&sets[0]->members, setcard_diff_cb, &ctx);
                card = ctx.count;
            }
        }
        rh_destroy(&seen);
        free(sets);
        resp_write_integer(out, card);
        return;
    }

    if (cmd_id == CMD_SINTERCARD) {
        if (argc < 3) {
            wrong_args(out, "sintercard");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        {
            const char *nv;
            size_t nvl;
            long long nk, limit = 0;
            size_t nkeys, rest, i;
            obj_set **sets;
            long long card = 0;
            if (!arg_str(&argv[1], &nv, &nvl))
                goto bad_type;
            if (!parse_i64(nv, nvl, &nk)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (nk <= 0 || (unsigned long long)nk > argc - 2) {
                static const char E[] =
                    "ERR Number of keys can't be greater than number of args";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            nkeys = (size_t)nk;
            rest = argc - 2 - nkeys;
            if (rest == 2) {
                const char *opt, *lv;
                size_t optl, lvl;
                if (!arg_str(&argv[2 + nkeys], &opt, &optl) ||
                    !arg_str(&argv[3 + nkeys], &lv, &lvl))
                    goto bad_type;
                if (!ci_equal(opt, optl, "LIMIT")) {
                    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                    return;
                }
                if (!parse_i64(lv, lvl, &limit)) {
                    resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (limit < 0) {
                    static const char E[] = "ERR LIMIT can't be negative";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
            } else if (rest != 0) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            sets = (obj_set **)malloc(nkeys * sizeof(*sets));
            for (i = 0; i < nkeys; i++) {
                const char *k;
                size_t kl;
                obj_set *s = NULL;
                int rc;
                if (!arg_str(&argv[2 + i], &k, &kl)) {
                    free(sets);
                    goto bad_type;
                }
                rc = get_set(d, out, k, kl, 0, now_ms, &s);
                if (rc < 0) {
                    free(sets);
                    return;
                }
                sets[i] = rc == 1 ? s : NULL;
            }
            /* a missing operand empties the intersection */
            for (i = 0; i < nkeys && sets[i] != NULL; i++)
                ;
            if (i == nkeys) {
                sintercard_ctx ctx;
                ctx.sets = sets;
                ctx.n = nkeys;
                ctx.count = 0;
                ctx.limit = limit;
                /* count matches against sets[1..n); LIMIT stops early */
                if (obj_set_is_listpack(sets[0])) {
                    sintercard_lp_ctx w;
                    w.ic = ctx;
                    w.stop = 0;
                    obj_set_each(sets[0], sintercard_lp_cb, &w);
                    ctx.count = w.ic.count;
                } else {
                    (void)rh_scan(&sets[0]->members, 0, SIZE_MAX,
                                  sintercard_cb, &ctx);
                }
                card = ctx.count;
            }
            free(sets);
            resp_write_integer(out, card);
        }
        return;
    }

    if (cmd_id == CMD_SINTERSTORE || cmd_id == CMD_SUNIONSTORE ||
        cmd_id == CMD_SDIFFSTORE) {
        int inter = cmd_id == CMD_SINTERSTORE;
        int sunion = cmd_id == CMD_SUNIONSTORE;
        if (argc < 3) {
            wrong_args(out, inter ? "sinterstore"
                                  : sunion ? "sunionstore" : "sdiffstore");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        {
            const char *dk;
            size_t dkl;
            rh_table result;
            size_t card;
            if (!arg_str(&argv[1], &dk, &dkl))
                goto bad_type;
            if (!storage_key_ok(dkl)) {
                storage_length_error(out);
                return;
            }
            rh_init(&result);
            if (setop_eval(d, out, argv + 2, argc - 2, inter, sunion, now_ms,
                           &result) != 0) {
                rh_destroy(&result);
                return;
            }
            card = rh_size(&result);
            if (card == 0) {
                db_del_kv(d, dk, dkl); /* empty result: dst goes away */
            } else {
                /* materialize the result as dst's set (any old type and
                 * TTL are overwritten, per Redis STORE semantics) */
                obj_set *ns = obj_set_new();
                char blob[9];
                rh_each(&result, set_store_cb, ns);
                obj_pack_ptr(blob, DDUP_OBJ_SET, ns);
                if (db_set_kv(d, dk, dkl, blob, 9, now_ms) != 0) {
                    obj_set_free(ns);
                    rh_destroy(&result);
                    storage_length_error(out);
                    return;
                }
            }
            rh_destroy(&result);
            resp_write_integer(out, (long long)card);
        }
        return;
    }

    /* ---------------- zset commands ---------------- */

    if (cmd_id == CMD_ZADD) {
        const char *k;
        size_t kl;
        int nx = 0, xx = 0, gt = 0, lt = 0, ch = 0, incr = 0;
        size_t start = 2;
        size_t i;
        if (argc < 4) {
            wrong_args(out, "zadd");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        while (start < argc) {
            const char *opt;
            size_t opl;
            if (!arg_str(&argv[start], &opt, &opl))
                goto bad_type;
            if (ci_equal(opt, opl, "NX") && !nx && !xx) {
                nx = 1;
                start++;
            } else if (ci_equal(opt, opl, "XX") && !nx && !xx) {
                xx = 1;
                start++;
            } else if (ci_equal(opt, opl, "GT") && !gt && !lt) {
                gt = 1;
                start++;
            } else if (ci_equal(opt, opl, "LT") && !gt && !lt) {
                lt = 1;
                start++;
            } else if (ci_equal(opt, opl, "CH") && !ch) {
                ch = 1;
                start++;
            } else if (ci_equal(opt, opl, "INCR") && !incr) {
                incr = 1;
                start++;
            } else {
                break;
            }
        }
        if (incr) {
            if (start + 2 != argc) {
                wrong_args(out, "zadd");
                return;
            }
        } else if (start >= argc || (argc - start) % 2 != 0) {
            wrong_args(out, "zadd");
            return;
        }
        if (incr) {
            const char *sv, *m;
            size_t svl, ml;
            double score, old, final_score;
            int has_old;
            long long rc;
            if (!arg_str(&argv[start], &sv, &svl) ||
                !arg_str(&argv[start + 1], &m, &ml))
                goto bad_type;
            if (!parse_double(sv, svl, &score)) {
                resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
                return;
            }
            if (!storage_key_ok(ml)) {
                storage_length_error(out);
                return;
            }
            {
                obj_zset *z;
                int zrc = get_zset(d, out, k, kl, xx ? 0 : 1, now_ms, &z);
                if (zrc < 0)
                    return;
                if (zrc == 0) {
                    resp_write_bulk(out, NULL, 0);
                    return;
                }
                has_old = obj_zset_score(z, m, ml, &old);
                final_score = has_old ? old + score : score;
                if (final_score != final_score) {
                    resp_write_error(out,
                                     "ERR resulting score is not a number (NaN)",
                                     41);
                    return;
                }
                if (nx && has_old) {
                    resp_write_bulk(out, NULL, 0);
                    return;
                }
                if (xx && !has_old) {
                    resp_write_bulk(out, NULL, 0);
                    return;
                }
                if (gt && has_old && !(final_score > old)) {
                    resp_write_bulk(out, NULL, 0);
                    return;
                }
                if (lt && has_old && !(final_score < old)) {
                    resp_write_bulk(out, NULL, 0);
                    return;
                }
                {
                    uint64_t before = obj_zset_mem(z);
                    rc = obj_zset_add(z, m, ml, final_score);
                    mem_sync(d, k, kl, before, obj_zset_mem(z));
                }
            }
            if (rc < 0) {
                storage_length_error(out);
                return;
            }
            {
                char num[40];
                int nl = fmt_score(num, sizeof(num), final_score);
                resp_write_bulk(out, num, (size_t)nl);
            }
            return;
        }
        {
            obj_zset *z;
            long long added = 0, changed = 0;
            uint64_t before;
            {
                int zrc = get_zset(d, out, k, kl, xx ? 0 : 1, now_ms, &z);
                if (zrc < 0)
                    return;
                if (zrc == 0) {
                    resp_write_integer(out, 0);
                    return;
                }
            }
            before = obj_zset_mem(z);
            for (i = start; i < argc; i += 2) {
                const char *sv, *m;
                size_t svl, ml;
                double score, old;
                int has_old;
                int add_rc;
                if (!arg_str(&argv[i], &sv, &svl) ||
                    !arg_str(&argv[i + 1], &m, &ml))
                    goto bad_type;
                if (!parse_double(sv, svl, &score)) {
                    resp_write_error(out, ERR_NOT_FLOAT,
                                     sizeof(ERR_NOT_FLOAT) - 1);
                    return;
                }
                if (!storage_key_ok(ml)) {
                    storage_length_error(out);
                    return;
                }
                has_old = obj_zset_score(z, m, ml, &old);
                if (nx && has_old)
                    continue;
                if (xx && !has_old)
                    continue;
                if (gt && has_old && !(score > old))
                    continue;
                if (lt && has_old && !(score < old))
                    continue;
                add_rc = obj_zset_add(z, m, ml, score);
                if (add_rc < 0) {
                    storage_length_error(out);
                    return;
                }
                if (add_rc)
                    added++;
                changed++;
            }
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            resp_write_integer(out, ch ? changed : added);
        }
        return;
    }

    if (cmd_id == CMD_ZSCORE) {
        if (argc != 3) {
            wrong_args(out, "zscore");
            return;
        }
        const char *k, *m;
        size_t kl, ml;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &m, &ml))
            goto bad_type;
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        double sc;
        if (rc == 1 && obj_zset_score(z, m, ml, &sc)) {
            char num[40];
            int nl = fmt_score(num, sizeof(num), sc);
            resp_write_bulk(out, num, (size_t)nl);
        } else {
            resp_write_bulk(out, NULL, 0);
        }
        return;
    }

    if (cmd_id == CMD_ZCARD) {
        if (argc != 2) {
            wrong_args(out, "zcard");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        resp_write_integer(out, rc == 1 ? (long long)obj_zset_len(z) : 0);
        return;
    }

    if (cmd_id == CMD_ZINCRBY) {
        if (argc != 4) {
            wrong_args(out, "zincrby");
            return;
        }
        const char *k, *dv, *m;
        size_t kl, dvl, ml;
        double delta, cur = 0.0;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &dv, &dvl) ||
            !arg_str(&argv[3], &m, &ml))
            goto bad_type;
        if (!storage_key_ok(kl) || !storage_key_ok(ml)) {
            storage_length_error(out);
            return;
        }
        if (!parse_double(dv, dvl, &delta)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        if (oom_blocked(d, out))
            return;
        obj_zset *z;
        if (get_zset(d, out, k, kl, 1, now_ms, &z) <= 0)
            return;
        obj_zset_score(z, m, ml, &cur);
        cur += delta;
        if (cur != cur) {
            resp_write_error(out, "ERR resulting score is not a number (NaN)",
                             41);
            return;
        }
        {
            uint64_t before = obj_zset_mem(z);
            char num[40];
            int nl;
            if (obj_zset_add(z, m, ml, cur) < 0) {
                storage_length_error(out);
                return;
            }
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            nl = fmt_score(num, sizeof(num), cur);
            resp_write_bulk(out, num, (size_t)nl);
        }
        return;
    }

    if (cmd_id == CMD_ZREM) {
        if (argc < 3) {
            wrong_args(out, "zrem");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        long long removed = 0;
        uint64_t before = obj_zset_mem(z);
        for (size_t i = 2; i < argc; i++) {
            const char *m;
            size_t ml;
            if (!arg_str(&argv[i], &m, &ml))
                goto bad_type;
            removed += obj_zset_rem(z, m, ml);
        }
        mem_sync(d, k, kl, before, obj_zset_mem(z));
        if (obj_zset_len(z) == 0)
            db_del_kv(d, k, kl); /* empty zset: the key goes away */
        resp_write_integer(out, removed);
        return;
    }

    if (cmd_id == CMD_ZRANGE || cmd_id == CMD_ZREVRANGE) {
        int rev = cmd_id == CMD_ZREVRANGE;
        {
            const char *o4;
            size_t o4l;
            int old_withscores = 0;
            if (argc == 5 && arg_str(&argv[4], &o4, &o4l) &&
                ci_equal(o4, o4l, "WITHSCORES"))
                old_withscores = 1;
            if (cmd_id == CMD_ZRANGE && argc >= 5 && !old_withscores) {
            const char *k, *sv, *ev;
            size_t kl, svl, evl;
            int byscore = 0, bylex = 0, opt_rev = 0, withscores = 0;
            long long off = 0, cnt = -1;
            size_t i;
            obj_zset *z;
            int rc;
            if (argc < 4) {
                wrong_args(out, "zrange");
                return;
            }
            if (!arg_str(&argv[1], &k, &kl) ||
                !arg_str(&argv[2], &sv, &svl) ||
                !arg_str(&argv[3], &ev, &evl))
                goto bad_type;
            for (i = 4; i < argc; i++) {
                const char *o;
                size_t ol;
                if (!arg_str(&argv[i], &o, &ol))
                    goto bad_type;
                if (ci_equal(o, ol, "BYSCORE") && !byscore && !bylex) {
                    byscore = 1;
                } else if (ci_equal(o, ol, "BYLEX") && !byscore && !bylex) {
                    bylex = 1;
                } else if (ci_equal(o, ol, "REV") && !opt_rev) {
                    opt_rev = 1;
                } else if (ci_equal(o, ol, "WITHSCORES") && !withscores) {
                    withscores = 1;
                } else if (ci_equal(o, ol, "LIMIT") && i + 2 < argc) {
                    const char *ov, *cv;
                    size_t ovl, cvl;
                    if (!arg_str(&argv[i + 1], &ov, &ovl) ||
                        !arg_str(&argv[i + 2], &cv, &cvl))
                        goto bad_type;
                    if (!parse_i64(ov, ovl, &off) ||
                        !parse_i64(cv, cvl, &cnt)) {
                        resp_write_error(out, ERR_NOT_INT,
                                         sizeof(ERR_NOT_INT) - 1);
                        return;
                    }
                    i += 2;
                } else {
                    resp_write_error(out, ERR_SYNTAX,
                                     sizeof(ERR_SYNTAX) - 1);
                    return;
                }
            }
            if (off < 0)
                off = 0;
            rc = get_zset(d, out, k, kl, 0, now_ms, &z);
            if (rc < 0)
                return;
            if (rc == 0) {
                resp_write_array_header(out, 0);
                return;
            }
            if (byscore) {
                zrangespec spec;
                const char *mins, *maxs;
                size_t minsl, maxsl;
                if (opt_rev) {
                    mins = ev;
                    minsl = evl;
                    maxs = sv;
                    maxsl = svl;
                } else {
                    mins = sv;
                    minsl = svl;
                    maxs = ev;
                    maxsl = evl;
                }
                if (!parse_bound(mins, minsl, &spec.min, &spec.minex) ||
                    !parse_bound(maxs, maxsl, &spec.max, &spec.maxex)) {
                    resp_write_error(out, ERR_NOT_FLOAT,
                                     sizeof(ERR_NOT_FLOAT) - 1);
                    return;
                }
                {
                    obj_zset_iter first, last, it;
                    long long c = 0, emitted = 0;
                    if (!obj_zset_first_in_range(z, &spec, &first) ||
                        !obj_zset_last_in_range(z, &spec, &last)) {
                        resp_write_array_header(out, 0);
                        return;
                    }
                    if (!opt_rev) {
                        it = first;
                        for (;;) {
                            if (c >= off)
                                emitted++;
                            c++;
                            if (obj_zset_iter_eq(&it, &last))
                                break;
                            if (!obj_zset_iter_next(&it))
                                break;
                        }
                    } else {
                        it = last;
                        for (;;) {
                            if (c >= off)
                                emitted++;
                            c++;
                            if (obj_zset_iter_eq(&it, &first))
                                break;
                            if (!obj_zset_iter_prev(&it))
                                break;
                        }
                    }
                    if (cnt >= 0 && emitted > cnt)
                        emitted = cnt;
                    resp_write_array_header(out, (size_t)emitted *
                                                     (withscores ? 2u : 1u));
                    c = 0;
                    if (!opt_rev) {
                        it = first;
                        for (;;) {
                            if (c >= off && (cnt < 0 || c - off < cnt))
                                zset_emit_member(out, &it, withscores);
                            c++;
                            if (obj_zset_iter_eq(&it, &last))
                                break;
                            if (!obj_zset_iter_next(&it))
                                break;
                        }
                    } else {
                        it = last;
                        for (;;) {
                            if (c >= off && (cnt < 0 || c - off < cnt))
                                zset_emit_member(out, &it, withscores);
                            c++;
                            if (obj_zset_iter_eq(&it, &first))
                                break;
                            if (!obj_zset_iter_prev(&it))
                                break;
                        }
                    }
                }
                return;
            }
            if (bylex) {
                zlexrangespec spec;
                const char *mins, *maxs;
                size_t minsl, maxsl;
                if (opt_rev) {
                    mins = ev;
                    minsl = evl;
                    maxs = sv;
                    maxsl = svl;
                } else {
                    mins = sv;
                    minsl = svl;
                    maxs = ev;
                    maxsl = evl;
                }
                if (!parse_lex_bound(mins, minsl, &spec.min) ||
                    !parse_lex_bound(maxs, maxsl, &spec.max)) {
                    static const char lex_err[] =
                        "ERR min or max is not a valid string range item";
                    resp_write_error(out, lex_err, sizeof(lex_err) - 1);
                    return;
                }
                {
                    obj_zset_iter first, last, it;
                    long long c = 0, emitted = 0;
                    if (!obj_zset_first_in_lex_range(z, &spec, &first) ||
                        !obj_zset_last_in_lex_range(z, &spec, &last)) {
                        resp_write_array_header(out, 0);
                        return;
                    }
                    if (!opt_rev) {
                        it = first;
                        for (;;) {
                            if (c >= off)
                                emitted++;
                            c++;
                            if (obj_zset_iter_eq(&it, &last))
                                break;
                            if (!obj_zset_iter_next(&it))
                                break;
                        }
                    } else {
                        it = last;
                        for (;;) {
                            if (c >= off)
                                emitted++;
                            c++;
                            if (obj_zset_iter_eq(&it, &first))
                                break;
                            if (!obj_zset_iter_prev(&it))
                                break;
                        }
                    }
                    if (cnt >= 0 && emitted > cnt)
                        emitted = cnt;
                    resp_write_array_header(out, (size_t)emitted *
                                                     (withscores ? 2u : 1u));
                    c = 0;
                    if (!opt_rev) {
                        it = first;
                        for (;;) {
                            if (c >= off && (cnt < 0 || c - off < cnt))
                                zset_emit_member(out, &it, withscores);
                            c++;
                            if (obj_zset_iter_eq(&it, &last))
                                break;
                            if (!obj_zset_iter_next(&it))
                                break;
                        }
                    } else {
                        it = last;
                        for (;;) {
                            if (c >= off && (cnt < 0 || c - off < cnt))
                                zset_emit_member(out, &it, withscores);
                            c++;
                            if (obj_zset_iter_eq(&it, &first))
                                break;
                            if (!obj_zset_iter_prev(&it))
                                break;
                        }
                    }
                }
                return;
            }
            {
                long long start, stop;
                long long len;
                if (!parse_i64(sv, svl, &start) ||
                    !parse_i64(ev, evl, &stop)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                len = (long long)obj_zset_len(z);
                if (start < 0)
                    start += len;
                if (start < 0)
                    start = 0;
                if (stop < 0)
                    stop += len;
                if (stop >= len)
                    stop = len - 1;
                if (start > stop || start >= len || stop < 0) {
                    resp_write_array_header(out, 0);
                    return;
                }
                if (opt_rev) {
                    long long rstart = len - 1 - stop;
                    long long rstop = len - 1 - start;
                    start = rstart;
                    stop = rstop;
                }
                if (off > 0) {
                    if (start + off > stop) {
                        resp_write_array_header(out, 0);
                        return;
                    }
                    start += off;
                }
                if (cnt >= 0 && start + cnt - 1 < stop)
                    stop = start + cnt - 1;
                {
                    obj_zset_iter it;
                    long long idx;
                    long long count = stop - start + 1;
                    if (count < 0)
                        count = 0;
                    resp_write_array_header(out, (size_t)count *
                                                     (withscores ? 2u : 1u));
                    if (opt_rev) {
                        if (obj_zset_seek(z, (size_t)stop, &it)) {
                            for (idx = stop; idx >= start; idx--) {
                                zset_emit_member(out, &it, withscores);
                                if (idx != start &&
                                    !obj_zset_iter_prev(&it))
                                    break;
                            }
                        }
                    } else {
                        if (obj_zset_seek(z, (size_t)start, &it)) {
                            for (idx = start; idx <= stop; idx++) {
                                zset_emit_member(out, &it, withscores);
                                if (idx != stop &&
                                    !obj_zset_iter_next(&it))
                                    break;
                            }
                        }
                    }
                }
            }
            return;
            }
        }
        if (argc != 4 && argc != 5) {
            wrong_args(out, rev ? "zrevrange" : "zrange");
            return;
        }
        const char *k, *sv, *ev;
        size_t kl, svl, evl;
        long long start, stop;
        int withscores = 0;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &sv, &svl) ||
            !arg_str(&argv[3], &ev, &evl))
            goto bad_type;
        if (argc == 5) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[4], &o, &ol))
                goto bad_type;
            if (!ci_equal(o, ol, "WITHSCORES")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            withscores = 1;
        }
        if (!parse_i64(sv, svl, &start) || !parse_i64(ev, evl, &stop)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        {
            long long len = (long long)obj_zset_len(z);
            long long i;
            obj_zset_iter it;
            if (start < 0)
                start += len;
            if (start < 0)
                start = 0;
            if (stop < 0)
                stop += len;
            if (stop >= len)
                stop = len - 1;
            if (start > stop || start >= len || stop < 0) {
                resp_write_array_header(out, 0);
                return;
            }
            resp_write_array_header(out,
                                    (size_t)(stop - start + 1) *
                                        (withscores ? 2u : 1u));
            if (!rev) {
                if (obj_zset_seek(z, (size_t)start, &it)) {
                    for (i = start; i <= stop; i++) {
                        zset_emit_member(out, &it, withscores);
                        if (i != stop && !obj_zset_iter_next(&it))
                            break;
                    }
                }
            } else {
                /* reversed index p == forward index len-1-p */
                if (obj_zset_seek(z, (size_t)(len - 1 - start), &it)) {
                    for (i = start; i <= stop; i++) {
                        zset_emit_member(out, &it, withscores);
                        if (i != stop && !obj_zset_iter_prev(&it))
                            break;
                    }
                }
            }
        }
        return;
    }

    if (cmd_id == CMD_ZRANK || cmd_id == CMD_ZREVRANK) {
        int rev = cmd_id == CMD_ZREVRANK;
        if (argc != 3) {
            wrong_args(out, rev ? "zrevrank" : "zrank");
            return;
        }
        const char *k, *m;
        size_t kl, ml;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &m, &ml))
            goto bad_type;
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        double sc;
        if (rc == 0 || !obj_zset_score(z, m, ml, &sc)) {
            resp_write_bulk(out, NULL, 0);
            return;
        }
        {
            long rank = obj_zset_rank(z, sc, m, ml);
            if (rank < 0) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            resp_write_integer(out, rev ? (long long)obj_zset_len(z) - 1 - rank
                                        : rank);
        }
        return;
    }

    if (cmd_id == CMD_ZCOUNT) {
        if (argc != 4) {
            wrong_args(out, "zcount");
            return;
        }
        const char *k, *minv, *maxv;
        size_t kl, minvl, maxvl;
        zrangespec spec;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &minv, &minvl) ||
            !arg_str(&argv[3], &maxv, &maxvl))
            goto bad_type;
        if (!parse_bound(minv, minvl, &spec.min, &spec.minex) ||
            !parse_bound(maxv, maxvl, &spec.max, &spec.maxex)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        resp_write_integer(out,
                           rc == 1
                               ? (long long)obj_zset_count_in_range(z, &spec)
                               : 0);
        return;
    }

    if (cmd_id == CMD_ZRANGEBYSCORE) {
        if (argc < 4) {
            wrong_args(out, "zrangebyscore");
            return;
        }
        const char *k, *minv, *maxv;
        size_t kl, minvl, maxvl;
        zrangespec spec;
        int withscores = 0;
        long long off = 0, cnt = -1;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &minv, &minvl) ||
            !arg_str(&argv[3], &maxv, &maxvl))
            goto bad_type;
        if (!parse_bound(minv, minvl, &spec.min, &spec.minex) ||
            !parse_bound(maxv, maxvl, &spec.max, &spec.maxex)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        for (size_t i = 4; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "WITHSCORES") && !withscores) {
                withscores = 1;
            } else if (ci_equal(o, ol, "LIMIT") && i + 2 < argc) {
                const char *ov, *cv;
                size_t ovl, cvl;
                if (!arg_str(&argv[i + 1], &ov, &ovl) ||
                    !arg_str(&argv[i + 2], &cv, &cvl))
                    goto bad_type;
                if (!parse_i64(ov, ovl, &off) ||
                    !parse_i64(cv, cvl, &cnt)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                i += 2;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (off < 0)
            off = 0;
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        {
            /* two passes over the range: count, then emit */
            long long emitted = 0;
            long long c;
            obj_zset_iter it;
            c = 0;
            if (obj_zset_first_in_range(z, &spec, &it)) {
                for (;;) {
                    double sc = obj_zset_iter_score(&it);
                    if (spec.maxex ? !(sc < spec.max) : !(sc <= spec.max))
                        break;
                    if (c >= off)
                        emitted++;
                    c++;
                    if (!obj_zset_iter_next(&it))
                        break;
                }
            }
            if (cnt >= 0 && emitted > cnt)
                emitted = cnt;
            resp_write_array_header(out,
                                    (size_t)emitted * (withscores ? 2u : 1u));
            c = 0;
            if (obj_zset_first_in_range(z, &spec, &it)) {
                for (;;) {
                    double sc = obj_zset_iter_score(&it);
                    if (spec.maxex ? !(sc < spec.max) : !(sc <= spec.max))
                        break;
                    if (c >= off && (cnt < 0 || c - off < cnt))
                        zset_emit_member(out, &it, withscores);
                    c++;
                    if (!obj_zset_iter_next(&it))
                        break;
                }
            }
        }
        return;
    }

    if (cmd_id == CMD_ZREMRANGEBYSCORE) {
        if (argc != 4) {
            wrong_args(out, "zremrangebyscore");
            return;
        }
        const char *k, *minv, *maxv;
        size_t kl, minvl, maxvl;
        zrangespec spec;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &minv, &minvl) ||
            !arg_str(&argv[3], &maxv, &maxvl))
            goto bad_type;
        if (!parse_bound(minv, minvl, &spec.min, &spec.minex) ||
            !parse_bound(maxv, maxvl, &spec.max, &spec.maxex)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        {
            /* bulk removal happens at the obj layer: collecting member
             * views first would dangle under listpack realloc */
            long long removed;
            uint64_t before = obj_zset_mem(z);
            removed = (long long)obj_zset_rem_range_by_score(z, &spec);
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            if (obj_zset_len(z) == 0)
                db_del_kv(d, k, kl);
            resp_write_integer(out, removed);
        }
        return;
    }

    if (cmd_id == CMD_ZPOPMIN || cmd_id == CMD_ZPOPMAX) {
        int min_side = cmd_id == CMD_ZPOPMIN;
        if (argc != 2 && argc != 3) {
            wrong_args(out, min_side ? "zpopmin" : "zpopmax");
            return;
        }
        const char *k;
        size_t kl;
        long long count = 1;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (argc == 3) {
            const char *cv;
            size_t cvl;
            if (!arg_str(&argv[2], &cv, &cvl))
                goto bad_type;
            if (!parse_i64(cv, cvl, &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (count <= 0) {
                static const char range_err[] =
                    "ERR value is out of range, must be positive";
                resp_write_error(out, range_err, sizeof(range_err) - 1);
                return;
            }
        }
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        {
            /* flat member/score pairs (Redis 6.2+ also without count) */
            size_t avail = (size_t)obj_zset_len(z);
            size_t n = (uint64_t)count < (uint64_t)avail ? (size_t)count
                                                         : avail;
            uint64_t before = obj_zset_mem(z);
            size_t i;
            resp_write_array_header(out, n * 2);
            for (i = 0; i < n; i++) {
                char *mv = NULL;
                size_t ml = 0;
                double sc = 0;
                char num[40];
                int nl;
                if (!obj_zset_pop(z, min_side, &mv, &ml, &sc))
                    break;
                nl = fmt_score(num, sizeof(num), sc);
                resp_write_bulk(out, mv, ml);
                resp_write_bulk(out, num, (size_t)nl);
                free(mv);
            }
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            if (obj_zset_len(z) == 0)
                db_del_kv(d, k, kl); /* empty zset: the key goes away */
        }
        return;
    }

    if (cmd_id == CMD_ZREMRANGEBYRANK) {
        if (argc != 4) {
            wrong_args(out, "zremrangebyrank");
            return;
        }
        const char *k, *sv, *ev;
        size_t kl, svl, evl;
        long long start, stop;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &sv, &svl) ||
            !arg_str(&argv[3], &ev, &evl))
            goto bad_type;
        if (!parse_i64(sv, svl, &start) || !parse_i64(ev, evl, &stop)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        {
            /* same negative-index/clamp rules as ZRANGE */
            long long len = (long long)obj_zset_len(z);
            long long removed = 0;
            uint64_t before = obj_zset_mem(z);
            if (start < 0)
                start += len;
            if (start < 0)
                start = 0;
            if (stop < 0)
                stop += len;
            if (stop >= len)
                stop = len - 1;
            if (start <= stop && start < len && stop >= 0)
                removed = (long long)obj_zset_rem_range_by_rank(
                    z, (size_t)start, (size_t)stop);
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            if (obj_zset_len(z) == 0)
                db_del_kv(d, k, kl);
            resp_write_integer(out, removed);
        }
        return;
    }

    if (cmd_id == CMD_ZMSCORE) {
        if (argc < 3) {
            wrong_args(out, "zmscore");
            return;
        }
        const char *k;
        size_t kl;
        size_t i;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        resp_write_array_header(out, argc - 2);
        for (i = 2; i < argc; i++) {
            const char *m;
            size_t ml;
            double sc;
            if (!arg_str(&argv[i], &m, &ml))
                goto bad_type;
            if (rc == 1 && obj_zset_score(z, m, ml, &sc)) {
                char num[40];
                int nl = fmt_score(num, sizeof(num), sc);
                resp_write_bulk(out, num, (size_t)nl);
            } else {
                resp_write_bulk(out, NULL, 0);
            }
        }
        return;
    }

    if (cmd_id == CMD_ZRANDMEMBER) {
        if (argc < 2 || argc > 4) {
            wrong_args(out, "zrandmember");
            return;
        }
        const char *k;
        size_t kl;
        long long count = 0;
        int withscores = 0;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (argc >= 3) {
            const char *cv;
            size_t cvl;
            if (!arg_str(&argv[2], &cv, &cvl))
                goto bad_type;
            if (!parse_i64(cv, cvl, &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
        }
        if (argc == 4) {
            const char *ov;
            size_t ovl;
            if (!arg_str(&argv[3], &ov, &ovl))
                goto bad_type;
            if (!ci_equal(ov, ovl, "WITHSCORES")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            withscores = 1;
        }
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (argc == 2) {
            /* single member form: bulk reply, random index seek */
            obj_zset_iter it;
            if (rc == 0) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            if (obj_zset_seek(
                    z, (size_t)(db_rand(d) % (uint32_t)obj_zset_len(z)),
                    &it)) {
                size_t ml = 0;
                const char *mv = obj_zset_iter_member(&it, &ml);
                resp_write_bulk(out, mv, ml);
            }
            return;
        }
        /* count form: array reply */
        if (rc == 0 || count == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        if (obj_zset_is_listpack(z)) {
            /* small zset: random pair indices, no collection pass */
            size_t n = (size_t)obj_zset_len(z);
            size_t i;
            if (count < 0) {
                /* with repeats */
                size_t total = (size_t)-count;
                resp_write_array_header(out, withscores ? total * 2 : total);
                for (i = 0; i < total; i++) {
                    obj_zset_iter it;
                    uint64_t idx = db_rand(d) % (uint32_t)n;
                    if (!obj_zset_seek(z, (size_t)idx, &it))
                        continue;
                    zset_emit_member(out, &it, withscores);
                }
            } else {
                /* distinct: partial Fisher-Yates over pair indices */
                size_t k2 = (size_t)count < n ? (size_t)count : n;
                uint32_t *idxs = (uint32_t *)malloc(n * sizeof(*idxs));
                if (idxs == NULL) {
                    fprintf(stderr, "ddup: out of memory\n");
                    exit(1);
                }
                for (i = 0; i < n; i++)
                    idxs[i] = (uint32_t)i;
                for (i = 0; i < k2; i++) {
                    size_t j = i + (size_t)(db_rand(d) % (uint32_t)(n - i));
                    uint32_t tmp = idxs[i];
                    idxs[i] = idxs[j];
                    idxs[j] = tmp;
                }
                resp_write_array_header(out, withscores ? k2 * 2 : k2);
                for (i = 0; i < k2; i++) {
                    obj_zset_iter it;
                    if (!obj_zset_seek(z, (size_t)idxs[i], &it))
                        continue;
                    zset_emit_member(out, &it, withscores);
                }
                free(idxs);
            }
            return;
        }
        {
            collect_ctx cc = {0};
            size_t n, i, k2;
            rh_each(&z->dict, collect_cb, &cc);
            n = cc.n;
            if (count < 0) {
                /* with repeats */
                size_t total = (size_t)-count;
                resp_write_array_header(out, withscores ? total * 2 : total);
                for (i = 0; i < total; i++) {
                    size_t idx = (size_t)(db_rand(d) % (uint32_t)n);
                    resp_write_bulk(out, cc.keys[idx], cc.lens[idx]);
                    if (withscores) {
                        double sc;
                        char num[40];
                        int nl;
                        if (obj_zset_score(z, cc.keys[idx], cc.lens[idx],
                                           &sc)) {
                            nl = fmt_score(num, sizeof(num), sc);
                            resp_write_bulk(out, num, (size_t)nl);
                        }
                    }
                }
            } else {
                k2 = (size_t)count < n ? (size_t)count : n;
                collect_shuffle(d, &cc, k2);
                resp_write_array_header(out, withscores ? k2 * 2 : k2);
                for (i = 0; i < k2; i++) {
                    resp_write_bulk(out, cc.keys[i], cc.lens[i]);
                    if (withscores) {
                        double sc;
                        char num[40];
                        int nl;
                        if (obj_zset_score(z, cc.keys[i], cc.lens[i], &sc)) {
                            nl = fmt_score(num, sizeof(num), sc);
                            resp_write_bulk(out, num, (size_t)nl);
                        }
                    }
                }
            }
            free(cc.keys);
            free(cc.lens);
        }
        return;
    }

    if (cmd_id == CMD_ZRANGEBYLEX || cmd_id == CMD_ZREVRANGEBYLEX) {
        int rev = cmd_id == CMD_ZREVRANGEBYLEX;
        if (argc < 4) {
            wrong_args(out, rev ? "zrevrangebylex" : "zrangebylex");
            return;
        }
        const char *k, *minv, *maxv;
        size_t kl, minvl, maxvl;
        zlexrangespec spec;
        long long off = 0, cnt = -1;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        /* ZREVRANGEBYLEX takes max before min */
        if (!arg_str(&argv[rev ? 3 : 2], &minv, &minvl) ||
            !arg_str(&argv[rev ? 2 : 3], &maxv, &maxvl))
            goto bad_type;
        if (!parse_lex_bound(minv, minvl, &spec.min) ||
            !parse_lex_bound(maxv, maxvl, &spec.max)) {
            static const char lex_err[] =
                "ERR min or max is not a valid string range item";
            resp_write_error(out, lex_err, sizeof(lex_err) - 1);
            return;
        }
        for (size_t i = 4; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "LIMIT") && i + 2 < argc) {
                const char *ov, *cv;
                size_t ovl, cvl;
                if (!arg_str(&argv[i + 1], &ov, &ovl) ||
                    !arg_str(&argv[i + 2], &cv, &cvl))
                    goto bad_type;
                if (!parse_i64(ov, ovl, &off) ||
                    !parse_i64(cv, cvl, &cnt)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                i += 2;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (off < 0)
            off = 0;
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_array_header(out, 0);
            return;
        }
        {
            /* walk the [first, last] span; two passes for the header */
            obj_zset_iter first, last, it;
            long long emitted = 0;
            long long c;
            if (!obj_zset_first_in_lex_range(z, &spec, &first) ||
                !obj_zset_last_in_lex_range(z, &spec, &last)) {
                resp_write_array_header(out, 0);
                return;
            }
            c = 0;
            it = first;
            for (;;) {
                if (c >= off)
                    emitted++;
                c++;
                if (obj_zset_iter_eq(&it, &last))
                    break;
                obj_zset_iter_next(&it);
            }
            if (cnt >= 0 && emitted > cnt)
                emitted = cnt;
            resp_write_array_header(out, (size_t)emitted);
            c = 0;
            if (!rev) {
                it = first;
                for (;;) {
                    if (c >= off && (cnt < 0 || c - off < cnt))
                        zset_emit_member(out, &it, 0);
                    c++;
                    if (obj_zset_iter_eq(&it, &last))
                        break;
                    obj_zset_iter_next(&it);
                }
            } else {
                it = last;
                for (;;) {
                    if (c >= off && (cnt < 0 || c - off < cnt))
                        zset_emit_member(out, &it, 0);
                    c++;
                    if (obj_zset_iter_eq(&it, &first))
                        break;
                    obj_zset_iter_prev(&it);
                }
            }
        }
        return;
    }

    if (cmd_id == CMD_ZREMRANGEBYLEX) {
        if (argc != 4) {
            wrong_args(out, "zremrangebylex");
            return;
        }
        const char *k, *minv, *maxv;
        size_t kl, minvl, maxvl;
        zlexrangespec spec;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &minv, &minvl) ||
            !arg_str(&argv[3], &maxv, &maxvl))
            goto bad_type;
        if (!parse_lex_bound(minv, minvl, &spec.min) ||
            !parse_lex_bound(maxv, maxvl, &spec.max)) {
            static const char lex_err[] =
                "ERR min or max is not a valid string range item";
            resp_write_error(out, lex_err, sizeof(lex_err) - 1);
            return;
        }
        obj_zset *z;
        int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
        if (rc < 0)
            return;
        if (rc == 0) {
            resp_write_integer(out, 0);
            return;
        }
        {
            long long removed;
            uint64_t before = obj_zset_mem(z);
            removed = (long long)obj_zset_rem_range_by_lex(z, &spec);
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            if (obj_zset_len(z) == 0)
                db_del_kv(d, k, kl);
            resp_write_integer(out, removed);
        }
        return;
    }

    if (cmd_id == CMD_ZUNIONSTORE || cmd_id == CMD_ZINTERSTORE) {
        int inter = cmd_id == CMD_ZINTERSTORE;
        const char *dst;
        size_t dstl;
        zsetop_args args;
        obj_zset *result;
        if (argc < 4) {
            wrong_args(out, inter ? "zinterstore" : "zunionstore");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (!arg_str(&argv[1], &dst, &dstl))
            goto bad_type;
        if (!storage_key_ok(dstl)) {
            storage_length_error(out);
            return;
        }
        if (zsetop_parse(d, out, argv, argc, 2, 1, 1, 0, 0, NULL, now_ms,
                         &args) != 0)
            return;
        result = obj_zset_new();
        if ((inter ? zset_inter_build(args.sets, args.nkeys, args.weights,
                                      args.aggregate, result)
                   : zset_union_build(args.sets, args.nkeys, args.weights,
                                      args.aggregate, result)) != 0) {
            zsetop_args_free(&args);
            obj_zset_free(result);
            resp_write_error(out, "ERR resulting score is not a number (NaN)",
                             41);
            return;
        }
        zsetop_args_free(&args);
        (void)zset_store_result(d, out, dst, dstl, result, now_ms);
        return;
    }

    if (cmd_id == CMD_ZDIFFSTORE) {
        const char *dst;
        size_t dstl;
        zsetop_args args;
        obj_zset *result;
        if (argc < 4) {
            wrong_args(out, "zdiffstore");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (!arg_str(&argv[1], &dst, &dstl))
            goto bad_type;
        if (!storage_key_ok(dstl)) {
            storage_length_error(out);
            return;
        }
        if (zsetop_parse(d, out, argv, argc, 2, 0, 0, 0, 0, NULL, now_ms,
                         &args) != 0)
            return;
        result = obj_zset_new();
        if (zset_diff_build(args.sets, args.nkeys, result) != 0) {
            zsetop_args_free(&args);
            obj_zset_free(result);
            resp_write_error(out, "ERR resulting score is not a number (NaN)",
                             41);
            return;
        }
        zsetop_args_free(&args);
        (void)zset_store_result(d, out, dst, dstl, result, now_ms);
        return;
    }

    if (cmd_id == CMD_ZUNION || cmd_id == CMD_ZINTER ||
        cmd_id == CMD_ZDIFF) {
        int inter = cmd_id == CMD_ZINTER;
        int diff = cmd_id == CMD_ZDIFF;
        zsetop_args args;
        obj_zset *result;
        if (argc < 3) {
            wrong_args(out, inter ? "zinter" : diff ? "zdiff" : "zunion");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (zsetop_parse(d, out, argv, argc, 1, !diff, !diff, 1, 0, NULL,
                         now_ms, &args) != 0)
            return;
        result = obj_zset_new();
        if ((diff ? zset_diff_build(args.sets, args.nkeys, result)
                  : inter ? zset_inter_build(args.sets, args.nkeys,
                                             args.weights, args.aggregate,
                                             result)
                          : zset_union_build(args.sets, args.nkeys,
                                             args.weights, args.aggregate,
                                             result)) != 0) {
            zsetop_args_free(&args);
            obj_zset_free(result);
            resp_write_error(out, "ERR resulting score is not a number (NaN)",
                             41);
            return;
        }
        zset_emit_all(out, result, args.withscores);
        obj_zset_free(result);
        zsetop_args_free(&args);
        return;
    }

    if (cmd_id == CMD_ZINTERCARD) {
        zsetop_args args;
        long long limit = 0;
        long long card;
        if (argc < 3) {
            wrong_args(out, "zintercard");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (zsetop_parse(d, out, argv, argc, 1, 0, 0, 0, 1, &limit, now_ms,
                         &args) != 0)
            return;
        card = zset_intercard(args.sets, args.nkeys, limit);
        zsetop_args_free(&args);
        resp_write_integer(out, card);
        return;
    }

    if (cmd_id == CMD_ZLEXCOUNT) {
        const char *k, *minv, *maxv;
        size_t kl, minvl, maxvl;
        zlexrangespec spec;
        if (argc != 4) {
            wrong_args(out, "zlexcount");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &minv, &minvl) ||
            !arg_str(&argv[3], &maxv, &maxvl))
            goto bad_type;
        if (!parse_lex_bound(minv, minvl, &spec.min) ||
            !parse_lex_bound(maxv, maxvl, &spec.max)) {
            static const char lex_err[] =
                "ERR min or max is not a valid string range item";
            resp_write_error(out, lex_err, sizeof(lex_err) - 1);
            return;
        }
        {
            obj_zset *z;
            int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
            obj_zset_iter first, last, it;
            long long count = 0;
            if (rc < 0)
                return;
            if (rc == 0 ||
                !obj_zset_first_in_lex_range(z, &spec, &first) ||
                !obj_zset_last_in_lex_range(z, &spec, &last)) {
                resp_write_integer(out, 0);
                return;
            }
            it = first;
            for (;;) {
                count++;
                if (obj_zset_iter_eq(&it, &last))
                    break;
                if (!obj_zset_iter_next(&it))
                    break;
            }
            resp_write_integer(out, count);
        }
        return;
    }

    if (cmd_id == CMD_ZREVRANGEBYSCORE) {
        const char *k, *maxv, *minv;
        size_t kl, maxvl, minvl;
        zrangespec spec;
        int withscores = 0;
        long long off = 0, cnt = -1;
        if (argc < 4) {
            wrong_args(out, "zrevrangebyscore");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &maxv, &maxvl) ||
            !arg_str(&argv[3], &minv, &minvl))
            goto bad_type;
        if (!parse_bound(minv, minvl, &spec.min, &spec.minex) ||
            !parse_bound(maxv, maxvl, &spec.max, &spec.maxex)) {
            resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
            return;
        }
        for (size_t i = 4; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "WITHSCORES") && !withscores) {
                withscores = 1;
            } else if (ci_equal(o, ol, "LIMIT") && i + 2 < argc) {
                const char *ov, *cv;
                size_t ovl, cvl;
                if (!arg_str(&argv[i + 1], &ov, &ovl) ||
                    !arg_str(&argv[i + 2], &cv, &cvl))
                    goto bad_type;
                if (!parse_i64(ov, ovl, &off) ||
                    !parse_i64(cv, cvl, &cnt)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                i += 2;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (off < 0)
            off = 0;
        {
            obj_zset *z;
            int rc = get_zset(d, out, k, kl, 0, now_ms, &z);
            if (rc < 0)
                return;
            if (rc == 0) {
                resp_write_array_header(out, 0);
                return;
            }
            {
                obj_zset_iter it;
                long long emitted = 0;
                long long c = 0;
                if (obj_zset_last_in_range(z, &spec, &it)) {
                    for (;;) {
                        double sc = obj_zset_iter_score(&it);
                        if (spec.minex ? !(sc > spec.min)
                                       : !(sc >= spec.min))
                            break;
                        if (c >= off)
                            emitted++;
                        c++;
                        if (!obj_zset_iter_prev(&it))
                            break;
                    }
                }
                if (cnt >= 0 && emitted > cnt)
                    emitted = cnt;
                resp_write_array_header(
                    out, (size_t)emitted * (withscores ? 2u : 1u));
                c = 0;
                if (obj_zset_last_in_range(z, &spec, &it)) {
                    for (;;) {
                        double sc = obj_zset_iter_score(&it);
                        if (spec.minex ? !(sc > spec.min)
                                       : !(sc >= spec.min))
                            break;
                        if (c >= off && (cnt < 0 || c - off < cnt))
                            zset_emit_member(out, &it, withscores);
                        c++;
                        if (!obj_zset_iter_prev(&it))
                            break;
                    }
                }
            }
        }
        return;
    }

    if (cmd_id == CMD_ZRANGESTORE) {
        const char *dst, *src;
        size_t dstl, srcl;
        int byscore = 0, bylex = 0, rev = 0;
        long long off = 0, cnt = -1;
        if (argc < 5) {
            wrong_args(out, "zrangestore");
            return;
        }
        if (!arg_str(&argv[1], &dst, &dstl) || !arg_str(&argv[2], &src, &srcl))
            goto bad_type;
        if (!storage_key_ok(dstl)) {
            storage_length_error(out);
            return;
        }
        for (size_t i = 5; i < argc; i++) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[i], &o, &ol))
                goto bad_type;
            if (ci_equal(o, ol, "BYSCORE") && !bylex) {
                byscore = 1;
            } else if (ci_equal(o, ol, "BYLEX") && !byscore) {
                bylex = 1;
            } else if (ci_equal(o, ol, "REV")) {
                rev = 1;
            } else if (ci_equal(o, ol, "LIMIT") && i + 2 < argc) {
                const char *ov, *cv;
                size_t ovl, cvl;
                if (!arg_str(&argv[i + 1], &ov, &ovl) ||
                    !arg_str(&argv[i + 2], &cv, &cvl))
                    goto bad_type;
                if (!parse_i64(ov, ovl, &off) ||
                    !parse_i64(cv, cvl, &cnt)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                i += 2;
            } else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (off < 0)
            off = 0;
        {
            obj_zset *z;
            int rc = get_zset(d, out, src, srcl, 0, now_ms, &z);
            obj_zset *result;
            if (rc < 0)
                return;
            result = obj_zset_new();
            if (rc == 1) {
                long long c = 0;
                if (byscore) {
                    zrangespec spec;
                    obj_zset_iter it;
                    const char *minv, *maxv;
                    size_t minvl, maxvl;
                    if (!arg_str(&argv[3], &minv, &minvl) ||
                        !arg_str(&argv[4], &maxv, &maxvl)) {
                        obj_zset_free(result);
                        resp_write_error(out, "ERR invalid argument type", 24);
                        return;
                    }
                    if (!parse_bound(minv, minvl, &spec.min, &spec.minex) ||
                        !parse_bound(maxv, maxvl, &spec.max, &spec.maxex)) {
                        obj_zset_free(result);
                        resp_write_error(out, ERR_NOT_FLOAT,
                                         sizeof(ERR_NOT_FLOAT) - 1);
                        return;
                    }
                    if (rev) {
                        if (obj_zset_last_in_range(z, &spec, &it)) {
                            for (;;) {
                                double sc = obj_zset_iter_score(&it);
                                size_t ml = 0;
                                const char *mv;
                                if (spec.minex ? !(sc > spec.min)
                                               : !(sc >= spec.min))
                                    break;
                                if (c >= off &&
                                    (cnt < 0 || c - off < cnt)) {
                                    mv = obj_zset_iter_member(&it, &ml);
                                    if (obj_zset_add(result, mv, ml, sc) < 0)
                                        break;
                                }
                                c++;
                                if (!obj_zset_iter_prev(&it))
                                    break;
                            }
                        }
                    } else {
                        if (obj_zset_first_in_range(z, &spec, &it)) {
                            for (;;) {
                                double sc = obj_zset_iter_score(&it);
                                size_t ml = 0;
                                const char *mv;
                                if (spec.maxex ? !(sc < spec.max)
                                               : !(sc <= spec.max))
                                    break;
                                if (c >= off &&
                                    (cnt < 0 || c - off < cnt)) {
                                    mv = obj_zset_iter_member(&it, &ml);
                                    if (obj_zset_add(result, mv, ml, sc) < 0)
                                        break;
                                }
                                c++;
                                if (!obj_zset_iter_next(&it))
                                    break;
                            }
                        }
                    }
                } else if (bylex) {
                    zlexrangespec spec;
                    obj_zset_iter first, last, cur;
                    const char *minv, *maxv;
                    size_t minvl, maxvl;
                    if (!arg_str(&argv[3], &minv, &minvl) ||
                        !arg_str(&argv[4], &maxv, &maxvl)) {
                        obj_zset_free(result);
                        resp_write_error(out, "ERR invalid argument type", 24);
                        return;
                    }
                    if (!parse_lex_bound(minv, minvl, &spec.min) ||
                        !parse_lex_bound(maxv, maxvl, &spec.max)) {
                        obj_zset_free(result);
                        static const char lex_err[] =
                            "ERR min or max is not a valid string range item";
                        resp_write_error(out, lex_err, sizeof(lex_err) - 1);
                        return;
                    }
                    if (rev) {
                        if (obj_zset_first_in_lex_range(z, &spec, &first) &&
                            obj_zset_last_in_lex_range(z, &spec, &last)) {
                            cur = last;
                            for (;;) {
                                size_t ml = 0;
                                const char *mv = obj_zset_iter_member(&cur, &ml);
                                double sc = obj_zset_iter_score(&cur);
                                if (c >= off &&
                                    (cnt < 0 || c - off < cnt)) {
                                    if (obj_zset_add(result, mv, ml, sc) < 0)
                                        break;
                                }
                                c++;
                                if (obj_zset_iter_eq(&cur, &first))
                                    break;
                                if (!obj_zset_iter_prev(&cur))
                                    break;
                            }
                        }
                    } else {
                        if (obj_zset_first_in_lex_range(z, &spec, &first) &&
                            obj_zset_last_in_lex_range(z, &spec, &last)) {
                            cur = first;
                            for (;;) {
                                size_t ml = 0;
                                const char *mv = obj_zset_iter_member(&cur, &ml);
                                double sc = obj_zset_iter_score(&cur);
                                if (c >= off &&
                                    (cnt < 0 || c - off < cnt)) {
                                    if (obj_zset_add(result, mv, ml, sc) < 0)
                                        break;
                                }
                                c++;
                                if (obj_zset_iter_eq(&cur, &last))
                                    break;
                                if (!obj_zset_iter_next(&cur))
                                    break;
                            }
                        }
                    }
                } else {
                    long long len = (long long)obj_zset_len(z);
                    long long start, stop;
                    const char *sv, *ev;
                    size_t svl, evl;
                    if (!arg_str(&argv[3], &sv, &svl) ||
                        !arg_str(&argv[4], &ev, &evl)) {
                        obj_zset_free(result);
                        resp_write_error(out, "ERR invalid argument type", 24);
                        return;
                    }
                    if (!parse_i64(sv, svl, &start) ||
                        !parse_i64(ev, evl, &stop)) {
                        obj_zset_free(result);
                        resp_write_error(out, ERR_NOT_INT,
                                         sizeof(ERR_NOT_INT) - 1);
                        return;
                    }
                    if (start < 0)
                        start += len;
                    if (start < 0)
                        start = 0;
                    if (stop < 0)
                        stop += len;
                    if (stop >= len)
                        stop = len - 1;
                    if (start <= stop && start < len && stop >= 0) {
                        obj_zset_iter it;
                        long long idx;
                        if (!rev) {
                            if (obj_zset_seek(z, (size_t)start, &it)) {
                                for (idx = start; idx <= stop; idx++) {
                                    size_t ml = 0;
                                    const char *mv = obj_zset_iter_member(&it, &ml);
                                    double sc = obj_zset_iter_score(&it);
                                    if (c >= off &&
                                        (cnt < 0 || c - off < cnt)) {
                                        if (obj_zset_add(result, mv, ml, sc) < 0)
                                            break;
                                    }
                                    c++;
                                    if (idx != stop && !obj_zset_iter_next(&it))
                                        break;
                                }
                            }
                        } else {
                            if (obj_zset_seek(z, (size_t)(len - 1 - start), &it)) {
                                for (idx = start; idx <= stop; idx++) {
                                    size_t ml = 0;
                                    const char *mv = obj_zset_iter_member(&it, &ml);
                                    double sc = obj_zset_iter_score(&it);
                                    if (c >= off &&
                                        (cnt < 0 || c - off < cnt)) {
                                        if (obj_zset_add(result, mv, ml, sc) < 0)
                                            break;
                                    }
                                    c++;
                                    if (idx != stop && !obj_zset_iter_prev(&it))
                                        break;
                                }
                            }
                        }
                    }
                }
            }
            (void)zset_store_result(d, out, dst, dstl, result, now_ms);
        }
        return;
    }

    if (cmd_id == CMD_ZMPOP) {
        const char *nv;
        size_t nvl;
        long long nk;
        size_t nkeys, i;
        int min_side;
        long long count = 1;
        if (argc < 3) {
            wrong_args(out, "zmpop");
            return;
        }
        if (crossslot_reject(d, out, argv, argc))
            return;
        if (!arg_str(&argv[1], &nv, &nvl))
            goto bad_type;
        if (!parse_i64(nv, nvl, &nk)) {
            resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
            return;
        }
        if (nk <= 0 ||
            (unsigned long long)nk > (unsigned long long)(argc - 2)) {
            static const char E[] =
                "ERR Number of keys can't be greater than number of args";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        nkeys = (size_t)nk;
        i = 2 + nkeys;
        if (i >= argc)
            goto bad_type;
        {
            const char *side;
            size_t sidel;
            if (!arg_str(&argv[i], &side, &sidel))
                goto bad_type;
            if (ci_equal(side, sidel, "MIN"))
                min_side = 1;
            else if (ci_equal(side, sidel, "MAX"))
                min_side = 0;
            else {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        if (i + 1 < argc) {
            const char *opt, *cv;
            size_t optl, cvl;
            if (i + 2 >= argc) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            if (!arg_str(&argv[i + 1], &opt, &optl) ||
                !arg_str(&argv[i + 2], &cv, &cvl)) {
                resp_write_error(out, "ERR invalid argument type", 24);
                return;
            }
            if (!ci_equal(opt, optl, "COUNT")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            if (!parse_i64(cv, cvl, &count)) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (count <= 0) {
                static const char E[] =
                    "ERR value is out of range, must be positive";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            if (i + 3 < argc) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
        }
        for (i = 0; i < nkeys; i++) {
            const char *k;
            size_t kl;
            obj_zset *z = NULL;
            int rc;
            if (!arg_str(&argv[2 + i], &k, &kl))
                goto bad_type;
            rc = get_zset(d, out, k, kl, 0, now_ms, &z);
            if (rc < 0)
                return;
            if (rc == 0 || obj_zset_len(z) == 0)
                continue;
            {
                size_t avail = obj_zset_len(z);
                size_t n = (uint64_t)count < (uint64_t)avail ? (size_t)count
                                                             : avail;
                uint64_t before = obj_zset_mem(z);
                size_t p;
                resp_write_array_header(out, 2);
                resp_write_bulk(out, k, kl);
                resp_write_array_header(out, n);
                for (p = 0; p < n; p++) {
                    char *mv = NULL;
                    size_t ml = 0;
                    double sc = 0.0;
                    char num[40];
                    int nl;
                    if (!obj_zset_pop(z, min_side, &mv, &ml, &sc))
                        break;
                    resp_write_array_header(out, 2);
                    resp_write_bulk(out, mv, ml);
                    nl = fmt_score(num, sizeof(num), sc);
                    resp_write_bulk(out, num, (size_t)nl);
                    free(mv);
                }
                mem_sync(d, k, kl, before, obj_zset_mem(z));
                if (obj_zset_len(z) == 0)
                    db_del_kv(d, k, kl);
            }
            return;
        }
        write_null_array(out);
        return;
    }

    if (cmd_id == CMD_SUBSCRIBE) {
        size_t i;
        if (argc < 2) {
            wrong_args(out, "subscribe");
            return;
        }
        for (i = 1; i < argc; i++) {
            const char *ch;
            size_t cl;
            if (!arg_str(&argv[i], &ch, &cl))
                goto bad_type;
        }
        {
            size_t old_nsub = s->nsub;
            size_t staged = 0;
            ptrdiff_t counts[argc - 1];
            unsigned char added[argc - 1];
            for (i = 1; i < argc; i++) {
                const char *ch;
                size_t cl;
                if (!arg_str(&argv[i], &ch, &cl)) goto bad_type;
                ptrdiff_t before = (ptrdiff_t)s->nsub;
                counts[i - 1] = s->subscribe != NULL
                                    ? s->subscribe(s->ps_ctx, s, ch, cl)
                                    : (ptrdiff_t)(++s->nsub);
                added[i - 1] = (unsigned char)(counts[i - 1] > before);
                if (counts[i - 1] < 0) {
                    while (staged-- > 0) {
                        const char *undo = argv[i - staged - 1].str;
                        if (added[i - staged - 1])
                            s->unsubscribe(s->ps_ctx, s, undo, argv[i - staged - 1].len);
                    }
                    s->nsub = old_nsub;
                    storage_length_error(out);
                    return;
                }
                staged++;
            }
            for (i = 1; i < argc; i++) {
                const char *ch = argv[i].str;
                write_sub_reply(out, "subscribe", 9, ch, argv[i].len,
                                (long long)counts[i - 1]);
            }
            /* hook sessions: the hook moved nsub already; rebuild it from
             * the staged adds. hook-less sessions counted in the loop. */
            if (s->subscribe != NULL) {
                s->nsub = old_nsub;
                for (i = 1; i < argc; i++)
                    if (added[i - 1])
                        s->nsub++;
            }
        }
        return;
    }

    if (cmd_id == CMD_UNSUBSCRIBE) {
        if (argc > 1) {
            size_t i;
            for (i = 1; i < argc; i++) {
                const char *ch;
                size_t cl;
                size_t cnt = 0;
                if (!arg_str(&argv[i], &ch, &cl))
                    goto bad_type;
                if (s->unsubscribe != NULL)
                    cnt = s->unsubscribe(s->ps_ctx, s, ch, cl);
                else if (s->nsub > 0)
                    cnt = --s->nsub;
                write_sub_reply(out, "unsubscribe", 11, ch, cl,
                                (long long)cnt);
            }
            return;
        }
        /* no args: unsubscribe everything, one push per channel */
        if (s->nsub == 0 || s->each_channel == NULL) {
            s->nsub = 0; /* registry-less session: just clear */
            write_sub_reply(out, "unsubscribe", 11, NULL, 0, 0);
            return;
        }
        {
            unsub_ctx u = {0};
            size_t i;
            s->each_channel(s->ps_ctx, s, unsub_collect_cb, &u);
            for (i = 0; i < u.n; i++) {
                size_t cnt = s->unsubscribe(s->ps_ctx, s, u.names[i],
                                            u.lens[i]);
                write_sub_reply(out, "unsubscribe", 11, u.names[i], u.lens[i],
                                (long long)cnt);
                free(u.names[i]);
            }
            free(u.names);
            free(u.lens);
        }
        return;
    }

    if (cmd_id == CMD_PSUBSCRIBE) {
        size_t i;
        if (argc < 2) {
            wrong_args(out, "psubscribe");
            return;
        }
        for (i = 1; i < argc; i++) {
            const char *ch;
            size_t cl;
            if (!arg_str(&argv[i], &ch, &cl))
                goto bad_type;
        }
        {
            size_t old_npsub = s->npsub;
            size_t staged = 0;
            ptrdiff_t counts[argc - 1];
            unsigned char added[argc - 1];
            for (i = 1; i < argc; i++) {
                const char *ch;
                size_t cl;
                if (!arg_str(&argv[i], &ch, &cl)) goto bad_type;
                ptrdiff_t before = (ptrdiff_t)s->npsub;
                counts[i - 1] = s->psubscribe != NULL
                                    ? s->psubscribe(s->ps_ctx, s, ch, cl)
                                    : (ptrdiff_t)(++s->npsub);
                added[i - 1] = (unsigned char)(counts[i - 1] > before);
                if (counts[i - 1] < 0) {
                    while (staged-- > 0) {
                        const char *undo = argv[i - staged - 1].str;
                        if (added[i - staged - 1])
                            s->punsubscribe(s->ps_ctx, s, undo,
                                            argv[i - staged - 1].len);
                    }
                    s->npsub = old_npsub;
                    storage_length_error(out);
                    return;
                }
                staged++;
            }
            /* the reported count spans channels + shard channels + patterns */
            for (i = 1; i < argc; i++) {
                const char *ch = argv[i].str;
                write_sub_reply(out, "psubscribe", 10, ch, argv[i].len,
                                (long long)(s->nsub + s->nssub) +
                                    (long long)counts[i - 1]);
            }
            /* hook sessions: the hook moved npsub already; rebuild it from
             * the staged adds. hook-less sessions counted in the loop. */
            if (s->psubscribe != NULL) {
                s->npsub = old_npsub;
                for (i = 1; i < argc; i++)
                    if (added[i - 1])
                        s->npsub++;
            }
        }
        return;
    }

    if (cmd_id == CMD_PUNSUBSCRIBE) {
        if (argc > 1) {
            size_t i;
            for (i = 1; i < argc; i++) {
                const char *ch;
                size_t cl;
                size_t cnt = 0;
                if (!arg_str(&argv[i], &ch, &cl))
                    goto bad_type;
                if (s->punsubscribe != NULL)
                    cnt = s->punsubscribe(s->ps_ctx, s, ch, cl);
                else if (s->npsub > 0)
                    cnt = --s->npsub;
                write_sub_reply(out, "punsubscribe", 12, ch, cl,
                                (long long)(s->nsub + s->nssub + cnt));
            }
            return;
        }
        /* no args: unsubscribe every pattern, one push per pattern */
        if (s->npsub == 0 || s->each_pattern == NULL) {
            size_t rest = s->nsub + s->nssub;
            s->npsub = 0; /* registry-less session: just clear */
            write_sub_reply(out, "punsubscribe", 12, NULL, 0,
                            (long long)rest);
            return;
        }
        {
            unsub_ctx u = {0};
            size_t i;
            s->each_pattern(s->ps_ctx, s, unsub_collect_cb, &u);
            for (i = 0; i < u.n; i++) {
                size_t cnt = s->punsubscribe(s->ps_ctx, s, u.names[i],
                                             u.lens[i]);
                write_sub_reply(out, "punsubscribe", 12, u.names[i], u.lens[i],
                                (long long)(s->nsub + s->nssub + cnt));
                free(u.names[i]);
            }
            free(u.names);
            free(u.lens);
        }
        return;
    }

    if (cmd_id == CMD_PUBLISH) {
        if (argc != 3) {
            wrong_args(out, "publish");
            return;
        }
        const char *ch, *msg;
        size_t cl, ml;
        if (!arg_str(&argv[1], &ch, &cl) || !arg_str(&argv[2], &msg, &ml))
            goto bad_type;
        resp_write_integer(out, s->publish != NULL
                                  ? s->publish(s->ps_ctx, ch, cl, msg, ml)
                                  : 0);
        return;
    }

    if (cmd_id == CMD_SSUBSCRIBE) {
        size_t i;
        if (argc < 2) {
            wrong_args(out, "ssubscribe");
            return;
        }
        for (i = 1; i < argc; i++) {
            const char *ch;
            size_t cl;
            if (!arg_str(&argv[i], &ch, &cl))
                goto bad_type;
        }
        {
            size_t old_nssub = s->nssub;
            size_t staged = 0;
            ptrdiff_t counts[argc - 1];
            unsigned char added[argc - 1];
            for (i = 1; i < argc; i++) {
                const char *ch;
                size_t cl;
                if (!arg_str(&argv[i], &ch, &cl)) goto bad_type;
                ptrdiff_t before = (ptrdiff_t)s->nssub;
                counts[i - 1] = s->ssubscribe != NULL
                                    ? s->ssubscribe(s->ps_ctx, s, ch, cl)
                                    : (ptrdiff_t)(++s->nssub);
                added[i - 1] = (unsigned char)(counts[i - 1] > before);
                if (counts[i - 1] < 0) {
                    while (staged-- > 0)
                        if (added[i - staged - 1])
                            s->sunsubscribe(s->ps_ctx, s, argv[i - staged - 1].str,
                                            argv[i - staged - 1].len);
                    s->nssub = old_nssub;
                    storage_length_error(out);
                    return;
                }
                staged++;
            }
            for (i = 1; i < argc; i++) {
                write_sub_reply(out, "ssubscribe", 10, argv[i].str,
                                argv[i].len, (long long)counts[i - 1]);
            }
            if (s->ssubscribe != NULL) {
                s->nssub = old_nssub;
                for (i = 1; i < argc; i++)
                    if (added[i - 1])
                        s->nssub++;
            }
        }
        return;
    }

    if (cmd_id == CMD_SUNSUBSCRIBE) {
        if (argc > 1) {
            size_t i;
            for (i = 1; i < argc; i++) {
                const char *ch;
                size_t cl;
                size_t cnt = 0;
                if (!arg_str(&argv[i], &ch, &cl))
                    goto bad_type;
                if (s->sunsubscribe != NULL)
                    cnt = s->sunsubscribe(s->ps_ctx, s, ch, cl);
                else if (s->nssub > 0)
                    cnt = --s->nssub;
                write_sub_reply(out, "sunsubscribe", 12, ch, cl,
                                (long long)cnt);
            }
            return;
        }
        /* no args: unsubscribe every shard channel */
        if (s->nssub == 0 || s->each_schannel == NULL) {
            s->nssub = 0;
            write_sub_reply(out, "sunsubscribe", 12, NULL, 0, 0);
            return;
        }
        {
            unsub_ctx u = {0};
            size_t i;
            s->each_schannel(s->ps_ctx, s, unsub_collect_cb, &u);
            for (i = 0; i < u.n; i++) {
                size_t cnt = s->sunsubscribe(s->ps_ctx, s, u.names[i],
                                             u.lens[i]);
                write_sub_reply(out, "sunsubscribe", 12, u.names[i],
                                u.lens[i], (long long)cnt);
                free(u.names[i]);
            }
            free(u.names);
            free(u.lens);
        }
        return;
    }

    if (cmd_id == CMD_SPUBLISH) {
        if (argc != 3) {
            wrong_args(out, "spublish");
            return;
        }
        const char *ch, *msg;
        size_t cl, ml;
        if (!arg_str(&argv[1], &ch, &cl) || !arg_str(&argv[2], &msg, &ml))
            goto bad_type;
        {
            long n = s->spublish != NULL
                         ? s->spublish(s->ps_ctx, ch, cl, msg, ml)
                         : 0;
            /* receivers count is local-only (Redis semantics); the bus
             * propagation is asynchronous and not counted */
            if (d->cluster_enabled && s->spublish_bus != NULL)
                s->spublish_bus(s->ps_ctx, ch, cl, msg, ml);
            resp_write_integer(out, n);
        }
        return;
    }

    if (cmd_id == CMD_PUBSUB) {
        const char *sub;
        size_t sl;
        if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
            goto bad_type;
        if (ci_equal(sub, sl, "HELP") && argc == 2) {
            static const char *help[] = {
                "CHANNELS [pattern]",
                "NUMSUB [channel ...]",
                "NUMPAT",
                "SHARDCHANNELS [pattern]",
                "SHARDNUMSUB [shardchannel ...]"
            };
            size_t i;
            resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
            for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
                resp_write_bulk(out, help[i], strlen(help[i]));
            return;
        }
        if (ci_equal(sub, sl, "CHANNELS") && (argc == 2 || argc == 3)) {
            const char *pat = NULL;
            size_t pl = 0;
            if (argc == 3 && !arg_str(&argv[2], &pat, &pl))
                goto bad_type;
            if (s->pubsub_channels != NULL)
                s->pubsub_channels(s->ps_ctx, pat, pl, out);
            else
                resp_write_array_header(out, 0);
            return;
        }
        if (ci_equal(sub, sl, "NUMSUB") && argc >= 2) {
            size_t i;
            resp_write_array_header(out, (argc - 2) * 2);
            for (i = 2; i < argc; i++) {
                const char *ch;
                size_t cl;
                if (!arg_str(&argv[i], &ch, &cl))
                    goto bad_type;
                resp_write_bulk(out, ch, cl);
                resp_write_integer(out,
                                   s->channel_nsub != NULL
                                       ? s->channel_nsub(s->ps_ctx, ch, cl)
                                       : 0);
            }
            return;
        }
        if (ci_equal(sub, sl, "NUMPAT") && argc == 2) {
            resp_write_integer(out,
                               s->numpat != NULL ? s->numpat(s->ps_ctx) : 0);
            return;
        }
        if (ci_equal(sub, sl, "SHARDNUMSUB") && argc >= 2) {
            size_t i;
            resp_write_array_header(out, (argc - 2) * 2);
            for (i = 2; i < argc; i++) {
                const char *ch;
                size_t cl;
                if (!arg_str(&argv[i], &ch, &cl))
                    goto bad_type;
                resp_write_bulk(out, ch, cl);
                resp_write_integer(out,
                                   s->schannel_nsub != NULL
                                       ? s->schannel_nsub(s->ps_ctx, ch, cl)
                                       : 0);
            }
            return;
        }
        if (ci_equal(sub, sl, "SHARDCHANNELS") && (argc == 2 || argc == 3)) {
            const char *pat = NULL;
            size_t pl = 0;
            if (argc == 3 && !arg_str(&argv[2], &pat, &pl))
                goto bad_type;
            if (s->shard_channels != NULL)
                s->shard_channels(s->ps_ctx, pat, pl, out);
            else
                resp_write_array_header(out, 0);
            return;
        }
        resp_write_error(out, "ERR Unknown PUBSUB subcommand", 27);
        return;
    }

    if (cmd_id == CMD_QUIT) {
        if (argc != 1) {
            wrong_args(out, "quit");
            return;
        }
        /* ack, then ask the server to close the connection once the
         * reply has been flushed (stack sessions only get the +OK) */
        resp_write_simple_string(out, "OK", 2);
        s->quit = 1;
        return;
    }

    if (cmd_id == CMD_COMMAND) {
        command_command(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_CLIENT) {
        command_client(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_MEMORY) {
        command_memory(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_SLOWLOG) {
        command_slowlog(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_BGSAVE) {
        command_bgsave(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_BGREWRITEAOF) {
        command_bgrewriteaof(s, argv, argc, out);
        return;
    }

    if (cmd_id == CMD_XADD) {
        command_xadd(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XLEN) {
        command_xlen(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XRANGE || cmd_id == CMD_XREVRANGE) {
        command_xrange_rev(s, argv, argc, out, now_ms,
                           cmd_id == CMD_XREVRANGE);
        return;
    }

    if (cmd_id == CMD_XDEL) {
        command_xdel(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XDELEX) {
        command_xdelex(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XTRIM) {
        command_xtrim(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XGROUP) {
        command_xgroup(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XACK) {
        command_xack(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XACKDEL) {
        command_xackdel(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XPENDING) {
        command_xpending(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XCLAIM) {
        command_xclaim(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XAUTOCLAIM) {
        command_xautoclaim(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XNACK) {
        command_xnack(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XREAD) {
        command_xread(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XREADGROUP) {
        command_xreadgroup(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XINFO) {
        command_xinfo(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XSETID) {
        command_xsetid(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XCFGSET) {
        command_xcfgset(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_XIDMPRECORD) {
        command_xidmprecord(s, argv, argc, out, now_ms);
        return;
    }

    if (cmd_id == CMD_SAVE) {
        if (argc != 1) {
            wrong_args(out, "save");
            return;
        }
        if (d->snapshot_path == NULL) {
            static const char E[] = "ERR snapshot path not configured";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (s->sel_fn != NULL) {
            /* multi-db build: every logical db goes into one file */
            int i;
            if (snapshot_save_multi(s->sel_ctx,
                                    (snapshot_db_get)s->sel_fn, s->sel_ndbs,
                                    d->snapshot_path) != 0) {
                static const char E[] = "ERR snapshot save failed";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            for (i = 0; i < s->sel_ndbs; i++)
                s->sel_fn(s->sel_ctx, i)->last_save = now_ms / 1000;
        } else {
            if (snapshot_save(d, d->snapshot_path) != 0) {
                static const char E[] = "ERR snapshot save failed";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            d->last_save = now_ms / 1000;
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_LASTSAVE) {
        if (argc != 1) {
            wrong_args(out, "lastsave");
            return;
        }
        resp_write_integer(out, (long long)d->last_save);
        return;
    }

    if (cmd_id == CMD_SHUTDOWN) {
        if (argc != 1) {
            wrong_args(out, "shutdown");
            return;
        }
        if (s->request_shutdown != NULL) {
            /* no reply: the server flushes persistence and goes down */
            s->request_shutdown(s->shutdown_ctx);
            return;
        }
        resp_write_error(out, "ERR shutdown not supported in this context",
                         44);
        return;
    }

    if (cmd_id == CMD_SYNC) {
        if (argc != 1) {
            wrong_args(out, "sync");
            return;
        }
        if (s->sync_hook == NULL) {
            resp_write_error(out, "ERR sync not supported in this context",
                             41);
            return;
        }
        /* the server writes the $<len> snapshot frame itself and marks the
         * connection as a downstream replica */
        s->sync_hook(s->sync_ctx, s);
        return;
    }

    if (cmd_id == CMD_PSYNC) {
        if (argc != 3) {
            wrong_args(out, "psync");
            return;
        }
        const char *rid, *offv;
        size_t rl, ol;
        long long poff;
        if (!arg_str(&argv[1], &rid, &rl) || !arg_str(&argv[2], &offv, &ol))
            goto bad_type;
        {
            char tmp[32];
            char *endp;
            if (ol >= sizeof(tmp))
                goto bad_type;
            memcpy(tmp, offv, ol);
            tmp[ol] = '\0';
            poff = strtoll(tmp, &endp, 10);
            if (endp != tmp + ol) {
                resp_write_error(out,
                                 "ERR value is not an integer or out of "
                                 "range",
                                 43);
                return;
            }
        }
        if (s->psync_hook == NULL) {
            resp_write_error(out, "ERR psync not supported in this context",
                             42);
            return;
        }
        {
            int rc = s->psync_hook(s->psync_ctx, s, rid, rl, poff);
            if (rc == -1)
                resp_write_error(out, "ERR partial resync output failed", 32);
            else if (rc == -2)
                resp_write_error(out, "ERR snapshot serialization failed", 33);
        }
        return;
    }

    if (cmd_id == CMD_REPLICAOF || cmd_id == CMD_SLAVEOF) {
        if (argc != 3) {
            wrong_args(out, cmd_id == CMD_SLAVEOF ? "slaveof" : "replicaof");
            return;
        }
        const char *host, *portv;
        size_t hl, pl;
        if (!arg_str(&argv[1], &host, &hl) || !arg_str(&argv[2], &portv, &pl))
            goto bad_type;
        if (s->replicaof_hook == NULL) {
            resp_write_error(out,
                             "ERR replicaof not supported in this context",
                             sizeof("ERR replicaof not supported in this "
                                    "context") -
                                 1);
            return;
        }
        if (ci_equal(host, hl, "NO") && ci_equal(portv, pl, "ONE")) {
            s->replicaof_hook(s->replicaof_ctx, NULL, 0); /* promote */
            resp_write_simple_string(out, "OK", 2);
            return;
        }
        {
            long long p;
            char hostbuf[64];
            if (!parse_i64(portv, pl, &p) || p <= 0 || p > 65535) {
                resp_write_error(out, ERR_NOT_INT, sizeof(ERR_NOT_INT) - 1);
                return;
            }
            if (hl >= sizeof(hostbuf)) {
                resp_write_error(out, "ERR invalid master host", 23);
                return;
            }
            memcpy(hostbuf, host, hl);
            hostbuf[hl] = '\0';
            if (s->replicaof_hook(s->replicaof_ctx, hostbuf, (uint16_t)p) !=
                0) {
                resp_write_error(out, "ERR could not connect to master", 29);
                return;
            }
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (cmd_id == CMD_UNWATCH) {
        if (argc != 1) {
            wrong_args(out, "unwatch");
            return;
        }
        session_watch_clear(s);
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    /* ---------------- cluster commands (single-node mode) ------------- */

    if (cmd_id == CMD_CLUSTER) {
        const char *sub;
        size_t sl;
        if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
            goto bad_type;
        if (ci_equal(sub, sl, "HELP") && argc == 2) {
            static const char *help[] = {
                "INFO",
                "MYID",
                "NODES",
                "SLOTS",
                "KEYSLOT <key>",
                "COUNTKEYSINSLOT <slot>",
                "ADDSLOTS <slot ...>",
                "DELSLOTS <slot ...>",
                "SETSLOT <slot> MIGRATING <node-id> | "
                    "SETSLOT <slot> IMPORTING <node-id> | "
                    "SETSLOT <slot> STABLE",
                "MEET <ip> <port>",
                "REPLICATE <node-id>",
                "FAILOVER [TAKEOVER]",
                "GETKEYSINSLOT <slot> <count>",
                "ADDSLOTSRANGE <start> <end> [start end ...]",
                "DELSLOTSRANGE <start> <end> [start end ...]",
                "BUMPEPOCH",
                "COUNT-FAILURE-REPORTS <node-id>",
                "FLUSHSLOTS",
                "FORGET <node-id>",
                "LINKS",
                "MYSHARDID",
                "REPLICAS <node-id>",
                "RESET [HARD|SOFT]",
                "SAVECONFIG",
                "SET-CONFIG-EPOCH <epoch>",
                "SHARDS",
                "SLAVES <node-id>"
            };
            size_t i;
            resp_write_array_header(out, sizeof(help) / sizeof(help[0]));
            for (i = 0; i < sizeof(help) / sizeof(help[0]); i++)
                resp_write_bulk(out, help[i], strlen(help[i]));
            return;
        }
        if (!d->cluster_enabled) {
            static const char E[] =
                "ERR This instance has cluster support disabled";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        {
            if (ci_equal(sub, sl, "INFO") && argc == 2) {
                char body[384];
                int covered = 0, fail_slots = 0;
                int i, sl2;
                uint8_t bm[2048];
                const char *state;
                memset(bm, 0, sizeof(bm));
                for (i = 0; i < d->nnodes; i++) {
                    /* only a quorum-confirmed FAIL holder fails the
                     * state; suspicion (PFAIL/disconnected) does not */
                    if (d->nodes[i].flags & CLUSTER_NODE_FAIL) {
                        for (sl2 = 0; sl2 < 16384 && !fail_slots; sl2++)
                            if (cluster_slots_get(d->nodes[i].slots,
                                                  (uint32_t)sl2))
                                fail_slots = 1;
                    }
                    for (sl2 = 0; sl2 < 16384; sl2++)
                        if (cluster_slots_get(d->nodes[i].slots,
                                              (uint32_t)sl2) &&
                            !cluster_slots_get(bm, (uint32_t)sl2)) {
                            cluster_slots_set(bm, (uint32_t)sl2, 1);
                            covered++;
                        }
                }
                state = (covered == 16384 && !fail_slots) ? "ok" : "fail";
                {
                    cluster_node *me = cluster_myself(d);
                    int nb = snprintf(
                        body, sizeof(body),
                        "cluster_enabled:1\r\ncluster_state:%s\r\n"
                        "cluster_slots_assigned:%d\r\ncluster_slots_ok:%d\r\n"
                        "cluster_known_nodes:%d\r\ncluster_size:%d\r\n"
                        "cluster_current_epoch:%llu\r\n"
                        "cluster_my_epoch:%llu\r\n",
                        state, covered, covered, d->nnodes, d->nnodes,
                        (unsigned long long)d->cluster_current_epoch,
                        (unsigned long long)(me != NULL ? me->epoch : 0));
                    resp_write_bulk(out, body, (size_t)nb);
                }
                return;
            }
            if (ci_equal(sub, sl, "MYID") && argc == 2) {
                resp_write_bulk(out, d->node_id, strlen(d->node_id));
                return;
            }
            if (ci_equal(sub, sl, "NODES") && argc == 2) {
                resp_buf lines;
                resp_buf_init(&lines);
                if (cluster_nodes_render(d, &lines) != 0) {
                    resp_buf_free(&lines);
                    resp_write_error(out,
                                     "ERR node table render failed", 27);
                    return;
                }
                resp_write_bulk(out, lines.data, lines.len);
                resp_buf_free(&lines);
                return;
            }
            if (ci_equal(sub, sl, "SLOTS") && argc == 2) {
                resp_write_array_header(out, 1);
                resp_write_array_header(out, 3);
                resp_write_integer(out, 0);
                resp_write_integer(out, 16383);
                resp_write_array_header(out, 3);
                resp_write_bulk(out, d->cluster_ip, strlen(d->cluster_ip));
                resp_write_integer(out, d->cluster_port);
                resp_write_bulk(out, d->node_id, strlen(d->node_id));
                return;
            }
            if (ci_equal(sub, sl, "KEYSLOT") && argc == 3) {
                const char *k;
                size_t kl;
                if (!arg_str(&argv[2], &k, &kl))
                    goto bad_type;
                resp_write_integer(out, (long long)hash_slot(k, kl));
                return;
            }
            if (ci_equal(sub, sl, "COUNTKEYSINSLOT") && argc == 3) {
                const char *sv;
                size_t svl;
                long long slot;
                if (!arg_str(&argv[2], &sv, &svl))
                    goto bad_type;
                if (!parse_i64(sv, svl, &slot)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (slot < 0 || slot >= 16384) {
                    resp_write_error(out, "ERR Invalid slot", 16);
                    return;
                }
                {
                    slot_scan_ctx ctx;
                    ctx.slot = (uint32_t)slot;
                    ctx.limit = -1;
                    ctx.emitted = 0;
                    ctx.out = NULL;
                    rh_each(&d->table, slot_scan_cb, &ctx);
                    resp_write_integer(out, ctx.emitted);
                }
                return;
            }
            if (ci_equal(sub, sl, "ADDSLOTS") && argc >= 3) {
                size_t i;
                int j;
                for (i = 2; i < argc; i++) {
                    const char *sv;
                    size_t svl;
                    long long slot;
                    if (!arg_str(&argv[i], &sv, &svl))
                        goto bad_type;
                    if (!parse_i64(sv, svl, &slot)) {
                        resp_write_error(out, ERR_NOT_INT,
                                         sizeof(ERR_NOT_INT) - 1);
                        return;
                    }
                    if (slot < 0 || slot >= 16384) {
                        resp_write_error(out, "ERR Invalid slot", 16);
                        return;
                    }
                    for (j = 0; j < d->nnodes; j++)
                        if (cluster_slots_get(d->nodes[j].slots,
                                              (uint32_t)slot)) {
                            char msg[64];
                            int n2 = snprintf(msg, sizeof(msg),
                                              "ERR Slot %lld is already busy",
                                              slot);
                            resp_write_error(out, msg, (size_t)n2);
                            return;
                        }
                    cluster_slots_set(cluster_myself(d)->slots, (uint32_t)slot,
                                      1);
                    d->cluster_changes++;
                    d->slot_owner_dirty = 1;
                }
                /* claiming slots bumps our config epoch (Redis rule) */
                cluster_myself(d)->epoch = cluster_next_epoch(d);
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "DELSLOTS") && argc >= 3) {
                size_t i;
                for (i = 2; i < argc; i++) {
                    const char *sv;
                    size_t svl;
                    long long slot;
                    if (!arg_str(&argv[i], &sv, &svl))
                        goto bad_type;
                    if (!parse_i64(sv, svl, &slot)) {
                        resp_write_error(out, ERR_NOT_INT,
                                         sizeof(ERR_NOT_INT) - 1);
                        return;
                    }
                    if (slot < 0 || slot >= 16384) {
                        resp_write_error(out, "ERR Invalid slot", 16);
                        return;
                    }
                    if (!cluster_slots_get(cluster_myself(d)->slots,
                                           (uint32_t)slot)) {
                        char msg[64];
                        int n2 = snprintf(
                            msg, sizeof(msg),
                            "ERR Slot %lld is already unassigned", slot);
                        resp_write_error(out, msg, (size_t)n2);
                        return;
                    }
                    cluster_slots_set(cluster_myself(d)->slots, (uint32_t)slot,
                                      0);
                    d->slot_migrating[slot] = 0xFFFFu;
                    d->slot_importing[slot] = 0xFFFFu;
                    d->cluster_changes++;
                    d->slot_owner_dirty = 1;
                }
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "SETSLOT") && (argc == 5 || argc == 6)) {
                const char *sv, *kw, *ids;
                size_t svl, kwl, idl;
                long long slot;
                char id[41];
                cluster_node *target;
                int j;
                if (!arg_str(&argv[2], &sv, &svl) ||
                    !arg_str(&argv[3], &kw, &kwl))
                    goto bad_type;
                if (!parse_i64(sv, svl, &slot)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (slot < 0 || slot >= 16384) {
                    resp_write_error(out, "ERR Invalid slot", 16);
                    return;
                }
                if (ci_equal(kw, kwl, "MIGRATING") ||
                    ci_equal(kw, kwl, "IMPORTING")) {
                    /* SETSLOT <slot> MIGRATING TO <id> /
                     * SETSLOT <slot> IMPORTING FROM <id> */
                    int migrating = ci_equal(kw, kwl, "MIGRATING");
                    const char *prep;
                    size_t prepl;
                    if (argc != 6 || !arg_str(&argv[4], &prep, &prepl) ||
                        !arg_str(&argv[5], &ids, &idl))
                        goto bad_type;
                    if (!ci_equal(prep, prepl, migrating ? "TO" : "FROM")) {
                        resp_write_error(out, ERR_SYNTAX,
                                         sizeof(ERR_SYNTAX) - 1);
                        return;
                    }
                    if (idl != 40) {
                        resp_write_error(out, "ERR Unknown node ", 17);
                        return;
                    }
                    memcpy(id, ids, 40);
                    id[40] = '\0';
                    target = cluster_node_find(d, id);
                    if (target == NULL) {
                        char msg[96];
                        int n2 = snprintf(msg, sizeof(msg),
                                          "ERR Unknown node %s", id);
                        resp_write_error(out, msg, (size_t)n2);
                        return;
                    }
                    if (target->flags & CLUSTER_NODE_MYSELF) {
                        static const char EM[] =
                            "ERR Can't migrate slot to myself";
                        static const char EI[] =
                            "ERR Can't import slot from myself";
                        const char *e = migrating ? EM : EI;
                        size_t el = migrating ? sizeof(EM) - 1
                                              : sizeof(EI) - 1;
                        resp_write_error(out, e, el);
                        return;
                    }
                    if (migrating) {
                        if (d->slot_owner_dirty)
                            db_rebuild_slot_owner(d);
                        if (d->slot_owner[slot] == 0xFFFFu ||
                            !(d->nodes[d->slot_owner[slot]].flags &
                              CLUSTER_NODE_MYSELF)) {
                            static const char E[] =
                                "ERR Can't migrate slot: hash slot is not "
                                "served by this node";
                            resp_write_error(out, E, sizeof(E) - 1);
                            return;
                        }
                        d->slot_migrating[slot] =
                            (uint16_t)(target - d->nodes);
                    } else {
                        if (d->slot_owner_dirty)
                            db_rebuild_slot_owner(d);
                        if (d->slot_owner[slot] != 0xFFFFu &&
                            (d->nodes[d->slot_owner[slot]].flags &
                             CLUSTER_NODE_MYSELF)) {
                            static const char E[] =
                                "ERR Can't import slot: hash slot is "
                                "already served by this node";
                            resp_write_error(out, E, sizeof(E) - 1);
                            return;
                        }
                        d->slot_importing[slot] =
                            (uint16_t)(target - d->nodes);
                    }
                    /* local-only state: not gossiped, no cluster_changes */
                    resp_write_simple_string(out, "OK", 2);
                    return;
                }
                /* SETSLOT <slot> NODE <id> */
                if (!ci_equal(kw, kwl, "NODE") || argc != 5) {
                    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                    return;
                }
                if (!arg_str(&argv[4], &ids, &idl))
                    goto bad_type;
                if (idl != 40) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                memcpy(id, ids, 40);
                id[40] = '\0';
                target = cluster_node_find(d, id);
                if (target == NULL) {
                    char msg[96];
                    int n2 = snprintf(msg, sizeof(msg), "ERR Unknown node %s",
                                      id);
                    resp_write_error(out, msg, (size_t)n2);
                    return;
                }
                for (j = 0; j < d->nnodes; j++)
                    cluster_slots_set(d->nodes[j].slots, (uint32_t)slot, 0);
                cluster_slots_set(target->slots, (uint32_t)slot, 1);
                d->slot_migrating[slot] = 0xFFFFu;
                d->slot_importing[slot] = 0xFFFFu;
                target->epoch = cluster_next_epoch(d);
                d->cluster_changes++;
                d->slot_owner_dirty = 1;
                resp_write_simple_string(out, "OK", 2);
                return;
            }

            if (ci_equal(sub, sl, "MEET") && argc == 4) {
                const char *ip, *pv;
                size_t ipl, pvl;
                long long port;
                if (!arg_str(&argv[2], &ip, &ipl) ||
                    !arg_str(&argv[3], &pv, &pvl))
                    goto bad_type;
                if (!parse_i64(pv, pvl, &port) || port <= 0 || port > 65535) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                {
                    char hostbuf[64];
                    if (ipl >= sizeof(hostbuf)) {
                        resp_write_error(out, "ERR invalid ip", 16);
                        return;
                    }
                    memcpy(hostbuf, ip, ipl);
                    hostbuf[ipl] = '\0';
                    if (s->cluster_meet == NULL ||
                        s->cluster_meet(s->cluster_ctx, hostbuf,
                                        (uint16_t)port) != 0) {
                        resp_write_error(out,
                                         "ERR cluster meet failed", 22);
                        return;
                    }
                }
                resp_write_simple_string(out, "OK", 2);
                return;
            }

            if (ci_equal(sub, sl, "REPLICATE") && argc == 3) {
                const char *ids;
                size_t idl;
                char id[41];
                cluster_node *target, *me;
                if (!arg_str(&argv[2], &ids, &idl))
                    goto bad_type;
                if (idl != 40) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                memcpy(id, ids, 40);
                id[40] = '\0';
                target = cluster_node_find(d, id);
                if (target == NULL) {
                    char msg[96];
                    int n2 = snprintf(msg, sizeof(msg), "ERR Unknown node %s",
                                      id);
                    resp_write_error(out, msg, (size_t)n2);
                    return;
                }
                if (target->flags & CLUSTER_NODE_MYSELF) {
                    static const char E[] = "ERR Can't replicate myself";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                if (!(target->flags & CLUSTER_NODE_MASTER)) {
                    static const char E[] =
                        "ERR I can only replicate a master";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                me = cluster_myself(d);
                if (me == NULL) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                me->flags &= ~(uint32_t)CLUSTER_NODE_MASTER;
                me->flags |= CLUSTER_NODE_SLAVE;
                snprintf(me->master_id, sizeof(me->master_id), "%s", id);
                d->cluster_changes++;
                /* data replication rides along (server hook; NULL in
                 * stack-session tests) */
                if (s->cluster_replicate != NULL)
                    s->cluster_replicate(s->cluster_ctx, target->ip,
                                         target->port);
                resp_write_simple_string(out, "OK", 2);
                return;
            }

            if (ci_equal(sub, sl, "FAILOVER") && (argc == 2 || argc == 3)) {
                cluster_node *me;
                if (argc == 3) {
                    const char *o;
                    size_t ol;
                    if (!arg_str(&argv[2], &o, &ol))
                        goto bad_type;
                    if (!ci_equal(o, ol, "TAKEOVER")) {
                        resp_write_error(out, ERR_SYNTAX,
                                         sizeof(ERR_SYNTAX) - 1);
                        return;
                    }
                }
                me = cluster_myself(d);
                if (me == NULL || !(me->flags & CLUSTER_NODE_SLAVE)) {
                    static const char E[] =
                        "ERR You should send CLUSTER FAILOVER to a replica";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                /* simplified: no master consent/vote; plain FAILOVER acts
                 * like TAKEOVER (documented) */
                if (cluster_failover_promote(d) &&
                    s->cluster_replicate != NULL)
                    s->cluster_replicate(s->cluster_ctx, NULL, 0);
                resp_write_simple_string(out, "OK", 2);
                return;
            }

            if (ci_equal(sub, sl, "GETKEYSINSLOT") && argc == 4) {
                const char *sv, *cv;
                size_t svl, cvl;
                long long slot, count;
                if (!arg_str(&argv[2], &sv, &svl) ||
                    !arg_str(&argv[3], &cv, &cvl))
                    goto bad_type;
                if (!parse_i64(sv, svl, &slot) ||
                    !parse_i64(cv, cvl, &count)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (slot < 0 || slot >= 16384) {
                    resp_write_error(out, "ERR Invalid slot", 16);
                    return;
                }
                if (count < 0)
                    count = 0;
                {
                    slot_scan_ctx ctx;
                    /* pass 1: count; pass 2: emit up to count */
                    ctx.slot = (uint32_t)slot;
                    ctx.limit = -1;
                    ctx.emitted = 0;
                    ctx.out = NULL;
                    rh_each(&d->table, slot_scan_cb, &ctx);
                    if (count > ctx.emitted)
                        count = ctx.emitted;
                    resp_write_array_header(out, (size_t)count);
                    ctx.limit = count;
                    ctx.emitted = 0;
                    ctx.out = out;
                    rh_each(&d->table, slot_scan_cb, &ctx);
                }
                return;
            }
            if (ci_equal(sub, sl, "ADDSLOTSRANGE") && argc >= 4) {
                size_t i;
                if ((argc - 2) % 2 != 0) {
                    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                    return;
                }
                for (i = 2; i < argc; i += 2) {
                    const char *sv, *ev;
                    size_t svl, evl;
                    long long start, end, j;
                    if (!arg_str(&argv[i], &sv, &svl) ||
                        !arg_str(&argv[i + 1], &ev, &evl))
                        goto bad_type;
                    if (!parse_i64(sv, svl, &start) ||
                        !parse_i64(ev, evl, &end)) {
                        resp_write_error(out, ERR_NOT_INT,
                                         sizeof(ERR_NOT_INT) - 1);
                        return;
                    }
                    if (start < 0 || end < 0 || start >= 16384 ||
                        end >= 16384 || start > end) {
                        resp_write_error(out, "ERR Invalid or out of range slot",
                                         sizeof("ERR Invalid or out of range slot") - 1);
                        return;
                    }
                    for (j = start; j <= end; j++) {
                        int k;
                        for (k = 0; k < d->nnodes; k++)
                            if (cluster_slots_get(d->nodes[k].slots,
                                                  (uint32_t)j)) {
                                char msg[64];
                                int n2 = snprintf(msg, sizeof(msg),
                                                  "ERR Slot %lld is already busy",
                                                  j);
                                resp_write_error(out, msg, (size_t)n2);
                                return;
                            }
                        cluster_slots_set(cluster_myself(d)->slots, (uint32_t)j, 1);
                    }
                }
                cluster_myself(d)->epoch = cluster_next_epoch(d);
                d->cluster_changes++;
                d->slot_owner_dirty = 1;
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "DELSLOTSRANGE") && argc >= 4) {
                size_t i;
                if ((argc - 2) % 2 != 0) {
                    resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                    return;
                }
                for (i = 2; i < argc; i += 2) {
                    const char *sv, *ev;
                    size_t svl, evl;
                    long long start, end, j;
                    if (!arg_str(&argv[i], &sv, &svl) ||
                        !arg_str(&argv[i + 1], &ev, &evl))
                        goto bad_type;
                    if (!parse_i64(sv, svl, &start) ||
                        !parse_i64(ev, evl, &end)) {
                        resp_write_error(out, ERR_NOT_INT,
                                         sizeof(ERR_NOT_INT) - 1);
                        return;
                    }
                    if (start < 0 || end < 0 || start >= 16384 ||
                        end >= 16384 || start > end) {
                        resp_write_error(out, "ERR Invalid or out of range slot",
                                         sizeof("ERR Invalid or out of range slot") - 1);
                        return;
                    }
                    for (j = start; j <= end; j++)
                        cluster_slots_set(cluster_myself(d)->slots, (uint32_t)j, 0);
                }
                d->cluster_changes++;
                d->slot_owner_dirty = 1;
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "BUMPEPOCH") && argc == 2) {
                cluster_myself(d)->epoch = cluster_next_epoch(d);
                d->cluster_changes++;
                resp_write_simple_string(out, "BUMPED 1", 8);
                return;
            }
            if (ci_equal(sub, sl, "COUNT-FAILURE-REPORTS") && argc == 3) {
                const char *ids;
                size_t idl;
                cluster_node *n;
                char id[41];
                if (!arg_str(&argv[2], &ids, &idl) || idl != 40) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                memcpy(id, ids, 40);
                id[40] = '\0';
                n = cluster_node_find(d, id);
                if (n == NULL) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                resp_write_integer(out, n->nreports);
                return;
            }
            if (ci_equal(sub, sl, "FLUSHSLOTS") && argc == 2) {
                int j;
                for (j = 0; j < 16384; j++)
                    cluster_slots_set(cluster_myself(d)->slots, (uint32_t)j, 0);
                d->cluster_changes++;
                d->slot_owner_dirty = 1;
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "FORGET") && argc == 3) {
                const char *ids;
                size_t idl;
                cluster_node *n;
                char id[41];
                int idx, j;
                if (!arg_str(&argv[2], &ids, &idl) || idl != 40) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                memcpy(id, ids, 40);
                id[40] = '\0';
                n = cluster_node_find(d, id);
                if (n == NULL) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                if (n->flags & CLUSTER_NODE_MYSELF) {
                    static const char E[] =
                        "ERR I tried hard but I can't forget myself";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                for (j = 0; j < 16384; j++)
                    if (cluster_slots_get(n->slots, (uint32_t)j)) {
                        static const char E[] =
                            "ERR Can't forget a node with slots assigned";
                        resp_write_error(out, E, sizeof(E) - 1);
                        return;
                    }
                idx = (int)(n - d->nodes);
                memmove(&d->nodes[idx], &d->nodes[idx + 1],
                        (size_t)(d->nnodes - idx - 1) * sizeof(d->nodes[0]));
                d->nnodes--;
                d->cluster_changes++;
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "LINKS") && argc == 2) {
                resp_write_array_header(out, 0);
                return;
            }
            if (ci_equal(sub, sl, "MYSHARDID") && argc == 2) {
                resp_write_bulk(out, d->node_id, strlen(d->node_id));
                return;
            }
            if (ci_equal(sub, sl, "REPLICAS") && argc == 3) {
                const char *ids;
                size_t idl;
                char id[41];
                if (!arg_str(&argv[2], &ids, &idl) || idl != 40) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                memcpy(id, ids, 40);
                id[40] = '\0';
                if (cluster_node_find(d, id) == NULL) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                resp_write_array_header(out, 0);
                return;
            }
            if (ci_equal(sub, sl, "RESET") && (argc == 2 || argc == 3)) {
                int hard = 0;
                cluster_node *me = cluster_myself(d);
                if (argc == 3) {
                    const char *mode;
                    size_t mdl;
                    if (!arg_str(&argv[2], &mode, &mdl))
                        goto bad_type;
                    if (ci_equal(mode, mdl, "HARD"))
                        hard = 1;
                    else if (!ci_equal(mode, mdl, "SOFT")) {
                        resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                        return;
                    }
                }
                if (me == NULL) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                if (me != &d->nodes[0]) {
                    cluster_node tmp = *me;
                    d->nodes[0] = tmp;
                }
                d->nnodes = 1;
                if (hard) {
                    int j;
                    for (j = 0; j < 16384; j++)
                        cluster_slots_set(d->nodes[0].slots, (uint32_t)j, 0);
                }
                {
                    int j;
                    for (j = 0; j < 16384; j++) {
                        d->slot_migrating[j] = 0xFFFFu;
                        d->slot_importing[j] = 0xFFFFu;
                    }
                }
                d->slot_owner_dirty = 1;
                d->cluster_changes++;
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "SAVECONFIG") && argc == 2) {
                static const char E[] =
                    "ERR The server is running without a config file";
                resp_write_error(out, E, sizeof(E) - 1);
                return;
            }
            if (ci_equal(sub, sl, "SET-CONFIG-EPOCH") && argc == 3) {
                const char *ev;
                size_t evl;
                long long epoch;
                if (!arg_str(&argv[2], &ev, &evl) ||
                    !parse_i64(ev, evl, &epoch)) {
                    resp_write_error(out, ERR_NOT_INT,
                                     sizeof(ERR_NOT_INT) - 1);
                    return;
                }
                if (epoch <= 0 || epoch <= (long long)d->cluster_current_epoch) {
                    static const char E[] =
                        "ERR The config epoch cannot be set to a value "
                        "lower or equal to the current one";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
                }
                cluster_myself(d)->epoch = (uint64_t)epoch;
                d->cluster_current_epoch = (uint64_t)epoch;
                d->cluster_changes++;
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(sub, sl, "SHARDS") && argc == 2) {
                resp_write_array_header(out, 0);
                return;
            }
            if (ci_equal(sub, sl, "SLAVES") && argc == 3) {
                const char *ids;
                size_t idl;
                char id[41];
                if (!arg_str(&argv[2], &ids, &idl) || idl != 40) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                memcpy(id, ids, 40);
                id[40] = '\0';
                if (cluster_node_find(d, id) == NULL) {
                    resp_write_error(out, "ERR Unknown node ", 17);
                    return;
                }
                resp_write_array_header(out, 0);
                return;
            }
            {
                char lc[32];
                char msg[160];
                size_t i;
                int n2;
                for (i = 0; i < sl && i < sizeof(lc) - 1; i++)
                    lc[i] = (sub[i] >= 'A' && sub[i] <= 'Z')
                                ? (char)(sub[i] + ('a' - 'A'))
                                : sub[i];
                lc[i] = '\0';
                n2 = snprintf(msg, sizeof(msg),
                              "ERR Unknown CLUSTER subcommand or wrong "
                              "number of arguments for '%s'",
                              lc);
                resp_write_error(out, msg, (size_t)n2);
            }
        }
        return;
    }

    {
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "ERR unknown command '%.*s'",
                         (int)nlen, name);
        resp_write_error(out, msg, (size_t)n);
    }
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

/* ------------------------------------------------------------------ */
/* MULTI queue-time validation                                        */
/* ------------------------------------------------------------------ */

/* Command table flags. */
#define CMD_WRITE 0x01

static const cmd_entry CMD_TABLE[] = {
    {"ping", CMD_PING, 1, 2, 0, 0},
    {"echo", CMD_ECHO, 2, 2, 0, 0},
    {"get", CMD_GET, 2, 2, 0, 0},
    {"set", CMD_SET, 3, -1, 0, CMD_WRITE},
    {"dump", CMD_DUMP, 2, 2, 0, 0},
    {"restore", CMD_RESTORE, 4, -1, 0, CMD_WRITE},
    {"migrate", CMD_MIGRATE, 6, -1, 0, CMD_WRITE},
    {"asking", CMD_ASKING, 1, 1, 0, 0},
    {"del", CMD_DEL, 2, -1, 0, CMD_WRITE},
    {"unlink", CMD_UNLINK, 2, -1, 0, CMD_WRITE},
    {"exists", CMD_EXISTS, 2, -1, 0, 0},
    {"incr", CMD_INCR, 2, 2, 0, CMD_WRITE},
    {"decr", CMD_DECR, 2, 2, 0, CMD_WRITE},
    {"append", CMD_APPEND, 3, 3, 0, CMD_WRITE},
    {"strlen", CMD_STRLEN, 2, 2, 0, 0},
    {"mget", CMD_MGET, 2, -1, 0, 0},
    {"mset", CMD_MSET, 3, -1, 1, CMD_WRITE},
    {"expire", CMD_EXPIRE, 3, 7, 0, CMD_WRITE},
    {"pexpire", CMD_PEXPIRE, 3, 7, 0, CMD_WRITE},
    {"expireat", CMD_EXPIREAT, 3, 7, 0, CMD_WRITE},
    {"pexpireat", CMD_PEXPIREAT, 3, 7, 0, CMD_WRITE},
    {"ttl", CMD_TTL, 2, 2, 0, 0},
    {"pttl", CMD_PTTL, 2, 2, 0, 0},
    {"persist", CMD_PERSIST, 2, 2, 0, CMD_WRITE},
    {"dbsize", CMD_DBSIZE, 1, 1, 0, 0},
    {"flushdb", CMD_FLUSHDB, 1, 1, 0, CMD_WRITE},
    {"config", CMD_CONFIG, 2, -1, 0, 0},
    {"info", CMD_INFO, 1, 1, 0, 0},
    {"hset", CMD_HSET, 4, -1, 2, CMD_WRITE},
    {"hmset", CMD_HMSET, 4, -1, 2, CMD_WRITE},
    {"hget", CMD_HGET, 3, 3, 0, 0},
    {"hdel", CMD_HDEL, 2, -1, 0, CMD_WRITE},
    {"hexists", CMD_HEXISTS, 3, 3, 0, 0},
    {"hlen", CMD_HLEN, 2, 2, 0, 0},
    {"hgetall", CMD_HGETALL, 2, 2, 0, 0},
    {"hkeys", CMD_HKEYS, 2, 2, 0, 0},
    {"hvals", CMD_HVALS, 2, 2, 0, 0},
    {"hmget", CMD_HMGET, 3, -1, 0, 0},
    {"hincrby", CMD_HINCRBY, 4, 4, 0, CMD_WRITE},
    {"hsetnx", CMD_HSETNX, 4, 4, 0, CMD_WRITE},
    {"lpush", CMD_LPUSH, 3, -1, 0, CMD_WRITE},
    {"rpush", CMD_RPUSH, 3, -1, 0, CMD_WRITE},
    {"lpushx", CMD_LPUSHX, 3, -1, 0, CMD_WRITE},
    {"rpushx", CMD_RPUSHX, 3, -1, 0, CMD_WRITE},
    {"lpop", CMD_LPOP, 2, 3, 0, CMD_WRITE},
    {"rpop", CMD_RPOP, 2, 3, 0, CMD_WRITE},
    {"llen", CMD_LLEN, 2, 2, 0, 0},
    {"lrange", CMD_LRANGE, 4, 4, 0, 0},
    {"lindex", CMD_LINDEX, 3, 3, 0, 0},
    {"lset", CMD_LSET, 4, 4, 0, CMD_WRITE},
    {"sadd", CMD_SADD, 3, -1, 0, CMD_WRITE},
    {"srem", CMD_SREM, 3, -1, 0, CMD_WRITE},
    {"sismember", CMD_SISMEMBER, 3, 3, 0, 0},
    {"smismember", CMD_SMISMEMBER, 3, -1, 0, 0},
    {"scard", CMD_SCARD, 2, 2, 0, 0},
    {"smembers", CMD_SMEMBERS, 2, 2, 0, 0},
    {"spop", CMD_SPOP, 2, 3, 0, CMD_WRITE},
    {"srandmember", CMD_SRANDMEMBER, 2, 3, 0, 0},
    {"smove", CMD_SMOVE, 4, 4, 0, CMD_WRITE},
    {"sinter", CMD_SINTER, 2, -1, 0, 0},
    {"sunion", CMD_SUNION, 2, -1, 0, 0},
    {"sdiff", CMD_SDIFF, 2, -1, 0, 0},
    {"zadd", CMD_ZADD, 4, -1, 0, CMD_WRITE},
    {"zscore", CMD_ZSCORE, 3, 3, 0, 0},
    {"zcard", CMD_ZCARD, 2, 2, 0, 0},
    {"zincrby", CMD_ZINCRBY, 4, 4, 0, CMD_WRITE},
    {"zrem", CMD_ZREM, 3, -1, 0, CMD_WRITE},
    {"zrange", CMD_ZRANGE, 4, -1, 0, 0},
    {"zrevrange", CMD_ZREVRANGE, 4, 5, 0, 0},
    {"zrank", CMD_ZRANK, 3, 3, 0, 0},
    {"zrevrank", CMD_ZREVRANK, 3, 3, 0, 0},
    {"zcount", CMD_ZCOUNT, 4, 4, 0, 0},
    {"zrangebyscore", CMD_ZRANGEBYSCORE, 4, -1, 0, 0},
    {"zremrangebyscore", CMD_ZREMRANGEBYSCORE, 4, 4, 0, CMD_WRITE},
    {"multi", CMD_MULTI, 1, 1, 0, 0},
    {"exec", CMD_EXEC, 1, 1, 0, 0},
    {"discard", CMD_DISCARD, 1, 1, 0, 0},
    {"watch", CMD_WATCH, 2, -1, 0, 0},
    {"unwatch", CMD_UNWATCH, 1, 1, 0, 0},
    {"subscribe", CMD_SUBSCRIBE, 2, -1, 0, 0},
    {"unsubscribe", CMD_UNSUBSCRIBE, 1, -1, 0, 0},
    {"publish", CMD_PUBLISH, 3, 3, 0, 0},
    {"quit", CMD_QUIT, 1, 1, 0, 0},
    {"sync", CMD_SYNC, 1, 1, 0, 0},
    {"psync", CMD_PSYNC, 3, 3, 0, 0},
    {"replicaof", CMD_REPLICAOF, 3, 3, 0, 0},
    {"save", CMD_SAVE, 1, 1, 0, 0},
    {"lastsave", CMD_LASTSAVE, 1, 1, 0, 0},
    {"shutdown", CMD_SHUTDOWN, 1, 1, 0, 0},
    {"cluster", CMD_CLUSTER, 2, -1, 0, 0},
    {"auth", CMD_AUTH, 2, 3, 0, 0},
    {"select", CMD_SELECT, 2, 2, 0, 0},
    {"swapdb", CMD_SWAPDB, 3, 3, 0, CMD_WRITE},
    {"eval", CMD_EVAL, 3, -1, 0, CMD_WRITE},
    {"evalsha", CMD_EVALSHA, 3, -1, 0, CMD_WRITE},
    {"script", CMD_SCRIPT, 2, -1, 0, 0},
    {"ssubscribe", CMD_SSUBSCRIBE, 2, -1, 0, 0},
    {"sunsubscribe", CMD_SUNSUBSCRIBE, 1, -1, 0, 0},
    {"spublish", CMD_SPUBLISH, 3, 3, 0, 0},
    {"pubsub", CMD_PUBSUB, 2, -1, 0, 0},
    {"type", CMD_TYPE, 2, 2, 0, 0},
    {"keys", CMD_KEYS, 2, 2, 0, 0},
    {"scan", CMD_SCAN, 2, -1, 0, 0},
    {"rename", CMD_RENAME, 3, 3, 0, CMD_WRITE},
    {"renamenx", CMD_RENAMENX, 3, 3, 0, CMD_WRITE},
    {"touch", CMD_TOUCH, 2, -1, 0, 0},
    {"randomkey", CMD_RANDOMKEY, 1, 1, 0, 0},
    {"expiretime", CMD_EXPIRETIME, 2, 2, 0, 0},
    {"pexpiretime", CMD_PEXPIRETIME, 2, 2, 0, 0},
    {"getdel", CMD_GETDEL, 2, 2, 0, CMD_WRITE},
    {"getex", CMD_GETEX, 2, -1, 0, CMD_WRITE},
    {"setex", CMD_SETEX, 4, 4, 0, CMD_WRITE},
    {"psetex", CMD_PSETEX, 4, 4, 0, CMD_WRITE},
    {"getset", CMD_GETSET, 3, 3, 0, CMD_WRITE},
    {"setrange", CMD_SETRANGE, 4, 4, 0, CMD_WRITE},
    {"getrange", CMD_GETRANGE, 4, 4, 0, 0},
    {"incrby", CMD_INCRBY, 3, 3, 0, CMD_WRITE},
    {"decrby", CMD_DECRBY, 3, 3, 0, CMD_WRITE},
    {"incrbyfloat", CMD_INCRBYFLOAT, 3, 3, 0, CMD_WRITE},
    {"hstrlen", CMD_HSTRLEN, 3, 3, 0, 0},
    {"hrandfield", CMD_HRANDFIELD, 2, 4, 0, 0},
    {"lpos", CMD_LPOS, 3, -1, 0, 0},
    {"lrem", CMD_LREM, 4, 4, 0, CMD_WRITE},
    {"ltrim", CMD_LTRIM, 4, 4, 0, CMD_WRITE},
    {"rpoplpush", CMD_RPOPLPUSH, 3, 3, 0, CMD_WRITE},
    {"linsert", CMD_LINSERT, 5, 5, 0, CMD_WRITE},
    {"lmove", CMD_LMOVE, 5, 5, 0, CMD_WRITE},
    {"lmovem", CMD_LMOVEM, 5, -1, 0, CMD_WRITE},
    {"lmpop", CMD_LMPOP, 4, -1, 0, CMD_WRITE},
    {"blpop", CMD_BLPOP, 3, -1, 0, CMD_WRITE},
    {"brpop", CMD_BRPOP, 3, -1, 0, CMD_WRITE},
    {"brpoplpush", CMD_BRPOPLPUSH, 4, 4, 0, CMD_WRITE},
    {"blmove", CMD_BLMOVE, 6, 6, 0, CMD_WRITE},
    {"blmovem", CMD_BLMOVEM, 6, -1, 0, CMD_WRITE},
    {"blmpop", CMD_BLMPOP, 4, -1, 0, CMD_WRITE},
    {"sintercard", CMD_SINTERCARD, 3, -1, 0, 0},
    {"sinterstore", CMD_SINTERSTORE, 3, -1, 0, CMD_WRITE},
    {"sunionstore", CMD_SUNIONSTORE, 3, -1, 0, CMD_WRITE},
    {"sdiffstore", CMD_SDIFFSTORE, 3, -1, 0, CMD_WRITE},
    {"zpopmin", CMD_ZPOPMIN, 2, 3, 0, CMD_WRITE},
    {"zpopmax", CMD_ZPOPMAX, 2, 3, 0, CMD_WRITE},
    {"bzpopmin", CMD_BZPOPMIN, 3, -1, 0, CMD_WRITE},
    {"bzpopmax", CMD_BZPOPMAX, 3, -1, 0, CMD_WRITE},
    {"bzmpop", CMD_BZMPOP, 4, -1, 0, CMD_WRITE},
    {"zremrangebyrank", CMD_ZREMRANGEBYRANK, 4, 4, 0, CMD_WRITE},
    {"zmscore", CMD_ZMSCORE, 3, -1, 0, 0},
    {"zrandmember", CMD_ZRANDMEMBER, 2, 4, 0, 0},
    {"zrangebylex", CMD_ZRANGEBYLEX, 4, -1, 0, 0},
    {"zrevrangebylex", CMD_ZREVRANGEBYLEX, 4, -1, 0, 0},
    {"zremrangebylex", CMD_ZREMRANGEBYLEX, 4, 4, 0, CMD_WRITE},
    {"psubscribe", CMD_PSUBSCRIBE, 2, -1, 0, 0},
    {"punsubscribe", CMD_PUNSUBSCRIBE, 1, -1, 0, 0},
    {"copy", CMD_COPY, 3, -1, 0, CMD_WRITE},
    {"object", CMD_OBJECT, 2, 3, 0, 0},
    {"setnx", CMD_SETNX, 3, 3, 0, CMD_WRITE},
    {"msetnx", CMD_MSETNX, 3, -1, 1, CMD_WRITE},
    {"getbit", CMD_GETBIT, 3, 3, 0, 0},
    {"setbit", CMD_SETBIT, 4, 4, 0, CMD_WRITE},
    {"bitcount", CMD_BITCOUNT, 2, 4, 0, 0},
    {"bitpos", CMD_BITPOS, 3, 5, 0, 0},
    {"bitop", CMD_BITOP, 4, -1, 0, CMD_WRITE},
    {"bitfield", CMD_BITFIELD, 3, -1, 0, CMD_WRITE},
    {"bitfield_ro", CMD_BITFIELD_RO, 3, -1, 0, 0},
    {"zunionstore", CMD_ZUNIONSTORE, 4, -1, 0, CMD_WRITE},
    {"zinterstore", CMD_ZINTERSTORE, 4, -1, 0, CMD_WRITE},
    {"zdiffstore", CMD_ZDIFFSTORE, 4, -1, 0, CMD_WRITE},
    {"zunion", CMD_ZUNION, 3, -1, 0, 0},
    {"zinter", CMD_ZINTER, 3, -1, 0, 0},
    {"zdiff", CMD_ZDIFF, 3, -1, 0, 0},
    {"zintercard", CMD_ZINTERCARD, 3, -1, 0, 0},
    {"zlexcount", CMD_ZLEXCOUNT, 4, 4, 0, 0},
    {"zrevrangebyscore", CMD_ZREVRANGEBYSCORE, 4, -1, 0, 0},
    {"zrangestore", CMD_ZRANGESTORE, 5, -1, 0, CMD_WRITE},
    {"zmpop", CMD_ZMPOP, 3, -1, 0, CMD_WRITE},
    {"hscan", CMD_HSCAN, 3, -1, 0, 0},
    {"sscan", CMD_SSCAN, 3, -1, 0, 0},
    {"zscan", CMD_ZSCAN, 3, -1, 0, 0},
    {"flushall", CMD_FLUSHALL, 1, 1, 0, CMD_WRITE},
    {"time", CMD_TIME, 1, 1, 0, 0},
    {"hincrbyfloat", CMD_HINCRBYFLOAT, 4, 4, 0, CMD_WRITE},
    {"readonly", CMD_READONLY, 1, 1, 0, 0},
    {"readwrite", CMD_READWRITE, 1, 1, 0, 0},
    {"substr", CMD_SUBSTR, 4, 4, 0, 0},
    {"slaveof", CMD_SLAVEOF, 3, 3, 0, 0},
    {"move", CMD_MOVE, 3, 3, 0, CMD_WRITE},
    {"role", CMD_ROLE, 1, 1, 0, 0},
    {"reset", CMD_RESET, 1, 1, 0, 0},
    {"hello", CMD_HELLO, 1, -1, 0, 0},
    {"lcs", CMD_LCS, 3, -1, 0, 0},
    {"sort", CMD_SORT, 2, -1, 0, CMD_WRITE},
    {"sort_ro", CMD_SORT_RO, 2, -1, 0, 0},
    {"pfadd", CMD_PFADD, 3, -1, 0, CMD_WRITE},
    {"pfcount", CMD_PFCOUNT, 2, -1, 0, 0},
    {"pfdebug", CMD_PFDEBUG, 3, -1, 0, 0},
    {"pfmerge", CMD_PFMERGE, 3, -1, 0, CMD_WRITE},
    {"pfselftest", CMD_PFSELFTEST, 1, 1, 0, 0},
    {"geoadd", CMD_GEOADD, 5, -1, 0, CMD_WRITE},
    {"geodist", CMD_GEODIST, 4, 5, 0, 0},
    {"geohash", CMD_GEOHASH, 2, -1, 0, 0},
    {"geopos", CMD_GEOPOS, 2, -1, 0, 0},
    {"georadius", CMD_GEORADIUS, 6, -1, 0, CMD_WRITE},
    {"georadius_ro", CMD_GEORADIUS_RO, 6, -1, 0, 0},
    {"georadiusbymember", CMD_GEORADIUSBYMEMBER, 5, -1, 0, CMD_WRITE},
    {"georadiusbymember_ro", CMD_GEORADIUSBYMEMBER_RO, 5, -1, 0, 0},
    {"geosearch", CMD_GEOSEARCH, 7, -1, 0, 0},
    {"geosearchstore", CMD_GEOSEARCHSTORE, 8, -1, 0, CMD_WRITE},
    {"command", CMD_COMMAND, 2, -1, 0, 0},
    {"client", CMD_CLIENT, 2, -1, 0, 0},
    {"memory", CMD_MEMORY, 2, -1, 0, 0},
    {"slowlog", CMD_SLOWLOG, 2, -1, 0, 0},
    {"bgsave", CMD_BGSAVE, 1, 1, 0, 0},
    {"bgrewriteaof", CMD_BGREWRITEAOF, 1, 1, 0, 0},
    {"xadd", CMD_XADD, 5, -1, 0, CMD_WRITE},
    {"xlen", CMD_XLEN, 2, 2, 0, 0},
    {"xrange", CMD_XRANGE, 4, -1, 0, 0},
    {"xrevrange", CMD_XREVRANGE, 4, -1, 0, 0},
    {"xdel", CMD_XDEL, 3, -1, 0, CMD_WRITE},
    {"xdelex", CMD_XDELEX, 5, -1, 0, CMD_WRITE},
    {"xtrim", CMD_XTRIM, 4, -1, 0, CMD_WRITE},
    {"xgroup", CMD_XGROUP, 2, -1, 0, CMD_WRITE},
    {"xack", CMD_XACK, 4, -1, 0, CMD_WRITE},
    {"xackdel", CMD_XACKDEL, 6, -1, 0, CMD_WRITE},
    {"xpending", CMD_XPENDING, 3, 7, 0, 0},
    {"xclaim", CMD_XCLAIM, 6, -1, 0, CMD_WRITE},
    {"xautoclaim", CMD_XAUTOCLAIM, 6, -1, 0, CMD_WRITE},
    {"xnack", CMD_XNACK, 7, -1, 0, CMD_WRITE},
    {"xread", CMD_XREAD, 4, -1, 0, 0},
    {"xreadgroup", CMD_XREADGROUP, 7, -1, 0, CMD_WRITE},
    {"xinfo", CMD_XINFO, 2, -1, 0, 0},
    {"xsetid", CMD_XSETID, 3, -1, 0, CMD_WRITE},
    {"xcfgset", CMD_XCFGSET, 2, -1, 0, CMD_WRITE},
    {"xidmprecord", CMD_XIDMPRECORD, 5, 5, 0, CMD_WRITE},
    {"eval_ro", CMD_EVAL_RO, 3, -1, 0, 0},
    {"evalsha_ro", CMD_EVALSHA_RO, 3, -1, 0, 0},
    {"fcall", CMD_FCALL, 3, -1, 0, CMD_WRITE},
    {"fcall_ro", CMD_FCALL_RO, 3, -1, 0, 0},
    {"function", CMD_FUNCTION, 2, -1, 0, CMD_WRITE},
    {"restore-asking", CMD_RESTORE_ASKING, 4, -1, 0, CMD_WRITE},
    {"lolwut", CMD_LOLWUT, 1, -1, 0, 0},
    {"wait", CMD_WAIT, 3, 3, 0, 0},
    {"waitaof", CMD_WAITAOF, 4, 4, 0, 0},
    {"replconf", CMD_REPLCONF, 3, -1, 0, 0},
    {"failover", CMD_FAILOVER, 1, -1, 0, CMD_WRITE},
    {"monitor", CMD_MONITOR, 1, 1, 0, 0},
    {"acl", CMD_ACL, 2, -1, 0, 0},
    {"debug", CMD_DEBUG, 2, -1, 0, 0},
    {"latency", CMD_LATENCY, 2, -1, 0, 0},
    {"module", CMD_MODULE, 2, -1, 0, 0},
    {"sentinel", CMD_SENTINEL, 2, -1, 0, 0},
    {"hgetdel", CMD_HGETDEL, 5, -1, 0, CMD_WRITE},
    {"hsetex", CMD_HSETEX, 6, -1, 0, CMD_WRITE},
    {"hgetex", CMD_HGETEX, 5, -1, 0, CMD_WRITE},
    {"hexpire", CMD_HEXPIRE, 6, -1, 0, CMD_WRITE},
    {"hpexpire", CMD_HPEXPIRE, 6, -1, 0, CMD_WRITE},
    {"hexpireat", CMD_HEXPIREAT, 6, -1, 0, CMD_WRITE},
    {"hpexpireat", CMD_HPEXPIREAT, 6, -1, 0, CMD_WRITE},
    {"hpersist", CMD_HPERSIST, 5, -1, 0, CMD_WRITE},
    {"httl", CMD_HTTL, 5, -1, 0, 0},
    {"hpttl", CMD_HPTTL, 5, -1, 0, 0},
    {"hexpiretime", CMD_HEXPIRETIME, 5, -1, 0, 0},
    {"hpexpiretime", CMD_HPEXPIRETIME, 5, -1, 0, 0},
    {"sunioncard", CMD_SUNIONCARD, 3, -1, 0, 0},
    {"sdiffcard", CMD_SDIFFCARD, 3, -1, 0, 0},
};

static const cmd_entry *cmd_table_entry(uint16_t id)
{
    static const cmd_entry unknown = {"unknown", CMD_ID_UNKNOWN, 0, 0, 0, 0};
    if (id == 0 || id > (uint16_t)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))
        return &unknown;
    return &CMD_TABLE[id - 1];
}

/* ------------------------------------------------------------------ */
/* Command name -> stable ID hash table                               */
/* ------------------------------------------------------------------ */

#define CMD_HASH_SIZE 512

typedef struct cmd_hash_slot {
    const char *name;
    uint8_t nlen;
    uint16_t id;
} cmd_hash_slot;

static cmd_hash_slot cmd_hash[CMD_HASH_SIZE];
static int cmd_hash_inited = 0;

static uint32_t cmd_hash_fn(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

static void cmd_hash_init(void)
{
    size_t i;
    if (cmd_hash_inited) return;
    memset(cmd_hash, 0, sizeof(cmd_hash));
    for (i = 0; i < sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0]); i++) {
        const cmd_entry *e = &CMD_TABLE[i];
        uint32_t h = cmd_hash_fn(e->name, strlen(e->name));
        size_t idx = h & (CMD_HASH_SIZE - 1);
        while (cmd_hash[idx].id != 0)
            idx = (idx + 1) & (CMD_HASH_SIZE - 1);
        cmd_hash[idx].name = e->name;
        cmd_hash[idx].nlen = (uint8_t)strlen(e->name);
        cmd_hash[idx].id = e->id;
    }
    cmd_hash_inited = 1;
}

uint16_t cmd_resolve(const char *name, size_t len)
{
    uint32_t h;
    size_t idx;
    if (!cmd_hash_inited) cmd_hash_init();
    if (len == 0 || len > 255) return CMD_ID_UNKNOWN;
    h = cmd_hash_fn(name, len);
    idx = h & (CMD_HASH_SIZE - 1);
    while (cmd_hash[idx].id != 0) {
        if (cmd_hash[idx].nlen == (uint8_t)len &&
            ci_equal(name, len, cmd_hash[idx].name))
            return cmd_hash[idx].id;
        idx = (idx + 1) & (CMD_HASH_SIZE - 1);
    }
    return CMD_ID_UNKNOWN;
}

int cmd_is_write(uint16_t cmd_id)
{
    if (cmd_id == 0 || cmd_id > (uint16_t)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))
        return 0;
    return (CMD_TABLE[cmd_id - 1].flags & CMD_WRITE) != 0;
}

int cmd_min_argc(uint16_t cmd_id)
{
    if (cmd_id == 0 || cmd_id > (uint16_t)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))
        return -1;
    return CMD_TABLE[cmd_id - 1].min_argc;
}

int cmd_max_argc(uint16_t cmd_id)
{
    if (cmd_id == 0 || cmd_id > (uint16_t)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))
        return -1;
    return CMD_TABLE[cmd_id - 1].max_argc;
}

int cmd_parity(uint16_t cmd_id)
{
    if (cmd_id == 0 || cmd_id > (uint16_t)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])))
        return 0;
    return CMD_TABLE[cmd_id - 1].parity;
}

/* Queue-time check: unknown command or bad arity writes the error reply,
 * flags multi_error and returns -1; 0 = queueable. */
static int queue_validate(session *s, const resp_value *argv, size_t argc,
                          resp_buf *out)
{
    const char *name;
    size_t nlen;
    uint16_t cmd_id;
    const cmd_entry *ce;
    if (!arg_str(&argv[0], &name, &nlen)) {
        s->multi_error = 1;
        resp_write_error(out, "ERR invalid command name", 23);
        return -1;
    }
    cmd_id = cmd_resolve(name, nlen);
    if (cmd_id == CMD_ID_UNKNOWN) {
        s->multi_error = 1;
        {
            char msg[128];
            int n = snprintf(msg, sizeof(msg), "ERR unknown command '%.*s'",
                             (int)nlen, name);
            resp_write_error(out, msg, (size_t)n);
        }
        return -1;
    }
    ce = &CMD_TABLE[cmd_id - 1];
    if ((int)argc < ce->min_argc ||
        (ce->max_argc >= 0 && (int)argc > ce->max_argc) ||
        (ce->parity == 1 && argc % 2 == 0) ||
        (ce->parity == 2 && argc % 2 == 1)) {
        s->multi_error = 1;
        wrong_args(out, ce->name);
        return -1;
    }
    return 0;
}

/* EXEC: replay the queue (or abort / null-array on dirty watch). */
static void exec_transaction(session *s, resp_buf *out, uint64_t now_ms)
{
    size_t i;
    if (!s->multi_error) {
        for (i = 0; i < s->nwatch; i++) {
            watch_entry *w = &s->watches[i];
            db *wd = s->d;
            if (s->sel_fn != NULL)
                wd = s->sel_fn(s->sel_ctx, w->db_index);
            if (wd == NULL || w->epoch != wd->flush_epoch ||
                w->version != db_key_version(wd, w->key, w->klen)) {
                /* dirty watch: null array, nothing applied */
                static const char null_arr[] = "*-1\r\n";
                if (resp_buf_reserve(out, sizeof(null_arr) - 1) != 0)
                    goto done;
                memcpy(out->data + out->len, null_arr, sizeof(null_arr) - 1);
                out->len += sizeof(null_arr) - 1;
                goto done;
            }
        }
    }
    if (s->multi_error) {
        static const char E[] =
            "EXECABORT Transaction discarded because of previous errors.";
        resp_write_error(out, E, sizeof(E) - 1);
        goto done;
    }
    /* cluster mode: every queued command's keys must share one slot */
    if (s->d->cluster_enabled) {
        int ok = 1, have = 0;
        uint32_t slot = 0;
        for (i = 0; i < s->queue_len; i++)
            if (!cmd_keys_accum(s->queue[i].argv, s->queue[i].argc, &have,
                                &slot)) {
                ok = 0;
                break;
            }
        if (!ok) {
            resp_write_error(out, CROSSSLOT_MSG, sizeof(CROSSSLOT_MSG) - 1);
            goto done;
        }
    }
    {
        uint64_t dirty_before = s->d->dirty;
        resp_write_array_header(out, s->queue_len);
        for (i = 0; i < s->queue_len; i++) {
#ifdef DDUP_NO_CMDSTATS
            command_dispatch(s, s->queue[i].argv, s->queue[i].argc, out,
                             now_ms);
#else
            uint64_t t0 = pal_now_us();
            const char *qn = NULL;
            size_t qnl = 0;
            uint16_t qid;
            command_dispatch(s, s->queue[i].argv, s->queue[i].argc, out,
                             now_ms);
            /* commandstats: each EXEC-replayed command counts individually */
            if (s->queue[i].argc > 0)
                (void)arg_str(&s->queue[i].argv[0], &qn, &qnl);
            qid = qn != NULL ? cmd_resolve(qn, qnl) : CMD_ID_UNKNOWN;
            if (qid != CMD_ID_UNKNOWN && qid < CMD_STATS_SLOTS) {
                s->d->cmd_calls[qid]++;
                s->d->cmd_usecs[qid] += pal_now_us() - t0;
            }
#endif
            /* EVAL flags itself: its effects are logged, not its argv */
            s->queue[i].skip_log = s->aof_skip;
            s->aof_skip = 0;
        }
        /* AOF: log each applied command individually (no MULTI wrapper);
         * EVAL entries log their effects instead */
        if (s->d->dirty != dirty_before && s->aof_log != NULL)
            for (i = 0; i < s->queue_len; i++)
                if (!s->queue[i].skip_log)
                    s->aof_log(s->aof_ctx, s->db_index, s->queue[i].argv,
                               s->queue[i].argc, NULL, 0);
    }
    /* queued writes may have crossed maxmemory */
    if (s->d->maxmemory_policy == DB_POLICY_ALLKEYS_LRU)
        db_evict_if_needed(s->d, now_ms);
done:
    session_queue_clear(s);
    session_watch_clear(s);
    s->in_multi = 0;
    s->multi_error = 0;
}

void session_execute_at(session *s, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    const char *name = NULL;
    size_t nlen = 0;
    uint16_t cmd_id = CMD_ID_UNKNOWN;
    uint64_t dirty_before = s->d->dirty;
    s->d->tier_io_error = 0;
    if (argc > 0)
        (void)arg_str(&argv[0], &name, &nlen);
    if (name != NULL)
        cmd_id = cmd_resolve(name, nlen);

    /* Lean GET/SET (Phase 36): a plain session (authed, not in MULTI, not
     * subscribed, cluster off) running GET, or SET with no options, skips
     * the second cmd_resolve, the READONLY/ownership wrappers and the
     * dispatch if-chain. Semantics are identical to the command_dispatch
     * blocks (same arg checks, same replies, same propagation/eviction
     * tail); cmd_calls still counts, per-call usec timing is skipped --
     * the two clock reads cost more than the stat is worth on this path. */
    if (cmd_id == CMD_GET && s->authed && !s->in_multi && s->nsub == 0 &&
        s->nssub == 0 && s->npsub == 0 && !s->d->cluster_enabled) {
        const char *k;
        size_t kl;
        const char *v;
        size_t vl;
        if (argc != 2) {
            wrong_args(out, "get");
            return;
        }
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (db_get(s->d, k, kl, &v, &vl, now_ms)) {
            const char *sv;
            size_t sl;
            if (!as_string(out, v, vl, &sv, &sl))
                return;
            resp_write_bulk(out, sv, sl);
        } else {
            if (s->d->tier_io_error) {
                tier_io_reply(out);
                return;
            }
            resp_write_bulk(out, NULL, 0);
        }
        s->d->cmd_calls[CMD_GET]++;
        if (s->d->maxmemory_policy == DB_POLICY_ALLKEYS_LRU)
            db_evict_if_needed(s->d, now_ms);
        return;
    }
    if (cmd_id == CMD_SET && argc == 3 && s->authed && !s->in_multi &&
        s->nsub == 0 && s->nssub == 0 && s->npsub == 0 &&
        !s->d->cluster_enabled &&
        (s->role == NULL || *s->role != SESSION_ROLE_REPLICA ||
         s->repl_link)) {
        const char *k, *v;
        size_t kl, vl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &v, &vl))
            goto bad_type;
        if (!storage_string_ok(kl, vl)) {
            storage_length_error(out);
            return;
        }
        {
            const char *old;
            size_t oldl;
            int exists = db_get(s->d, k, kl, &old, &oldl, now_ms);
            if (exists && obj_tag_of(old, oldl) != DDUP_OBJ_STRING) {
                wrongtype(out);
                return;
            }
            if (s->d->tier_io_error) {
                tier_io_reply(out);
                return;
            }
        }
        if (oom_blocked(s->d, out))
            return;
        if (db_set_string(s->d, k, kl, v, vl, now_ms) != 0) {
            storage_length_error(out);
            return;
        }
        resp_write_simple_string(out, "OK", 2);
        s->d->cmd_calls[CMD_SET]++;
        if (s->d->dirty != dirty_before && s->aof_log != NULL &&
            !s->aof_skip)
            s->aof_log(s->aof_ctx, s->db_index, argv, argc, s->raw_cmd,
                       s->raw_cmd_len);
        s->aof_skip = 0;
        if (s->d->maxmemory_policy == DB_POLICY_ALLKEYS_LRU)
            db_evict_if_needed(s->d, now_ms);
        return;
    }

    /* AUTH gate: unauthenticated sessions may only run AUTH and QUIT */
    if (!s->authed && name != NULL && cmd_id != CMD_AUTH &&
        cmd_id != CMD_QUIT && cmd_id != CMD_RESET && cmd_id != CMD_HELLO) {
        static const char E[] = "NOAUTH Authentication required.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    /* subscribed mode: only a small command set is allowed */
    if ((s->nsub > 0 || s->nssub > 0 || s->npsub > 0) && name != NULL &&
        cmd_id != CMD_SUBSCRIBE &&
        cmd_id != CMD_UNSUBSCRIBE &&
        cmd_id != CMD_SSUBSCRIBE &&
        cmd_id != CMD_SUNSUBSCRIBE &&
        cmd_id != CMD_PSUBSCRIBE &&
        cmd_id != CMD_PUNSUBSCRIBE &&
        cmd_id != CMD_PING && cmd_id != CMD_QUIT && cmd_id != CMD_RESET &&
        cmd_id != CMD_SHUTDOWN) {
        char lc[32];
        char msg[192];
        size_t i;
        int n;
        for (i = 0; i < nlen && i < sizeof(lc) - 1; i++)
            lc[i] = (name[i] >= 'A' && name[i] <= 'Z')
                        ? (char)(name[i] + ('a' - 'A'))
                        : name[i];
        lc[i] = '\0';
        n = snprintf(msg, sizeof(msg),
                     "ERR Can't execute '%s': only (P)SUBSCRIBE / "
                     "(P)UNSUBSCRIBE / PING / QUIT / SHUTDOWN are allowed in "
                     "this context",
                     lc);
        resp_write_error(out, msg, (size_t)n);
        return;
    }

    /* transaction control commands are never queued */
    if (name != NULL && cmd_id == CMD_MULTI) {
        if (argc != 1) {
            wrong_args(out, "multi");
            return;
        }
        if (s->in_multi) {
            static const char E[] = "ERR MULTI calls can not be nested";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        s->in_multi = 1;
        s->multi_error = 0;
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (name != NULL && cmd_id == CMD_EXEC) {
        if (argc != 1) {
            wrong_args(out, "exec");
            return;
        }
        if (!s->in_multi) {
            static const char E[] = "ERR EXEC without MULTI";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        exec_transaction(s, out, now_ms);
        return;
    }
    if (name != NULL && cmd_id == CMD_DISCARD) {
        if (argc != 1) {
            wrong_args(out, "discard");
            return;
        }
        if (!s->in_multi) {
            static const char E[] = "ERR DISCARD without MULTI";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        session_queue_clear(s);
        session_watch_clear(s);
        s->in_multi = 0;
        s->multi_error = 0;
        resp_write_simple_string(out, "OK", 2);
        return;
    }
    if (name != NULL && cmd_id == CMD_RESET) {
        if (argc != 1) {
            wrong_args(out, "reset");
            return;
        }
        session_queue_clear(s);
        session_watch_clear(s);
        s->in_multi = 0;
        s->multi_error = 0;
        s->read_only = 0;
        s->asking = 0;
        s->db_index = 0;
        if (s->sel_fn != NULL)
            s->d = s->sel_fn(s->sel_ctx, 0);
        if (s->reset_hook != NULL)
            s->reset_hook(s->reset_ctx, s);
        resp_write_simple_string(out, "RESET", 5);
        return;
    }
    if (name != NULL && cmd_id == CMD_WATCH) {
        size_t i;
        if (argc < 2) {
            wrong_args(out, "watch");
            return;
        }
        if (s->in_multi) {
            static const char E[] = "ERR WATCH inside MULTI is not allowed";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        if (crossslot_reject(s->d, out, argv, argc))
            return;
        if (!cluster_check_ownership(s, argv, argc, out, now_ms))
            return;
        for (i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
        session_watch_add(s, k, kl, db_key_version(s->d, k, kl),
                          s->d->flush_epoch, s->db_index);
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (s->in_multi && name != NULL) {
        /* inside MULTI: validate and queue (UNWATCH queues like any cmd) */
        if (queue_validate(s, argv, argc, out) != 0)
            return;
        if (session_queue_push(s, argv, argc) != 0) {
            static const char err[] = "ERR command is too large";
            resp_write_error(out, err, sizeof(err) - 1);
            return;
        }
        resp_write_simple_string(out, "QUEUED", 6);
        return;
    }

    {
        uint64_t t0 = 0;
#ifdef DDUP_NO_CMDSTATS
        if (s->slowlog_add != NULL)
            t0 = pal_now_us();
        command_dispatch(s, argv, argc, out, now_ms);
#else
        t0 = pal_now_us();
        command_dispatch(s, argv, argc, out, now_ms);
        /* commandstats: count every dispatched command (queueing/blocked
         * paths above do not reach here) */
        if (cmd_id != CMD_ID_UNKNOWN && cmd_id < CMD_STATS_SLOTS) {
            s->d->cmd_calls[cmd_id]++;
            s->d->cmd_usecs[cmd_id] += pal_now_us() - t0;
        }
#endif
        if (s->slowlog_add != NULL)
            s->slowlog_add(s->slowlog_ctx, argv, argc, pal_now_us() - t0,
                           now_ms);
    }
    if (s->d->tier_io_error) {
        tier_io_reply(out);
        return;
    }
    /* AOF: log the original command if it mutated the db (script effects
     * were already logged individually by redis.call) */
    if (s->d->dirty != dirty_before && s->aof_log != NULL && !s->aof_skip)
        s->aof_log(s->aof_ctx, s->db_index, argv, argc, s->raw_cmd,
                   s->raw_cmd_len);
    s->aof_skip = 0;
    /* allkeys-lru eviction runs after write commands (and CONFIG SET) */
    if (s->d->maxmemory_policy == DB_POLICY_ALLKEYS_LRU)
        db_evict_if_needed(s->d, now_ms);
    return;

bad_type:
    resp_write_error(out, "ERR invalid argument type", 24);
}

void session_execute(session *s, const resp_value *argv, size_t argc,
                     resp_buf *out)
{
    session_execute_at(s, argv, argc, out, pal_wall_ms());
}

void command_execute_at(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    session s;
    session_init(&s, d);
    session_execute_at(&s, argv, argc, out, now_ms);
    session_release(&s);
}

void command_execute(db *d, const resp_value *argv, size_t argc, resp_buf *out)
{
    command_execute_at(d, argv, argc, out, pal_wall_ms());
}
