/* cluster.h - cluster node identity, topology, and failure state.
 *
 * The node id is a stable 40-hex-char identifier, generated on first boot
 * and persisted in the cluster config file (Redis nodes.conf style, single
 * line). Multi-node gossip/MEET/migration are supported by the server
 * control plane; this header also exposes the shared metadata snapshot used
 * by mt workers.
 */
#ifndef DDUP_CLUSTER_H
#define DDUP_CLUSTER_H

#include <stddef.h>
#include <stdint.h>

#include "resp/resp_writer.h"

/* cluster_node flag bits (rendered as comma-separated nodes.conf flags). */
#define CLUSTER_NODE_MYSELF       (1u << 0)
#define CLUSTER_NODE_MASTER       (1u << 1)
#define CLUSTER_NODE_HANDSHAKE    (1u << 2)
#define CLUSTER_NODE_NOADDR       (1u << 3)
#define CLUSTER_NODE_DISCONNECTED (1u << 4)
#define CLUSTER_NODE_SLAVE        (1u << 5)
#define CLUSTER_NODE_PFAIL        (1u << 6) /* local suspicion (fail?) */
#define CLUSTER_NODE_FAIL         (1u << 7) /* quorum-confirmed (fail) */

/* node table cap (bounded for predictable memory use) */
#define CLUSTER_MAX_NODES 32

/* one failure report: `reporter` claims the node is down, valid for
 * cluster_node_timeout_ms * 2 (Redis NODE_TIMEOUT*2 rule) */
typedef struct cluster_fail_report {
    char reporter[41];
    uint64_t time_ms;
} cluster_fail_report;

typedef struct cluster_node {
    char id[41];
    char ip[64];
    uint16_t port;
    uint16_t bus_port;
    uint32_t flags;
    uint64_t last_seen_ms; /* last PONG (or direct contact) */
    uint64_t ping_sent_ms;
    uint64_t epoch;
    char master_id[41]; /* 40-hex master id, or "-" when a master */
    uint8_t slots[2048]; /* 16384-bit slot bitmap */
    cluster_fail_report reports[CLUSTER_MAX_NODES]; /* who suspects it */
    int nreports;
    uint64_t fail_time_ms; /* when FAIL was marked (0 = never) */
} cluster_node;

/* Immutable-by-convention copy of the cluster metadata carried by a db.
 * The mt server snapshots this on worker 0 and replays it on the other
 * workers, so every key-routing worker makes MOVED/CLUSTERDOWN decisions
 * from the same node/slot table without sharing the table itself. */
typedef struct cluster_state {
    int cluster_enabled;
    char node_id[41];
    cluster_node nodes[CLUSTER_MAX_NODES];
    int nnodes;
    uint16_t slot_owner[16384];
    int slot_owner_dirty;
    uint16_t slot_migrating[16384];
    uint16_t slot_importing[16384];
    uint64_t cluster_changes;
    uint64_t cluster_current_epoch;
    uint64_t cluster_node_timeout_ms;
    uint64_t last_vote_epoch;
    uint64_t failover_req_epoch;
    uint32_t failover_ack_mask;
    int failover_ack_count;
    char fail_broadcast_id[41];
    char cluster_ip[64];
    uint16_t cluster_port;
} cluster_state;

struct db; /* command.h */

/* Load the node id from path (first 40 hex chars of the file), or generate
 * and persist a new one when the file does not exist. Returns 0 on success
 * (out_id is NUL-terminated, 40 chars), -1 on unreadable/malformed file or
 * write failure. */
int cluster_node_id_load_or_create(const char *path, char out_id[41]);
/* Generate a random 40-hex id (node ids, replication ids). */
void cluster_gen_id(char out[41]);

/* node table */
void cluster_nodes_init(struct db *d);
cluster_node *cluster_node_find(struct db *d, const char *id);
cluster_node *cluster_node_add(struct db *d, const char *id);

/* the node flagged MYSELF, or NULL */
cluster_node *cluster_myself(struct db *d);

/* Return whether the cluster can safely serve data-plane commands. The
 * result is cached across unchanged topology/flag generations. */
int cluster_state_is_ok(struct db *d);
/* Return whether all slots are covered but this view has lost its master
 * majority. Incomplete coverage keeps the historical per-slot error. */
int cluster_state_is_minority(struct db *d);

/* Copy cluster metadata out of / into a db (data tables are untouched). */
void cluster_state_snapshot(const struct db *d, cluster_state *out);
void cluster_state_restore(struct db *d, const cluster_state *in);

/* slot bitmap */
void cluster_slots_set(uint8_t *bm, uint32_t slot, int on);
int cluster_slots_get(const uint8_t *bm, uint32_t slot);
/* render as "0-100 105 200-300" (empty string when empty); returns length */
int cluster_slots_render(const uint8_t *bm, char *out, size_t cap);
/* parse ranges/singles back into the bitmap */
void cluster_slots_parse(uint8_t *bm, const char *s, size_t len);

/* Next config epoch for a new slot claim (++db.cluster_current_epoch). */
uint64_t cluster_next_epoch(struct db *d);

