# benchmark - installed-SDK performance consumer

This C99 application owns benchmark host policy, scenario selection and capture
timing. It includes public API umbrellas only and links an installed JCE SDK.
No internal engine, editor or third-party headers are permitted here.

`src/default_main.c` delegates to the installed SDK's four stock application
callbacks. Its wrapper only selects explicit effect gates through public
pipeline/cvar APIs and records public GPU view statistics. Never access the
included host's private globals. `tools/postfx_fixture.py` owns the first-party
PBR room; `postfx_suite.py` requires live pass submissions, empty/repeated
controls and changed pixels before accepting an effect. These captures are
correctness evidence, not post-effect performance or lighting fidelity.
`postfx_transition_suite.py` checks warm gate changes against independent steady
captures. `report_postfx.py` rehashes the archived binary and each original
picture/log before recomputing and plotting the four-backend result.

The optional `src/default_rt.c` observer uses that SDK's borrowed const accessor
and the existing typed RGBA8 readback queue, recording exact request/completion
frames and row origin. It allocates nothing unless explicit target/output
inputs are supplied. Release its queue before the stock host destroys graphics.
`src/rt_witness.c` is an opt-in intermediate-target inspector in the custom
consumer, using public target enumeration/readback. Its SSR diagnostic binds
the existing public colour/composite inputs; ordinary runs remain unchanged.

`src/main.c` composes the ordinary application, scene, runtime, camera, offscreen
target and post-processing lifecycle. It does not implement a second renderer or
physics solver. Shared diagnostic fixtures are supplied by the public scene API.
`src/probes*.c` owns project-only CPU workload policy and checks actual results
through SDK services. `tools/probe_suite.py` owns its size/worker/shape matrix,
using shared process sampling and identity tooling. These are not engine defaults.
Generated scenes, captures, measurements and executables stay in ignored output.
`tools/report_static_asset.py` rehashes four-backend source artifacts, captures
and raw process/resource records before plotting static liveness and diagnostic
memory. Optional before/after matrices must match original asset, topology and
scene framing and pass the unchanged pixel budget. Resident byte estimates
remain distinct from physical VRAM and process RSS; these are not timing runs.
`tools/startup_suite.py` keeps camera-only startup/readback trials separate
from steady frames. It uses shared OS/live-marker sampling, validates completed
first-frame PNGs and retains polling resolution. Cache state is uncontrolled;
readback completion is never described as physical onscreen presentation.
Explicit startup auto/low profiles are private run-local INI inputs, consumed
through `jce_engine_set_config_path` before boot. They isolate persisted host
preferences; forced low pools/workers on a modern host do not prove old-device
compatibility. The configuration file hash stays with each result.
Vsync is explicit for new startup/steady matrices. `tools/report_startup.py`
rehashes captures and effective configuration, checks live marker ordering,
recalculates steady CSV summaries and plots every trial without removing spikes.
`tools/media_suite.py` reuses the existing native public-video probe, rather
than duplicating decode/seek logic. It owns project-level malformed-file,
paused-preview, monotonic picture/transport, sustained wall-clock lag and
current seek-picture controls. Opt-in muted real-device output validates PCM
consumption cadence as well as video PTS; it does not measure acoustic speaker
latency or post-seek device resume. Full-resolution decode and process memory
remain distinct from GPU upload and editor preview, which need separate tests.
Host files are hashed,
read-only inputs, never redistributed with benchmark source or output bundles.

`src/mutations.c` and `tools/check_metadata_pixels.py` own a warmed opaque-to-blend
edit witness. Cached and uncached images must agree and both must differ from
the unchanged control. This scenario is opt-in and never changes ordinary
steady timing runs. Missing edit markers or images fail the witness.

`src/probes_contacts.c` supplies an independently settled box/floor contact
probe and a deliberately disabled-body control. It uses only public physics
APIs. Count, contact participation, finite supported poses and disabled-pose
invariance are checked outside each timed fixed step. Active probes wake bodies
before the timed interval so a sleeping stack cannot impersonate solver load.

`src/probes_animation.c` owns a first-party 64-joint chain and two linear clips.
Independent analytical skin matrices validate every evaluated joint. Sampling
and two-clip blending are CPU-only measurements, excluding validation and GPU
upload/draw. They must not be labelled total character-frame rendering costs.

