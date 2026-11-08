# Visual and timing evidence

Canonical readback tracking is opt-in through `--readback-operations`. It
links accepted operations by exact capture-local ID/context/ticket and separates
provider-ready observations, cancellation intent, ordinary CPU release and
release after backend stop. Unknown/unlinked work and outstanding observations
remain explicit; they are not global leaks, device idle or Present completion.
`renderer.backend_shutdown_cpu` measures only the actual synchronous call.
Offline `tracy-import-chrome` can visualize Core CPU zones and reported counter
plots; it does not create native GPU contexts or a GPU timeline. Keep canonical
JSON as the exact identity authority. PresentMon, PDH, GPU-view stats and ETW
require their own process/clock/context/packet/Present/DWM joins. A retained EXE
hash plus sampler PID/birth alone does not establish those joins. Fixed dynamic
A/B runs exclude concurrent compilers, captures and other GPU validations.

Start a new graphics or system-collector protocol with one short owned
control. Set separate finite limits for run, cleanup and evidence volume.
The process sampler gives the launched child five seconds to exit after
termination. An unconfirmed exit fails the run. A kernel-owned ETW session
needs its own identity and retirement observation, independent of process
exit. If either owner cannot confirm cleanup, stop the sequence. Preserve
the failed receipt and continue offline analysis. A software watchdog
requires a responsive OS and cannot guarantee recovery from a host freeze.

Input replay proves the recorded input channel, not that the active camera
controller consumes it. Check the actual project/controller binding: a render
benchmark may substitute a synthetic trajectory with stationary intervals.
Label that workload accordingly. Actual changed camera poses or projection
observations are needed before claiming a moving-camera acceptance; backend,
Engine and simulation frame indices are not interchangeable.

Keep a timed-out nonforeground graphics control as failed. A same-executable
experiment with an explicit startup VSync override can discriminate a blocked
submission/presentation path; retain that override in the receipt. A completed
hidden control neither repairs nor certifies foreground/default presentation,
GPU completion or whole-frame throughput. Never silently disable VSync to pass.
The lifecycle probe strips inherited JCE_* variables, including a parent's
JCE_LAUNCH_VSYNC. Use its explicit --vsync on|off (API vsync="on"|"off") and
verify the recorded child environment contains JCE_LAUNCH_VSYNC=1|0. Omitted/None
keeps sanitization and does not request a VSync override. The recorded request
is not proof of the renderer's effective reset/presentation policy.

Filesystem open-failure details describe one candidate attempt. Before declaring
an asset missing, inspect the owning operation's terminal result and any supported
fallback. A failed relative probe followed by a successful asset-root load is
not a terminal missing-asset result; absent success evidence remains unavailable.


For an existing shader container, use `python tools/shader_payload.py --inspector
<native-jce_shader_inspect> --input <existing.bin> --out <new.dxbc>` for offline
payload export. The native opt-in `--json --payload-hex` reuses the sole reflection
parser; the wrapper bounds and verifies bytes, records hashes and exclusively
creates the output. It neither compiles a shader nor initializes a GPU. Existing
outputs are refused. Disassemble the exported payload with the platform tool;
container, payload and active-draw identities are separate evidence.


A fixed simulation timestep does not fix random streams. Use existing public
component parameters or supported test controls to seed each participating
stream, and retain the actual seed-application witness. Compare dynamic geometry
and alpha as well as camera clocks before diagnosing pixel noise. A shared Lua
VM seed does not establish native/GPU particle or whole-application determinism.
Do not subtract, mask or label real geometry differences as antialiasing noise.

For a shader-only experiment, keep one pinned optimized executable and isolate
only the changed compiled stage under the startup JCE_SHADER_DEV_DIR overlay.
Require the rebuilt baseline's complete container to match the retained same-tuple
reference, and preserve stage IO/uniform metadata. A present override that fails
must not silently become a successful fallback. Reuse earlier image controls only
when executable, inputs, simulation clock, camera and effective quality still match.
An exact eager-loader Memory SHA/create/link receipt proves CPU acquisition, not
GPU draw use. Keep the short instrumented provenance witness separate from
uninstrumented native whole-frame A/B; do not treat fewer shader instructions or
an FPS cap as a performance gain. Reuse compiled shader-cache entries and bounded
image/trace evidence rather than rebuilding or copying entire application trees.

Shader optimizations need the actual compiled payload and consumed resource
state. A source branch or a smaller instruction count alone proves no saving.
Keep same-capture shader replacement, native execution and cross-backend compile
results separate: exact replay pixels and local event savings do not certify
dynamic image quality, another device or whole-frame performance. Inspect actual
sample counts and final effective configuration; repairing conflicting setting
layers is correctness work, not a same-quality performance comparison.

Tracy collection requires a client and command-line collector with matching
protocols. Inspect an explicit user-supplied tool directory before rebuilding
missing tools. Confirm the actual producer's effective profiling flags: native
`dist` forces Tracy off, while authoring Release and Debug may enable it. A
working capture needs a fresh nonempty `.tracy`, successful export and real zone
rows, not only an open socket. On-demand collection omits earlier events; the
collector duration starts after connection, so keep an external finite owner.
CPU zones and latched bgfx timings do not establish a Tracy GPU timeline without
actual GPU-context instrumentation and observations. Keep diagnostic builds and
optimized throughput controls in separate groups.

Before an all-thread IP sample, use the existing non-suspending
`run_process(sample_threads=True, thread_limit=256)` observations to locate
CPU-consuming TIDs. Compare actual queried counts and truncation with the
sampler's bounded historical roster. A sweep omitting the busy TIDs cannot
explain their CPU use. Retain PID/birth and TID reuse/exit gaps; one live thread
name/creation snapshot does not close those gaps. IP residence is wall time,
so waiting-function samples alone do not prove that a thread burns CPU.

FrameLog context3 keeps optional viewport availability independent. When the
Game panel selects a separate PostFX output, `display_size` can be unavailable
while requested/allocated dimensions, CPU completion, image and submit records
remain valid. Preserve that null; never fill it from the bridge extent. An
explicit hidden capture can validate offscreen CPU rendering without proving
physical display dimensions, Present, GPU completion or visible product FPS.


The editor lifecycle probe accepts a bounded schedule with `--frames`,
`--play-at`, `--stop-at`, `--observe-at` and `--shot-at`; defaults remain the
240-frame restoration witness. `--stop-at` counts PLAYING ticks, not absolute
host frames. Admit the complete plan before output or child creation, and leave
enough time for the project's initialization and camera motion before the
during snapshot and screenshot. Retain actual transform/script witnesses; a
requested later frame alone does not prove motion. Fixed-step hidden diagnostics
are distinct from ordinary input, dynamic visual quality and native FPS controls.

Explicit renderer comparisons can use lifecycle-probe --backend (d3d11, d3d12,
vulkan, opengl), --gpu-adapter (auto, intel, nvidia, amd), and --gpu-tier
(low, medium, high, ultra). Absent selectors preserve prior defaults. Only
explicit allowlisted requests cross the filtered environment and fresh owned
profile/session. Record requests separately from the actual backend, device,
render extent and effective quality; fallback can differ from the request.
A vendor request is not an exact physical-device identity. The same requested
tier alone does not certify equal output or workload across adapters.

Long lifecycle validation can select the existing exact SQLite history owner via
--trace-history-db, --disk-history-max-records and --disk-history-max-bytes.
Supply one fresh absolute database path with an existing parent and explicit
positive record/byte limits. The probe refuses invalid or occupied paths before
launch and forwards the tuple through the same ACK or canonical reader. An
explicit history-only request also validates the trace. Defaults keep the
bounded RAM history. Preserve the original failed run and qualify any offline
revalidation separately; do not rerun the application just to lift a reader cap.
Actual closed-store bytes and observed memory belong in the receipt, not a
claim based solely on the configured ceiling.

Tracy csvexport -e/--self reports exclusive CPU zone time. Keep that label;
a parent self total may be smaller than a child's. Cross-thread totals are CPU
work attribution, not elapsed frame time, GPU time or presentation latency.

Before optional RenderDoc image statistics, retain each completed state's full
byte hashes, binding observations and repeat/restoration comparisons. A numeric
comparison exhausting its observer budget must preserve these earlier facts and
the original trial failure. Do not diagnose a Renderer failure from observer
timeout or reopen the capture merely to recover discarded in-memory statistics.

For RenderDoc constant-buffer inspection, an advertised descriptor range can
exceed the actual backing buffer. Check the offset and backing length, bound
reads by reflection and the declared observer budget, and retain these bounds
before optional reads. Use the reflection-array ordinal where the API requires
it, rather than the binding register. A late short read leaves a partial
observer result; preserve earlier counter/image diagnostics and the original
failure instead of diagnosing an Engine buffer fault from that read alone.

For a rebuilt executable, `JCE_KPI_BACKEND_TIMINGS=1` adds optional
`frame_backend_latched_*` rows to an explicitly requested FrameLog. The
capture owner reads this selector once per session; absent, empty or `0`
preserves the default CSV bytes. Other selectors refuse capture before IO.
The encoder reuses the existing statistics snapshot without another backend
query. `benchmark_suite.read_frames` groups these rows separately under
`backend_latched_ms`; never add them to CPU phases or join them to the current
engine frame. Missing or invalid values remain unavailable. A retained zero
does not establish timer support or the absence of waiting. Rebuild and
validate the actual producer before assigning this capability to an artifact.

Explicit periodic performance logging can consume bounded GPU provenance and
view-name diagnostics independently of continuous tracing, without adding trace
ring events. Verify this on the rebuilt producer. Provider timings are latched;
do not assume groups or views share a source frame or join them to the current
CPU frame. Preserve unavailable data, empty names and truncation/rotation of
bounded name inventories. A Top-8 list is not a complete GPU cost decomposition.
With both observers off, the snapshot admission path adds no statistics getter;
this does not mean the entire Renderer never reads statistics. Keep diagnostic
overhead separate from uninstrumented throughput controls. These records do not
certify GPU completion, presentation, image quality or a performance gain.

For an explicit Engine tail witness, use `perf_bench.run(...,
require_terminal_ack=True)` (CLI `--require-terminal-ack`). The tool supplies a
fresh token and validates the original child's stderr, owner-session ticket and
cleanup scope. Requested tracing requires a new continuous-stream path and an
unchanged complete capture with the matching terminal-session counter. The
acknowledgement follows the owned cleanup return, covering the finite tail that
the exported CPU prefix cannot observe. It does not certify GPU completion,
presentation, durable log writes or unregistered user resources. Default runs
retain their existing behavior; an older executable cannot inherit this witness.

`tools/editor_lifecycle_probe.py --require-terminal-ack` applies the same
owned-child tail validation to its hidden Play/Stop restoration probe. With
`--trace`, it also validates the continuous trace and terminal-session binding;
`--resource-trace` additionally requires resource observations. It reuses the
validated report and artifact hashes. Defaults remain unchanged. Hidden,
forced-focus lifecycle fixtures do not certify ordinary desktop input or
throughput, and a successful CPU tail does not certify GPU retirement/Present.

For Web targets configured through the SDK helper, a fresh
`#jce_terminal_ack=<32 lowercase hexadecimal characters>` fragment explicitly
opts into the same Engine terminal owner before main. The fragment is not part
of the HTTP request. Missing opt-in leaves startup unchanged; malformed,
duplicate or conflicting token ownership refuses startup. Preserve the page's
existing callbacks and console routing. Observe exactly one matching
`JCE_TERMINAL_ACK` after the fixture's actual checks, bound to the admitted HTML
hash and this navigation's token. This is browser console evidence, not an
owned native child/stderr observation: never fabricate process metadata for
`observe_terminal_ack`. Record absent trace binding separately. Neither a zero
canvas size, init OK, `EXITSTATUS=0` nor closing the tab proves the Engine tail,
GPU retirement or complete browser resource reclamation.

Use lifecycle_trace_report.py --renderer-submissions to opt into typed CPU
submission packet validation. The existing streaming/B/E owner checks all four
fields against their active service identity/thread/category, fixed order and
successful closure. Zero/wrap uint32 serials are valid; generation -1 is unknown.
Exact values wider than53 bits are reported as decimal strings. Default legacy
scope analysis does not certify these packets. Even a complete packet stream
does not establish GPU completion, presentation or a join to latched GPU timers.

The optional raw performance-counter fields distinguish the actual backend
API interval from the wider engine renderer tail. Accept their linkage only
through the same native thread and nested submission owner, with equal counter
frequencies and contained edges. Older traces may lack the fields. Keep exact
ticks and bind the process instance before aligning to an OS trace. Scheduler
wake attribution requires switch and ready events. RenderDoc replay timing,
OS GPU engine rates, and a native Present interval answer different questions.

Use three steps: define the observable claim, prove the measurement path ran, then inspect and compare the result. Log output alone does not prove pixels, timing or user interaction.

Hidden-window rendering and renderer-free headless execution are different modes. Rendering validation needs an initialized backend even when no window is shown.

Use separate temporary output directories for captures and traces. Match renderer/backend, viewport, scene, effective settings, frame count and simulation clock between comparisons. A screenshot triggered at a simulation timestamp is different from one driven by wall-clock delay.

JCE_FRAME_DT_FIXED controls deterministic simulation steps; JCE_MAX_FRAMES bounds a run. JCE_WINDOW_HIDDEN hides a rendering window, while JCE_MULTI_INSTANCE avoids an existing editor lock invalidating an isolated capture. Check current source for the meaning of each switch before composing a run.

For real-time throughput, leave `JCE_FRAME_DT_FIXED` unset or empty. The Engine
interprets nonempty `0` as its default fixed step; it is not a disable switch.
Remove `JCE_DBG_FOCUS_GAME` entirely for ordinary Editor input tests: its
presence, including `0`, forces focus. After a viewport toggle, wait for a
fresh observation before retrying; an input call returning is not a rendered
layout acknowledgement. Preserve ineffective clicks and fallback shortcuts.

