/* cluster.h - single-node cluster mode: node identity.
 *
 * The node id is a stable 40-hex-char identifier, generated on first boot
 * and persisted in the cluster config file (Redis nodes.conf style, single
 * line). Multi-node gossip/MEET/migration is explicitly out of scope.
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

/* node table cap (documented; single node table per db) */
#define CLUSTER_MAX_NODES 32

typedef struct cluster_node {
    char id[41];
    char ip[64];
    uint16_t port;
    uint16_t bus_port;
    uint32_t flags;
    uint64_t last_seen_ms; /* last PONG (or direct contact) */
    uint64_t ping_sent_ms;
    uint64_t epoch;
    uint8_t slots[2048]; /* 16384-bit slot bitmap */
} cluster_node;

struct db; /* command.h */

/* Load the node id from path (first 40 hex chars of the file), or generate
 * and persist a new one when the file does not exist. Returns 0 on success
 * (out_id is NUL-terminated, 40 chars), -1 on unreadable/malformed file or
 * write failure. */
int cluster_node_id_load_or_create(const char *path, char out_id[41]);

/* node table */
void cluster_nodes_init(struct db *d);
cluster_node *cluster_node_find(struct db *d, const char *id);
cluster_node *cluster_node_add(struct db *d, const char *id);

/* slot bitmap */
void cluster_slots_set(uint8_t *bm, uint32_t slot, int on);
int cluster_slots_get(const uint8_t *bm, uint32_t slot);
/* render as "0-100 105 200-300" (empty string when empty); returns length */
int cluster_slots_render(const uint8_t *bm, char *out, size_t cap);
/* parse ranges/singles back into the bitmap */
void cluster_slots_parse(uint8_t *bm, const char *s, size_t len);

/* nodes.conf format: one line per node
 * "<id> <ip:port@busport> <flags> <master> <ping> <pong> <epoch>
 *  <link> <slots>" */
int cluster_nodes_render(struct db *d, resp_buf *out);
int cluster_nodes_parse_line(struct db *d, const char *line, size_t len);

#endif /* DDUP_CLUSTER_H */
