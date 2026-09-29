# engine/src/middleware/net — Networking (L4)

> Snapshot + replication over ENet, with protobuf payloads.

## Identity

- **Layer**: L4. C99 + C++ bridge for protobuf-generated code.
- **Public umbrella**: `<jce/api_net.h>` → `<jce/middleware/net/jce_*.h>`
- **Deps (PRIVATE)**: `enet`, `protobuf`, `flecs::flecs_static`.  Target is CXX-linked.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_net.h` / `jce_net_types.h` | `jce_net.c` | Public client/server, channels, send/recv, connect/disconnect |
| (internal) `jce_net_proto.h` | `jce_net_proto.cpp` | protobuf (de)serialization bridge |
| (internal) — | `jce_net_web.c` | WebSocket-style transport for browser (Emscripten) |
| `jce_replication.h` | `jce_replication.c` | **P3-D.1 + D.3** — NetworkObject table, per-component registry (mirrors `jce_snapshot` pattern), full-state snapshot encode/decode, ownership transfer + lifecycle events, scene-side `JceNetworkObjectComponent` mirror |
| `jce_rpc.h`         | `jce_rpc.c`         | **P3-D.2** — fixed-cap RPC registry (256 names), wire codec, send/receive paths, authority gate, atomic stats counters.  Borrows the replication host via the internal `jce__rpc_transport_broadcast` / `jce__rpc_transport_send` seams. |
| `jce_session.h`     | `jce_session.c` + `jce_session_internal.h` | **P3-D.6** — singleton session state machine (HOST / DEDICATED_SERVER / CLIENT), HELLO handshake, fixed-cap (64) client roster, connect timeout, event callback, owns the channel-7 poll loop and demuxes session vs replication packets. |
| `jce_net_transform.h` | `jce_net_transform.c` | **P3-D.4** — transform snapshot replication for predicted objects.  Per-net-object register, server/owner UNRELIABLE broadcasts of `(pos, rot, vel)` at `snapshot_hz` (packet type 5), client ring-buffer (16 slots) + interpolation at `render_tick = server_tick − interp_delay_ticks`, owned-object snap-correction on divergence.  Reuses the `jce__rpc_transport_broadcast` seam — does NOT include `jce_player_loop.h`. |
| `jce_lan_discovery.h` | `jce_lan_discovery.c` | **P3-D.5** — LAN host discovery (UDP broadcast over `enet_socket_*`).  Reactive server beacon (REQ→RESP, no spam), client broadcasts to `ENET_HOST_BROADCAST:47775`, dedupes by `address:game_port`, auto-stops after `duration_ms`.  Caller-driven tick (NO `jce_player_loop.h` include, per L4 rule).  One-time ENet init mirrors `jce_net.c`'s ref-counted pattern via a local `g_lan_enet_init` flag — no `atexit(enet_deinitialize)` here because `jce_net.c` already owns it. |

## Proto pipeline

- `.proto` files live in `engine/proto/`.
- Generated C++ goes into the build dir; included only from `jce_net_proto.cpp`.
- Public headers expose POD message structs, not `google::protobuf::*` types.

## Replication design (P3-D.1)

- **Reuse律**: `jce_replication.c` deliberately mirrors `engine/src/middleware/save/jce_snapshot.c` —
  same `Provider` / `CompEntry` registry shape with `(name, version, write_fn, read_fn, user)`.  The save
  framework already solved "versioned bag of typed sections"; replication just retargets that idea at the
  ENet transport.
- **Wire format**: little-endian, documented in `jce_replication.h`.  Prefab paths still ship inline;
  component **names are interned to a u16 index** (registration order is identical on both peers) — see
  P1-networking-full below.
- **flecs access**: world pointer plumbed in via `jce_net_replication_set_world(jce_scene_get_world(s))`.
  Encoder calls `ecs_get_id` per (object, component); decoder uses `ecs_ensure_id` + `ecs_modified_id`.
  A component registered with `flecs_component_id == 0` has its id resolved lazily by NAME
  (`ecs_lookup(world, cd->name)`) so L6 callers (the runtime) can register scene components without
  pulling flecs into their TU.

## P1-networking-full — delta + late-joiner + interest + bridge

- **Bridge**: `jce_runtime.c` walks authored `JceNetworkObject` entities at create() (only when a
  session is live), `jce_net_object_adopt(entity, owner, flags, prefab)` binds the EXISTING flecs
  entity into the replication table (server only — clients learn objects from the spawn stream), then
  registers the transform with `jce_net_transform_register()` from the authored `JceNetTransform`
  override.  `jce_net_transform_fixed_step()` + `jce_net_replication_tick()` are driven in the fixed
  loop after `jce_session_tick()`; `jce_net_transform_render_step(alpha)` runs after `jce_scene_update`.
- **Acked-baseline delta**: the server caches the last-sent payload per (net id, comp index) in a flat
  `BaselineEntry` table and diffs each encode against it — only CHANGED component entries go on the wire
  (`memcmp`).  Baselines are dropped when an object despawns.  Clients ack each snapshot
  (`JCE_REPL_PKT_ACK == 8`, broadcast to the single server peer); the server records per-peer
  `last_acked_tick`.  `jce_net_replication_comp_entries_sent()` exposes the count of changed entries.
- **Late-joiner**: `encode_and_broadcast` refreshes a peer table from the session roster
  (`jce__session_iter_remote_peers`); a freshly-seen peer is flagged `needs_full_burst` and gets ONE
  reliable FULL snapshot (every object re-spawned + every component, `JCE_REPL_SNAP_FLAG_FULL`) before
  joining the delta stream.
- **Interest management**: `jce_net_replication_set_interest_radius(m)` enables a per-peer squared-
  distance filter.  When on (and a session owns the poll loop) the server sends a per-peer delta whose
  component stream drops objects farther than the radius from the peer's interest origin (the first
  positioned object the peer's client owns).  0 = off → single shared delta broadcast (v1 behaviour).
  Spawn/despawn/ownership are never interest-filtered; only the per-tick component stream is.
- **Channel**: reserves ENet channel `JCE_NET_REPL_CHANNEL == 7`.  Host must be created with
  `channel_count >= 8` before `jce_net_replication_attach_host()`.
- **Layer constraint**: NO include of `jce_player_loop.h` (that lives in jce_application, L5).
  Application is expected to register a FIXED_UPDATE callback that calls `jce_net_replication_tick()`.
- **Self-test**: `#ifndef NDEBUG` exposes `jce_net_replication_self_test()` exercising the NetObject
  table + registry + tick sweep + ownership transfer + event listeners.

