/* redbus.c - Redis cluster bus wire codec; layout documented in redbus.h. */
#include "core/redbus.h"

#include <stdio.h>
#include <string.h>

#include "core/cluster.h"

/* ------------------------------------------------------------------ */
/* big-endian helpers                                                 */
/* ------------------------------------------------------------------ */

static void put16be(char *p, uint16_t v)
{
    p[0] = (char)((v >> 8) & 0xFFu);
    p[1] = (char)(v & 0xFFu);
}

static void put32be(char *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++)
        p[i] = (char)((v >> (24 - 8 * i)) & 0xFFu);
}

static void put64be(char *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (char)((v >> (56 - 8 * i)) & 0xFFu);
}

static uint16_t get16be(const char *p)
{
    return (uint16_t)(((uint16_t)(uint8_t)p[0] << 8) |
                      (uint16_t)(uint8_t)p[1]);
}

static uint32_t get32be(const char *p)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++)
        v = (v << 8) | (uint32_t)(uint8_t)p[i];
    return v;
}

static uint64_t get64be(const char *p)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++)
        v = (v << 8) | (uint64_t)(uint8_t)p[i];
    return v;
}

/* ------------------------------------------------------------------ */
/* flag mapping ddup <-> redis                                        */
/* ------------------------------------------------------------------ */

static uint16_t flags_to_wire(const cluster_node *n)
{
    uint16_t f = 0;
    if (n->flags & CLUSTER_NODE_MYSELF)
        f |= REDBUS_NODE_MYSELF;
    if (n->flags & CLUSTER_NODE_MASTER)
        f |= REDBUS_NODE_MASTER;
    if (n->flags & CLUSTER_NODE_SLAVE)
        f |= REDBUS_NODE_SLAVE;
    if (n->flags & CLUSTER_NODE_HANDSHAKE)
        f |= REDBUS_NODE_HANDSHAKE;
    if (n->flags & CLUSTER_NODE_NOADDR)
        f |= REDBUS_NODE_NOADDR;
    if (n->flags & CLUSTER_NODE_DISCONNECTED)
        f |= REDBUS_NODE_FAIL; /* PFAIL left unset (documented) */
    return f;
}

static uint32_t flags_from_wire(uint16_t f)
{
    uint32_t g = 0;
    if (f & REDBUS_NODE_MASTER)
        g |= CLUSTER_NODE_MASTER;
    if (f & REDBUS_NODE_SLAVE)
        g |= CLUSTER_NODE_SLAVE;
    if (f & REDBUS_NODE_HANDSHAKE)
        g |= CLUSTER_NODE_HANDSHAKE;
    if (f & REDBUS_NODE_NOADDR)
        g |= CLUSTER_NODE_NOADDR;
    if (f & (REDBUS_NODE_FAIL | REDBUS_NODE_PFAIL))
        g |= CLUSTER_NODE_DISCONNECTED; /* PFAIL mapped too (documented) */
    return g;
}

/* write a 40-hex id (or zeros for "-") into a fixed 40-byte field */
static void put_name(char *p, const char *id)
{
    memset(p, 0, 40);
    if (id[0] != '-' && id[0] != '\0') {
        size_t l = strlen(id);
        if (l > 40)
            l = 40;
        memcpy(p, id, l);
    }
}

/* 1 when the 40-byte wire name is all zeros */
static int name_is_zero(const char *p)
{
    int i;
    for (i = 0; i < 40; i++)
        if (p[i] != 0)
            return 0;
    return 1;
}

static void put_ip(char *p, const char *ip)
{
    size_t l = strlen(ip);
    memset(p, 0, 46);
    if (l > 45)
        l = 45;
    memcpy(p, ip, l);
}

/* ------------------------------------------------------------------ */
/* build                                                              */
/* ------------------------------------------------------------------ */

