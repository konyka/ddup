/* cluster.c - single-node cluster mode: node identity; see cluster.h. */
#include "core/cluster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "pal/pal_file.h"

#include "pal/pal_time.h"

static int parse_u16_token(const char *s, uint16_t *out)
{
    uint32_t value = 0;
    size_t i;

    if (s == NULL || *s == '\0')
        return -1;
    for (i = 0; s[i] != '\0'; i++) {
        unsigned digit;
        if (s[i] < '0' || s[i] > '9')
            return -1;
        digit = (unsigned)(s[i] - '0');
        if (value > (UINT16_MAX - digit) / 10u)
            return -1;
        value = value * 10u + digit;
    }
    *out = (uint16_t)value;
    return 0;
}

/* Parse one space-delimited unsigned field without reading beyond end. */
static int parse_u64_field(const char **pp, const char *end, uint64_t *out)
{
    const char *p;
    uint64_t value = 0;

    if (pp == NULL || *pp == NULL || end == NULL || out == NULL ||
        *pp >= end)
        return -1;
    p = *pp;
    while (p < end && *p != ' ') {
        unsigned digit;
        if (*p < '0' || *p > '9')
            return -1;
        digit = (unsigned)(*p++ - '0');
        if (value > (UINT64_MAX - digit) / 10u)
            return -1;
        value = value * 10u + digit;
    }
    if (p == *pp)
        return -1;
    *pp = p;
    *out = value;
    return 0;
}

static int cluster_id_valid(const char *id)
{
    size_t i;
    if (id == NULL || strlen(id) != 40)
        return 0;
    for (i = 0; i < 40; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

void cluster_gen_id(char out[41])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t x = pal_now_us() ^ 0x9E3779B97F4A7C15ULL;
    int i;
    if (out == NULL)
        return;
    for (i = 0; i < 40; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = hex[x & 0xF];
    }
    out[40] = '\0';
}

int cluster_node_id_load_or_create(const char *path, char out_id[41])
{
    if (path == NULL || out_id == NULL || path[0] == '\0')
        return -1;
    if (pal_file_exists(path)) {
        pal_file *f = pal_file_open_read(path);
        char buf[41];
        ptrdiff_t n;
        int i;
        if (f == NULL)
            return -1;
        memset(buf, 0, sizeof(buf));
        n = pal_file_read(f, buf, 40);
        pal_file_close(f);
        if (n != 40)
            return -1;
        for (i = 0; i < 40; i++) {
            char c = buf[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return -1;
        }
        memcpy(out_id, buf, 40);
        out_id[40] = '\0';
        return 0;
    }

    cluster_gen_id(out_id);
    {
        pal_file *f = pal_file_open_write(path);
        char line[128];
        int len;
        if (f == NULL)
            return -1;
        len = snprintf(line, sizeof(line),
                       "%s :0@0 myself,master - 0 0 1 connected\n", out_id);
        if (len < 0 || (size_t)len >= sizeof(line) ||
            pal_file_write(f, line, (size_t)len) != len) {
            pal_file_close(f);
            return -1;
        }
        if (pal_file_close(f) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* cluster node table                                                 */
/* ------------------------------------------------------------------ */

void cluster_nodes_init(struct db *d)
{
    if (d == NULL)
        return;
    d->nnodes = 0;
}

cluster_node *cluster_node_find(struct db *d, const char *id)
{
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        !cluster_id_valid(id))
        return NULL;
    for (i = 0; i < d->nnodes; i++)
        if (memcmp(d->nodes[i].id, id, 40) == 0)
            return &d->nodes[i];
    return NULL;
}

cluster_node *cluster_node_add(struct db *d, const char *id)
{
    cluster_node *n;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        id == NULL)
        return NULL;
    if (!cluster_id_valid(id))
        return NULL;
    n = cluster_node_find(d, id);
    if (n != NULL)
        return n;
    if (d->nnodes >= CLUSTER_MAX_NODES)
        return NULL;
    n = &d->nodes[d->nnodes++];
    memset(n, 0, sizeof(*n));
    memcpy(n->id, id, 40);
    n->id[40] = '\0';
    n->flags = CLUSTER_NODE_MASTER;
    n->master_id[0] = '-';
    n->master_id[1] = '\0';
    return n;
}

void cluster_report_failure(struct db *d, cluster_node *subject,
                            const char *reporter, uint64_t now_ms)
{
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        subject == NULL || subject->nreports < 0 ||
        subject->nreports > CLUSTER_MAX_NODES || !cluster_id_valid(reporter))
        return;
    for (i = 0; i < subject->nreports; i++)
        if (memcmp(subject->reports[i].reporter, reporter, 40) == 0) {
            subject->reports[i].time_ms = now_ms; /* refresh */
            return;
        }
    if (subject->nreports >= CLUSTER_MAX_NODES)
        return; /* report table cap (documented) */
    memcpy(subject->reports[subject->nreports].reporter, reporter, 40);
    subject->reports[subject->nreports].reporter[40] = '\0';
    subject->reports[subject->nreports].time_ms = now_ms;
    subject->nreports++;
}

void cluster_report_heal(struct db *d, cluster_node *subject,
                         const char *reporter)
{
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        subject == NULL || subject->nreports < 0 ||
        subject->nreports > CLUSTER_MAX_NODES || !cluster_id_valid(reporter))
        return;
    for (i = 0; i < subject->nreports; i++)
        if (memcmp(subject->reports[i].reporter, reporter, 40) == 0) {
            subject->reports[i] = subject->reports[subject->nreports - 1];
            subject->nreports--;
            return;
        }
}

int cluster_report_count(struct db *d, cluster_node *subject, uint64_t now_ms)
{
    uint64_t window;
    int i = 0;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        subject == NULL || subject->nreports < 0 ||
        subject->nreports > CLUSTER_MAX_NODES)
        return 0;
    window = d->cluster_node_timeout_ms > UINT64_MAX / 2
                 ? UINT64_MAX
                 : d->cluster_node_timeout_ms * 2;
    /* drop expired entries in place, then the remainder is the count */
    while (i < subject->nreports) {
        if (now_ms - subject->reports[i].time_ms > window) {
            subject->reports[i] = subject->reports[subject->nreports - 1];
            subject->nreports--;
        } else {
            i++;
        }
    }
    return subject->nreports;
}

