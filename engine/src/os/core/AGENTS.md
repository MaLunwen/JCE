# engine/src/os/core — Core / Foundation (L2)

Core sampling calls the private OS `jce_dbghelp_native_internal.h` adapter for
optional module-list refresh. The adapter borrows the already-linked DbgHelp
module; a missing export returns false with CALL_NOT_IMPLEMENTED rather than
adding a static OS import or pretending to refresh. Existing symbol lookup and
its ownership remain with the sampler. The complete sampler typed-port controls
cover present/missing/provider-failure behavior; XP runtime/PE evidence remains
separate from these CPU tests.


The private `jce_sha256_internal.c/.h` owns SHA-256 once for Renderer payload
fingerprints and Archive HMAC streams. The implementation moved from Archive;
Resource keeps its existing private HMAC/erasure entry points and delegates to
Core. State and schedule erasure use the same volatile Core helper. No public
crypto API, dependency, allocation, filesystem or clock is introduced. Fixed
Core known-answer/padding/chunking controls and Archive RFC 4231 short/long-key
HMAC controls constrain this shared owner.

`jce_fs_read_pak_bounded` is an explicit archive-only public read/probe. It
uses only the supplied FS's existing PAK/provider precedence, never global
PhysFS mounts or active/host fallback. The first matching entry owns failure;
metadata enforces caller bounds and sentinel headroom before allocation/decode.
Absent, failed, over-limit and OK are distinct. A NULL data output probes only
metadata; successful owned bytes use the existing Core allocator/free route.
Provider/mount configuration retains its owner-thread contract and reads use
the existing FS lock. Ordinary open/read_all semantics are unchanged.

The private logger final-close getter records the actual final file flush and
close API results, reset by init and retained across idempotent shutdown. Close
still executes after a failed flush and storage is released normally. No file,
unobserved/non-async shutdown, API success and API failure are distinct. These
results certify the observed API returns, not durable media or earlier writes.
The actual logger fixture forwards native IO, injects terminal return failures
after real calls and checks no replay, double close or false success.
The logger's minimum level, color selection and final-close result share the
single `g_log_status` aggregate. This is existing configuration/result ownership,
not a new synchronization mechanism: init resets only final-close observation,
normal color detection remains, and shutdown retains configured level/colors.
The old-close-success fixture mutates the aggregate member's actual assignment.

Both logger entry points share one private, single-pass formatter. The fixed
512-byte message and existing ring remain unchanged. Up to 511 formatted bytes
are preserved verbatim. Truncation ends on a valid UTF-8 boundary and appends
` [jce_log_truncated required_bytes=N]`; N excludes the terminating NUL and is
the original formatter's required byte count. A negative format return emits
`[jce_log_format_error]` instead of presenting uncertain partial bytes as valid.
The formatter adds no allocation, recursive log or second formatting pass.
Actual-owner sink/file controls cover both public entries, native async delivery
and quiescent synchronous delivery. This marks bounded evidence as incomplete;
structured producers must still budget or split records needed for analysis.

The private `jce_async_session_internal.h` adds an Engine-session creation latch
under the existing default-executor guard, separate from transient shutdown's
`g_default_closing`. It starts open for standalone consumers. Engine opens it
only after publishing a valid owner and closes it before DRAINING, retaining the
closed state through teardown and between sessions. Public shutdown/retry still
clears its own transient flag and never reopens this latch. Existing default
lookup, pump and stats stay available for a retained executor; after retirement,
lookup cannot create a new pool until the next admitted Engine session. This is
lazy-creation admission, not revocation of borrowed pointers or independent
executor ownership. The transitions invoke no callbacks and introduce no new
thread, mutex or queue. Actual Core and headless Engine callback controls cover
standalone recreation, failed initialization, constructor borrowing and reboot.

The shared frame-work pool has the same private session boundary through
`jce_thread_session_internal.h`. Its separate `g_shared_closing` is set under
the existing guard before detaching the old pool and stays set throughout the
real native join. Work completing during that join receives NULL from shared
lookup, and recursive shared shutdown returns without joining itself. Clearing
this transient flag preserves standalone recreation; it never opens the Engine
session latch. Existing borrows remain queryable before their actual shutdown.
The actual-worker control releases a running task at the real join boundary;
an old-closing-wire mutant is drained safely and must fail ordinary assertions.
Idle worker creation is not proven absent merely by a zero trace-thread count.

`jce_executable_path.c` owns the public filesystem current-executable query.
Its first include is the private Platform native adapter so POSIX declarations
are selected before libc under strict C99. It does not add a Core-to-Platform
target edge or change SDL base-path/current-directory behavior. Windows and
Linux query the running image directly, independently of cwd/argv[0], and
refuse truncation, allocation/conversion/OS failure. Native separators remain.
Other targets currently return false with valid output cleared; macOS/mobile/
Web support is not inferred from a desktop or interpreter path.

The shared mimalloc accounting owner uses the private platform native-word
adapter for every producer update and shared observation, including debug
live/bucket counters, peak CAS and Release per-frame totals. Alignment alone
does not make plain C increments MT-safe. These operations add no allocator,
SDL, lock or initialization recursion. Observations are individually atomic,
not a globally consistent multi-counter snapshot; native-word wrap remains.
`jce_alloc_frame_delta` has exactly one consumer thread owning its plain
last-read baseline. Reset only after all producers are quiescent. Realloc
continues to account old storage only after the real realloc succeeds.

