# Application CPU fixtures

Stock FrameLog `natural` removes only FrameLog's draw-capture override. Empty
FrameLog paths never force draw; default/invalid selectors retain prior capture
behavior, and explicit game screenshots keep precedence. The actual stock draw
prefix plus shared lifecycle/host policy is exercised by
`test_jce_default_cache_deferred`; `test_jce_default_frame_log_old_force` restores
only the old force and must compile then fail ordinary admission assertions.
These CPU controls do not certify native focus, GPU load or performance gains.

`test_jce_render_msaa_boot` compiles the complete actual RenderPipeline and
RenderSettings owners, includes the complete Engine graphics-config function
and renderer MSAA setter, and executes the two stock-entry configuration blocks
in their actual source order. Only unrelated services, the host-base lookup and
the backend reset boundary are typed ports. Real JSON/file inputs distinguish
explicit canonical MSAA from legacy fallback; user graphics and numeric QA force
have separate precedence controls. Assertions compare requested/effective RP with
actual renderer reset flags, preserving VSync, first application, legal legacy
16-sample requests, unavailable-renderer refusal and later direct UI requests.
The packed variant uses the actual stock config decompressor plus the public
archive writer/PAK reader, and conflicts loose files against packed policy.
The normal variant separately requires a project's custom cooked_assets path
over a conflicting default file and a malformed host legacy file falling back
to valid PAK bytes. Both force invocations cover valid user graphics and absent
graphics sections through stock's final effective synchronization.
Public loads still retain LOW defaults; Engine's absent-graphics API remains
a strict no-op independently of stock's final synchronization.
The old_late_legacy variant restores the former late lower-priority writer and
must compile then fail unchanged ordinary Unity assertions. The isolated force
process shares a CTest resource lock with ordinary file controls. Registration
is in render_msaa_boot_controls.cmake. CPU reset requests do not certify scene
geometry MSAA, backend allocation, GPU execution, pixels or performance.

Host cleanup controls consume the complete desktop JNI host from
engine/src/application/jce_jni_bridge.c, beside the SDL host. The source move
does not change the typed adapter ports or accepted ordinary negative controls;
actual JVM linking and execution remain separate validation.

The complete SDL host fixture supplies SDL's actual retained appstate parameter,
including failure, duplicate Init and final-only context/ACK release. OOM and
foreign-thread ports cannot create or borrow an owner; nested finalization must
latch failure without a second release. Quit is SDL's once-only final callback:
forced refusal is process-lifetime retention with no ACK, never a fabricated
subsequent Iterate. Fixture quarantine prevents old-owner mutations from reading
freed memory; its physical cleanup is case isolation, not SDL recovery. Exact
per-allocation flags independently check SDL and JNI ACK ordering. A real Core
allocation paired across logger and SDL shutdown verifies live-count/byte
balance without resetting counters. Current-source plus omitted retention,
ACK, free-order, thread-admission and reentry guards must compile and fail
ordinary unchanged assertions; these CPU ports do not run SDL's real event loop.

The terminal acknowledgement fixtures keep host adapter and actual Engine
evidence distinct. `test_jce_host_cleanup` includes the complete SDL/JNI adapters
and terminal observer with typed ownership/IO ports; retained, empty, duplicate,
immediate failed-init, JNI bridge-free ordering and partial-output cases remain
ordinary assertions. Typed variadic print ports consume the actual format and
arguments; the partial-prefix port returns a negative stdio error after retaining
only the acknowledgement prefix. A recovered second call must neither append
nor replay it. `test_jce_engine_terminal` uses the real headless Engine,
logger and continuous capture for ready, failed and borrowed construction. Its
accepted stderr bytes must occur after SDL and owner release and correlate the
actual successful cleanup scope/session packet with the emitted JSON. Omitted
tail/session/host-ack wires are compiled complete-owner negative controls. These
CPU controls do not qualify native UI, JVM integration, durable IO or GPU work.

