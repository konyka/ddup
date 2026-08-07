/* redbus.h - Redis cluster bus wire codec (clusterMsg), big-endian.
 *
 * Frame layout (Redis 7.0 static-assert offsets, verified against
 * redis/redis@7.0 src/cluster.h and src/cluster.c):
 *   sig[4] "RCmb" @0, u32 totlen BE @4, u16 ver @8, u16 port BE @10,
 *   u16 type BE @12, u16 count BE @14, u64 currentEpoch BE @16,
 *   u64 configEpoch BE @24, u64 offset BE @32, sender[40] @40,
 *   myslots[2048] @80, slaveof[40] @2128, myip[46] @2168,
 *   u16 extensions @2214, notused1[30] @2216, u16 pport BE @2246,
 *   u16 cport BE @2248, u16 flags BE @2250, u8 state @2252,
 *   u8 mflags[3] @2253, data @2256.
 * Gossip entry (104 bytes each): nodename[40], u32 ping_sent BE,
 *   u32 pong_received BE, ip[46], u16 port BE, u16 cport BE,
 *   u16 flags BE, u16 pport BE, u16 notused1 BE.
 * All integers are BIG-ENDIAN on the wire.
 */
#ifndef DDUP_REDBUS_H
#define DDUP_REDBUS_H

#include <stddef.h>
#include <stdint.h>

#include "core/command.h"
#include "resp/resp_writer.h"

#define REDBUS_TYPE_PING 0
#define REDBUS_TYPE_PONG 1
#define REDBUS_TYPE_MEET 2
#define REDBUS_TYPE_FAIL 3
#define REDBUS_TYPE_PUBLISH 4
#define REDBUS_TYPE_AUTH_REQUEST 5 /* Redis 7.0 numbering (verified) */
#define REDBUS_TYPE_AUTH_ACK 6
#define REDBUS_TYPE_UPDATE 7
#define REDBUS_TYPE_PUBLISHSHARD 10 /* Redis 7.0 numbering (verified) */

/* redis node flag bits (subset we map; rest tolerated) */
#define REDBUS_NODE_MASTER 1
#define REDBUS_NODE_SLAVE 2
#define REDBUS_NODE_PFAIL 4
#define REDBUS_NODE_FAIL 8
#define REDBUS_NODE_MYSELF 16
#define REDBUS_NODE_HANDSHAKE 32
#define REDBUS_NODE_NOADDR 64

#define REDBUS_HDR_LEN 2256
#define REDBUS_GOSSIP_LEN 104
#define REDBUS_GOSSIP_MAX 10

/* Build a PING/PONG/MEET frame from the myself node + up to 10 gossip
 * entries (other known nodes), Redis wire format. */
void redbus_build_frame(struct db *d, int type, resp_buf *out);

/* Parse one frame defensively: upsert the sender (slots via
 * cluster_merge_claims with configEpoch), merge gossip entries,
 * update last_seen; MEET additionally completes the handshake; FAIL
 * marks the named node disconnected; UPDATE applies a slot claim
 * (adopting it when it names myself); FAILOVER_AUTH_REQUEST is answered
 * with an ACK frame when the vote-grant conditions hold; AUTH_ACKs are
 * counted into the pending election; other types are tolerated
 * (ignored). src_ip (may be NULL) is the connection's peer address,
 * used when the frame's myip is empty (redis auto-discovery: myip is
 * only filled when cluster-announce-ip is configured). On PING or
 * MEET a PONG frame is appended to reply_out (empty on PONG).
 * Returns 0, -1 on malformed. */
int redbus_handle_frame(struct db *d, const char *frame, size_t len,
                        resp_buf *reply_out, uint64_t now_ms,
                        const char *src_ip);

/* Build a FAILOVER_AUTH_REQUEST for an in-flight election: PING-like
 * frame whose slots/configEpoch alias the dead master (Redis
 * clusterRequestFailoverAuth semantics) and whose currentEpoch is the
 * election epoch. */
void redbus_build_auth_request(struct db *d, uint64_t election_epoch,
                               resp_buf *out);

/* Build a PUBLISH (type 4) or PUBLISHSHARD (type 10) frame: full header
 * with count=0 and no gossip entries, then the clusterMsgDataPublish
 * payload u32BE channel_len, u32BE message_len, channel, message. */
void redbus_build_publish(struct db *d, int type, const char *ch,
                          size_t chlen, const char *msg, size_t mlen,
                          resp_buf *out);

/* Build a FAIL frame: full header (count=0, no gossip) + the
 * clusterMsgDataFail payload nodename[40]. Receivers mark the subject
 * FAIL immediately. */
void redbus_build_fail(struct db *d, const char *subject_id,
                       resp_buf *out);

#endif /* DDUP_REDBUS_H */
