/* cluster.c - single-node cluster mode: node identity; see cluster.h. */
#include "core/cluster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/command.h"
#include "pal/pal_file.h"

#include "pal/pal_time.h"

static void gen_node_id(char out[41])
{
    static const char hex[] = "0123456789abcdef";
    uint64_t x = pal_now_us() ^ 0x9E3779B97F4A7C15ULL;
    int i;
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

    gen_node_id(out_id);
    {
        pal_file *f = pal_file_open_write(path);
        char line[128];
        int len;
        if (f == NULL)
            return -1;
        len = snprintf(line, sizeof(line),
                       "%s :0@0 myself,master - 0 0 1 connected 0-16383\n",
                       out_id);
        if (pal_file_write(f, line, (size_t)len) != len) {
            pal_file_close(f);
            return -1;
        }
        pal_file_close(f);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* cluster node table                                                 */
/* ------------------------------------------------------------------ */

void cluster_nodes_init(struct db *d)
{
    d->nnodes = 0;
}

cluster_node *cluster_node_find(struct db *d, const char *id)
{
    int i;
    for (i = 0; i < d->nnodes; i++)
        if (memcmp(d->nodes[i].id, id, 40) == 0)
            return &d->nodes[i];
    return NULL;
}

cluster_node *cluster_node_add(struct db *d, const char *id)
{
    cluster_node *n = cluster_node_find(d, id);
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

void cluster_slots_set(uint8_t *bm, uint32_t slot, int on)
{
    if (on)
        bm[slot / 8] |= (uint8_t)(1u << (slot % 8));
    else
        bm[slot / 8] &= (uint8_t)~(1u << (slot % 8));
}

int cluster_slots_get(const uint8_t *bm, uint32_t slot)
{
    return (bm[slot / 8] >> (slot % 8)) & 1;
}

int cluster_slots_render(const uint8_t *bm, char *out, size_t cap)
{
    size_t n = 0;
    int s = -1;
    uint32_t i;
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
    memset(bm, 0, 2048);
    while (i < len) {
        unsigned long a = 0, b;
        while (i < len && s[i] == ' ')
            i++;
        if (i >= len)
            break;
        while (i < len && s[i] >= '0' && s[i] <= '9')
            a = a * 10 + (unsigned long)(s[i++] - '0');
        b = a;
        if (i < len && s[i] == '-') {
            i++;
            b = 0;
            while (i < len && s[i] >= '0' && s[i] <= '9')
                b = b * 10 + (unsigned long)(s[i++] - '0');
        }
        if (b > 16383)
            b = 16383;
        for (; a <= b; a++)
            cluster_slots_set(bm, (uint32_t)a, 1);
    }
}

uint64_t cluster_next_epoch(struct db *d)
{
    return ++d->cluster_current_epoch;
}

void cluster_merge_claims(struct db *d, cluster_node *claimant,
                          const uint8_t *bm, uint64_t epoch)
{
    cluster_node *me = cluster_myself(d);
    uint32_t s;
    int i;
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
    return f;
}

int cluster_nodes_render(struct db *d, resp_buf *out)
{
    int i;
    for (i = 0; i < d->nnodes; i++) {
        cluster_node *n = &d->nodes[i];
        char flags[64], slots[256];
        size_t start;
        int w;
        resp_buf_reserve(out, 512);
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
    unsigned port = 0, bus = 0;
    unsigned long long ping, pong, epoch;
    cluster_node *n;
    int used;

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
        ping = strtoull(p, (char **)&p, 10);
        while (p < end && *p == ' ')
            p++;
        pong = strtoull(p, (char **)&p, 10);
        while (p < end && *p == ' ')
            p++;
        epoch = strtoull(p, (char **)&p, 10);
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

    n = cluster_node_add(d, id);
    if (n == NULL)
        return -1;
    /* addr: ip:port@busport */
    {
        char *colon = strchr(addr, ':');
        char *at = strchr(addr, '@');
        if (colon != NULL)
            *colon = '\0';
        if (at != NULL) {
            *at = '\0';
            bus = (unsigned)strtoul(at + 1, NULL, 10);
        }
        if (colon != NULL)
            port = (unsigned)strtoul(colon + 1, NULL, 10);
        snprintf(n->ip, sizeof(n->ip), "%s", addr);
    }
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

void cluster_bus_build_frame(struct db *d, int type, resp_buf *out)
{
    const cluster_node *sn = NULL;
    size_t start, ipl, total;
    char *p, *gcp;
    uint16_t gc = 0;
    int i;

    for (i = 0; i < d->nnodes; i++)
        if (d->nodes[i].flags & CLUSTER_NODE_MYSELF) {
            sn = &d->nodes[i];
            break;
        }
    if (sn == NULL && d->nnodes > 0)
        sn = &d->nodes[0];
    if (sn == NULL)
        return;

    start = out->len;
    resp_buf_reserve(out, 4096);
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
    out->len = start + total;
    put32(out->data + start + 4, (uint32_t)total);
}

int cluster_bus_handle_frame(struct db *d, const char *frame, size_t len,
                             resp_buf *reply_out, uint64_t now_ms)
{
    const char *p, *end;
    uint32_t totlen, flags;
    uint16_t type, ipl, port, busport;
    char id[41], ip[64], master_id[41];
    uint64_t epoch;
    int v2;
    cluster_node *n;

    if (len < 10)
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
    if (type < CLUSTER_MSG_PING || type > CLUSTER_MSG_MEET)
        return -1;

    p = frame + 10;
    end = frame + len;
    if ((size_t)(end - p) < 42)
        return -1;
    memcpy(id, p, 40);
    id[40] = '\0';
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
    if (v2) {
        if ((size_t)(end - p) < 2048 + 48)
            return -1;
        memcpy(master_id, p + 2048, 40);
        master_id[40] = '\0';
        epoch = get64(p + 2048 + 40);
        snprintf(n->master_id, sizeof(n->master_id), "%s", master_id);
    } else {
        epoch = 0; /* v1: no epochs on the wire */
    }
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
            p += 40;
            ipl = get16(p);
            p += 2;
            if ((size_t)(end - p) < (size_t)ipl + 10)
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
            }
            /* unseen nodes are added, then claims merged (epoch rules);
             * known nodes only accept gossip at least as fresh as ours */
            g = cluster_node_find(d, id);
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
        cluster_bus_build_frame(d, CLUSTER_MSG_PONG, reply_out);
    d->cluster_changes++;
    d->slot_owner_dirty = 1;
    return 0;
}

cluster_node *cluster_myself(struct db *d)
{
    int i;
    for (i = 0; i < d->nnodes; i++)
        if (d->nodes[i].flags & CLUSTER_NODE_MYSELF)
            return &d->nodes[i];
    return NULL;
}
