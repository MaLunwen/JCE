# Editor/runtime parity

Hidden Editor QA uses an explicitly selected virtual Game pointer source with
the normal runtime commit order. It must not forge OS focus/hover or change
ordinary mouse admission. Record source and accepted state; synthetic success
does not qualify physical desktop input. Reset virtual state on ownership,
geometry, visibility and Play lifetime changes. Preserve failed old-artifact
captures and validate the rebuilt executable through the complete Play/Stop
and shutdown path before claiming the input repair consumed.

Runtime pointer commit and Canvas UI pointer submission are separate consumers.
Use the same bridge's non-consuming, current-geometry-checked virtual peek for
Canvas input; consuming twice would drain Runtime state. A Runtime unit pass
cannot prove UIButton dispatch. Verify actual button effect and before/after
scene observations in the normal rendered host, plus complete Stop restoration.

Canvas button parity includes outside-held entry, drag-out/back-in, A-to-B drags
and recovery after hidden, disabled or invalid-pointer intervals. Only a new down edge owns
a button; an unknown pointer interval needs an observed up before another press.
Verify Play/Stop/restart and empty or rejected render attempts do not replay
prior click/value polling outputs. Preserve InputField text/submit clear-on-read
ownership. Headless owner controls do not certify rebuilt-host physical input.

Editor Play and a shipped game must use shared runtime, scene and input services rather than duplicate game logic. Trace each host's construction, per-frame submission and teardown through its public API.

Compare effective runtime descriptors, camera settings, input actions, script tick order, entity changes, resource mounts, navigation, save paths and streaming ownership. A matching struct definition does not prove both hosts populate it the same way.

Resource lookup may differ between loose project files, cooked directories and embedded PAKs. Test a relocated package without source-tree or loose-asset fallback before claiming embedded portability.

Match render backend and effective rendering settings before a pixel comparison. Verify that temporal passes execute and use the same clock; similar call traces do not guarantee equivalent images.

Windows native D3D Auto uses a private current-window monitor observation to
request a unique vendor/device pair before the existing bgfx init. The result
is queried once per renderer creation and reused across fallback attempts;
unavailable or ambiguous matches retain the previous policy. Editor and shipped
windows can land on different displays, so compare their actual selected device
and fallback reason as well as the requested backend. Do not add adapter
selection to game code or infer that Auto always selects integrated graphics.

The bounded DIST startup record `GPU display adapter` separates observed
vendor/device and LUID, effective requested key, selected caps key, policy,
refusal reason and exact-match flag. `renderer.adapter.startup` adds correlated
reason/observed/requested/selected/exact-match counters to the existing core
trace; exhausted backend initialization ends FAILED. Trace-off emits no ring
work. This span records the CPU startup decision and reported identity, not GPU
completion, native migration or performance. The actual-owner CPU fixtures and
assertion-failing controls do not replace native selection/capture evidence.

Current Windows PE high-performance hints remain
`NvOptimusEnablement=1` / `AmdPowerXpressRequestHighPerformance=1`, and public
`integrated` / `discrete` still mean Intel / NVIDIA brand aliases. Neither those
names nor the Auto match guarantee driver routing or improved Copy traffic.
See [platform and low-end requirements](crossplatform-lowend.md) for the private
selection boundaries; retain explicit display/device/presentation observations
when comparing Editor and executable performance.

Editor Play has an independent preview presentation cap: `editor.frame_rate.play`
defaults to 60, and 0 removes the additional Editor cap. With a positive preview
cap, use the lower of that cap and a positive authored project FPS; an uncapped
project uses the preview cap. With preview 0, retain the authored FPS, including
uncapped 0. Preserve authored VSync and restore the editor entry presentation on
create failure or Stop. Missing render composition still begins the FPS
transaction while retaining entry VSync. This preference does not rewrite the
project/runtime FPS intent, fixed-timestep or capture protocols. Match effective
presentation rates explicitly when measuring host parity.

```bash
python tools/lint/check_runtime_desc_parity.py
python tools/lint/check_project_settings_consumed.py
python tools/lint/check_play_mode_isolation.py
```

