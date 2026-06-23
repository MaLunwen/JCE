/*
 * jce_replication.h  NetworkObject + component snapshot replication (P3-D.1).
 *
 * The model layer of the P3-D networking theme.  Provides:
 *   - JceNetObjectId: server-authoritative identity wrapping a flecs entity.
 *   - Ownership (server vs. specific client).
 *   - A per-component registry that MIRRORS jce_snapshot.h's section
 *     registry (stable string id + version + write_fn / read_fn / user).
 *     Future readers should immediately see the symmetry with the
 *     save framework — both solve "serialize a bag of typed sections
 *     versionably", just over different transports.
 *   - A tick entry point called from JCE_PHASE_FIXED_UPDATE; the server
 *     packetises spawns/despawns/component deltas, the client applies
 *     received snapshots.
 *
 * Scope of this header (P3-D.1 ONLY):
 *   - NetworkObject identity + table.
 *   - Component registry shape (1:1 with snapshot pattern).
 *   - Full-state snapshots (no delta compression yet — TODO).
 *   - Hook for the existing jce_net transport (channel JCE_NET_REPL_CHANNEL).
 *
 * Out of scope (later D-tasks):
 *   - NetworkVariable     -> P3-D.2
 *   - RPC                 -> P3-D.3
 *   - Interpolation       -> P3-D.4
 *   - LAN discovery       -> P3-D.5
 *   - Host/client lifecycle -> P3-D.6
 *
 * Packet layout (little-endian, documented for forward compat):
 *
 *   [ packet_type:    u8   ]   1 = snapshot, 2 = ownership-change (P3-D.3)
 *
 * Snapshot (type=1):
 *   [ tick:           u32  ]   sender tick at send time
 *   [ ack_tick:       u32  ]   most recent peer tick observed (0 = none)
 *   [ spawn_count:    u16  ]
 *   [ despawn_count:  u16  ]
 *   [ comp_count:     u16  ]
 *   [ flags:          u16  ]   reserved (must be 0)
 *   spawn_count    times: { net_id:u32, owner:u16, flags:u16,
 *                           prefab_path_len:u16, prefab_path:bytes }
 *   despawn_count  times: { net_id:u32 }
 *   comp_count     times: { net_id:u32, comp_name_len:u16, comp_name:bytes,
 *                           comp_version:u32, payload_size:u32, payload:bytes }
 *
 * Ownership change (type=2, P3-D.3):
 *   [ tick:           u32  ]
 *   [ net_id:         u32  ]
 *   [ new_owner:      u16  ]
 *   [ reserved:       u16  ]   must be 0
 *
 * Ownership messages are sent reliable + separately from snapshots so
 * late joiners can be brought current via a single broadcast.
 *
 * v1 sends full component names + prefab paths inline; a future revision
 * will intern these at handshake and ship 16-bit ids only.  Wire byte
 * order is little-endian (Unity NGO does the same).
 *
 * Layer: L4 (middleware/net).  Consumed via <jce/api_net.h>.
 */

#ifndef JCE_REPLICATION_H
#define JCE_REPLICATION_H

#include <jce/middleware/net/jce_net.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Identity types                                                      */
/* ================================================================== */

typedef uint32_t JceNetObjectId;   /* server-authoritative; 0 = invalid */
typedef uint32_t JceNetTick;       /* monotonic per session             */
typedef uint16_t JceClientId;      /* 0 = server, 1..N = remote clients */

#define JCE_NET_OBJECT_INVALID  ((JceNetObjectId)0)
#define JCE_CLIENT_SERVER       ((JceClientId)0)

/* Reserved ENet channel for replication traffic.  Hosts created via
 * jce_net_host_create() must request channel_count > JCE_NET_REPL_CHANNEL
 * (i.e. at least 8) before attaching to the replication subsystem. */
#define JCE_NET_REPL_CHANNEL    ((uint8_t)7)

/* ================================================================== */
/* Role                                                                */
/* ================================================================== */