## D.3 — NetworkObject component + ownership

- **Scene mirror**: a flecs component `JceNetworkObjectComponent` is declared in
  `engine/include/jce/middleware/scene/jce_scene.h` and registered in `jce_scene.c`.  It holds
  `(net_id, owner, flags, is_owner)` so gameplay systems can query authority without round-tripping
  through net APIs.  The replication layer **does not** include `jce_scene.h` for the type — it
  resolves the flecs component id lazily with `ecs_lookup(world, "JceNetworkObjectComponent")` and
  caches it (invalidated on `jce_net_replication_set_world`).
- **Wire**: ownership changes get their own packet type (`JCE_REPL_PKT_OWNER_CHG == 2`), sent
  RELIABLE on the same channel.  Snapshots are now prefixed with `JCE_REPL_PKT_SNAPSHOT == 1`.
- **Authority gating**: `jce_net_object_has_authority(id)` is the single source of truth.
  `jce_net_object_set_owner()` is server-only — it queues the change; the next
  `jce_net_replication_tick()` drains the queue before encoding the snapshot.
- **Lifecycle events**: clients fire `JCE_NETOBJ_SPAWNED` / `JCE_NETOBJ_DESPAWNED` /
  `JCE_NETOBJ_OWNER_CHANGED` through a fixed 16-slot listener table.  Listener handles start
  at 1 (0 means invalid / cap reached).

## D.2 — RPC

- **Registry**: fixed `RpcEntry[256]` with linear scan.  Names ≤120 bytes (the cap covers
  Unity-style `Namespace.Class.Method` comfortably).  Re-registering by name updates in place.
- **Wire format**: see `jce_rpc.h` — `packet_type=3` extends the replication dispatcher's
  switch (`JCE_REPL_PKT_RPC == 3`).  Tick is reserved for future use.