Test repeated Play/Stop cycles and language backends in both hosts. Runtime shutdown must not invalidate authoring-scene selection or leak resources into the next simulation. Report the tested language/backend pairs, not merely that a scene opened.

Authored scene restoration and host preview restoration are separate obligations.
Game's stopped preview camera must retain complete pose and projection state
across Play, failure, hidden-view Stop and retry. Keep runtime camera mutation
in its own existing owner; an incomplete public getter snapshot cannot preserve
orthographic scale or roll. Verify both the camera state and rendered views.

Application acceptance includes repeated editor simulation, corresponding
platform executable execution and reopening its packaged bundle in the editor.
Match shared services and effective settings across all three hosts, including
script/runtime lifecycle and relocation. Compilation alone is insufficient.

Submit viewport-local pointer coordinates through `jce_runtime_set_pointer_position`
alongside deltas/buttons/touches. Zero viewport dimensions explicitly mean no
input ownership. Native script modules select cursor image assets through
`JceScriptHost.cursor_set`; the window host owns platform handles and arbitrates
cursor display. Do not create SDL cursors from a module's private static SDL.
Keep cooked mip chains through worker decode and render-thread upload with the
public `JceTextureCpu` service. A base-mip preview decoder cannot preserve render
parity when it reconstructs a cooked chain with another filtering policy.
Use `tools/compare_captures.py` for equal-size captures with explicit regions;
report numeric differences and exclude only documented diagnostic overlays.

UIButton color feedback must respect graphic hierarchy and clipping; verify modal occlusion and child labels using actual captures. Async startup restoration dispatches scene bundles through the isolated bundle loader, rather than parsing their binary bytes as scene JSON.

Canvas child traversal and equal-sort-order root canvases must use stable entity order. ECS component enable/disable changes migrate archetypes and must not change painter or hit-test ordering; keep the overlapping-button enable-toggle regression.

### Pointer cameras and sparse scenes

Authored-camera-only stock hosts reapply the primary scene Camera after script update and before draw. Editor Play pointer ownership does not require a Virtual Camera; map both pointer position and displacement from the displayed Game image to actual render pixels. SDL button events publish their own absolute position before the button transition. An explicit model waiting for loading must not fall through to an unrelated primitive. Camera diagnostics default off and use bounded camera/aspect caches when opted in.

Use public prefab spawn and reuse for dynamic actor populations, and declare script dependency sidecars. Authoring does not update an existing cooked tree: run formal cook before building. The reusable cook driver exposes `--rgba8 --mipmaps` for explicitly uncompressed art with complete color-space-aware mip chains. Realtime UTC epoch milliseconds are separate from monotonic performance durations. Audit the source clock semantics, but follow the user's requested gameplay design. For modern recreation, use one time-based simulation clock for motion and timers, bound catch-up work and declare suspension policy; keep legacy frame-dependent behavior only in explicit diagnostics when useful. Verify state equivalence across presentation rates.

Explicit authored model materials become visible atomically: request all nonempty texture map paths together, and defer both color and shadow submission until geometry and all maps are resident. Pending authored models claim the pass without primitive fallback; incomplete textures must not flash embedded defaults.

SDL gameplay input consumes physical mouse and touch streams once. The private translator rejects touch-generated mouse mirrors and mouse-generated touch mirrors, while raw application/window event dispatch preserves the UI compatibility path. Do not disable touch UI globally or implement per-game mirror suppression. Verify native translator negative controls, browser single/two/three-finger gestures and touch activation of UI controls after SDK refresh.

Pointer-only UI consumers use jce_input_primary_pointer with their logical surface dimensions. It preserves primary-touch release coordinates and supports real input and replay while mouse/touch gameplay queries remain independent. Default Canvas UI reuses this owner; verify a touch press/release actually activates the authored button after filtering SDL mirrors.

The Web live SDL adapter converts inclusive touch endpoints to logical-surface normalization before neutral ingestion. Browser touch gestures must match client-pixel trajectories; keep the correction in the platform adapter, not individual games or UI consumers.