The actual accounting-owner fixture uses real mimalloc and JCE worker/barrier
services with isolated accounting symbols; joined balances and single-consumer
batch deltas exclude thread-service allocation noise. Its forwarding native-op
ports observe actual atomic operations. The old plain-RMW control executes only
single-threaded protocol assertions, never concurrent C UB or probabilistic
race reproduction. The standalone adapter fixture links no engine/SDL/mimalloc;
neither fixture certifies uncompiled 32-bit/Web/other architecture behavior.
The same worker fixture is compiled with existing NDEBUG selection both ways:
debug tracking checks live/buckets/peak; Release still checks concurrent pf
totals and the disabled snapshot's zeroed result. MSVC Interlocked ordering can
be stronger than GNU relaxed ordering; lock-free is not a zero-overhead claim.

> Lowest engine layer. Everything else depends on this. See repo-root `AGENTS.md`.

## Identity

- **Layer**: L2 (OS Core). Pure C99. No graphics, no platform-window code.
- **Public umbrella**: `<jce/api_core.h>` → `<jce/os/core/jce_*.h>`
- **Deps (PUBLIC link)**: none. Public headers do not expose dependency types.
- **Deps (PRIVATE)**: `sdl::sdl` (internal thread/atomic primitives), `mimalloc-static`, `cjson::cjson`, `enkits::enkits`, `zstd::libzstd_static`, `xxHash::xxhash`, `physfs-static`, optionally `Tracy::TracyClient`.

## Subsystem map

| Public header | TU | Role |
|---------------|----|------|
| `jce_alloc.h` | `jce_alloc.c` / `jce_allocator.c` / `jce_memory.c` | mimalloc-backed `jce_malloc/realloc/free`; pool + arena allocators |
| `jce_mem_profile.h` | `jce_mem_profile.c` | Tag-categorised memory accounting (P3-A.5). Voluntary `record_alloc/free` from high-traffic subsystems; backs the editor Memory Profiler panel. Process stats sourced from `mi_process_info`. |
| `jce_log.h` | `jce_log.c` / `jce_log_ring.h` | Async MPSC ring → backend IO thread; level/colors/file/per-thread name |
| `jce_math.h` | (header-only) | `jce_vec2/3/4`, `jce_quat`, `jce_mat4` (column-major). **The math API. Do not add cglm.** |
| `jce_filesystem.h` | `jce_filesystem.c` / `jce_filesystem_host.c` | PhysFS-backed VFS + raw host FS for tools |
| `jce_path.h` | `jce_path.c` | Path normalize, join, exe-relative. The decomposition family (parent/basename/stem/extension) may be called with `out == path`; every read of `path` must precede the first write to `out`, and `tests/os/core/test_jce_path.c` asserts it. |
| `jce_thread.h` | `jce_thread.c` | SDL mutex/cond/atomic/thread/TLS primitives and enkiTS frame fork/join pools, including the process-wide shared frame pool. |
| `jce_async.h` | `jce_async.c` | Bounded structured asynchronous executors/tasks with owner-thread completion, cancellation and join-before-release; enkiTS workers or cooperative Web execution. Resource adapters consume this owner rather than a second jobs implementation. |
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
| `jce_env.h` | (header-only) | Pure borrowed-string policy for legacy environment flags: NULL, empty and first character `0` are false; every other nonempty value is true. No environment lookup, allocation or state. |
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

The sampler and Windows crash label share the private Platform
`jce_thread_description_native_internal.h` adapter. Thread descriptions are
optional: resolve the loaded KernelBase export first, then loaded Kernel32,
and retain the existing thread-ID label when neither exists or the call fails.
No post-Windows-7 thread-description symbol is statically imported. Borrowed
module handles are not released; each caller releases any native description
exactly once, including API failure or an empty description. The deterministic
sampler owner ports cover module/export absence and name-output ownership;
actual executable import admission and target-OS execution remain separate.

The optional Windows instruction-pointer sampler owns a bounded session roster
of 128 immutable (thread ID, creation time) identities, excluding the collector.
The private uint16 thread index keeps the main-thread sentinel separate; histogram
and thread/function budgets remain unchanged. New lifetimes beyond the retained
roster are counted as skipped coverage. Historical histogram slots
are never recycled when threads reorder or retire. Native collector join must
be confirmed before report/free/reset or host-owned suspend recovery. Refused
shutdown retains ownership and blocks reinit. These diagnostics are aggregate
PC counts, not call stacks or timestamped/GPU causality. Controlled native-port
CPU tests do not certify actual OS suspension recovery or a total shutdown
latency bound.

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

Realtime epoch milliseconds are available through jce_time_now_epoch_milliseconds; monotonic perf counters remain the duration authority. The consumer preserves original frame-based movement and wall-dt shooting/waves. Explicit fixed-step reference fixtures are distinct from realtime interaction.

Assertion/ensure logging supplies source file and line before variadic format arguments, matching the public logger ABI on strict C99/wasm as well as native builds.

The shared Chrome JSON encoder appends contiguous ordinary name bytes as one
checked explicit-length span; only quotes, backslashes and control bytes expand.
It retains lowercase six-byte control escapes, raw non-control bytes, first-NUL
termination and the fixed event-name contract. Length-plus-NUL and size growth
are checked before memory access; whole-batch failure restores size, first-event
state and the terminating NUL. This consumer optimization does not change the
producer ring, sampling, event order/schema, capture ownership or overflow policy.

Trace JSON exports carry `jceTrace` recorded/captured counts, separately named
pre-capture ring overwrites and cursor losses, their total `lost`, and capture
reset-generation stability. Task-end args retain the actual success/failure/
cancellation state. These metadata describe capture coverage, not proof that all
expected lifecycle phases occurred; callers must reject incomplete evidence.
The bootstrap scope CPU target validates actual nested public trace events and
parent restoration without starting a graphics device.