Active `JCE_KPI_GAME_SHOTS` defaults its capture target to 1280x720 when no
capture size is supplied. Declare image-QA dimensions explicitly. Keep shots
and capture overrides absent in natural-viewport performance runs, and verify
the actual Game target and visible viewport independently.



The reusable tools are tools/envshot.py, tools/visual_diff.py, tools/render_parity.py and tools/perf_bench.py. Read their --help instead of inventing arguments. Inspect the actual captured image and reject empty, stale or unrelated output before comparing metrics.

Bind a delivery receipt to the actual artifact SHA-256 and byte size before
using its version, contents or source context. An older adjacent manifest can
survive repackaging; retain it as stale evidence rather than treating its
filename as authority. Native single-EXE editor receipts are under
build/reports/delivery, separate from the shipped EXE. A packaging source_commit
is packaging context, not proof of the compiled source; retain build input
hashes, the actual build log and executable/SDK version observations.

Use elapsed wall-clock measurements for playback/seek claims. For media, observe displayed video time, audio progression, pause/resume and seek completion; advancing the clock while the matching frame is unavailable is not synchronization.

For a new gate, include a failing control. A check that never sees its target, silently skips or exhausts machine resources provides no PASS evidence. Report real failures, unavailable subjects and resource failures distinctly.

## Core-5 editor and Player benchmark

Read tools/BENCHMARK.md and run `python tools/benchmark_suite.py --help`.
Select `--host editor` or `--host player` explicitly with `--exe` pointing to the
corresponding artifact. Preserve actual backend and executable SHA-256; keep
editor and Player measurements in separate groups. Shared workloads use authored
camera and fixed simulation steps. Captures use separate witness processes;
steady metrics use the full wall-clock CSV, not the live UI ring.
Use `--profile publication` for seven repeats and `--compare-backend` for paired
backend A/B. A `--profile soak --soak-seconds 600` run is short diagnostic evidence;
it does not replace the protocol's final `--profile soak --soak-seconds 1800` run.
Keep exported controls, manifests and raw samples in ignored reports/.
Do not stop a concurrent user editor without authorization or label concurrent
measurements exclusive. Ambient stability observations are permitted when their
contention and scope limits are explicit. External-engine, subsystem and visual
fidelity readiness require separate evidence; absent measurements stay unavailable.

For explicitly requested full-frame recording, `JCE_KPI_WRITER_QUEUE=bytes,records`
selects a bounded payload queue and descriptor count before file IO/allocation.
Unset or empty preserves normal 1 MiB/1024 and LOW_MEMORY 256 KiB/256 defaults.
Both fields require positive ASCII decimal values, at most 64 MiB/65536; malformed
selectors refuse capture. For example, `33554432,16384` accommodates the observed
8-second write interval when budgeting for 700 x 3617-byte records/second,
not a measured 700 FPS result; it does not fix IO latency
or guarantee arbitrary stalls. Descriptors and reusable encoder scratch are
separate allocations, so payload capacity is not total capture memory. Declare
the same effective budgets for every arm and retain peak occupancy, failures and
complete writer/terminal accounting. Never disable strict FULL or certify an
unwritten tail to obtain a performance number.

Frame CSV export is batched and its `kpi_export` phase remains inside wall-clock
measurements. Default durability flushes occur at startup/shutdown. Use the
explicit `JCE_KPI_DURABLE_FLUSH_INTERVAL` only for a declared diagnostic control;
recurring storage synchronization must not masquerade as rendering cost.

For an explicit uncapped control, `JCE_LAUNCH_VSYNC=0` and
`JCE_LAUNCH_TARGET_FPS=0` request session-sticky overrides in the shared Engine
owners. Vsync accepts exactly0/1; FPS accepts unsigned decimal through INT_MAX.
Present empty, signed, fractional, whitespace or overflow values refuse startup.
Absence preserves ordinary project/Editor policy. Check the rebuilt executable's
launch-policy records after app initialization and before destruction; an
environment request alone is not effective-setting evidence. The FPS getter is
the Engine rate owner; Renderer reset flags are CPU state, not a backend or
display acknowledgement. Noop/fallback presentation stays unavailable. These
controls preserve visibility, focus, background admission and scene quality.

For natural Editor throughput, explicitly set
`JCE_KPI_FRAME_LOG_VIEWPORT=natural`. It removes only FrameLog's request for
otherwise clipped or unfocused viewport work; the existing writer and normal
viewport gates remain. Unset, empty, `capture` and unsupported values retain
legacy capture workload; unsupported values are not natural evidence. Active
Game capture and DrawLog still request their existing work. This selector does
not change Replay/Shot admission, VSync, caps, rendering quality or simulation dt.
Retain an observer-off control and actual viewport extents alongside the CSV.

Requested window dimensions, cached viewport requests and PNG dimensions are
different observations. Offscreen targets may clamp a request; use the target
owner's committed extent for that source. A post-processing output has its own
extent. Retain the producing frame and hidden/failed-prepare state; reading
current state while exporting the previous completed interval can mislabel it.
Existing phase durations and latched backend statistics alone do not establish
continuous foreground state or same-frame render geometry.
Game Stats labels UI cadence separately and reports admitted content versus
committed 3D target extents with X/Y ratios. Missing, stale or failed current
attempts remain unavailable; CPU completion and target metadata do not certify
GPU execution or presentation.

Memory logs distinguish unavailable data from measured zero. Preserve the raw
token, provider and unit: MiB-rounded known zero remains zero; a legacy zero
that could have come from clamping an unavailable value remains ambiguous.
Unavailable RSS cannot certify a memory ceiling. Process working set/private
commit, adapter-reported memory and tracked texture/target bytes have different
ownership scopes and must not be summed into a claimed allocation inventory.

An uncapped 400/600 FPS target means 2.5/1.67 ms completed frame intervals for
the explicitly declared hardware, dimensions and workload. Screen refresh,
presentation cadence, CPU work and delayed GPU durations remain distinct.
Remove both authored caps and VSync through their existing owners, verify the
actual reset/adapter/extent and retain P95/P99, spikes and sustained coverage.
Neither C/C++ nor a requested cap establishes that target. Keep normal idle
power policy separate from the throughput protocol.
Keep every spike and require complete raw samples plus clean process exit.
Predeclare paired-run drift and geometry admission limits; control warmup and
run gaps. Retain contaminated or wrong-geometry runs as failed, without trimming
them into acceptance. Budget UI setup by actual wall time and throughput,
including launcher/terminal waits, so setup finishes before the declared window.
Without their observations, do not attribute drift to hardware clocks or power state.

## Media picture and seek controls

Match native output PTS with an independent decoder before comparing pictures.
Declare matrix, signal range and chroma reconstruction. Different RGB resamplers
are different reference policies; rerun both before/after controls with the same
policy rather than relaxing a threshold. Retain decoded YUV and use a standard
floating-point inverse when validating fixed-point nearest-chroma CPU kernels.
Odd crop sizes need ceil-rounded 4:2:0 planes, not truncated last rows/columns.

Keep the transport pinned until the current-generation picture and PCM are
ready. Retire a streaming voice before a discontinuous seek and create a fresh
one after readiness; pausing alone preserves source/resampler prebuffering.
Measure actual post-seek picture, transport and consumed-PCM progression.
Consumed PCM is not acoustic speaker latency, and CPU publication is not GPU
upload or editor presentation. Do not infer unmeasured stages from these probes.

### Interactive picture and clock evidence

Camera variable traces alone do not prove the rendered view moved. Compare actual post-input images and picking projections, including the Editor normal event bridge. `JCE_KPI_GAME_EVENTS` is an explicit normalized synthetic QA producer through the normal editor event processor and live viewport gates; never call it physical mouse testing. Frozen WebGL probes must wait for requestAnimationFrame presentation before browser screenshot submission; save only after the rendered picture is visible. Input-image comparisons declare diagnostic overlay exclusions. Check build-card animation across selection changes and consecutive first-placement frames. Performance runs use real dt, no readback/PNG encoding, declared warmup/sample counts and independent repeats; retain CPU/GPU availability, raw spikes, host identities, OS memory samples and competing processes. Different frame rates in a per-frame original change wall-time trajectories and must be reported.

The Editor snapshots these fixture options once per GUI host initialization:

| Option | Meaning |
| --- | --- |
| `JCE_KPI_GAME_EVENTS` | Text file read once, capped at 1 MiB. Each whitespace-separated row is `position M/B/W/K x y a b`, with an integral position in 0..UINT_MAX. Rows are consumed in file order once due. |
| `JCE_KPI_GAME_EVENTS_CLOCK` | Absent, empty or `frame` preserves the visible-Playing frame counter. Explicit `elapsed_ms` interprets position as monotonic elapsed milliseconds, independent of simulation dt and presentation rate. Other values refuse the fixture. |
| `JCE_KPI_GAME_EVENTS_START` | Optional for `elapsed_ms` only: a fully qualified absolute host path, at most 4095 bytes, initially absent. A newly observed regular file activates the elapsed origin; file contents are ignored, so an empty file is sufficient. |

M uses normalized x/y position and a/b displacement in the current displayed
viewport; B uses normalized x/y and button/down in a/b; W uses vertical steps
in a; K uses scancode/down in a/b and ignores x/y. The shared decoder retains
finite-value and integer-range checks. Pointer rows use the existing virtual
Game bridge and ordinary pre-step runtime commit; keys use the ordinary Editor
event processor. `JCE_INPUT_REPLAY` and its JIRC CPU-tick format are unchanged.

Elapsed mode requires Playing, a visible valid runtime-owned viewport and its
current runtime. Without a marker it starts on the first admitted observation.
With a marker, the path must be absent both at initialization and at the first
eligible admitted poll. Waiting polls run at most 10 Hz, retaining the poll gate
across invalid admission; returning to valid admission may wait for the next
eligible poll. Start time is the poll that observes the file, not its creation
timestamp. Finish Play/layout/readiness
preparation while the marker is absent, then publish a fresh file. Docked-to-Max
geometry changes before activation are allowed. A file or directory already
present at initialization, or a marker already present at the first eligible
poll on initial admission or after an observed invalid interval, refuses
activation.

After activation, Stop/Pause, hidden or invalid viewport, runtime/owner change
or any display/render geometry change cancels the remaining rows and clears
virtual state; restoring admission does not restart the file. Elapsed-only
active validation also runs before the ordinary pre-step commit. Cancellation
observes normal host update/draw boundaries and the existing runtime pointer;
there is no Play generation. A Stop-to-Play cycle that reuses the same pointer
entirely between observations is outside this evidence. The public
performance counter/frequency owns elapsed time: equal readings are legal;
zero frequency or an observed backwards reading refuses the fixture. Normal
EOF/error clearing remains deferred until the next draw so the last row reaches
its ordinary runtime commit. Invalid options refuse only the fixture. The
default frame path performs no added clock reads or marker polls. Source and
clock records identify synthetic input; scheduler/bridge CPU controls do not
certify physical mouse motion, native timing, GPU completion or rendered motion.

## Diagnostic SDK configuration and readiness

Strict performance probes that parse actual INFO records require a RELEASE SDK. DIST compiles INFO logging out; its compact single-application smoke remains valid delivery evidence, but it cannot supply those diagnostic records. Use the embedded delivery profile unless optional external-VM authoring behavior is explicitly under test:

```bash
python scripts/jce.py sdk --variant release --delivery-profile embedded --profiling off --patented-codecs off --no-debug
```

Keep the delivered DIST and diagnostic RELEASE artifacts and their hashes separate. Do not weaken performance validation or change production logging policy to force a missing-record run to pass. A registered native ABI is not proof that project script classes are embedded.

For asynchronous model loading, wait for actual readiness within an explicit bounded timeout. Warm up for 120 fixed-dt frames or at least 3 seconds of actual continuous realtime frame intervals, then collect at least 600 measured frames for each declared repeat. A realtime frame count alone does not establish elapsed warmup; discard startup delays separately and rederive the bounded warmup and measured boundaries from raw CSV. Import/setup latency and timeout outcomes are separate from steady-state performance; preserve them instead of treating an early readiness probe or a missing performance line as success.

Readiness of a cached model does not prove that the authored scene renders it. Validate actual scene membership, the selected mesh path and transform, and the active camera pose/projection through public SDK observations. Stock hosts expose owner-thread borrowed observations through jce_default_main_get_scene, jce_default_main_get_camera and jce_default_main_get_scene_renderer; never retain or destroy them across shutdown. jce_camera_get_fov returns degrees. Keep actual submitted topology and independent picture controls distinct from cache census.

Modern recreation may intentionally replace frame-dependent source behavior. Distinguish legacy reference captures from normal time-based gameplay; measure accepted simulation time, dropped suspension time and remainder, and verify multiple presentation rates with the same inputs. Do not require unbounded wall-time catch-up after stalls.

Keep an uninstrumented frame-rate control beside GPU query and driver traces.
An elapsed query around a whole callback includes command gaps; it is not an
isolated shader duration. Record per-draw scope and query availability separately.
Exclude other graphical applications and CPU builds from acceptance timing.
For graphics liveness, require valid targets, actual submissions and visible
pixels; phase markers or an absence of errors alone do not prove an effect ran.

Graphics probes must preserve the return value and attributes of wrapped context-creation APIs. Record the actual context version and device capability table in each run, including instrumented negative controls; a separate uninstrumented context is not proof of the probe context.

For GPU teardown, inspect actual live objects before closing the test page.
Removing an Emscripten context record does not prove WebGL objects were deleted.
Preserve getContext return values and use program/shader validity plus deletion
records to identify survivors; correlate shader source with its first-party owner.
# Whole-scene lifetime coverage

An effect-only probe cannot certify scene resource teardown. Also run a real
scene containing imported models, resolved material textures, lighting and UI.
Before closing the browser page, count actual GPU creations and deletions and
check remaining object validity. Record leaks even after the Emscripten context
table is empty. Verify shared material/light bindings across device recreation,
and distinguish runtime-owned texture cache uploads from callback-borrowed
textures. Keep failing raw evidence and bind each result to the tested artifact.