## Lifecycle ownership and complete captures

Reuse the existing jce_trace authority for startup, application service and
shutdown scopes; preserve parent task ownership across nested synchronous work.
JCE_TRACE_EXPORT requests capture and exports only resident records after
producers join. Inspect jceTrace loss and generation metadata before treating
the output as complete. Use tools/lifecycle_trace_report.py with explicit
--require phase names; failed/cancelled, unpaired, reset and overwritten scopes
are unavailable acceptance evidence. CPU scopes do not prove GPU completion.

Listener registration/removal during an event must dispatch entry identities
exactly once: removed listeners never fire, newly registered listeners wait for
the next event, and shutdown cancels the current lifetime. Exercise nested
emits, stale handles, large listener counts and allocation failure. Editor Play
requires a valid pre-play snapshot before mutable runtime creation; release
failed construction's services before restore, and retain failed restore state
for an explicit Stop retry. Repeated Play/Stop must preserve authoring state.
Use the existing performance phases for capture/create/teardown/restore, not
a second gameplay loop or project-specific profiler.

Terminal Play retirement calls Stop only for active Play, a pending request or
a retained restore point. An already retired host keeps its final shutdown
scope without manufacturing another Stop transaction. Exercise fresh, stopped,
pending and active terminal paths, including repeated finalization; retain the
ordinary old-behavior assertion evidence and the failed real-host trace.

For concurrent PostFX pipelines, select TAA records by view_base. The on/off
marker records CPU resolve submission, not the requested toggle or GPU completion.
Do not infer Game state from the last Scene record; an annotated log missing the
requested viewport is incomplete evidence.

Temporal history must match the immediately preceding rendered frame. Invalidate
it on disabled applies, resize, skip and failed history-copy submission. First/resumed
frames use current colour without sampling undefined history; activation and
visual temporal quality still require separate real GPU evidence.

PostFX consumes frame-local matrices and external motion even when apply skips.
A second apply needs fresh inputs. Reset active/output state before validation;
reported motion-blur/DOF activation requires a submitted composite.

A temporary end-frame disarm followed by re-enable without an intervening apply
preserves the immediately preceding history; only an actual skipped/disabled
apply consumes it without replacement. Do not reset on the toggle alone.

PlayerLoop phase and executed-callback spans share core bounded service tracing.
Preserve callback names before mutation, monotonic handles across restart and
shutdown cancellation. Large phases must dispatch without heap snapshots or OOM
truncation. Renderer memory/timer snapshots distinguish resource counts and
transient use, estimates and adapter-wide reporting from owned heap/presentation.

DIST filesystem and sync/finalize asset spans use core trace independently of
Tracy. Actual returned status/slot READY determines success; NULL cleanup is
cancelled. Byte-read zero cannot distinguish EOF/errors and is not success
evidence. Asset finalization is CPU preparation, never GPU completion.

Stock shader ownership records actual loader-call acquisitions in a bounded
batch. The shared production table loader reuses identical name pairs; repeated
table cells and nine scalar PBR aliases borrow and never add acquired refs.
Distinct actual loads may share a cached handle: cancel only their extra acquired
refs before publication. Deferred Web IDs retire their own resolve-time ref.
Use actual duplicate counters and real backend startup/draw/teardown controls;
CPU refcount controls do not certify backend lifetime or measured memory savings.

Relative script/data/cursor addresses resolve through the active mounted VFS
before loose host-path resolution. ISOLATED misses never fall through to host
files or the runtime PAK. Explicit absolute tooling paths stay host-owned.
The capped path uses core's private jce_filesystem_read_internal.h entry: PAK
metadata is checked before eager decompression, PhysFS size before allocation,
short reads fail, and the existing PAK file buffer transfers without a second
copy. Runtime/Lua controls use real archives, an actual oversized asset and
observe zero oversized decompression calls; loose OVERLAY fallback is retained.

