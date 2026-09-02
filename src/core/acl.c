#include "core/acl.h"

#include <string.h>
#include <stdio.h>

static int eq(const char *a, size_t al, const char *b)
{
    size_t bl = strlen(b);
    return al == bl && memcmp(a, b, al) == 0;
}

static int eq_ci(const char *a, size_t al, const char *b)
{
    size_t i, bl = strlen(b);
    if (al != bl) return 0;
    for (i = 0; i < al; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

void acl_init(acl_registry *r, const char *requirepass)
{
    memset(r, 0, sizeof(*r));
    r->count = 1;
    memcpy(r->users[0].name, "default", 8);
    r->users[0].enabled = 1;
    r->users[0].all_commands = 1;
    r->users[0].generation = ++r->generation_next;
    memcpy(r->users[0].patterns[0], "*", 2);
    r->users[0].pattern_count = 1;
    r->users[0].all_channels = 1;
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
    u->channel_count = 0;
    u->all_channels = 0;
    u->password[0] = '\0';
    u->no_password = 0;
    u->enabled = 0;
}

static int set_cmd(acl_user *u, const char *p, size_t n, int allow)
{
    uint16_t id;
    size_t i;
    if (n > 2 && (p[0] == '+' || p[0] == '-') && p[1] == '@') {
        const char *cat = p + 2;
        size_t clen = n - 2;
        if (clen == 3 && eq_ci(cat, clen, "all")) {
            u->all_commands = allow;
            if (allow) memset(u->deny, 0, sizeof(u->deny));
            else memset(u->allow, 0, sizeof(u->allow));
            return 0;
        }
        if ((clen == 4 && eq_ci(cat, clen, "read")) ||
            (clen == 5 && eq_ci(cat, clen, "write")) ||
            (clen == 10 && eq_ci(cat, clen, "connection"))) {
            for (i = 1; i < CMD_STATS_SLOTS; i++) {
                int match = 0;
                if (clen == 5 && cmd_is_write((uint16_t)i)) match = 1;
                if (clen == 4 && !cmd_is_write((uint16_t)i)) match = 1;
                if (clen == 10 && (i == CMD_PING || i == CMD_ECHO || i == CMD_AUTH || i == CMD_QUIT || i == CMD_SELECT)) match = 1;
                if (match) {
                    if (allow) u->allow[i / 64] |= UINT64_C(1) << (i % 64);
                    else u->deny[i / 64] |= UINT64_C(1) << (i % 64);
                }
            }
            return 0;
        }
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
        u->generation = ++r->generation_next;
    }
    temp = *u;
    for (i = 0; i < nrules; i++) {
        if (rules[i].type != RESP_BULK_STRING) return -1;
        p = rules[i].str; n = rules[i].len;
        if (eq_ci(p, n, "on")) temp.enabled = 1;
        else if (eq_ci(p, n, "off")) temp.enabled = 0;
        else if (eq_ci(p, n, "reset")) clear_rules(&temp);
        else if (eq_ci(p, n, "allkeys")) {
            temp.pattern_count = 0;
            memcpy(temp.patterns[0], "*", 2);
            temp.pattern_count = 1;
        }
        else if (eq_ci(p, n, "resetkeys")) { temp.pattern_count = 0; }
        else if (eq_ci(p, n, "allcommands")) {
            temp.all_commands = 1;
            memset(temp.deny, 0, sizeof(temp.deny));
        }
        else if (eq_ci(p, n, "nocommands")) { temp.all_commands = 0; memset(temp.allow, 0, sizeof(temp.allow)); memset(temp.deny, 0, sizeof(temp.deny)); }
        else if (eq_ci(p, n, "nopass")) { temp.password[0] = '\0'; temp.no_password = 1; }
        else if (eq_ci(p, n, "resetpass")) { temp.password[0] = '\0'; temp.no_password = 0; }
        else if (n > 1 && p[0] == '>') {
            if (n >= ACL_MAX_PASSWORD) return -1;
            memcpy(temp.password, p + 1, n - 1); temp.password[n - 1] = '\0';
        } else if (n > 1 && p[0] == '~') {
            if (n == 2 && p[1] == '*') {
                temp.pattern_count = 0;
                memcpy(temp.patterns[0], "*", 2);
                temp.pattern_count = 1;
                continue;
            }
            if (temp.pattern_count >= ACL_MAX_PATTERNS || n >= ACL_MAX_PATTERN)
                return -1;
            memcpy(temp.patterns[temp.pattern_count], p + 1, n - 1);
            temp.patterns[temp.pattern_count++][n - 1] = '\0';
        } else if (n > 1 && p[0] == '&') {
            if (n == 2 && p[1] == '*') {
                temp.channel_count = 0;
                temp.all_channels = 1;
            }
            else {
                if (temp.channel_count >= ACL_MAX_CHANNELS || n >= ACL_MAX_PATTERN)
                    return -1;
                memcpy(temp.channels[temp.channel_count], p + 1, n - 1);
                temp.channels[temp.channel_count++][n - 1] = '\0';
            }
        } else if (eq_ci(p, n, "allchannels")) {
            temp.channel_count = 0;
            temp.all_channels = 1;
        } else if (eq_ci(p, n, "resetchannels")) {
            temp.channel_count = 0;
            temp.all_channels = 0;
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
            r->users[i].generation = ++r->generation_next;
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
    if (u->no_password) return u;
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

int acl_authorize_channel(const acl_user *u, const char *channel,
                          size_t clen, int is_pattern)
{
    size_t i;
    if (u == NULL || !u->enabled || channel == NULL) return 0;
    if (u->all_channels) return 1;
    for (i = 0; i < u->channel_count; i++) {
        size_t plen = strlen(u->channels[i]);
        if ((is_pattern && plen == clen && memcmp(u->channels[i], channel, clen) == 0) ||
            (!is_pattern && acl_match_pattern(u->channels[i], plen, channel, clen)))
            return 1;
    }
    return 0;
}

void acl_log_event(acl_registry *r, const char *reason, const char *user,
                   size_t ulen, const char *object, size_t olen,
                   uint64_t now_ms)
{
    acl_log_entry *e;
    size_t n;
    if (r == NULL || reason == NULL) return;
    if (user == NULL) ulen = 0;
    if (object == NULL) olen = 0;
    if (r->log_len > 0) {
        size_t last = (r->log_next + ACL_LOG_MAX - 1) % ACL_LOG_MAX;
        acl_log_entry *prev = &r->log[last];
        if (strcmp(prev->reason, reason) == 0 &&
            strlen(prev->username) == ulen &&
            (ulen == 0 || memcmp(prev->username, user, ulen) == 0) &&
            strlen(prev->object) == olen &&
            (olen == 0 || memcmp(prev->object, object, olen) == 0)) {
            if (prev->count != UINT64_MAX) prev->count++;
            prev->age_ms = now_ms;
            return;
        }
    }
    e = &r->log[r->log_next];
    memset(e, 0, sizeof(*e));
    e->count = 1; e->age_ms = now_ms;
    n = strlen(reason); if (n >= sizeof(e->reason)) n = sizeof(e->reason) - 1;
    memcpy(e->reason, reason, n); e->reason[n] = '\0';
    if (user != NULL) { if (ulen >= sizeof(e->username)) ulen = sizeof(e->username) - 1; memcpy(e->username, user, ulen); e->username[ulen] = '\0'; }
    if (object != NULL) { if (olen >= sizeof(e->object)) olen = sizeof(e->object) - 1; memcpy(e->object, object, olen); e->object[olen] = '\0'; }
    r->log_next = (uint8_t)((r->log_next + 1) % ACL_LOG_MAX);
    if (r->log_len < ACL_LOG_MAX) r->log_len++;
}

void acl_log_reset(acl_registry *r)
{
    if (r == NULL) return;
    r->log_len = 0; r->log_next = 0;
}

void acl_log_write(const acl_registry *r, long long count, uint64_t now_ms,
                   resp_buf *out)
{
    size_t take, i, idx;
    if (r == NULL) { resp_write_array_header(out, 0); return; }
    if (count < 0) count = 0;
    take = (size_t)count < r->log_len ? (size_t)count : r->log_len;
    resp_write_array_header(out, take);
    for (i = 0; i < take; i++) {
        idx = (r->log_next + ACL_LOG_MAX - 1 - i) % ACL_LOG_MAX;
        resp_write_map_header(out, 5);
        resp_write_bulk(out, "count", 5); resp_write_integer(out, (long long)r->log[idx].count);
        resp_write_bulk(out, "reason", 6); resp_write_bulk(out, r->log[idx].reason, strlen(r->log[idx].reason));
        resp_write_bulk(out, "username", 8); resp_write_bulk(out, r->log[idx].username, strlen(r->log[idx].username));
        resp_write_bulk(out, "object", 6); resp_write_bulk(out, r->log[idx].object, strlen(r->log[idx].object));
        resp_write_bulk(out, "age-seconds", 11); resp_write_double(out, (double)(now_ms - r->log[idx].age_ms) / 1000.0);
    }
}

int acl_authorize(const acl_user *u, uint16_t cmd_id, const resp_value *argv,
                  size_t argc)
{
    size_t i, first = 1, step = 1, nkeys = 0;
    int keyless = 0;
    if (u == NULL || !u->enabled || cmd_id >= CMD_STATS_SLOTS) return 0;
    if (!u->all_commands && !(u->allow[cmd_id / 64] & (UINT64_C(1) << (cmd_id % 64)))) return 0;
    if (u->deny[cmd_id / 64] & (UINT64_C(1) << (cmd_id % 64))) return 0;
    if (argc < 2) return 1;
    if (cmd_id == CMD_SUBSCRIBE || cmd_id == CMD_UNSUBSCRIBE ||
        cmd_id == CMD_PSUBSCRIBE || cmd_id == CMD_PUNSUBSCRIBE ||
        cmd_id == CMD_SSUBSCRIBE || cmd_id == CMD_SUNSUBSCRIBE ||
        cmd_id == CMD_PUBLISH || cmd_id == CMD_SPUBLISH) {
        size_t first = 1, end, i;
        int pattern = cmd_id == CMD_PSUBSCRIBE || cmd_id == CMD_PUNSUBSCRIBE;
        end = (cmd_id == CMD_PUBLISH || cmd_id == CMD_SPUBLISH) ? 2 : argc;
        if (!u->all_channels) {
            for (i = first; i < end; i++) {
                if (argv[i].type != RESP_BULK_STRING ||
                    !acl_authorize_channel(u, argv[i].str, argv[i].len, pattern))
                    return 0;
            }
        }
        return 1;
    }
    switch (cmd_id) {
    case CMD_PING: case CMD_ECHO: case CMD_AUTH: case CMD_QUIT:
    case CMD_RESET: case CMD_HELLO: case CMD_TIME: case CMD_ROLE:
    case CMD_READONLY: case CMD_READWRITE: case CMD_SELECT:
    case CMD_COMMAND: case CMD_INFO: case CMD_LATENCY: case CMD_LOLWUT:
    case CMD_CONFIG: case CMD_CLIENT: case CMD_SLOWLOG:
    case CMD_WAIT: case CMD_WAITAOF: case CMD_PUBSUB: case CMD_FUNCTION:
    case CMD_SCRIPT: case CMD_DEBUG: case CMD_MODULE:
        keyless = 1; break;
    default: break;
    }
    if (cmd_id == CMD_MEMORY && argc >= 2) {
        if (argv[1].type == RESP_BULK_STRING &&
            argv[1].len == 5 && memcmp(argv[1].str, "USAGE", 5) == 0)
            nkeys = 1, first = 2;
        else
            keyless = 1;
    }
    if (cmd_id == CMD_DEBUG && argc >= 2) {
        if (argv[1].type == RESP_BULK_STRING &&
            argv[1].len == 6 && memcmp(argv[1].str, "OBJECT", 6) == 0)
            nkeys = 1, first = 2, keyless = 0;
        else
            keyless = 1;
    }
    if (keyless) return 1;
    /* Extract key positions for the common key-bearing command families.
     * Values/options are never treated as keys, avoiding false denials. */
    switch (cmd_id) {
    case CMD_SMOVE: case CMD_RENAME: case CMD_RENAMENX:
    case CMD_RPOPLPUSH: case CMD_LMOVE: case CMD_LMOVEM:
    case CMD_COPY: case CMD_LCS: case CMD_BRPOPLPUSH:
    case CMD_BLMOVE: case CMD_BLMOVEM:
        nkeys = 2; break;
    case CMD_GET: case CMD_SET: case CMD_GETDEL: case CMD_GETEX:
    case CMD_SETEX: case CMD_PSETEX: case CMD_GETSET: case CMD_APPEND:
    case CMD_INCR: case CMD_DECR: case CMD_INCRBY: case CMD_DECRBY:
    case CMD_INCRBYFLOAT: case CMD_STRLEN: case CMD_TYPE:
    case CMD_EXPIRETIME: case CMD_PEXPIRETIME:
        nkeys = 1; break;
    case CMD_MGET: case CMD_DEL: case CMD_UNLINK: case CMD_EXISTS:
    case CMD_TOUCH: case CMD_SINTER: case CMD_SUNION: case CMD_SDIFF:
        nkeys = argc - 1; break;
    case CMD_MSET: case CMD_MSETNX:
        nkeys = (argc - 1) / 2; step = 2; break;
    default:
        nkeys = 1;
        break;
    }
    if (nkeys == 0) return 1;
    if (u->pattern_count == 0) return 0;
    for (i = 0; i < nkeys && first + i * step < argc; i++) {
        size_t ai = first + i * step;
        if (argv[ai].type == RESP_BULK_STRING) {
            size_t p;
            for (p = 0; p < u->pattern_count; p++)
                if (acl_match_pattern(u->patterns[p], strlen(u->patterns[p]), argv[ai].str, argv[ai].len)) break;
            if (p == u->pattern_count) return 0;
        }
    }
    return 1;
}

void acl_write_user(const acl_user *u, resp_buf *out)
{
    size_t i;
    resp_write_array_header(out, 10);
    resp_write_bulk(out, "flags", 5);
    resp_write_array_header(out, 2);
    resp_write_bulk(out, u->enabled ? "on" : "off", u->enabled ? 2 : 3);
    resp_write_bulk(out, u->all_commands ? "allcommands" : "nocommands", u->all_commands ? 11 : 10);
    resp_write_bulk(out, "passwords", 9);
    resp_write_array_header(out, u->password[0] ? 1 : 0);
    if (u->password[0]) resp_write_bulk(out, u->password, strlen(u->password));
    {
        size_t j, count = 0;
        for (j = 1; j < CMD_STATS_SLOTS; j++)
            if ((u->allow[j / 64] | u->deny[j / 64]) &
                (UINT64_C(1) << (j % 64))) count++;
        resp_write_bulk(out, "commands", 8);
        resp_write_array_header(out, count);
        for (j = 1; j < CMD_STATS_SLOTS; j++) {
            char cmd[64];
            const char *name = cmd_name((uint16_t)j);
            int n;
            if (name == NULL || !((u->allow[j / 64] | u->deny[j / 64]) & (UINT64_C(1) << (j % 64)))) continue;
            n = snprintf(cmd, sizeof(cmd), "%c%s", (u->deny[j / 64] & (UINT64_C(1) << (j % 64))) ? '-' : '+', name);
            resp_write_bulk(out, cmd, (size_t)n);
        }
    }
    resp_write_bulk(out, "keys", 4);
    resp_write_array_header(out, u->pattern_count);
    for (i = 0; i < u->pattern_count; i++) {
        char pat[ACL_MAX_PATTERN + 1];
        pat[0] = '~';
        memcpy(pat + 1, u->patterns[i], strlen(u->patterns[i]) + 1);
        resp_write_bulk(out, pat, strlen(pat));
    }
    resp_write_bulk(out, "channels", 8);
    resp_write_array_header(out, u->all_channels ? 1 : u->channel_count);
    if (u->all_channels) resp_write_bulk(out, "&*", 2);
    else for (i = 0; i < u->channel_count; i++) {
        char pat[ACL_MAX_PATTERN + 1];
        pat[0] = '&';
        memcpy(pat + 1, u->channels[i], strlen(u->channels[i]) + 1);
        resp_write_bulk(out, pat, strlen(pat));
    }
}

void acl_write_rule_line(const acl_user *u, resp_buf *out)
{
    size_t i;
    char buf[ACL_MAX_NAME + ACL_MAX_PASSWORD +
             (ACL_MAX_PATTERNS + ACL_MAX_CHANNELS) * ACL_MAX_PATTERN + 128];
    int n = snprintf(buf, sizeof(buf), "user %s %s %s", u->name,
                     u->enabled ? "on" : "off",
                     u->all_commands ? "allcommands" : "resetkeys");
    if (n < 0 || (size_t)n >= sizeof(buf)) return;
    for (i = 1; i < CMD_STATS_SLOTS; i++) {
        size_t w;
        if ((u->allow[i / 64] & (UINT64_C(1) << (i % 64))) == 0) continue;
        w = strlen(buf);
        if (w + 2 + 32 >= sizeof(buf)) break;
        buf[w++] = ' '; buf[w++] = '+';
        {
            const char *cn = cmd_name((uint16_t)i);
            if (cn != NULL) { size_t cl = strlen(cn); if (w + cl >= sizeof(buf)) break; memcpy(buf + w, cn, cl); w += cl; }
        }
        buf[w] = '\0';
        n = (int)w;
    }
    for (i = 1; i < CMD_STATS_SLOTS; i++) {
        size_t w;
        if ((u->deny[i / 64] & (UINT64_C(1) << (i % 64))) == 0) continue;
        w = strlen(buf);
        if (w + 2 + 32 >= sizeof(buf)) break;
        buf[w++] = ' '; buf[w++] = '-';
        {
            const char *cn = cmd_name((uint16_t)i);
            if (cn != NULL) { size_t cl = strlen(cn); if (w + cl >= sizeof(buf)) break; memcpy(buf + w, cn, cl); w += cl; }
        }
        buf[w] = '\0'; n = (int)w;
    }
    if (u->password[0]) n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                                      " >%s", u->password);
    for (i = 0; i < u->pattern_count && (size_t)n < sizeof(buf); i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " ~%s", u->patterns[i]);
    for (i = 0; i < u->channel_count && (size_t)n < sizeof(buf); i++)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " &%s", u->channels[i]);
    if (u->all_channels && (size_t)n < sizeof(buf))
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " &*");
    if ((size_t)n + 2 < sizeof(buf)) { buf[n++] = '\r'; buf[n++] = '\n'; buf[n] = '\0'; }
    if (resp_buf_reserve(out, (size_t)n) == 0) { memcpy(out->data + out->len, buf, (size_t)n); out->len += (size_t)n; }
}
