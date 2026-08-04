/* cluster.h - single-node cluster mode: node identity.
 *
 * The node id is a stable 40-hex-char identifier, generated on first boot
 * and persisted in the cluster config file (Redis nodes.conf style, single
 * line). Multi-node gossip/MEET/migration is explicitly out of scope.
 */
#ifndef DDUP_CLUSTER_H
#define DDUP_CLUSTER_H

/* Load the node id from path (first 40 hex chars of the file), or generate
 * and persist a new one when the file does not exist. Returns 0 on success
 * (out_id is NUL-terminated, 40 chars), -1 on unreadable/malformed file or
 * write failure. */
int cluster_node_id_load_or_create(const char *path, char out_id[41]);

#endif /* DDUP_CLUSTER_H */
