/* command.c - RESP command dispatch; see command.h. */
#include "core/command.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void db_init(db *d)
{
    rh_init(&d->table);
}

void db_destroy(db *d)
{
    rh_destroy(&d->table);
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

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

void command_execute(db *d, const resp_value *argv, size_t argc, resp_buf *out)
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
        if (rh_get(&d->table, k, kl, &v, &vl))
            resp_write_bulk(out, v, vl);
        else
            resp_write_bulk(out, NULL, 0);
        return;
    }

    if (ci_equal(name, nlen, "SET")) {
        if (argc != 3) {
            wrong_args(out, "set");
            return;
        }
        const char *k, *v;
        size_t kl, vl;
        if (!arg_str(&argv[1], &k, &kl) || !arg_str(&argv[2], &v, &vl))
            goto bad_type;
        rh_set(&d->table, k, kl, v, vl);
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
            deleted += rh_del(&d->table, k, kl);
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
            found += rh_get(&d->table, k, kl, &v, &vl);
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
        if (rh_get(&d->table, k, kl, &v, &vl)) {
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
        rh_set(&d->table, k, kl, num, (size_t)nl);
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
        if (rh_get(&d->table, k, kl, &old, &oldl)) {
            resp_buf tmp;
            resp_buf_init(&tmp);
            resp_buf_reserve(&tmp, oldl + vl);
            memcpy(tmp.data, old, oldl);
            memcpy(tmp.data + oldl, v, vl);
            tmp.len = oldl + vl;
            rh_set(&d->table, k, kl, tmp.data, tmp.len);
            resp_write_integer(out, (long long)tmp.len);
            resp_buf_free(&tmp);
        } else {
            rh_set(&d->table, k, kl, v, vl);
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
        resp_write_integer(out,
                           rh_get(&d->table, k, kl, &v, &vl) ? (long long)vl : 0);
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
            if (rh_get(&d->table, k, kl, &v, &vl))
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
            rh_set(&d->table, k, kl, v, vl);
        }
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