Renderer lifecycle CPU spans use the core bounded service scope for attempted
creation, view configuration, CPU command submission and teardown in every host.
Null frame/cleanup calls retain cancellation; failed creation retains failure.
Compile the production adapter with explicit call seams to verify forwarding,
balanced spans and parent restoration. CPU spans do not certify GPU execution,
presentation or memory reclamation. Diagnose WebGL cost using linked-program
identity and recorded call arguments/payload sizes; triangle/index counts alone
cannot classify UI/world draws. Suppressed draw diagnostics alter the workload
and never qualify as production performance measurements.

Process residency probes use tools/memory_residency_probe.py and the existing
sampler with an explicit workload environment and covered warmup/window. Keep
RSS/private commit, allocator availability and voluntary tagged accounting
separate. The aggregate tagged peak is simultaneous; the legacy total peak
sums individual tag peaks. Neither proves complete heap ownership or the
512 MiB physical-device target. Retain raw samples and tested artifact identity.

Continuous CPU lifecycle capture is opt-in through JCE_TRACE_STREAM=<output.json>.
Set JCE_TRACE_STREAM_MODE to threaded or cooperative only when explicitly testing
that consumer; the native default uses a worker and Web defaults to cooperative
pumping. Set environment variables before process creation. Capture IO suppresses
its own trace events per thread, without disabling other producers. Stop only
after application producers have joined, then drain the consumer and inspect its
footer. Ring capacity alone never proves that the full application was captured.

Validate the saved JSON with tools/lifecycle_trace_report.py and an explicit
--out path. Repeat `--require NAME` for actual submitted, self-owned SERVICE
tasks (`cat=task`, submitted `kind=2`, scope `id == parent`). Each required
SERVICE must complete successfully; matching failures or cancellations reject
the capture. Repeat `--require-wait NAME` for an independently required complete
`cat=wait` B/E pair. Both controls use the same full event validator. CPU wait
completion cannot satisfy a SERVICE requirement or prove successful target work
or GPU completion. Declare service-name queries and requirements before capture;
category totals do not prove per-service coverage. After sealing, leave an
unqueried owner's coverage unavailable rather than rescan raw evidence.

Require zero loss/unexported records, a stable generation, successful writes,
matched owner spans and no
active threads/work/waits. Keep failed nonrequired IO attempts as diagnostic
outcomes; required phases must succeed. Compare performance with tracing off,
and retain trace-enabled coverage runs separately. CPU lifecycle evidence does
not prove GPU completion, presentation or reclaimed device memory.

The CLI validates the complete UTF-8 JSON event stream and footer with bounded
state, then reads the entire file again to compare SHA-256 and file identity.
Its report contains counts and bounded first/last witnesses, not every scope or
counter value. This output sampling never skips validation. The in-memory
`analyze_trace` API keeps its existing complete lists. `--max-history` (default
200000), `--max-active` (8192) and `--max-names` (4096) are explicit validation
budgets; exceeding one is INVALID, never a prefix PASS. Invalid input retains
its full byte hash where readable, while `json_utf8_eof_validated=false` records
an interrupted syntax check. Stable fingerprints are not an atomic snapshot.

In bounded-stream mode, `nonrequired_task_outcomes.aggregates` groups every
failed or cancelled diagnostic scope by canonical `(name, state)`, retaining
count, first/last endpoint witnesses and finite timing sum/min/max rather than
an unbounded event list. First/last refer to export order, not a cross-thread
happens-before claim. At most two outcome rows use each name admitted by the
existing name budget; history, pairing, required-success and complete EOF
validation remain strict. The in-memory API retains complete diagnostic scope
lists. A service label alone cannot identify a failed file address, failure
stage or optionality when the producer did not record that metadata.

The generic editor lifecycle probe accepts optional `--trace`:
`python tools/editor_lifecycle_probe.py --exe <editor> --scene <scene-or-bundle>
--out <ignored-output> --trace`. The default stays off. It stores and hashes
`lifecycle-trace.json` in a unique output root, using the native default consumer.
Play/Stop restoration success does not replace explicit phase validation with
`tools/lifecycle_trace_report.py`; choose required phases from the actual host
path. Trace-enabled coverage is separate from GPU and performance acceptance.

Lifecycle reports preserve submitted task parents separately from scope owners.
An owned wait may have a different label from its SERVICE owner; its endpoints
still require matching identity, thread and owner. Required self-owned SERVICE
phases retain strict submitted-name and successful-outcome checks.

For explicit uninstrumented controls, set `JCE_TRACE=0` and clear both
`JCE_TRACE_STREAM` and `JCE_TRACE_EXPORT`. Explicit trace-off overrides automatic
tracing from `JCE_PERF_LOG` or Debug defaults; nonempty capture paths still
request capture. Boolean flags require a nonempty value whose first character
is not `0`; `JCE_PERF_LOG=0` disables optional frame telemetry. KPI export costs
remain separate. Check effective settings in the actual artifact.

For a single cooperative Web capture, preserve the original `tid=0` rather than
inventing an OS thread ID. Validation requires continuous cooperative mode, no
worker consumer, one unambiguous thread name and the same zero identity on every
event. Threaded, mixed or unnamed zero identities remain invalid. Scope pairing,
required success, exact counts and zero-loss rules stay strict.

Renderer CPU view evidence must follow the actual host path: standalone 3D setup
and offscreen keep/overlay setup both emit renderer.view_configure. A delegating
full prepare records once; outcomes follow the real result and scope parent is
restored. Noop API controls verify ownership but actual graphical host captures
remain required. Naming one wrapper never establishes unexecuted path coverage.

Screenshot completion belongs to the existing renderer callback and bounded PNG
writer. `jce_renderer_screenshot_poll()` and `jce_screenshot_poll()` are
non-consuming latest-file-request queries: -1 idle, 0 pending, 1 encoded and
written, 2 failed. Terminal results remain until the next accepted request;
main-thread callers must coordinate one owner. Pending includes actual encoding
and file writing, not only pixel delivery or queue admission. A busy request is
rejected; invalid arguments do not replace the previous result. The stock
schedule propagates real failure. The legacy texture-PNG poll consumes its
terminal result once and only accepts RGBA16F sources with BLIT/READBACK support;
use the existing typed render-readback API for other formats. Noop has no file
capture capability. Shutdown drains CPU writes, rejects late callbacks and
retains GPU-borrowed CPU storage until the backend stops; this is not durable
filesystem sync or evidence of GPU idle. Test blocked writes, real write failure,
late identity, cancellation and backend-stop lifetime. Keep image stability and
independent visual comparisons separate from successful file completion.

Classic Blinn-Phong shininess uses one finite-domain lobe power: exponent zero
returns one, zero base with a positive exponent returns zero, and positive
inputs keep the original pow. Apply the same boundary to all shininess lobes;
keep fixed-exponent Fresnel powers unchanged. This does not repair non-finite
input vectors. Compile HLSL, SPIR-V, GLSL, ESSL and Metal through the existing
shader recipe, but report that as portability only. GPU numeric validation
remains NOT_RUN until measured; Metal compilation is not an Apple device test.

Establish a production NaN cause from its actual RenderDoc event and shader
output, with artifact identity and render/depth state. Black PNG pixels or a
CPU formula cannot establish that cause. Require actual finite shaderOut in
the corrected GPU run before declaring the boundary fixed. Preserve original
reference captures and visual differences caused by undefined pow(0,0); never
pick a random black result as parity, alter expected pictures or relax the
existing comparison threshold to conceal that difference.

Render-pipeline quality evidence separates the requested material look from
the active postfx cost budget. Requested MID/HIGH/ULTRA honors inexpensive
hemisphere/wrap/rim lighting even when GLES or hardware clamps active bloom
quality to LOW; an explicit requested LOW stays neutral. Use the existing
requested/active/pending owner. Deferred edits retain user intent, clear their
snapshot before the single apply implementation, and preserve observer-queued
work. A complete apply replaces older pending edits; tier re-resolution keeps
them. Boolean JCE_RP_FORCE=stylized_look=0/1 remains highest priority; numeric
JCE_RP_FORCE=post_quality is not an implemented force knob.

Record the mounted RP asset identity, parsed requested post_quality, active
budget and actual feature query. Scene rendering JSON describes authored values,
not effective GPU quality. Verify the actually drawn program and submitted
CB/GL uniforms; a LOW active bloom budget alone does not establish a neutral
material gate. Keep previous captures before restoring source lighting and
rerun uninstrumented visual/performance acceptance on the new artifact.

Focused CPU evidence: 13 production-path controls had nine real before failures
and then all 13 passed, alongside three existing tier controls. A further
full-apply replacement control failed against that intermediate implementation;
the final 14 controls and three tier controls pass. This does not certify the
new candidate's GPU uniforms, pixels or performance; those remain NOT_RUN until
actually measured.

Use a separate actual linked-program and submitted-uniform observation before timing. Bind the complete observed shader bytes to the reviewed installed SDK payload, not only a function name or source prefix; unknown lowering requires review. Retain failed or stale quality evidence and reject promotion by status changes. Keep synthetic approved-source identities confined to explicit test mocks. During timing, remove draw/readback instrumentation and match the actual GPU/context to the prior quality witness.

JCE_SHADER_DEV_DIR is a startup snapshot. The Engine injects it into the admitted
Renderer; grass/grass_shadow and foliage families use the existing per-stage
overlay authority. Missing stages follow PAK/embedded fallback; a present invalid
stage rejects. Restart consumes a changed root; the existing standard Reload
command has separate cache coverage. Keep explicit PAK loader semantics and
program ownership intact. Use absolute roots for portable comparisons when a
host may change CWD, and prove the selected payload from the actual bound shader.
An environment value or successful source compilation alone is insufficient.

Distinguish throughput from presentation: throughput requires VSync and frame limiter disabled, fixed resolution and actual settings observations. Requested settings alone are intent. Presentation runs retain the observed pacing and never imply uncapped throughput. Summarize independent repetitions with the median of run medians, median p95/p99 and spread; retain CPU/GPU valid sample counts, unavailable stages and every original spike.

Final presentation comparisons use seven independent repeats per host, at least 3 seconds of actual warmup and at least 600 continuous measured intervals. Bound slow sample windows and retain partial raw arrays on timeout; tagged shorter protocols remain diagnostic. Distinguish declared window labels and input viewport sizes from resolved Game FBO extents. A capture size without an active shot schedule does not size the normal Editor Game target. If an actual target extent is unavailable, retain that absence and treat hosts as separate workloads; do not infer an equal-surface comparison.

Required counter controls are explicit: `--require-counter-zero NAME` requires
every matching C value to be zero; `--require-counter-last-zero NAME` requires
the last matching value in exported stream order to be zero. Missing counters,
boolean/string/null values and nonfinite numbers fail. Preserve raw indices and
values. Do not infer these requirements from a generic CPU footer. For resource
coverage, also match allocation/token/context identities, transfers/releases or
cancellations, and handle destruction; require zero metadata-unavailable and
origin-capture gaps. Scoped counter IDs need not be zero. Backend ownership
callbacks and balanced CPU ledgers do not prove GPU completion or physical memory
reclamation. Keep tracing disabled in performance controls.

Validate common instance-owner resource identity separately with
`python tools/instance_upload_trace_report.py <trace.json> --out <report.json>`.
For a workload expected to exercise dynamic fallback, add `--require-dynamic`;
a transient-only observation must remain `NOT_EXERCISED` for that route. Match
token/allocation generations and submission epochs, committed slices, bindings,
retirement/destruction and staging transfer/release or cancellation. Do not
promote a balanced footer or zero aggregate counters into identity closure.
Report the captured subset, metadata gaps and unexercised routes explicitly;
this ledger is not an all-renderer, GPU-completion or physical-memory certificate.

## Live observation of diagnostic CSV files

On the current Windows SDL backend, a writable file uses exclusive sharing.
An external live CSV reader can therefore receive a sharing violation until the
writer closes. `JCE_KPI_DURABLE_FLUSH_INTERVAL` changes storage durability; it
does not grant concurrent read access. A missing live marker stays unavailable
and does not prove that rendering stopped. Validate complete CSV intervals and
clock accounting after clean process exit. For frame-sensitive resize or capture
controls, use actual completed-frame signals and verify the owned window and
resulting target dimensions. File existence, a header and shutdown-only rows do
not prove live frame progress. Preserve failed observation attempts and keep
upstream file-sharing implementations unchanged.

## Clock and zero-duration sampling

Durations use the public jce_time_perf_* monotonic provider and its matching
frequency. Preserve actual native/browser resolution evidence; nanosecond units
do not imply nanosecond precision, and browser privacy may coarsen readings.
Epoch time is a separate realtime domain. A present finite zero-duration CPU row
is a valid sample. Require phase membership independently from aggregate workload
liveness; never infer missing data from zero or fill missing rows with zero.
Provider seam controls do not certify the target: also run the installed-SDK
public clock probe on the actual platform and retain its raw output and artifact
identity. Retain before failures and rerun changed producers and consumers.

## Completed intervals and backend observations

Match the completed wall interval to its CPU snapshot at the same engine boundary.
An ever-available snapshot is not proof of freshness. Initial, skipped or disabled
measurement intervals remain ineligible for CPU attribution; preserve missing rows
instead of synthesizing zero. A sampled finite zero wall or phase value remains
valid. Keep the original wall distribution, every spike and the exported row count;
do not remove slow walls because their phase attribution is unavailable. Rendering
boundary exports do not include every paused main-loop iteration. Label any active
subset separately and preserve its exclusions and full capture scope.

Raw capture omits initialization and records each completed render once. Normal
iteration entry captures the previous render; terminal completion captures the
final current render before writer finish. Quit between iterations and explicit
host destruction preserve pending completed work. Verify contiguous raw ordinals,
actual final rendered count and native clock coverage together. A footer proving
all accepted records were written does not prove the whole timed run was accepted;
retain partial-window diagnostics and failures. Headless or incomplete iterations
do not supply rendered rows. Keep explicit capture-limit prefixes separate.

