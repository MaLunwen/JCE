# engine/src/application — Application / Bootstrap (L6)

The Engine owns the renderer tail around the public end-frame call. The
private trace-scope helper records that measured interval and the wrapper
return. The existing Engine device-admission gate follows independently.

The graphics-config owner preserves its no-graphics strict no-op contract.
After applying a valid user graphics descriptor, it reads the effective RP
before forwarding MSAA so numeric QA forces retain precedence. Stock default
main loads legacy quality before canonical RP/user policy and before creating
SceneRenderer (whose TAA default consumes RP). Its final startup sync also
covers absent graphics sections; the renderer deduplicates an unchanged request.
Other legacy quality services apply after their owners exist. No additional
renderer policy, public ABI, scene-MSAA or GPU-performance claim is introduced.

The desktop JNI host is `jce_jni_bridge.c`, compiled by `jce_application` beside
`jce_main_sdl.c`. Only Application receives `JCE_BUILD_JNI` and JNI include paths;
Platform has no JNI-to-Application include exemption. The host uses public owned
create/try-destroy and its same-layer private terminal ticket. Failed deferred
construction retains a Java-reachable bridge; closing never forwards gameplay,
and refused destroy throws before freeing either owner. Normal release frees the
bridge before acknowledging its matching terminal ticket. Java signatures and
Android's separate entry path are unchanged. Shared-library consumers must retain
the Application archive's otherwise unreferenced JVM exports; Caged Kingdom's
macOS link metadata force-loads that archive. Source placement and CPU adapter
controls do not certify physical JVM/device or macOS linking behavior.

## Optional finite terminal acknowledgement

`jce_engine_terminal.c` observes the existing single owner on its owner thread.
Only an admitted allocated owner starts a ticket, with a frozen valid 32-character
lowercase hexadecimal `JCE_TERMINAL_ACK_TOKEN`. Missing/invalid tokens produce no
receipt. The final successful cleanup scope carries `engine.terminal_session`;
capture publication and logger API results are retained before their owners end.
The snapshot becomes ready only after SDL/storage/Engine free and quit-session
closure. SDL/JNI consume the matching ticket after create/destroy returns with an
empty owner; JNI first releases its bridge. SDL releases its allocated callback
context in the single final AppQuit before acknowledging, including failed Init.
Immediate failed construction that
actually owned resources is distinct from rejection before owner allocation.
An empty no-op, deferred owner, duplicate callback or stale ticket cannot certify
completion. No public ABI, per-frame producer, thread or console is introduced.

SDL's appstate owns its Engine slot, sticky terminal result, closing state and
ticket; no session storage is global. Main-thread admission precedes any context
borrow because SDL may deliver foreign-thread events during Iterate or Quit.
Those unsupported Engine events retain FAILURE semantics without mutating the
context. Finalizing rejects a nested release while retaining a late failure.
SDL calls Quit once and drops appstate afterward: successful Quit releases this
process-heap block before ACK; forced refusal leaves the block and Engine live
until process termination, logs unresolved ownership and emits no ACK. It does
not promise a post-Quit retry or treat process termination as completed cleanup.
Core allocation counters persist independently of Engine/logger/SDL shutdown;
the host allocation remains genuinely live until its paired free, without resets.

One bounded `JCE_TERMINAL_ACK ` JSON line goes directly to inherited stderr,
after the logger lifetime. The fixed `fprintf(stderr, "%s", line)` call must
return the complete formatted length, and the following `fflush` must succeed.
The emitted latch is set before either IO call: a negative print result may
follow an accepted prefix, which is never retried. Host
observation failure does not revoke completed cleanup; consumers must reject a
missing, duplicate or malformed requested receipt. `trace_status=unavailable`
means no continuous-capture result was observed, not that capture was unrequested.
The schema reports the real final log flush/close API outcome, not durable IO,
and does not certify process exit, unowned resources or GPU fence/Present work.

> Engine boot, subsystem ownership, main entry. Top of the engine; below consumers.

## Identity

- **Layer**: L6 (Application). C99.
- **Public umbrella**: `<jce/api_app.h>` → `<jce/application/*.h>`
- **Deps**: every layer below (PUBLIC link in CMake), notably `jce_core`, `jce_platform`, renderer, scene, middleware.

## Explicit Engine cleanup ownership

`jce_engine_create_owned` publishes an allocated owner before trace registration
or sampler startup. CONTINUE means READY; FAILURE can retain a DRAINING owner.
`jce_engine_try_destroy` keeps that caller slot until complete release. Null
slots reject, empty slots succeed, and host-thread/active identity checks happen
before dereferencing an engine address. The process admits one Engine owner;
CONSTRUCTING and DRAINING sessions prevent replacement and pre-boot descriptor
or configuration changes. The independent constructor borrow remains active
even after a close request changes state to DRAINING; repeated requests cannot
free the enclosing constructor's storage. An init callback requesting close
cannot resurrect READY by returning true. An entered init, including one returning false, owns
one exit callback; failures before init do not call application exit.

The legacy create symbol retains a failed deferred owner in one explicit private
pending slot. `jce_engine_destroy(NULL)` or the next legacy create retries it.
That create invocation drains only and returns NULL even on successful cleanup,
allowing the caller to reapply previously refused pre-boot settings before a
subsequent create; it never implicitly restarts the old descriptor. This compatibility slot is not
a second registry or lifecycle. All cleanup uses the same try-destroy body and
sampler, KPI, async, application, resource and final host release order. FPS
override and descriptor ownership survive refusal. No forced free or unbounded
retry loop is introduced; existing native join semantics remain authoritative.

Each admitted cleanup attempt has `engine.cleanup_attempt` and scoped
`cleanup.reason`/`cleanup.completed`: 0 completes, 1 defers for construction or
active callback, 2 rejects recursion/finalization, 3 retains sampler, 4 retains
KPI capture, 5 retains async execution, 6 retains audio-master callbacks.
Refused scopes end FAILED, never SUCCEEDED. Every retry remains traced; the
human ERROR for barriers 3/4/5/6 is emitted only
on the first refusal or a change of barrier, using one per-owner reason byte.
Callback/reentrant refusals remain quiet. There is no timer, worker or log queue.
Completion closes after all retryable service barriers and before the final
trace export; host slot clearing follows trace/log/SDL/PlayerLoop teardown.
It is CPU ownership evidence, not GPU completion or portable shutdown latency.

Engine default-executor creation admission uses the private Core session latch.
Open only after the constructor validates its input and publishes an allocated
owner; close immediately before that owner enters DRAINING, including borrowed
constructor/frame/event refusals. The latch survives sampler/KPI/async refusal,
all exit/subsystem callbacks and the final inter-session gap. A failed early
constructor or legacy drain-only retry cannot open it. Existing retained default
executors remain queryable for their original owner-thread cleanup contract;
after shutdown, callback lookup cannot lazily start a replacement worker pool.
Only the next admitted Engine constructor reopens creation. Public standalone
default shutdown/recreate and independent executors keep their original policy.
The shared frame pool's private session latch opens/closes at the same Engine
owner transitions. A shared-pool join cannot be undone by a later application
or subsystem teardown callback. Its own transient closing guard also prevents
in-flight frame work from creating a second pool while the first is joining.
`engine.shared_pool_shutdown` records the existing shared-pool join call with
the same service-scope authority; it remains before terminal diagnostic joins.

The final diagnostic barrier is owned by `jce_engine_diagnostics_retirement.h`:
join the continuous capture worker without releasing its stream/cursor, then
drain/join the logger under `engine.log_quiesce`. The logger keeps its file and
observer locks and emits subsequent tail diagnostics synchronously. Only then
close the successful cleanup scope, unregister main, and perform capture's final
drain/footer/publication. Existing log shutdown releases the retained storage
before SDL teardown. The logger's raw SDL worker is not registered in trace;
the completed quiesce scope, not `active_threads == 0`, observes its native join.
No new producer, collector or per-frame operation is introduced. The trace ends
before final log-file close/SDL/PlayerLoop release and does not certify those
later operations or process/GPU completion. The actual logger/capture plus
shared-call-order native fixture is `test_jce_diagnostics_retirement`.

Closing prevents new event/frame/resize callbacks. The existing CPU-tick owner
checks `engine_owner_ready` between callback phases and fixed steps; a callback
already running retains its borrow. The outer render owner still balances an
opened renderer frame before release, while later business phases are skipped.
Exact-body fixtures must provide a typed readiness port; they must not remove
these checks merely to keep a stripped fixture compiling.

The private host OPEN-failure detail context is embedded in this same sole Engine
owner, with two atomic counters and no extra heap allocation. Read
`JCE_FS_OPEN_FAILURE_DETAIL` once after allocated-owner publication; only exact
`1` lends that context to the existing host-read service. OFF binds nothing.
Invalid input, duplicate create and allocation/launch refusal before owner
publication cannot reset a live policy. Deferred cleanup retains the borrow.
Active same/foreign bind and foreign/repeated end refuse without reset/detach.
After `engine_quiesce_diagnostics` joins producers, including the log backend,
matching end detaches before summary on the retained synchronous logger path.
The POD remains valid until Engine free. VFS routing changes do not define this
Engine lifetime. This is the existing quiescent synchronous-borrow contract,
not general thread/refcount tracking, a registry, public API or default warning.

## Explicit launch presentation policy

Engine bootstrap reads `JCE_LAUNCH_VSYNC` (exact 0/1) and
`JCE_LAUNCH_TARGET_FPS` (decimal 0..INT_MAX) once, before trace registration or
GPU creation. Missing variables do not override; present empty, signed, spaced,
fractional, trailing or overflowing values reject startup. These typed settings
do not use the unrelated truthy flag convention. A local parse commits only on
complete validity; no environment strings are retained. User config and Bundle
bytes are not rewritten to persist an override.

The same private launch-policy header owns the complete positive-seconds scalar
parser for `JCE_MAX_SECONDS`. It preserves strtod spelling, the positive one-day
bound and invalid-as-disabled behavior; Engine still reads the environment once
at its existing common budget initialization. No clock, iteration or shutdown
ownership moves with this pure parser.

The existing frame-cap owner keeps the opt-in session identity, latest requested
FPS and effective FPS. Explicit zero stays uncapped despite later project,
Pause/background or Editor requests. Invalid/foreign session changes refuse;
actual final teardown restores the latest request and clears the deadline.
Deferred teardown retains the session. No override preserves existing pre-boot,
negative-to-zero and ordinary zero setter semantics. This changes rate selection,
never focus, simulation, drawable or input admission.

