#include "core/acl.h"

#include <string.h>

static int eq(const char *a, size_t al, const char *b)
{
    size_t bl = strlen(b);
    return al == bl && memcmp(a, b, al) == 0;
}

void acl_init(acl_registry *r, const char *requirepass)
{
    memset(r, 0, sizeof(*r));
    r->count = 1;
    memcpy(r->users[0].name, "default", 8);
    r->users[0].enabled = 1;
    r->users[0].all_commands = 1;
    if (requirepass != NULL && requirepass[0] != '\0') {
        size_t n = strlen(requirepass);
        if (n >= ACL_MAX_PASSWORD) n = ACL_MAX_PASSWORD - 1;
        memcpy(r->users[0].password, requirepass, n);
        r->users[0].password[n] = '\0';
    }
}

acl_user *acl_find(acl_registry *r, const char *name, size_t nlen)
{
    size_t i;
    for (i = 0; i < r->count; i++)
        if (r->users[i].name[0] != '\0' && eq(name, nlen, r->users[i].name)) return &r->users[i];
    return NULL;
}

const acl_user *acl_find_const(const acl_registry *r, const char *name,
                               size_t nlen)
{
    return acl_find((acl_registry *)r, name, nlen);
}

static void clear_rules(acl_user *u)
{
    memset(u->allow, 0, sizeof(u->allow));
    memset(u->deny, 0, sizeof(u->deny));
    u->pattern_count = 0;
    u->all_commands = 0;
}

static int set_cmd(acl_user *u, const char *p, size_t n, int allow)
{
    uint16_t id;
    if (n == 4 && memcmp(p, "@all", 4) == 0) {
        u->all_commands = allow;
        return 0;
    }
    if (n == 0 || n > 255) return -1;
    id = cmd_resolve(p[0] == '+' || p[0] == '-' ? p + 1 : p,
                     p[0] == '+' || p[0] == '-' ? n - 1 : n);
    if (id == CMD_ID_UNKNOWN || id >= CMD_STATS_SLOTS) return -1;
    if (allow) u->allow[id / 64] |= UINT64_C(1) << (id % 64);
    else u->deny[id / 64] |= UINT64_C(1) << (id % 64);
    return 0;
}

int acl_setuser(acl_registry *r, const char *name, size_t nlen,
                const resp_value *rules, size_t nrules)
{
    acl_user temp;
    acl_user *u;
    size_t i;
    const char *p;
    size_t n;
    if (nlen == 0 || nlen >= ACL_MAX_NAME || !rules) return -1;
    u = acl_find(r, name, nlen);
    if (u == NULL) {
        for (i = 0; i < r->count; i++) if (r->users[i].name[0] == '\0') { u = &r->users[i]; break; }
        if (u == NULL) { if (r->count >= ACL_MAX_USERS) return -1; u = &r->users[r->count++]; }
        memset(u, 0, sizeof(*u));
        memcpy(u->name, name, nlen);
        u->name[nlen] = '\0';
    }
    temp = *u;
    for (i = 0; i < nrules; i++) {
        if (rules[i].type != RESP_BULK_STRING) return -1;
        p = rules[i].str; n = rules[i].len;
        if (n == 2 && memcmp(p, "on", 2) == 0) temp.enabled = 1;
        else if (n == 3 && memcmp(p, "off", 3) == 0) temp.enabled = 0;
        else if (n == 5 && memcmp(p, "reset", 5) == 0) clear_rules(&temp);
        else if (n > 1 && p[0] == '>') {
            if (n >= ACL_MAX_PASSWORD) return -1;
            memcpy(temp.password, p + 1, n - 1); temp.password[n - 1] = '\0';
        } else if (n > 1 && p[0] == '~') {
            if (temp.pattern_count >= ACL_MAX_PATTERNS || n >= ACL_MAX_PATTERN)
                return -1;
            memcpy(temp.patterns[temp.pattern_count], p + 1, n - 1);
            temp.patterns[temp.pattern_count++][n - 1] = '\0';
        } else if (n > 1 && (p[0] == '+' || p[0] == '-')) {
            if (set_cmd(&temp, p, n, p[0] == '+') != 0) return -1;
        } else return -1;
    }
    *u = temp;
    return 0;
}