`test_jce_engine_async_retirement` includes the complete shipping Engine owner
and links the real default executor. Its typed initial-registry port registers
a test subsystem before forwarding to actual init_all; application exit,
subsystem quiesce and shutdown then run through unchanged production teardown.
Each attempts lazy default creation after the original executor has retired.
Ready/failed-init/borrowed-constructor cases require rejection, closed admission
after full release and after invalid early create attempts, and a working next
admitted Engine. The constructor case explicitly drains its retained executor
after requesting close to prove that a deferred borrow cannot reopen creation.
The complete-Engine old_default_reopen control removes only the session-close
call. Any recreated real pool is immediately drained by the probe, then ordinary
unchanged assertions fail; leaking threads, crashing or failing to compile is
not an accepted negative result. Core's separate fixture retains standalone
shutdown/recreate and queryability of an already retained default executor.
The same actual Engine fixture now creates the shared frame pool at init, joins
it inside the first exit callback, and checks that subsequent exit/quiesce/
shutdown callbacks and the inter-session gap cannot recreate it. The borrowed
constructor path explicitly joins it before lookup as well. A working next
Engine must admit both pools. Complete-Engine old_shared_reopen removes only
the shared-pool session-close wire and must fail ordinary unchanged assertions
after safely draining any unexpected pool. Core's actual-worker reentry fixture
separately tests the join-in-progress guard; neither test substitutes for the other.

`test_jce_engine_sampler_lifetime` links the actual complete Engine and includes
the actual sampler owner behind its existing typed native-operation port. Its
original ready-session shutdown cases remain separate from combined failed-init
cases. `cleanup_owner_controls.cmake` registers fresh-process owned and legacy
bootstrap failures with collector timeout, WAIT_FAILED and unresolved resume,
plus repeated constructor-callback and update-callback destruction. Assertions require retained owner identity,
unchanged occupied output slots, no replacement session or descriptor hijack,
no event/update/resize callbacks while draining, no service-teardown query while
the constructor is still borrowed, closed reason/completion trace
attempts, exactly-once final release, restored FPS ownership and a working next
boot. Native collector operations are controlled; the Engine, executor, renderer
Noop owner and trace ring are real. These CPU controls do not certify native
sampler timing, a rendered application, GPU completion or whole-frame speed.
The old-lost-owner target compiles the complete shipping Engine TU with only
the failure-tail output retention removed. Its unchanged early owner assertion
must fail ordinarily before any borrowed-service access; configure refusal,
compile failure or crash is not an accepted behavioral negative.
The old-state-borrow target changes only the constructor borrow check back to
the mutable lifecycle state. A real sampler timeout holds storage safely while
the second in-init close incorrectly queries teardown; the unchanged zero-query
assertion must fail ordinarily. Both mutants share the complete Engine source
and the positive fixture, with no copied cleanup implementation.

The existing `test_jce_engine_cleanup_owned_timeout` also enables its test-only
`JCE_ENGINE_CLEANUP_TRACE=1` selector. It uses production continuous threaded
capture for the failed-init/refusal/recovery session, then switches the existing
capture path before its required next boot. Files under the configured test
output are `owned-timeout.cleanup.json` and `owned-timeout.reboot.json`; the
fixture retains bounded first-session bytes and proves reboot did not overwrite
them. Other modes and behavioral negatives keep export disabled by default.
The real Engine/sampler fault-port assertions remain unchanged. Run the first
file through `tools/lifecycle_trace_report.py --cleanup-ownership --require
engine.async_shutdown --out <report.json>` to validate actual producer events
and a complete continuous footer; file existence alone is not lifecycle PASS.
Require three sampler deferrals and exactly one final success for this specific
failed-init receipt. The reboot receipt is a separate successful Engine session,
not an extra success appended to the failed session. This is CPU fault-injection
evidence, not native collector timing, GPU completion or an application benchmark.
These are reusable build-cache outputs: the fixture removes only its exact
selected cleanup/reboot paths before each corresponding capture. It never writes
under reports; archive each tested file and analyzer receipt there before a new
CTest run if that run's evidence must remain immutable. Do not require
`engine.cleanup_attempt` as a universally successful service: its intentional
failed retries are validated by `--cleanup-ownership` instead.

`test_jce_host_cleanup` includes both complete shipping SDL and JNI adapters.
First-party authored SDL/JNI declaration ports do not include upstream headers,
open a window, start a JVM or invoke a device. Counted engine-result, event,
poll, delay, exception and allocator ports exercise boot failure with retained
ownership, terminal precedence, repeated cleanup refusal, recovery, manual close,
external quit, duplicate init and allocation refusal. Core supplies the real
bridge heap; the free port quarantines it until tearDown so premature-free
negative controls fail ordinary assertions without fixture use-after-free.
The old SDL pointer-clear and JNI free-on-refusal variants must compile and fail
unchanged assertions. Native Engine cleanup integration is a separate fixture;
these host ports do not prove the engine itself releases a real worker safely.

