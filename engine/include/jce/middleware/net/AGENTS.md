# engine/include/jce/middleware/net — Networking public headers (L4)

Public API for the networking subsystem.  Consumed via `<jce/api_net.h>`.

## File map

| Header | Role |
|--------|------|
| `jce_net_types.h`   | Shared handle / event / delivery enums.                    |
| `jce_net.h`         | Host lifecycle, connect, send/recv, polling (ENet under).  |
| `jce_replication.h` | **P3-D.1 + D.3** — NetworkObject + per-component snapshot replication + ownership transfer + lifecycle events.  Mirrors `jce_snapshot.h`'s registry pattern (name / version / write_fn / read_fn / user). |
| `jce_rpc.h`         | **P3-D.2** — ServerRpc / ClientRpc.  Caller-owned opaque byte payload, name-on-wire (TODO: intern at handshake), authority gate on receive (server drops a ServerRpc whose `sender` does not own `net_id`). |
| `jce_session.h`     | **P3-D.6** — NetworkManager-parity Host/DedicatedServer/Client lifecycle.  Owns the `client_id ↔ peer` roster (cap 64), HELLO handshake, connection events, and the channel-7 poll loop.  Replication & RPC ride on top of it. |
| `jce_net_transform.h` | **P3-D.4** — Snapshot interpolation + client prediction foundation.  Per-net-object register/unregister, server/owner broadcasts authoritative `JceNetTransformSnapshot` at `snapshot_hz` (default 20 Hz), clients interpolate at `server_tick − interp_delay_ticks` (default 100 ms ≈ 6 ticks @ 60 Hz fixed step), owned objects snap-correct on divergence (default 0.5 m / 30°).  No rollback in v1. |
| `jce_lan_discovery.h` | **P3-D.5** — LAN host discovery via UDP broadcast on the ENet socket layer.  Server beacons reactively on `JCE_LAN_DISCOVERY_DEFAULT_PORT` (47775); client broadcasts a REQ to `255.255.255.255:<port>` and accumulates RESP packets in a fixed `JCE_LAN_DISCOVERY_MAX_SERVERS` (32) table for `duration_ms`.  Wire magic = `'JCEL'` (`0x4C45434A` LE), `JCE_LAN_PROTOCOL_VERSION = 1`.  TODO: some VPN/corp networks block UDP broadcast — UI should retain a "Join by address" fallback. |

## Rules

1. **No third-party headers leak out.**  Public structs never expose
   `ENet*`, `google::protobuf::*`, or `flecs.h` types — `flecs_component_id`
   is intentionally typed as `uint64_t` so consumers don't pick up flecs.
2. **Channel allocation.**  Replication reserves
   `JCE_NET_REPL_CHANNEL == 7`.  Hosts created via `jce_net_host_create()`
   must request `channel_count > 7` before
   `jce_net_replication_attach_host()`.
3. **Reuse律.**  The replication registry intentionally clones the
   `jce_snapshot_register()` shape.  Do NOT invent a parallel pattern —
   extend the snapshot mental model.
4. **Layer direction.**  L4 — never includes anything from L5+ (no
   `jce_player_loop.h`).  Applications drive replication's tick from
   their fixed-update hook.

## What lives where (P3-D roadmap)

| Sub-task | Header lands in |
|----------|-----------------|
| D.1 NetworkObject + snapshots | `jce_replication.h` ✅ |
| D.2 RPC                       | `jce_rpc.h` ✅ |
| D.3 NetworkObject component + ownership transfer | `jce_replication.h` ✅ (scene side: `JceNetworkObjectComponent` in `jce_scene.h`) |
| D.4 Interpolation             | `jce_net_transform.h` ✅ (packet type 5; scheduled by application's fixed+render hooks, mirroring replication's no-auto-install rule) |
| D.5 LAN discovery             | `jce_lan_discovery.h` ✅ |
| D.6 Host/client lifecycle     | `jce_session.h` ✅ |

## Wire format (v3, P3-D.2)

Every replication packet now starts with a single `u8` type byte:

| Type | Name | Payload |
|------|------|---------|
| `1`  | `JCE_REPL_PKT_SNAPSHOT`  | `tick u32 · ack u32 · spawn_n u16 · desp_n u16 · comp_n u16 · flags u16 · …` (unchanged from D.1) |
| `2`  | `JCE_REPL_PKT_OWNER_CHG` | `tick u32 · net_id u32 · new_owner u16 · reserved u16` — RELIABLE on `JCE_NET_REPL_CHANNEL` |
| `3`  | `JCE_REPL_PKT_RPC`       | `tick u32 · net_id u32 · name_len u16 · name bytes · sender u16 · target u8 · specific_client u16 · reliability u8 · payload_size u32 · payload bytes` — delivery follows the registered RPC's reliability flag |
| `4`  | `JCE_REPL_PKT_SESSION`   | `sub u8 · proto_ver u16 · assigned_id u16 · name_len u16 · name bytes` — sub-opcodes: `1` HELLO_REQ (client→server, carries player_name, assigned_id=0), `2` HELLO_ACK (server→client, assigns client_id + server_name), `3` GOODBYE.  Always RELIABLE on `JCE_NET_REPL_CHANNEL`. |
| `5`  | `JCE_REPL_PKT_NET_TRANSFORM` | `server_tick u32 · count u16` then per entry `net_id u32 · pos f32×3 · rot f32×4 · vel f32×3` (44 B / entry, 7 B header incl. type byte).  UNRELIABLE on `JCE_NET_REPL_CHANNEL`. Cadence gated server-side at each entry's `snapshot_hz`. |

> v1 RPC ships the name string on the wire to stay debuggable.  Handshake-time interning (`u16` id table) is tracked as a TODO for D.6.

## Authority model (D.2 + D.3)

- Server is **always** authoritative.
- Clients are authoritative only for objects they own.
- `jce_net_object_has_authority(id)` is the single source of truth for
  gameplay-side write-gating.
- `jce_net_object_set_owner()` is **server-only**; it updates the local
  table and queues a reliable broadcast on the next tick.

## D.2 — RPC

- **Two flavours, Unity-NGO style.**
  - `ServerRpc` (`server_authoritative=true`, `target=JCE_RPC_TO_SERVER`):
    client → server.  The client may only invoke one on an object it
    owns.  Server re-validates on receive: if `sender != owner`, it
    drops + bumps `jce_rpc_rejected_authority()`.
  - `ClientRpc` (any other target): server → client.  Server must hold
    authority over `net_id`.  Targets: `TO_OWNER`, `TO_ALL_CLIENTS`,
    `TO_NOT_OWNER`, `TO_CLIENT_ID`.
- **Payload** is opaque caller-owned bytes — no serialization scheme
  baked in.  Typed-RPC codegen is a future P3-D+ task.
- **Transport reuse.**  RPC encodes its own packet but borrows the
  replication host + `JCE_NET_REPL_CHANNEL` via an L4-internal seam
  (`jce__rpc_transport_broadcast`, declared `extern` in `jce_rpc.c`,
  defined in `jce_replication.c`).  Not in any public header.
- **Per-peer routing** (TO_OWNER / TO_NOT_OWNER / TO_CLIENT_ID) now
  resolves through the session's `client_id → peer` roster
  (`jce__session_get_peer` / `jce__session_iter_remote_peers`) and
  sends via direct `jce_net_send`.  Local-seat targets short-circuit
  to in-process handler dispatch.  When no session is active the path
  gracefully falls back to broadcast + receive-side filter for
  back-compat.  Filter retained as defence in depth.

## D.6 — Session

- **Modes**: `JCE_SESSION_HOST` (listen-server + local seat),
  `JCE_SESSION_DEDICATED_SERVER` (listen-server, no local seat),
  `JCE_SESSION_CLIENT` (connect to remote).
- **Roster**: fixed capacity `JCE_SESSION_CLIENT_CAP = 64`.  HOST is
  always `client_id = 1`.  Remote clients are allocated monotonically
  by the server (skipping occupied slots).
- **Events**: `JCE_SESSION_EVT_STARTED`, `_STOPPED`, `_CONNECTING`,
  `_CONNECTED`, `_DISCONNECTED`, `_CLIENT_JOINED`, `_CLIENT_LEFT`,
  `_CONNECT_FAILED`.  Delivered through `jce_session_set_event_cb`.
- **Polling cooperation**: `jce_session_tick()` owns
  `enet_host_service` while a session is attached; replication's
  client tick checks `jce__session_owns_poll()` and skips its own
  poll to avoid stealing events.  Session demuxes channel-7 packets:
  type `4` → session, types `1/2/3` → replication handler.
- **Internal seam**: `engine/src/middleware/net/jce_session_internal.h`
  (not public).  Used by replication (dispatcher hook) and RPC
  (`get_peer`, `iter_remote_peers`, `is_local_client_internal`).