`src/probes_streaming.c` exercises a fixed loose-file camera route through
public VFS/world streaming, with exact chunk/entity identity and spawn/despawn
counts. Its cooperative/async timings include polling and correctness checks,
but exclude GPU rendering/upload. Files stay in the supplied run scratch path;
warm OS-cache routes must not be described as cold disk-load or frame timings.

`src/probes_ui.c` times headless Canvas layout/raycast records and validates
every element independently. `src/text_witness.c` supplies four opt-in UTF-8
labels through the same public Canvas API. Fonts are explicit host inputs,
hashed by the witness runner and never copied or redistributed. This capture
composes post output back into the plain bridge before Canvas, reusing the
editor's public composition API and the bridge's readback row convention.
It is not an assertion about native-resolution backbuffer overlay parity.

`src/metrics.c` exports complete JSONL through SDK filesystem APIs. Tagged
allocation totals cover opt-in instrumentation, not the whole heap. Separate
mimalloc process snapshots and OS sampling must retain their measurement scope.

`src/asset_witness.c` observes live imported topology and one renderer-owned
animation player by path plus every matching component's published clock/layer.
Its explicit JCE_BENCH_WAIT_ASSET=1 diagnostic holds only the consumer fixture
clock until the ordinary asynchronous renderer publishes the requested model.
Rendering continues during import; a 60-second watchdog fails missing assets.
It exits after 180 ready fixture frames through the public engine quit API.
The caller supplies a larger engine frame/wall limit and verifies the ready
marker plus topology and captures. This opt-in route is not steady timing,
startup performance, a synchronous loader replacement or an engine default.
`tools/static_asset_suite.py` independently counts unchanged GLB node/primitive
references and verifies actual imported counts, readiness and empty/static/repeat
pictures. Framing is caller input. Optional material extensions, every primitive's
visible coverage and throughput are outside its base static liveness claim.
Sources and binaries are rehashed after the run; failures retain partial suites.
`tools/check_asset_pixels.py` owns character framing,
frozen/advancing/repeated/empty controls and asset hashes. It does not modify
the original GLB or prove independently correct deformation for every instance.
Crowd captures use an elevated orthographic camera and full-image pixel deltas.
They explicitly disable the public TAA console authority after renderer creation;
a startup RenderPipeline descriptor alone does not override that cvar default.
Layer/mask controls compare ordinary and batched paths without engine policy.
The optional capture frame is project policy; default captures remain frame 20.

`src/culling_witness.c` positions SDK-created primitive entities in three known
front/offscreen/behind groups. It disables motion and shadows for this opt-in
frustum comparison and verifies public candidate/visible/culled counters at two
fixed frames. The fixture has one camera, one light and one non-rendering root;
all three conservatively pass the current renderer's entity culling. This is
project test policy, not a new engine culling implementation or default.

`src/occlusion_witness.c` uses the existing public hardware-query culler with
a fixed wall and known hidden/visible groups. Query budget overflow stays
visible and is retained in evidence. Its wall-reveal route never resets the
culler to hide history artifacts. `tools/occlusion_suite.py` compares fixed
and reveal captures with query-disabled controls before timing. Unsupported
hardware is unavailable, and a diagnostic static-only preflight is not full
history acceptance. This fixture does not measure HiZ/GPUScene.
The optional imported-model path assigns neutral UVs through the public
MeshRenderer setter. `tools/gpu_history_suite.py` reuses this same wall/reveal
route with hardware queries disabled to test the separate GPU HiZ path.
Independent projected cells require coverage for every revealed object and
zero changed cells at sampled reveal frames. `tools/report_occlusion.py`
rehashes source captures and recalculates before/after differences with the
same RGB threshold; a failed suite or edited capture cannot become a report.