The application-private reporter resets its rolling history, window and log budget
per engine session. Its sequence advances independently of automatic quit limits.
Capture owned phase names from all registered slots; later snapshot rotations must
not change a captured report. Reject callback reentry before changing the outer
clock or snapshot. Default SDL/JNI event pumping is outside engine iteration, so
ordinary external modal service retains the existing renderer guard. A recursive
user callback is not a safe modal checkpoint. CPU guard tests are not an actual
window-drag, fullscreen or presentation test.

`renderer_frame_advance_cpu` and `renderer.frame_advance_cpu` describe the main-thread
backend frame-advance call only. They are nested within the broader frame submission
interval; do not add them to `end_frame`. They may include synchronization or OS
scheduling and do not isolate GPU execution or presentation latency.

`wait_submit` is the render thread waiting for API submissions; `wait_render`
is the API thread waiting for the render-thread handoff. Neither is GPU or
Present time. Published backend fields may come from different buffered frames;
do not force a uniform one-row offset or add them to nested callback durations.


Benchmark SDK observations identify the consumer update sequence and CPU read clock.
Backend CPU/GPU/wait counters are latest asynchronous observations, with no observed
GPU frame join. A successful stats getter does not establish timer support; retain
finite zero, nonfinite, negative and unavailable states distinctly. Query actual
fullscreen through the public window API; drawable/logical dimensions are separate.

Periodic `perf` keeps its legacy parser fields, but `cpu_scope=bgfx_frame_interval`
is the backend wall interval and can include prior engine pacing;
`gpu_scope=latched_backend` is a returned asynchronous observation. Neither is
OS process execution or a joined CPU/GPU frame. `app_update` is the compatibility
update+draw aggregate. `perf-pacing` reports only `scope=last_completed_interval`
with its ordinal, target FPS, planned/actual wait and clock validity; it is not
the periodic window average or proof that a backend timer refers to that frame.
The actual valid-clock pacing branch also emits a balanced `cat=wait` span under
the existing SERVICE parent. Require it with `--require-wait engine.frame_pacing`
only in a capture that actually waited; uncapped, first, overrun, expired-deadline,
invalid-clock and trace-disabled branches have no WAIT event. Typed owner controls
and an omitted-end assertion mutant do not certify native scheduling or GPU work.
Query supported current window bits with `jce_window_get_flags`; retain its
success separately from a valid zero mask. A diagnostic can request
`JCE_WINDOW_NOT_FOCUSABLE=1` before creation, with `JCE_MULTI_INSTANCE=1` for an
isolated host. Verify the observed nonfocusable bit and absent input-focus bit.
Platform flags do not certify OS foreground ownership, physical visibility or
presentation. Keep visible and hidden controls as separate workloads.
Requested fullscreen, hidden or focus settings are intent. Keep unavailable actual
hidden/focus state explicit rather than deriving it from environment variables.

The performance runner requires periodic PERF output by default. A deliberately
disabled periodic-telemetry control can use require_perf_log=False only with
explicit JCE_PERF_LOG=0, fresh raw CPU CSV and exact writer count/byte completion.
Missing periodic metrics remain unavailable. Full raw admission preserves finite
zero wall intervals through the shared parser's explicit allow_zero_wall option;
negative, nonfinite, duplicate or missing samples still fail. PERF_LOG changes
combined telemetry policy, including optional query requests and hitch/log work.
An observed timing difference alone does not isolate GPU timer causation.

The optional private instruction-pointer sampler retains its state until a
confirmed collector join and resolution of its acquired suspends. A signaled
held thread handle can confirm termination; an exit-time field alone cannot.
Aggregate PC counts lack per-hitch timestamps and call stacks. CPU fake-native
ownership controls do not certify real host-thread recovery or bounded native
suspend/context, symbol lookup and report IO.

Complete writer accounting certifies the declared raw capture window. A timed
limit marker alone does not establish whole-duration CSV coverage; matched
controls must validate that horizon separately. Invalid raw-mode parameters
are rejected before deleting prior evidence, even when environment values
would shadow them.

JCE_MAX_SECONDS begins at service initialization and includes later app init;
completed-render CSV starts after initialization. Never equate those windows,
move the timer origin, drop frame zero or add asynchronous frame_cpu to repair
a horizon. Explicit timed+KPI capture emits one private `frame KPI clock v1`
witness with the same engine monotonic frequency/counter: timer_start/check,
render_start/end, summed render ticks/intervals, omitted ticks, valid/errors.
Require actual completed writer count/bytes and all raw rows as well. Invalid,
duplicate or missing witnesses, counter/frequency/ordinal discontinuities and
omitted render intervals cannot certify a continuous window. Historical captures
without this witness stay unavailable; do not relabel them after a helper fix.

Verify the application deadline against actual timer_checked-timer_start and
report the observed rendered length separately. Join frame_wall to render_ticks
using the encoder's %.4f milliseconds half-unit per retained row, not a relaxed
horizon tolerance. Initialization prefix and terminal tail remain explicit;
no exact OS/trace clock alignment or presentation FPS is implied. CPU declared
input and actual-header runtime mutants establish their own protocol/ownership
controls only; fresh SDK/editor execution must establish the production wire.

A zero monotonic counter boundary is valid. The actual wall wrapper and iterate
dt path share private ordered-difference arithmetic; never infer initialization
from counter!=0. Frequency/order admission is separate from completed-render
eligibility, so admitting zero does not create an initialization frame. Arithmetic
and old-zero-guard CPU controls do not establish physical timer or GPU validity.

Explicit `JCE_BGFX_ALLOC_TRACE` observes the existing backend allocator with a
fixed private address ledger. Exported `{backend_epoch, allocation_id}` and
revision distinguish resize and address reuse; addresses and payloads are not
exported. Requests no longer stop at a fixed event prefix. Check matched
transitions, terminal quiescence, ledger capacity/identity/unknown-input flags
and event-history gaps together with the existing capture loss/footer, rather
than accepting zero live bytes alone. A paused capture can retain correct
internal pairing while its exported history is incomplete. Requested-byte
closure is neither OS-residency closure nor GPU completion.

Renderer font-source scope acceptance needs an exercised `jce_font_open*` path.
ImGui and RmlUI retain their own font owners; empty UIText and sprite labels do
not exercise `font.blob.acquire/release`. A complete capture with no such events
remains NOT_OBSERVED. Reuse a real text consumer rather than weaken required-phase
validation or synthesize font calls for acceptance. Verify shared-source identity,
reference transitions, last release and the common capture footer together.
Avoided retained raw bytes are computed from observed simultaneous references and
unique source sizes; they do not establish RSS, IO or GPU savings.

Process RSS growth cannot be attributed from private-commit differences alone.
Working-set membership, subsequent region type and mapped-file lookups are
separate non-atomic OS observations. Preserve unknown/decommitted pages, lookup
errors and collection windows; never label opaque mappings as a driver allocation
or claim exact native-phase alignment from an OS time sample.

`tools/memory_commit_probe.py --mapped-file-names` optionally names at most 32
MEM_MAPPED allocation groups after a complete address walk. It reuses the same
identity-checked handle, keeps original device-form names, and reports omitted,
failed or truncated queries separately from walk totals/status. The existing
QUERY_INFORMATION | SYNCHRONIZE access mask suffices; it does not read process
contents. A fixed 32768-character buffer and 0.5-second inter-call budget bound
work, but do not interrupt an entered OS call. Missing names cannot imply an
anonymous heap or GPU allocation. API unavailability and actual Win32 errors
remain explicit. See Microsoft's [GetMappedFileNameW contract](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmappedfilenamew).
Pure Python counted-port controls validate protocol behavior, not actual live
mapping ownership, atomic residency or physical-device memory budgets.

Actual GUI acceptance must name the operated workflow, executable hash and
observed result. Menu visibility, typed CPU ports and complete CTest domain
labels do not establish every editor function's reachability or consumption.
Use isolated profiles/projects, refresh window/pixel state after focus or size
changes, and retain rejected actions instead of guessing their effect.

A full native PASS applies to that configured registry. Extract unavailable
suites from the same configure log and ignored/early-return branches from its
copied LastTest.log; do not turn their containing CTest PASS into feature proof.
Keep engine/editor/consumer and unit/integration/contract counts distinct.
Actual Test Runner GUI acceptance records the selected profile's child logs for
PASS, explicit exit failure, observed-deadline TIMEOUT and user CANCELLED, with
literal argv and child identity. Preparation hashes are not execution hashes;
a later atomic package or test relink starts a different artifact cohort.
Tool focus guards or stale pixels remain inconclusive until native focus and
fresh pixels agree. A paused viewport or unanimated single step is not measured
simulation time, and live pixel observations without saved images must say so.

Long interactive captures are separate pressure controls. A clean teardown and
an observer's local all-released flag do not repair global cursor loss. Require
recorded/captured/lost and capture_complete together; preserve a large or slow
raw export as failed evidence. Do not expand the ring, omit phases or subtract
instrumentation work to turn an incomplete trace into complete history.

TAA request synchronization and target-format eligibility precede projection
jitter. CPU/Noop controls can establish first-frame policy, clean rejected
projections and preserved overrides; they cannot establish shader/input/target
admission, temporal image fidelity or physical GPU completion.

TAA acceptance covers sustained camera rotation/translation, objects moving
then stopping, wind-driven and camera-facing vegetation, occlusion/reveal and
viewport resume. All viewports in one host frame consume generation-consistent
previous snapshots. A verified resolve-on marker observes CPU submission; it
does not establish velocity coverage or temporal image quality. Keep that
witness, latched GPU-view timing and GPU completion/presentation separate. If
temporal quality fails, explicitly select and validate a spatial-AA default and
document its quality/performance tradeoff; it cannot inherit a TAA acceptance.

Serialized multi-host workflows must acknowledge actual application with the
same request identity. A void message-send port is not an activation receipt.
Distinguish unavailable native/AI providers from explicitly named offline
presets even when both share the same UI and acknowledgement protocol.

A trace formal output is a publication transaction: an exclusively created invalid
PENDING reservation is not a valid capture; a sibling partial is diagnostic evidence
only. Require complete event accounting, successful footer write/close and the
owner's publication before accepting the formal JSON. Keep rejected reservations
and partials, and do not use path-stat failure as proof of absence.

The user-referenced Cherno gist is design reference material, not authority to
change JCE's stack, create repositories or commit/push. Its single export and
feature-scene/testing ideas fit the existing engine/editor separation. Record
feature owners, exercised paths and missing runtime coverage instead of accepting
a list of components as complete workflow validation.

PresentMon presentation observations are separate from the engine frame loop and
CPU trace. Pin the official tool hash, exact process and swapchain, raw units,
capture window and display availability. Present API waiting, GPU event durations
and OS memory membership represent different costs; do not equate them or infer
original-game parity from a different example's isolated pilot.

Use tools/presentation_report.py for explicitly selected, pinned PresentMon
v2.6.0 --qpc_time_ms CSV and process receipts. It validates exact PID/name,
normal exits and tool bytes, separates swapchains and uses CPUStartQPCTimeInMs;
the legacy TimeInSeconds header carries milliseconds in this mode. Bounded
row/chain limits refuse a prefix certificate. Preserve NA metrics. Compare
like workloads and settings before/after; a lower presentation rate is not a
faster GPU pass or proof of reduced DWM cross-adapter copies.

The explicit `presentmon-2.6-native-v1` schema has `QPCTime` in seconds even
when the tool invocation requests QPC milliseconds. Render-start/ready offsets
are signed. A mode/clock/unit mismatch must refuse; preserve unknown display
facts. With one explicit `--no_track_input`, the native-v1 CSV omits
`msSinceInput` and reports input tracking as disabled. Other required columns
stay unchanged. WPA exporter may emit collapsed aggregates despite a successful exit;
inspect the actual view, expansion and aggregate units before using rows.
Use exact context/sequence/packet and surface/present/bind keys for correlation.
GPU packet stop, driver execution, composition and scanout remain separate.
A matched backend screenshot `renderer.screenshot.cpu_pixels` marker observes
CPU-readable pixels before encoding. Default framebuffer 65535 still requires
an exact submitted-frame and native provider join for a completion bound.

Stream limited ETW providers/time ranges and bound retained bytes before writing;
attached stacks can exceed an observed polling limit. For formal A/B, isolate
background GPU consumers (including live wallpaper) and verify real moving
camera poses and distinct allocated raster/display texture/window extents.

Explicit user-script logs use the existing runtime logger in DIST too; its
level/filter/sink policy still applies. Internal engine informational chatter
remains compile-time gated. A missing INFO line cannot prove a role never ran;
verify its actual binding, start/update probe and behavior independently.

Keep complete source-gate failures separate from later focused repairs. Independently linked ordinary runtime negative controls need their exact expected failure result; a targeted PASS does not rewrite an earlier full command. Configured native domain totals retain unavailable/ignored branches, while report-only architecture checks are not binding or release certification. A GUI activation/permission failure followed by operator-forced stop is unavailable UI evidence, not a product-crash claim.

Desktop minimization emits focus loss and blocks GPU drawing; true OS/manual pause remains a separate state. run_in_background controls admitted CPU hooks, not proof that an application-specific update callback or simulation advanced. Verify those events separately from service polling and drawable admission. Quality vsync_count=2 currently reaches a boolean renderer bridge, so every-second-vblank presentation remains unimplemented. Keep that gap explicit. Per-script JSON cost candidates require isolated steady/intro/transition profiling through the existing runtime script-cost owner. PDH per-engine percentages, PresentMon timings and OS memory snapshots use separate windows; a named MEM_MAPPED cache allocation is not resident/private memory or GPU ownership and cannot be subtracted from RSS.