Renderer creation receives a resolved config copy before backend initialization.
The already sanctioned renderer lifecycle bridge binds only opaque renderer
policy state before app.init; Renderer owns later Vsync resolution. Explicit
binding rejoins at renderer_ready for headless, normal and software fallback.
Fallback binds before its existing early return, without adding service or app
initialization to that path; failed renderer creation never binds. The headless
join still skips the same windowed subsystems.
Explicit
requests publish direct INFO and balanced scoped counters at bind, post-init,
before-destroy and changed requested/effective values. Repeated identical setter
requests publish nothing. Observations read actual owner getters, not driver
acknowledgement, presentation cadence or GPU completion. Noop/headless/fallback
presentation is explicitly unavailable. New SDK/product acceptance is required;
older binaries cannot certify this policy by carrying new environment keys.

## Benchmark clock ownership

The stock SDK entry template exposes a borrowed const scene-renderer accessor
for public profiling and intermediate-target inspection. It is NULL outside
the initialized application lifecycle. Consumers may observe through public
APIs, never destroy the renderer or depend on the template's private globals.

The engine rotates the completed perf-phase snapshot at each observed frame boundary.
Normal iteration entry captures the previous render; terminal full completion
rotates and captures the final current render before writer finalization.
Initialization is not a rendered sample: N completed render iterations produce
N contiguous raw ordinals unless an explicit capture limit declares a prefix.
Programmatic quit, should_quit, SDL quit and explicit host destruction retain a
pending completed render; headless, paused and incomplete iterations add no
render row. CPU phases use the same boundary and unchanged eligibility rules.
Wall intervals retain counter precision independently of float simulation dt.
Writer finish and collector join do not become a fabricated final frame.
Consumers read it; a second rotation discards the measured phases.
JCE_KPI_FRAME_LOG retains the long-format phase CSV and the unclamped wall
interval. Renderer CPU/GPU rows are latest asynchronous stats, not proven
frame-aligned timings. JCE_MAX_SECONDS bounds unattended runs by elapsed
wall time since service initialization, independently of fixed simulation dt;
JCE_MAX_FRAMES can also end the run, and the log identifies the winning limit.
Bootstrap reads its named environment value once before the shared positive-u32
parser, as it does for the asset/frame capture and durability limits. The parser
consumes that raw value; null/empty, SDL_atoi and positive/fallback behavior stay
with the existing owner. The auto-stop counter counts completed iterations,
including headless work, and never certifies presentation or GPU completion.
The existing private auto-stop owner admits elapsed time after the minimized
yield and capture poll, without incrementing its frame counter or satisfying
a frame limit. Normal completed and headless iterations retain their existing
counter behavior. The paused exit releases an incomplete frame; it never runs
app update/draw or manufactures completed work. Exact-body CPU controls and
the existing public headless lifecycle remain distinct from an actual OS
minimize/restore and presentation receipt.

KPI export has its own main-thread `kpi_export` phase. The private
`jce_engine_kpi_capture.c` owns optional capture lifecycle, interval eligibility,
frame ordinals and observed renderer stats. `jce_frame_kpi.c` owns the one
complete-record encoder shared by queued capture and retained synchronous
export. Capture bounds reusable encoding scratch independently of the writer's
byte queue and record descriptors; these limits do not certify process memory.
Long names remain complete. Invalid records, exhausted budgets and IO errors
fail capture explicitly rather than dropping, truncating or retrying a prefix.
The encoder formats the uint64 frame-index/comma prefix once per complete
record into 22 bounded stack bytes and reuses it for all rows and growth
retries. The existing `%.4f` formatter, row order, context and complete-record
failure semantics remain authoritative; no extra queue, cache or allocation is
introduced. This removes repeated integer conversion, not backend waits or
project diagnostics, and is not a frame-throughput guarantee.

`JCE_KPI_BACKEND_TIMINGS=1` opts a requested capture session into three
`frame_backend_latched_*` rows from its already acquired `JceGpuStats` snapshot.
Missing/empty/"0" preserves the exact existing CSV bytes; other nonempty values
fail before stream IO, preserving any existing destination. Selection is latched
until the capture owner is destroyed, then resolved anew for a reused session.
No sampler, renderer query, task or public ABI is added. `cpu_submit` is render
thread command issue, `wait_submit` is render-thread waiting for submit and
`wait_render` is API-thread waiting for the backend. They are asynchronously
latched observations, never additive CPU phases or exact current-frame joins.
Invalid backend timings, including `frame_cpu` and `frame_gpu`, are omitted
independently even when the snapshot is valid. Finite zero is retained but
does not establish timer availability. Wall, context, phase and complete-record
scratch/queue/IO failure rules still apply.

`jce_frame_kpi_writer.c` owns copied records and the physical stream after
successful ownership transfer. It borrows the existing executor and runs one
finite LOW-priority task at a time. Threaded mode moves physical IO off the
owner thread; cooperative mode may block its pump and reports that capability.
Worker tracing uses the existing thread-aware async owner and never writes the
main-thread phase table. `kpi_encode` and `kpi_enqueue` are CPU caller actors,
not physical write or GPU-completion timings.

Private writer config keeps immediate dispatch when either `min_batch_bytes` or
`max_delay_ms` is zero. Capture explicitly requests 16 KiB or 8 ms of oldest
queued-record age, whichever is observed first by existing owner submit/poll.
This is a conservative syscall/task reduction budget, not a proven optimum or
storage deadline. Each existing record descriptor retains its admission clock;
later submissions never reset the oldest age. Byte/record ring capacities and
64 KiB/64-record finite drain quotas are unchanged. Descriptor saturation and
accepted records reaching the next durability boundary also dispatch promptly;
finish forces every partial tail without waiting for its age. Existing complete
record batching, single borrowed executor/task, exact IO/accounting, strict FULL,
cancellation, first-error retention and final callback/flush/close remain owners.
No extra queue, thread, periodic flush or file buffering layer is added. A blocked
physical write can still exhaust a finite queue; this does not diagnose or fix a
particular historical storage/OS scheduling stall.

Default durability flushes occur at initialization/shutdown. The opt-in
`JCE_KPI_DURABLE_FLUSH_INTERVAL` enables controlled recurring flushes. Finish
must observe real completion callbacks and exact accepted/written accounting,
flush and close before a terminal success or borrowed-executor cancellation.
Owner reentry defers mutation and cleanup without declaring data failure;
retain the owner for retry. Destroy success means safe cleanup, not successful
capture. Blocking storage has no universal finite shutdown guarantee. An
interrupted process may lose its buffered tail; require clean, complete raw
sequences. Use `test_jce_frame_kpi`, `test_jce_frame_kpi_writer`,
`test_jce_engine_kpi_capture` and `test_jce_engine_kpi_lifecycle` for the exact
owner controls; these do not certify GPU pixels or hardware latency.

The private writer retains one first-error snapshot under its existing queue
lock: actual queued bytes/records, published successful task submissions, real
work entries, completion callbacks and an available atomic task state. A fast
worker can enter before its returned handle is published; an unavailable task
state is not a claim that no work exists. Queue occupancy is never inferred
from accepted-minus-written accounting. Each physical WriteIO, including the
header, uses two existing monotonic-ns clock reads and copied API outcome fields;
no trace stream, worker, sampler or public API is added. `active` means its
return observation has not been published, not an OS-thread or storage probe.
The frozen snapshot remains immutable; a separate fixed slot retains the same
pending write serial's subsequently published return, including across wrapped
second writes. `return_published_later` describes publication after the snapshot:
the API may already have returned while waiting for the queue mutex. Actual
return versus failure ordering comes from `return_ns` and the failure `ns`;
equal clock readings establish no strict order.
The capture failure footer preserves its original accounting line and emits
two bounded diagnostic lines once, within the logger's existing 512-byte cap.
These are inclusive API-bracket wall observations, not disk-only latency,
per-frame CPU/GPU alignment or a throughput guarantee. Queue/drain budgets,
LOW priority, strict no-drop/no-block admission and default selectors are unchanged.

`JCE_FRAME_LOG_CONTEXT=1` explicitly extends FrameLog to the fixed four-column
`frame_index,phase,ms,context_json` schema. Missing/empty/"0" keeps the exact legacy
three-column bytes; only explicit "1"/"2"/"3" select context, and other nonempty selectors reject a requested capture with
a diagnostic before opening its stream. Only `frame_wall` carries a CSV-quoted
version-1 JSON object; other rows keep an empty fourth field. Dimensions and
flags are typed context, never millisecond phases, and the shared complete
record encoder/writer retains its original scratch, byte and record budgets.

The optional begin observation runs after CPU update/PRE_RENDER immediately
before renderer begin; end runs after successful renderer end/admission. It
separates cached point/drawable dimensions from live native pixel queries and
preserves unavailable flags/native sizes/backend IDs as null. Public JCE flags
remain raw values. Only a full completed render seals this fixed-size packet;
the next interval, terminal full completion and pending quit/destroy exports
consume that same sealed packet. Successful enqueue consumes it once. Default,
disabled, failed and exhausted captures perform no added window/backend query.
These are CPU-side observations, not GPU completion or presentation evidence.

Explicit `JCE_FRAME_LOG_CONTEXT=2` extends the same window object with
`viewport_reported` and at most four caller-assigned viewport observations.
The additive public sized host seam admits only the current ready Engine's
main-thread app.draw callback. Wrong-thread refusal precedes owner access.
Copy the 64-byte uint32-only known prefix immediately; 4-aligned strides are
64..4096 bytes and may contain ignored future fields. A short size is rejected
before reading the remaining prefix. Unique nonzero source IDs, typed outcomes,
known flags and availability/value consistency are checked transactionally.
A rejected or duplicate report never changes prior accepted data. NULL/count0
is explicit empty; no report is unknown. Each admitted render begins empty.
Requested/allocation/display extents, prepare success, CPU outcome, Image calls,
queued commands and main/recording CPU submits remain independent facts.

Explicit `JCE_FRAME_LOG_CONTEXT=3` retains that exact v2 window/viewport schema
and adds `clock` with domain `jce-performance-counter` and canonical uint64
decimal strings `frequency_hz`, `begin` and `end`. The existing completed export
borrows Engine frequency and interval boundaries, including pending/terminal
exports. No new clock query, allocation, queue or public ABI is introduced.
Zero counters and zero duration are valid; zero frequency, reversed boundaries,
missing clock or duration disagreement with frame_wall fail requested capture.
Duration validation reuses the existing clock arithmetic with 0.0000501 ms plus
1e-12 relative tolerance for four-decimal CSV rounding. Each begin clears the
clock with the context; v1/v2 bytes and native-query counts stay unchanged. These
performance ticks carry no Unix/GPU/Present epoch or external calibration.