`test_jce_java_host_cleanup.py` compiles complete `JceRuntime.java` and `Main.java`
with only native-library loading/ABI calls and six native declarations replaced
by typed Java ports. Shipping init/iterate/result/close and Main's finally drain
are unchanged. Controls assert failed-init reachability, repeated-close refusal,
idempotent init/close, pending-session rejection, original init-failure reporting
and interruption-safe cleanup. Same-source premature Java clear and abandoned
try-resource Main variants must produce ordinary AssertionError after successful
compilation; compilation errors never qualify a negative control. Missing JDK
is explicit UNAVAILABLE/77. Use the configured JNI JDK when supplied, retain
generated inputs/classes under the caller's build cache, and do not infer actual
JNI native loading or GPU behavior from these Java forwarding controls.

`test_jce_engine_launch_policy` includes the complete strict launch parser and
actual frame-cap owner with the real Core trace ring. Six groups cover absent
versus explicit zero, typed bounds/rejection without destination mutation,
pre-boot/latest request restoration, foreign owner refusal, same-rate deadlines,
background permission preservation, disabled tracing and closed transition-only
getter evidence. Its old-requested-wins variant removes only the actual forced
resolution and must compile then fail ordinary assertions. Registration lives
in launch_policy_controls.cmake; it does not measure native pacing or GPU work.
The existing real headless-boot fixture separately checks bootstrap rejection,
environment snapshot immutability, actual app.init failure cleanup and the next
ordinary session. It retains all prior cases, including its hidden-window ABI
cases; the added launch cases themselves request headless Noop only.

`test_jce_engine_launch_bootstrap` consumes the actual Renderer creation/common
bind, software fallback early-return and headless join blocks in their original
source order. Core-only typed factory, warning, clock and Renderer-bind ports
never initialize SDL, a window or GPU; the complete FPS owner is real. Existing
groups cover normal, headless, fallback, Noop availability, both creation-failure
paths, absent policy and foreign FPS ownership. Assertions retain fallback's
early return and headless's join while requiring exactly one admitted bind.
Its old-fallback-bind variant moves the same actual bind after the early return
and must compile then fail ordinary unchanged assertions. Intervening shader,
input and audio bodies are outside this extracted CPU control; existing actual
headless bootstrap integration remains independent. This is not a native SDL
fallback or presentation test.

`test_jce_runtime_trace` compiles the complete shipping Runtime TU with real
Scene/Core dependencies and the canonical trace ring. A typed malloc forwarder
only injects the actual constructor's first allocation failure. Five groups
cover create/tick/destroy host parenting, nested phase closure, invalid desc,
allocation and viewport-options failures, disabled capture and NULL no-ops.
The old-missing-wire complete-owner variant must compile and fail unchanged
ordinary assertions. These are CPU lifecycle observations, not device, GPU,
whole-application completeness or performance certification. Runtime's existing
optional subsystem failure semantics remain unchanged.

Follow the repository and tests/ charters. This directory owns engine/application
lifecycle and capture controls; generated owners remain under the configured
build directory. Keep production implementations in engine/src/application.

Frame KPI legacy fixtures retain the exact three-column header and payload.
Explicit context controls use the fixed fourth context_json column, CSV escaping,
typed null/zero distinctions and one completed context per accepted writer record.
Header bytes stay outside writer payload accounting. Selector output files are
target-specific build paths; invalid selectors must refuse before stream IO.

The same complete-writer fixture uses its typed clock-read port only in
cooperative cases to check low-backlog byte/oldest-age dispatch, a later arrival
not resetting the oldest age, a fresh age after drain, descriptor saturation,
durability-record boundaries and immediate finish of an unaged tail. It compares
actual executor task entries/completions, SDL data callback counts, exact payload
bytes and flush positions; zero config retains the original immediate calls.
Enabled coalescing additionally covers real RUNNING/QUEUED strict FULL, cancellation,
executor rejection, short/error writes and terminal flush/close failures. Clock
ports leave executor/native clocks real and all owners join before assertions.
The ordinary `old_immediate_dispatch` complete-owner mutation removes only the
dispatch gate; unchanged count assertions must fail, never compilation or cleanup.
These controls prove dispatch behavior and call reduction for their inputs, not
hardware IO latency or elimination of a previous multi-second write wall bracket.

