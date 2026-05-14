/*
 * jce_net_replication.h  Entity replication / RPC data layer.
 *
 * Pure-CPU snapshot serialisation + delta encoding + RPC headers.
 * Decoupled from the actual transport (ENet, WebSocket, custom UDP);
 * the caller serialises a snapshot to a byte buffer with this module
 * and ships the buffer however they like.
 *
 * Wire format is little-endian, 32-bit aligned.  Both client and
 * server use the same encoder/decoder; "authority" is a logical layer
 * on top — this module doesn't enforce it.
 *
 * Snapshot anatomy:
 *
 *   [JceNetSnapHeader]                       (12 B)
 *     uint32 protocol_version
 *     uint16 entity_count
 *     uint16 reserved
 *     uint32 server_tick      // snapshot's logical clock
 *
 *   [JceNetEntitySnap × entity_count]
 *     uint32 entity_id
 *     uint32 component_mask   // which fields are present in this entry
 *     // followed by per-field payloads in mask-bit order:
 *     //   FIELD_TRANSFORM   → 10 floats (pos3 + quat4 + scale3)
 *     //   FIELD_VELOCITY    → 6  floats (linear3 + angular3)
 *     //   FIELD_HEALTH      → 1  float
 *     //   ...
 *
 * RPC anatomy:
 *
 *   [JceNetRpcHeader]                        (16 B)
 *     uint32 protocol_version
 *     uint32 rpc_id           // application-defined
 *     uint32 sender           // entity / peer id
 *     uint16 reliable         // 0 = unreliable, 1 = reliable
 *     uint16 payload_size
 *     // payload bytes follow, payload_size of them
 *
 * Layer: middleware / net (Layer 4) — public.
 */

#ifndef JCE_NET_REPLICATION_H
#define JCE_NET_REPLICATION_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_NET_PROTOCOL_VERSION 2u   /* v2 adds owner_peer_id per entity */
#define JCE_NET_PROTOCOL_V1      1u   /* legacy decoder fallback */

/* Sentinel peer id meaning "no owner" / server-owned. */
#define JCE_NET_PEER_SERVER  ((uint16_t)0xFFFFu)

/* Per-entity field bits (extend as gameplay code needs more).
 * The mask is 32-bit so we have plenty of headroom; reserved bits at
 * the top let us version field-set additions without bumping protocol. */
#define JCE_NET_FIELD_TRANSFORM     (UINT32_C(1) << 0)  /* pos + rot + scale */
#define JCE_NET_FIELD_VELOCITY      (UINT32_C(1) << 1)  /* linear + angular */
#define JCE_NET_FIELD_HEALTH        (UINT32_C(1) << 2)  /* single float */
#define JCE_NET_FIELD_ANIM_STATE    (UINT32_C(1) << 3)  /* state name + time */
#define JCE_NET_FIELD_INPUT         (UINT32_C(1) << 4)  /* 6 floats input vec */

/* Reliability classes — caller maps to its transport's channels. */
typedef enum {
    JCE_NET_CHANNEL_UNRELIABLE     = 0, /* lossy, fire-and-forget */
    JCE_NET_CHANNEL_RELIABLE       = 1, /* guaranteed, ordered    */
    JCE_NET_CHANNEL_RELIABLE_UNORD = 2  /* guaranteed, unordered  */
} JceNetChannel;

/* Snapshot entry — caller builds an array of these and passes it
 * to encode_snapshot which produces the wire blob. */
typedef struct {
    uint32_t entity_id;
    uint32_t mask;             /* JCE_NET_FIELD_* bits set */
    /* Owning peer id.  Used by `jce_net_should_replicate` to decide
     * whether the local host writes back this entity.  v1 wire data
     * decodes with owner_peer_id = JCE_NET_PEER_SERVER for backwards
     * compatibility. */
    uint16_t owner_peer_id;
    uint16_t _pad;             /* reserved — keeps the struct 4-aligned */
    /* Field payloads — only those whose bit is set in `mask` are read. */
    jce_vec3 position;
    jce_quat rotation;
    jce_vec3 scale;
    jce_vec3 linear_velocity;
    jce_vec3 angular_velocity;
    float    health;
    char     anim_state[32];
    float    anim_time;
    float    input[6];
} JceNetEntitySnap;

/* ── Encode / decode ──────────────────────────────────────────────── */

/* Compute the maximum bytes a snapshot of N entities may produce.
 * Use this to size your output buffer. */
JCE_API uint32_t jce_net_snapshot_max_bytes(uint32_t entity_count);

/* Serialise the snapshot into `out`.  Returns the bytes written or 0
 * on overflow / invalid input. */
JCE_API uint32_t jce_net_snapshot_encode(const JceNetEntitySnap *entities,
                                          uint32_t                count,
                                          uint32_t                server_tick,
                                          uint8_t                *out,
                                          uint32_t                out_capacity);

/* Decode `in` (size bytes).  Writes entities to `out_entities` (capped
 * at `max_entities`), and returns the actual count.  Returns 0 on
 * malformed input or protocol-version mismatch.  Both v1 (no owner)
 * and v2 (with owner) blobs decode — v1 entries default owner to
 * JCE_NET_PEER_SERVER. */
JCE_API uint32_t jce_net_snapshot_decode(const uint8_t        *in,
                                          uint32_t              size,
                                          uint32_t             *out_server_tick,
                                          JceNetEntitySnap     *out_entities,
                                          uint32_t              max_entities);

/* Authority check: returns true when the local peer should apply
 * its own input + simulation to the entity described by `snap`.
 *
 * The convention:
 *   - server (`local_peer == JCE_NET_PEER_SERVER`) always writes.
 *   - clients write iff snap.owner_peer_id matches local_peer.
 *
 * Game code calls this before running input / physics on a remote
 * peer's character or pickup. */
JCE_API bool jce_net_should_replicate(const JceNetEntitySnap *snap,
                                       uint16_t                local_peer);

/* ── Delta encoding ──────────────────────────────────────────────── *
 *
 * For each entity in `current` whose mask differs OR whose field
 * values differ from `previous`, emit a snap entry.  Entities present
 * in `previous` but not `current` are emitted with mask=0 to indicate
 * "removed" so the receiver can clean up.
 *
 * Ordering within both arrays must be by entity_id ascending; merge-
 * walks the two lists once.  Produces a self-contained wire blob —
 * apply with jce_net_snapshot_decode. */
JCE_API uint32_t jce_net_snapshot_encode_delta(const JceNetEntitySnap *previous,
                                                uint32_t                prev_count,
                                                const JceNetEntitySnap *current,
                                                uint32_t                cur_count,
                                                uint32_t                server_tick,
                                                uint8_t                *out,
                                                uint32_t                out_capacity);

/* ── RPC headers ─────────────────────────────────────────────────── */

typedef struct {
    uint32_t      rpc_id;
    uint32_t      sender;
    JceNetChannel channel;
    /* Payload owned by caller; not copied here. */
    const uint8_t *payload;
    uint16_t       payload_size;
} JceNetRpc;

/* Encode an RPC header + payload into `out`.  Returns bytes written
 * or 0 on overflow. */
JCE_API uint32_t jce_net_rpc_encode(const JceNetRpc *rpc,
                                     uint8_t         *out,
                                     uint32_t         out_capacity);

/* Decode RPC.  `out_rpc->payload` will point inside `in` after
 * decode — do NOT free, and do NOT use after `in` goes away. */
JCE_API bool jce_net_rpc_decode(const uint8_t *in, uint32_t size,
                                 JceNetRpc      *out_rpc);

JCE_EXTERN_C_END

#endif /* JCE_NET_REPLICATION_H */