`tools/gpu_model_suite.py` reuses the front/offscreen/behind arrangement with
an unchanged SDK-packed static GLB. It checks actual compute, upload and
indirect/fixed-count submissions against CPU pixels. The dedicated indirect
switch configures `r.gpu_driven.indirect`; it never spoofs unsupported hardware.
Both GPU tools preserve input counts as inputs. Optional survivor evidence
comes from actual asynchronous GPU readback, independently decoded and hashed.
`src/gpu_survivors.c` optionally consumes the shared color-model snapshot through
the existing typed asynchronous readback queue. It retains dispatch/source-frame
identity and raw float words; `tools/gpu_counter_evidence.py` independently
decodes exact uint32 halves and requires complete known-fixture counts. Static
`--survivors` controls require actual counts, while fixed-count support stays
unavailable for this compacted-counter diagnostic. Measured processes strip all
capture and counter-readback controls and revalidate prior evidence hashes.
Static and sampled reveal witnesses are limited correctness evidence, not
full motion or crossover performance. Wall/reveal `--survivors` additionally
requires half the model inputs before removal and all inputs from frame 80.
`gpu_history_suite.py --motion camera` uses SDK camera translation and yaw
continuously at frames 51..80, restoring the authored pose at 81. It observes
all thirty actual poses plus restoration, compares CPU/HiZ pixels and requires
full conservative GPU survivors through restoration before static half-count
recovery at 82. This opt-in consumer route lives in src/mutations.c; it must
change actual images and must not become engine policy or timing evidence.
The corresponding --motion occluder route changes the wall through ordinary
scene transforms, using the same deterministic wave and restoration frames.
Its actual transforms, revealed pixels and conservative GPU counters have
separate controls; unchanged SDK command logs cannot impersonate movement.
Motion captures/readbacks additionally require first movement at 51 and peak
displacement at 66, not just the near-restored frames 79..90. Pixel comparisons
remain outside timing runs, and the unchanged static/wall-reveal route keeps
its original seven counter request frames.
The motion wave has its sole discrete peak at frame 66; adjacent poses must
change throughout 51..81. A former half-integer peak repeated the camera pose
at 65/66 and legitimately reused depth, contradicting that test's moving-frame
counter expectation. Keep that diagnostic failure instead of changing engine
history rules. Independently projected left cells at peak must contain newly
exposed models outside the moving wall; wall pixels alone are a failing control.
`tools/report_gpu_motion.py` rehashes source artifacts and every raw image,
reparses actual pose/resource logs and GPU words before plotting a complete
four-backend/two-motion matrix. Missing cases, mixed binaries or changed raw
evidence must fail. It remains sampled correctness evidence, never timing.
`src/hiz_probe.c` reuses that asynchronous queue with the public GPUScene helper
and synthetic SDK textures. `tools/hiz_npot_suite.py` requires real survivor
counts for odd source dimensions, odd intermediate mips and a one-texel axis,
including fully covered and fully clear controls. This is diagnostic coverage;
it bypasses ordinary scene rendering and is never frame-performance evidence.
`tools/gpu_time_suite.py` requires the same binary and rehashes the GLB PAK,
scene and every capture. It rereads actual resource logs and independently
rechecks every front cell before interleaved CPU/fixed/indirect/HiZ timings.
PNG readback is absent from timed processes. Build/gate/report work must finish
before these trials; delayed backend GPU records are not CPU/GPU paired samples.

`tools/time_asset.py` requires same-executable character pixel evidence before
whole Player timings; every character cell, original file identity and complete
component population remain checked. It never schedules PNG capture in timings.
The diagnostic durable-flush interval is an explicit measurement control, not
a license to filter spikes from raw CSV samples.
`tools/report_frames.py` rechecks this consumer's raw CSV identities and trial
percentiles before creating separate character/culling charts. Every trial's
maximum and long-frame count remain visible; failed or incomplete input is
refused. Scenario policy remains here instead of entering generic tools/.

Build with `python scripts/jce.py app tests/consumers/benchmark --arch x64 --variant release`.
GPU probes use `JCE_WINDOW_HIDDEN=1`; do not take desktop focus during tests.

`tools/media_color_fixtures.py` generates first-party procedural color inputs
with an explicit external reference executable. `media_pixel_suite.py` retains
independent frame PTS, raw pixels and optional decoded YUV. Its nearest-YUV
oracle uses standard floating-point matrix equations; default FFmpeg RGB is a
separate reconstruction policy. Before/after suites must declare the same
policy. `report_media_pixels.py` rehashes inputs, rereads actual PTS records and
recomputes the reference and differences before charting. These CPU pictures
are separate from device cadence, GPU upload and editor presentation.
`media_suite.py --resume-seconds` opts into current-generation picture/PCM
readiness followed by a fresh muted public streaming voice and actual resumed
clock/frame progression. It does not measure acoustic output delay.