The complete-writer fixture additionally uses real threaded executors and
finite safety releases for an actual blocked SDL sink and an occupied single
worker. Each covers successful release/drain and strict full-queue refusal;
RUNNING versus QUEUED comes from the real task, never a fake state provider.
An actual blocked durability flush proves that already-written bytes can still
occupy the queue, so the first-error snapshot cannot use accepted-minus-written.
A wrapped write preserves its same-serial later return even when a second span
updates the live observation. The log's `return_published_later` reports later
publication, not later API completion; physical return may precede the failure
snapshot while waiting for its mutex, and equal timestamps prove no strict order.
All blocked owners are released/joined before
ordinary assertions. `frame_kpi_writer_controls.cmake` compiles complete-owner
missing-snapshot and inferred-occupancy variants; both must fail those unchanged
assertions, not compilation or cleanup. The real capture/logger fixture checks
that the original incomplete footer and its two bounded diagnostic records
appear once, including in DIST. These CPU controls distinguish retained evidence,
not storage latency, device performance or a particular historical failure cause.

Backend-timing controls retain exact default CSV bytes and the same one stats
getter per accepted record. They exercise both directions of environment changes
after admission, destroy/reuse, and invalid selections preserving a pre-existing
destination sentinel before any writer is acquired. Encoder controls preserve
independent unavailable timers, ambiguous finite zero, exact/one-byte-short
complete-record budgets and the existing single-write synchronous path. These
are CPU/IO ownership controls, not GPU execution or current-frame attribution.
The selector fixture sets the same libc environment consumed by production
`getenv` (`_putenv_s` on Windows, `setenv`/`unsetenv` elsewhere) and reads it back
before each requested open. SDL's Win32 environment setter may update a separate
OS/SDL table without updating the executable CRT table; setter success alone
does not prove the production reader observed the requested selector.

`test_jce_frame_kpi_prefix` includes the complete shipping private encoder and
the exact former per-row-format variant generated from guarded owner wires.
Only this internal owner fixture may include the private encoder/memory headers
and use CRT `vsnprintf` forwarding and `printf` for optional CPU diagnostics;
JCE remains the actual allocator, timer, phase and trace owner. Ports replace
format/reallocation/free calls after their real declarations, count uint64
conversions and inject prefix-format/refused-growth failures without new
production flags or ABI. Controls retain exact old/new bytes across integer
boundaries, signed zero, full phase/context records, long rows, exact/short
budgets and first/growth OOM cleanup. The old-per-row complete-owner target must
compile and fail unchanged ordinary assertions. `--benchmark` on the positive
exe runs 12 alternating old/current pairs of 256 records, retains every group's
wall duration and formatter/allocation counts, and checks full bytes each time.
It times encode plus exact comparison with test ports and phase observation;
there is no speed threshold, OS on-CPU measurement or application FPS claim.

test_jce_engine_frame_context compiles the exact production context helpers,
renderer begin/end adjacency and frame-release seal with typed public window and
renderer ports plus a private SDL native-size macro port. The real capture,
encoder, writer, cooperative executor and terminal drain remain linked. Its
missing-begin/end/seal variants remove actual wires without changing declarations;
the capture old-context-wire variant removes the actual encoder argument. These
variants must compile and fail ordinary unchanged assertions. Preserve the older
context-admission fixture's independent loss/refusal boundary.
The same fixture includes the actual completed/pending export and frame-wall
arithmetic bodies. V3 checks borrowed begin/end/frequency strings, normal pending
and terminal completion, failed app results, invalid frequency/order/duration,
zero counters/durations and next-context reset. A typed counter port distinguishes
the existing two end_frame bracket reads per render from the one Engine terminal
boundary read; pending and direct completed export add no counter reads. The
ordinary missing-clock wire target compiles the same bodies and must fail the
same assertions without crashing. Legacy paths retain their getter/native-query
counts and bytes. The encoder separately checks maximum uint64 strings, rounding,
all four maximum-width viewports and exact/short fixed record budgets. The real
capture writer's data callback attempts both valid and invalid v3 interval
rebinding while its owner operation is active; both refuse without changing the
context, failure/index or writer state. A later normal rebind and second complete
row succeed after that callback returns. Legacy/inactive paths skip this guard.