Editor startup configuration is profile-scoped: bind the validated editor-runtime.ini before engine creation, and preserve its explicit contents. A caller's project working-directory .config/jce.ini must not become Editor configuration. Capture actual config path, effective Vsync, drawable size, window flags and cadence mode with the executable hash. Changed focus, fullscreen, presentation interval or adapter clocks make old observations unsuitable for a fair performance improvement percentage. A source-baseline improvement and a successful embedded Bundle reopen are separate outcomes from GUI reachability and hardware performance acceptance.

Maximizing Game View does not establish OS-window maximization. Equal native and drawable extents do not establish equal window flags; retain actual MAXIMIZED, focus and fullscreen observations. Record adapter clocks, temperature and slowdown reasons for each comparison window. A changed thermal regime or an incomplete repeated sequence cannot certify a final performance gain.

For offline Windows correlation, use a common ETL fingerprint and a unique
PID/swapchain plus Present start and stop, with explicit tool clock units and
quantization bounds. PresentMon native-v1 relative and absolute QPC columns
may witness the offset when checked against the same decoded calls. Do not
join by row number or infer the DWM wait object from a matching display row.

The opt-in canonical trace reader can match sampled D3D11 provider source
frames to prior accepted submissions by exact device epoch and source serial.
An unavailable or unjoined sample is separate. A match supplies neither a
GPU fence nor an OS Present/DWM join.

For quality comparisons, use the closed `render_pipeline.publish` trace scope
to observe resolved CPU policy, including shadow filter, backbuffer MSAA,
post quality, HDR, depth prepass and SSGI. This does not witness actual
attachment samples or formats, pass execution or pixel parity; keep target
extent and rendered image witnesses separate.

GPU timer attribution uses the renderer's existing private snapshot: keep API frame, host submission ordinal and device epoch separate from the provider's global and per-view GPU source frame. Preserve raw ticks/frequency and explicit availability as integers, including values above double precision. Repeated source frame and ticks are latched observations, not new GPU work. This bounded opt-in transport does not certify full-device completion, presentation or resource retirement; a CPU trace footer cannot supply a missing GPU fence. Validate source controls and actual native trace independently.

PresentMon CPU event-span fields are distinct from OS process execution time. Preserve raw fields, but measure actual process CPU with identity-pinned kernel plus user time and the QPC elapsed interval. Report a single-core equivalent percentage unless the actual machine normalization is known. An Editor cadence observation requires its real window/focus/Play state; an uncapped authored Play session or benchmark exception is not the idle baseline. Keep working set, private commit, mapped allocation size and resident ownership separate.

Opt-in GPU view-name maps are bounded sampled subsets. Preserve actual/listed counts, rotated start, truncation and availability together with API/device/provider frame identity; decode whole-entry percent encoding before comparing names. A truncated row does not certify all views. Check the actual native Main and offscreen composite labels independently of formatter unit controls.

Selected Windows page diagnostics reuse WindowsReader working_set_pages on its identity-pinned query handle. Keep raw QueryWorkingSetEx flags, exact selected addresses/counts, inter-call budgets and QPC/identity windows. Interpret valid-page fields only when Valid is set; large/AWE pages can fall outside ordinary working-set membership. A named mapped allocation plus valid/shareable page flags does not establish JCE private-heap or GPU ownership and must not be subtracted from RSS. Keep selected-page completion separate from process-wide mapped-name coverage; a bounded name scan cannot prove an unobserved cache absent. Shared means shareable; a valid ShareCount above one is a distinct, capped observation. Retain the exact tested executable before replacing the published artifact, and bind later page observations to their new process birth rather than a prior cohort PID.

## Backend callback CPU evidence

An enabled dependency and a registered bridge remain distinct from an observed
callback. Check compiled (-1 unknown, 0 off, 1 on), bridge support, actual begin
count and saturation at attempt boundaries, together with the executable hash.
Pair complete CPU events by actual TID/category/id and same-thread nesting;
retain unmatched events and counter/capture failures. Never invent lifecycle
registration for a third-party thread merely because a scope used its TID.

Pinned bgfx's `bgfx/API thread wait` runs on the render thread waiting for API
submission; `bgfx/Render thread wait` runs on the API thread waiting for the
backend. JCE prefixes these names with `bgfx.cpu.`. Interpret names using the
actual executing TID and enclosing scopes. These callback spans use the task
category; their names alone do not satisfy typed wait-ledger requirements.
CPU frame-marker containment can establish an observed time window. Cross-thread
submit/flip overlap is not an exact backend batch, GPU or presentation join.
Keep startup, all deadline misses, geometry/focus availability and raw samples;
a short hidden capture cannot certify sustained visible throughput.

## Profiler and memory evidence boundaries

Profiler backend interval, API-thread render wait, render-thread submission wait
and latched GPU duration are separate observations. Their current public stats do
not provide an exact engine frame join or planned pacing attribution. Preserve
availability and raw scope; never derive CPU work by subtracting cross-thread
waits or classify CPU/GPU bottlenecks from these unjoined timers. Submission-wait
and backend-wait labels describe observed waiting only; otherwise retain
undetermined. A getter success or an absent timer represented as zero does not
establish supported GPU timing, low cost or completion.

Bind actual panel evidence to the rebuilt executable, window state, isolated
profile and locale. Inspect N/A handling, waiting/undetermined labels and the scope
caption/clipboard fields. The interpretation owner fixture and its old-wait-math
mutation are CPU evidence, not physical GUI delivery or GPU frame attribution.

Memory evidence must distinguish current-process RSS, private commit, available
allocator counters and voluntary tagged accounting. The panel's 512 MiB bar uses
RSS only; it does not state device capacity or certify an industry memory budget.
Zero tagged usage is partial accounting, not zero process cost. Preserve allocator
unavailability rather than using zero as a successful budget result.

For an actual Snapshot action, retain its reported profile path and decode the
published `jce.editor.memory_snapshot.v2` file under `reports/memory/`. Require
`complete: true`, explicit counter availability/nulls, all captured tag rows,
`complete_process_coverage: false` and `budget.scope: process_rss_only`. Preserve
exact unsigned byte values. Verify profile refusal has no CWD fallback and encoding
or atomic-write failure publishes no incomplete report or damaged prior report.
Complete serialization is distinct from a simultaneous collection window; atomic
replacement alone does not certify filesystem or power-loss durability.

Use the panel workflow's Profiler/Memory owner targets and all five ordinary
assertion-failing mutations. Retain actual child exit codes and named assertions;
a WILL_FAIL result alone cannot distinguish an expected assertion from a crash or
timeout. Those controls cover interpretation, profile resolution and publication;
full UI reachability, OS residency ownership and hardware performance require
separate observations.

## Host read request details

`fs.host.read_all` and `fs.host.read_capped` retain their service result policy.
When that existing scope is admitted, fifteen fixed CORE counters share its
id/parent: `fs.read.path_hash_hi/lo`, `total_hi/lo`, `requested_hi/lo`, `cap_hi/lo`,
`actual_hi/lo`, plus `dispatch`, `known`, `failure_stage`, `close` and `short`
(all byte/status names have the `fs.read.` prefix). Reconstruct each unsigned
64-bit pair with integer arithmetic. The shared FNV-1a-64 hash names the exact
borrowed request string, not a canonical file, VFS source or inode. NULL requests
use zero; empty strings retain their real hash. No raw path/error text is emitted.
Disabled or consumer-suppressed scopes do no hashing or metadata initialization.

`dispatch`: 0 none, 1 host, 2 VFS, 3 VFS then host. `known` bits: 1 reported
size, 2 requested bytes, 4 input cap, 8 observed actual bytes. Successful delegated
reads expose returned size; failed delegates expose no byte knowledge. After
host dispatch, actual counts only this owner's host reads. `failure_stage` is
0 none, 1 invalid request, 2 isolated delegated refusal, 3 host open, 4 size
query, 5 length limit, 6 allocation, 7 whole-file short read. A delegated refusal
is not proof of absence. `close`: 0 no attempt, 1 SDL accepted, 2 SDL refused;
`short`: 0 no SDL read, 1 requested count returned, 2 fewer bytes returned.
Cleanup never replaces the first terminal stage. Legal caps, historical capped
short reads and ignored close failures retain their existing successful result;
separate observations do not prove durable IO. Whole-file short reads still fail.

The existing real filesystem/ring fixtures and whole-owner ordinary omission
controls consume this contract. After rebuilding, require a fresh complete trace;
older service-only failures cannot reveal their missing identity or purpose.
Byte/counter coverage is CPU IO evidence, not a GPU or full-workflow certificate.

## Threaded capture write batching

The private continuous capture owner coalesces threaded short/empty-read batches
within the existing bounded staging buffer. A 1MiB threshold or 16ms from the
first pending encode start requests a flush; later events cannot renew it.
Sleep rounding, scheduling and synchronous IO can exceed the deadline. Stop
joins the worker and force-flushes remaining events before completeness checks.
Cooperative/Web poll behavior stays immediate. Lost/unexported/generation and
active-owner refusal, exclusive partial publication and full accepted-write
accounting remain mandatory; no sampling or omission hides writer pressure.

Optional footer writer_diagnostics version 1 observes payload-write attempts:
inclusive size bounds 4096/16384/65536/262144/1048576 bytes plus overflow and
wall bounds 100000/1000000/4000000/16000000/100000000/1000000000ns plus overflow.
The slowest sample binds monotonic start/end to requested bytes and the elapsed
maximum; reservation/header/footer/close are excluded. The same lifecycle
metadata parser validates its types, bucket sums and sample/byte bounds while
admitting old captures without it. Controlled owner clock/thread ports prove
policy, not scheduler/IO throughput. Actual real Engine join/tail captures and
new application measurements are still required for performance conclusions.

## Bounded public GPU timing observations

Use `jce_renderer_get_gpu_timing_observation(out, sizeof(*out))` on the renderer
API-owner thread. It returns false without writes/queries for a NULL or short
buffer; a copied version-1 observation returns true even when its explicit reason
is unavailable. Oversized caller storage beyond the current struct is untouched.
The old `JceGpuStats` ABI and getter behavior remain compatible.
Preserve signed raw frequency/begin/end and `gpu_source_frame`; positive frequency
and a strictly forward nonnegative interval admit `duration_ms`. Zero frequency,
equal/reversed ticks or missing context/backend are unavailable, not zero cost.
`read_host_frame_index` and `context_generation` identify the read; they do not
join that read to the backend's possibly repeated/older GPU sample. No fence,
presentation, full-device completion or resource-retirement proof follows.
Profiler and Profile Analyzer consume this API and retain delayed-value and
unavailable presentation. Their common helper reports the original backend CPU
interval and separate thread waits, without subtracting other-thread waits.
Actual-owner typed CPU controls and ordinary omitted-wire controls test the
transport/consumer semantics; actual backend timing needs separate observation,
SDK/Editor consumption and an uninstrumented control.

## Shared application resource observations

JCE_RESOURCE_TRACE=1 adds owner-thread resource counters to the existing enabled
trace. It defaults off; JCE_RESOURCE_TRACE_MS defaults to1000 and clamps to
250..60000ms. Use JCE_TRACE_STREAM for continuous capture and request
--resource-observations in tools/lifecycle_trace_report.py, retaining every
required lifecycle/wait gate. A requested flag without actual valid samples
is not acceptance. The same Engine path serves editor, player and Bundle hosts;
verify each actual host and distinguish synthetic QA from physical input.

The observer snapshots process RSS/commit, partial tagged allocations, current
non-creating default Async stats and qualified latched GPU clocks. Availability
is explicit; unknown values remain null in analysis. Wide counters are hi32/lo32
pairs; the report reconstructs exact integers and emits decimal strings beyond
JS's exact range. Bounded first/last witnesses and sampled extrema are not
lifetime maxima or complete heap ownership. Preserve raw GPU frequency/ticks,
source frame, read-host frame and context generation; they cannot become fences,
presentation FPS or a same-frame join.

Benchmark distributions now use actual monotonic panel-observation wall cadence
and separate backend CPU/GPU observations. Coverage interruptions invalidate
baseline comparison; the v1 file namespace preserves legacy baselines. This is
a limited workload/profile comparison, not OS/GPU exclusivity. For external
process diagnostics, memory_residency_probe.py --sample-threads optionally
retains bounded cumulative TID CPU. PID and psutil creation time describe the
child; missing thread birth/name prevents thread lifetime/rate attribution.

Process memory qualification follows the actual platform provider. On Web,
mimalloc committed/resident estimates are not OS process RSS or commit;
`jce_mem_stats` leaves current/peak RSS and commit at unavailable zero while
retaining its copy result and other supported fields. The resource consumer
records unavailable process memory as null and excludes it from sampled extrema.
Do not replace missing process memory with Web linear-memory capacity or partial
tagged allocations. Native providers retain their established semantics.

## Opt-in Windows process GPU rates

`memory_residency_probe.py --sample-gpu` and
`run_process(sample_gpu=True)` add a persistent PDH observer; default runs do
not import or load the Windows provider. The launched PID is the target;
comparison processes require explicit repeatable `--gpu-peer-pid` values.
Bind each retained subject to its initial psutil creation time and fresh PID
readers before/after every query. Unknown identity cannot become measured zero;
peers are never owned for termination. This bracket is not an atomic lifetime
snapshot or an exact native birth token.

`perf_bench.run(..., sample_gpu=True, gpu_limit=256, gpu_peer_pids=(pid,))`
forwards to that same observer and retains its receipt in `process`. Shared
option validation runs before output deletion or launch. Defaults remain off;
counted CPU forwarding and omitted-wire controls do not query PDH or establish
GPU work. Counter windows never supply FPS or replace frame timing.

The English `GPU Engine(*)/Utilization Percentage` wildcard uses the
[language-neutral add API](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)
and the documented
[wildcard formatted-array API](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw).
The array route retains one wildcard handle; the separately documented
localized expansion procedure is for adding individual matching handles.
[Rate counters need two collections at least one second apart](https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data).
Preserve priming/waiting, absent instances, denied/unsupported/invalid statuses,
query elapsed time, raw CStatus and explicit null. Only a valid finite counter
may establish an observed 0%. Failed array growth uses a fresh size probe;
finite retry/buffer bounds reject incomplete data rather than publishing it.