int acl_deluser(acl_registry *r, const char *name, size_t nlen)
{
    size_t i;
    if (eq(name, nlen, "default")) return 0;
    for (i = 0; i < r->count; i++) {
        if (eq(name, nlen, r->users[i].name)) {
            /* Keep array addresses stable: live sessions retain user refs. */
            r->users[i].enabled = 0;
            r->users[i].password[0] = '\0';
            clear_rules(&r->users[i]);
            r->users[i].name[0] = '\0';
            return 1;
        }
    }
    return 0;
}

const acl_user *acl_authenticate(const acl_registry *r, const char *name,
                                 size_t nlen, const char *password,
                                 size_t plen)
{
    const acl_user *u = acl_find_const(r, name, nlen);
    size_t i;
    unsigned char diff = 0;
    if (u == NULL || !u->enabled) return NULL;
    if (strlen(u->password) != plen) return NULL;
    for (i = 0; i < plen; i++) diff |= (unsigned char)u->password[i] ^ (unsigned char)password[i];
    return diff == 0 ? u : NULL;
}

int acl_match_pattern(const char *pat, size_t plen, const char *key, size_t klen)
{
    size_t pi = 0, ki = 0, star = SIZE_MAX, mark = 0;
    while (ki < klen) {
        if (pi < plen && (pat[pi] == '?' || pat[pi] == key[ki])) { pi++; ki++; }
        else if (pi < plen && pat[pi] == '*') { star = pi++; mark = ki; }
        else if (star != SIZE_MAX) { pi = star + 1; ki = ++mark; }
        else return 0;
    }
    while (pi < plen && pat[pi] == '*') pi++;
    return pi == plen;
}

int acl_authorize(const acl_user *u, uint16_t cmd_id, const resp_value *argv,
                  size_t argc)
{
    size_t i;
    if (u == NULL || !u->enabled || cmd_id >= CMD_STATS_SLOTS) return 0;
    if (!u->all_commands && !(u->allow[cmd_id / 64] & (UINT64_C(1) << (cmd_id % 64)))) return 0;
    if (u->deny[cmd_id / 64] & (UINT64_C(1) << (cmd_id % 64))) return 0;
    if (u->pattern_count == 0 || argc < 2) return 1;
    for (i = 1; i < argc; i++) {
        if (argv[i].type == RESP_BULK_STRING) {
            size_t p;
            for (p = 0; p < u->pattern_count; p++)
                if (acl_match_pattern(u->patterns[p], strlen(u->patterns[p]), argv[i].str, argv[i].len)) break;
            if (p == u->pattern_count) return 0;
        }
    }
    return 1;
}

void acl_write_user(const acl_user *u, resp_buf *out)
{
    size_t i;
    resp_write_array_header(out, 8);
    resp_write_bulk(out, "flags", 5);
    resp_write_array_header(out, 2);
    resp_write_bulk(out, u->enabled ? "on" : "off", u->enabled ? 2 : 3);
    resp_write_bulk(out, u->all_commands ? "allcommands" : "resetchannels", u->all_commands ? 11 : 12);
    resp_write_bulk(out, "passwords", 9);
    resp_write_array_header(out, u->password[0] ? 1 : 0);
    if (u->password[0]) resp_write_bulk(out, u->password, strlen(u->password));
    resp_write_bulk(out, "commands", 8);
    resp_write_array_header(out, 0);
    resp_write_bulk(out, "keys", 4);
    resp_write_array_header(out, u->pattern_count);
    for (i = 0; i < u->pattern_count; i++) resp_write_bulk(out, u->patterns[i], strlen(u->patterns[i]));
}