Public offscreen targets retain replaced handles in a bounded eight-slot queue,
aged by actual submitted renderer frames. Repeated preparation in the same frame
does not age it, and collection continues while hidden. A full queue rejects new
resize requests while preserving the current target; preparation at its unchanged
extent can still succeed. Renderer shutdown detaches GPU
handles before the backend closes; callers may later destroy the wrapper.
Applications use public offscreen APIs and ordinary lifecycle, not the private
retirement/submission seam. Actual Noop resource-count tests verify ownership;
GPU quality and Bundle reopening need separate evidence. This frame clock is
not a GPU-completion fence or universal device-safety certificate.

CPU instance uploads share the renderer lifetime owner for qualified 32/64/80-byte
layouts. Scene/particle adapters and applications must not add a second arena or
call its private APIs. Engine maintenance keeps owner-thread reservation/commit,
immutable same-epoch encoder bindings, honest partial counts, and separate CPU
staging/backend retirement. The shared 128 MiB logical charge and the explicit
transient reserve are design budgets, not physical residency or GPU-completion
measurements. Verify actual multi-view pixels and submitted populations after
capacity-policy changes; helper tests or a normal process exit are insufficient.
Preserve explicit authored instance payloads when automatic merging is disabled,
and distinguish queue submission statistics from hardware invocation counts.

Host presentation extent is independent of pointer availability, focus and
hover. `jce_runtime_create_with_options` copies the sized
`JceRuntimeCreateOptions` width/height before script `on_start`; the old
`JceRuntimeDesc` and constructors keep their contracts; zero options size keeps
legacy behavior. Both zero dimensions means
unavailable; otherwise dimensions must be finite and in (0, 65535]. Invalid
dimensions, a short nonzero options size or NULL with nonzero size reject
creation. Publish later changes with `jce_runtime_set_viewport_size`; it stores
Canvas pixels before optional `on_viewport_resize` notification, without an input
event, extra simulation tick or timestep change. Scripts can query Lua
`jce.get_viewport_size()` for width/height and relayout in that callback. The
appended `JceScriptHost.viewport_size` uses the existing caller-size copy; absent
callbacks return two zeros. Pointer coordinates remain a separate input channel.

Native DPI hosting uses `jce_window_get_point_size` for cached actual SDL window
points and `jce_window_point_to_pixels` for conversion to drawable/Canvas pixels.
Do not substitute configured logical layout: `jce_window_get_logical` reports
that configuration, while `jce_window_get_size` reports drawable pixels. Creation
and resize refresh the point cache; reads/conversion query no OS. Conversion
does not clamp outside points; invalid dimensions, coordinates or outputs return
false and preserve outputs. ScriptHost and Canvas share the conversion, and
normalized touch scales exactly once; unavailable pointer input still reports
zeros through its separate channel. Window logs distinguish logical, point and
pixel sizes. Exact-owner controls do not replace actual DPI click/ray acceptance.

```bash
python scripts/jce.py test --suite engine --filter "^test_jce_(runtime_viewport(_old_initial|_old_notify)?|window_coordinates(_old_scale)?)$"
```

Scene video hosts should call public `jce_scene_video_advance` once per admitted
simulation/preview tick. It advances decoder transport without texture creation,
pixel upload or draw submission; existing path changes and ECS destruction may
issue one resource retirement command. Ordinary scene rendering prepares the
latest published frame through the private owner, independent of `playing`, so
a finished one-shot retains its last frame and multiple views do not duplicate
uploads. Existing `jce_scene_video_update` remains the combined compatibility
entry. Failed texture preparation preserves the owned old image and retryable
frame counter. Common CPU advance/prepare/retire spans are not GPU fences or
proof of presentation. Exact-owner counted controls remain separate from actual
background lifecycle, rendered video and GPU measurements.

The current exact-owner CPU baseline passes all 15 cases. Ordinary negative
controls report 10/1/3/8 Unity failures when advance uploads, a playing gate
loses the last frame, failed preparation consumes the counter, or the real
render-prefix wire is omitted. CTest accepts those expected assertion failures
with WILL_FAIL; no crash or build failure is counted as a control. This is
counted CPU ownership evidence, not a video-pixel or GPU-completion certificate.

