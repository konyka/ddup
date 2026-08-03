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

#include "pal/pal_time.h"

void db_init(db *d)
{
    rh_init(&d->table);
    rh_init(&d->expires);
    d->expired_keys = 0;
    d->evicted_keys = 0;
    d->used_memory = 0;
    d->maxmemory = 0;
    d->maxmemory_policy = DB_POLICY_ALLKEYS_LRU;
    d->rng_state = 0x9E3779B9u; /* nonzero xorshift seed */
}

void db_destroy(db *d)
{
    rh_destroy(&d->table);
    rh_destroy(&d->expires);
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
    if (!rh_get(&d->expires, key, klen, &v, &vl) || vl != 8)
        return 0;
    if (get_u64(v) > now_ms)
        return 0;
    rh_del(&d->expires, key, klen);
    d->used_memory -= entry_bytes(klen, 8);
    if (rh_get(&d->table, key, klen, &v, &vl)) {
        rh_del(&d->table, key, klen);
        d->used_memory -= entry_bytes(klen, vl);
    }
    d->expired_keys++;
    return 1;
}

/* Expire-aware lookup; a hit also refreshes the LRU clock. */
static int db_get(db *d, const char *key, size_t klen, const char **val,
                  size_t *vlen, uint64_t now_ms)
{
    db_expire_if_needed(d, key, klen, now_ms);
    if (!rh_get(&d->table, key, klen, val, vlen))
        return 0;
    rh_touch(&d->table, key, klen, lru_clock(now_ms));
    return 1;
}

/* Overwrite a value; clears any expiry; refreshes the LRU clock. */
static void db_set_kv(db *d, const char *key, size_t klen, const char *val,
                      size_t vlen, uint64_t now_ms)
{
    const char *old;
    size_t oldl;
    if (rh_get(&d->expires, key, klen, &old, &oldl)) {
        rh_del(&d->expires, key, klen);
        d->used_memory -= entry_bytes(klen, 8);
    }
    if (rh_get(&d->table, key, klen, &old, &oldl))
        d->used_memory -= entry_bytes(klen, oldl);
    rh_set(&d->table, key, klen, val, vlen);
    rh_touch(&d->table, key, klen, lru_clock(now_ms));
    d->used_memory += entry_bytes(klen, vlen);
}

/* Delete key and expiry. Returns 1 if the key existed. */
static int db_del_kv(db *d, const char *key, size_t klen)
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
        rh_del(&d->table, key, klen);
        d->used_memory -= entry_bytes(klen, oldl);
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