A complete PID/LUID/physical-GPU/engine structure may carry an empty OS engine
label. Preserve its actual index and qualified counter; represent the label as
`engine_type=null`, `engine_type_available=false`, `engine_type_reason=EMPTY_OS_LABEL`.
Never infer 3D, Other or unused. Nonempty labels retain their actual values;
missing structure remains an unparsed name with bounded diagnostic witnesses.

Rows retain PID/birth, adapter LUID, physical GPU, engine index/type and window
identity. `--gpu-limit` bounds retained rows, not OS enumeration or query time;
truncation and actual queried/matching counts remain explicit. Never sum engine
rates into total utilization, FPS, presentation or a CPU/GPU frame join.
[Microsoft distinguishes independent engines and Task Manager's busiest-engine summary](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/).
Same-window Editor/DWM observations can show concurrent work, but cannot prove
cross-adapter copies or a causal bottleneck. Keep real driver/locale counter
acceptance separate from typed DLL/CLI CPU controls and compare uninstrumented
runs; query overhead and incomplete rate windows are part of the evidence.

## Profiler UI observations

Profiler current UI interval comes from monotonic editor-update wall time;
its current cadence and ImGui rolling average are separate observations, not
presentation FPS. Keep backend CPU interval, independent thread waits and
qualified delayed GPU timing separate. A submission-wait indication describes
that observed CPU interval; it does not establish a GPU or DWM bottleneck.

Historical GPU/API/submit plots must consume availability masks. Connect only
adjacent known samples, preserve genuine known zero and leave unknown gaps.
Show known/unknown sample counts; an unknown latest sample must not erase earlier
known history. Context changes clear backend timing qualifications while retaining
UI history. The bounded panel history counts observations, including potentially
repeated delayed GPU values; it is not a distinct GPU-work or same-frame series.
Actual chart consumer controls and physical UI checks remain separate evidence.

A late event consumer may not have observed a thread's registration prefix.
Keep unknown registration, observed registration and observed unregistration
distinct; trace unregistration is not proof of OS thread exit. Producer current
registration counts and consumer historical rows have different scopes. Clear
or cursor loss must invalidate unsupported qualifications. Render backend
memory-unavailable sentinels as unavailable, retain raw evidence and valid zero.

Analyzer export must encode every selected row before atomic publication under
the owned profile report directory. Preserve ring order and explicit metric
availability; path, encoding, allocation and write failures require diagnostics
and cannot certify an old or partial CSV. Verify the actual export consumer and
failure/retry route separately from a codec test and a physical button click.

PresentMon output schemas retain their own metric names. An explicit
`--v2_metrics` receipt uses `CPUStartQPCTime` only with `--qpc_time_ms`; retain
CPU-start cadence, FrameTime, CPUWait and GPUWait with their distinct meanings.
Legacy fields absent from that schema remain unavailable. Preserve the raw CSV
and failed admission receipt when adding a supported schema; create a fresh
review rather than rewriting a historical failure. These observations do not
establish optical cadence, whole-frame joins or matched framebuffer workload.
All admitted report modes identify GPU timing as unchanged event-derived
estimates, retain NA and valid zero, and leave hardware-accelerated GPU scheduling
(HWS) state unknown. This CSV review does not independently certify HWS state or
true hardware busy time. The [pinned PresentMon v2.6.0 HWS precision guidance](https://github.com/GameTechDev/PresentMon/blob/v2.6.0/README.md#tracking-gpu-work-with-hardware-accelerated-gpu-scheduling-enabled)
warns that GPU execution timing can be delayed or inflated when HWS is enabled;
the amount depends on the workload and GPU. Keep this precision boundary with
the original values rather than applying an inferred correction. A HybridPresent
CSV value of zero without swapchain creation/resize event coverage cannot alone
prove there were no cross-adapter copies; the report does not summarize that field.

An explicit `JCE_FRAME_LOG_CONTEXT=1` capture adds `context_json` to the private
frame CSV. Each completed `frame_wall` row carries its sealed v1 window packet;
other phase rows have an empty fourth field. Empty or `0` retains the legacy
three-column format. Keep null query results distinct from valid zero flags,
cached point/drawable sizes distinct from live native begin/end observations,
and backend frame indices distinct from hardware completion. The shared parser
validates the entire file, preserves selected packets alongside every timing
sample and accounts for the actual schema header. A resize, missing query or
lost focus remains evidence; never drop its timing to improve percentiles.
Window packets alone do not establish viewport/FBO workload, unobscured pixels,
adapter LUID, GPU fences or presentation cadence.

## Cache and allocation evidence

A cache timing workload must record actual hit/miss deltas, monotonic frequency
and every timed block. Pointer-dependent hash collisions can turn warmed labels
into repeated shaping; source arithmetic alone does not explain an earlier
uninstrumented failure. Keep the existing savings threshold, equal work and all
alternating-order pairs. Debug CPU microbenchmarks are distinct from graphical
product performance. A deterministic same-owner conflict control should fail
ordinary reuse assertions independently of timing and address randomization.

Counted component-patch observations must distinguish cJSON allocation calls,
whole-tree clones and process bytes. Explicit construction/merge refusal before
parse is narrower than a fully transactional serializer/parser OOM guarantee.
Custom PBR stage traces name the called host/VFS API and actual engine-PAK
fallback; host API success alone does not identify a physical host provider.
Program-handle validity is API admission, not GPU completion or pixel validation.

The existing Windows commit probe optionally consumes raw page flags with
`--working-set-pages`; it remains off by default. Keep its selected/queried page
coverage, previous-walk PRIVATE/MAPPED/IMAGE labels, raw flag histogram and native
identity/QPC window. Valid=0 is an observed invalid-page flag; denied/malformed
queries are unknown. Valid-page bytes may include AWE/large pages and do not form
an RSS identity. File-name metadata and address matches are non-atomic observations;
keep the original OS RSS/private-commit values and partial name/query outcomes.
Do not trim, suspend or touch target pages to improve a memory result.

## Consumer diagnostic admission

Before adapting a harness, check its actual input range and completion contract.
A relocated-Editor validator requiring at least 240 frames cannot certify a
60-frame stock Bundle run. Preserve the original refusal, then validate the
retained artifact under its explicit scope without rerunning or rewriting it.
GUI-subsystem programs may not deliver LOG_INFO to stdout; inspect declared
output files instead of treating an empty pipe as project-emission failure.

For long captures, a present `sequence_exhausted` metadata field must be the
strict boolean false. Reject true and mistyped values even when other counters
balance. Preserve an older capture's absent field as absent, not known false;
finite cursor exhaustion cannot certify unlimited recording. Complete EOF,
loss, unexported events, generation and owner closure remain separate gates.

Treat `[jce_log_truncated required_bytes=N]` and `[jce_log_format_error]` as
incomplete-message evidence. Do not accept the retained prefix as a complete
structured record or fill missing fields from source/defaults. Exercise the
actual sink's message capacity, preserve the original failure and use a fresh
receipt for a corrected producer/consumer.

With explicit `JCE_RENDER_RESOURCE_DIAG=1`, resource log schema2 needs complete
parts1/2 joined by event_ordinal, creation_serial, origin_capture_generation and
view. Reject missing, duplicate, interleaved/mismatched or exhausted part0
observations. Then correlate committed allocation and view_configured using
resource identity, FBO/attachments, allocated extent and forwarded rectangle;
requested extent alone is insufficient. These are CPU configuration-call
observations with GPU completion unknown, not an allocation-completion fence.

A render-scale `1.0 -> 0.5 -> 1.0` control lowers quality and diagnoses pixel
load sensitivity. Verify actual allocation/view extents, retain both full-scale
runs' drift and every CSV spike, and keep artifact/Bundle, clock and observers
fixed. Never report that reduction as same-quality optimization or sustained
400/600 FPS. Latched backend submit/wait groups are independent observations,
not additive stages or a causal GPU/DWM attribution.

WASM engine shutdown and Emscripten runtime exit are separate observations.
Explicit smoke markers plus a canonically closed CPU/resource trace can prove
that engine run; an unobserved onExit remains null, never inferred exit zero.
Keep intentional script exception controls and unavailable optional VM rosters
visible. A grass early-gate admission is narrower than prepare/colour submission;
an empty shadow-caster walk cannot prove a shadow draw. Missing images remain
unverified pixels. Freeze binary/SDK identities before comparing these paths.

Canonical Runtime create/tick/destroy and Editor operation/Play scopes describe
synchronous CPU call walls. An operation's default success means normal return;
known failures need explicit failed state. Viewport policy, actual render call,
early admission and final refusal are distinct observations. Reset call details
retain requested versus passed extent/flags and the reason; a returned void
bgfx call leaves backend application and GPU completion unknown. Never infer
repeated swapchain rebuilds from SDL event counts or reset-call counts alone.
Use closed real-ring controls plus separate artifact-bound consumers. A hidden
window witness cannot qualify foreground presentation or all physical UI routes.

Observe initial backend flags before creation as well as later reset calls.
Keep requested policy, launch override presence/value and effective/passed flags
distinct. Verify the actual backend/adapter: hidden windows can execute real GPU
work, and enumerating an adapter does not prove selection. `JCE_PERF_LOG=0`
disables optional telemetry, not necessarily compiled or admitted Tracy/bgfx
profiling. Bind profiler claims to the actual build and admission observations;
these CPU flags alone do not certify backend application or presentation.

Readback cancellation is a logical outcome, not permission to free a pending GPU
destination. A successful release request may still retain the slot/buffer;
reclaim only through the existing ready-frame/retire/backend-stop authority.
Ordinary UI cancellation must not introduce render submissions or bypass drawable
admission. Hold a current-frame busy decision even if polling completes before
later viewport calls, so those calls cannot reuse the capture's view band.
Choose readback format from the prepared target, not its pre-allocation default;
authored HDR/LDR output and actual source format are separate facts.

Trace-off sampling must consume the effective engine policy, not a harness's
handwritten flag. A nonempty JCE_TRACE_STREAM or JCE_TRACE_EXPORT enables tracing
before JCE_TRACE is considered. Remove both paths, set JCE_TRACE=0 explicitly and
record actual environment and stream observations; retain any earlier mislabeled
receipt and append a correction. KPI CSV, context packets and process sampling
still have observer costs. Compare frozen artifacts under the same declared
workload, retain every frame and separate CPU intervals from GPU/presentation.

Canonical counter JSON preserves int64 decimal values; use an integer-preserving
reader for raw GPU ticks and identities above 2^53. JavaScript Number/JSON.parse
does not preserve those values. Inspect the actual writer/provider chain before
attributing apparent quantization to export. A nonzero equal begin/end interval
can be a valid measured zero; the all-zero initial state remains unavailable.

## Canonical GPU view packets

`python tools/lifecycle_trace_report.py TRACE --require renderer.submit_cpu
--gpu-view-observations --out SUMMARY` explicitly enables the bounded typed
consumer in `tools/gpu_view_observation.py`; omission preserves the old report.
It reuses the canonical reader and B/E owner, requiring the actual service,
active TID/category/id/parent, ordered counters and successful closure. Each
`gpu.view.<uint16_view_id>.` packet has exactly nine fields in producer order:
`device_epoch`, `api_frame`, `submission_ordinal`, `source_frame`, `begin_ticks`,
`end_ticks`, `timer_frequency`, `interval_available`, `reported_ticks`.
Validate uint32 frame/ordinal values and signed int64 clocks without coercion.
Known epochs are positive int64 values; -1 stays unknown. Preserve exact raw
integers and encode values wider than 53 bits as decimal strings in the report.

An interval is available only when frequency > 0, begin >= 0, end >= begin and
the endpoints are not both zero. Reported ticks must equal end - begin when
available and -1 otherwise. Nonzero equal endpoints remain valid zero duration;
unavailable duration remains null. Packets are sparse, latched provider samples:
each view owns its source_frame independently of other views, the current API
frame and CPU/CSV rows. Never sum views into frame time or infer presentation,
fences, GPU completion or per-frame coverage from these records.

Retain bounded active records, known-(epoch, view) aggregates, first/last
witnesses and one last signature per group. Repeat qualification compares only
the preceding observation for that known group using epoch/view/source/raw
ticks/frequency; unknown epochs are not grouped and break comparison continuity.
Duration aggregates exclude these adjacent repeats, not all historical reuse,
and are not per-frame averages. Explicit requests with no packets fail; packets
with wholly unavailable timing retain an explicit unavailable status. Acceptance
still requires the canonical full-file EOF, stable input fingerprint, complete
capture metadata, loss controls, closed scopes and reader/observer limits;
recognizing a packet prefix or reading a partial file is insufficient.

## Screenshot schedules and moving images

Native screenshot schedule times are due thresholds. A preceding in-flight
capture can defer the next request until PNG encoding and writing complete.
Require a same-artifact A/A control before attributing moving-image differences;
reduce capture frequency to isolate writer backpressure. File modification time
is not a renderer source-frame or camera-pose observation. Preserve unavailable
frame/pose joins and account for startup reveal/composite passes that may hide
underlying scene differences. Successful exit alone does not prove every
scheduled capture completed, especially across application-exit cancellation.

Request labels and nominal simulation timestamps are not source-pose joins.
PNG backpressure may defer later captures to different camera states; join each
admitted image to an observed source frame/pose, or retain that join as unavailable.

`--screenshot-requests` explicitly enables the canonical lifecycle reader's
bounded request ledger. Admissions and terminals use the accepted uint64 request
identity, not a task scope or a global cross-thread timestamp sort. Match every
terminal to one unmatched admission; allow different requests to close out of
order. Require strictly increasing admission serials, legal mode/outcome fields,
complete continuous capture and the existing loss/EOF/identity gates. Keep a
bounded active map, finite aggregates and first/last witnesses; do not retain an
unbounded set of closed identities. `NO_ACTIVITY` is not screenshot acceptance.
CPU encoder/file success, failure and shutdown cancellation do not prove GPU
completion, durable storage or the image's source-frame/camera pose. A real
ring/writer protocol fixture and an actual product screenshot are separate tests.

Temporal history must belong to its viewport owner. Reuse requires matching
scene, renderer context, consecutive CPU submission, Play ownership, extents and
projection; commit metadata only after a successful resolve. Lifecycle focus,
pause and device events can invalidate history even when the backend frame did
not advance. Take and clear the listener token, then unregister before resource
release; failed registration must roll back through the complete existing owner.
CPU admission tests do not certify dynamic pixels or GPU retirement.

Hidden-window execution can legitimately omit Scene/Game rendering while the
editor UI continues. Require actual admitted viewport, draw/pass and resource
witnesses before treating a run as a rendered application workload. An explicit
capture override changes that admission policy; record it as part of the subject,
rather than calling it natural visibility. A short initial window/context CSV
cannot certify later manual inputs or the whole timed run. Wall-IP histograms,
OS CPU scheduling stacks and per-engine GPU rate windows have separate scopes.

UI row virtualization must preserve native keyboard navigation, selection,
popups, copy operations and full content extents. Match actual toolkit item
rounding for fractional spacing. Derived geometry belongs to the existing panel
owner and must be invalidated by real font identity changes and released by its
shutdown; pointer equality alone is not a reload generation.

Repeated offscreen identity counters inside `renderer.view_configure` describe
successful CPU configuration calls; count allocations only under the actual
create-committed owner. Match each complete packet to its service, resource
serial, capture generation and submission ordinal. Allocation extent, forwarded
rectangle and the texture selected for UI consumption are separate witnesses;
none proves GPU completion. Retain real missing-field assertion failures and
unchanged allocation lifecycle counts when extending these observations.

## Cache invalidation and mutation notifications

A cache generation is not a setter or notification counter. When avoiding an
unnecessary invalidation, retain the actual typed mutation and ECS notification,
including equal writes. Verify notifications with the real observer, not a
generation increment used as a proxy. Compare the fields the cache actually
depends on; mutable aliases require conservative treatment without a prior
snapshot. Preserve membership changes and restore any private specialization
before expanding unrelated setters.

Check the pinned ECS implementation before asserting deferred visibility: an
existing value may be copied immediately while its notification is deferred.
Fixtures must retire their own deferred scopes even after assertion unwinding.
Keep ordinary assertion totals and exit codes for negative controls; a crash or
missing test summary cannot qualify merely because CTest WILL_FAIL accepts it.
CPU cache controls and local phase savings do not certify pixels, branch-hit
counts, GPU gains or normal-clock whole-frame performance.

## Ordinary Editor workflow and effective GPU load

Distinguish manifest scenes, loose scenes and isolated Bundles. Exercise the actual
File/recent-scene and Assets open entries, including a refused load, rather than
qualifying them through an automatic Bundle/forced-Game run. Record the loaded
path and script set: a restored user session may select a different project.
Clone configuration for controlled runs; distinguish deliberate clone edits from
the application's ordinary session saves.

Bind the actual backend and vendor/device or adapter LUID after initialization.
A saved Editor preference can precede environment hints; Auto may select the
display's integrated adapter. An authored HUD label can be stale at startup.
Keep independent GPU engines and DWM separate, and observe the desktop with the
Editor closed before attributing compositor load.

Record internal target, final presentation extent, sample counts and active
resolution policy. Adaptive scaling can change quality while the outer window
stays fixed. `ImGui::Begin` returning true does not prove exposed presentation:
a same-host overlay may cover the panel. Decide coverage from a frame-frozen
snapshot and viewport identity, without suppressing detached viewports. Skip only
UI aggregation, layout and draw submission when fully covered; retain necessary state
transitions and host/window maintenance. Hidden dock Begin/End alone does not
prove 3D work; require render/admission/submission and actual pass evidence.
Do not rank backends using different content, visibility, scaling or Play phases.

An active screenshot extent can override the Game target. A capture-disabled run
at the same outer window can render a different natural surface. Observe requested,
allocated and displayed extents in both runs, then declare the actual current
workarea and match controls; never infer it from the screenshot request.

An accepted replay row proves admission only. Verify held-button consumption,
full camera transform/projection, focus/hover and target extents across drag
intervals. Compare actual component geometry/state; seed equality or a partial
camera probe is insufficient.

Fixed-step motion controls must cover stopped preview and Play through explicit
QA selectors, preserving ordinary timing policy. Use existing project seed
controls and compare observed clocks/state/geometry: a seeded runtime can inherit
unseeded preview state. Keep particle RNG/age gaps explicit and use same-algorithm
image repeats to measure residual noise.

Before timing a replacement, require nonzero matching submits from the workload
producers through the existing shared wrapper, including direct/indirect model
paths. Candidate load, selector admission and wrapper unit tests alone do not
establish workload coverage. CPU coverage still does not certify GPU binding
or execution. Earlier source/DXBC discard and declared shader temporaries do not
establish screen coverage, physical register allocation/occupancy or whole-frame
savings. Source-texture transparency is not measured GPU work avoided.

For a CPU whole-owner Editor fixture, unrelated About/asset references may pull
renderer shader cooking. Keep the actual tested owner; isolate only those
unrelated dependencies with typed ports that fail on unexpected calls. Do not
replace the tested owner or remove Engine dependencies exercised by assertions.

Repeated resource-admission refusals are diagnostics, not new owned allocations.
Bound their reporting with precise owner/epoch totals and first witnesses while
retaining actual creation/retirement lifecycles. Lost events, count saturation,
truncated detail and unexercised owners remain explicit coverage limits; a valid
JSON footer or unexported=0 alone does not establish a complete capture.

### Current-frame GPU resource dependencies

A CPU `prepared`/`depth_prepass_frame` flag and a bound SRV prove admission and binding, not GPU production or sampling. Join the exact consumer shader branch and actual constants to the resource ID, producer clear/draw and final backend view order. A producer sorted after its reader supplies earlier or undefined contents unless an explicit history contract owns it. Preserve current-color postfx dependencies when correcting depth order; cover optional-pass-only and the final hardware view-ID window with ordinary old-order controls. Do not diagnose TAA from a ghost-like image without observing an active TAA pass. A failed capture gate, a missing target or unknown identity remains unavailable.

### Exact process identity transport

Pass floating process-birth stamps as strings or structured argv. PowerShell can reformat an unquoted numeric literal before the observer receives it. Keep identity comparisons exact; a transport precision loss must not be hidden with a tolerance. Preserve the original refusal and diagnose the argument boundary before another capture. An explicit GPU request still permits independent display-topology observation; retain observed, requested and actual selected adapters separately. A display LUID mismatch or high Copy-engine utilization alone does not prove a cross-adapter copy.

KPI capture dispatch coalescing reuses the existing bounded byte ring and single executor task. The capture selects 16 KiB or an observed oldest-record age of 8 ms; zero/zero writer configuration preserves immediate dispatch. Age is checked by the next submit/poll, not a timer or a hard IO deadline. Finish forces the remaining batch, and original durability boundaries, strict FULL and partial-write failure rules remain. Lower fixture task/write counts establish scheduling reduction only; they cannot resolve an unexplained OS write stall or certify whole-frame improvement.

## Same-interval host viewport observations

Explicit `JCE_FRAME_LOG_CONTEXT=2` selects the independent bounded context-v2
CPU schema. Default and selector1 keep their original bytes and strict keys.
The Engine copies a sized host report only inside its active owner-thread
app.draw, then seals it with that completed interval. Missing report differs
from explicit empty. Requested, allocated and selected display extents, prepare
success, CPU-tail completion, Image invocation, final queued commands and actual
main/recording view submit calls remain separate availability-qualified facts.
A retained allocation can display old content after refusal; an unknown PostFX
extent cannot borrow bridge dimensions. Main-view calls may target a screenshot
FBO. These observations do not establish GPU execution, retirement, backbuffer
presentation or complete shader success. Use the strict reader and retain writer
terminal/clock ownership before computing a same-interval rendered-work subset.

Explicit `JCE_FRAME_LOG_CONTEXT=3` retains the v2 viewport facts and adds the
completed `frame_wall` interval from the existing Engine performance counter.
Its `clock` object uses domain `"jce-performance-counter"` and canonical uint64
decimal strings for `frequency_hz`, `begin` and `end`; retain their precision.
The strict readers validate duration, order and frequency across all rows,
including warmup and tail. Defaults and selectors1/2 preserve their bytes.
This clock is not Unix time, a GPU timer or Present. Cross-provider correlation
needs a verified clock mapping and observed intervals; temporal overlap alone
does not establish causality or join latest-latched GPU statistics to a CPU frame.

For the public default runtime entry, nonempty `JCE_KPI_FRAME_LOG` with exact
`JCE_KPI_FRAME_LOG_VIEWPORT=natural` respects the host draw policy, including
focus loss and pause. It removes only FrameLog's force-draw override; explicit
screenshot capture can still force drawing. Empty FrameLog does not force it,
and other viewport selectors keep their existing capture behavior.


## Optional filesystem read detail packets

Request `--filesystem-read-details` in `tools/lifecycle_trace_report.py`, or
`filesystem_read_details=True` in its Python API, during the existing canonical
analysis pass. Existing scoped counters provide exact request-string hash,
dispatch, first failure stage, known-byte mask, total/requested/cap/actual bytes,
close and short-read observations. No raw path/error text is emitted. The hash
is not a canonical identity or proof of a valid path, including zero/empty cases.

The collector requires all15 unique typed fields once a packet starts; their
order is unrestricted. It correlates counter id to the active read scope and
counter parent to the submitted caller, preserving the different B/E self-owner.
Absent whole packets remain unknown; partial, duplicate or cross-owner packets
reject. Active state shares max-active; diagnostics retain at most
min(history,256) first N-1/last witnesses with explicit omitted counts. Every
packet still validates, and finite aggregates never grow by path identity.

Missing byte observations remain null; observed zero stays zero. CPU return
success can coexist with failed close or a capped short read. Optional/required
intent and errno remain unavailable: an OPEN failure is not necessarily ENOENT,
and isolated VFS refusal is not necessarily absence. Keep original task outcomes
and required-phase/loss/EOF/clock/fingerprint controls. Default reports are
unchanged. Apply the option to the next owned analysis, without rescanning a
previously completed application trace merely to enrich its old summary.

For a new owned Engine session, JCE_FS_OPEN_FAILURE_DETAIL=1 enables bounded
OPEN-failure log details through the existing host read owner. It is default-off
and leaves the numeric trace schema unchanged. Correlate both path/error records
by actual scope/parent, generation and request hash; validate byte lengths, hex
prefixes and truncation. The Engine owns the budget context; the host-read
service borrows it until diagnostic producers join, independently of VFS
switches. Repeated or foreign begin/end cannot reset or detach it. The64-group
session budget, filtering, logger drops,
rotation or a missing record can leave evidence incomplete. Attempted emission
is not acknowledged delivery. SDL text is a provider explanation, not a portable
errno or proof that the file was optional. This cannot recover old trace text.

The shared default SDK entry also publishes context2 standalone source1.
Host-policy and missing-owner early returns are explicit CPU_SKIPPED facts.
Requested scene-config extents, successful prepare and admitted offscreen
allocation remain independent; a direct fallback clears the bridge facts.
CPU_COMPLETED means the scene-render call returned and the host tail was reached,
including a renderer's internal refusal returning the same view ID. It is not
render or Present success. Requested rectangles cannot become observed display;
final display/texture and ImGui counts remain unknown for this host, including
void PostFX present and screenshot scheduling. Default/v1 adds only the enabled
check, without observation clearing or extra extent/window queries. CPU owner
controls and refreshed SDK consumption do not qualify visible input or timing.

### RenderDoc draw-count semantics

RenderDoc ActionDescription.numInstances is0 when instancing is unused. Reject
zero instances only when ActionFlags.Instanced is present; retain the raw count
and, if needed, a separately labelled effective count1 for ordinary draws.
Keep draw admission in one helper covered by ordinary/noninstanced0,
instanced0, empty-index and positive-instanced controls. Before concluding a
shader was absent, retain bounded actual shader identities and explicit draw
rejection counts. Reference: RenderDoc1.44 api/replay/data_types.h.

### Native capture controls and output ownership

Before a visible RenderDoc session, check capture hotkeys against the tested UI
shortcuts. F11 capture can also react to Shift+F11; the Game toolbar Maximize
button avoids that collision. An unsolicited NewCapture fails a sole-trigger
protocol even if the application's UI transition succeeded.

A renderer may replace the injector's capture-file template during startup.
Inspect the selected dependency's actual configuration before declaring output
roots; bgfx can select its configured temp/bgfx template. Record raw notification
path/local/API/size before admission. Permit only predeclared, owned local roots
and names; retain path containment, file identity and unique-capture checks.
A rejected run stays failed. Normal Stop/exit and process retirement are separate
from successful capture, replay, GPU timing or presented-image certification.


## Optional shader CPU provenance

Use `tools/lifecycle_trace_report.py TRACE --require renderer.shader.create --shader-provenance --out REPORT`
(or `shader_provenance=True`) in the next owned canonical analysis. Enable the
existing trace capture and device `JCE_RENDER_RESOURCE_DIAG=1` before eager
shader loading. Default analysis does not import this observer or add its key.
The ordered render packets bind actual CPU payload extent/SHA256, create return,
same-call link and creator-release return using exact acquisition/capture/context
IDs. They share the existing reader, B/E owner, loss, EOF and fingerprint rules.
Missing packets and nonzero reason codes retain unavailable gaps; partial,
duplicate, cross-owner, reused or out-of-order receipts reject. Pending receipts
are bounded and are observations, not leaks. The reported digest cannot be
independently rehashed without its original bytes. This subset does not certify
other shader owners, bgfx cached bytes, final program references, GPU execution,
fences, presentation or whole-frame improvement. Synthetic controls need a
fresh native export-to-consumer check before native integration is certified.

For visible F5 tests, refresh the window state after the transition; an immediate
pre-transition image must not trigger a later second F5 that stops Play. Keep
operator mistakes failed and rerun with one toggle and a fresh PLAYING witness.
Compute deadline-exceedance counts from the measured intervals, never a report
template. A tiny exceedance of a 60 Hz deadline differs from a severe hitch.

Terminal ACK callers can request `trace_analysis_options={"required": PHASES,
"shader_provenance": True}` and `return_trace_report=True`. ACK validates the
final cleanup/session through that same canonical report, with one JSON decode
and the existing full second-byte fingerprint; no independent session scan or
caller-supplied trusted report is accepted. The returned `trace_report` is the
complete analysis result. Require its shader observed/complete receipt fields
separately: a successful ACK alone can legitimately have no shader observations.
Do not run a second full analysis merely to retrieve these observations.

## Process visibility and decoder admission

A process inventory can be restricted to an execution sandbox or session.
Verify that the observer can see the known PID, creation time and executable
before using an absent row as termination evidence. A restricted-query error
is unavailable, not zero live processes. Use the actual host/owner observation
for retirement; never act on a guessed PID.

Choose the intended typed observers before the first full trace traversal.
For load-failure diagnosis, explicitly request filesystem read details and
shader provenance when the producer supports them. Successful required CPU
scopes do not certify unrequested packet details. Preserve unknown paths,
errno, optionality and omitted witnesses; do not label failed reads as normal
probing without evidence.


Large offline traces may opt into exact disk history through the same canonical
reader and ACK route. Supply a fresh absolute database path and both explicit
record/byte budgets; default memory and other RAM limits remain unchanged:

```text
python -B tools/lifecycle_trace_report.py TRACE --require engine.log_quiesce --cleanup-ownership --terminal-session-binding --out REPORT --history-db ABSOLUTE_FRESH_DB --disk-history-max-records 8000000 --disk-history-max-bytes 536870912
```

This disposable index retains every task, parent, completed-service membership
and terminal session exactly; it cannot repair a lost capture. Storage, budget
or close failure denies validation without a RAM fallback. The canonical full
EOF and two complete byte fingerprints still apply. See
[trace validation budgets](../../../tools/BENCHMARK.md) for API/ACK parameters.
Optional version-1 `reader_diagnostics` retains first/last loss detection
snapshots and read-stop progress. Its checked process clock describes detection,
not the overwrite instant or its cause; it does not supply a CPU/GPU clock join
or override capture completeness. Old captures without the block stay compatible.

## Matched performance, bounded views and build containment

Keep raw hardware temperature, clock, power and limiter observations with each
measurement window. Equal scene/size is insufficient when operating points
change. Reject unmatched comparisons, retain every spike and unavailable field,
and never normalize FPS by MHz to manufacture a gain. Distinguish cold and
sustained workloads; state any segmented-observer deviation. Size the operator
and observer deadlines before launch to cover real UI/tool latency, warmup,
measurement, post-window and cleanup. A guard exit does not certify a manual
Stop or Alt-F4. Never extend a running budget retroactively.

For reordered rendering, prove the exact accepted ranges, admission and draw
count before claiming equivalent work: transient-prefix allocation can be
order-sensitive. Use current A/A before A/B pixel comparisons, with QA fixed
simulation time kept separate from natural-time user-input performance. A
screenshot texture size is not visible-window/Max proof; seeded Lua alone does
not prove native/GPU RNG determinism. An opt-in candidate stays unqualified
until actual admission, images and matched whole-frame results are observed.

The Editor live trace model bounds retained thread, counter and active-span
records and reports local refused events separately from Core producer loss.
Known keys remain updatable at capacity. Clear resets this view only; public
cursor/stats expose no generation token, so an external quiescent reset requires
an explicit view Clear. CPU cache-count controls are not physical-memory or
native-presentation certification.

`tools/build/windows_build_job.py` is an explicit experimental owner for
caller-exclusive Windows process trees. Default/runtime runners are unchanged.
Suspended creation, Job membership and exact path/hash/held-handle identity
prove containment, not logical exclusivity of shared IPC services. Shared
MSPDBSRV/VCTIP policies are rejected; do not wrap real MSVC builds with this
owner or relabel old strict cleanup failures. Controlled CPU/CLI fixtures are
not a real compiler integration certificate. Planned helper termination,
emergency containment, unavailable metadata and close failures stay distinct.

Opaque geometry alone does not make reordered draws pixel-equivalent: finite-depth
quantization and overlapping geometry can change the winning sample. An existing
visibility AABB is not automatically a complete animated-geometry bound. Retain
failed current A/B images, qualify the cause, and remove an unproven candidate
rather than silently changing the comparison rule.

For a manually stopped diagnostic with `JCE_MAX_SECONDS=0`, the timed-deadline
KPI ledger is disabled, not a valid deadline check. Keep it NOT_APPLICABLE and
validate CSV clock continuity, frame disposition and terminal ACK separately.
Do not run an otherwise unnecessary long GPU stress merely to expire a timer.

Private startup queue/flip experiments require a rebuilt producer. Before process
launch, independently set `JCE_GFX_FLIP_AFTER_RENDER=0|1` and/or
`JCE_GFX_MAX_FRAME_LATENCY=0..3`; absence or empty values preserves bgfx defaults.
Values must be exactly one ASCII digit. Invalid explicit windowed requests refuse
hardware initialization before native/device acquisition and identify the invalid
field without echoing arbitrary environment content. The enclosing application
may still choose its normal software fallback. One snapshot serves every backend
attempt; changing either variable requires a new process/device creation.
Flip selects bgfx flip scheduling, not DXGI flip-model. Latency is init-only and
forwards only to pinned bgfx 1.146.9306-550 D3D11/Vulkan. Unsupported D3D12,
GL/GLES and other backends keep ctor defaults; Noop/headless/software execution
is inapplicable. Explicit zero requests backend defaults, not a zero-depth queue. Vulkan's fence/deferred-release ring can reduce throughput.
The bounded `launch-gfx` record and `renderer.launch_gfx` scope describe CPU
forwarding and admission, never backend acknowledgement or physical completion.
`latency_support`: 1 D3D11/Vulkan forwarding, 2 D3D12 unsupported, 3 GL/GLES
unsupported, 4 Noop inapplicable, 5 other/unknown. `phase`: 0 invalid refusal,
1 init failed, 2 CPU admitted, 3 API refused, 4 headless, 5 software; non-attempts
report `cpu_init=-1`. Headless/software entries ignore the experiments and report
that inapplicability, including malformed values. No new default-off log/trace
packets or per-frame environment reads are added. Complete-owner CPU ports and
an ordinary missing-forward-wire mutation establish only admission, forwarding,
reset-bit preservation and failure publication semantics. For performance, retain
identical assets, dynamic camera, dimensions, quality, pacing and adapter across
repeated A/B runs, inspect the actual backend and forwarded startup packet, and
measure throughput plus presentation latency separately. A PresentMon average
latency/frame-time ratio alone does not prove a precise queued-frame count.

### Natural DrawLog admission

For an Editor producer built with this selector, an active DrawLog accepts
`JCE_KPI_DRAW_LOG_VIEWPORT=natural`. It removes only that logger's legacy
capture override; missing, empty, `capture` and invalid values keep the old
behavior. FrameLog has its own `JCE_KPI_FRAME_LOG_VIEWPORT` selector, so set both
to `natural` when both logs are enabled. Explicit Game capture still wins.
Keep `JCE_KPI_SHOT` and `JCE_IMPOSTOR_BAKE` absent in natural observations.
Verify actual visible viewport/target dimensions and the compiled producer;
this flag does not update an older executable or certify a GPU workload.
Draw counts are latched whole-frame samples including Editor UI; shared RQ/cull
values are not exact same-frame or single-viewport attribution. For a rebuilt producer that exports `gpu_stats_available`, zero marks a failed
statistics observation and one admits a genuine zero/positive count. The legacy
numeric fallback remains zero; older CSVs without this bit retain ambiguous zero.
This does not supply a backend source frame or execution witness. CSV
formatting/file IO is observer work, separate from ordinary product throughput.

### Lifecycle provenance opt-ins

For a supporting native producer, `editor_lifecycle_probe.py --trace` can add
`--shader-provenance` for CPU shader acquisition/link evidence and
`--filesystem-open-failures` for bounded host-open diagnostics. Both explicit
options are validated before IO and enabled after the normal environment
filter. Add `--resource-trace --require-terminal-ack` when those witnesses are
needed. Requested analysis reuses the canonical trace reader and its hashes;
these flags do not upgrade an older binary or certify all modules.
FS details stay in raw WARN artifacts: task/parent/generation/hash/operation,
path/error UTF8prefixes up to128bytes, lengths and truncation. At most64groups
are reserved per Engine session. Error text is not errno; optionality and
delivery remain unknown. The current typed FS-read reader does not decode that
text. Preserve missing/budget-limited evidence, and do not turn CPU records into
GPU completion, physical presentation or a performance improvement.

### Prepass storage evidence

A depth-only request is an internal storage preference, not proof of a depth-only
physical framebuffer. Retain the actual backend, consumer demand, allocated
normal/albedo/velocity/depth handles and target generation. A provider may refuse
the preferred layout and successfully retain the full MRT fallback under the
same preferred request key. Counted ports prove ownership/dispatch only; two
avoided RGBA8 planes have nominal size width*height*8 bytes, not a measured GPU
commit reduction. Compare frozen baseline/candidate executables at identical
scene, resolution, camera, quality and observer state. Check moving image/depth,
normal/velocity feature toggles, allocation failure/reuse and complete shutdown.
Do not describe skipped unused color storage or clearing as a frame-time gain
until same-condition whole-frame measurements support it.

### External sampling clock evidence

Process sampling records one high-resolution performance-counter origin and
aggregate OS-query start/end bounds as exact decimal-string nanoseconds, with
the runtime clock implementation and resolution. Thread/marker/deadline timing
and the injected Windows GPU observer share that origin. Samples span queries;
they are not instantaneous frame states. Verify producer/package identity,
same live clock epoch, export precision and query bounds before any CPU phase
association. A clock label alone is insufficient. JCE resource.sample_ns uses
SDL tick-relative time, unlike raw-QPC trace timestamps. Older records without
anchors stay unavailable, and first_frame_s is file visibility rather than an
actual rendered-frame or startup event. Default required-phase aggregates omit absolute interval witnesses. Use the
explicit required-service witness option below when needed; the clock repair
alone does not supply a join or certify GPU completion or performance.

The Engine-owned FS text budget starts after Engine allocation. App-descriptor
bootstrap configuration reads can retain numeric trace events without path/error
text. This boundary does not prove capture loss or optionality.

### Required-service interval witnesses

The optional canonical reader `--scope-witnesses` retains first/last successful
required-service CPU intervals, timestamps and raw event indices, bounded to128
explicit names. The Editor lifecycle probe accepts the same flag with --trace
and requires actual Play/Stop services. ACK and non-ACK routes reuse the one
complete reader and its fingerprint. Defaults remain unchanged. Samples follow
exported completion order; repeated envelopes are not a full timeline. Combine
only qualified same-run clock domains and producer identities; the option alone
does not align OS samples, SDL-relative resource counters or GPU execution.
Synthetic observer composition and native application evidence remain separate.

For a rebuilt producer, explicit renderer resource DIAG plus enabled Core tracing
adds the `sr.sky.admission` CPU service with scoped `sky.*` counters. It records
the effective caller/view gates, prepared rendering context, CPU frame/view, raw
program index and actual uploaded sky parameter bits. Policy skips succeed;
invalid program sentinels and refused transient allocation fail; a successful
submission means only that the CPU submit returned. Raw-index admission is a
sentinel check, not proof of program generation or backend lifetime, and
`sky.gpu_completion_known=0` never certifies GPU pixels. Do not resolve a program
merely to observe it or infer a base-camera clear mode from overlay-only config.
Keep expected diagnostic failures outside mandatory-success lifecycle phases.
The canonical reader validates all events; its required-phase summary alone is
not a complete projection of arbitrary scoped counters. Retain a bounded
selected-scope projection alongside the same complete validation when comparing
scene/Bundle transitions, and qualify missing observations explicitly. Defaults
remain unchanged; validate the actual rebuilt executable before using this data.

The prepared rendering context identifies the rendering context, not a unique
scene or SceneRenderer object. Join scene identity through its existing owner.

When qualifying a render-order change, pair captured frame identity with
actual draw order, attachment subresources, depth and blend state, and
shader bindings. Compare raw intermediate color and depth before the
next consumer and compare final native frames at two distinct poses.
A serialized draw-duration gain and fewer pixel invocations describe
that replay. Keep live wall-frame ABBA/BAAB evidence separate. Require
stable repeated effects before promoting a default and preserve conflicting
tail or one-percent-low observations. When replay counters drift on the
unchanged shader, report sample coverage and pixel invocations separately.
Bind GPU Engine samples to exact PID/birth and adapter LUID, including
multiple LUIDs for the same PCI identity. Truncated rosters and observer
gaps remain explicit rejection reasons.

For a depth-sampling blend, qualify actual depth SRV and destination attachments
in the same draw. A color-only alias must preserve the underlying color
resource and subresource even if the RTV view identity changes. Check both
blend operations in order and prove that no depth copy or second output
texture entered the pass. Test repeated reuse and retirement on the CPU
separately from the native graphics binding.

For terminal ring exports, record the retained range and overwrite count.
Validate the exact QPC, scope, thread and frame identity of complete scopes
inside the requested Play interval. Missing scopes remain unavailable when
the ring contains only Stop or editor frames. Keep a same-ETL Present match,
per-process GPU Engine intervals, and an exact DWM token-to-schedule match
as separate evidence. Cross-thread QPC overlap alone cannot attribute an
application wait to Present or DWM.