Delayed reuse is covered by the existing completed-interval owner, the actual
capture sealed state and the counted no-resampling control together; it does
not turn the fixture's synthetic boundary into an actual native frame clock.

The same context fixture now includes the exact app.draw observation gate and
public Engine report entries, actual main-thread registry and a joined native
wrong-thread call. The Engine-ready state remains a typed owner port; real
boot/disposition tests retain lifecycle acceptance. Mode2 tests distinguish
outside-draw/off/v1/inactive refusal, failed count/stride/alignment/size/duplicate
inputs, copied ownership, explicit empty, unreported and fresh-interval reset.
Real encoder/capture/writer owners preserve the sealed packet through delayed
export; v1 exact-byte controls remain unchanged. The encoder fixture additionally
covers all four maximum-width records within the unchanged writer budget,
future-sized prefix copies, short prefixes, null versus measured zero and
fail-closed malformed internal packets. These are CPU ownership/encoding tests,
not actual Editor consumption, hardware submission or visual/performance proof.

These CPU observations do not establish native resize/focus delivery, GPU frame
identity, presentation, pixel correctness or performance. Do not run native
builds, tests or applications when the active task explicitly reserves them.

test_jce_engine_background consumes the actual drawable query, private
HostLifecycle transition observer and real Core trace ring. Counted native-size
and public-flag ports cover changed and repeated inputs, combined pause/minimize,
known zero versus failed queries, headless/no-window admission, new capture
generation and disabled/suppressed/zero-scope retry. Flags are transition samples
only and must not enter render policy. Same-source missing-publication and
unknown-as-known-zero controls must compile and fail ordinary assertions; the
existing CPU policy/restore controls remain independent. These fixtures do not
qualify actual native events, visibility, DWM/GPU load or frame throughput.

`test_jce_engine_terminal` consumes the real retained process trace ring across
three Engine sessions. Match the acknowledgement's exact session and cleanup
scope; earlier nonzero session tickets may remain in later captures and must
be strictly older, never accepted as the current terminal witness. Require one
current packet without resetting away prior history. The omitted-tail and
omitted-session targets retain the same ordinary assertions.


## Real boot fixture isolation

`test_jce_engine_kpi_disposition` reuses the complete public Engine lifecycle
fixture and real counter/logger/writer shutdown. Three headless iterations,
nested rejected iterate calls and repeated terminal delivery require one bounded
raw-boundary disposition summary with exact interval/row identities. No headless
work may become a render row or a successful 240-second deadline. The old-terminal
target compiles the complete Engine with its former result body frozen in
engine_interval_controls.cmake and the new teardown publication removed;
unchanged ordinary assertions must reject missing
publication after successful compilation. This does not certify native drawable
events, GPU execution, presentation or throughput. Root runs RED/GREEN serially.

The lifecycle fixture verifies JCE_LOG_FILE equals its compile-time output and
constant case name before starting a fresh empty session log with public host IO.
This resets only that exact owned test-cache file, never an arbitrary environment
path or archived report. Repeats retain the same once-only assertions and bounded
read budget without counting appended previous sessions. The direct-destroy case
shares the raw summary/identity assertions but reaches teardown as its first
terminal path, with no preceding quit event. Both paths require exactly one
clock-v1 witness with unchecked/invalid status and no deadline-reached marker.
The disposition receipt cannot substitute for that independent witness. Both have the same
ordinary old-terminal missing-publication control.
The old-direct-clock complete-Engine variant removes only the teardown clock
reporter call; the unchanged direct-destroy assertions must fail ordinarily for
the missing clock witness while disposition remains present. The frame-context
fixture extracts the actual shared clock reporter and direct capture-close body.
Two real writer records and the real capture-destroy reset prove the saved final
ordinal reaches actual clock arithmetic. Its typed logger observes the latch
before publication and attempts bounded reentry; repeated closure and disabled
clock capture do not publish twice. A reset-index reporter argument mutant must
fail the same nonzero-ordinal assertions. Synthetic raw boundaries and typed
logger/cleanup ports here do not establish native rendering or GPU completion.