```bash
python scripts/jce.py test --suite engine --filter "^test_jce_scene_video_phases(_old_cpu_upload|_old_playing_gate|_old_failed_counter|_missing_render_wire)?$"
```

## Non-drawable host CPU lifecycle

Desktop minimize is focus loss; mobile/explicit PAUSE is a distinct hard freeze.
Restore queries actual focus and discards one observed suspended simulation
interval without rewriting wall/KPI clocks. Zero drawable pixels admit no render
frame, resize, GPU asset finalize or screenshot hook; background CPU work remains
subject to the shared effective permission. `jce_engine_set_run_in_background`
and its getter are main-thread host properties, default false, not authored
Project Settings accessors. The stock/Editor policy saves and publishes authored
intent subject to manual/OS pause, then conditionally restores its prior value.
Foreground auxiliary hooks retain their existing frequency independently of
runtime pause; that separate consistency gap is not resolved here.
A capture may admit drawing without granting simulation time. Prove background
simulation with an advancing accepted clock and actual scene/camera changes;
a picture alone is insufficient. Natural Editor viewport admission remains a
separate gate. For packaged runtime parity, record the actual AUTO-selected
adapter/backend and committed 3D target as well as the output image extent.

Normal and allowed background updates consume one actual CPU hook owner with
its existing INITIALIZATION frequency, input/actions, fixed clock, subsystems,
UPDATE/app/LATE order. Accepted dt is finite and capped at 0.1 s after fixed-dt
selection. Background ticks consume the same authored pacing owner; they never
count a completed render or satisfy MAX_FRAMES. Async/child/flush maintenance
stays upstream, including frozen paths. A restored zero dt cannot be replaced
by Editor's fixed-dt Play override. Stock entity-cache retirement waits for an
admitted draw. Video advances through the public CPU API and uploads at renderer
prepare; retained one-time texture retirement does not prove device completion.
Actual owner CPU controls and ordinary negatives are distinct from fresh SDK,
physical minimize/restore, Web resize, timing and resource-closure evidence.

### VFS whole-file capacity

Public `jce_fs_read_all` delegates to its existing bounded VFS owner with
`SIZE_MAX-1`. This rejects unrepresentable total/sentinel headroom before eager
PAK allocation and loose-file allocation. Public failure output remains zero;
accepted output is the actual whole length. Internal bounded rejection metadata
continues to retain the observed total. Strict PAK decoded-length validation and
transfer of its existing decoded buffer are explicit behavior changes; do not
claim a common NUL sentinel for PAK and loose-file data. SIZE32 CPU port controls
are arithmetic models, not target-architecture/GPU/performance validation.

### Runtime network interpolation gate

Application-driven transform and animator render steps share the existing
`net_bridged && session_mode != NONE` condition, in that order with the runtime's
clock alpha. Keep prediction/sequencer/video outside this networking guard.
The lower-layer animator intentionally supports direct no-session loopback;
fix the runtime consumer rather than introducing a global session prerequisite
into that API. Four actual-owner call-site CPU cases plus an omitted-braces
ordinary negative complement the unchanged seven L4 animator cases; they do
not prove real network transport or full runtime lifecycle execution.

### Scene resource preparation

Scene/Game explicitly consume the public render-thread resource stage after
editor asset finalization; legacy rendering calls the same owner automatically.
Scene/context/backend CPU frame/cache facts deduplicate unchanged preparation;
new terminal decodes and unuploaded video counters remain observable within a
round. Existing model upload limits and per-complete-render reference/eviction
clock remain distinct. The scoped resource phase records reuse and actual
identity, not GPU completion. It permits uploads/retirements but submits no
scene colour, shadow or compute and performs no simulation/animation advance.
Full rendering stays continuous: this slice alone does not make `live_preview`
effective, justify 5 Hz colour reuse, or prove reduced GPU load. SDK/ABI, actual
Editor reachability and native/Web performance require separate fresh evidence.

