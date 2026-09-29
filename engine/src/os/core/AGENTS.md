# engine/src/os/core — Core / Foundation (L2)

> Lowest engine layer. Everything else depends on this. See repo-root `AGENTS.md`.

## Identity

- **Layer**: L2 (OS Core). Pure C99. No graphics, no platform-window code.
- **Public umbrella**: `<jce/api_core.h>` → `<jce/os/core/jce_*.h>`
- **Deps allowed (PUBLIC link)**: `sdl::sdl` (for thread/atomic primitives used internally; do NOT expose SDL in public headers).
- **Deps (PRIVATE)**: `mimalloc-static`, `cjson::cjson`, `enkits::enkits`, `zstd::libzstd_static`, `xxHash::xxhash`, `physfs-static`, optionally `Tracy::TracyClient`.

## Subsystem map

| Public header | TU | Role |
|---------------|----|------|
| `jce_alloc.h` | `jce_alloc.c` / `jce_allocator.c` / `jce_memory.c` | mimalloc-backed `jce_malloc/realloc/free`; pool + arena allocators |
| `jce_mem_profile.h` | `jce_mem_profile.c` | Tag-categorised memory accounting (P3-A.5). Voluntary `record_alloc/free` from high-traffic subsystems; backs the editor Memory Profiler panel. Process stats sourced from `mi_process_info`. |
| `jce_log.h` | `jce_log.c` / `jce_log_ring.h` | Async MPSC ring → backend IO thread; level/colors/file/per-thread name |
| `jce_math.h` | (header-only) | `jce_vec2/3/4`, `jce_quat`, `jce_mat4` (column-major). **The math API. Do not add cglm.** |
| `jce_filesystem.h` | `jce_filesystem.c` / `jce_filesystem_host.c` | PhysFS-backed VFS + raw host FS for tools |
| `jce_path.h` | `jce_path.c` | Path normalize, join, exe-relative. The decomposition family (parent/basename/stem/extension) may be called with `out == path`; every read of `path` must precede the first write to `out`, and `tests/os/core/test_jce_path.c` asserts it. |
| `jce_thread.h` | `jce_thread.c` | Mutex/cond/atomic/thread primitives (SDL backed) |
| `jce_jobs.h` | `jce_jobs.c` | Fixed-pool job system + JceJobGroup (cooperative wait). Uses enkiTS internally where possible. |
| `jce_timer.h` | `jce_timer.c` | High-precision time |
| `jce_fixed_clock.h` | `jce_fixed_clock.c` | Glenn-Fiedler fixed-step accumulator (P3-B.2). Drives `JCE_PHASE_FIXED_UPDATE` (default **60 Hz**, 0.25 s spiral cap); exposes `jce_fixed_clock_default()` + tick/alpha for renderers/netcode. **P1-fixed-clock-unify**: `jce_fixed_clock_default()` is the single source of truth for the fixed cadence — the per-runtime physics clock adopts its `fixed_dt`, so the default matches physics (was 50 Hz, which desynced). Determinism = caller's responsibility. |
| `jce_event.h` | `jce_event.c` | Event queue primitive |
| `jce_handle.h` | `jce_handle.c` | Versioned handle table (generation+index) |
| `jce_config.h` | `jce_config.c` | INI-style config loader |
| `jce_json.h` | `jce_json.c` | Thin cJSON wrapper |
| `jce_str.h` | `jce_str.c` | String utilities (UTF-8 safe) |
| `jce_i18n.h` | `jce_i18n.c` | Translation table lookup |
| `jce_sysinfo.h` | `jce_sysinfo.c` | CPU/RAM/OS introspection |
| `jce_process.h` | `jce_process.c` | Child process spawn/wait (build tools) |
| `jce_profiler.h` | `jce_profiler.c` | Tracy zone macros (gated by `JCE_TRACY_ENABLED`) |
| `jce_trace.h` | `jce_trace.c` | Bounded, allocation-free producer ring for thread/task/wait/frame/counter events; cursor consumers, aggregate stats, and explicit Chrome/Perfetto JSON export |
| `jce_crash_handler.h` | `jce_crash_handler.c` | Signal handler + stack walk; preserves `-fno-omit-frame-pointer` requirement |
| `jce_coro.h` | (header) | Coroutine primitive |
| `jce_defs.h` | (header) | `JCE_PLATFORM_*`, `JCE_ARCH_*`, `JCE_API`, `JCE_CALL`, `JCE_INLINE`, `JCE_EXTERN_C_BEGIN/END` |
| `jce_asset_types.h` | (header) | Shared asset enum/types used by resource layer |

## Rules

1. **No platform macros** in public headers. Detection is via `JCE_PLATFORM_*` in `jce_defs.h`.
2. **No libc on hot paths** in users' code; this layer IS the wrapper — internally `malloc`/`pthread` are allowed only here (and routed to mimalloc / SDL primitives).
3. **No upper-layer includes** — this layer has zero knowledge of renderer/middleware/runtime/app.
4. **Tracy is compile-time gated**: every profiler call must compile to zero when `JCE_TRACY_ENABLED=0`.
5. **Buffers returned from FS APIs must be freed via `jce_free`**, not libc `free`.
6. **Thread-safety**: log/alloc/jobs are MT-safe; document any new API that isn't.
7. **Frame pointers preserved** (`-fno-omit-frame-pointer`); don't override.

## Adding a primitive

- Header in `engine/include/jce/os/core/jce_<thing>.h` (C99-only, `JCE_EXTERN_C_BEGIN/END`).
- Impl in `engine/src/os/core/jce_<thing>.c`.
- If new external dep needed → owner approval + `THIRD_PARTY_LICENSES.md`.

## Don't

- Don't add C++ here.
- Don't call any other JCE layer.
- Don't pull in SDL types into public headers (use opaque structs or POD).
- Don't introduce a second allocator/logger/job system.

## Seekable input

`jce_read_source.c` implements public shared file/memory/callback input with 64-bit
offsets, one 64 KiB read-ahead cache, reference ownership and synchronized reads.
Host-file input is explicit; it must never quietly fall back to full reads/mapping.
