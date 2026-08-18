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
    d->lua_state = NULL;
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
    return rh_get_touch(&d->table, key, klen, val, vlen, lru_clock(now_ms));
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

/* Sample up to DB_EVICT_SAMPLES keys and evict the one with the oldest
 * 24-bit LRU clock. Returns 1 if a key was evicted. */
static int db_evict_one(db *d)
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

    /* the sampled views dangle once we delete: copy the victim key first */
    {
        char stackbuf[256];
        char *kb = stackbuf;
        size_t kl = cand_klen[oldest];
        if (kl > sizeof(stackbuf))
            kb = (char *)malloc(kl);
        memcpy(kb, cand_key[oldest], kl);
        db_del_kv(d, kb, kl);
        if (kb != stackbuf)
            free(kb);
    }
    d->evicted_keys++;
    return 1;
}

static void db_evict_if_needed(db *d)
{
    if (d->maxmemory == 0)
        return;
    while (d->used_memory > d->maxmemory && rh_size(&d->table) > 0) {
        if (!db_evict_one(d))
            break;
    }
}

static const char OOM_MSG[] =
    "OOM command not allowed when used memory > 'maxmemory'.";

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
        resp_write_error(out, E, sizeof(E) - 1);
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
        resp_write_error(out, E, sizeof(E) - 1);
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
                  "# Stats\r\n"
                  "expired_keys:%llu\r\n"
                  "evicted_keys:%llu\r\n"
                  "# Keyspace\r\n"
                  "dbsize:%llu\r\n",
                  (unsigned long long)st->used_memory, human,
                  (unsigned long long)home->maxmemory,
                  policy_name(home->maxmemory_policy),
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
static void cmd_expire(db *d, const resp_value *argv, resp_buf *out,
                       uint64_t now, long long scale, int absolute,
                       const char *cmdname)
{
    const char *k, *t;
    size_t kl, tl;
    long long tv, base, exp;
    const char *v;
    size_t vl;

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
    if (!db_get(d, k, kl, &v, &vl, now)) {
        resp_write_integer(out, 0);
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
        cmd_id == CMD_COPY) {
        for (i = 1; i < argc && i < 3; i++) {
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
        cmd_id == CMD_ZMPOP) {
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
    case CMD_READWRITE:
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
        cmd_id == CMD_COPY) {
        for (i = 1; i < argc && i < 3; i++) {
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
        cmd_id == CMD_ZMPOP) {
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

    /* replicas are read-only for client writes (replication link bypasses) */
    if (s->role != NULL && *s->role == SESSION_ROLE_REPLICA &&
        !s->repl_link && is_write_command(name, nlen)) {
        static const char E[] =
            "READONLY You can't write against a read only replica.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    /* cluster mode: ownership enforcement (-MOVED / -CLUSTERDOWN / -ASK) */
    if (!cluster_check_ownership(s, argv, argc, out, now_ms))
        return;

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

    if (cmd_id == CMD_RESTORE) {
        if (argc != 4 && argc != 5) {
            wrong_args(out, "restore");
            return;
        }
        const char *k, *t, *p;
        size_t kl, tl, pl;
        long long ttl;
        int replace = 0;
        int rc;
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
        if (argc == 5) {
            const char *o;
            size_t ol;
            if (!arg_str(&argv[4], &o, &ol))
                goto bad_type;
            if (!ci_equal(o, ol, "REPLACE")) {
                resp_write_error(out, ERR_SYNTAX, sizeof(ERR_SYNTAX) - 1);
                return;
            }
            replace = 1;
        }
        if (oom_blocked(d, out))
            return;
        rc = snapshot_restore_key(d, k, kl, p, pl,
                                  ttl > 0 ? now_ms + (uint64_t)ttl : 0,
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

    if (cmd_id == CMD_EVAL || cmd_id == CMD_EVALSHA) {
        int is_sha = cmd_id == CMD_EVALSHA;
        long long numkeys;
        const char *kv, *nv;
        size_t kvl, nvl;
        char sha[41];
        if (argc < 3) {
            wrong_args(out, is_sha ? "evalsha" : "eval");
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
        script_exec(s, sha, argv + 3, (size_t)numkeys,
                    argc - 3 - (size_t)numkeys, out, now_ms);
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
        if (argc != 3) {
            wrong_args(out, "expire");
            return;
        }
        int seconds = cmd_id == CMD_EXPIRE ||
                      cmd_id == CMD_EXPIREAT;
        int absolute = cmd_id == CMD_EXPIREAT ||
                       cmd_id == CMD_PEXPIREAT;
        cmd_expire(d, argv, out, now_ms, seconds ? 1000 : 1, absolute, "expire");
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
        if (argc != 3) {
            wrong_args(out, "object");
            return;
        }
        if (!arg_str(&argv[1], &sub, &subl) || !arg_str(&argv[2], &k, &kl))
            goto bad_type;
        if (!ci_equal(sub, subl, "ENCODING")) {
            resp_write_error(out, "ERR unknown OBJECT subcommand", 29);
            return;
        }
        if (!db_get(d, k, kl, &v, &vl, now_ms)) {
            resp_write_bulk(out, NULL, 0);
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

    if (cmd_id == CMD_CONFIG) {
        if (argc < 3) {
            wrong_args(out, "config");
            return;
        }
        const char *sub;
        size_t sl;
        if (!arg_str(&argv[1], &sub, &sl))
            goto bad_type;
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
            } else {
                resp_write_array_header(out, 0);
            }
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
                int set_rc = obj_hash_set(h, f, fl, v, vl);
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
        if (rc == 1 && obj_hash_get(h, f, fl, &v, &vl))
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
                           rc == 1 && obj_hash_get(h, f, fl, &v, &vl) ? 1 : 0);
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
        resp_write_integer(out, rc == 1 ? (long long)obj_hash_len(h) : 0);
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
        resp_write_array_header(out, cmd_id == CMD_HGETALL
                                        ? obj_hash_len(h) * 2
                                        : obj_hash_len(h));
        obj_hash_each(h, hdump_cb, &ctx);
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
            if (rc == 1 && obj_hash_get(h, f, fl, &v, &vl))
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
            if (obj_hash_get(h, f, fl, &v, &vl) && !parse_i64(v, vl, &cur)) {
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
            if (obj_hash_set(h, f, fl, num, (size_t)nl) < 0) {
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
            if (obj_hash_get(h, f, fl, &v, &vl) && !parse_ld(v, vl, &cur)) {
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
            if (obj_hash_set(h, f, fl, buf, (size_t)nl) < 0) {
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
            if (obj_hash_get(h, f, fl, &old, &oldl)) {
                resp_write_integer(out, 0);
                return;
            }
        }
        {
            uint64_t before = obj_hash_mem(h);
            if (obj_hash_set(h, f, fl, v, vl) < 0) {
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
        if (argc < 4 || argc % 2 != 0) {
            wrong_args(out, "zadd");
            return;
        }
        const char *k;
        size_t kl;
        size_t pairs = (argc - 2) / 2;
        double *scores;
        size_t j;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        if (!storage_key_ok(kl)) {
            storage_length_error(out);
            return;
        }
        scores = (double *)malloc(pairs * sizeof(double));
        for (j = 0; j < pairs; j++) {
            const char *sv;
            size_t svl;
            if (!arg_str(&argv[2 + 2 * j], &sv, &svl))
                goto bad_type;
            if (!parse_double(sv, svl, &scores[j])) {
                free(scores);
                resp_write_error(out, ERR_NOT_FLOAT, sizeof(ERR_NOT_FLOAT) - 1);
                return;
            }
        }
        if (oom_blocked(d, out)) {
            free(scores);
            return;
        }
        for (j = 0; j < pairs; j++) {
            const char *m;
            size_t ml;
            if (!arg_str(&argv[3 + 2 * j], &m, &ml)) {
                free(scores);
                goto bad_type;
            }
            if (!storage_key_ok(ml)) {
                free(scores);
                storage_length_error(out);
                return;
            }
        }
        {
            obj_zset *z;
            long long added = 0;
            uint64_t before;
            if (get_zset(d, out, k, kl, 1, now_ms, &z) <= 0) {
                free(scores);
                return;
            }
            before = obj_zset_mem(z);
            for (j = 0; j < pairs; j++) {
                const char *m;
                size_t ml;
                if (!arg_str(&argv[3 + 2 * j], &m, &ml))
                    goto bad_type;
                {
                    int add_rc = obj_zset_add(z, m, ml, scores[j]);
                    if (add_rc < 0) {
                        free(scores);
                        storage_length_error(out);
                        return;
                    }
                    added += add_rc;
                }
            }
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            free(scores);
            resp_write_integer(out, added);
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
        if (!d->cluster_enabled) {
            static const char E[] =
                "ERR This instance has cluster support disabled";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        {
            const char *sub;
            size_t sl;
            if (argc < 2 || !arg_str(&argv[1], &sub, &sl))
                goto bad_type;
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
    {"restore", CMD_RESTORE, 4, 5, 0, CMD_WRITE},
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
    {"expire", CMD_EXPIRE, 3, 3, 0, CMD_WRITE},
    {"pexpire", CMD_PEXPIRE, 3, 3, 0, CMD_WRITE},
    {"expireat", CMD_EXPIREAT, 3, 3, 0, CMD_WRITE},
    {"pexpireat", CMD_PEXPIREAT, 3, 3, 0, CMD_WRITE},
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
    {"zadd", CMD_ZADD, 4, -1, 2, CMD_WRITE},
    {"zscore", CMD_ZSCORE, 3, 3, 0, 0},
    {"zcard", CMD_ZCARD, 2, 2, 0, 0},
    {"zincrby", CMD_ZINCRBY, 4, 4, 0, CMD_WRITE},
    {"zrem", CMD_ZREM, 3, -1, 0, CMD_WRITE},
    {"zrange", CMD_ZRANGE, 4, 5, 0, 0},
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
    {"sintercard", CMD_SINTERCARD, 3, -1, 0, 0},
    {"sinterstore", CMD_SINTERSTORE, 3, -1, 0, CMD_WRITE},
    {"sunionstore", CMD_SUNIONSTORE, 3, -1, 0, CMD_WRITE},
    {"sdiffstore", CMD_SDIFFSTORE, 3, -1, 0, CMD_WRITE},
    {"zpopmin", CMD_ZPOPMIN, 2, 3, 0, CMD_WRITE},
    {"zpopmax", CMD_ZPOPMAX, 2, 3, 0, CMD_WRITE},
    {"zremrangebyrank", CMD_ZREMRANGEBYRANK, 4, 4, 0, CMD_WRITE},
    {"zmscore", CMD_ZMSCORE, 3, -1, 0, 0},
    {"zrandmember", CMD_ZRANDMEMBER, 2, 4, 0, 0},
    {"zrangebylex", CMD_ZRANGEBYLEX, 4, -1, 0, 0},
    {"zrevrangebylex", CMD_ZREVRANGEBYLEX, 4, -1, 0, 0},
    {"zremrangebylex", CMD_ZREMRANGEBYLEX, 4, 4, 0, CMD_WRITE},
    {"psubscribe", CMD_PSUBSCRIBE, 2, -1, 0, 0},
    {"punsubscribe", CMD_PUNSUBSCRIBE, 1, -1, 0, 0},
    {"copy", CMD_COPY, 3, -1, 0, CMD_WRITE},
    {"object", CMD_OBJECT, 3, 3, 0, 0},
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

#define CMD_HASH_SIZE 256

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
        db_evict_if_needed(s->d);
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
            resp_write_bulk(out, NULL, 0);
        }
        s->d->cmd_calls[CMD_GET]++;
        if (s->d->maxmemory_policy == DB_POLICY_ALLKEYS_LRU)
            db_evict_if_needed(s->d);
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
            db_evict_if_needed(s->d);
        return;
    }

    /* AUTH gate: unauthenticated sessions may only run AUTH and QUIT */
    if (!s->authed && name != NULL && cmd_id != CMD_AUTH &&
        cmd_id != CMD_QUIT) {
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
        cmd_id != CMD_PING && cmd_id != CMD_QUIT &&
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
#ifdef DDUP_NO_CMDSTATS
        command_dispatch(s, argv, argc, out, now_ms);
#else
        uint64_t t0 = pal_now_us();
        command_dispatch(s, argv, argc, out, now_ms);
        /* commandstats: count every dispatched command (queueing/blocked
         * paths above do not reach here) */
        if (cmd_id != CMD_ID_UNKNOWN && cmd_id < CMD_STATS_SLOTS) {
            s->d->cmd_calls[cmd_id]++;
            s->d->cmd_usecs[cmd_id] += pal_now_us() - t0;
        }
#endif
    }
    /* AOF: log the original command if it mutated the db (script effects
     * were already logged individually by redis.call) */
    if (s->d->dirty != dirty_before && s->aof_log != NULL && !s->aof_skip)
        s->aof_log(s->aof_ctx, s->db_index, argv, argc, s->raw_cmd,
                   s->raw_cmd_len);
    s->aof_skip = 0;
    /* allkeys-lru eviction runs after write commands (and CONFIG SET) */
    if (s->d->maxmemory_policy == DB_POLICY_ALLKEYS_LRU)
        db_evict_if_needed(s->d);
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