The normal-buffer asset suite owns the original unit Box/Fox framing and
negative pixel controls. It uses SDK components and existing pack/capture
helpers, never private renderer internals. Full deformation fidelity needs an
independent oracle beyond moving normal pixels.

`tools/report_gbuffer_assets.py` owns normal-buffer evidence export. It rehashes
all supplied inputs and recomputes pixel metrics, backend identity and model
clock/topology markers; summaries alone cannot certify a capture.

`src/lod_witness.c` owns only fixture geometry, camera policy and public global
LOD observation. It resolves procedural planes through existing public asset
callbacks and destroys them after the scene renderer. No new engine policy.
`tools/lod_suite.py` validates all route frames against a separate public-contract
distance oracle, with empty/disabled/enabled/repeat pixel controls. Plane triangle
counts are input topology, not independently measured GPU primitive counters.

`tools/report_lod.py` rehashes the archived executable and route artifacts,
revalidates actual backends, all selection frames and pixel controls before plotting.

`tools/light_suite.py` projects one hundred independent receiver patches and
checks point/spot zero, ten, hundred, omitted, repeated and reordered-sentinel
controls. All cases retain one hundred authored components; this is positive
light participation evidence, not component throughput or cluster occupancy.
`tools/shadow_suite.py` compares directional shadow empty, disabled, enabled,
repeated and moved-caster pictures against unshadowed geometry movement. Its
authored cascade/map settings do not certify effective renderer settings, and
pixel liveness alone is not complete shadow performance acceptance. Both tools
retain executable/source/artifact identity and partial failed manifests.

`src/light_transition.c` is an opt-in public-cvar witness: it changes only
Forward+ authority at fixed frames, logs actual readback and leaves the scene
and materials untouched. Missing/invalid authority logs an error and requests
quit, so runners must reject error logs even if the host exits with zero.
`tools/report_lights.py` independently rehashes and recomputes all raw light
captures before exporting a four-backend same-executable correctness heatmap.

`src/particle_witness.c` observes public scene component readiness after the
shared renderer draw. `tools/particle_suite.py` owns equal authored emission
budgets, zero/active/changed/repeated controls and explicit experimental GPU
requests. Component handles and requested capacity are not actual alive counts.
Missing public GPU population observation is unavailable, never R11 acceptance.

`src/stream_route.c` drives public world streaming, resolved model accounting,
actual eviction counters and deterministic return paths. Its dedicated runner
keeps correctness deadlines distinct from timed routes and physical VRAM.
`src/load_witness.c` observes public loader phases at an explicit frame. GPU
upload CPU submission is not GPU completion; decoder setup, integrity checking
and cleanup boundaries are explicitly reported. No audio is played.

The loader consumer shares CPU decode, main-thread upload and owned-payload
cleanup between synchronous `load_witness.c` and `async_load_witness.c`.
The async companion uses the public structured executor and task-result
publication, never waits in a frame, and verifies actual threaded execution
and worker identity. Cooperative execution cannot pass worker-avoidance gates.
Queue/worker/poll/submit elapsed time stays distinct from summed phase work;
GPU submit is not GPU completion. Discard outstanding tasks before destroying
consumer storage, graphics or audio. Raw synchronous artifacts remain unchanged.

The streaming route optional indexed-grid pressure retains the minimal default
control. Expected public model topology comes from bounded explicit inputs.
Route frame tails use raw KPI CSV; sampled process IO covers each complete
prefix run, including startup/cache reads, and cannot be labeled per-step disk
throughput or guessed GPU upload bandwidth.

The optional positive JCE_BENCH_PLAY_FRAME selects the consumer simulation start
(default 10). Particle parity controls use frame 1 on both paths and verify
actual populations; renderer creation staggering is not silently equated with
CPU simulation age. This setting belongs to the benchmark host, not the engine.

`tools/particle_timing_suite.py` requires a rehashed separate burst correctness
suite before bounded frame timing. Frame-50 typed counts finish before the
project-only `JCE_BENCH_PARTICLE_STATS_UNTIL_FRAME` cutoff disables diagnostic
statistics. Zero/unset retains existing diagnostic behavior. Measured frames
start after 120 warm-up frames, without captures or readbacks; captures belong
to the prerequisite process. This 600-frame workload is not a long soak.