void cluster_mark_fail(struct db *d, cluster_node *subject, uint64_t now_ms)
{
    if (d == NULL || subject == NULL)
        return;
    if (subject->flags & (CLUSTER_NODE_FAIL | CLUSTER_NODE_MYSELF))
        return;
    subject->flags &= ~(uint32_t)CLUSTER_NODE_PFAIL;
    subject->flags |= CLUSTER_NODE_FAIL;
    subject->fail_time_ms = now_ms;
    d->cluster_changes++;
}

int cluster_mark_fail_if_quorum(struct db *d, cluster_node *subject,
                                uint64_t now_ms)
{
    cluster_node *me;
    int masters = 0, failures, i;
    uint32_t sl;

    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        subject == NULL || subject->nreports < 0 ||
        subject->nreports > CLUSTER_MAX_NODES)
        return 0;
    me = cluster_myself(d);

    /* local suspicion is required, and FAIL is terminal here */
    if (!(subject->flags & CLUSTER_NODE_PFAIL) ||
        (subject->flags & CLUSTER_NODE_FAIL))
        return 0;

    /* cluster size: masters serving at least one slot */
    for (i = 0; i < d->nnodes; i++) {
        cluster_node *n = &d->nodes[i];
        if (!(n->flags & CLUSTER_NODE_MASTER))
            continue;
        for (sl = 0; sl < 16384; sl++)
            if (cluster_slots_get(n->slots, sl))
                break;
        if (sl < 16384)
            masters++;
    }

    failures = cluster_report_count(d, subject, now_ms);
    if (me != NULL && (me->flags & CLUSTER_NODE_MASTER))
        failures++; /* our own suspicion counts (we are a master) */
    if (failures < masters / 2 + 1)
        return 0;

    cluster_mark_fail(d, subject, now_ms);
    memcpy(d->fail_broadcast_id, subject->id, 41);
    return 1;
}

void cluster_slots_set(uint8_t *bm, uint32_t slot, int on)
{
    if (bm == NULL || slot >= 16384)
        return;
    if (on)
        bm[slot / 8] |= (uint8_t)(1u << (slot % 8));
    else
        bm[slot / 8] &= (uint8_t)~(1u << (slot % 8));
}

int cluster_slots_get(const uint8_t *bm, uint32_t slot)
{
    if (bm == NULL || slot >= 16384)
        return 0;
    return (bm[slot / 8] >> (slot % 8)) & 1;
}

int cluster_slots_render(const uint8_t *bm, char *out, size_t cap)
{
    size_t n = 0;
    int s = -1;
    uint32_t i;
    if (bm == NULL || out == NULL || cap == 0)
        return -1;
    out[0] = '\0';
    for (i = 0; i <= 16384; i++) {
        int on = i < 16384 ? cluster_slots_get(bm, i) : 0;
        if (on && s < 0)
            s = (int)i;
        if (!on && s >= 0) {
            int w;
            if (s == (int)i - 1)
                w = snprintf(out + n, cap - n, "%s%d", n ? " " : "", s);
            else
                w = snprintf(out + n, cap - n, "%s%d-%u", n ? " " : "", s,
                             i - 1);
            if (w < 0 || (size_t)w >= cap - n) {
                out[n] = '\0';
                return (int)n;
            }
            n += (size_t)w;
            s = -1;
        }
    }
    return (int)n;
}

void cluster_slots_parse(uint8_t *bm, const char *s, size_t len)
{
    size_t i = 0;
    if (bm == NULL)
        return;
    memset(bm, 0, 2048);
    if (s == NULL)
        return;
    while (i < len) {
        size_t start, end, dash = SIZE_MAX;
        uint32_t a = 0, b = 0;
        int invalid = 0;

        while (i < len && (s[i] == ' ' || s[i] == '\t' ||
                           s[i] == '\r' || s[i] == '\n'))
            i++;
        if (i >= len)
            break;
        start = i;
        while (i < len && s[i] != ' ' && s[i] != '\t' &&
               s[i] != '\r' && s[i] != '\n') {
            if (s[i] == '-') {
                if (dash != SIZE_MAX)
                    invalid = 1;
                else
                    dash = i;
            }
            i++;
        }
        end = i;

        /* Parse each side with overflow detection; malformed tokens are ignored. */
        {
            size_t p = start;
            size_t stop = dash == SIZE_MAX ? end : dash;
            if (p == stop)
                invalid = 1;
            while (!invalid && p < stop) {
                unsigned digit;
                if (s[p] < '0' || s[p] > '9') {
                    invalid = 1;
                    break;
                }
                digit = (unsigned)(s[p++] - '0');
                if (a > (UINT32_MAX - digit) / 10u) {
                    invalid = 1;
                    break;
                }
                a = a * 10u + digit;
            }
            b = a;
            if (!invalid && dash != SIZE_MAX) {
                p = dash + 1;
                if (p == end)
                    invalid = 1;
                while (!invalid && p < end) {
                    unsigned digit;
                    if (s[p] < '0' || s[p] > '9') {
                        invalid = 1;
                        break;
                    }
                    digit = (unsigned)(s[p++] - '0');
                    if (b > (UINT32_MAX - digit) / 10u) {
                        invalid = 1;
                        break;
                    }
                    b = b * 10u + digit;
                }
            }
        }
        if (invalid || a > 16383 || b < a)
            continue;
        if (b > 16383)
            b = 16383;
        for (; a <= b; a++)
            cluster_slots_set(bm, a, 1);
    }
}

