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
    if (n->flags & CLUSTER_NODE_PFAIL)
        f |= REDBUS_NODE_PFAIL;
    if (n->flags & CLUSTER_NODE_FAIL)
        f |= REDBUS_NODE_FAIL;
    /* CLUSTER_NODE_DISCONNECTED is a local link state: never on the wire */
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
    if (f & REDBUS_NODE_PFAIL)
        g |= CLUSTER_NODE_PFAIL;
    if (f & REDBUS_NODE_FAIL)
        g |= CLUSTER_NODE_FAIL;
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

/* the node whose identity goes into the frame header: myself, falling
 * back to the first known node (pre-handshake corner) */
static const cluster_node *find_sender(struct db *d)
{
    int i;
    for (i = 0; i < d->nnodes; i++)
        if (d->nodes[i].flags & CLUSTER_NODE_MYSELF)
            return &d->nodes[i];
    return d->nnodes > 0 ? &d->nodes[0] : NULL;
}

/* write the 2256-byte header (count left 0, totlen patched by caller);
 * returns the payload cursor past the header, NULL without a sender */
static char *write_header(struct db *d, const cluster_node *sn, int type,
                          char *p)
{
    memset(p, 0, REDBUS_HDR_LEN);
    memcpy(p, "RCmb", 4);
    /* totlen patched by caller */
    put16be(p + 8, 1); /* protocol version 1 */
    put16be(p + 10, sn->port);
    put16be(p + 12, (uint16_t)type);
    /* count patched by caller (0 default from memset) */
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
    return p + REDBUS_HDR_LEN;
}