- **Send path**:
  - `ServerRpc + TO_SERVER` from a SERVER role short-circuits the wire and runs the handler
    locally (headless tests, host-local clients).
  - **D.6 update**: TO_OWNER / TO_NOT_OWNER / TO_CLIENT_ID now resolve targets through
    `jce__session_get_peer` / `jce__session_iter_remote_peers` (defined in `jce_session.c`)
    and use direct `jce_net_send` via the new `jce__rpc_transport_send` seam.  When the
    target resolves to HOST's local seat, the handler is dispatched in-process (no wire
    round-trip).  When no session is attached, the path gracefully falls back to broadcast
    so legacy callers / unit tests keep working.
  - TO_ALL_CLIENTS still uses `jce__rpc_transport_broadcast()`; HOST also runs the handler
    locally to mirror "the local seat is a client".
  - Reliability flag from the registered desc maps to `JCE_NET_RELIABLE` / `JCE_NET_UNRELIABLE`.
- **Receive path**:
  - Re-feeds the full packet (incl. type byte) so the dispatcher stays independent.
  - Authority gate: ServerRpc on a non-SERVER role → drop + bump `rejected_authority`.
    ServerRpc where `sender != owner(net_id)` → drop + bump `rejected_authority`.
  - Target filter kept as defence in depth: wire `target` + `specific_client` are still
    validated client-side so a stray broadcast (e.g. fallback path) doesn't deliver to
    the wrong client.
- **Self-test** (`#ifndef NDEBUG`): `jce_rpc_self_test()` covers ServerRpc owner-allowed,
  ServerRpc non-owner rejected, ClientRpc broadcast / TO_OWNER / TO_NOT_OWNER / TO_CLIENT_ID.

## D.6 — Session

- **Singleton state** (`g_sess` in `jce_session.c`): roster of up to
  `JCE_SESSION_CLIENT_CAP = 64` `ClientSlot { id, peer_idx, is_local,
  is_host, name[64] }` entries, role, state, attached `JceNetHost*`,
  monotonic uint16 `next_client_id` (skips occupied slots), connect
  deadline, server name, event callback + user pointer.
- **Local seat sentinel**: HOST registers `peer_idx = UINT32_MAX`
  meaning "loopback, do not transmit".  Used by RPC to short-circuit
  to the in-process handler.
- **Handshake state machine** (CLIENT):
  `STARTING → CONNECTING → HANDSHAKING (got ENet CONNECT, sent
  HELLO_REQ) → RUNNING (got HELLO_ACK)`.  Timeout via
  `jce_time_ticks_ms()` delta vs `connect_started_ms`; on expiry the
  session fires `JCE_SESSION_EVT_CONNECT_FAILED` and tears down.
- **Polling cooperation**: while a session is attached, replication's
  client tick consults `jce__session_owns_poll()` and skips
  `jce_net_poll()`.  Session's `jce_session_tick()` becomes the sole
  consumer of `enet_host_service` and forwards non-session channel-7
  packets to `jce_net_replication_handle_packet()`.