Trace event timestamps use the existing performance counter/frequency through
`jce_trace_clock_internal.h`, not SDL lifetime-relative ticks. SDL quit/reinit
does not erase the ring or its pre-start prefix. Integer conversion floors to
nanoseconds exactly, with a bounded wide-product path and no floating-point
rounding; invalid frequency or uint64 overflow leaves its reserved sequence
unpublished, so complete capture accounting cannot pass. No global timer API or
frame-clock behavior changes. Capture payload-write start/finish now share this
helper and declare `writer_diagnostics.clock_domain=process-perf-ns`; convert
Chrome event microseconds before joining these nanoseconds. Untagged historical
samples retain an unknown epoch. Failed/overflowing reads or reversed intervals
latch write failure: pre-write failure attempts no IO, and post-write failure
never replays retained staging on poll/stop. Join, close and buffer release still
run, and no complete formal trace is published. Flush deadlines and local
encode/drain/join/close durations retain their SDL-lifetime clocks; they are not
exported absolute event-axis samples. Native controls cover retained prefix
across real SDL restart and both failed clock edges with actual owner cleanup.

Process memory fields use the existing mi_process_info provider, preserving native
behavior. The Emscripten primitive does not observe OS process memory: its mimalloc
commit estimate must not become RSS/commit (or peak) evidence. jce_mem_stats zeros
these four Web fields as unavailable while retaining provider elapsed/page faults
and the copied/NULL contract. Zero is N/A, not a measured zero process footprint.
Allocator-specific heap fields must never alias process commit/peak or voluntary
tag totals; partial tagged accounting retains its existing independent scope.

Private synchronous service spans live in jce_trace_scope_internal.h. Application
aliases and renderer adapters reuse this single implementation, preserving parent
IDs and actual failure/cancellation with no allocation or side registry.

One ProfileState owns tag counters and simultaneous aggregate current/peak under
the same lock and lazy-init lifetime. get_total retains the sum-of-tag-peaks
contract; get_aggregate_totals reports voluntary tagged simultaneous high-water.

The shared encoder's integer-only args use a checked ASCII scratch writer in
the existing event-local suffix buffer, followed by one explicit-length append.
It preserves the original unsigned casts and signed int64 interpretation;
unsigned subtraction forms INT64_MIN's magnitude without signed overflow.
Scratch capacity failure appends no args, and the batch retains its original
rollback. The mixed category/phase/thread/timestamp snprintf is unchanged,
including double nanoseconds-to-microseconds conversion and %.3f rounding.
Do not replace that timestamp with integer quotient/remainder under a same-byte
claim. No event-type/category validation or wire/schema change is added.

Optional private trace capture continuously consumes the existing bounded ring
and shared Chrome encoder. Its owner starts it before producers and stops it
after producer joins/main unregister but before SDL shutdown. Stop joins the
capture worker and performs a final owner drain; loss, generation changes,
unclosed work/waits and IO failure are explicit, never GPU completion proof.

Continuous capture's optional `reader_diagnostics` version 1 summarizes the
same reader's existing reserved-sequence snapshot, cursor progress and stop
reason: caught-up, batch-limit, slot-busy, reserved-unpublished or invalid args.
The public reader and private observed entry share one implementation. Busy
slots expose no unlocked published value; no producer checks, ring capacity,
backpressure, sleep policy or event/schema changes accompany this observation.
One capture owns fixed read/stop/loss counters, its last read and exactly two
loss witnesses (first and last), each with the prior read snapshot. Only losing
reads request a checked process-performance-clock sample. The exported point is
post-read detection time, not an overwrite instant, scheduler cause or evidence
about missing events. An unavailable clock remains explicitly false with zero
as an unavailable placeholder. Threaded, cooperative and terminal-drain phases
remain distinct. Fixed checked serialization retains incomplete captures and
all existing completeness/publication rules; old captures need no new block.

`jce_trace_capture_quiesce` is a private owner-thread join-only barrier. It keeps
the stream/cursor/staging alive for stop's final drain, and repeated/NULL calls
are safe. The result's `worker_available` records successful startup, even after
prejoining; it is not current worker liveness. Normal stop still invokes this
barrier. The join snapshots its enclosing task id once and uses it on both wait
edges; wait-begin's implicit-parent behavior is not shared by wait-end. Engine
calls it before `jce_log_quiesce_internal`: the capture writer
may log, so it must exit before the log backend drains/joins. Logger quiescence
retains file/sink locks and switches both variadic entry points to synchronous
emission, keeping final capture diagnostics visible. Flush still flushes that
file. Init may restart the retained ring; shutdown performs final release once.
These terminal/configuration operations require a single owner and no concurrent
log producers or sink reentry. The logger uses its existing raw SDL worker;
trace active-thread counts do not certify it. Engine's completed log-quiesce
scope and the native SDL join control explicitly cover this ownership boundary.