### Skeletal actors across views

The existing velocity-frame guard samples skeletal animation once for a shared
renderer. Its membership is now the scene's short animator snapshot, not the
first camera's culled entity list; all six pose passes consume that snapshot in
authored order. Requests and entity IDs reuse one checked per-renderer buffer.
Snapshot failure leaves the generation unstamped for safe same-round retry;
scene changes invalidate the previous scene's accepted generation before prune
publishes the new scene, including same-round allocation failure.
Serial event additions join the next generation and removals retain existing
has/get checks. No host CPU epoch or low-frequency render control is added.
SimLOD camera policy, legacy per-render timing and GPU preparation remain
separate contracts. Whole-owner real ECS/clip/player controls and ordinary
former-wire negatives complement actual SDK, rendered-pose and timing checks.

### Conservative content presence and optional Mixer defaults

`jce_fs_probe_read` returns `JceFsPresence` through the same active reader and
policy as `jce_fs_host_read_all`. Relative builtin VFS sources retain priority;
OVERLAY may reach the host only after confirmed absence, while ISOLATED does not.
Absolute paths and a NULL active reader remain host-owned. A custom reader
without a presence callback returns UNKNOWN; replacing the reader revokes the
builtin probe. UNKNOWN never skips the higher-priority source's actual read.
Exact not-found results, or fresh OK after stat with a confirmed empty PhysFS
search path, permit an absence classification. Observe that search path under
the existing VFS lock and discard stale TLS errors before stat. Fresh query
errors, nonregular paths, allocation failures and unclassified adapters stay
UNKNOWN; fresh OK alone cannot bypass a nonempty higher-priority source. PRESENT and ABSENT
are metadata observations, not successful content reads or durable snapshots.
A successful `fs.probe_read` scope does not certify that bytes were read.

The Mixer loader probes only an implicit NULL/empty-path default. Confirmed
ABSENT skips that read and retains the existing direct runtime PAK fallback or
real seeded defaults; this fallback does not redefine the filesystem policy.
Explicit paths and UNKNOWN still consume the real read service, preserving
active VFS/custom-reader and host-override precedence before the runtime PAK.
Present but malformed data, invalid lengths, allocation/decode failures and
explicit configuration errors remain failures, never optional absence.
`runtime.mixer.configure` records the address hash, selected/attempted source,
outcome, read attempt/failure and applied/default-seeded state once. Original
FAILED IO remains traceable even when the PAK fallback supplies usable content;
seeding defaults must not turn such failures into successful configuration.

Verify real-owner controls for policy precedence, custom readers, packed-only
content, parse/allocation errors and ordinary omitted-wire failures. Separately
exercise actual target OS missing/access/nonregular cases, refresh installed
SDKs and consume relocated PAKs in the application. CPU controls and SDK builds
do not establish GPU completion, rendering quality or measured performance.

### Native cleanup ownership

Prefer `jce_engine_create_owned` and `jce_engine_try_destroy`. A failed creation
may still return a nonnull cleanup owner; preserve it until successful cleanup
clears the slot. Pending cleanup admits no further business callbacks or second
engine session. Never clear an SDL/JNI/Java handle merely because a terminal
result or a void destruction call was observed. The legacy failed-create pending
slot is retried by `jce_engine_destroy(NULL)` or the next legacy create. That
create call drains only and returns NULL even after successful cleanup; configure
and explicitly create the next session afterward. There is no autonomous collector.

Engine teardown also closes private default-async lazy-creation admission before
DRAINING. Exit/subsystem callbacks and the inter-session gap cannot recreate a
default pool; only the next admitted Engine constructor reopens it after owner
publication. Early rejects and legacy drain-only calls must not reopen it.
This does not revoke existing borrows or alter independently owned executors:
an existing default remains queryable until owner-thread shutdown. Standalone
public default shutdown/recreate retains its original behavior. Use the shared
Engine/Core owner rather than adding project-level pool guards. Navigate via
`contracts/module-memory-index.md` to the private session owner and actual
`test_jce_engine_async_retirement` controls. They passed in retirement-r3 on
2026-10-18, with the old close-wire behavior independently failing ordinary
assertions. Requalify changed artifacts; earlier binaries do not inherit this.