The existing begin/end/seal/export owner retains the v2/v3 packet without later
resampling; terminal/pending intervals use the same path. V1 encoder bytes and
default three-column output remain unchanged. The maximum four-viewport uint32
window suffix is 1806 CSV bytes (plus NUL), below the fixed 4096-byte stack
scratch; actual encoder controls enforce the bound. No queue/drain budgets,
worker, clock, GPU query or writer was added. Strict shared readers validate
v1 and v2 independently and reject mixed schemas even in warmup/unselected tails.
These are optional consumer-reported CPU facts, not a trusted GPU fence, exact
Present mapping, or proof of rendered quality or performance improvement.

The existing drawable owner also emits `engine.drawable_transition` through
the same bounded trace ring on the first published observation, a new capture
generation or a changed decision-input snapshot. Its private HostLifecycle
cache is updated only after an admitted trace scope; it never controls rendering.
Headless bypass, window presence, minimize gate, lifecycle pause, native-size
query attempt/validity, raw positivity gate and final allowed bool are separate.
Native dimensions are emitted only for a successful query, preserving a valid
zero independently of unavailable output. Public window flags are queried only
when publishing and named transition_*; valid zero flags remain zero, failed
queries omit dependent values. These samples do not observe continuous focus,
OS occlusion, presentation or GPU completion. Disabled/suppressed tracing and
unchanged observations add no flag query, clock, scope or periodic log.

Physics timing separates external-edit detection, fixed simulation work and
render-pose synchronization. `rt_capture_fixed_pose` lives with physics helpers
in `jce_rt_physics.c`; editor and Player use the same runtime and clock.

The single gameplay-script creation path initializes the entire ScriptEntry
before publishing its entity/ref. Fresh profiler rows have zero calls and cost,
and measured=false, independently of allocator contents or profiling state.
The public-runtime script-cost regression checks this before any update;
Debug allocator contents previously exposed inherited garbage measurements.

## Bootstrap window defaults

`jce_engine_window_policy.h` seeds valid application dimensions and the actual
maximized bool before the existing INI loader. Explicit user keys win; absent
keys retain their defaults. Zero or unrepresentable descriptor dimensions keep
engine defaults. The descriptor has no fullscreen/resizable members: config
owns those false/true defaults and their explicit overrides. Size-only INI
keys do not imply normal-window mode; missing maximized retains the app default.