uint64_t cluster_next_epoch(struct db *d)
{
    if (d == NULL)
        return 0;
    return ++d->cluster_current_epoch;
}

void cluster_adopt_claims(struct db *d, cluster_node *claimant,
                          const uint8_t *bm, uint64_t epoch)
{
    uint32_t s;
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        claimant == NULL || bm == NULL)
        return;
    if (epoch > claimant->epoch)
        claimant->epoch = epoch;
    if (epoch > d->cluster_current_epoch)
        d->cluster_current_epoch = epoch;
    for (s = 0; s < 16384; s++) {
        if (!cluster_slots_get(bm, s))
            continue;
        for (i = 0; i < d->nnodes; i++)
            if (&d->nodes[i] != claimant)
                cluster_slots_set(d->nodes[i].slots, s, 0);
        cluster_slots_set(claimant->slots, s, 1);
    }
    d->slot_owner_dirty = 1;
    d->cluster_changes++;
}

int cluster_failover_promote(struct db *d)
{
    cluster_node *me;
    cluster_node *master;
    char mid[41];
    uint32_t s;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES)
        return 0;
    me = cluster_myself(d);
    if (me == NULL || !(me->flags & CLUSTER_NODE_SLAVE))
        return 0;
    memcpy(mid, me->master_id, sizeof(mid));
    me->flags |= CLUSTER_NODE_MASTER;
    me->flags &= ~(uint32_t)CLUSTER_NODE_SLAVE;
    me->master_id[0] = '-';
    me->master_id[1] = '\0';
    me->epoch = cluster_next_epoch(d);
    master = cluster_node_find(d, mid);
    if (master != NULL) {
        for (s = 0; s < 16384; s++) {
            if (cluster_slots_get(master->slots, s)) {
                cluster_slots_set(master->slots, s, 0);
                cluster_slots_set(me->slots, s, 1);
            }
        }
    }
    d->slot_owner_dirty = 1;
    d->cluster_changes++;
    return 1;
}

void cluster_merge_claims(struct db *d, cluster_node *claimant,
                          const uint8_t *bm, uint64_t epoch)
{
    cluster_node *me;
    uint32_t s;
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        claimant == NULL || bm == NULL)
        return;
    me = cluster_myself(d);
    if (epoch > claimant->epoch)
        claimant->epoch = epoch;
    if (epoch > d->cluster_current_epoch)
        d->cluster_current_epoch = epoch;
    if (claimant == me)
        return; /* our own claims are locally authoritative */
    for (s = 0; s < 16384; s++) {
        int claims = cluster_slots_get(bm, s);
        int has = cluster_slots_get(claimant->slots, s);
        if (!claims) {
            if (has)
                cluster_slots_set(claimant->slots, s, 0); /* retraction */
            continue;
        }
        /* contested slot: resolve pairwise against every other holder */
        {
            int wins = 1;
            for (i = 0; i < d->nnodes; i++) {
                cluster_node *o = &d->nodes[i];
                if (o == claimant || !cluster_slots_get(o->slots, s))
                    continue;
                if (claimant->epoch > o->epoch ||
                    (claimant->epoch == o->epoch &&
                     memcmp(claimant->id, o->id, 40) > 0)) {
                    cluster_slots_set(o->slots, s, 0); /* loser yields */
                } else {
                    wins = 0;
                }
            }
            if (wins) {
                if (!has)
                    cluster_slots_set(claimant->slots, s, 1);
            } else if (has) {
                cluster_slots_set(claimant->slots, s, 0);
            }
        }
    }
    d->slot_owner_dirty = 1;
    d->cluster_changes++;
}

static void flags_render(uint32_t flags, char *out, size_t cap)
{
    out[0] = '\0';
    if (flags & CLUSTER_NODE_MYSELF)
        strcat(out, "myself,");
    if (flags & CLUSTER_NODE_MASTER)
        strcat(out, "master,");
    if (flags & CLUSTER_NODE_SLAVE)
        strcat(out, "slave,");
    if (flags & CLUSTER_NODE_HANDSHAKE)
        strcat(out, "handshake,");
    if (flags & CLUSTER_NODE_NOADDR)
        strcat(out, "noaddr,");
    if (flags & CLUSTER_NODE_DISCONNECTED)
        strcat(out, "disconnected,");
    if (flags & CLUSTER_NODE_PFAIL)
        strcat(out, "fail?,");
    if (flags & CLUSTER_NODE_FAIL)
        strcat(out, "fail,");
    {
        size_t n = strlen(out);
        if (n == 0)
            snprintf(out, cap, "noflags");
        else
            out[n - 1] = '\0';
    }
    (void)cap;
}