Continuous capture owns one lazy reusable buffer in the existing Chrome encoder,
merging 512-event reads before host append at approximately 1 MiB. Threaded
mode now also retains short/empty-read batches across polls until 16ms from the
first pending encode start; cooperative polls keep immediate flush behavior. Its hard limit
adds one batch at the exporter-validated 1024-byte event bound and one NUL; no
second event registry is introduced. Full 128-batch passes
resume draining without an idle sleep while still checking stop each pass. Empty
or short reads retain the bounded poll sleep. Encoder OOM/limit, short append and
host CloseIO failure reject capture; join precedes exactly one staging release.
Already consumed events are counted captured only after successful payload
append, and failed output never publishes a success footer. The old allocating
batch encoder remains a wrapper of the same append-into transaction. Failed
encoding restores buffer size and comma state so it cannot publish partial JSON.
Footer writer_* fields count attempted encoding/appends, successful payload bytes
excluding header/footer, retained buffer capacity/limit, and maximum measured
encode/append/drain durations. These are nonrecursive consumer CPU/IO observations,
not business spans, physical GPU completion or a throughput guarantee. Existing
zero-loss/generation/active-owner gates remain mandatory. Actual ring/SDL fixtures
and compiled exact-owner OOM/old-write/old-sleep/old-close ordinary failure controls
cover distinct scopes; an application capture must still be measured separately.

Capture output publication requires a fresh caller-exclusive final destination.
The private host-FS reserve-new owner uses SDL_IOFromFile("wbx") as exclusive
creation authority, confirms the full initial write and CloseIO, and reports
created ownership even when that write/close fails. A failed stat/unique-path
query is never absence authority. Existing files/directories are refused without
removal or overwrite. Public `jce_fs_host_write_new(path,data,size)` is the thin
traced facade over this same exclusive owner; editor/tools consume the public API
instead of engine-private headers. False can leave a newly created partial file
and must not be interpreted as absence; no second exclusive-create algorithm or
cleanup/overwrite fallback is permitted. First reserve the exact final path with invalid-JSON
JCE_TRACE_PENDING bytes, then exclusively create the nonce sibling .partial
payload through the same host owner. Only complete history plus successful final
footer append/CloseIO and a capped byte-exact match of this owner's unchanged
reservation can reach the public jce_fs_host_rename transaction. Failures leave
the PENDING marker and any created partial for
diagnosis and reject published/capture_complete; startup/failure logs name the
stage and final/partial paths once. A partial can contain complete positive JSON
bytes after close/rename failure and is not a published capture certificate.
Readers must select valid JSON at the requested final path, reject PENDING, and
never discover a partial as fallback. No RemovePath or overwrite fallback exists.
The public rename directly uses SDL_RenamePath without deleting the old target;
it only replaces this capture's verified reservation. The read/rename boundary
still requires exclusive caller ownership and does not defend malicious external
replacement or claim fsync durability.
Every retained path and encoder buffer is freed after worker join. Real SDL footer
close, rename failure, existing file/directory, injected query failure, and
reservation short-write/close controls complement compiled old-publication and
nonexclusive-open ordinary runtime contrasts; prior throughput results do not
cover this added transaction.

Public jce_fs_host_copy_file_new reuses that same exclusive stream creator and
close owner. It copies the initially reported source length through one 64 KiB
engine allocation, checks terminal EOF, and succeeds only after both handles
close successfully. Existing destinations are never overwritten or removed.
The required out_created is cleared before validation and remains true once
exclusive creation succeeds, including short IO and close failures; only that
caller may remove its failed candidate. The API itself never removes it.
Optional fs.host.copy_file_new details retain source/destination request hashes,
reported length, SDL-accepted read/write bytes, known bits, creation, first
failure stage and separate closes in the existing scope/ring. Disabled or
consumer-suppressed calls do not hash or initialize detail storage. These are
host transfers, not fsync durability, a concurrent-source snapshot or OS CAS.

Filesystem adapters trace actual create/mount/open/read-all/write-all/atomic/copy/
rename/remove/close attempts through the shared private scope. Success follows
the existing returned result; NULL cleanup is cancelled. Ambiguous byte-read
zero/EOF is not falsely labelled success. Host-tool recipes link the same core
trace/timer owner. Capture/export IO suppresses only its consumer thread to
avoid self-recording; ordinary producers remain observable.

Host capped reads clamp the nonnegative SDL file length in uint64_t before
converting to size_t and reject a clamped length above SIZE_MAX-1, preserving
room for the NUL sentinel. The total-size output keeps the reported full file
length; zero caps still return an allocated empty buffer. Existing exact-owner
host controls add real SDL small/zero-cap reads and counted large-length/allocation
ports. The size32 variant models only 32-bit length arithmetic while including
the actual owner, not a 32-bit OS/ABI run. Its build-local old narrowing/admission
wire must fail ordinary assertions; ports refuse giant or wrapped-zero allocation
requests rather than permitting a crash or a multi-gigabyte allocation.

Host whole-file reads likewise reject nonnegative uint64_t lengths above
SIZE_MAX-1 before narrowing or allocating the text sentinel. Their existing
active-VFS dispatch and buffer owner stay unchanged. The exact-owner fixture
also preserves real small/empty file bytes and NUL sentinels, once-close on
unknown size or allocation refusal, and modeled size32 last-safe/overflow
metadata admission. Separate old capped/read-all wire mutants each remove one
uniquely checked production guard while retaining the other; both must fail
ordinary assertions with bounded allocation ports. This is native host IO and
modeled arithmetic evidence, not a physical 32-bit OS/ABI qualification.

VFS creation fails closed if its process-global mutex cannot be created: free
the candidate and leave PhysFS/user counts untouched. The existing main-thread
create-before-workers contract and process-lifetime lock ownership remain.