typedef enum JceNetRole {
    JCE_NET_ROLE_NONE   = 0,
    JCE_NET_ROLE_SERVER = 1,
    JCE_NET_ROLE_CLIENT = 2
} JceNetRole;

JCE_API JceNetRole JCE_CALL jce_net_replication_role(void);
JCE_API void       JCE_CALL jce_net_replication_set_role(JceNetRole role);

/* ================================================================== */
/* Subsystem init / shutdown                                           */
/* ================================================================== */

/* Initialise the replication subsystem.  Safe to call more than once.
 * Role defaults to JCE_NET_ROLE_NONE.  The application is responsible
 * for scheduling jce_net_replication_tick() under JCE_PHASE_FIXED_UPDATE
 * (the substrate stays at L4 and does not reach up into the L5
 * player-loop registry to preserve the layer direction). */
JCE_API void JCE_CALL jce_net_replication_init(void);

/* Tear down internal tables.  Called by the engine on shutdown. */
JCE_API void JCE_CALL jce_net_replication_shutdown(void);

/* Bind the flecs world (cast to ecs_world_t* internally).  Pass the
 * pointer returned by jce_scene_get_world().  May be NULL to detach. */
JCE_API void JCE_CALL jce_net_replication_set_world(void *ecs_world);

/* Bind the JceNetHost used for snapshot send / receive.  May be NULL
 * to detach (e.g. between session start/end). */
JCE_API void JCE_CALL jce_net_replication_attach_host(JceNetHost *host);

/* ================================================================== */
/* NetworkObject lifecycle                                             */
/* ================================================================== */

typedef struct JceNetObjectDesc {
    const char *prefab_path;   /* VFS path to .prefab.json (NULL for
                                * code-spawned objects)               */
    JceClientId owner;         /* JCE_CLIENT_SERVER or a client id    */
    uint16_t    flags;         /* reserved (interp, prediction, ...)  */
} JceNetObjectDesc;

/* Server-side spawn.  Allocates a net id, creates the backing flecs
 * entity, and queues a spawn message broadcast on the next tick.
 * Returns JCE_NET_OBJECT_INVALID on failure or non-server role. */
JCE_API JceNetObjectId JCE_CALL
jce_net_object_spawn(const JceNetObjectDesc *desc);

/* Server-side adopt.  Registers an EXISTING flecs entity (cast to u64)
 * as a NetworkObject instead of creating a fresh one.  This is the
 * entry the runtime uses to bridge authored scene entities carrying a
 * JceNetworkObject component into the replication table: the scene owns
 * the entity, replication only wraps it with a net id + ownership +
 * spawn broadcast.  Re-adopting the same entity returns its existing id.
 * Returns JCE_NET_OBJECT_INVALID on failure or non-server role. */
JCE_API JceNetObjectId JCE_CALL
jce_net_object_adopt(uint64_t entity, JceClientId owner,
                     uint16_t flags, const char *prefab_path);

/* Server-side despawn.  Removes the local entity and queues a despawn
 * broadcast on the next tick.  Silent no-op for unknown ids. */
JCE_API void JCE_CALL jce_net_object_despawn(JceNetObjectId id);

/* Owner of `id`, or JCE_CLIENT_SERVER if unknown / invalid. */
JCE_API JceClientId JCE_CALL jce_net_object_owner(JceNetObjectId id);

/* Net id <-> flecs entity translation.  Returns 0 / INVALID when not
 * present in the local table. */
JCE_API uint64_t       JCE_CALL jce_net_object_to_entity(JceNetObjectId id);
JCE_API JceNetObjectId JCE_CALL jce_net_object_from_entity(uint64_t entity);

/* Number of NetObjects currently tracked locally (diagnostic). */
JCE_API uint32_t JCE_CALL jce_net_object_count(void);

/* ================================================================== */
/* Ownership + authority (P3-D.3)                                      */
/* ================================================================== */