/* tiny local substring search (memmem is GNU-only). */
static const void *strnstr(const char *h, size_t hlen, const char *n,
                           size_t nlen)
{
    size_t i;
    if (nlen == 0 || hlen < nlen)
        return NULL;
    for (i = 0; i + nlen <= hlen; i++)
        if (memcmp(h + i, n, nlen) == 0)
            return h + i;
    return NULL;
}

static uint32_t flags_parse(const char *s, size_t len)
{
    uint32_t f = 0;
    if (strnstr(s, len, "myself", 6) != NULL)
        f |= CLUSTER_NODE_MYSELF;
    if (strnstr(s, len, "master", 6) != NULL)
        f |= CLUSTER_NODE_MASTER;
    if (strnstr(s, len, "slave", 5) != NULL)
        f |= CLUSTER_NODE_SLAVE;
    if (strnstr(s, len, "handshake", 9) != NULL)
        f |= CLUSTER_NODE_HANDSHAKE;
    if (strnstr(s, len, "noaddr", 6) != NULL)
        f |= CLUSTER_NODE_NOADDR;
    if (strnstr(s, len, "disconnected", 12) != NULL)
        f |= CLUSTER_NODE_DISCONNECTED;
    /* "fail?" contains "fail": test the longer token first */
    if (strnstr(s, len, "fail?", 5) != NULL)
        f |= CLUSTER_NODE_PFAIL;
    else if (strnstr(s, len, "fail", 4) != NULL)
        f |= CLUSTER_NODE_FAIL;
    return f;
}

int cluster_nodes_render(struct db *d, resp_buf *out)
{
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        out == NULL)
        return -1;
    for (i = 0; i < d->nnodes; i++) {
        cluster_node *n = &d->nodes[i];
        char flags[64], slots[256];
        size_t start;
        int w;
        if (resp_buf_reserve(out, 512) != 0)
            return -1;
        start = out->len;
        flags_render(n->flags, flags, sizeof(flags));
        cluster_slots_render(n->slots, slots, sizeof(slots));
        w = snprintf(out->data + out->len, out->cap - out->len,
                     "%s %s:%u@%u %s %s %llu %llu %llu %s %s\n", n->id,
                     n->ip, (unsigned)n->port, (unsigned)n->bus_port, flags,
                     n->master_id,
                     (unsigned long long)n->ping_sent_ms,
                     (unsigned long long)n->last_seen_ms,
                     (unsigned long long)n->epoch,
                     (n->flags & CLUSTER_NODE_DISCONNECTED) ? "disconnected"
                                                            : "connected",
                     slots);
        if (w < 0 || (size_t)w >= out->cap - out->len)
            return -1;
        out->len = start + (size_t)w;
    }
    return 0;
}

int cluster_nodes_parse_line(struct db *d, const char *line, size_t len)
{
    char id[41], ip[64], flags[64], slots[512], addr[128], master[41];
    uint16_t port = 0, bus = 0;
    uint64_t ping, pong, epoch;
    cluster_node *n;
    int used;

    if (d == NULL || line == NULL)
        return -1;

    memset(ip, 0, sizeof(ip));
    memset(flags, 0, sizeof(flags));
    memset(slots, 0, sizeof(slots));
    memset(addr, 0, sizeof(addr));
    memset(master, 0, sizeof(master));
    used = 0;
    if (len < 40)
        return -1;
    memcpy(id, line, 40);
    id[40] = '\0';
    /* id addr flags master ping pong epoch link slots... */
    {
        const char *p = line + 40;
        const char *end = line + len;
        const char *s;
        size_t alen, flen, mlen;
        /* addr token */
        while (p < end && *p == ' ')
            p++;
        s = p;
        while (p < end && *p != ' ')
            p++;
        alen = (size_t)(p - s);
        if (alen == 0 || alen >= sizeof(addr))
            return -1;
        memcpy(addr, s, alen);
        /* flags token */
        while (p < end && *p == ' ')
            p++;
        s = p;
        while (p < end && *p != ' ')
            p++;
        flen = (size_t)(p - s);
        if (flen == 0 || flen >= sizeof(flags))
            return -1;
        memcpy(flags, s, flen);
        /* master token */
        while (p < end && *p == ' ')
            p++;
        s = p;
        while (p < end && *p != ' ')
            p++;
        mlen = (size_t)(p - s);
        if (mlen >= sizeof(master))
            return -1;
        memcpy(master, s, mlen);
        /* ping pong epoch link */
        while (p < end && *p == ' ')
            p++;
        if (parse_u64_field(&p, end, &ping) != 0)
            return -1;
        while (p < end && *p == ' ')
            p++;
        if (parse_u64_field(&p, end, &pong) != 0)
            return -1;
        while (p < end && *p == ' ')
            p++;
        if (parse_u64_field(&p, end, &epoch) != 0)
            return -1;
        while (p < end && *p == ' ')
            p++;
        s = p;
        while (p < end && *p != ' ')
            p++; /* link-state token */
        /* rest: slots */
        while (p < end && *p == ' ')
            p++;
        if ((size_t)(end - p) >= sizeof(slots))
            return -1;
        memcpy(slots, p, (size_t)(end - p));
        used = (int)(end - line);
        (void)used;
    }

    /* addr: ip:port@busport; reject truncation rather than silently
     * accepting a node address that cannot be represented locally. */
    {
        char *colon = strchr(addr, ':');
        char *at = strchr(addr, '@');
        char *port_text = colon != NULL ? colon + 1 : NULL;
        size_t ip_len = colon != NULL ? (size_t)(colon - addr) : strlen(addr);
        if (ip_len >= sizeof(ip))
            return -1;
        if (at != NULL && (colon == NULL || at < colon))
            return -1;
        if (colon != NULL)
            *colon = '\0';
        if (at != NULL) {
            *at = '\0';
            if (parse_u16_token(at + 1, &bus) != 0)
                return -1;
        }
        if (colon != NULL && parse_u16_token(port_text, &port) != 0)
            return -1;
        memcpy(ip, addr, ip_len);
        ip[ip_len] = '\0';
    }
    /* Do not publish a node until every bounded field has been validated. */
    n = cluster_node_add(d, id);
    if (n == NULL)
        return -1;
    memcpy(n->ip, ip, strlen(ip) + 1);
    n->port = (uint16_t)port;
    n->bus_port = (uint16_t)bus;
    n->flags = flags_parse(flags, strlen(flags));
    n->ping_sent_ms = ping;
    n->last_seen_ms = pong;
    n->epoch = epoch;
    if (master[0] != '\0')
        snprintf(n->master_id, sizeof(n->master_id), "%s", master);
    cluster_slots_parse(n->slots, slots, strlen(slots));
    return 0;
}