`src/light_timing_witness.c` inventories actual light component types and finite
positive intensity once at frame100, outside the measured interval. The timing
runner removes unwanted light entities entirely for actual0/10/100 populations;
it never treats100 zero-intensity components as a zero-component workload.
All100receiver meshes and original light quality parameters remain unchanged.
The hidden timing track retains full720-frame CSVs and measures frames120..719,
with separate visual correctness evidence required for the same executable.

`src/nodraw_witness.c` is an opt-in public-SDK component-presence consumer. It
adds one assetless LineRenderer at frame30 and removes it at70, with actual
public presence/population observations. `tools/nodraw_suite.py` compares eight
cache-on/off warm and independent steady captures. Pixel participation is
required; component presence alone is not rendering proof. This covers the line
family, not terrain/water/trail fidelity or unknown custom render extensions.


`src/physics_pressure_witness.c/.h` and `tools/physics_pressure_suite.py` own the
P00 consumer fixture. Their state belongs to the existing `Benchmark` lifecycle.
Only an explicit `JCE_BENCH_PHYSICS_PRESSURE` input enables it. The observer
borrows the runtime's public physics world; it never creates a second world or
steps the solver a second time. Ordinary benchmark workloads retain their prior
runtime configuration. The P00 tool uses exactly 2,000 dynamic bodies, a separate
static floor where applicable, fixed 60 Hz, solver iteration input 10 and discrete
CCD. Boxes form 20 x 20 five-body columns; spheres and mixed bodies occupy one
spaced floor grid. Frozen rotation is an explicit authored fixture property.
These fixtures do not certify arbitrary rotated stacks or shape equivalence to
another engine's public measurements.

Correctness processes enable contact listeners and full-body JSONL observations
at frames 1, 120, 600 and 900. Actual body count, opaque full handle/generation,
entity identity, finite poses, velocities, contact events and independent
floor/stack support ranges must pass; matching pictures alone are insufficient.
Disabled controls must have no physics world and preserve every authored pose.
Unbound controls have no floor and must follow the gravity oracle. All input,
executable, frame CSV, observation and image evidence stays in ignored output.

Formal timing processes use the same rehashed scene and executable with the
observer disabled: no listener, full-body traversal, capture or JSONL observation
runs in measured frames. Initialization inventories actual body participation
outside the 120-frame warm-up / 600-frame measured interval. Report existing
runtime physics CPU phases only if present, alongside whole-frame measurements;
never invent solver-only timings or submitted work from authored counts.
Public sleep state and internal solver/CCD counters are unavailable. The
stationary velocity proxy is explicitly not sleeping state; these omissions
keep complete P00 acceptance false rather than disguising unavailable metrics.

The P00 JSON canonical component spelling is `Rigidbody`, not `RigidBody`;
unknown registry names are silently ignored by the public loader. Setup logs
retain registry IDs, actual loaded component counts and the first invalid
public body handle to distinguish schema errors from runtime initialization.
Static floor colliders follow the ordinary runtime's deferred proximity policy.
The final exact body-count inventory is emitted after the first normal runtime
step (frame 1), before measured frames; no extra resync or physics step is used.

P00 correctness runs retain 32 ordinary rendered tail frames after the last
numeric observation at frame 900. This lets the existing asynchronous GPU
readback complete; shutdown polling alone is not a replacement for subsequent
GPU frames. Tail frames are diagnostic lifetime overhead, outside the frame-900
pose oracle and separate from the unchanged formal timing interval. A missing
or incomplete final PNG still fails; no synchronous capture bypass is used.

`tools/static_asset_timing_suite.py` uses the stock SDK target for the same
fixed GLB/camera across base and individual effects. It requires actual public
frame-120 view submissions and the opt-in JCE_BENCH_POST_MODEL model census
before seven runs of 600 measured frames; no pixel/readback observer runs in
that timing interval. `stream_route_metrics.py` reports sampled process RSS
and OS read counters separately from tracked model budget; cold physical disk
throughput and renderer upload bytes remain unavailable without sensors.