The actual PerfHitch owner controls raw zero/ordered boundaries, disjoint reason
counts, entry-row identity, normal and terminal closure, rejected nested or
pre-boundary calls, repeated reporting, invalid frequency/order/release and
overflow. Its missing-release-witness variant removes only the actual release
assignment; unchanged assertions must fail ordinarily. The background fixture
executes the actual drawable and blocked-render branch with every simultaneous
gate combination while trace is disabled; a missing-disposition-wire variant
must fail those same reason assertions. The context-admission fixture retains
the exact rendering-guard branch and actual ledger, with typed input/poll ports.
Minimal paused-timer fixtures keep their older exact timer/branch assertions and
link the real disabled ledger rather than replacing the new observation call.

test_jce_headless_boot and test_jce_engine_input_backend are Engine integration
fixtures because each contains a real hidden window/device branch. They share
the existing jce_renderer_context CTest resource name, retaining the 60-second
timeout. The lock only coordinates participating tests; it is not proof that a
backend or cleanup stall is fixed. Neither fixture pins D3D11 or forces all boots
headless. The input constructor and actual windowed call-site assertions, all
headless/descriptor-ABI cases and launch-policy cases remain present.

Each target receives its own ignored CMake output directory. Every Unity case
writes a fresh PID/case-scoped config using public host IO, explicitly selecting
Auto with no remembered backend. jce_engine_set_config_path owns selection;
app.init captures a PID/case clear-color marker from JceServices.config; tearDown
asserts actual selection, clears the override and removes only that case's file, even
when an assertion aborted. Existing executable-adjacent and user configs are
never read as this fixture's selected engine config or rewritten.

The headless simulation's network portion is explicitly a real client/ephemeral
ENet host smoke through public port=0, as in the existing ENet lifetime fixture.
It must create successfully, enter the actual public poll/service path on all
ninety simulation updates and
be released by app exit. Host creation failure fails app.init and uses actual
Engine cleanup; it is no longer silently tolerated. This does not certify a
server listener, remote peer handshake or payload exchange. Ordinary missing-host
or omitted-pump-branch mutations must fail these assertions; the local counter
is a CPU path witness, not transport delivery. Neither a timeout nor
an unavailable device qualifies a behavioral negative.

The default-entry context-v2 publisher is the static SDK helper
jce_default_frame_observation.inc.h, consumed by the actual app_draw. The
existing default-cache fixture retains all six admission/cache cases and adds
five observation cases. Its generated adapter executes the exact complete
scene-draw branch between the actual admission prefix and final publication;
UI/capture work between rendering and the final tail is outside this CPU
fixture. Inert GPU ports count requests/returns and never replace the branch
algorithm. Missing-early-publication and missing-CPU-return variants must compile
and fail ordinary assertions. Direct fallback must clear bridge dimensions;
prepare success, admitted allocation and CPU-call-return remain separate. The
public renderer may refuse internally and still return its base view, so
CPU_COMPLETED means call return plus host tail only. Display/texture/ImGui
counts remain unavailable, including screenshots and PostFX void present.
Disabled/v1 observation performs only the existing public enabled check and no
observation POD clear or extra size/extent query. Root serializes builds and
native acceptance; these CPU controls prove neither pixels nor GPU throughput.

`test_jce_engine_audio_retirement` also invokes the actual private retirement
adapter with the real trace ring and actual callback gate. Typed device-state
ports distinguish retained live-device refusal, successful live-to-retired,
no-device success and disabled-observer execution. Each enabled attempt needs
one correctly parented 0/1 had-device/after-device/result triple inside its
balanced scope. Generated complete-Engine adapter controls remove the counter
or hardcode had-device false and must fail ordinary assertions after compiling.
Separate actual-master/no-audio fixtures validate the real flag getter; these
CPU ports do not certify a physical device or the unexamined GUI trace.

The complete capture fixture opens real host streams to verify the explicit
`JCE_KPI_WRITER_QUEUE` selector reaches the actual writer, preserves normal and
low-memory defaults, accepts both upper bounds, latches each reused session and
refuses malformed/zero/over-limit values before truncation or tracked owned
allocation. Its disabled path ignores the selector. The complete writer's
existing threaded blocked SDL sink consumes the same production private
selector, retains identical 4 KiB bursts under an enlarged explicit budget and
still refuses independent byte/descriptor exhaustion. Late successful return
must not clear the original FULL; every owner joins and closes once before
assertions. These CPU controls establish bounded admission/retirement, not a
physical disk latency improvement or native whole-frame performance.

## Stock startup-scene diagnostic controls