/* ------------------------------------------------------------------ */
/* ddup cluster bus protocol v1                                       */
/* ------------------------------------------------------------------ */

#define CLUSTER_GOSSIP_MAX 10

static void put16(char *p, uint16_t v)
{
    p[0] = (char)(v & 0xFFu);
    p[1] = (char)((v >> 8) & 0xFFu);
}

static void put32(char *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (char)((v >> (8 * i)) & 0xFFu);
}

static uint16_t get16(const char *p)
{
    return (uint16_t)((uint16_t)(uint8_t)p[0] |
                      ((uint16_t)(uint8_t)p[1] << 8));
}

static uint32_t get32(const char *p)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++)
        v |= (uint32_t)(uint8_t)p[i] << (8 * i);
    return v;
}

static void put64(char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (char)((v >> (8 * i)) & 0xFFu);
}

static uint64_t get64(const char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v |= (uint64_t)(uint8_t)p[i] << (8 * i);
    return v;
}

int cluster_bus_build_frame(struct db *d, int type, resp_buf *out)
{
    const cluster_node *sn = NULL;
    size_t start, ipl, total;
    char *p, *gcp;
    uint16_t gc = 0;
    int i;

    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        out == NULL)
        return -1;
    for (i = 0; i < d->nnodes; i++)
        if (d->nodes[i].flags & CLUSTER_NODE_MYSELF) {
            sn = &d->nodes[i];
            break;
        }
    if (sn == NULL && d->nnodes > 0)
        sn = &d->nodes[0];
    if (sn == NULL)
        return -1;

    start = out->len;
    if (resp_buf_reserve(out, CLUSTER_MSG_MAX) != 0)
        return -1;
    p = out->data + out->len;
    memcpy(p, CLUSTER_BUS_MAGIC_V2, 4);
    p += 4;
    p += 4; /* totlen patched below */
    put16(p, (uint16_t)type);
    p += 2;
    memcpy(p, sn->id, 40);
    p += 40;
    ipl = strlen(sn->ip);
    put16(p, (uint16_t)ipl);
    p += 2;
    memcpy(p, sn->ip, ipl);
    p += ipl;
    put16(p, sn->port);
    p += 2;
    put16(p, sn->bus_port);
    p += 2;
    put32(p, sn->flags);
    p += 4;
    memcpy(p, sn->slots, 2048);
    p += 2048;
    memcpy(p, sn->master_id, 40); /* v2: role master_id + config epoch */
    p += 40;
    put64(p, sn->epoch);
    p += 8;

    gcp = p;
    p += 2; /* gossip count */
    for (i = 0; i < d->nnodes && gc < CLUSTER_GOSSIP_MAX; i++) {
        const cluster_node *n = &d->nodes[i];
        char slots[256];
        int sl;
        if (n == sn)
            continue;
        memcpy(p, n->id, 40);
        p += 40;
        ipl = strlen(n->ip);
        put16(p, (uint16_t)ipl);
        p += 2;
        memcpy(p, n->ip, ipl);
        p += ipl;
        put16(p, n->port);
        p += 2;
        put16(p, n->bus_port);
        p += 2;
        put32(p, n->flags);
        p += 4;
        sl = cluster_slots_render(n->slots, slots, sizeof(slots));
        put16(p, (uint16_t)sl);
        p += 2;
        memcpy(p, slots, (size_t)sl);
        p += sl;
        memcpy(p, n->master_id, 40); /* v2 extras */
        p += 40;
        put64(p, n->epoch);
        p += 8;
        gc++;
    }
    put16(gcp, gc);

    total = (size_t)(p - (out->data + start));
    if (total > CLUSTER_MSG_MAX || total > SIZE_MAX - start)
        return -1;
    out->len = start + total;
    put32(out->data + start + 4, (uint32_t)total);
    return 0;
}