static void build_gossip_entry(char *p, const cluster_node *n)
{
    memset(p, 0, REDBUS_GOSSIP_LEN);
    put_name(p, n->id);
    put32be(p + 40, (uint32_t)(n->ping_sent_ms / 1000));
    put32be(p + 44, (uint32_t)(n->last_seen_ms / 1000));
    put_ip(p + 48, n->ip);
    put16be(p + 94, n->port);
    put16be(p + 96, n->bus_port);
    put16be(p + 98, flags_to_wire(n));
    /* pport @100, notused1 @102: zero */
}

void redbus_build_frame(struct db *d, int type, resp_buf *out)
{
    const cluster_node *sn = NULL;
    size_t start, total;
    char *p, *cp;
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
    resp_buf_reserve(out, REDBUS_HDR_LEN +
                             REDBUS_GOSSIP_MAX * REDBUS_GOSSIP_LEN + 16);
    p = out->data + out->len;
    memset(p, 0, REDBUS_HDR_LEN);
    memcpy(p, "RCmb", 4);
    /* totlen patched below */
    put16be(p + 8, 1); /* protocol version 1 */
    put16be(p + 10, sn->port);
    put16be(p + 12, (uint16_t)type);
    /* count patched below */
    put64be(p + 16, d->cluster_current_epoch);
    put64be(p + 24, sn->epoch);
    put64be(p + 32, 0); /* replication offset: unused on the bus */
    put_name(p + 40, sn->id);
    memcpy(p + 80, sn->slots, 2048);
    put_name(p + 2128, sn->master_id);
    put_ip(p + 2168, sn->ip);
    /* extensions @2214, notused1 @2216, pport @2246: zero */
    put16be(p + 2248, sn->bus_port);
    put16be(p + 2250, flags_to_wire(sn));
    p[2252] = 0; /* CLUSTER_OK */
    /* mflags[3] @2253: zero */
    cp = p + REDBUS_HDR_LEN;

    for (i = 0; i < d->nnodes && gc < REDBUS_GOSSIP_MAX; i++) {
        const cluster_node *n = &d->nodes[i];
        if (n == sn)
            continue;
        build_gossip_entry(cp, n);
        cp += REDBUS_GOSSIP_LEN;
        gc++;
    }
    put16be(p + 14, gc);

    total = (size_t)(cp - p);
    out->len = start + total;
    put32be(out->data + start + 4, (uint32_t)total);
}

/* ------------------------------------------------------------------ */
/* parse                                                              */
/* ------------------------------------------------------------------ */

/* apply a wire node record (sender or gossip) to the local table;
 * slots only present for the sender (gossip entries carry none).
 * An empty wire ip falls back to src_ip (redis auto-discovery); a
 * known ip is never blanked by an empty one. */
static cluster_node *apply_node(struct db *d, const char *id, const char *ip,
                                uint16_t port, uint16_t cport, uint16_t wflags,
                                const char *slaveof, uint64_t epoch,
                                uint32_t extra_flags, const char *src_ip)
{
    cluster_node *n = cluster_node_add(d, id);
    const char *use_ip = ip[0] != '\0' ? ip : src_ip;
    if (n == NULL)
        return NULL;
    if (use_ip != NULL && use_ip[0] != '\0')
        snprintf(n->ip, sizeof(n->ip), "%s", use_ip);
    n->port = port;
    n->bus_port = cport;
    n->flags = flags_from_wire(wflags) | extra_flags;
    if (slaveof != NULL) {
        if (name_is_zero(slaveof))
            snprintf(n->master_id, sizeof(n->master_id), "-");
        else {
            memcpy(n->master_id, slaveof, 40);
            n->master_id[40] = '\0';
        }
    }
    if (epoch > n->epoch)
        n->epoch = epoch;
    if (epoch > d->cluster_current_epoch)
        d->cluster_current_epoch = epoch;
    return n;
}