Trace ring producers are bounded and allocation-free, not lock-free: each slot
owns a short SDL spinlock around event copy/publication. Logical lock storage is
capacity * sizeof(SDL_SpinLock); actual slot/ring size includes compiler padding
and is measured by test_jce_trace_publication, not inferred from field sizes.
The fixed power-of-two ring holds 65,536 events. At an observed 120-byte slot
layout it occupies 7.5 MiB, 5.625 MiB above the former 16,384-slot bound; this
native layout does not certify other ABIs. Disabled tracing still reserves BSS
address space, and reset/writes touch its pages; no zero-RSS/commit claim follows.
Web static storage consumes linear-memory budget even with tracing off and needs
target-specific link/memory validation. Explicit resident export additionally
allocates one capacity-sized event copy and JSON; continuous capture retains its
fixed 512-event batch and existing bounded text staging instead.
The continuous drain budget is 128 batches, covering the entire ring in one
quiescent final drain. Both export forms report actual capacity in metadata.
Extra backlog allowance is not a losslessness guarantee: overwrite, incomplete
tail, generation, work and IO refusal rules remain unchanged.
No IO, allocation or callback runs under this lock. Consumers use try-lock and
retain the cursor at pending slots. Delayed old publishers cannot replace newer
wraps; overwrite remains reported loss. Reset requires quiescent producers and
consumers and clears publication plus slot lock state.

Trace event sequences use SDL's unsigned 32-bit CAS. UINT32_MAX is a permanent
exhaustion sentinel, never an event; the final legal sequence is MAX-1 and its
consumer cursor can remain at MAX without wrapping. Exhaustion disables producer
admission and survives disable/re-enable. Only quiescent reset clears it, after
which recording can be explicitly enabled again. The same private validity owner
guards continuous start/drain/final publication and resident export; metadata
and the private capture result retain sequence_exhausted separately from loss,
generation and IO. Numerically balanced counters cannot authorize an exhausted
capture. This finite public cursor ABI is not advertised as unlimited history.

One private TraceIdentityState owns capture ID allocation and the registration
accounting epoch, both reset only at the existing quiescent boundary. Its selected
64-bit native-word or 32-bit lock/value strategy occupies the same object; mutually
exclusive implementations are not separate process owners. No new allocation,
thread, lock, public layout or duration-maximum storage is introduced.
IDs retain their full uint64 range: 64-bit hosts reuse the existing allocation-
free native-word CAS adapter; 32-bit hosts protect its uint64 member with the same
short SDL spinlock. No heap/mutex owner or registry is added. Reserving beyond UINT64_MAX
reports through the Core assertion owner and unconditionally aborts even if an
assertion handler returns; returning zero or a recycled ID violates the public
contract. Reset/seed of this counter requires the existing quiescent reset rule.
One compiler-TLS TraceThreadState owns full-width task ID, registration generation,
active status and nested pointer-width suppression. MSVC uses __declspec(thread),
GCC/Clang use __thread, and unknown compilers fail explicitly rather than sharing
state between threads. These scalars allocate no JCE heap and perform no SDL TLS
lookup. Suppression overflow/underflow, thread isolation and generation-sensitive
registration remain explicit contracts; compiler-target validation is still needed.
The enable setter rechecks validity after its final enable store, closing the
cold-path race with a producer's exhaustion store without adding a per-event
validity read. Capture generation changes on re-enable and still invalidates an
older continuous stream. Active-thread accounting instead uses a separate
single-writer TraceU64 epoch advanced only by quiescent reset. Re-registering an
already-counted thread across off/on emits no second lifetime or increment;
actual unregister retires its count even without re-registering. A registration
from an earlier reset epoch cannot decrement the new epoch's count. Reset is
explicit accounting restart, not evidence that an old native thread exited.

Profiler zones reuse their existing private per-thread span stack to retain and
restore the enclosing trace task. Recorded zones submit SERVICE owners before
begin/end; beyond the bounded 64-level stack they do not alter trace task state.
The public JceProfileZone layout is unchanged. Native nested-service and depth
boundary controls validate the real Tracy-enabled bridge; disabled builds report
that bridge control as ignored rather than claiming it executed.

enki pool trace registration remains lazy on real worker task execution. The
scheduler's existing threadStop callback unregisters on that same worker before
its actual exit/join; task completion is not thread exit. Default scheduler
configuration and creator-thread ownership are retained. No cross-thread trace
counter reset substitutes for worker lifetime completion.

Relative script/data/cursor addresses resolve through the active mounted VFS
before loose host-path resolution. ISOLATED misses never fall through to host
files or the runtime PAK. Explicit absolute tooling paths stay host-owned.
The capped path uses core's private jce_filesystem_read_internal.h entry: PAK
metadata is checked before eager decompression, PhysFS size before allocation,
short reads fail, and the existing PAK file buffer transfers without a second
copy. Runtime/Lua controls use real archives, an actual oversized asset and
observe zero oversized decompression calls; loose OVERLAY fallback is retained.

Structured async executors and frame pools share the private
jce_enki_trace_internal.h worker-stop callback. Both scheduler owners attach the
existing enki lifecycle hook; default async shutdown must preserve actual worker
exit events before continuous capture stops. Task completion alone is insufficient.

enki workers are created outside SDL. Their shared actual worker-stop bridge
calls SDL_CleanupTLS after trace unregister to release SDL-owned TLS containers
and user-registered destructors, including trace-off lifetimes. Trace identity
state itself uses compiler TLS, not allocated JCE heap objects. Actual
worker destructor controls verify cleanup occurs before each scheduler join
returns; activity counters alone do not certify heap or GPU completion.

Private scoped counters preserve explicit id/parent through the same ring and
Chrome encoder as public counters; they do not register a producer thread or
create a SERVICE/task lifetime. Thread activity changes only at explicit
registration/unregistration owners. A callback counter must not invent thread
ownership or invoke worker TLS cleanup. Resource-specific closure is validated
separately from the generic continuous CPU footer; zero activity and zero loss
alone do not prove that every resource allocation has been released.

