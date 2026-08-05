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
#include "pal/pal_time.h"

#include <math.h>

#include "core/session.h"
#include "core/hashslot.h"
#include "core/snapshot.h"
#include "core/migrate.h"

static void free_obj_cb(const char *key, size_t klen, const char *val,
                        size_t vlen, void *ctx);

void db_init(db *d)
{
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
    strcpy(d->cluster_ip, "0.0.0.0");
    d->cluster_port = 0;
    d->snapshot_path = NULL;
    d->last_save = 0;
    d->rng_state = 0x9E3779B9u; /* nonzero xorshift seed */
}

void db_destroy(db *d)
{
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

void db_touch_key(db *d, const char *key, size_t klen)
{
    const char *v;
    size_t vl;
    uint64_t ver = 0;
    char b[8];
    d->dirty++;
    /* version bumps are only needed while at least one WATCH is active */
    if (d->watch_refs == 0)
        return;
    if (rh_get(&d->keyvers, key, klen, &v, &vl) && vl == 8)
        ver = get_u64(v);
    put_u64(b, ver + 1);
    rh_set(&d->keyvers, key, klen, b, 8);
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
static void db_set_kv(db *d, const char *key, size_t klen, const char *val,
                      size_t vlen, uint64_t now_ms)
{
    const char *old;
    size_t oldl;
    if (rh_get(&d->expires, key, klen, &old, &oldl)) {
        rh_del(&d->expires, key, klen);
        d->used_memory -= entry_bytes(klen, 8);
    }
    if (rh_get(&d->table, key, klen, &old, &oldl)) {
        d->used_memory -= entry_bytes(klen, oldl) + obj_extra_mem(old, oldl);
        obj_free_value(old, oldl);
    }
    rh_set(&d->table, key, klen, val, vlen);
    rh_touch(&d->table, key, klen, lru_clock(now_ms));
    d->used_memory += entry_bytes(klen, vlen) + obj_extra_mem(val, vlen);
    db_touch_key(d, key, klen);
}

/* Store a string payload (adds the type tag). */
static void db_set_string(db *d, const char *key, size_t klen, const char *val,
                          size_t vlen, uint64_t now_ms)
{
    char stackbuf[256];
    char *buf = stackbuf;
    if (vlen + 1 > sizeof(stackbuf))
        buf = (char *)malloc(vlen + 1);
    buf[0] = (char)DDUP_OBJ_STRING;
    memcpy(buf + 1, val, vlen);
    db_set_kv(d, key, klen, buf, vlen + 1, now_ms);
    if (buf != stackbuf)
        free(buf);
}

/* Delete key and expiry (and any owned object). Returns 1 if existed. */
int db_del_kv(db *d, const char *key, size_t klen)
{
    const char *old;
    size_t oldl;
    int existed;
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

static void db_set_expiry(db *d, const char *key, size_t klen, uint64_t when_ms)
{
    char b[8];
    const char *old;
    size_t oldl;
    put_u64(b, when_ms);
    if (!rh_get(&d->expires, key, klen, &old, &oldl))
        d->used_memory += entry_bytes(klen, 8);
    rh_set(&d->expires, key, klen, b, 8);
}

void db_install_blob(db *d, const char *key, size_t klen, const char *blob,
                     size_t bloblen, uint64_t now_ms)
{
    db_set_kv(d, key, klen, blob, bloblen, now_ms);
}

void db_install_expiry(db *d, const char *key, size_t klen, uint64_t when_ms)
{
    db_set_expiry(d, key, klen, when_ms);
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
        db_set_kv(d, k, kl, blob, 9, now);
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
        db_set_kv(d, k, kl, blob, 9, now);
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
        db_set_kv(d, k, kl, blob, 9, now);
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
        db_set_kv(d, k, kl, blob, 9, now);
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
    db_set_expiry(d, k, kl, (uint64_t)exp);
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
        cmd_id == CMD_SINTER || cmd_id == CMD_SUNION ||
        cmd_id == CMD_SDIFF || cmd_id == CMD_WATCH) {
        for (i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_MSET) {
        for (i = 1; i + 1 < argc; i += 2) {
            const char *k;
            size_t kl;
            if (arg_str(&argv[i], &k, &kl) &&
                !slot_accum(k, kl, have, slot))
                return 0;
        }
        return 1;
    }
    if (cmd_id == CMD_SMOVE) {
        for (i = 1; i < argc && i < 3; i++) {
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
    case CMD_PSYNC:      case CMD_AUTH:
    case CMD_SUBSCRIBE:  case CMD_UNSUBSCRIBE:case CMD_PUBLISH:
    case CMD_QUIT:       case CMD_MULTI:      case CMD_EXEC:
    case CMD_DISCARD:    case CMD_UNWATCH:    case CMD_DBSIZE:
    case CMD_FLUSHDB:    case CMD_CLUSTER:    case CMD_PERSIST:
    case CMD_MIGRATE:    case CMD_ASKING:
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
    if (cluster_keyless_id(cmd_id))
        return 1;
    if (cmd_id == CMD_MGET || cmd_id == CMD_DEL ||
        cmd_id == CMD_UNLINK || cmd_id == CMD_EXISTS ||
        cmd_id == CMD_SINTER || cmd_id == CMD_SUNION ||
        cmd_id == CMD_SDIFF || cmd_id == CMD_WATCH) {
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
    if (cmd_id == CMD_SMOVE) {
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
        int nx = 0, xx = 0, has_ttl = 0;
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
                       !has_ttl && i + 1 < argc) {
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
        }
        if (oom_blocked(d, out))
            return;
        db_set_string(d, k, kl, v, vl, now_ms);
        if (has_ttl)
            db_set_expiry(d, k, kl, now_ms + ttl_ms);
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

    if (cmd_id == CMD_INCR || cmd_id == CMD_DECR) {
        if (argc != 2) {
            wrong_args(out, cmd_id == CMD_INCR ? "incr" : "decr");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        long long delta = cmd_id == CMD_INCR ? 1 : -1;
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
        if ((delta > 0 && cur == LLONG_MAX) ||
            (delta < 0 && cur == LLONG_MIN)) {
            resp_write_error(out, ERR_OVERFLOW, sizeof(ERR_OVERFLOW) - 1);
            return;
        }
        cur += delta;
        char num[24];
        int nl = snprintf(num, sizeof(num), "%lld", cur);
        if (oom_blocked(d, out))
            return;
        db_set_string(d, k, kl, num, (size_t)nl, now_ms);
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
            resp_buf_reserve(&tmp, osl + vl);
            memcpy(tmp.data, os, osl);
            memcpy(tmp.data + osl, v, vl);
            tmp.len = osl + vl;
            db_set_string(d, k, kl, tmp.data, tmp.len, now_ms);
            resp_write_integer(out, (long long)tmp.len);
            resp_buf_free(&tmp);
        } else {
            db_set_string(d, k, kl, v, vl, now_ms);
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
            if (!arg_str(&argv[i], &k, &kl) || !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            db_set_string(d, k, kl, v, vl, now_ms);
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
        rh_each(&d->table, free_obj_cb, NULL);
        rh_destroy(&d->table);
        rh_destroy(&d->expires);
        rh_destroy(&d->keyvers);
        rh_init(&d->table);
        rh_init(&d->expires);
        rh_init(&d->keyvers);
        d->used_memory = 0;
        d->flush_epoch++; /* invalidates all WATCHes */
        d->dirty++;
        resp_write_simple_string(out, "OK", 2);
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
                d->maxmemory = (uint64_t)mv;
                resp_write_simple_string(out, "OK", 2);
                return;
            }
            if (ci_equal(p, pl, "maxmemory-policy")) {
                if (ci_equal(v, vl2, "allkeys-lru")) {
                    d->maxmemory_policy = DB_POLICY_ALLKEYS_LRU;
                } else if (ci_equal(v, vl2, "noeviction")) {
                    d->maxmemory_policy = DB_POLICY_NOEVICTION;
                } else {
                    static const char E[] =
                        "ERR invalid argument for CONFIG SET "
                        "'maxmemory-policy'";
                    resp_write_error(out, E, sizeof(E) - 1);
                    return;
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
        if (argc != 1) {
            wrong_args(out, "info");
            return;
        }
        {
            char human[32];
            char buf[768];
            int n2;
            human_bytes(d->used_memory, human, sizeof(human));
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
                          "dbsize:%llu\r\n"
                          "# Cluster\r\n"
                          "cluster_enabled:%d\r\n",
                          (unsigned long long)d->used_memory, human,
                          (unsigned long long)d->maxmemory,
                          policy_name(d->maxmemory_policy),
                          (unsigned long long)d->expired_keys,
                          (unsigned long long)d->evicted_keys,
                          (unsigned long long)rh_size(&d->table),
                          d->cluster_enabled);
            if (s->repl != NULL) {
                const repl_info *ri = s->repl;
                n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                               "# Replication\r\n"
                               "role:%s\r\n"
                               "connected_slaves:%llu\r\n"
                               "master_replid:%s\r\n"
                               "master_repl_offset:%llu\r\n",
                               ri->role == SESSION_ROLE_REPLICA ? "slave"
                                                                : "master",
                               (unsigned long long)ri->connected_slaves,
                               ri->role == SESSION_ROLE_REPLICA
                                   ? ri->master_replid
                                   : ri->replid,
                               (unsigned long long)ri->offset);
                if (ri->role == SESSION_ROLE_REPLICA)
                    n2 += snprintf(buf + n2, sizeof(buf) - (size_t)n2,
                                   "master_host:%s\r\n"
                                   "master_port:%u\r\n"
                                   "master_link_status:%s\r\n",
                                   ri->master_host,
                                   (unsigned)ri->master_port,
                                   ri->link_up ? "up" : "down");
            }
            resp_write_bulk(out, buf, (size_t)n2);
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
        if (oom_blocked(d, out))
            return;
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
            added += obj_hash_set(h, f, fl, v, vl);
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
        if (rh_size(&h->fields) == 0)
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
        resp_write_integer(out, rc == 1 ? (long long)rh_size(&h->fields) : 0);
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
                                        ? rh_size(&h->fields) * 2
                                        : rh_size(&h->fields));
        rh_each(&h->fields, hdump_cb, &ctx);
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
            obj_hash_set(h, f, fl, num, (size_t)nl);
            mem_sync(d, k, kl, before, obj_hash_mem(h));
        }
        resp_write_integer(out, cur);
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
            obj_hash_set(h, f, fl, v, vl);
            mem_sync(d, k, kl, before, obj_hash_mem(h));
        }
        resp_write_integer(out, 1);
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
        if (oom_blocked(d, out))
            return;
        obj_list *l;
        int rc = get_list(d, out, k, kl, !only_if_exists, now_ms, &l);
        if (rc < 0)
            return;
        if (rc == 0) { /* LPUSHX/RPUSHX on missing key */
            resp_write_integer(out, 0);
            return;
        }
        {
            uint64_t before = obj_list_mem(l);
            for (size_t i = 2; i < argc; i++) {
                const char *v;
                size_t vl;
                if (!arg_str(&argv[i], &v, &vl))
                    goto bad_type;
                obj_list_push(l, left, v, vl);
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
        }
        resp_write_integer(out, (long long)l->len);
        return;
    }

    if (cmd_id == CMD_LPOP || cmd_id == CMD_RPOP) {
        int left = cmd_id == CMD_LPOP;
        if (argc != 2) {
            wrong_args(out, left ? "lpop" : "rpop");
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
            if (l->len == 0)
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
        resp_write_integer(out, rc == 1 ? (long long)l->len : 0);
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
            long long len = (long long)l->len;
            long long i;
            list_node *n;
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
            n = obj_list_at(l, (size_t)start);
            for (i = start; i <= stop && n != NULL; i++, n = n->next)
                resp_write_bulk(out, n->data, n->len);
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
            idx += (long long)l->len;
        {
            list_node *n =
                idx >= 0 ? obj_list_at(l, (size_t)idx) : NULL;
            if (n != NULL)
                resp_write_bulk(out, n->data, n->len);
            else
                resp_write_bulk(out, NULL, 0);
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
            idx += (long long)l->len;
        if (idx < 0) {
            static const char E_RANGE[] = "ERR index out of range";
            resp_write_error(out, E_RANGE, sizeof(E_RANGE) - 1);
            return;
        }
        {
            uint64_t before = obj_list_mem(l);
            if (!obj_list_set_at(l, (size_t)idx, v, vl)) {
                static const char E_RANGE[] = "ERR index out of range";
                resp_write_error(out, E_RANGE, sizeof(E_RANGE) - 1);
                return;
            }
            mem_sync(d, k, kl, before, obj_list_mem(l));
        }
        resp_write_simple_string(out, "OK", 2);
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
        if (oom_blocked(d, out))
            return;
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
            added += obj_set_add(s, m, ml);
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
        if (rh_size(&s->members) == 0)
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
                           rc == 1 ? (long long)rh_size(&s->members) : 0);
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
            resp_write_array_header(out, rh_size(&s->members));
            rh_each(&s->members, hdump_cb, &ctx);
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
            collect_ctx cc = {0};
            size_t idx;
            if (rc == 0) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            rh_each(&s->members, collect_cb, &cc);
            idx = (size_t)(db_rand(d) % (uint32_t)cc.n);
            if (pop) {
                char *copy = (char *)malloc(cc.lens[idx] + 1);
                uint64_t before;
                memcpy(copy, cc.keys[idx], cc.lens[idx]);
                before = obj_set_mem(s);
                obj_set_rem(s, copy, cc.lens[idx]);
                mem_sync(d, k, kl, before, obj_set_mem(s));
                if (rh_size(&s->members) == 0)
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
        /* count form: array reply */
        if (rc == 0 || (count == 0 && pop) || (count == 0 && !pop)) {
            resp_write_array_header(out, 0);
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
                if (rh_size(&s->members) == 0)
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
        {
            int rcd = get_set(d, out, dk, dkl, 0, now_ms, &dst);
            if (rcd < 0)
                return;
        }
        if (oom_blocked(d, out))
            return;
        {
            uint64_t before = obj_set_mem(src);
            obj_set_rem(src, m, ml);
            mem_sync(d, sk, skl, before, obj_set_mem(src));
        }
        if (rh_size(&src->members) == 0)
            db_del_kv(d, sk, skl);
        get_set(d, out, dk, dkl, 1, now_ms, &dst);
        {
            uint64_t before = obj_set_mem(dst);
            obj_set_add(dst, m, ml);
            mem_sync(d, dk, dkl, before, obj_set_mem(dst));
        }
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
            size_t nkeys = argc - 1;
            obj_set **sets =
                (obj_set **)malloc(nkeys * sizeof(*sets));
            rh_table result;
            size_t i;
            rh_init(&result);
            /* resolve operands with type checks first */
            for (i = 0; i < nkeys; i++) {
                const char *k;
                size_t kl;
                obj_set *s = NULL;
                int rc;
                if (!arg_str(&argv[i + 1], &k, &kl))
                    goto bad_type;
                rc = get_set(d, out, k, kl, 0, now_ms, &s);
                if (rc < 0) {
                    rh_destroy(&result);
                    free(sets);
                    return;
                }
                sets[i] = rc == 1 ? s : NULL;
            }
            if (sunion) {
                for (i = 0; i < nkeys; i++)
                    if (sets[i] != NULL)
                        rh_each(&sets[i]->members, set_union_cb, &result);
            } else if (sets[0] != NULL) {
                setop_ctx ctx;
                ctx.sets = sets;
                ctx.n = nkeys;
                ctx.result = &result;
                ctx.inter = inter;
                rh_each(&sets[0]->members, setop_cb, &ctx);
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
            free(sets);
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
        double *scores = (double *)malloc(pairs * sizeof(double));
        size_t j;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
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
                added += obj_zset_add(z, m, ml, scores[j]);
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
        resp_write_integer(out, rc == 1 ? (long long)rh_size(&z->dict) : 0);
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
            obj_zset_add(z, m, ml, cur);
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
        if (rh_size(&z->dict) == 0)
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
            long long len = (long long)z->sl->length;
            long long i;
            zsl_node *n;
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
                n = zsl_at(z->sl, (size_t)start);
                for (i = start; i <= stop && n != NULL;
                     i++, n = n->forward[0]) {
                    resp_write_bulk(out, n->member, n->mlen);
                    if (withscores) {
                        char num[40];
                        int nl = fmt_score(num, sizeof(num), n->score);
                        resp_write_bulk(out, num, (size_t)nl);
                    }
                }
            } else {
                /* reversed index p == forward index len-1-p */
                n = zsl_at(z->sl, (size_t)(len - 1 - start));
                for (i = start; i <= stop && n != NULL;
                     i++, n = n->backward) {
                    resp_write_bulk(out, n->member, n->mlen);
                    if (withscores) {
                        char num[40];
                        int nl = fmt_score(num, sizeof(num), n->score);
                        resp_write_bulk(out, num, (size_t)nl);
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
            long rank = zsl_rank(z->sl, sc, m, ml);
            if (rank < 0) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
            resp_write_integer(out, rev ? (long long)z->sl->length - 1 - rank
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
                               ? (long long)zsl_count_in_range(z->sl, &spec)
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
            zsl_node *n;
            c = 0;
            n = zsl_first_in_range(z->sl, &spec);
            while (n != NULL && (spec.maxex ? n->score < spec.max
                                            : n->score <= spec.max)) {
                if (c >= off)
                    emitted++;
                c++;
                n = n->forward[0];
            }
            if (cnt >= 0 && emitted > cnt)
                emitted = cnt;
            resp_write_array_header(out,
                                    (size_t)emitted * (withscores ? 2u : 1u));
            c = 0;
            n = zsl_first_in_range(z->sl, &spec);
            while (n != NULL && (spec.maxex ? n->score < spec.max
                                            : n->score <= spec.max)) {
                if (c >= off && (cnt < 0 || c - off < cnt)) {
                    resp_write_bulk(out, n->member, n->mlen);
                    if (withscores) {
                        char num[40];
                        int nl = fmt_score(num, sizeof(num), n->score);
                        resp_write_bulk(out, num, (size_t)nl);
                    }
                }
                c++;
                n = n->forward[0];
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
            /* collect node views first, then delete (views stay valid:
             * deleting a node frees only that node) */
            zsl_node **nodes = NULL;
            size_t nn = 0, cap = 0;
            zsl_node *n = zsl_first_in_range(z->sl, &spec);
            long long removed = 0;
            uint64_t before = obj_zset_mem(z);
            size_t i;
            while (n != NULL &&
                   (spec.maxex ? n->score < spec.max
                               : n->score <= spec.max)) {
                if (nn == cap) {
                    size_t ncap = cap == 0 ? 16 : cap * 2;
                    zsl_node **nn2 = (zsl_node **)realloc(
                        nodes, ncap * sizeof(*nn2));
                    if (nn2 == NULL) {
                        free(nodes);
                        fprintf(stderr, "ddup: out of memory\n");
                        exit(1);
                    }
                    nodes = nn2;
                    cap = ncap;
                }
                nodes[nn++] = n;
                n = n->forward[0];
            }
            for (i = 0; i < nn; i++)
                removed += obj_zset_rem(z, nodes[i]->member, nodes[i]->mlen);
            free(nodes);
            mem_sync(d, k, kl, before, obj_zset_mem(z));
            if (rh_size(&z->dict) == 0)
                db_del_kv(d, k, kl);
            resp_write_integer(out, removed);
        }
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
            if (s->subscribe != NULL) {
                write_sub_reply(out, "subscribe", 9, ch, cl,
                                (long long)s->subscribe(s->ps_ctx, s, ch, cl));
            } else {
                s->nsub++; /* no registry (stack session) */
                write_sub_reply(out, "subscribe", 9, ch, cl,
                                (long long)s->nsub);
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

    if (cmd_id == CMD_QUIT) {
        if (argc != 1) {
            wrong_args(out, "quit");
            return;
        }
        /* acknowledged; the server does not close the connection yet
         * (documented simplification) */
        resp_write_simple_string(out, "OK", 2);
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
        if (snapshot_save(d, d->snapshot_path) != 0) {
            static const char E[] = "ERR snapshot save failed";
            resp_write_error(out, E, sizeof(E) - 1);
            return;
        }
        d->last_save = now_ms / 1000;
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
        s->psync_hook(s->psync_ctx, s, rid, rl, poff);
        return;
    }

    if (cmd_id == CMD_REPLICAOF) {
        if (argc != 3) {
            wrong_args(out, "replicaof");
            return;
        }
        const char *host, *portv;
        size_t hl, pl;
        if (!arg_str(&argv[1], &host, &hl) || !arg_str(&argv[2], &portv, &pl))
            goto bad_type;
        if (s->replicaof_hook == NULL) {
            resp_write_error(out,
                             "ERR replicaof not supported in this context",
                             45);
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
                    if (d->nodes[i].flags & CLUSTER_NODE_DISCONNECTED) {
                        for (sl2 = 0; sl2 < 16384 && !fail_slots; sl2++)
                            if (cluster_slots_get(d->nodes[i].slots,
                                                  (uint32_t)sl2))
                                fail_slots = 1;
                        continue;
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

/* Unified command metadata. IDs are assigned in this table order and must
 * remain stable (used for AOF/replication/serialization). */
typedef struct cmd_entry {
    const char *name;
    uint16_t    id;
    int         min_argc;
    int         max_argc;   /* -1 = unbounded */
    int         parity;     /* 0 any, 1 odd, 2 even */
    uint8_t     flags;
} cmd_entry;

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
    {"lpop", CMD_LPOP, 2, 2, 0, CMD_WRITE},
    {"rpop", CMD_RPOP, 2, 2, 0, CMD_WRITE},
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
    {"auth", CMD_AUTH, 2, 3, 0, 0},
    {"quit", CMD_QUIT, 1, 1, 0, 0},
    {"sync", CMD_SYNC, 1, 1, 0, 0},
    {"psync", CMD_PSYNC, 3, 3, 0, 0},
    {"replicaof", CMD_REPLICAOF, 3, 3, 0, 0},
    {"save", CMD_SAVE, 1, 1, 0, 0},
    {"lastsave", CMD_LASTSAVE, 1, 1, 0, 0},
    {"shutdown", CMD_SHUTDOWN, 1, 1, 0, 0},
    {"cluster", CMD_CLUSTER, 2, -1, 0, 0},
};

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
            if (w->epoch != s->d->flush_epoch ||
                w->version != db_key_version(s->d, w->key, w->klen)) {
                /* dirty watch: null array, nothing applied */
                static const char null_arr[] = "*-1\r\n";
                resp_buf_reserve(out, sizeof(null_arr) - 1);
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
        for (i = 0; i < s->queue_len; i++)
            command_dispatch(s, s->queue[i].argv, s->queue[i].argc, out,
                             now_ms);
        /* AOF: log each applied command individually (no MULTI wrapper) */
        if (s->d->dirty != dirty_before && s->aof_log != NULL)
            for (i = 0; i < s->queue_len; i++)
                s->aof_log(s->aof_ctx, s->queue[i].argv, s->queue[i].argc);
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

    /* AUTH gate: unauthenticated sessions may only run AUTH and QUIT */
    if (!s->authed && name != NULL && cmd_id != CMD_AUTH &&
        cmd_id != CMD_QUIT) {
        static const char E[] = "NOAUTH Authentication required.";
        resp_write_error(out, E, sizeof(E) - 1);
        return;
    }

    /* subscribed mode: only a small command set is allowed */
    if (s->nsub > 0 && name != NULL &&
        cmd_id != CMD_SUBSCRIBE &&
        cmd_id != CMD_UNSUBSCRIBE &&
        !ci_equal(name, nlen, "PSUBSCRIBE") &&
        !ci_equal(name, nlen, "PUNSUBSCRIBE") &&
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
                              s->d->flush_epoch);
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (s->in_multi && name != NULL) {
        /* inside MULTI: validate and queue (UNWATCH queues like any cmd) */
        if (queue_validate(s, argv, argc, out) != 0)
            return;
        session_queue_push(s, argv, argc);
        resp_write_simple_string(out, "QUEUED", 6);
        return;
    }

    command_dispatch(s, argv, argc, out, now_ms);
    /* AOF: log the original command if it mutated the db */
    if (s->d->dirty != dirty_before && s->aof_log != NULL)
        s->aof_log(s->aof_ctx, argv, argc);
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