/*
 * Authority model
 * ---------------
 * - The SERVER is always authoritative over every NetworkObject.
 * - A CLIENT is authoritative ONLY over the objects it owns; this is
 *   the hook future P3-D.4 client-side prediction will use (e.g. the
 *   player's own character / vehicle / input-driven systems).
 * - Downstream systems (movement, input, animation) MUST gate writes
 *   on jce_net_object_has_authority(id) — that is the single source
 *   of truth.
 *
 * Ownership transfer is server-authoritative: jce_net_object_set_owner()
 * is a no-op on a client (logs a warning).  The change is broadcast on
 * JCE_NET_REPL_CHANNEL as a separate, RELIABLE message so it does not
 * piggy-back on the lossy snapshot stream.
 */

/* Transfer ownership of a NetworkObject.  Server-only — returns false
 * on a client or for an unknown id.  On success the local table is
 * updated immediately, the JceNetworkObjectComponent on the backing
 * flecs entity is refreshed, JCE_NETOBJ_OWNER_CHANGED fires, and a
 * reliable broadcast is queued for the next tick. */
JCE_API bool JCE_CALL
jce_net_object_set_owner(JceNetObjectId id, JceClientId new_owner);

/* Local client id.  Server returns JCE_CLIENT_SERVER (0); clients
 * return whatever id the server assigned them at handshake (defaults
 * to JCE_CLIENT_SERVER until set via the internal setter — D.6 will
 * wire this from the session handshake). */
JCE_API JceClientId JCE_CALL jce_net_local_client_id(void);

/* Convenience: am I the owner of this object?  Equivalent to
 * jce_net_object_owner(id) == jce_net_local_client_id(). */
JCE_API bool JCE_CALL jce_net_object_is_owner_local(JceNetObjectId id);

/* Authority check: can we legally mutate component data for this id?
 * Server: always true.  Client: true iff we own it. */
JCE_API bool JCE_CALL jce_net_object_has_authority(JceNetObjectId id);

/* Iterate every locally-known NetworkObject (for editor / inspector
 * panels).  Callback runs synchronously inside this call; do NOT
 * mutate the object table from the callback. */
typedef void (*JceNetObjectIterFn)(JceNetObjectId id,
                                   JceClientId    owner,
                                   void          *user);
JCE_API void JCE_CALL
jce_net_object_iterate(JceNetObjectIterFn fn, void *user);

/* ── Lifecycle events ─────────────────────────────────────────────── */

typedef enum JceNetObjectEvent {
    JCE_NETOBJ_SPAWNED       = 0,
    JCE_NETOBJ_DESPAWNED     = 1,
    JCE_NETOBJ_OWNER_CHANGED = 2
} JceNetObjectEvent;

typedef void (*JceNetObjectEventFn)(JceNetObjectEvent ev,
                                    JceNetObjectId    id,
                                    JceClientId       owner,
                                    void             *user);

/* Register a listener.  Returns a non-zero handle on success, 0 if the
 * internal slot table is full (cap = 16 listeners). */
JCE_API uint32_t JCE_CALL
jce_net_object_add_event_listener(JceNetObjectEventFn fn, void *user);

/* Unregister a previously-registered listener.  Silent no-op for 0
 * or unknown handles. */
JCE_API void JCE_CALL
jce_net_object_remove_event_listener(uint32_t handle);

/* Internal — set the local client id (called by future session/
 * handshake code, P3-D.6).  Exposed publicly for headless tests and
 * the LAN session bring-up that lands later in the D track. */
JCE_API void JCE_CALL
jce_net_replication_set_local_client_id(JceClientId id);

/* ================================================================== */
/* Component replication registry                                      */
/* ================================================================== */

/* MIRRORS the (write_fn / read_fn / version / user) quartet used by
 * jce_snapshot.h.  Per-component serializer pair packs the component
 * struct into raw bytes; replication wraps those bytes with net id +
 * stable name + version on the wire. */