## D.4 — Snapshot interpolation + client prediction

- **Header**: `jce_net_transform.h`.  Implementation in `engine/src/middleware/net/jce_net_transform.c`.
- **Roles**:
  - SERVER (or owner, when `JCE_NET_AUTH_OWNER`): samples scene `JceTransform` via `jce_scene_get_transform`, encodes packet type 5, broadcasts UNRELIABLE through the L4-internal `jce__rpc_transport_broadcast` seam at `snapshot_hz` cadence (default 20 Hz).
  - CLIENT, non-owned: ring-buffers the last 16 snapshots, finds the pair straddling `render_tick = last_known_server_tick − interp_delay_ticks` (default 100 ms ≈ 6 ticks @ 60 Hz), and applies `lerp(pos)` + `slerp(rot)` from `jce_math`.  Hold-newest on the leading edge.
  - CLIENT, owned + SERVER authority: gameplay/prediction continues to write the scene each fixed tick; incoming snapshots flag `pending_correction` and `jce_net_transform_render_step` snaps only when divergence exceeds `divergence_snap_distance` (0.5 m default) or `divergence_snap_angle_deg` (30°).  No rewind / replay in v1.
- **Scheduling**: caller-driven — application invokes `jce_net_transform_fixed_step()` from its fixed-update and `jce_net_transform_render_step(alpha)` from its render loop.  L4 must NOT include `jce_player_loop.h` (L5) per §0.
- **Stats**: `jce_net_transform_registered_count()` / `_snap_corrections_count()` / `_reset_stats()`.
- **Self-test**: `jce_net_transform_self_test()` (NDEBUG-gated) covers interp pair lookup, slerp math, and the snap divergence branch using an in-TU stub scene (no ECS / replication boot required).

## D.5 — LAN discovery

- **Header**: `jce_lan_discovery.h`.  Implementation in `engine/src/middleware/net/jce_lan_discovery.c`.
- **Transport**: UDP via ENet's socket layer only (`enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM)`, `ENET_SOCKOPT_NONBLOCK`, `ENET_SOCKOPT_BROADCAST`).  **No** `<winsock2.h>` / `<sys/socket.h>` / `<arpa/inet.h>` includes — the engine constraint.
- **Default port**: `JCE_LAN_DISCOVERY_DEFAULT_PORT = 47775`.  Independent of the actual game port (e.g. 7777) that ENet hosts listen on; clients receive the game port in the RESP payload.
- **Magic**: `'JCEL'` = `0x4C45434A` LE.  `JCE_LAN_PROTOCOL_VERSION = 1`.  Mismatched magic / version / opcode / nonce are silently dropped.
- **Wire format** (little-endian):
  - REQ : `[ magic u32 ][ version u16 ][ opcode u8 = 1 ][ client_nonce u32 ]` (11 B)
  - RESP: `[ magic u32 ][ version u16 ][ opcode u8 = 2 ][ client_nonce u32 ][ game_port u32 ][ current u16 ][ max u16 ][ protocol u32 ][ name_len u16 ][ name bytes ]` (23 B + name)
- **Server**: reactive beacon — does NOT spam broadcasts.  Open socket, bind `0.0.0.0:<discovery_port>`, reply per REQ.  `jce_lan_discovery_server_set_player_count()` updates the live count.
- **Client**: bind `0.0.0.0:0`, send one REQ to `ENET_HOST_BROADCAST:<discovery_port>`, accumulate RESPs into a fixed `JCE_LAN_DISCOVERY_MAX_SERVERS = 32` table deduped by `address:game_port`.  `ping_ms` is a best-effort RTT (first response only).  Scan auto-stops after `duration_ms`; results stay until `_clear()`.
- **Scheduling**: caller-driven — application calls `jce_lan_discovery_server_tick()` / `_client_tick()` each frame (e.g. from EarlyUpdate).  L4 must NOT include `jce_player_loop.h` (L5) per §0 — same rule that governs replication and transform.
- **Self-test**: `jce_lan_discovery_self_test()`, gated behind `JCE_NET_SELF_TEST` (off by default — loopback broadcast can be blocked in CI sandboxes; the test logs + soft-passes rather than abort).
- **Known limitation**: routers don't forward broadcasts off-segment (this is correct scope — LAN only).  Some VPN / corporate networks block UDP broadcast entirely; a direct-IP "Join by address" entry is the documented UI fallback.