CPU phase reports reset only the reporting-window totals. The existing private
per-frame bucket retains pending samples independently until frame_tick consumes
it exactly once. Report/average calls before that tick cannot erase the completed
frame; duplicate ticks with no new samples preserve its snapshot. An explicit
zero-duration sample marks a new frame and clears the previous nonzero snapshot.
Snapshot availability records accepted samples rather than positive duration;
zero-duration snapshots remain available to CSV export and introspection. Ticks
before any samples leave it unavailable. The native controls cover zero-row
export, initial availability, two 120-frame boundaries and unchanged averages.

Duration and frame clocks share jce_timer.c and the public jce_time_perf_*
provider. Native keeps SDL's performance counter; Web uses the browser monotonic
emscripten_get_now clock through a private platform branch. Web ticks reuse that
provider, while epoch time remains a separate realtime clock. Nanosecond units
do not promise nanosecond resolution: browser privacy may coarsen observations.
Renderer duration callbacks must use the same counter/frequency provider.
Deterministic provider controls and actual installed-SDK/browser probes are
separate evidence; a native seam alone does not certify Web execution.

Host atomic save uses the existing SDL rename replacement owner. A failed write,
close or rename must leave the old destination intact; cleanup touches only the
sibling candidate and must not delete the destination or retry destructively.
Current fixed temporary naming requires caller serialization and does not imply
power-loss durability. Shared path absolute classification rejects drive-relative
C: and C:file; native fully-qualified admission must additionally account for
current-drive rooted paths. Renderer cull timing state must be initialized before
all fast-path branches; clocks represent their documented measured interval.

Private jce_atomic_storage.h shares the existing JceAtomicI32 representation for
embedded engine service flags. Public atomics remain opaque. Embedded storage
uses the existing atomic operations and never calls the heap-owning destroy.

Private embedded atomic storage also supplies the typed CAS operation used by
bounded renderer callback counters. It retains the existing OS-owned
JceAtomicI32 layout; renderer code does not name SDL atomics. Failed expected
comparisons leave storage unchanged. Neither the storage nor its accessors own
heap lifetime.

jce_trace_counter_ring_internal is a private callback entry over trace_emit,
the existing single bounded publication writer. It preserves category, id,
parent and value while bypassing optional profiler plots. It does not create
tasks, register threads, set task TLS or invoke external consumer callbacks.
Public and ordinary scoped counters retain their existing profiler behavior.
Exact production-TU controls in tools/tests/test_bgfx_allocator_trace.py reject
the actual profiled-counter owner as a runtime negative; native renderer
observer controls separately consume the real ring and OS atomic operations.

## Toolchain override admission

Explicit overrides reuse automatic discovery's kind-specific executable/SDK
marker helpers. Empty directories, missing markers, marker-named directories
and arbitrary files cannot set present/from_override. Per-executable tools
accept their bin directory or the correctly named executable. MSVC accepts the
Visual Studio root or its VC/Auxiliary/Build/vcvarsall.bat and always stores the
VS root consumed by the build manager. SDK overrides use root directories.
Rejected overrides keep the existing automatic fallback with from_override
false; clearing an override restores discovery. Optional version-query failure
does not invalidate an existing executable/marker.

This is plausible discovery, not a certificate that a compiler, target SDK,
VM, signing setup or ABI closure can execute a build. Xcode/Command Line Tools
share the Developer/usr/bin/clang marker; it does not certify an iOS SDK.
test_jce_toolchain_override compiles the actual owner with real temporary
host files and same-signature environment/process ports, never a mirror probe
or an executed fake compiler.

Automatic discovery and optional version queries own both nonblocking output
pipes, including when a windowed caller has no standard stderr handle. Only a
successfully exited, drained stdout query supplies its first nonempty line;
stderr is discarded and cannot supply a path or version. The fixed stdout
prefix, discard scratch and existing cooperative timeout remain bounded, and
full stdout storage cannot stop draining either pipe. Timeout, nonzero exit and
blank output reject query results; optional version failure preserves an already
verified executable/SDK marker. The actual-owner fixture retains override
controls and adds automatic MSVC, cleanup and dual-pipe drain observations.
Its separate old-stderr wire must compile then fail ordinary assertions, with
normal/mutant temporary directories isolated. A public SDK windowed-process
handle comparison is separate evidence; these ports do not execute a compiler.

Path normalization reads its borrowed source before changing the output. The
ordinary, same-buffer and backward-overlap paths remain allocation-free; only
forward overlap snapshots the source through the existing JCE allocator, with
OOM leaving input intact and one release on every acquired exit. Dot-parent
resolution reverses over actual already-written segments, without a fixed depth
registry; relative leading parents cannot cancel each other. Existing
drive/absolute-prefix policy remains, including the current leading-separator
collapse (not a UNC host-resolution certificate). The exact-owner path fixture
retains all previous controls and adds real overlap, small final capacity,
allocator failure/cleanup, relative parents and more-than-256-segment controls.

