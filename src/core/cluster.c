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

static void flags_render(uint32_t flags, char *out, size_t cap)
{
    out[0] = '\0';
    if (flags & CLUSTER_NODE_MYSELF)
        strcat(out, "myself,");
    if (flags & CLUSTER_NODE_MASTER)
        strcat(out, "master,");
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
                     "%s %s:%u@%u %s - %llu %llu %llu %s %s\n", n->id, n->ip,
                     (unsigned)n->port, (unsigned)n->bus_port, flags,
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
    char id[41], ip[64], flags[64], slots[512], addr[128];
    unsigned port = 0, bus = 0;
    unsigned long long ping, pong, epoch;
    cluster_node *n;
    int used;

    memset(ip, 0, sizeof(ip));
    memset(flags, 0, sizeof(flags));
    memset(slots, 0, sizeof(slots));
    memset(addr, 0, sizeof(addr));
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
        (void)mlen;
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
    cluster_slots_parse(n->slots, slots, strlen(slots));
    return 0;
}