`default_scene_boot_controls.cmake` extracts the complete current startup-scene
selection/load owner into C99 controls. Typed creation/serial-VFS/Bundle and
logger boundaries observe selection precedence, failed/wrong/missing loads,
published owner ordering, actual override paths and existing diagnostic value
policy. Dist, ordinary, packaged-only and logging-disabled builds preserve their
separate behavior. The old_missing and early_success whole-function variants
must compile and fail ordinary unchanged assertions. These controls prove the
shared host's successful-load record and refusal boundaries, not actual JSON
parsing, logger delivery, refreshed SDK execution, pixels, audio or throughput.

## AudioSource asynchronous Stop/restart controls

`audio_pending_controls.cmake` compiles the complete shipping decode, queue,
poll, start and Play/Stop bodies with real Core async tasks and one native
worker. A semaphore-controlled PCM provider and typed scene/audio ports expose
running, queued and completed-before-poll requests. The fixture covers Stop,
bounded repeated restart, clip replacement, full entity generations, disable,
independent sources and the actual audio teardown segment. Every args/CPU
allocation retires once; submitted args remain alive through terminal state.
Old Stop, stale-clip admission and missing intent-disarm variants must compile
and fail ordinary unchanged assertions. These controls do not certify decoder
fidelity, real scene storage, audible output, complete application exit or FPS.

Noninteractive bootstrap coverage extends those same fixtures. The launch
control consumes the complete private launch-duration parser through its real
header, with scalar-bound controls in the existing launch-policy fixture. Its
extracted unsigned parser and resolved UI-policy call retain
common budget placement and real late-clock assignment. Dist macro compilation
checks the one-time fallback warning separately from its optional dialog; old
late-budget, missing-modal and missing-warning variants fail ordinary assertions.
The minimized control also executes actual fatal, should_quit and software
fallback bodies. Finite frame/time schedules require SUCCESS through the original
quit owner with FULL=false; poisoned uninitialized callback counts stay zero.
Original paused-poll and completed-headless semantics remain required. The
background fixture's declaration marks its ordinary app initialized, preserving
its existing lifecycle/PlayerLoop assertions. The separate shared window-policy
truth table covers all combinations of headless/hidden/not-focusable. No control
creates a native window, proves a GPU row or certifies actual XP fallback.

The stock runtime's offscreen scene-depth test in
test_jce_default_cache_deferred consumes the actual draw host with typed
ports. It checks fog off, fog on and failed bridge preparation. Volumetric
fog dimensions remain feature-owned while the valid scene depth reaches
all consumers. A CPU config observation does not prove shader binding.

The real runtime buoyancy/wake target also runs 30 simulated seconds at six
render/fixed-step combinations, including the editor's 50 Hz physics cadence.
It checks bounded motion and the known flat-water force equilibrium. Preserve
the original impulse-sign, sampled-disturbance and no-body-step checks. The old
continuous one-shot call must fail the long-running ordinary assertion. These
CPU Bullet/runtime controls do not replace full authored-scene native validation.


The complete Rml scene-overlay header controls in rml_scene_batch_controls.cmake
compile as C99 and C++20. The actual tag registry and first-match lookup helpers
are extracted unchanged; scene iteration, component storage, UI, input and clocks
are counted typed ports. Keep dynamic retag/delete/recreate, duplicate first
entity without a UI component, late arrival, cross-scene identity, Untagged and
intern refusal, same-call command deduplication, simultaneous key edges and
original pointer/quarantine controls. These prove local operation counts and
semantics, not native ECS/RmlUi timing, installed-SDK compatibility or FPS. The
complete-header per_binding variant must compile and exit normally with failed
unchanged scan-budget assertions. Preserve that direct receipt in addition to
CTest WILL_FAIL. Regenerate private include ports only under the build cache.

The existing test_jce_runtime_script_cost target also selects an exact authored
script path through the same native C runtime environment consumed by the
private owner. It compares current on_update rows against the same-frame
script_focus/script_other phases, includes two matching instances, and requires
a later runtime with no matching callback to publish zero. It retains the
original ranking, disabled-measurement, fresh-row and non-consuming-reset tests.
Application CMake registration is unchanged. Separate complete-owner/actual-loop
CPU ports exercise selector refusal, skipped updates, multiple runtime calls,
zero durations and ordinary missing-wire/misclassification/gating failures; they
are not execution of the native Lua/Engine test or a graphical acceptance run.