void redbus_build_frame(struct db *d, int type, resp_buf *out)
{
    const cluster_node *sn = find_sender(d);
    size_t start, total;
    char *p, *cp;
    uint16_t gc = 0;
    int i;

    if (sn == NULL)
        return;

    start = out->len;
    resp_buf_reserve(out, REDBUS_HDR_LEN +
                             REDBUS_GOSSIP_MAX * REDBUS_GOSSIP_LEN + 16);
    p = out->data + out->len;
    cp = write_header(d, sn, type, p);

    /* FAILOVER_AUTH_REQUEST/ACK are header-only messages in Redis
     * (receivers reject them with gossip attached) */
    if (type != REDBUS_TYPE_AUTH_REQUEST && type != REDBUS_TYPE_AUTH_ACK)
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

void redbus_build_publish(struct db *d, int type, const char *ch,
                          size_t chlen, const char *msg, size_t mlen,
                          resp_buf *out)
{
    const cluster_node *sn = find_sender(d);
    size_t start, total;
    char *p, *cp;

    if (sn == NULL)
        return;

    start = out->len;
    resp_buf_reserve(out, REDBUS_HDR_LEN + 8 + chlen + mlen);
    p = out->data + out->len;
    cp = write_header(d, sn, type, p); /* count stays 0: no gossip */
    put32be(cp, (uint32_t)chlen);
    put32be(cp + 4, (uint32_t)mlen);
    memcpy(cp + 8, ch, chlen);
    memcpy(cp + 8 + chlen, msg, mlen);
    cp += 8 + chlen + mlen;

    total = (size_t)(cp - p);
    out->len = start + total;
    put32be(out->data + start + 4, (uint32_t)total);
}

void redbus_build_fail(struct db *d, const char *subject_id, resp_buf *out)
{
    const cluster_node *sn = find_sender(d);
    size_t start, total;
    char *p, *cp;

    if (sn == NULL)
        return;

    start = out->len;
    resp_buf_reserve(out, REDBUS_HDR_LEN + 40);
    p = out->data + out->len;
    cp = write_header(d, sn, REDBUS_TYPE_FAIL, p); /* count stays 0 */
    put_name(cp, subject_id); /* clusterMsgDataFail: nodename[40] */
    cp += 40;

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

/* grant a failover vote when the Redis conditions hold (7.0 semantics;
 * the voted_time throttle is the documented omission): voter is a master
 * serving slots, req epoch >= current, not yet voted this epoch, requester
 * is a slave whose master is marked failed, and the claim's configEpoch
 * dominates every current slot owner. On grant: record last_vote_epoch and
 * append an AUTH_ACK frame to reply_out. */
static void handle_auth_request(struct db *d, const char *frame,
                                resp_buf *reply_out)
{
    cluster_node *me = cluster_myself(d);
    uint64_t req_cur = get64be(frame + 16);
    uint64_t req_cfg = get64be(frame + 24);
    char id[41];
    cluster_node *sender, *master;
    uint32_t s;

    if (me == NULL || !(me->flags & CLUSTER_NODE_MASTER))
        return;
    for (s = 0; s < 16384; s++)
        if (cluster_slots_get(me->slots, s))
            break;
    if (s == 16384)
        return; /* masters without slots have no vote */
    if (req_cur > d->cluster_current_epoch)
        d->cluster_current_epoch = req_cur;
    if (req_cur < d->cluster_current_epoch)
        return; /* stale election epoch */
    if (d->last_vote_epoch == d->cluster_current_epoch)
        return; /* already voted in this epoch */

    memcpy(id, frame + 40, 40);
    id[40] = '\0';
    sender = cluster_node_find(d, id);
    if (sender == NULL || !(sender->flags & CLUSTER_NODE_SLAVE))
        return;
    if (sender->master_id[0] == '-' || sender->master_id[0] == '\0')
        return;
    master = cluster_node_find(d, sender->master_id);
    if (master == NULL || !(master->flags & CLUSTER_NODE_DISCONNECTED))
        return; /* its master is not failing in our view */

    /* the claimed slots must dominate their current owners' epochs */
    for (s = 0; s < 16384; s++) {
        int j;
        if (!(frame[80 + s / 8] & (1u << (s % 8))))
            continue;
        for (j = 0; j < d->nnodes; j++) {
            cluster_node *o = &d->nodes[j];
            if (o != sender && cluster_slots_get(o->slots, s) &&
                o->epoch > req_cfg)
                return; /* a live owner has a newer claim */
        }
    }
    d->last_vote_epoch = d->cluster_current_epoch;
    redbus_build_frame(d, REDBUS_TYPE_AUTH_ACK, reply_out);
}

static void handle_auth_ack(struct db *d, const char *frame)
{
    char id[41];
    cluster_node *sender;
    uint64_t ack_epoch = get64be(frame + 16);
    int idx;
    if (d->failover_req_epoch == 0 || ack_epoch < d->failover_req_epoch)
        return;
    memcpy(id, frame + 40, 40);
    id[40] = '\0';
    sender = cluster_node_find(d, id);
    if (sender == NULL || !(sender->flags & CLUSTER_NODE_MASTER))
        return;
    for (idx = 0; idx < 16384; idx++)
        if (cluster_slots_get(sender->slots, (uint32_t)idx))
            break;
    if (idx == 16384)
        return; /* only masters serving slots count */
    idx = (int)(sender - d->nodes);
    if (idx < 32 && !(d->failover_ack_mask & (1u << idx))) {
        d->failover_ack_mask |= 1u << idx;
        d->failover_ack_count++;
    }
}

void redbus_build_auth_request(struct db *d, uint64_t election_epoch,
                               resp_buf *out)
{
    size_t start = out->len;
    cluster_node *me = cluster_myself(d);
    cluster_node *m = (me != NULL && me->master_id[0] != '-')
                          ? cluster_node_find(d, me->master_id)
                          : NULL;
    redbus_build_frame(d, REDBUS_TYPE_AUTH_REQUEST, out);
    if (out->len == start)
        return; /* no myself node: nothing was built */
    put64be(out->data + start + 16, election_epoch);
    if (m != NULL) {
        /* alias the dead master's claim (Redis: a slave announces its
         * master's slots and configEpoch with the request) */
        put64be(out->data + start + 24, m->epoch);
        memcpy(out->data + start + 80, m->slots, 2048);
    }
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
        /* clusterMsgDataFail: nodename[40]; force FAIL on receipt */
        if (len < REDBUS_HDR_LEN + 40)
            return -1;
        memcpy(id, frame + REDBUS_HDR_LEN, 40);
        id[40] = '\0';
        n = cluster_node_find(d, id);
        if (n != NULL)
            cluster_mark_fail(d, n, now_ms);
        return 0;
    }
    if (type == REDBUS_TYPE_AUTH_REQUEST) {
        if (len < REDBUS_HDR_LEN)
            return -1;
        handle_auth_request(d, frame, reply_out);
        return 0;
    }
    if (type == REDBUS_TYPE_AUTH_ACK) {
        if (len < REDBUS_HDR_LEN)
            return -1;
        handle_auth_ack(d, frame);
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
        cluster_node *g;
        memcpy(id, p, 40);
        id[40] = '\0';
        memcpy(ip, p + 48, 46);
        ip[46] = '\0';
        gflags = get16be(p + 98);
        g = cluster_node_find(d, id);
        /* only add when unseen (third-party records never overwrite) */
        if (g == NULL) {
            (void)apply_node(d, id, ip, get16be(p + 94), get16be(p + 96),
                             gflags, NULL, 0, CLUSTER_NODE_HANDSHAKE, src_ip);
        } else if (!(g->flags & CLUSTER_NODE_MYSELF) &&
                   (n->flags & CLUSTER_NODE_MASTER) != 0) {
            /* failure reports: a master's PFAIL/FAIL gossip about a known
             * third node is a report from the sender; clean retracts it */
            if (gflags & (REDBUS_NODE_PFAIL | REDBUS_NODE_FAIL)) {
                cluster_report_failure(d, g, n->id, now_ms);
                (void)cluster_mark_fail_if_quorum(d, g, now_ms);
            } else {
                cluster_report_heal(d, g, n->id);
            }
        }
        p += REDBUS_GOSSIP_LEN;
    }

    if (type != REDBUS_TYPE_PONG)
        redbus_build_frame(d, REDBUS_TYPE_PONG, reply_out);
    d->cluster_changes++;
    d->slot_owner_dirty = 1;
    return 0;
}