typedef int (*JceNetCompWriteFn)(void       *dst,
                                 uint32_t    cap,
                                 const void *component,
                                 void       *user);

typedef int (*JceNetCompReadFn)(const void *src,
                                uint32_t    size,
                                void       *component,
                                void       *user);

typedef struct JceNetCompDesc {
    const char       *name;               /* stable id; handshake key  */
    uint32_t          version;            /* bumped on schema change   */
    uint64_t          flecs_component_id; /* ecs_id_t; e.g. ecs_id(T)  */
    uint32_t          size;               /* sizeof(component struct)  */
    JceNetCompWriteFn write;
    JceNetCompReadFn  read;
    void             *user;
} JceNetCompDesc;

/* Register / re-register a component.  Re-registering by the same name
 * updates the entry in place (matches snapshot behaviour). */
JCE_API void JCE_CALL
jce_net_replication_register_component(const JceNetCompDesc *desc);

/* Number of components currently registered (diagnostic). */
JCE_API uint32_t JCE_CALL jce_net_replication_component_count(void);

/* ================================================================== */
/* Interest management (P1-networking-full)                            */
/* ================================================================== */

/* Server-side relevance radius (metres).  When > 0 the server only
 * replicates an object's component state to a peer whose interest
 * origin is within this radius of the object (squared-distance test).
 * 0 disables the filter (replicate everything to everyone — the v1
 * behaviour).  Spawn / despawn / ownership messages are NOT filtered;
 * only the per-tick component delta stream is, so far objects still
 * exist on every peer but stop streaming until they come back in
 * range.  Default: 0 (off). */
JCE_API void  JCE_CALL jce_net_replication_set_interest_radius(float radius_m);
JCE_API float JCE_CALL jce_net_replication_get_interest_radius(void);

/* ================================================================== */
/* Bandwidth / delta diagnostics (P1-networking-full)                  */
/* ================================================================== */

/* Total component-entries sent across all snapshot encodes since init.
 * With acked-baseline delta this counts only CHANGED entries, so it is
 * a direct measure of how much the delta path saved versus full-state. */
JCE_API uint64_t JCE_CALL jce_net_replication_comp_entries_sent(void);

/* ================================================================== */
/* Tick                                                                */
/* ================================================================== */

/* Called automatically from JCE_PHASE_FIXED_UPDATE when replication is
 * initialised; exposed so callers can drive it manually in tests or
 * headless tools.  `tick` should be jce_fixed_clock_default()->tick_count
 * truncated to 32 bits. */
JCE_API void JCE_CALL jce_net_replication_tick(JceNetTick tick);

/* Apply a single inbound replication packet (call from your net poll
 * loop when channel == JCE_NET_REPL_CHANNEL).  Safe to skip — the
 * subsystem also auto-drains via jce_net_replication_tick() when a host
 * is attached. */
JCE_API void JCE_CALL
jce_net_replication_handle_packet(const void *data, uint32_t size);

/* Encode a snapshot into a freshly-allocated buffer using the SAME encoder
 * the broadcast path runs — no JceNetHost required.  This is the transport-
 * free seam for headless tools and tests: drive the REAL delta / baseline /
 * full-burst encoder, then feed the exact bytes to
 * jce_net_replication_handle_packet().  Returns the encoded byte count (0
 * on failure); on success *out_buf points to a heap buffer the caller MUST
 * release with jce_net_replication_free_buffer().  `full` forces a full-
 * state burst (every object + component, baseline ignored); otherwise the
 * result is a delta versus the live baseline AND advances that baseline
 * exactly as a real broadcast would. */
JCE_API uint32_t JCE_CALL
jce_net_replication_encode_snapshot(JceNetTick tick, bool full,
                                    void **out_buf);

/* Release a buffer returned by jce_net_replication_encode_snapshot(). */
JCE_API void JCE_CALL jce_net_replication_free_buffer(void *buf);

JCE_EXTERN_C_END

#endif /* JCE_REPLICATION_H */