/* Adopt a slot claim naming myself (Redis SETSLOT NODE / UPDATE-to-self):
 * take every bit in bm, clearing it from all other nodes; raise epochs. */
void cluster_adopt_claims(struct db *d, cluster_node *claimant,
                          const uint8_t *bm, uint64_t epoch);

/* Failover promotion: a slave becomes master, clears master_id, claims all
 * of its master's slots with a bumped config epoch. Returns 1 when the
 * promotion happened, 0 when myself is not a slave. */
int cluster_failover_promote(struct db *d);

/* Merge a wire slot claim into the local table: higher config epoch wins
 * a contested slot, ties go to the lexicographically larger node id
 * (Redis rule); losers' bits are cleared (myself yields too). Bits the
 * claimant holds locally but does not claim are retracted. The claimant's
 * epoch/current_epoch are raised to the wire epoch. A claim for myself's
 * own entry is never applied (our claims are locally authoritative). */
void cluster_merge_claims(struct db *d, cluster_node *claimant,
                          const uint8_t *bm, uint64_t epoch);

/* ------------------------------------------------------------------ */
/* failure reports (PFAIL quorum input)                               */
/* ------------------------------------------------------------------ */

/* Record/refresh reporter's suspicion of subject (time injected). */
void cluster_report_failure(struct db *d, cluster_node *subject,
                            const char *reporter, uint64_t now_ms);
/* Retract reporter's suspicion (its gossip shows the subject healthy). */
void cluster_report_heal(struct db *d, cluster_node *subject,
                         const char *reporter);
/* Count reports still inside the validity window
 * (db.cluster_node_timeout_ms * 2); expired entries are dropped. */
int cluster_report_count(struct db *d, cluster_node *subject,
                         uint64_t now_ms);

/* Force the objective FAIL state (a FAIL frame was received; receivers
 * honor it immediately). Clears PFAIL, records fail_time_ms, bumps
 * cluster_changes. No-op on myself or an already-FAIL node. */
void cluster_mark_fail(struct db *d, cluster_node *subject,
                       uint64_t now_ms);

/* Promote subject PFAIL -> FAIL when we suspect it locally AND a
 * majority of slot-serving masters (valid reports + myself if master)
 * suspect it too (Redis markNodeAsFailingIfNeeded). On promotion the
 * subject id lands in db.fail_broadcast_id for the server to broadcast
 * a FAIL frame, and 1 is returned. */
int cluster_mark_fail_if_quorum(struct db *d, cluster_node *subject,
                                uint64_t now_ms);

/* nodes.conf format: one line per node
 * "<id> <ip:port@busport> <flags> <master> <ping> <pong> <epoch>
 *  <link> <slots>" */
int cluster_nodes_render(struct db *d, resp_buf *out);
int cluster_nodes_parse_line(struct db *d, const char *line, size_t len);

/* ------------------------------------------------------------------ */
/* ddup cluster bus protocol (simplified; see docs/architecture.md)   */
/* ------------------------------------------------------------------ */
#define CLUSTER_MSG_PING 1
#define CLUSTER_MSG_PONG 2
#define CLUSTER_MSG_MEET 3
#define CLUSTER_MSG_PUBLISH 4
#define CLUSTER_MSG_FAIL 5
#define CLUSTER_MSG_MAX 16384

/* Wire magic: "RCMB" = v1 (no role/master_id/epoch fields; senders are
 * treated as masters), "RCM2" = v2 (per-node master_id + config epoch). */
#define CLUSTER_BUS_MAGIC_V1 "RCMB"
#define CLUSTER_BUS_MAGIC_V2 "RCM2"

/* Build a PING/PONG/MEET frame from the myself node + up to 10 gossip
 * entries (other known nodes). Returns 0 on success, -1 without changing
 * out->len on failure. */
int cluster_bus_build_frame(struct db *d, int type, resp_buf *out);

/* Parse one frame defensively: upsert the sender, merge gossip entries,
 * update last_seen (MEET additionally completes the handshake). On PING or
 * MEET a PONG frame is appended to reply_out (empty on PONG). Returns 0 on
 * success, -1 on malformed input. */
int cluster_bus_handle_frame(struct db *d, const char *frame, size_t len,
                             resp_buf *reply_out, uint64_t now_ms);

/* Build a PUBLISH frame (shard channel fan-out): [RCM2][totlen u32le]
 * [type u16le=4][u32le chlen][channel][u32le msglen][message]. These
 * frames carry no node record; receivers deliver to local shard
 * subscribers and never feed them to cluster_bus_handle_frame. Returns 0
 * on success, -1 without changing out->len on failure. */
int cluster_bus_build_publish(struct db *d, const char *ch, size_t chlen,
                              const char *msg, size_t mlen, resp_buf *out);

/* Build a FAIL frame: [RCM2][totlen=50][type u16le=5][subject id 40].
 * Receivers mark the subject FAIL immediately (Redis FAIL semantics).
 * Returns 0 on success, -1 without changing out->len on failure. */
int cluster_bus_build_fail(struct db *d, const char *subject_id, resp_buf *out);

#endif /* DDUP_CLUSTER_H */