Default worker budgets share private jce_worker_budget_internal.h. The actual
config publisher always pushes LOW=1 or full=cores-1 capped at eight before a
lazy frame scheduler exists; an auto/full publication also clears a historical
uncreated-pool pin. Engine boot resolves RAM/core/config/JCE_LOW_MEM once and
publishes that effective worker budget into the existing shared-pool authority.
Public machine-class/job-worker accessors and authored JceConfig fields retain
their loaded values, preserving renderer AUTO decisions. Structured async reads
the same existing shared budget without creating a pool; explicit env=0 restores
the full default even when the authored class is LOW.
Explicit job_workers keeps its prior frame ceiling of sixteen and async ceiling
of eight. Publishing after a pool exists does not resize its owned workers.
Cooperative Web execution, cancellation and join-before-release stay with the
existing owners; no second scheduler or global configuration mirror is added.
The worker-budget controls observe real enki worker counts and real public
headless engine first-bootstrap/failure-retry paths. Build-local actual-owner
mutants remove the LOW publisher or effective-engine publish wire and must
compile then fail ordinary assertions. These controls do not certify a total
process thread count, GPU threads, one-core CPU usage or low-memory residency.


Continuous trace capture reserves its formal path using exclusive invalid-JSON
PENDING bytes, then holds one exclusively created partial SDL_IOStream through
header, payload batches and footer. The existing host filesystem private stream
helpers own exclusive open, full write and once-close; reserve_new composes the
same helpers. There is no new IO backend, extra ring, per-batch reopen/close or
sampling policy. The owner joins the consumer before final drain and final
close; a write, close or reservation read-back failure retains diagnostic files
and forbids formal publication. Close clears its handle even on failure.
The footer's writer_append_calls/max_append_ns retain compatibility while now
counting/measuring payload stream writes. writer_io_model distinguishes this
protocol. One explicit-capture summary records actual final close duration and
maximum write/drain times; an observed >=2s write warns once without claiming an
AV cause or converting loss to success. The JSON buffer keeps its existing hard
cap. Twenty prior capture controls, fourteen host controls and four publication
controls remain; persistent stream/close-failure controls and actual-owner runtime
mutants are separate from application throughput, device IO durability or GPU.


The sole `jce_process.c` SDL owner resolves each output stream once per spawn.
Capture keeps APP without observing parent handles. On Windows, uncaptured
valid handles (including pipes/files and non-inheritable handles) retain SDL
INHERITED; NULL/INVALID/stale handles use SDL NULL/NUL. Private observation uses
GetStdHandle/GetHandleInformation behind the private platform inline seam, never
changes or closes borrowed parent handles, and adds no core-to-platform library
link or public ABI. POSIX inheritance
remains unchanged. Never substitute BACKGROUND: it changes supervision and exit
semantics. SDL retains child/pipe/NUL ownership; normal poll, kill, reap and
destroy remain the public lifetime authority. Spawn errors are copied into a
bounded owned buffer before property/argv cleanup can overwrite SDL's buffer.

A missing-handle startup records one bounded DIST diagnostic with effective
stream policy and actual spawn status, never arguments/environment/output.
`process.spawn` records the synchronous CPU attempt and four correlated stdio
mode/reason counters; it does not certify child exit or OS completion. Capture
true still requires the caller to drain both requested streams.
`test_jce_process_stdio` compiles the complete owner with real SDL property bags
and typed counted native/process ports; invalid inheritance refuses spawn.
Actual old-inheritance and missing-error-copy controls must compile and then
fail ordinary assertions. `test_jce_process_gui_stdio` is a Windows GUI target
using only public JCE headers plus Windows SDK. Its real self-child writes both
streams and exits 23; default parent-handle admission returns SKIP 77 if handles
are present. Actual GUI absence, installed-SDK consumption and launcher-provided
inheritance remain separate native evidence; the counted fixture certifies none
of those outcomes by itself.

The private jce_filesystem_pak_internal.h bridge registers the Resource owner's
canonical three-callback tuple and its mount predicate without extending the
public JceFsPakProvider layout. The actual filesystem owner keeps the current
provider, registered tuple, registered predicate and current predicate in one
private static service structure; no independent globals duplicate this state. Both named and legacy mount paths check it before
slot mutation or allocation. Both also refuse a NULL provider, preventing
mount-before-canonical-restore from bypassing admission. The old public setter recognizes the same tuple,
including a copied tuple restored after a custom provider; unknown opaque
providers never reach the native-Pak predicate. Registration and shared mount
mutation retain the existing serial owner-thread contract. This is standard SDK
same-handle inspection admission, not a custom-provider security sandbox.

