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

int db_expire_if_needed(db *d, const char *key, size_t klen, uint64_t now_ms)
{
    const char *v;
    size_t vl;
    if (!rh_get(&d->expires, key, klen, &v, &vl) || vl != 8)
        return 0;
    if (get_u64(v) > now_ms)
        return 0;
    rh_del(&d->expires, key, klen);
    rh_del(&d->table, key, klen);
    d->expired_keys++;
    return 1;
}

/* Expire-aware lookup. */
static int db_get(db *d, const char *key, size_t klen, const char **val,
                  size_t *vlen, uint64_t now_ms)
{
    db_expire_if_needed(d, key, klen, now_ms);
    return rh_get(&d->table, key, klen, val, vlen);
}

/* Overwrite a value; clears any expiry. */
static void db_set_kv(db *d, const char *key, size_t klen, const char *val,
                      size_t vlen)
{
    rh_del(&d->expires, key, klen);
    rh_set(&d->table, key, klen, val, vlen);
}

/* Delete key and expiry. Returns 1 if the key existed. */
static int db_del_kv(db *d, const char *key, size_t klen)
{
    rh_del(&d->expires, key, klen);
    return rh_del(&d->table, key, klen);
}

static void db_set_expiry(db *d, const char *key, size_t klen, uint64_t when_ms)
{
    char b[8];
    put_u64(b, when_ms);
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
                                 &vlen))
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

void command_execute_at(db *d, const resp_value *argv, size_t argc,
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
        db_set_kv(d, k, kl, v, vl);
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
        db_set_kv(d, k, kl, num, (size_t)nl);
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
        if (db_get(d, k, kl, &old, &oldl, now_ms)) {
            resp_buf tmp;
            resp_buf_init(&tmp);
            resp_buf_reserve(&tmp, oldl + vl);
            memcpy(tmp.data, old, oldl);
            memcpy(tmp.data + oldl, v, vl);
            tmp.len = oldl + vl;
            db_set_kv(d, k, kl, tmp.data, tmp.len);
            resp_write_integer(out, (long long)tmp.len);
            resp_buf_free(&tmp);
        } else {
            db_set_kv(d, k, kl, v, vl);
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
        for (size_t i = 1; i + 1 < argc; i += 2) {
            const char *k, *v;
            size_t kl, vl;
            if (!arg_str(&argv[i], &k, &kl) || !arg_str(&argv[i + 1], &v, &vl))
                goto bad_type;
            db_set_kv(d, k, kl, v, vl);
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
        resp_write_integer(out, rh_del(&d->expires, k, kl));
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
        resp_write_simple_string(out, "OK", 2);
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

void command_execute(db *d, const resp_value *argv, size_t argc, resp_buf *out)
{
    command_execute_at(d, argv, argc, out, pal_wall_ms());
}