int cluster_bus_build_publish(struct db *d, const char *ch, size_t chlen,
                              const char *msg, size_t mlen, resp_buf *out)
{
    size_t start;
    size_t total;
    char *p;
    (void)d; /* publish frames carry no node record */
    if (out == NULL || (ch == NULL && chlen != 0) ||
        (msg == NULL && mlen != 0))
        return -1;
    start = out->len;
    if (chlen > SIZE_MAX - 18 || mlen > SIZE_MAX - 18 - chlen)
        return -1;
    total = 18 + chlen + mlen;
    if (start > SIZE_MAX - total)
        return -1;
    if (resp_buf_reserve(out, total) != 0)
        return -1;
    p = out->data + start;
    memcpy(p, CLUSTER_BUS_MAGIC_V2, 4);
    put32(p + 4, (uint32_t)total);
    put16(p + 8, (uint16_t)CLUSTER_MSG_PUBLISH);
    put32(p + 10, (uint32_t)chlen);
    memcpy(p + 14, ch, chlen);
    put32(p + 14 + chlen, (uint32_t)mlen);
    memcpy(p + 18 + chlen, msg, mlen);
    out->len = start + total;
    return 0;
}

int cluster_bus_build_fail(struct db *d, const char *subject_id, resp_buf *out)
{
    size_t start;
    char *p;
    (void)d; /* FAIL frames carry no node record */
    if (subject_id == NULL || out == NULL)
        return -1;
    start = out->len;
    if (start > SIZE_MAX - 50)
        return -1;
    if (resp_buf_reserve(out, 50) != 0)
        return -1;
    p = out->data + start;
    memcpy(p, CLUSTER_BUS_MAGIC_V2, 4);
    put32(p + 4, 50);
    put16(p + 8, (uint16_t)CLUSTER_MSG_FAIL);
    memcpy(p + 10, subject_id, 40);
    out->len = start + 50;
    return 0;
}