static void command_dispatch(db *d, const resp_value *argv, size_t argc,
                             resp_buf *out, uint64_t now_ms)
{
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

    if (ci_equal(name, nlen, "PING")) {
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

    if (ci_equal(name, nlen, "ECHO")) {
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

    if (ci_equal(name, nlen, "GET")) {
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
        if (db_get(d, k, kl, &v, &vl, now_ms))
            resp_write_bulk(out, v, vl);
        else
            resp_write_bulk(out, NULL, 0);
        return;
    }

    if (ci_equal(name, nlen, "SET")) {
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
            if ((nx && exists) || (xx && !exists)) {
                resp_write_bulk(out, NULL, 0);
                return;
            }
        }
        if (oom_blocked(d, out))
            return;
        db_set_kv(d, k, kl, v, vl, now_ms);
        if (has_ttl)
            db_set_expiry(d, k, kl, now_ms + ttl_ms);
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (ci_equal(name, nlen, "DEL") || ci_equal(name, nlen, "UNLINK")) {
        if (argc < 2) {
            wrong_args(out, "del");
            return;
        }
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

    if (ci_equal(name, nlen, "EXISTS")) {
        if (argc < 2) {
            wrong_args(out, "exists");
            return;
        }
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

    if (ci_equal(name, nlen, "INCR") || ci_equal(name, nlen, "DECR")) {
        if (argc != 2) {
            wrong_args(out, ci_equal(name, nlen, "INCR") ? "incr" : "decr");
            return;
        }
        const char *k;
        size_t kl;
        if (!arg_str(&argv[1], &k, &kl))
            goto bad_type;
        long long delta = ci_equal(name, nlen, "INCR") ? 1 : -1;
        long long cur = 0;
        const char *v;
        size_t vl;
        if (db_get(d, k, kl, &v, &vl, now_ms)) {
            if (!parse_i64(v, vl, &cur)) {
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
        db_set_kv(d, k, kl, num, (size_t)nl, now_ms);
        resp_write_integer(out, cur);
        return;
    }

    if (ci_equal(name, nlen, "APPEND")) {
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
            resp_buf tmp;
            resp_buf_init(&tmp);
            resp_buf_reserve(&tmp, oldl + vl);
            memcpy(tmp.data, old, oldl);
            memcpy(tmp.data + oldl, v, vl);
            tmp.len = oldl + vl;
            db_set_kv(d, k, kl, tmp.data, tmp.len, now_ms);
            resp_write_integer(out, (long long)tmp.len);
            resp_buf_free(&tmp);
        } else {
            db_set_kv(d, k, kl, v, vl, now_ms);
            resp_write_integer(out, (long long)vl);
        }
        return;
    }

    if (ci_equal(name, nlen, "STRLEN")) {
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
        resp_write_integer(
            out, db_get(d, k, kl, &v, &vl, now_ms) ? (long long)vl : 0);
        return;
    }

    if (ci_equal(name, nlen, "MGET")) {
        if (argc < 2) {
            wrong_args(out, "mget");
            return;
        }
        resp_write_array_header(out, argc - 1);
        for (size_t i = 1; i < argc; i++) {
            const char *k;
            size_t kl;
            if (!arg_str(&argv[i], &k, &kl))
                goto bad_type;
            const char *v;
            size_t vl;
            if (db_get(d, k, kl, &v, &vl, now_ms))
                resp_write_bulk(out, v, vl);
            else
                resp_write_bulk(out, NULL, 0);
        }
        return;
    }

    if (ci_equal(name, nlen, "MSET")) {
        if (argc < 3 || argc % 2 == 0) {
            wrong_args(out, "mset");
            return;
        }
        if (oom_blocked(d, out))
            return;
        for (size_t i = 1; i + 1 < argc; i += 2) {
            const char *k, *v;
            size_t kl, vl;
            if (!arg_str(&argv[i], &k, &kl) || !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            db_set_kv(d, k, kl, v, vl, now_ms);
        }
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (ci_equal(name, nlen, "EXPIRE") || ci_equal(name, nlen, "PEXPIRE") ||
        ci_equal(name, nlen, "EXPIREAT") || ci_equal(name, nlen, "PEXPIREAT")) {
        if (argc != 3) {
            wrong_args(out, "expire");
            return;
        }
        int seconds = ci_equal(name, nlen, "EXPIRE") ||
                      ci_equal(name, nlen, "EXPIREAT");
        int absolute = ci_equal(name, nlen, "EXPIREAT") ||
                       ci_equal(name, nlen, "PEXPIREAT");
        cmd_expire(d, argv, out, now_ms, seconds ? 1000 : 1, absolute, "expire");
        return;
    }

    if (ci_equal(name, nlen, "TTL") || ci_equal(name, nlen, "PTTL")) {
        if (argc != 2) {
            wrong_args(out, "ttl");
            return;
        }
        cmd_ttl(d, argv, out, now_ms, ci_equal(name, nlen, "PTTL"));
        return;
    }

    if (ci_equal(name, nlen, "PERSIST")) {
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

    if (ci_equal(name, nlen, "DBSIZE")) {
        if (argc != 1) {
            wrong_args(out, "dbsize");
            return;
        }
        /* O(1) size; may include expired keys not yet collected. */
        resp_write_integer(out, (long long)rh_size(&d->table));
        return;
    }

    if (ci_equal(name, nlen, "FLUSHDB")) {
        if (argc != 1) {
            wrong_args(out, "flushdb");
            return;
        }
        rh_destroy(&d->table);
        rh_destroy(&d->expires);
        rh_init(&d->table);
        rh_init(&d->expires);
        d->used_memory = 0;
        resp_write_simple_string(out, "OK", 2);
        return;
    }

    if (ci_equal(name, nlen, "CONFIG")) {
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

    if (ci_equal(name, nlen, "INFO")) {
        if (argc != 1) {
            wrong_args(out, "info");
            return;
        }
        {
            char human[32];
            char buf[512];
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
                          "dbsize:%llu\r\n",
                          (unsigned long long)d->used_memory, human,
                          (unsigned long long)d->maxmemory,
                          policy_name(d->maxmemory_policy),
                          (unsigned long long)d->expired_keys,
                          (unsigned long long)d->evicted_keys,
                          (unsigned long long)rh_size(&d->table));
            resp_write_bulk(out, buf, (size_t)n2);
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

void command_execute_at(db *d, const resp_value *argv, size_t argc,
                        resp_buf *out, uint64_t now_ms)
{
    command_dispatch(d, argv, argc, out, now_ms);
    /* allkeys-lru eviction runs after write commands (and CONFIG SET) */
    if (d->maxmemory_policy == DB_POLICY_ALLKEYS_LRU)
        db_evict_if_needed(d);
}

void command_execute(db *d, const resp_value *argv, size_t argc, resp_buf *out)
{
    command_execute_at(d, argv, argc, out, pal_wall_ms());
}
