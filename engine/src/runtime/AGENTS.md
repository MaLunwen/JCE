# engine/src/runtime — Runtime / Editor↔Game Bridge (L5)

> The thin layer between L6 Application and L7 Consumers (editor + games).

## Identity

- **Layer**: L5. C99.
- **Public umbrella**: `<jce/api_runtime.h>`.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_game_module.h` | `jce_game_module.c` | Registry of `JceGameModule` (= `JceAppDesc`). Standalone games and editor Play mode both go through this. Owns `s_active_scene` (set via `jce_game_module_set_active_scene`, queried via the private `jce_game_module_active_scene()` getter used by `middleware/scene/jce_scene_systems.c`). |
| `jce_player_loop.h` | `jce_player_loop.c` | 8-phase PlayerLoop (Initialization → EarlyUpdate → FixedUpdate → Update → LateUpdate → PreRender → PostRender → EndOfFrame). Registration is main-thread only; dispatch is driven by `jce_engine_iterate`. **P3-B.5**: each slot records `last_ms` (always-on, ~30 ns wrap via `jce_time_perf_counter`), honours an `enabled` flag (set via `jce_player_loop_set_enabled`), and carries an optional 32-byte `debug_name`. `_iterate` walks all phases for the editor Systems panel. FixedUpdate accumulator is a P3-B.2 follow-up. |
| `jce_coroutine.h` | `jce_coroutine.c` | **P3-B.4** Unity-parity coroutine yield primitives. Deferred-callback scheduler: registers three PlayerLoop hooks (Update / FixedUpdate / EndOfFrame, priority 1000) and dispatches coroutines whose wait kind matches the current phase. Static pool (`JCE_COROUTINE_MAX_ACTIVE`=256), no allocator. Handle packs `(generation<<32)\|slot` so stale handles fail the `is_alive` check. WAIT_SECONDS deadlines use a cooperative `s_time_seconds` accumulated from Update `dt` (matches Unity `Time.time`). Auto-init on first start. Coroutine bodies are plain C state-machines — no stackful coroutines, no `setjmp`, no threads. `#ifndef NDEBUG` exposes `jce_coroutine_self_test()` covering next_frame / seconds / until / cancel paths. |
| `jce_ai_scene_director.h` | `jce_ai_scene_director.c` | Converts a correlated semantic AI response, cache hit, or bundled bootstrap recipe into a deterministic FrozenPlan v2. Catalog metadata is sorted before prompt/cache identity; hierarchy contract versions are explicit; malformed responses get only the configured bounded retry; stale responses fail closed. |
| `jce_scene_generation.h` | `jce_scene_generation.c` | Owns the planning-to-transaction state machine. It never calls provider code after a frozen plan exists and never commits without an explicit caller-selected frame boundary. |

## Rules

1. A "game module" is just a `JceAppDesc`. Same module runs:
   - As a standalone app driven by `jce_main_sdl.c` → engine frame loop.
   - Inside the editor's Game View panel during Play mode, driven by the editor's per-frame call into the module's `update/render` callbacks.
2. **The default module** (`jce_game_module_default()`) is always registered at index 0 — pure ECS tick with no game-specific code. Never remove it.
3. **Module registration must use program-lifetime storage** for the name (string literal or static buffer). Names are not copied.
4. **No game logic in this layer.** The runtime only routes. Per-game code lives in `examples/caged_kingdom/` or editor `editor/src/game_modules/`.
5. **Networking hooks are caller-driven.** L4 net modules (`jce_net_replication_tick`, `jce_net_transform_fixed_step` / `jce_net_transform_render_step`) deliberately do **not** include `jce_player_loop.h` to keep the layer rule clean. Game modules that opt into networking must register their own PlayerLoop callbacks — typically FixedUpdate → `jce_net_replication_tick()` + `jce_net_transform_fixed_step()`, and PreRender / Update → `jce_net_transform_render_step(alpha)` with `alpha` from the FixedUpdate accumulator (P3-B.2). The runtime layer holds no opinion on cadence.
6. **Generated scene activation is caller-driven.** A coordinator may reach
   `FROZEN_READY` during update, but only the application/editor selects the
   frame boundary and calls commit. The following health result either retires
   the previous scene or restores it.
7. **Do not repeat inference for a frozen plan.** Once the director has emitted
   a verified plan, transaction, cache, replay, and future replication consume
   that artifact and its identities directly.

## Common tasks

| Task | Steps |
|------|-------|
| Add a new built-in module | Implement a `JceAppDesc` in your consumer dir; call `jce_game_module_register("<name>", &desc)` at startup |
| Drive a module from the editor | Editor's Play button picks via `jce_game_module_find()` and runs its `update/render` per frame |

## Don't

- Don't expand this layer with game systems — those belong in middleware (engine-side primitives) or in consumer dirs (gameplay).
- Don't import editor headers from here — runtime is *below* L7 consumers.

## 协程句柄：generation 从 1 起，不是 0（2026-08-31）

`JceCoroutineHandle` = `(generation << 32) | slot`，而 `JCE_COROUTINE_INVALID` = `0`。
所以 **slot 0 + generation 0 打包出来就是 0**——一次成功的启动与文档规定的失败值逐位相同。
后果不止「调用方误判失败」：`resolve()` 第一行就对哨兵短路，于是那个协程
**`is_alive()` 报死、`cancel()` 静默失效**，会一直跑到进程结束。

`jce_coroutine_system_init()` 的 `memset` 之后必须把每个槽的 `generation` 置 1。
`free_slot()` 只做 `generation++`，不重置，所以那个 memset 是唯一的重置点。

**加新的句柄类型时先问：全零的槽打包出来会不会等于你的哨兵。**

这个缺陷是靠**第一次运行 `jce_coroutine_self_test()`** 发现的——它已经存在、写得完整正确、
但全树没有任何调用者。现在由 `tools/lint/check_self_tests_run.py` 守着。