int cluster_bus_handle_frame(struct db *d, const char *frame, size_t len,
                             resp_buf *reply_out, uint64_t now_ms)
{
    const char *p, *end;
    uint32_t totlen, flags;
    uint16_t type, ipl, port, busport;
    char id[41], gossip_id[41], ip[64], master_id[41];
    uint64_t epoch;
    int v2;
    cluster_node *n;

    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES ||
        frame == NULL || reply_out == NULL || len < 10)
        return -1;
    if (memcmp(frame, CLUSTER_BUS_MAGIC_V2, 4) == 0) {
        v2 = 1;
    } else if (memcmp(frame, CLUSTER_BUS_MAGIC_V1, 4) == 0) {
        v2 = 0;
    } else {
        return -1;
    }
    totlen = get32(frame + 4);
    if (totlen != len || totlen > CLUSTER_MSG_MAX)
        return -1;
    type = get16(frame + 8);
    if (type == CLUSTER_MSG_FAIL) {
        /* [RCM2][totlen=50][type=5][subject id]: force FAIL on receipt */
        cluster_node *f;
        char fid[41];
        if (totlen != 50)
            return -1;
        memcpy(fid, frame + 10, 40);
        fid[40] = '\0';
        f = cluster_node_find(d, fid);
        if (f != NULL)
            cluster_mark_fail(d, f, now_ms);
        return 0;
    }
    if (type < CLUSTER_MSG_PING || type > CLUSTER_MSG_MEET)
        return -1;

    p = frame + 10;
    end = frame + len;
    if ((size_t)(end - p) < 42)
        return -1;
    memcpy(id, p, 40);
    id[40] = '\0';
    if (!cluster_id_valid(id)) {
        return -1;
    }
    p += 40;
    ipl = get16(p);
    p += 2;
    if (ipl >= sizeof(ip) || (size_t)(end - p) < (size_t)ipl + 2056)
        return -1;
    memcpy(ip, p, ipl);
    ip[ipl] = '\0';
    p += ipl;
    port = get16(p);
    p += 2;
    busport = get16(p);
    p += 2;
    flags = get32(p);
    p += 4;

    if (v2) {
        if ((size_t)(end - p) < 2048 + 48)
            return -1;
        memcpy(master_id, p + 2048, 40);
        master_id[40] = '\0';
        if (!(master_id[0] == '-' && master_id[1] == '\0') &&
            !cluster_id_valid(master_id))
            return -1;
        epoch = get64(p + 2048 + 40);
    } else {
        epoch = 0; /* v1: no epochs on the wire */
    }

    /* Preflight the gossip tail before mutating the node table. */
    {
        const char *q = p + 2048 + (v2 ? 48 : 0);
        uint16_t gc, j;
        if ((size_t)(end - q) < 2)
            return -1;
        gc = get16(q);
        q += 2;
        for (j = 0; j < gc; j++) {
            uint16_t glen, slen;
            if ((size_t)(end - q) < 42)
                return -1;
            memcpy(gossip_id, q, 40);
            gossip_id[40] = '\0';
            if (!cluster_id_valid(gossip_id)) {
                return -1;
            }
            q += 40;
            glen = get16(q);
            q += 2;
            if (glen >= 64 || (size_t)(end - q) < (size_t)glen + 10)
                return -1;
            q += glen + 2 + 2 + 4;
            if ((size_t)(end - q) < 2)
                return -1;
            slen = get16(q);
            q += 2;
            if ((size_t)(end - q) < (size_t)slen)
                return -1;
            q += slen;
            if (v2) {
                if ((size_t)(end - q) < 48)
                    return -1;
                q += 48;
            }
        }
        if (q != end)
            return -1;
    }

    /* Validate the sender extension before publishing any topology state. */
    n = cluster_node_add(d, id);
    if (n == NULL)
        return -1;
    snprintf(n->ip, sizeof(n->ip), "%s", ip);
    n->port = port;
    n->bus_port = busport;
    n->flags = flags & ~(uint32_t)CLUSTER_NODE_MYSELF;
    if (type == CLUSTER_MSG_MEET)
        n->flags &= ~(uint32_t)CLUSTER_NODE_HANDSHAKE;
    n->last_seen_ms = now_ms;
    if (v2)
        snprintf(n->master_id, sizeof(n->master_id), "%s", master_id);
    /* slot claims go through epoch conflict resolution */
    cluster_merge_claims(d, n, (const uint8_t *)p, epoch);
    p += 2048;
    if (v2)
        p += 48;

    /* gossip entries */
    if ((size_t)(end - p) < 2)
        return -1;
    {
        uint16_t gc = get16(p);
        uint16_t j;
        p += 2;
        for (j = 0; j < gc; j++) {
            uint16_t sl;
            cluster_node *g;
            if ((size_t)(end - p) < 42)
                return -1;
            memcpy(id, p, 40);
            id[40] = '\0';
            if (!cluster_id_valid(id))
                return -1;
            p += 40;
            ipl = get16(p);
            p += 2;
            if ((size_t)(end - p) < (size_t)ipl + 10)
                return -1;
            if (ipl >= sizeof(ip))
                return -1;
            memcpy(ip, p, ipl);
            ip[ipl] = '\0';
            p += ipl;
            port = get16(p);
            p += 2;
            busport = get16(p);
            p += 2;
            flags = get32(p);
            p += 4;
            sl = get16(p);
            p += 2;
            if ((size_t)(end - p) < sl)
                return -1;
            if (v2) {
                if ((size_t)(end - p) < (size_t)sl + 48)
                    return -1;
                memcpy(master_id, p + sl, 40);
                master_id[40] = '\0';
                if (!(master_id[0] == '-' && master_id[1] == '\0') &&
                    !cluster_id_valid(master_id))
                    return -1;
            }
            /* unseen nodes are added, then claims merged (epoch rules);
             * known nodes only accept gossip at least as fresh as ours */
            g = cluster_node_find(d, id);
            /* failure reports: a master's PFAIL/FAIL gossip about a known
             * third node is a report from the frame sender; a clean view
             * retracts that sender's report (Redis gossip semantics) */
            if (g != NULL && !(g->flags & CLUSTER_NODE_MYSELF) &&
                (n->flags & CLUSTER_NODE_MASTER) != 0) {
                if (flags & (CLUSTER_NODE_PFAIL | CLUSTER_NODE_FAIL)) {
                    cluster_report_failure(d, g, n->id, now_ms);
                    (void)cluster_mark_fail_if_quorum(d, g, now_ms);
                } else {
                    cluster_report_heal(d, g, n->id);
                }
            }
            epoch = 0;
            if (v2)
                epoch = get64(p + sl + 40);
            if (g == NULL) {
                g = cluster_node_add(d, id);
                if (g != NULL) {
                    uint8_t gbm[2048];
                    snprintf(g->ip, sizeof(g->ip), "%s", ip);
                    g->port = port;
                    g->bus_port = busport;
                    g->flags = (flags & ~(uint32_t)CLUSTER_NODE_MYSELF) |
                               CLUSTER_NODE_HANDSHAKE;
                    if (v2) {
                        memcpy(g->master_id, p + sl, 40);
                        g->master_id[40] = '\0';
                    }
                    cluster_slots_parse(gbm, p, sl);
                    cluster_merge_claims(d, g, gbm, epoch);
                }
            } else if (v2 && epoch >= g->epoch) {
                uint8_t gbm[2048];
                cluster_slots_parse(gbm, p, sl);
                cluster_merge_claims(d, g, gbm, epoch);
            }
            p += sl;
            if (v2)
                p += 48;
        }
        if (p != end)
            return -1;
    }

    if (type != CLUSTER_MSG_PONG)
        if (cluster_bus_build_frame(d, CLUSTER_MSG_PONG, reply_out) != 0)
            return -1;
    d->cluster_changes++;
    d->slot_owner_dirty = 1;
    return 0;
}

cluster_node *cluster_myself(struct db *d)
{
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES)
        return NULL;
    for (i = 0; i < d->nnodes; i++)
        if (d->nodes[i].flags & CLUSTER_NODE_MYSELF)
            return &d->nodes[i];
    return NULL;
}

static uint64_t cluster_state_signature(const struct db *d)
{
    uint64_t sig = 1469598103934665603ULL;
    int i;
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES)
        return 0;
    for (i = 0; i < d->nnodes; i++) {
        sig ^= (uint64_t)d->nodes[i].flags;
        sig *= 1099511628211ULL;
        sig ^= d->nodes[i].epoch;
        sig *= 1099511628211ULL;
    }
    sig ^= (uint64_t)(unsigned)d->slot_owner_dirty;
    sig *= 1099511628211ULL;
    return sig;
}