Host read detail observation stays in the actual read_all/read_capped owner.
Only a nonzero existing service scope initializes metadata or hashes the borrowed
request string with the shared FNV-1a-64 helper; disabled and consumer-suppressed
reads perform neither operation. Fifteen fixed CORE counters use that same
scope id/parent and the existing ring-only entry. No raw path or SDL error text
enters trace, and there is no new public ABI or secondary registry. Default-off
`JCE_FS_OPEN_FAILURE_DETAIL=1` is the sole narrow LOG-only exception: an admitted
Engine owner embeds its two-counter context and lends it to the existing host-read
service only while enabled; no independent file-global diagnostic state exists.
The binding pointer is the sole admission fact, with OFF unbound. Active same or
foreign begin and foreign/repeated end refuse without resetting or detaching an
owner. Only the matching end, after FS/log producer joins, detaches before its
summary; the POD remains alive until Engine free. VFS set/unset, reader replacement
and probe invalidation update their routing tuple only, preserving this separate
Engine-session borrow. This is a synchronous borrow under existing quiescence,
not a general OS-thread/refcount or registry protocol. Rejected boot and deferred
cleanup do not reset a live latch. Only a pre-OPEN admitted,
nonzero scope with compiled logging snapshots the failed SDL provider error
first, using pure C before another SDL/trace/log/allocator call, then the request.
Two WARN records reuse scope id/parent/hash and process trace generation (not a
unique capture ID). Each field retains its original byte length and at most
128 raw bytes encoded as ASCII hex, with explicit truncation and optional=unknown.
The largest record is 470 bytes; actual-formatter native controls remain required.
The shared session budget admits 64 whole groups; refused counts saturate at
INT_MAX, and the terminal attempted/refused summary reports saturation and
`delivery=unavailable`. Filtering, ring drops, rotation, missing pairs, compile-out
and truncation cannot certify complete path/error evidence or absence of failure.
An empty SDL error is an unavailable provider explanation, never a portable errno.
Existing alerts, read behavior and all fifteen counters remain unchanged. Existing
consumer suppression bounds synchronous sink reentry; sinks still cannot log.
Default-off reads do no text copy/GetError/allocation or detail-log work. Request hashes
and all four byte fields use unsigned high/low 32-bit pairs. A known bitset
separates unknown delegated failure sizes from observed zero-byte host attempts.
The first NULL-causing stage survives cleanup; isolated delegated refusal does
not identify why that reader returned NULL. Capped truncation and its historical
short-read/ignored-close success are unchanged. Close and short-read observations
remain independent from task outcome and do not certify durable IO. Whole-file
short reads still fail. Existing exact-owner counted ports and real VFS/ring
fixtures validate details, scope parent restoration and trace-off suppression;
whole-owner missing-detail/stage/enable-guard mutants must compile and fail
ordinary assertions. Fresh rebuilt application captures are needed to identify
new failures; the fifteen older read_all failures cannot gain retroactive paths
or be reclassified as optional misses.

## VFS public whole-read length admission

`jce_fs_read_all` consumes the existing `jce_fs_read_all_bounded_internal` owner
with `SIZE_MAX-1` capacity, before eager PAK decode allocation or PhysFS whole-file
allocation. No second VFS read pipeline is added. Public failures leave optional
`out_size` zero; success publishes the actual accepted bytes. The internal bounded
reader retains its independent observed-total-on-rejection contract. Empty or
unknown-length input remains rejected, opened handles close once, and short loose
reads free their buffer. PAK decode must match its declared whole length; successful
PAK reads transfer the existing decoded allocation rather than copying it twice.
Returned data remains binary and has no general NUL-termination promise across
backends. Sentinel headroom is a representability bound, not a scene/JSON budget,
physical-memory guarantee or permission to allocate enormous assets.

The complete-owner fixture uses typed bounded PhysFS/provider/allocation ports and
actual public read/mount/close paths. Its SIZE32 variant models owner length and
allocator-port arithmetic only, not a native 32-bit architecture or IO performance.
Historical whole-read and missing-output-reset variants must compile and fail
ordinary unchanged assertions; huge/zero allocations and IO are refused by ports.

## Threaded capture coalescing and payload-write observations

The existing consumer retains its bounded staging buffer across threaded polls.
The first pending encode-start clock is not renewed by later events; 1MiB or
16ms requests a write, and pending sleep is limited by that remaining deadline.
Millisecond rounding, scheduler delays and synchronous writes can exceed it.
This is a flush-initiation policy, not a hard completion/latency guarantee.
Stop joins the writer before one forced final drain/flush, including its join
and unregister tail; only a fully accepted payload write commits captured events.
Cooperative/Web and all loss/generation/publication refusal semantics remain.

Optional private footer writer_diagnostics version 1 counts payload-write
attempts only, excluding reservation/header/footer/close. Its inclusive size
bounds are 4096/16384/65536/262144/1048576 bytes plus overflow; wall-time bounds
are 100000/1000000/4000000/16000000/100000000/1000000000ns plus overflow.
The slowest sample retains monotonic start/end and requested bytes, not failed
physical transfer or disk/GPU causality. Fixed counters and checked formatting
use no producer-slot IO, recursive trace emission, new registry or public ABI.
The existing lifecycle metadata validator checks the optional block through
its same parser while retaining old captures that lack it.

## Conservative content presence

jce_fs_probe_read mirrors the existing active reader/policy before host fallback.
The same active tuple holds the builtin metadata callback; every reader setter
invalidates it. Custom/no-probe is UNKNOWN; a NULL reader retains host-only
semantics, even with an isolated fs handle. Builtin metadata requires its
existing mutex and distinguishes regular files, canonical NOT_FOUND, and
unknown errors/nonregular sources. Under that lock, observe the actual PhysFS
search path without allocation and clear stale TLS error before stat; fresh OK
permits absence only when that search path is confirmed empty. Fresh query
errors remain UNKNOWN, including invalid paths and allocation failure. Unknown
priority is never hidden by a fallback. The private platform presence header supplies exact native error
classification; Core adds no raw platform API. A successful query is not read
success, content validity or a durable snapshot. Probes never read/decompress
and never turn existing read FAILED events into success.

## Current default Async statistics

The private `jce_async_stats_internal.h` snapshot belongs to the existing
process-default Async owner. It keeps that owner's guard through the current
executor's mutex-protected stats copy, performs no callbacks or creation, and
returns false with zeroed output when absent or closing. Default shutdown sets
closing under the same guard and drops it before waiting on executor work;
publication/destroy/recreation remain under that owner. Never cache a borrowed
boot pointer for periodic observation: public owner-thread shutdown ends its
lifetime, and recreation may reuse its address. Current snapshots are not
continuous instance accounting or complete task lifetimes.

The private jce_half_internal.h owns the existing round-to-nearest-even binary16 encoder, moved unchanged from net_quant. The public network wrapper delegates to it; water mip upload uses it without depending on networking. It adds no public ABI or third-party dependency.