int redbus_handle_frame(struct db *d, const char *frame, size_t len,
                        resp_buf *reply_out, uint64_t now_ms,
                        const char *src_ip)
{
    const char *p;
    uint32_t totlen;
    uint16_t type, port, cport, wflags, count, j;
    uint64_t current_epoch, config_epoch;
    char id[41], ip[47], slaveof[41];
    cluster_node *n;

    if (len < REDBUS_HDR_LEN || memcmp(frame, "RCmb", 4) != 0)
        return -1;
    totlen = get32be(frame + 4);
    if (totlen != len || totlen < REDBUS_HDR_LEN)
        return -1;
    type = get16be(frame + 12);
    count = get16be(frame + 14);
    if (len < REDBUS_HDR_LEN + (size_t)count * REDBUS_GOSSIP_LEN)
        return -1;

    if (type == REDBUS_TYPE_UPDATE) {
        /* clusterMsgDataUpdate: u64 configEpoch BE, nodename[40], slots */
        const char *u = frame + REDBUS_HDR_LEN;
        cluster_node *owner;
        uint64_t uepoch;
        if (len < REDBUS_HDR_LEN + 8 + 40 + 2048)
            return -1;
        uepoch = get64be(u);
        memcpy(id, u + 8, 40);
        id[40] = '\0';
        owner = cluster_node_find(d, id);
        if (owner != NULL) {
            if (owner->flags & CLUSTER_NODE_MYSELF)
                cluster_adopt_claims(d, owner, (const uint8_t *)(u + 48),
                                     uepoch);
            else
                cluster_merge_claims(d, owner, (const uint8_t *)(u + 48),
                                     uepoch);
        }
        if (uepoch > d->cluster_current_epoch)
            d->cluster_current_epoch = uepoch;
        return 0;
    }
    if (type == REDBUS_TYPE_FAIL) {
        /* clusterMsgDataFail: nodename[40] */
        if (len < REDBUS_HDR_LEN + 40)
            return -1;
        memcpy(id, frame + REDBUS_HDR_LEN, 40);
        id[40] = '\0';
        n = cluster_node_find(d, id);
        if (n != NULL) {
            n->flags |= CLUSTER_NODE_DISCONNECTED;
            d->cluster_changes++;
        }
        return 0;
    }
    if (type != REDBUS_TYPE_PING && type != REDBUS_TYPE_PONG &&
        type != REDBUS_TYPE_MEET) {
        return 0; /* PUBLISH and friends: tolerated, ignored */
    }

    port = get16be(frame + 10);
    current_epoch = get64be(frame + 16);
    config_epoch = get64be(frame + 24);
    memcpy(id, frame + 40, 40);
    id[40] = '\0';
    memcpy(slaveof, frame + 2128, 40);
    slaveof[40] = '\0';
    memcpy(ip, frame + 2168, 46);
    ip[46] = '\0';
    cport = get16be(frame + 2248);
    wflags = get16be(frame + 2250);

    n = apply_node(d, id, ip, port, cport, wflags, slaveof, config_epoch, 0,
                   src_ip);
    if (n == NULL)
        return -1;
    if (type == REDBUS_TYPE_MEET)
        n->flags &= ~(uint32_t)CLUSTER_NODE_HANDSHAKE;
    cluster_merge_claims(d, n, (const uint8_t *)(frame + 80), config_epoch);
    n->last_seen_ms = now_ms;
    if (current_epoch > d->cluster_current_epoch)
        d->cluster_current_epoch = current_epoch;

    /* gossip entries: register nodes (no slot data on the wire) */
    p = frame + REDBUS_HDR_LEN;
    for (j = 0; j < count; j++) {
        uint16_t gflags;
        memcpy(id, p, 40);
        id[40] = '\0';
        memcpy(ip, p + 48, 46);
        ip[46] = '\0';
        gflags = get16be(p + 98);
        /* only add when unseen (third-party records never overwrite) */
        if (cluster_node_find(d, id) == NULL) {
            (void)apply_node(d, id, ip, get16be(p + 94), get16be(p + 96),
                             gflags, NULL, 0, CLUSTER_NODE_HANDSHAKE, src_ip);
        }
        p += REDBUS_GOSSIP_LEN;
    }

    if (type != REDBUS_TYPE_PONG)
        redbus_build_frame(d, REDBUS_TYPE_PONG, reply_out);
    d->cluster_changes++;
    d->slot_owner_dirty = 1;
    return 0;
}
