# engine/include/jce/runtime — Runtime headers (L5)

> Public headers for the editor↔game bridge and the per-frame PlayerLoop.

## Identity

- **Layer**: L5. C99.
- **Public umbrella**: `<jce/api_runtime.h>` (re-exports the runtime headers below; also pulled in by `<jce/api.h>`).

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_game_module.h` | `engine/src/runtime/jce_game_module.c` | Registry of `JceGameModule` (= `JceAppDesc`). Same module runs standalone or inside the editor's Play mode. |
| `jce_player_loop.h` | `engine/src/runtime/jce_player_loop.c` | 8-phase ordered hook registry. Subsystems and game code register *auxiliary* callbacks against a `JcePlayerLoopPhase`; `jce_engine_iterate` dispatches each phase once per frame. **NOT full Unity PlayerLoop parity**: the engine's real per-frame work (renderer begin/submit, scene update, physics, input) still runs INLINE in `jce_engine_iterate` / `jce_runtime_step`, not as registered phase callbacks — the phases are a hook surface layered alongside that inline work. Current real consumers: coroutines (`UPDATE`/`FIXED_UPDATE`/`END_OF_FRAME`) and scene-async (`EARLY_UPDATE`). Main-thread only in v1. **P3-B.5**: introspection (`jce_player_loop_iterate`, `_set_enabled`, `_set_debug_name`, `_phase_to_string`); per-callback `last_ms` timing + runtime disable, consumed by editor Systems panel. |
| `jce_coroutine.h` | `engine/src/runtime/jce_coroutine.c` | **P3-B.4** cooperative coroutine yield primitives (Unity-parity: `WaitForSeconds` / `WaitForFixedUpdate` / `WaitForEndOfFrame` / `WaitUntil` + implicit "next frame"). Deferred-callback scheduler hooked into `JCE_PHASE_UPDATE` / `JCE_PHASE_FIXED_UPDATE` / `JCE_PHASE_END_OF_FRAME` at priority 1000. Static pool of `JCE_COROUTINE_MAX_ACTIVE` (256) slots; handle = `(generation << 32) \| slot`. No threads, no stackful context — coroutine bodies are user-written state machines that return false to exit or fill `next_wait` and return true. Auto-installs on first `jce_coroutine_start`. |
| `jce_ai_scene_director.h` | `engine/src/runtime/jce_ai_scene_director.c` | Provider-neutral planner boundary. Injected transport/cache callbacks, strict response correlation, bounded retry/timeout, deterministic bootstrap fallback, and local SceneRecipe/FrozenPlan v2 compilation with stable hierarchy constraints. |
| `jce_scene_generation.h` | `engine/src/runtime/jce_scene_generation.c` | L5 coordinator that composes planning with the L4 transaction state machine. Build/update is bounded; activation is explicit; post-commit health failure rolls back. |

## Rules

1. Runtime headers are consumed via the umbrella `<jce/api_runtime.h>`. Don't `#include "engine/src/runtime/..."` from outside this layer.
2. PlayerLoop registration / unregistration is **main thread only** until further notice — phase dispatch is also single-threaded.
3. `JCE_PHASE_FIXED_UPDATE` is driven by `jce_fixed_clock_default()` (P3-B.2): 0..N invocations per frame at a fixed cadence (default **60 Hz**, tune via `jce_engine_set_fixed_hz`). Callbacks must use the supplied `dt` (= `fixed_dt`) and **must not** assume one call per render frame. Determinism beyond the cadence (math reproducibility for replay/rollback) is the caller's responsibility. **P1-fixed-clock-unify**: `jce_fixed_clock_default()` is the SINGLE source of truth for the fixed cadence. The per-runtime physics clock (`rt->clock` in `jce_runtime.c`) keeps its own accumulator/`tick_count` (it is a separate call site from `jce_engine_iterate`, so sharing one accumulator would double-bank the frame dt) but ADOPTS this clock's `fixed_dt` at create() and re-adopts it every `jce_runtime_step` — so `jce_engine_set_fixed_hz()` governs physics and the FIXED_UPDATE phase identically and they cannot desync. An explicit `JceRuntimeDesc.fixed_timestep` instead retunes the global clock to match. `jce_net_transform.c` reads the same global clock for tick conversion, so it now tracks the real physics tick rate.
4. Game-module rules are unchanged; see `engine/src/runtime/AGENTS.md`.
5. AI transports never own policy. They submit/poll bytes only; semantic
   validation, deterministic compilation, cache identity, and activation
   safety remain inside JCE.
6. Director prompts and fixtures must advertise the exact recipe/compiler
   versions and `stableRoleInstance` hierarchy mode; version drift fails closed.