Validate failed app initialization combined with sampler timeout, failed join
and failed resume, repeated refusal, callback reentry and eventual exactly-once
release. Keep each refused attempt and its reason in the trace. A completed CPU
teardown does not certify physical GPU completion or whole-frame performance.

For retry captures, consume the full stream with `lifecycle_trace_report.py
--cleanup-ownership` and an actually successful required phase such as
`engine.async_shutdown`. Requiring `engine.cleanup_attempt` with the generic
`--require` gate rejects legitimate earlier refusal. Preserve distinct first-owner
and reboot output paths. Terminal diagnostic order is capture-worker join, logger
drain/join with synchronous file/sink retained, final trace publication, then
logger storage release. A registered-thread count alone does not cover the raw
SDL logger; inspect the completed `engine.log_quiesce` scope as well.

Treat the project's render pipeline as a boot dependency. The shared Bundle
packer canonicalizes `Settings/RenderPipeline.rp.json` to
`settings/render_pipeline.rp.json`; a present invalid/conflicting file fails
packaging. Verify the actual Bundle bytes and the running policy in every host.
Editor Bundle admission uses `jce_fs_read_pak_bounded`/`jce_render_pipeline_load_fs`
on its explicit archive owner, not global loose-file precedence. Snapshot the
requested descriptor with `jce_render_pipeline_get_requested`, since a currently
clamped descriptor cannot preserve project intent across hardware tiers.
`render_pipeline.publish` scopes record effective `rp.*` CPU policy after
resolution; they do not prove GPU pass execution. Restoring omitted project
settings is a configuration correctness fix and must be identified as such in
performance comparisons. Uncapped GPU saturation alone does not establish waste:
also measure the ordinary capped workload and frame costs on the actual adapter.

### Editor content clock during Play

Scene navigation retains its bounded wall-clock delta. The Scene viewport's
reflection, main render, fullscreen content effects and Canvas consume the
accepted Play render delta during a session; Pause publishes zero through the
existing Play owner. A stopped authoring preview retains its original wall
clock. Game consumes the same accepted running delta. Keep all render/resource
admission and the wall-clock timestamp update intact when changing this wire.
CPU routing controls do not establish GPU first-view ownership, temporal pixels
or a power saving. Single-step publication and render-owned absolute clocks
need their own qualification; this rule does not certify them.

## PostFX output and Game overlays

Query `jce_postfx_get_output_target` after apply with the current structure size
and version. Its owned texture/FBO pair carries the actual allocated extent and
format; empty chains report a borrowed input without a writable FBO. Handles
remain borrowed until the next apply, resize or destruction. CPU admission does
not certify GPU completion or context liveness. Overlay submissions must preserve
color and require no scene depth/stencil.

Game View may draw screen-space Canvas directly into that output only on D3D11
with matching current viewport/bridge/output extents and formats, distinct
attachments, and no physics-debug tail. Otherwise retain the bridge composite.
Display and capture must select the same final texture. Scene View and standalone
presentation keep their existing paths. Validate actual pixels and dynamic input
before claiming parity; a removed copy alone is not a whole-frame speedup.

## AudioSource pending playback

`jce_runtime_audio_stop` stops tracked voices and pending play intent, including
a decode that completed before the next runtime poll. Unsubmitted queue slots
are freed immediately. Submitted tasks are disarmed and cancellation is
requested; their borrowed decode storage remains owned until terminal poll or
the existing scene-teardown discard barrier. Stop does not block on a decoder,
and a later Play creates a fresh intent rather than stacking old requests.
Completion also checks the full entity binding and captured clip path before
upload/play, so changed clips cannot consume an earlier decode. Zero voices at
async scene creation alone is expected, not evidence of missing assets. The
CPU owner controls establish these lifetime rules, not decoder fidelity,
audible output, GPU behavior or a whole-frame performance improvement.