- **Closes D.2 TODO**: the comment in old `jce_rpc.c` ("Per-peer
  targeted sends require a client_id -> peer mapping that doesn't
  exist yet (TODO D.6)") is gone — RPC now resolves directly.
- **Self-test**: gated behind `JCE_NET_SELF_TEST` (off by default —
  the standard `NDEBUG`-disabled build path doesn't compile it).
  Single-process loopback HOST/CLIENT testing requires two
  `SessionState` instances, which the singleton intentionally doesn't
  support; integration coverage will live in a future multi-process
  test harness.

## D.4 — Transform replication (snapshot interp + prediction)

- **State**: per-object `NtEntry { id, cfg, ring[16], ring_head, ring_count,
  last_sent_tick, pending_correction, pending_snapshot }` in a heap
  array; linear scan on register/unregister (registrations are rare).
- **Send path** (SERVER, or owner when `JCE_NET_AUTH_OWNER` on CLIENT):
  `jce_net_transform_fixed_step()` walks entries and, at each entry's
  cadence (`fixed_hz / snapshot_hz`), samples the scene transform via
  `jce_scene_get_transform` + `jce_net_object_to_entity`, encodes one
  packet per entry, and dispatches via the L4-internal
  `jce__rpc_transport_broadcast(data, size, JCE_NET_UNRELIABLE)` seam
  declared `extern` (defined in `jce_replication.c`).
- **Receive path**: `jce__net_transform_recv_packet(data, size)` is
  declared `extern` and wired into `jce_replication.c`'s packet-type
  switch under `JCE_REPL_PKT_NET_TRANSFORM (=5)`.  Decoded snapshots
  are appended to the entry ring (auto-prune of older-than-current);
  for owned/SERVER-authority entries on a CLIENT we additionally set
  `pending_correction = true` + cache the snapshot for the next
  render-step snap check.
- **Render path** (`jce_net_transform_render_step`):
  - Non-owned entries: `ring_find_pair(render_tick)` → `jce_v3_lerp` +
    `jce_q_slerp` (shortest-path), then `jce_scene_get_transform` write
    back.  Empty buffer ⇒ hold newest.
  - Owned + CLIENT + SERVER auth: read scene, compare to
    `pending_snapshot`, snap iff `dist > divergence_snap_distance` OR
    `angle > divergence_snap_angle_deg`; bump
    `jce_net_transform_snap_corrections_count()`.
- **Wire**: `[type u8 = 5][server_tick u32][count u16]` then per entry
  `[net_id u32][pos f32×3][rot f32×4][vel f32×3]` (44 B/entry, 7 B
  header).  Little-endian via local `WBuf`/`RBuf`.
- **Layer constraint**: like replication, NO include of
  `jce_player_loop.h` (L5).  Caller wires
  `jce_net_transform_fixed_step()` into the application fixed-update
  and `jce_net_transform_render_step(alpha)` into the render loop.
- **Self-test** (`#ifndef NDEBUG`): `jce_net_transform_self_test()`
  uses an in-TU stub branch in `entity_sample_transform` /
  `entity_apply_transform` (gated by `g_nt_stub_active`) to validate
  the interp pair lookup, lerp/slerp math, and the snap-divergence
  branch without booting an ECS world or a real replication session.



## Rules

1. **enet / protobuf / flecs are private.** Public API: `JceNetEndpoint*`, `JceNetPacket*`, callback table.
2. **Replication is the substrate, not the policy.** NetworkVariable (D.2), RPC (D.4), interpolation (D.5)
   layer on top — no game-specific replication rules here.
3. **Threading**: net runs on its own thread; deliver received packets via lock-free queue to main.
4. **WebSocket path** (`jce_net_web.c`) is Emscripten-only — gate via `JCE_PLATFORM_WEB`.
5. **Bandwidth budget**: target dial-up baseline; default tick rate ≤ 20Hz; expect compression.

## Don't

- Don't expose enet / protobuf / flecs types in public headers.
- Don't add a second transport (UDP raw, QUIC) without owner approval.
- Don't put gameplay replication rules here.
- Don't re-invent the section registry — mirror `jce_snapshot.h`.

## despawn 的回收不能只在有 host 时发生（2026-08-31）

`jce_net_object_despawn()` 立即做本地清理，只保留表项（`pending_despawn`）好让
下一个出站包能报出这个 id；回收由 `jce_net_replication_tick()` 做。那段扫描原本写在
`if (role == SERVER && g_repl.host)` **里面** ⇒ **没有 host 时（单机、listen() 之前）
永不执行**，每次 despawn 永久占住一个对象表槽位，`jce_net_object_count()` 再也降不下来。

现在 SERVER 无 host 时也扫描，但**不清 `pending_spawn`**——清它只有在 spawn 已经广播出去
之后才正确；host 后来接上时那次 spawn 还得发。

**五个 `*_self_test` 的真实状态**（`JCE_NET_SELF_TEST` 没有任何构建文件定义）：
`replication` / `net_transform` / `rpc` 只受 `#ifndef NDEBUG` 约束，**Debug 下会编译**，
已接进 Debug 测试套件；`session`（绑 27015 端口）与 `lan_discovery`（LAN 广播）
额外受 `JCE_NET_SELF_TEST` 约束、从不编译，已在
`tools/lint/self_test_exempt.txt` 里带理由豁免。