int cluster_state_is_ok(struct db *d)
{
    uint64_t sig;
    uint8_t covered[2048];
    int covered_count = 0;
    int masters = 0, reachable = 0, fail_slots = 0;
    int i, s;

    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES)
        return d == NULL || !d->cluster_enabled ? 1 : 0;
    if (!d->cluster_enabled)
        return 1;
    sig = cluster_state_signature(d);
    if (d->cluster_state_cache_valid &&
        d->cluster_state_cache_changes == d->cluster_changes &&
        d->cluster_state_cache_signature == sig)
        return d->cluster_state_cache_ok;

    memset(covered, 0, sizeof(covered));
    for (i = 0; i < d->nnodes; i++) {
        int serves = 0;
        for (s = 0; s < 16384; s++) {
            if (!cluster_slots_get(d->nodes[i].slots, (uint32_t)s))
                continue;
            serves = 1;
            if (d->nodes[i].flags & CLUSTER_NODE_FAIL)
                fail_slots = 1;
            if (!cluster_slots_get(covered, (uint32_t)s)) {
                cluster_slots_set(covered, (uint32_t)s, 1);
                covered_count++;
            }
        }
        if ((d->nodes[i].flags & CLUSTER_NODE_MASTER) && serves) {
            masters++;
            if (!(d->nodes[i].flags &
                  (CLUSTER_NODE_FAIL | CLUSTER_NODE_DISCONNECTED)))
                reachable++;
        }
    }
    d->cluster_state_cache_changes = d->cluster_changes;
    d->cluster_state_cache_signature = sig;
    d->cluster_state_cache_valid = 1;
    d->cluster_state_cache_covered = covered_count;
    d->cluster_state_cache_masters = masters;
    d->cluster_state_cache_reachable = reachable;
    d->cluster_state_cache_fail_slots = fail_slots;
    d->cluster_state_cache_ok = covered_count == 16384 && !fail_slots &&
                                masters > 0 &&
                                reachable >= masters / 2 + 1;
    return d->cluster_state_cache_ok;
}

int cluster_state_is_minority(struct db *d)
{
    if (d == NULL || d->nnodes < 0 || d->nnodes > CLUSTER_MAX_NODES)
        return 0;
    (void)cluster_state_is_ok(d);
    return d != NULL && d->cluster_enabled &&
           d->cluster_state_cache_covered == 16384 &&
           d->cluster_state_cache_fail_slots == 0 &&
           d->cluster_state_cache_masters > 0 &&
           d->cluster_state_cache_reachable <
               d->cluster_state_cache_masters / 2 + 1;
}

void cluster_state_snapshot(const struct db *d, cluster_state *out)
{
    if (d == NULL || out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->cluster_enabled = d->cluster_enabled;
    memcpy(out->node_id, d->node_id, sizeof(d->node_id));
    memcpy(out->nodes, d->nodes, sizeof(d->nodes));
    out->nnodes = d->nnodes;
    memcpy(out->slot_owner, d->slot_owner, sizeof(d->slot_owner));
    out->slot_owner_dirty = d->slot_owner_dirty;
    memcpy(out->slot_migrating, d->slot_migrating,
           sizeof(d->slot_migrating));
    memcpy(out->slot_importing, d->slot_importing,
           sizeof(d->slot_importing));
    out->cluster_changes = d->cluster_changes;
    out->cluster_current_epoch = d->cluster_current_epoch;
    out->cluster_node_timeout_ms = d->cluster_node_timeout_ms;
    out->last_vote_epoch = d->last_vote_epoch;
    out->failover_req_epoch = d->failover_req_epoch;
    out->failover_ack_mask = d->failover_ack_mask;
    out->failover_ack_count = d->failover_ack_count;
    memcpy(out->fail_broadcast_id, d->fail_broadcast_id,
           sizeof(d->fail_broadcast_id));
    memcpy(out->cluster_ip, d->cluster_ip, sizeof(d->cluster_ip));
    out->cluster_port = d->cluster_port;
}

void cluster_state_restore(struct db *d, const cluster_state *in)
{
    int i;
    if (d == NULL || in == NULL)
        return;
    if (in->nnodes < 0 || in->nnodes > CLUSTER_MAX_NODES)
        return;
    for (i = 0; i < in->nnodes; i++)
        if (in->nodes[i].nreports < 0 ||
            in->nodes[i].nreports > CLUSTER_MAX_NODES)
            return;
    d->cluster_enabled = in->cluster_enabled;
    memcpy(d->node_id, in->node_id, sizeof(d->node_id));
    memcpy(d->nodes, in->nodes, sizeof(d->nodes));
    d->nnodes = in->nnodes;
    memcpy(d->slot_owner, in->slot_owner, sizeof(d->slot_owner));
    d->slot_owner_dirty = in->slot_owner_dirty;
    memcpy(d->slot_migrating, in->slot_migrating,
           sizeof(d->slot_migrating));
    memcpy(d->slot_importing, in->slot_importing,
           sizeof(d->slot_importing));
    d->cluster_changes = in->cluster_changes;
    d->cluster_state_cache_valid = 0;
    d->cluster_current_epoch = in->cluster_current_epoch;
    d->cluster_node_timeout_ms = in->cluster_node_timeout_ms;
    d->last_vote_epoch = in->last_vote_epoch;
    d->failover_req_epoch = in->failover_req_epoch;
    d->failover_ack_mask = in->failover_ack_mask;
    d->failover_ack_count = in->failover_ack_count;
    memcpy(d->fail_broadcast_id, in->fail_broadcast_id,
           sizeof(d->fail_broadcast_id));
    memcpy(d->cluster_ip, in->cluster_ip, sizeof(d->cluster_ip));
    d->cluster_port = in->cluster_port;
}