The existing core config owner parses and persists `window.maximized` without
changing a public structure. Fullscreen suppresses maximization for creation
while preserving windowed restore mode and dimensions. Hidden maximized sizing
uses only final resolved windowed mode and local creation geometry; never write
actual drawable or simulated display bounds into persisted restore dimensions.
The existing backend-memory save consumes that resolved config. Controls live
in `tests/os/core/test_jce_config.c`; CPU policy tests do not establish OS focus,
actual drawable size, physical LOW-device readiness or smooth resizing.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_engine.h` | `jce_engine.c` | `JceEngine` lifecycle: create, destroy, frame step. Owns window/renderer/audio/input/PAK/app. `jce_engine_iterate` walks the 8 PlayerLoop phases via `jce_player_loop_run_phase` (P3-B.1) as an AUXILIARY hook surface — the real renderer / subsystem / input / scene work remains INLINE in iterate (and physics in `jce_runtime_step`) to preserve resize-watcher and modal-loop guards; this is *not* full Unity PlayerLoop parity, the phases run alongside the inline work rather than owning it. **P3-B.2 / P1-fixed-clock-unify**: `JCE_PHASE_FIXED_UPDATE` is accumulator-driven via `jce_fixed_clock_default()` (0..N steps per frame, default **60 Hz**, configurable through `jce_engine_set_fixed_hz`). That global clock is the single source of truth for the fixed cadence — the per-runtime physics clock adopts its `fixed_dt` (see runtime AGENTS rule 3), so `set_fixed_hz` now governs physics too instead of silently desyncing from the old 50 Hz default. **P3-B.3**: registers an internal lifecycle listener at boot that bridges `JCE_LIFECYCLE_LOW_MEMORY` → `jce_streaming_signal_low_memory_all()` (pairs with P3-A.2 mip-streaming hook); `jce_engine_event` translates SDL platform-lifecycle events (focus/pause/resume/low-memory/will-quit/device-reset) into `jce_lifecycle_emit` calls before delegating to the app, and `jce_engine_destroy` calls `jce_lifecycle_shutdown` after PlayerLoop teardown. |
| `jce_lifecycle.h` | `jce_lifecycle.c` | **P3-B.3** Unity-parity application lifecycle event registry (`FOCUS_GAINED/LOST`, `PAUSE/RESUME`, `LOW_MEMORY`, `WILL_QUIT`, `DEVICE_LOST/RESET`). Priority-sorted listener table (same pattern as `jce_player_loop.c`); emit is main-thread only — events originate from the SDL pump in `jce_engine_event`. Cached `is_focused` / `is_paused` state is updated before listeners fire so getters reflect the new state inside callback code. |
| `jce_main.h` | `jce_main_sdl.c` | SDL3 entry-point shim → forwards to `jce_engine_*` callbacks. Single place where `SDL_AppInit/Event/Iterate/Quit` lives. |
| `jce_app_interface.h` | (in `jce_engine.c`) | `JceAppDesc` callback table (`on_init/update/render/event/shutdown`). Consumers implement this. |
| `jce_subsystem.h` | `jce_subsystem.c` | Subsystem dependency ordering / shutdown reverse-order |
| (internal) `jce_frame_kpi.h` | `jce_frame_kpi.c` | Shared complete-record CSV encoding and reusable scratch; retained synchronous export uses the same encoder. |
| (internal) `jce_frame_kpi_writer.h` | `jce_frame_kpi_writer.c` | Bounded copied-record queue, physical stream IO and actual async completion/close lifetime; borrows the existing executor. |
| (internal) `jce_engine_kpi_capture.h` | `jce_engine_kpi_capture.c` | Per-engine optional capture orchestration, ordinals, stats, encoding budget and terminal-result qualification. |
| (internal) `jce_engine_window_policy.h` | (header-only) | Application defaults before user INI; effective windowed maximization without mutating saved restore geometry. |
| `jce_args.h` | `jce_args.c` | argv snapshot + `--dev <assets-dir>` parser (also `JCE_DEV_ASSETS` env). Stashed first thing in `jce_engine_create`; consumers query via `jce_args_get_dev_assets`. |
| `jce_camera_controller.h` | `jce_camera_controller.c` | First/third-person camera input handler (utility for consumers) |
| `jce_screenshot.h` | `jce_screenshot.c` | PNG file capture via the renderer callback and serial SDL_image writer; memory capture remains reserved |
| `jce_runtime.h` | `jce_runtime.c` | Shared standalone/editor runtime state. Pointer and touch samples are frame-transient; hosts set the full input first, then replace touch state through `jce_runtime_set_touch_input()`. Touches are finite, stable-order, capped at five, and released automatically when not resubmitted on the next frame. |
| (internal) `jce_rt_script.h` | `jce_rt_script.c` | Public-runtime-to-Lua host bridge, including bounded VFS text reads and the one-based `jce.get_touch(index)` view over runtime touch state. |
| (public include) `jce_default_main.inc.h` | — | Thin default consumer shell. Samples platform pointer/touch input once per frame and submits the same normalized runtime input contract used by editor Play/Game View hosts. |
| (internal) `jce_engine_windowed_input.h` | (in `jce_engine.c`) | `jce_engine_create_windowed_input()` — the ONE place a windowed boot builds its raw input: `jce_input_create()` **plus** `jce_input_set_backend(jce_input_sdl_backend())`. It is a named function rather than two lines inside `jce_engine_create` so the install is reachable from a test with no window; that install went missing for a release while three comments claimed it existed, and no display-less runner could see it (976d38fa). Guard: `tests/application/test_jce_engine_input_backend.c`. Raw input, the action map and input record/replay are created **above** the fallback-renderer early return, so a safe-mode boot has a live `e->input` and a loaded `e->actions`. It does **not** have a populated `JceServices`: `e->svc`, `jce_subsystem_init_all` and the app's `init()` are all *below* that return, so on a safe-mode boot no app and no subsystem ever receives one. The only readers on that path are `jce_input_handle_event` / `jce_input_update` and the public `jce_engine_get_actions()`; `jce_actions_update` is below the fallback branch's return in `jce_engine_iterate`, so the action map is loaded and never evaluated there. Don't build a safe-mode consumer on `svc->actions`. |
| (internal) `jce_embedded_assets.h` | (generated) | Embedded fallback assets for JNI-desktop builds. Also declares the protected-asset key shares (`jce_embedded_pak_key_shares[64]` + `_present`) on ALL platforms; `jce_engine_create` XORs the two shares, immediately erases the temporary key, and installs it via `jce_archive_set_process_key` BEFORE the first PAK open. The shares only avoid a contiguous static key signature; they do not make a client-held key secret. |
| (internal) - | `jce_pak_key_default.c` | Zeroed default for the key-share externs. It is compiled into the normally linked `jce_pak_key_defaults` archive, outside the SDK's force-loaded core. An editor-generated `jce_generated/jce_pak_key.c` target object resolves the symbols first; otherwise the fallback member is extracted and `present == 0` skips key installation. |
| (internal) `jce_version.c` | — | Built from `jce_version.h.in` (CMake-configured) |

## Rules

Project saves preserve the original strict JSON object's unowned nodes,
including nested authoring extensions, and replace only fields represented by
JceProject. Root-key duplicates, malformed/non-object data, decoded/raw NUL,
failed reads and unknown host presence refuse the save. Relative manifest paths
are resolved to host-absolute paths before probing or reading, independently of
an active Bundle mount. Existing files use the shared JSON atomic writer;
absent manifests use exclusive host creation so a racing file cannot be replaced.
Neither path promises power-loss durability or concurrent-editor merge semantics.
String/array construction must complete before publication. The public struct
and schema have no extension-specific fields. Real public load/save controls
live in test_jce_project_script_modules; GPU/Editor reachability is separate.
Project load/new return either a complete owned object or NULL. Optional absent
values remain valid, but a failed string/list allocation cannot publish a
partial project or silently omit an entry. Array construction publishes its
pointer/count only after success; failed new-project defaults release every
already copied element. The existing single cleanup path also releases parsed
JSON on load refusal. test_jce_project_allocation compiles the complete owner
with counted project-allocation ports and real JSON/host IO, sweeping every
observed load/new allocation ordinal. This is scoped project-owned storage,
not a process-wide allocator/OS OOM, GPU or UI certificate.

1. **SDL application entry stays in `jce_main_sdl.c`.** Window/input platform details stay in OS adapters. Private bootstrap and diagnostic stream owners may consume SDL internally without adding it to public application interfaces.
2. **`JceEngine` is the singleton owner.** Subsystems are owned via `jce_subsystem.c` in deterministic order; shutdown is reverse.
3. **Consumer integration paths**:
   - **Standalone game**: link `JCE`, implement `JceAppDesc`, call `jce_engine_set_app_desc` before engine create. `main()` comes from `jce_main_sdl.c`.
   - **Editor-driven (Play mode)**: editor registers a `JceGameModule` (see runtime layer); engine is driven by the editor's frame loop, not SDL_App.
   - **JNI**: `engine/java/com/jce/JceRuntime.java` calls into `application/jce_jni_bridge.c` which forwards to `jce_engine_*`.
4. **No game logic** lives here. Camera controller is a *utility* consumers may opt into.
5. **Config path / PAK path overrides** are set via `jce_engine_set_config_path/_pak_path` before `jce_engine_create`. Don't hard-code paths.

## Don't

- Don't add a second `main()`. The only entries are SDL3 shim, JNI bridge, and editor.
- Don't depend on consumer-specific code (examples/caged_kingdom/editor).
- Don't bypass `jce_subsystem` for init/shutdown order.

## 2026-09-06 — `rt_rigidbody_kind()`：只有一个答案

「这是什么类型的刚体」此前有**两个**答案：两条 3D spawn 路径按 `is_kinematic` / `mass` 判，
而 `rt_softbody_mirror_static` 按 `JceRigidBodyComponent.body_type` 判。那个字段
**从不被 parse**（`parse_rigidbody` 不读 `bodyType` 键）、**从不被序列化**、**没有控件**
⇒ 它在有史以来每个组件里都是 0，而 0 == `JCE_BODY_STATIC` ⇒ 镜像判定恒为「不是动态」
⇒ **每一个动态 BoxCollider 都被复制进软体世界当不可移动的地面**。

三处现在都调 `rt_rigidbody_kind()`（`jce_rt_internal.h` 里声明）。
**新增第四个需要这个判断的地方时调它，不要再抄一份三行的 if 链。**

`body_type` 仍未被它读取，理由写在 `tools/lint/component_field_baseline.json` 里：
0 同时表示「作者说静态」和「没人说过」，honour 它会冻结树里每一个动态刚体；
要让它成为权威需要一个表示「未设置」的取值，那是**编码变更**不是接线。

同一提交里 `rt_voice_should_be_3d()` 的形状相同：一个导出的谓词，三个必须一致的调用点。

## 2026-09-06 补 — `body_handle_idx`：0 是一个合法句柄

`rt_spawn_body2d` 一直把创建出的 body 索引写回组件，注释写着它 "mirrors 3D contract"
——**而 3D 侧从没实现过那个契约**。现在两条 3D 路径都调 `rt_record_body_handle()`。

**关键不是「写回去」，是 0 同时表示两件事。** `JceBodyHandle.idx` 打包了 slot 与
generation，`jce_body_valid()` 只拒绝 `UINT32_MAX` ⇒ **slot 0 / gen 0 的 body 其
idx 就是 0**，与 memset 默认值逐位相同。所以 `rt_spawn_entity_body` 在做任何判断
**之前**先盖上 `JCE_BODY_INVALID`：此后 0 是一个 body，`UINT32_MAX` 是「spawn 看过了
但没造」，只有运行时从没访问过的组件仍然是 0——而那一个确实就是「编辑器里未激活」。

Inspector 那行的判据也从 `!= 0` 改成了 `jce_body_valid()`。
**再有第三个地方要问「这个组件有 body 吗」，用 `jce_body_valid()`，不要判 0。**

## 2026-09-20 — 每脚本开销：`measured` 不是 `last_ms != 0`

脚本子系统此前是**唯一零插桩的中间件**（`jce_script.c` / `jce_script_vm.c` /
`jce_rt_script.c` 里 `JCE_PROFILE_ZONE` 各 0 个），而 `on_update` 的循环
（`jce_runtime.c` 里）本身就在 `rt_tick_gameplay` **内部**——所以它和触发器重叠、
生成密度、GAS 复制、布娃娃混合、武器计时**共用 `gameplay` 这一个桶**。
「一个脚本吃 8 ms」与「一个触发器吃 8 ms」是同一个读数。

现在两层：一个 `script` perf phase 回答「脚本到底花不花时间」，
`jce_runtime_iterate_script_costs` 回答「是哪一个」。

**三条改这段代码前必须知道的：**

1. **计量门 `jce_perf_phase_enabled()` 每帧读一次，不是每脚本读一次。**
   每脚本读它，就是把这道门存在的目的（避免开销）花在门本身上。
2. **`cost_measured` 由「计量这个动作」置位，不由数值推断。**
   关掉计量时每行都是 0.0，而一个真的不花时间的脚本也是 0.0——
   两个状态不许共用一个值。这棵树已经为「稠密表里的 0 到底是快还是没跑」付过一次账。
3. **读与重置是两个调用。** 一次会重置的读**无法被调用两次**：面板按自己的刷新率轮询，
   第二个读者（测试、日志行、automation 工具）会静默拿到面板已经消费掉的那个窗口。
   `jce_perf_phase_report()` 正是这个历史，`peek_frame()` 是后来补上的；这里从那个结局起步。

**判据是排名不是量级。** 绝对毫秒是这台机器的属性（而它被记录为不稳定）；
「忙脚本比空脚本贵」是这套度量的属性。一个在总量上正确、但把开销记到错误行上的
profiler 会在排名上红，而一台慢机器不会。


## 2026-09-20 — 二十个可增长注册表里**有一个没有 free**，而它是我上一单加的

找地方释放曲线缓存时顺手扫了 `RT_GROW_FN` 的全表：**20 个里 19 个在
`jce_runtime_destroy` 里有 `jce_free`，缺的那个是 `rt->joints`**——
`a1a245ee` 随 joint motor 一起发的泄漏。游戏里看不见（一个 runtime），
**在建几十个 runtime 的测试二进制里不是看不见**。

**要点不是这个泄漏，是扫表这个动作。** 「我刚好绊到的那一个」不是一个站得住的边界；
`RT_GROW_FN` **是一张表**，而表正是检查器读得了的东西。20 里差 1
恰好是那种「逐个改动评审都过得去、只有从表上才看得见」的密度。

**缓存的释放放在填它的那个文件里**（`rt_curve_cache_clear` 在 `jce_rt_script.c`）：
一是 `jce_runtime.c` 因此只多 2 行而不是 6 行（它 5460 行，上限 3000，
`check_file_size.py` 会为此拦你，而且拦得对），
二是**释放的那个循环和创建的那个循环不会再对「NULL 条目是什么意思」产生分歧**——
这里 NULL 是**记住了的失败**，不是空槽。

`jce.curve_eval` 的 host 回调形状是 `fallible_out`：
**「曲线求值为 0」和「曲线不存在」不能是同一个读数**。这是 `get_param`
和 profiler 的 `measured` 之后**同一课的第三次**。

## 2026-09-21 — 项目**名字**是人看的，CMake 目标和 C 函数名是**标识符**

`jce_project_create_from_template()` 把同一个 `@JCE_PROJECT_NAME@`
同时塞进三个位置：`project(... C)`、`add_executable(...)`、
以及 `static JceAppDesc ..._get_desc`。而编辑器新建项目对话框
**唯一的校验是 `strlen(project_name) > 0`**（`jce_dialog_project.cpp:583`）。

叫一个项目 "My Game"，实测 cmake 3.27.9：

```
CMake Error: Could not find cmake module file: CMakeDetermineGameCompiler.cmake
No CMAKE_Game_COMPILER could be found.
```

**一个关于空格的缺陷，报出来的是「找不到 Game 语言的编译器」。**
连字符更安静也更糟：`my-game_get_desc` 是**合法的减法表达式**，
失败从 configure 期挪到编译期，变成一堆「未声明的标识符」。

**Automation 层的 `project.create` 多年前就算了一个消毒后的 target，
并在注释里写明这是从一次失败里学到的。两个脚手架，只有一个知道。**
⇒ 规则落在**引擎**里（`project_target_from_name()`），两条路径一起拿到。

三件必须成对的事：

1. **模板要两个 token。** `@JCE_PROJECT_TARGET@` 进标识符位置，
   `@JCE_PROJECT_NAME@` 留给人看的文本（`message(STATUS ...)`、`d.name`）。
   只改一个 = 发布一个仍然坏掉的一半。
2. **manifest 要和它刚写下的文件一致。** `jce_project_new()` 把
   `target_name` 默认成 `name`——对手写项目是对的，对生成项目是错的：
   CMakeLists 上写的是消毒后的名字，manifest 写原名，
   构建就会去找一个 CMake 从没产出过的可执行文件。
3. **替换逻辑当时有两份。** `jce_project_reset_main_c()` 里手抄了一份。
   只修 walk 那条路径，「重置 main.c」这个**修复入口**会把缺陷装回去。
   已折成 `subst_mem()`，两处共用。

判据：`tests/application/test_jce_project_template_name.c`，**双向断言**——
标识符被消毒，**并且**显示名保留空格。只断言前者的话，
一个把所有名字都压成 `JceGame` 的消毒器可以全绿。

### 补（2026-09-21 同日）：把规则放进引擎的那个提交，自己制造了一次漂移

`project_target_from_name()` 是为了「两个脚手架共用一条规则」而写的，
**而我写的时候没有去比对 Automation 层里已经存在几个月的那条**：

```
"My_Game"   ->  MyGame   (project.create)     My_Game  (引擎)
"2048"      ->  2048     (project.create)     _2048    (引擎)
```

Python 抹掉 `_` 且没有首位数字防护；引擎保留 `_` 并补前缀。
**修「缺一条规则」的那次改动，引入了「两条规则不一致」。**

**两边都不会产生坏掉的工程**——`project.create` 的模板用固定的 `app_get_desc`，
没有 C 标识符暴露面，而 `2048` 是合法的 CMake 目标名。
⇒ **没有门会红、没有构建会失败、两个答案各自都站得住**，
这正是这种漂移能活下来的原因：只有在有人**去比**的时候它才存在。

定案：**保留 `_`**（在 CMake 目标和 C 标识符里都合法，抹掉它只是无谓地改写作者写的字），
Python 向引擎对齐。

**判据只能是一份跨语言共用的语料**（两条规则不可能共享代码，所以共享**测试**）：
同样六个名字、同样的期望目标，同时出现在
`test_project_target_matches_the_engine_rule`（Python）和
`tests/application/test_jce_project_template_name.c`（引擎）。
每个用例钉一条子句——普通名（**阳性对照**）/空格/下划线/连字符/首位数字/纯标点。

**而 Python 那一半还断言「这些名字确实在 C 那个文件里」。**
一份由两个文件钉住的差分，只在**两个文件都还装着同样的用例**时才算钉住；
一个只是**声称**对面覆盖了同样范围的测试，什么都没钉。
它在第一次运行就红了（`missing: ['My_Game']`），因为我先写了 Python 那半。

⇒ **一条一致性断言只在它红的那一刻证明自己。** 下次应当**故意**：
先加断言、看它红、再去满足它——和变异对照是同一条纪律，只是对象从行为换成了一致性。

**刻意没有对齐的**：`exe` 字段。两边各自**内部自洽**
（各自的 manifest 写的就是各自 CMakeLists 产出的那个文件名），
所以那是命名风格而不是漂移，选哪个是产品决定。写下来是为了让下一个人知道它不是被漏掉的。

## 2026-09-21 — 两个断裂阈值必须**各自一个 `if`**

`rt_cfg_joint_breaks` 里 `break_force` 与 `break_torque` 分成两个独立的
guarded block，不是风格选择：

```c
if (ce->break_force  > 0.0f) { ... if (imp > ce->break_force * cs->dt) return true; }
if (ce->break_torque > 0.0f) { ... if (tq  > ce->break_torque)         return true; }
```

把它们合进一个 `if` 会让**未设置的 `break_force` 悄悄关掉 `break_torque`**。
场景解析器对两者的默认值都是 **1e30**（不是 0），所以嵌套版本在默认场景里
**仍然会进去**——于是这个回归**在默认配置下看不出来**。
它只在作者显式写了 `breakForce: 0`（「这根关节拧断，但拉不断」）时才暴露。

⇒ 所以 `test_jce_joint_break_torque.c` 的断裂用例把 `breakForce` **显式写成 0**。
这不是装饰，它就是那条断言。

**单位不靠记忆。** 本包只发 Bullet 头、没有求解器源码，
「求解器存之前有没有除以 timestep」猜错就是 60 倍偏差，
而症状与「`break_torque` 还是什么都没做」**无法区分**。
所以 case 1 在**已知**的施加力矩下直接读数并做 [0.25x, 4x] 夹逼——
两个候选答案相差 60 倍，这个夹逼分得开，且不假装有 Bullet 软 6DOF 求解器
没有的精度。

Script cursors are owned by the runtime host: `JceScriptHost.cursor_set` loads
raw/cooked image assets, caches eight handles, and releases them after scene
script teardown. Native modules must not create cursors through their own SDL
copy. Editor Game View reapplies the selected host cursor only while its
pointer sample is valid, leaving cursor arbitration with the editor host.

Allocator policy is resolved once per JceEngine lifecycle after SDL initialization
and before renderer/audio/user initialization. AUTO hardware detection is overridden
by the published machine class, then a nonempty JCE_LOW_MEM value. Both LOW and
FULL are applied: FULL restores the allocator purge delay after a previous LOW
lifecycle. Periodic trim state belongs to the engine instance; boot does not trim.

Authored-camera-only consumers resolve the live primary camera after script update and before each draw, including lens, mask and clear policy. A boot-only camera copy diverges from script picking after camera animation.

## Optional script services

jce_rt_script_services.c owns the append-only request_quit/stat callbacks.
Runtime quit is only a latch; stock standalone and editor consumers inspect it
after callbacks return, with editor Stop rather than process exit. Runtime owns
no platform backend: an explicit setter borrows the host's service pointer,
NULL remains unavailable and no LOCAL store is auto-created. Detach/destroy the
runtime before host service shutdown. Stats are signed32 at the script seam;
backend out-of-range values return the explicit fallback rather than truncating.
New value/fallback inputs use existing double marshalling and central finite,
integral, INT32-range admission; invalid fallback and a missing host return0.
A real runtime lacking an injected service returns the valid fallback.
ID admission prevents existing LOCAL fixed-buffer truncation aliases. Available
queries declared read/write/flush callbacks only; successful writes/flushes must
still be checked. Host assignments remain in rt_script_install_vm so the existing writer gate
observes them; implementations live in the private service TU. Generator order:
update exposure+host, unified gen_script_bindings.py --write (contracts first),
gen_script_c_abi.py --write, then check BOTH tools. C ABI consumes the canonical
contract, so checking only one tool can miss stale signatures.
Generated C ABI and every language share the exposure contract.
No LLM or new filesystem persistence implementation is part of this bridge.

Trusted host injection uses the public inline jce_runtime_stats_host.h, without
new exported symbols. The stock main and editor Play group their existing runtime
pointer with one owned service and configured directory. Only explicit absolute
JCE_RUNTIME_STATS_DIR opens existing LOCAL services; no config allocates no tables.
The shared path APIs reject relative/oversized/derived-path overflow before I/O.
Repeated same-host attachment reuses the service; directory changes require close.
Destroy/null runtime first, then close performs checked flush and existing shutdown.
Failures warn explicitly; capability does not promise successful durable storage.
The existing LOCAL backend's fixed tables are allocated only for opted-in hosts.

The stock template retains s_runtime as a legacy private macro alias to the
same grouped owner for existing historical consumer harnesses. It introduces
no second runtime pointer/global. New projects must not depend on template
private names; migration to public host integration is a separate task.

The stat-host helper prepares/checks ownership only; stock and editor callers
explicitly bind through the public runtime setter after successful configuration.
Editor also checks declared service capability at the host trust boundary. The
helper is reachable from api_app.h; no exempted or unconsumed public service seam.

Service ownership must be prepared BEFORE runtime_create_with_platform_services:
script on_start runs synchronously inside construction. The borrowed field is
seeded before VM/spawn, using the single existing constructor implementation;
legacy create wraps it with NULL service and no Desc layout change. A post-create
setter alone cannot make initial on_start see stats. Create failure leaves the
service host-owned and closes it after confirmed NULL runtime. Runtime destroy
never closes borrowed services. Native on_start witness requires real constructor
side effects before any step/setter, legacy unavailable and failed-create cleanup.

Runtime diagnostic environment reads use C99 getenv consistently: the Emscripten libc ENV and SDL private environment table can disagree. Web frame/KPI options must arm actual engine collection.

Frame KPI export remains enabled when JCE_PERF_LOG is absent; periodic console logging must not disable an explicitly opened frame CSV accumulator.

The public default host applies packed settings/project-settings.json when authoring host files are absent, using the existing bounded PAK config reader. SDK embedding copies authored project settings into that reserved resource path and removes stale generated settings when absent. Colour space, quality and time/audio parameters must survive single-file relocation.

Default-host Canvas UI consumes jce_input_primary_pointer for physical mouse or primary touch, retaining touch release coordinates. Gameplay mouse/touch streams remain independent; platform mirror filtering must not disable touch UI.

## Lifecycle dispatch mutation and teardown

Lifecycle emit freezes an entry identity ceiling and walks the existing sorted
registry by immutable (priority, identity), requiring no temporary allocation. Callback removal
is immediate; insertion waits for another emit. Nested emits own independent
snapshots and getters retain the most recent paired event. Shutdown cancels all
active snapshots through a private epoch and resets focus/pause even before
allocator initialization. Opaque IDs remain monotonic across shutdown; exhaustion
fails registration rather than aliasing stale handles. LOW_MEMORY dispatch
still reaches every surviving entry listener when allocation fails; new listener
table growth can fail explicitly without disturbing existing listeners.
`test_jce_lifecycle_device_lost` covers live mutation/nesting/shutdown/80listeners;
`test_jce_lifecycle_allocation` compiles the exact production registry with a
faultable allocator to exercise allocation-free delivery and table growth failure
without devices.

## Service trace scopes

`jce_runtime_trace_internal.h` reuses the same Core service scopes for actual
Runtime construction, destruction and tick, including nested physics, Scene
update, particle and gameplay CPU call intervals. Constructor refusal reasons
1/2/3 mean invalid description, allocation failure and viewport-options refusal;
each closes FAILED before returning NULL. Successful creation means a live
Runtime handle was returned, not that every optional subsystem is ready.
NULL step/destroy remain no-ops. Disabled capture adds no trace clock/id/detail
work; existing perf-phase timing remains independent. No simulation scheduling,
fixed clock, Play policy or public ABI changes. Parent ownership is restored
after the actual calls, and capture changes still require producer quiescence.

jce_engine_trace_scope.h aliases the core private service scope, which reuses
the public trace task authority for synchronous
window/renderer/subsystem/application construction and teardown. It restores
parent task ownership, adds no global registry or public ABI, and is idle when
capture is off. JCE_TRACE_EXPORT itself enables capture; exported resident
records carry loss/generation metadata and task outcomes. Window/renderer
creation spans measure attempted service work, not successful GPU completion.

Optional application update and draw callbacks dispatch independently through
jce_engine_callbacks.h. Draw-only applications are valid. app_simulation and
app_draw report separate CPU phases; app_update remains the historical combined
metric for compatibility. Neither metric substitutes for GPU execution time.
Actual optional callbacks additionally own engine.app_update/engine.app_draw
SERVICE scopes through the existing private trace helper, independent of
compile-time Tracy profiling. Disabled capture and absent callbacks emit no
service; callback order, arguments and existing metrics remain unchanged.
Auxiliary PlayerLoop phase scopes do not substitute for actual callback spans.

Continuous trace capture is opt-in through JCE_TRACE_STREAM and consumes the
existing core ring. The engine owns its private capture handle: start after
SDL/instance early exits, before application workers; stop after producer joins
and MAIN unregistration, before SDL shutdown. The consumer joins and drains its
own unregister/join tail before its footer. Loss or write failure remains an
explicit incomplete capture, not a complete lifecycle or GPU certificate.

JCE_TRACE_STREAM_MODE accepts threaded/cooperative; unsupported values reject
capture explicitly without aborting the application. Default mode is chosen by
the core consumer. All continuing iterate paths pump a cooperative capture;
NULL/threaded captures return immediately and never acquire a second reader.
Headless application update uses the same explicit CPU service scope as other
host lifecycle callbacks; compile-time Tracy zones are not substituted for it.

The manual Frame profiler zone must end on every iterate exit, including quit,
headless autostop, fallback, minimized/reentrant rendering and KPI write errors.
A balanced newly added service scope does not compensate for an unclosed outer
profiler zone. Full engine trace tests require zero active work at shutdown.

Startup trace and frame perf flags share the private environment policy in
jce_engine_trace_scope.h: empty and leading-zero boolean flags are disabled.
A nonempty explicit JCE_TRACE overrides perf/debug auto-enablement; nonempty
JCE_TRACE_EXPORT or JCE_TRACE_STREAM paths require tracing even with TRACE=0.
Perf telemetry respects PERF_LOG=0 independently of trace capture selection.

Relative script/data/cursor addresses resolve through the active mounted VFS
before loose host-path resolution. ISOLATED misses never fall through to host
files or the runtime PAK. Explicit absolute tooling paths stay host-owned.
The capped path uses core's private jce_filesystem_read_internal.h entry: PAK
metadata is checked before eager decompression, PhysFS size before allocation,
short reads fail, and the existing PAK file buffer transfers without a second
copy. Runtime/Lua controls use real archives, an actual oversized asset and
observe zero oversized decompression calls; loose OVERLAY fallback is retained.

## Screenshot completion

The application screenshot utility accepts PNG only; BMP/TGA/invalid enum
requests and BMP-extension PNG requests fail before graphics submission. The
renderer retains its direct extension-selected BMP path. File capture needs an
active windowed renderer; headless/fallback and destroyed contexts reject.

`jce_screenshot_poll` is the non-consuming latest-file result: -1 idle, 0 pending,
1 actual encoder/file API success, 2 failure. Pending includes pixel delivery and
CPU encoding/writing. A new accepted request replaces the previous terminal
result; hosts must read their result before another file consumer submits.
Shutdown fails an unfinished request and retains completed results. This is
not a durable-storage, GPU-idle or presentation guarantee. The stock schedule
uses this outcome rather than treating `!pending` as success, and F12 does not
replace a schedule request whose completion is still awaiting observation.

`test_jce_screenshot_completion` runs the exact production callback/writer with
GPU seams, real SDL encoders and actual successful/failed file IO. It also
checks the application adapter and schedule failure. No screenshot pixels or
GPU timing are certified by this fixture.

## Completed-interval diagnostics

`jce_engine_perf_hitch.c` is the application-private owner of per-engine perf
enablement, warm-up, the sole 2048 paired wall/comparison ring, 120-sample windows and bounded hitch
reports. It reuses the core phase snapshot without ticking/resetting it. The
outer iterate rejects nested entry before clocks/callbacks, using a separate
main-thread per-engine gate; the existing renderer/live-resize atomic lifetime
is unchanged. The ordinary boundary tick precedes current callbacks; terminal full completion
has one additional tick for its own completed interval, under the same entry
gate. The private terminal transition is idempotent and leaves no pending row
for event/destroy recapture. A completed
rendering iteration certifies the next interval only when its phase gate
remained armed at the observed boundaries; skipped or disabled intervals
invalidate phase pairing. An ever-available core snapshot is not freshness. A reporter-owned ordinal advances independently of auto-stop.
Sequential app sessions reset all diagnostic state; concurrent live engines
remain outside the existing process-singleton application contract.
The default SDL/JNI event pumps run outside iterate; their modal/resize frame
service remains available. Recursive iterate from a user callback inside an
unfinished iteration is rejected. CPU entry-gate tests do not certify actual
OS modal dragging, presentation or live-resize behavior.

The sole process-global frame cap remains in the private implementation include
`jce_engine_frame_cap.inc.h`, included once by the engine. Its public setter,
getter, pure deadline policy, bulk sleep and tail spin retain their behavior.
The wait records its actually selected FPS, planned wait and actual returned
wait using the existing counter reads. An admitted next interval receives only
the previous full frame's witness; a skip discards it, and current terminal
completion replaces it with the current witness (no wait means no deduction).
Identical FPS assignments do not re-anchor or reset diagnostic history.
Hitch comparison subtracts only a valid planned wait from raw wall, preserving
oversleep and work stalls. Invalid clocks/metadata give no deduction and never
reject an otherwise valid raw wall observation. The paired ring shares one
head/count; window averages, worst values, percentiles, KPI rows and trace
ordinals remain raw wall. Comparison is residual wall, not OS CPU execution
time. Captured WARNs include both values, comparison mean and the selected
FPS/planned/actual/clock witness. Zero comparison/baseline formatting never
divides by zero. Actual-owner typed clock/sleep controls and the missing-witness
wire mutant do not certify native scheduling, focus or GPU presentation.

Only the actual valid-clock wait branch publishes a balanced `cat=wait`
`engine.frame_pacing` span under the current SERVICE parent. Disabled tracing,
uncapped/first/overrun/deadline-passed frames and invalid clocks publish no WAIT.
Its duration reuses the existing owner counter samples; trace publication has
its own event clock, and neither alters the cap policy or sleep/spin reads.
`test_jce_engine_frame_cap` includes that exact owner with typed clock, sleep
and trace ports. The same-source `test_jce_engine_frame_cap_missing_wait_end`
control must compile and fail the named balancing assertion ordinarily.
These CPU witnesses are distinct from a native capture and scheduling result.

Periodic perf retains legacy `cpu`/`gpu` fields for parser compatibility and
labels them `cpu_scope=bgfx_frame_interval` and `gpu_scope=latched_backend`.
The backend CPU interval may include prior pacing; it is not process execution.
Memory values use binary MiB explicitly. Negative backend memory values and
failed/zero process-provider byte samples print N/A independently per field;
known GPU zero and positive process bytes rounded below one MiB remain 0.
The existing periodic provider read is unchanged. Backend total usage is not
the texture/RT sum; the log labels backend usage and mimalloc process sources.
Neither proves per-allocation ownership or a complete memory budget.
`perf-pacing` reports only the last completed engine interval's ordinal, target,
planned/actual wait and clock validity, not a window average or a paired GPU
frame. `app_update` remains the legacy update+draw aggregate. Do not subtract
cross-thread waits or add overlapping phases to manufacture CPU work.

Finite zero wall intervals advance the same history/window cadence as any
valid interval. Hitch payloads own their paired values and escaped names. Sampled all-zero,
unavailable, discontinuous and invalid snapshots are distinct observations;
none implies that all wall time occurred outside instrumented phases. Raw KPI
export uses the same interval eligibility: missing phases are omitted, never
filled with zero, and legitimate zero samples remain present. Backend stats
remain latest asynchronous observations without a claimed frame match.

The existing per-engine iterate gate also protects borrowed frame lifetime.
Event and resize callbacks preserve a private nested dispatch guard. A destroy
request in either interval latches the existing quit request and retains the
engine for the outer host; never free before SDL/JNI callers finish their call.
A refused shared-executor shutdown retains the remaining owners for retry after
the owner-thread pump returns. Keep the frame gate armed through terminal KPI
finalization, then release it on every exit.

The private sampler shutdown is an ownership barrier before KPI writer and
executor shutdown. Refusal requests quit and retains the engine/services for
host retry. Only confirmed collector join and resolved acquired suspends permit
teardown; native OS calls/report IO have no total local deadline. Real headless
engine controls inject native outcomes through the actual sampler owner, not a
copied application implementation.

Completed-boundary CPU controls use the exact private state owner and actual
encoder/writer; they cover initialization omission, contiguous N rows, final
long wall/CPU pairing, zero wall, disabled phases and no incomplete-row relabel.
They do not replace real SDK timed-horizon or window/presentation validation.

JCE_WINDOW_NOT_FOCUSABLE is an optional bootstrap window policy using the
shared environment bool rules. It is ignored in headless mode and independent
of JCE_WINDOW_HIDDEN. The default focus/visibility behavior is unchanged.
A duplicate nonactivating launch exits through the existing single-instance
path without activating its predecessor. It does not implicitly enable multiple
instances; QA may explicitly opt into the existing JCE_MULTI_INSTANCE policy.
Fatal/software-fallback dialogs remain a separate interactive error path.
Visible nonfocus controls need actual supported-backend/no-fallback checks and
separate focus observations; the policy does not certify them by itself.

`jce_engine_quit.c/.h` owns the main-thread process-global quit request and its
terminal result. Public `jce_engine_request_quit_with_result` accepts SUCCESS or
FAILURE; nonterminal/invalid requests are ignored with a bounded diagnostic.
Failure cannot be downgraded by a later ordinary quit. `jce_engine.c` resolves
terminal frame/event results through its existing KPI finalization and lifecycle
path, and clears the latch only after actual complete session teardown. Deferred
or recursive destruction retains it; a pre-create request is not erased at
create entry. A private latch unit test does not prove SDL exit or WILL_QUIT.

Explicit performance/capture observations use public jce_log_write(INFO) with
source location instead of the ordinary INFO macro. JCE_DIST still strips
ordinary INFO; no default logger policy is raised. JCE_PERF_LOG retains the
actual eligible 120-frame window cadence and optional phase/view availability.
An explicitly selected frame/time limit retains its actual quit witness. Only
an enabled writer's successful closed/equal-accounting finalization emits its
single completion footer; failures keep their existing error path. Runtime
level/sink filtering remains authoritative. The same real capture/encoder/writer
fixture compiled with JCE_DIST checks footer fields, once-only reporting and
runtime level suppression. Its old-INFO runtime mutant must compile and fail
those assertions; neither control certifies OS window, Play, pixels or GPU timing.

`jce_engine_kpi_clock.h` is a private ledger for explicit timed KPI capture. It
borrows the existing engine counter/frequency and unchanged service-init timer
origin; it does not run another clock, profiler or default reporter. The actual
completed-export owner records only successful index increases with their raw
start/end boundaries; normal, terminal and pending paths share this wire. Raw
initialization rows are never fabricated. The successful original timer check
supplies its actual counter. After terminal writer finalization, a single opt-in
`frame KPI clock v1` record preserves frequency, timer origin/check, first/last
render boundaries, summed render ticks/count, omitted ticks, validity and error
mask. Zero origins/intervals are valid. Frequency changes, reversed/overlapping
counters, ordinal mismatch, gaps and arithmetic overflow invalidate the ledger.
Skipped gaps retain an explicit diagnostic and cannot certify continuous render.
This metadata does not replace writer completion or change application results.
The event-result and direct-destroy paths share one private clock reporter.
Direct destroy saves the final accepted ordinal after pending export and before
capture destruction clears its state, then reports only after successful capture
destruction. The enabled/reported latch is checked and marked before logging,
so reentry, later teardown and disabled captures do not publish another witness.
Actual deadline checks, clock arithmetic and v1 error bits are unchanged; an
early direct close retains timer_checked=0 and invalid terminal status.

Explicit FrameLog also enables the fixed per-Engine disposition ledger in the
existing PerfHitch owner. At each admitted iterate's existing raw boundary it
closes the preceding released interval and retains the current interval ordinal
and writer ordinal observed at entry. The latter is not a completed CSV or GPU
frame join. Startup, pre-boundary device rejection and recursive entry never
fabricate intervals. Full, headless, drawable gate combinations, renderer guard,
fallback, closing, quit and capture/device failure have bounded disjoint buckets;
minimized, paused and nonpositive raw extent inputs remain independent bits.
Classification does not depend on trace being enabled or on the transition cache.
Failed extent queries retain the original gate decision; the existing drawable
transition continues to distinguish unknown dimensions from known zero.

Release stores the disposition without reading a second clock. Existing next
iterate or terminal counter samples supply its end. Each bucket keeps exact
count/ticks and first/last raw boundaries with interval/entry-row identities.
Frequency, ordering, ordinal/release and overflow errors refuse validity; no
unsigned wrap or partial admission is a complete receipt. After writer closure,
one bounded summary and occupied-bucket records use the common INFO logger,
including DIST. No per-frame log/trace packet, new registry, periodic flush or
default reporter is added. Terminal completion and teardown retries cannot count
the tail twice. This ledger explains CPU host interval disposition and does not
change clock v1 continuity/deadline checks, certify 240-second completion, native
focus/minimize causality, GPU completion, fences, presentation or throughput.
The terminal buckets contain exact aggregate counts and first/last witnesses,
not a reconstructable record of every intermediate non-full interval. Validation
errors preserve a certified prefix with explicit invalid status, never a complete
timeline. Existing drawable transition trace packets remain independently useful.

JCE_MAX_SECONDS includes app initialization after service initialization;
completed-render frame_wall covers a later window. Validate the original timed
budget and declared rendering length separately, using the same raw counter
witness and every CSV row. Join CSV %.4f millisecond precision with a per-row
half-unit rounding bound, not an arbitrary clock tolerance. Keep absent/invalid
witnesses unavailable; renderer frame_cpu is asynchronous and cannot supply a
missing startup offset. Actual-header nine CPU controls and compiled
accepted-row and zero-origin guard mutants do not certify host timing, Play or GPU output.

The wall wrapper and iterate dt calculation both consume the private
jce_engine_frame_clock_wall_ms arithmetic owner. Counter zero is valid, not an
initialization sentinel; frequency zero, reversed counters and null output are
rejected without overwriting output. Ordered uint64 subtraction occurs before
floating millisecond scaling, avoiding unsigned multiplication/wrap. The first
initialization interval still cannot become a rendered row: existing completed
render eligibility, phase rotation, capture and timer origins are unchanged.
The old-zero-guard runtime mutant reinstates only begin==0 rejection in this
actual shared arithmetic owner and must fail the unchanged zero assertions.


Editor authoring cadence is a consumer of the public target-FPS API; it does not
change the generic default 0 uncapped or add a second frame wait. At engine init,
explicit JCE_PERF_LOG or JCE_EDITOR_CADENCE_DIAG reports the actually selected
configuration path and resulting VSync intent once through direct common logger
routing, including DIST. CWD .config/jce.ini retains its existing precedence;
no diagnostic rewrites user configuration or certifies a present mode.

Explicit JceScriptHost.log messages go through the runtime jce_log_write owner
in DIST as well as authoring builds. Runtime level/filter/sink policy still
applies; ordinary internal LOG_INFO stays stripped. The shared callback never
guesses a caller language or treats the message as a printf format string.
Actual callback/old-macro runtime controls: tools/tests/test_script_runtime_log.py.


`jce_runtime_host_policy.inc.h` is a C99 implementation-only SDK include shared
by stock default_main and Editor Play. Its static helpers export no JCE_API or
new C ABI; the existing public runtimeStats layout is unchanged. The host owns
one lifecycle listener and borrowed runtime, applies temporary pause/background
FPS through the existing engine deadline, and pairs audio pause/resume once.
`run_in_background` is now consumed from the actual runtime boot manifest.
False prevents unfocused simulation, true permits it; native OS pause/minimized
retains the existing engine paused-render behavior. Resume deliberately discards
one observed inactive interval, never moving the benchmark timer origin or
manufacturing a render row. Headless hosts register no focus listener. Repeated
focus, nested lifecycle callbacks, listener allocation failure and close before
runtime destruction are controlled with the real lifecycle owner and typed
FPS/audio ports. Actual stock-entry adapter controls are separate from installed
SDK C99 build/run, native focus, audio output, PresentMon and GPU evidence.


Runtime pause/resume audio lives in `jce_rt_audio_pause.c` and shares the existing
`VoiceEntry` owner. It records only currently playing full-generation handles
paused during this host episode. Resume clears each record before the device
call and only targets the same binding handle; voices already user-paused are
not acquired. Stop, unload or pool recycling makes a saved handle stale through
the existing audio generation check, and a newly assigned binding starts with
no pause ownership. Array compaction moves the record with its voice; runtime
shutdown stops/unloads it without an extra resume. Real offline PCM controls
and an old blanket-resume runtime mutant distinguish this ownership contract
from native focus timing, output-device completion or audible playback.

The existing iterate callsites trace engine.async_completion_pump and engine.frame_pacing through the shared private core span owner. No scheduling, frame-cap policy, clock origin or wait owner is changed. These scopes report elapsed callback/wait intervals; returned pump work is not proof that all queued work completed, and frame-pacing wall duration is not OS CPU execution time. Trace-disabled paths keep the shared fast path. Validate actual frame-cap/trace/lifecycle controls and a current artifact capture; actual GPU completion and native OS focus remain separate.

Host viewport presentation is owned by `jce_rt_viewport.c`. The sized optional
creation extension installs its copied extent before the existing synchronous
script on_start; legacy constructors retain zero/unavailable presentation and
JceRuntimeDesc keeps its layout. Nonzero short options or invalid dimensions fail
before scene setup. Current extent is bounded, persistent, and independent of
pointer focus/ownership and frame-sample expiry. Invalid updates clear it and
warn once per runtime. Changed extent uses the existing optional script-message
seam for on_viewport_resize, without stepping time. The publication trace records
owner and exact float dimension bits; publication does not certify callback or
GPU success. ScriptHost appends a sized-compatible viewport_size reader shared
by all language backends. Actual Runtime/Lua controls cover first start, expiry,
resize/clear, invalid options and unknown tails; missing-initial and missing-notify
owner variants must fail ordinary assertions. Physical layout/input and DPI
acceptance are separate. Native hosts publish cached drawable dimensions; Editor
Game View publishes the same resolved Canvas/FBO extent as its pointer mapping.

Native default hosting converts raw window-point mouse coordinates through the
window owner cached point-to-drawable mapping before ScriptHost pointer_position
and Canvas input. Independent viewport remains drawable size across invalid
pointer/focus samples. The point surface comes from actual SDL dimensions, never
configured logical layout; normalized UI touch is scaled exactly once. Native
DPI/input acceptance requires real clicks and ray selection alongside the CPU
window-owner positive/negative controls.

The normal and permitted non-drawable paths share the complete
jce_engine_cpu_tick.inc.h owner: INITIALIZATION, EARLY_UPDATE, input replay/action
snapshot, fixed-clock hooks, subsystem update, UPDATE, app update and LATE_UPDATE.
INITIALIZATION retains its existing per-iteration contract; no startup latch is
introduced. PRE/POST_RENDER, asset GPU finalization, draw and screenshot/end-frame
hooks remain render-admitted. CPU relative order is preserved; LATE_UPDATE now
finishes before the render preparation/draw, matching the canonical phase order.
Desktop MIN emits focus loss, never mobile/explicit PAUSE. Restore uses observed
window focus and cannot release another pause reason. Actual drawable queries
refuse zero pixels, including software fallback; MAX_FRAMES is not a bypass.
The resize watcher rejects a real OS pause before renderer resize. Resumed
non-drawable intervals discard simulation dt once, retaining original wall/KPI
clocks. Accepted CPU dt is finite and bounded at 0.1 seconds even after a fixed-dt
override; the existing fixed clock owns its accumulator/tick policy.

Effective background set/get APIs reuse the frame-cap host property owner and
are main-thread only, default false. They are not getters for authored settings.
The shared SDK/Editor policy saves, publishes run_in_background subject to manual
and OS pause, then conditionally restores its prior permission. LIFO nested
policies and a later external override have actual-property controls; this is
not a host registry or an arbitrary concurrent-policy guarantee. Background true
consumes shared CPU hooks and the existing authored pacing wait; frozen/no-cap
paths retain a finite responsiveness yield. Maintenance stays in the existing
upstream async/should_quit owners. A CPU tick never increments completed render
indices or satisfies MAX_FRAMES; elapsed autostop and capture poll still run.
Foreground auxiliary CPU hooks retain their prior dispatch independently of a
runtime's manual/focus gate. That existing foreground consistency gap remains
explicit; this change does not introduce a second CPU-step authority.

Stock scene transition updates only note entity-cache invalidation. An admitted
app_draw consumes its pending reset once; teardown clears retained state. Runtime
and Editor use public scene_video_advance; upload occurs at scene renderer
preparation. Existing video path/ECS texture retirement may still issue destroy,
which is observed once and is not a GPU fence. Shared-owner CPU fixtures and
single-wire ordinary negatives complement future rebuilt SDK, physical window,
Web resize, closure and performance evidence; they certify none of those alone.

## Runtime networking interpolation admission

The runtime's two networking render-step consumers share one explicit
`net_bridged && session_mode != NONE` block, after scene updates and before
prediction/sequencer/video. Preserve transform-then-animator order and forward
the same runtime fixed-clock alpha once. A session shutdown or unbridged runtime
must not service the process-global animator snapshot table. Do not push this
gate into the L4 animator API: its direct no-session injection/loopback contract
and tests remain valid. Prediction and other scene lifecycle consumers keep
their existing independent guards. The actual call-site four-combination CPU
fixture and historical omitted-braces ordinary control do not certify sockets,
full runtime creation or physical rendering. No public ABI changes are needed.

## Lost Web context admission

The Engine consumes the renderer's private current-context/epoch gate before
async completions, callbacks, resize and frame work. Refusal terminates with
JCE_APP_FAILURE through existing finalization/teardown; it never selects the
minimized/background CPU branch. Event dispatch refuses before user callbacks;
resize watch skips reset and lets the main owner finish the failure. Rechecks
after BeginFrame and EndFrame prevent later owned GPU work or a false completed
interval when loss is observed during callbacks/submission. CPU branch controls
cover entry/result/cleanup requests, not real Web loss or resource recovery.

## Optional runtime mixer resolution

Only the implicit Settings/audio_mixer.json address may bypass the live read
service after jce_fs_probe_read proves ABSENT. Explicit paths and UNKNOWN
(including custom readers) retain their actual read opportunity before the
existing runtime PAK fallback. Packed-only NULL paths stay supported. Exact
PAK decode length, allocation/size and parse errors never become optional
absence; real IO FAILED states remain unchanged. runtime.mixer.configure emits
once at initialization with selected/attempted source, outcome, probe, explicit
request, read failure and actual defaults/config consumption. A degraded PAK
fallback retains the failed high-level outcome and low-level IO. Actual parser
and offline-device sends are tested separately from rebuilt SDK/browser traces.

## Shared optional resource observations

`jce_engine_resource_observation.h` is one private Engine-owned, main-thread
sample state. `JCE_RESOURCE_TRACE` follows the existing environment flag policy
and defaults off; `JCE_RESOURCE_TRACE_MS` uses the existing positive-u32 boot
parser, defaults to1000ms and clamps to250..60000ms. Disabled observation does
not query time, stats or fallback state and creates no tracing metadata. Trace
must also be enabled. No new thread, allocator, exporter or registry is owned.
The original boot initialization remains unchanged. Periodic Async stats use
the private non-creating current-default snapshot under its existing lifetime
guard; no borrowed boot pointer is retained. Absent/closing returns unavailable,
and public shutdown/recreation is observed through the current owner without
pointer-identity or continuous-instance claims. Engine destruction/reentry
guards retain their authority.

Headless, render-blocked (including paused/non-drawable) and software-fallback
hosts observe CPU resources before their no-draw paths. Normal hosts observe
after their app draw and before the existing end-frame, when the qualified GPU
getter can read the existing context. The same cadence state admits one current
sample, never catch-up; monotonic rollback rebases it. Wrong-owner calls refuse.
These observations neither change simulation dt nor force draw, wait or submit.

The existing `engine.app_resources` trace scope owns fixed counters. All wide
values use `name.hi32`/`name.lo32`; reconstruct unsigned as `(hi<<32)|lo` and raw
signed GPU values by interpreting those64 bits as two's complement. Each half
fits exact JSON/JS integers, retaining the existing Chrome pid1 and parser.
Values unavailable in this sample emit zero placeholders with distinct
availability flags, never stale valid data or a claimed zero-ms GPU cost.
RSS/commit belong to the process snapshot; tagged totals explicitly have partial
voluntary coverage. Async counts snapshot the current default owner. GPU context and
host/source identities are observation facts, not same-frame joins, fences or
presentation/completed retirement. The actual owner thread and headless state
are available; PID/host role are unavailable until an authoritative shared
source exists. An app name must not become a guessed role or executable identity.


Resource observation ownership preserves the initialized raw thread ID, including
SDL's zero ID on the current non-pthread Web build. Enabled state and exact
current/initialized-owner equality remain required; no substitute positive ID
is invented. The Chrome producer/encoder retain their existing identity rules.
Actual Web collection is distinct from native CPU owner/encoder ports.


## Shared audio master retirement

Engine checks the audio owner's allocation-free current-callback query before
creation or destruction can start other service waits, including same-thread
Web audio callbacks. A newly admitted owner opens audio session admission after
marking the host thread. Every DRAINING route closes it alongside default async
and shared-pool admission. `jce_engine_audio_retirement.inc.h` observes the
existing private audio owner under `engine.audio_master_retire` after async
shutdown succeeds and before app.exit/event-watch/subsystem teardown. Refusal
uses cleanup reason6, retains the same Engine/app owners and never enters the
irreversible tail. Final device retirement precedes graph release in app.exit;
ordinary Play/Stop does not retire the master. Native joins have no portable
latency guarantee, and CPU retirement does not certify presentation or GPU
completion. Complete-Engine controls port the audio service through its actual
callback gate; ordinary missing-retirement/session-close controls must fail
runtime assertions after compilation.
The same service scope publishes typed `audio_master.had_device`,
`audio_master.device_live_after` and `audio_master.retired` counters (0/1),
using the serialized audio owner's actual device flag before and after its
unchanged retirement call. A successful no-device no-op is explicitly 0/0/1;
a device-bearing successful transition is 1/0/1; refusal preserves the actual
after flag and FAILED scope. Missing counters are unavailable, never inferred
from Dist-compiled-out INFO logs. Disabled scopes add no device queries. This
observes the native uninit call returning, not acoustic output or a portable
join deadline; no public API, cleanup policy or per-frame producer changes.

Startup shader development roots reuse the Engine's sole JCE_SHADER_DEV_DIR
read. The same resolved value is bound through the private renderer lifecycle
bridge before standard shader loading and app initialization; failed binding
fails boot and follows the existing cleanup path. No second environment owner
or public API is introduced. The renderer snapshot covers startup/restart
consumption by independent grass and foliage programs; it is not a hot-reload
broadcast or proof that every renderer program consumes the development root.

## Explicit frame KPI queue budget

Requested captures latch `JCE_KPI_WRITER_QUEUE` once before stream IO or owned
allocation. Unset/empty retains normal 1 MiB/1024 or low-memory 256 KiB/256
limits. An explicit positive ASCII decimal `bytes,records` pair selects both
budgets atomically, at most 67108864 bytes and 65536 descriptors. Signs,
whitespace, missing/extra delimiters, overflow and zero fail initialization
without truncating an existing destination. Disabled captures ignore the
selector. Existing writer status/start/failure logs expose the effective
capacities. This is an explicit recording-memory budget, not an IO deadline
or a guarantee against arbitrary storage/scheduling stalls. It does not change
16 KiB/oldest 8 ms dispatch, finite LOW tasks, strict FULL, first-error retention,
completion-before-close, or default/low-memory allocation policy.

The retained frame KPI success footer appends actual `capacity_bytes` and `capacity_records` after the unchanged accepted/written/peak prefix, so Dist observers do not depend on compile-stripped startup INFO.

## AudioSource pending play ownership

Stop/restart retires both active voices and pending play intents. A taskless
decode slot has no executor borrower and may be freed immediately. Submitted
slots disarm their full entity binding and request cancellation; their stable
args and CPU result remain owned until terminal poll or the existing scene
teardown discard barrier. A completed decode is not permission to play after
Stop. Poll also rejects a component whose current clip differs from the
captured decode path. Do not turn cancellation requests into premature frees,
block frame-time Stop waiting on a decoder, or stack old pending restarts.

## Noninteractive bootstrap and finite fallback

Resolve modal eligibility once from the existing headless, hidden-window and
not-focusable requests through jce_engine_window_policy.h. Fatal errors always
retain the original ERROR payload; software fallback logs its original warning
once. Only eligible interactive requests open the original SDL dialogs. Dist
retains real WARN/ERROR macros; do not infer their availability from INFO.

Read the existing finite-run budgets before any renderer-ready early return.
Normal timed runs retain their late resource-initialization clock origin;
software fallback starts at its existing reset-frame-clock boundary. Its actual
completed software iterations use the same autostop/quit owner but release with
FULL=false and FALLBACK disposition. Minimized polls still cannot count as
completed frames. Do not turn software iterations into raw GPU/frame evidence.
Since fallback skips app.init, should_quit and blocked CPU application updates
require the existing app_init_entered ownership state. Exit already has that
ownership guard; do not add a second loop, public flag or platform special case.

The existing minimized, launch, frame-cap and background Native controls consume
actual private bodies and the shared policy, with typed host destinations.
Ordinary missing modal, warning, budget, callback and false-FULL controls retain
runtime assertion failures. These CPU controls do not certify native SDL dialog
suppression, an unsupported real graphics driver or XP application execution.

Runtime buoyancy blends its existing ambient WaterField sample with the
scene's published WaterNearshore sample, then adds the legacy wake height.
Interior dry coverage suppresses the buoyancy force and wake impulse.

Runtime continuous buoyancy/wake coupling calls the private scene ripple driver
with upward body velocity and the executed fixed timestep. It must not repeat
the public one-shot velocity impulse every tick: additive feedback can lower the
water under the body until the depth-dependent drag becomes unstable. Collider
radius, submersion/coverage and the existing deadband keep their current owners.
The force model and public event-impulse API remain unchanged.

## Optional exact-path script cost partition

`jce_rt_script_cost.c` owns the unchanged public script-cost iterate/reset
bodies and an optional private launch selector. The gameplay loop still reads
`jce_perf_phase_enabled()` once, preserves its existing two counter reads around
each admitted on_update, and forwards that same measured ms to the partition.
Inactive and SimLod-skipped callbacks do not contribute. Fixed updates and VM
coroutines retain their separate existing owners and are not included.

`JCE_PERF_SCRIPT_DETAIL=1` explicitly enables an exact, case-sensitive
`JCE_PERF_SCRIPT_PATH` byte match against ScriptEntry's authored path. The
main-thread selector is latched on the first profiled script pass for this
process; it copies at most255 path bytes, rejects empty/overlength/control bytes,
and does not retain getenv storage. Profiling off performs no selector read.
Defaults register no new phase names. Explicit selector/refusal diagnostics use
the existing direct logger so Dist can observe them subject to sink filtering.

Only the fixed literal slots `script_focus` and `script_other` are registered.
They accumulate the current measured callbacks across runtimes into the same
Engine-owned frame snapshot as `script`; no per-entity names, frame IDs, clocks,
resets or public ABI are added. Each enabled pass publishes both subtotals,
including zero. Zero does not certify that a callback ran. The sum agrees with
`script` up to floating-point accumulation and CSV rounding. The public cost
iterator keeps its intentional most-recent-row semantics and is never polled
to manufacture a current-frame partition. Additional diagnostics perturb timing
and require a separate capture mode. Native runtime tests remain in the
existing test_jce_runtime_script_cost target; CPU source ports do not certify
Lua execution, a rebuilt Editor, graphical behavior or performance.

## Player identity for read-only debug range

`jce_rt_character.c` owns `jce_runtime_get_player_character`: return the current
character only when runtime, physics, nonzero character entity and valid handle
exist. Fresh zeroed state must not expose slot zero. This adds no body/entity
guessing, scene mutation, simulation step or retained cache. The receiving
physics query checks the live pool/world; character handles remain borrowed
current-call identities. Keep this accessor with the existing character owner
rather than growing the frozen large jce_runtime.c. Player-centered debug range
uses the existing smoothed player-position accessor, with camera fallback.
