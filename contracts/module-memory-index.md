# Module ownership index

Native natural-size Game image scheduling remains in
`editor/src/core/jce_editor_kpi_game_capture.{h,cpp}` and the existing Game panel.
The scene viewport owns actual current-frame rendering, file readback and
completion; the controller owns only its first-Play schedule and receipt
identity. `tools/editor_play_screenshots.py` belongs to the tools charter and
is consumed by `editor_play_diagnostic.py` for owned PNG validation, bounded
receipts and separately reported screenshot-interval exclusions. It adds no OS
screen capture, process launcher or rendering policy. Existing capture/diagnostic
test owners cover CPU boundaries; native PNG contents and performance remain
independent acceptance work.


Asset Browser directory snapshots: editor/src/panels/jce_panel_assets_directory.h
owns copied scan requests, task-owned results, two active-lifecycle flight slots,
current-folder generations and visible tree FIFO/cache. The existing public
async executor owns scheduling; editor adds no worker pool or filesystem service.
assets.cpp consumes the 350 ms membership poll and preserves synchronous changed
folder materialization. nav.cpp publishes complete tree results before recursion
with a minimum one-second refresh interval, preserving stale valid rows through
work/failure and deferred root invalidation. ui/jce_editor_panels owns its public
init/shutdown hooks; late tasks own no UI state. Cooperative execution remains on
the owner pump, and immediate lifecycle restart can overlap cancelled old jobs
until executor retirement. tests/editor/asset_directory_controls.cmake compiles
the actual owner/caller bodies with real headless ImGui and controlled public
async/filesystem ports; seven ordinary mutants and panel-state hook controls are
separate from native executor/SDL/UI/FPS evidence.

Exact collected-layout identity remains in scene/jce_scene_renderer.c: focus
movement recollects normally and preserves generation only for the complete,
identical ordered result and owner/cohort. Existing scene/jce_sr_model_bounds.c
publishes default and opt-in geometry invalidation; the normal drain batches
default retirement while selective dependency tracking stays opt-in. Scene
entry and public model-cache reset share those owners. Invalid bounds proofs
refresh through a conservative collect-all before returning to exact focus;
both collection modes require valid ownership for reuse. CPU controls live in
tests/middleware/scene/test_jce_collect_layout.c and its registered
collect_layout_controls.cmake; tools/validation/collect_layout_controls.py
prepares actual owner regions and ordinary negative variants. These do not
constitute native renderer, asynchronous-loader or whole-frame acceptance.

Natural visible Editor diagnostics belong to `tools/editor_play_diagnostic.py`.
The explicit checkout supplies process_sampler, perf_bench's log authority and
benchmark_suite's CSV authority. The tool controls one child capture without
autoplay, fixed timestep, viewport forcing or render-quality policy. Its
focused Game CPU spans retain actual clocks, runtime-tick observations and
native writer byte/record integrity; they do not certify GPU presentation or
causal before/after performance. Controls live in
`tools/tests/test_editor_play_diagnostic.py`; tools/AGENTS.md owns the boundary.
Stable comparisons exclude observed lifecycle and mixed-view frames. Recurring
phase ranking requires declared coverage; sparse event averages remain separate.
The first visible CSV marker does not establish native startup or presentation.

The private editor/src/scene/jce_editor_physics_overlay.h owns shared Scene/Game
Play/Pause collider drawing from admitted live native physics shapes, optional
conservative camera segment rejection, bounded shape requests, retained-line
sampling, and scoped flags/sink lifetime. jce_editor_physics_debug_settings.h
reuses the existing UI-state persistence owner for a default 30-metre radius
(0 disables distance only), Current Camera/Player centre and optional frustum.
Both callers use the rendered view; absent Player position falls back to that
camera. application/jce_rt_character.c exposes the current borrowed character
identity; the Bullet bridge resolves its live character slot before giving it
priority and a distance exemption. Shape requests read only bounded native
indices; contacts/constraints retain the separate legacy debug flush.
Game bridge retention consumes the same request, including non-wireframe flags.
renderer/jce_debug_line_sample.h owns whole-line transient reduction used by
jce_debug_draw.c; no prefix fallback, and line rendering preserves scene depth.
gizmo/jce_gizmo_compound_collider.cpp owns cached Edit-only cooked previews,
including full-range triangle sampling and capsule caps; Edit convex hull bounds
are an explicit preview limit. tests/editor/test_jce_physics_overlay.cpp consumes
the actual private CPU owners with SDK-typed ports. The private physics
`jce_physics_debug_query.cpp` owns finite native object/shape/part admission and
bounded normalization of the old query prefix. `jce_physics_debug_range.hpp`
walks the live quantized BVH with explicit node/candidate limits before nearby
triangle quotas; `jce_physics_debug_mesh.hpp` owns bounded indexed reads.
Boundary-intersecting shapes/faces stay whole; LIMIT/UNSUPPORTED/INVALID remain
explicit. No cross-frame pointers, render asset loads or per-frame hull cooking
are added. Stopped authoring and 2D previews keep their existing separate paths.
The native range control links the production query to real Bullet CPU objects;
the overlay/settings controls use typed endpoint fixtures. Neither is a full
JCE host/SDK build, native IDE acceptance, or rendered-pixel validation.

Per-operation readback witnesses remain in renderer/jce_render_readback.c;
its existing slots retain capture-local identities and readiness history.
`tools/readback_operation_observation.py` is an opt-in typed consumer inside
the single canonical lifecycle reader, not a second parser or resource registry.
Backend-stop release never invents readback readiness, device idle or Present.
Legacy callback PNG/video staging consumes the same public format-admission
owner before file/texture/CPU acquisition. Actual backend shutdown intervals
remain synchronous CPU observations around the five existing call sites.

Optional retired-process binding belongs to tools/presentation_report.py. It
checks the existing sampler's PID/birth and retained artifact/receipt bytes;
it does not establish an independent OS-image-to-PID witness, clock bridge,
Present token, scheduler wait or DWM composition causality.

Renderer API admission remains in `jce_renderer_caps.c`; its explicit
compatibility tier does not alter the existing tiers or public snapshot layout.
`contracts/shader-keywords.json` and the existing shader generator own the
stock-profile declaration, generated `stock_profiles.json` and private profile
table. `jce_renderer_shader_profile_internal.h` is the shared runtime predicate;
PAK/FS acquisition and builtin cooking consume the same profile identity.
Invalid skipped cells and valid handle zero retain distinct ownership. A
catalog entry is not driver-link, pixels or complete-feature acceptance.

Lighting's existing `s_lu` owns `jce_light_cookie_internal.h` for typed cookie
creation, binding and release. No array support or compatibility GL/ES means no
cookie allocation/bind; modern fallback is a real two-layer white array.
PostFX owns `jce_postfx_grade_internal.h` inside each pipeline: uniforms and the
placeholder are owned, while authored LUT/size/strength are borrowed. The
compatibility path explicitly omits 3D LUT resources/binds and warns once when
requested; no 2D grading fallback is claimed. Both owners retain partial-failure
cleanup and valid-zero semantics. Counted CPU controls and compiled GLSL
payloads are separate from actual graphics and whole-frame performance.




Core's private `os/core/jce_sha256_internal.c/.h` is the sole SHA-256/volatile
erasure implementation shared with Resource's existing Archive HMAC owner.
Renderer blob program create and destroy live with the shader loader in
`jce_shaders.c` and share its sole owned link transaction. The Engine tail
observation helper lives in Application,
while Renderer owns the narrower backend API clock edges.

Renderer's `jce_shaders.c` owns optional synchronous PAK/FS/overlay payload,
create, link and creator-release provenance through existing Core scopes/ring
and the device's existing DIAG latch. Stack receipts carry capture/context
identity; no global handle registry or submit instrumentation is introduced.
Other loader/deferred/program-final/GPU ownership remains explicitly outside
this initial scope. Real stock/Noop integration and ordinary complete-TU
negative controls belong to tests/renderer/shader_provenance_controls.cmake;
private SHA known answers belong to tests/os/core/test_jce_sha256.c.

Tools shader provenance observations belong to
`tools/shader_provenance_observation.py` inside the existing canonical
`lifecycle_trace_report.py` reader and bounded B/E/loss/EOF authority. Exact
acquisition IDs and unavailable gaps do not imply global handle or GPU/final
program lifetimes. Its tests live in
`tools/tests/test_shader_provenance_observation.py`; tools/AGENTS.md owns the
analysis contract. Terminal acknowledgement reuses this canonical analysis;
cleanup owns bounded session witnesses, with no independent JSON parser.

Offline shader artifact export belongs to `tools/shader_payload.py` and the
opt-in `tools/jce_shader_inspect.c --json --payload-hex` host CLI. Both reuse the
existing public reflection parser and validated payload range; no second BGFX
parser or runtime dependency is introduced. The wrapper bounds and verifies
bytes and exclusively creates output; CPU artifact identity does not imply
active draw consumption. `tools/tests/test_shader_payload.py` owns the controls.

Asset Browser captions remain derived members of the existing FileEntry
vectors. `editor/src/panels/jce_panel_assets_caption.h` owns exact-name/font/
context/atlas/size/spacing/DPI/wrap keyed reuse of the existing two-line byte
truncation result; kind-filter copies warm the original entry. No global path
map or cell virtualization changes selection, rename, tooltip or keyboard
submission. `tests/editor/asset_caption_controls.cmake` consumes this owner and
actual filtered-copy wiring with real ImGui; output/layout/call budgets and
ordinary cache-bypass/stale-key/missing-warmup negatives remain distinct from
native UI and whole-frame performance acceptance.

Console keeps its existing UI state and bounded Engine/Editor log rings as
owners. Intrinsic text geometry is derived there; real ImGui ItemSize retains
fractional row rounding, and all Selectable/Popup submissions retain native
keyboard reach while only offscreen Text is skipped. The shared panels shutdown
sequence calls the private Console shutdown last: disarm resident null-userdata
commands, detach the logger through its synchronous sink barrier, then release
output/session/mutex/cache. Existing Core trace records editor.console.shutdown
and before/after ownership counters; repeated empty close emits no fictitious
cleanup. Complete-owner CPU controls cover layout, real keyboard navigation,
shared shutdown and ordinary missing-wire mutations; native input, logger races,
GPU completion and whole-scene performance require separate evidence.

Scene Overview consumes enabled renderable entities from the existing registry
and the shared entity-focus bounds helper. Restore full entity generations from
registry indices; successful bounds are finite and ordered. Streaming chunk
bounds keep their owner, and an empty non-streaming scene uses the existing
camera reset. The same helper serves entity focus; do not add a second scene
bounds registry. Real Scene/Camera CPU controls and old-index/invalid-bounds
mutations do not establish loaded-mesh bounds or graphical input/pixels.

Scene viewport content time stays with the existing Play owner while navigation
keeps its wall clock. Scene's reflection, main render, fullscreen effects and
Canvas receive the same accepted running delta as Game, including the Play
owner's zero on Pause. Stopped authoring preview retains the original wall
clock. tests/editor/scene_content_clock_controls.cmake imports the actual clock
block and call statements through typed CPU ports; it does not execute the
surrounding full renderer, qualify GPU first-view deduplication, publish a
single-step delta or establish temporal pixels and power savings.

Core FS owns explicit archive-only bounded reads, including absence/limit
outcomes before payload allocation. Renderer owns strict RenderPipeline parsing,
requested intent and effective render_pipeline.publish counters. Editor's
io/jce_editor_bundle_render_pipeline adapter owns temporary Bundle policy through
synchronous editor.bundle_pipeline.prepare/apply/restore scopes. Its actual
Renderer publication is a child of apply/restore; absence-only tier fallback is
successful, rejected preparation/application is failed, and inactive repeated
close emits no fictitious restoration. It reuses the public Core trace and the
existing Editor CPU scope; no cross-frame scope or second policy registry.
Native controls use real archive/RP/Core trace owners; graphical host, complete
descriptor restoration and GPU execution require their separate evidence.

Engine creation/cleanup ownership remains in application/jce_engine.c and the
public application/jce_engine.h. Explicit create_owned/try_destroy slots retain
failed initialization and deferred cleanup; the singleton active owner blocks
replacement and preboot descriptor changes. constructor_active separately guards
the constructor borrow across repeated close requests. jce_engine_cpu_tick.inc.h
stops later phases after a closing request; a currently executing phase keeps its
existing dispatch contract. SDL host draining lives in application/jce_main_sdl.c;
JNI bridge retention in application/jce_jni_bridge.c; Java handle/finally draining
in engine/java/com/jce/JceRuntime.java and Main.java. Application cleanup/host
controls exercise these actual sources and ordinary lost-owner mutations.
CPU owner closure is separate from physical GPU completion and performance.
Desktop JNI's target is jce_application, which alone receives its headers and
definition. The Platform-to-Application include exceptions are removed. Consumer
shared-library links must retain Application's JVM-discovered exports; the
macOS Caged Kingdom metadata force-loads that owner (actual device/link evidence
remains separate). Core logger level/colors/final-close observation are members
of one g_log_status aggregate with the existing init/retirement semantics.

Application `jce_engine_terminal.c` and its private header own one finite,
default-off session/capture/log tail snapshot. The existing SDL/JNI host emits
its once-only acknowledgement after owned destruction returns. Core logger owns
final flush/close API observations; no public logging ABI changes. Tools
`terminal_ack_observation.py` validates the original owned-child stderr and,
when requested, the same complete trace's final cleanup scope/session counter.
`perf_bench.py` supplies this opt-in without adding engine policy to the generic
process sampler. Acknowledgement is distinct from GPU/process teardown or durable
storage. Native terminal/host/diagnostic controls and Python protocol controls
remain separate evidence.

The private application/jce_engine_diagnostics_retirement.h orders terminal
diagnostic workers: Core trace_capture quiescence joins its writer while keeping
the capture, then Core log quiescence drains/joins its backend while preserving
synchronous file/sink output. Final capture publication and logger storage release
remain with those same owners. tests/os/core/diagnostics_retirement_controls.cmake
exercises native joins and ordinary omitted-wire failures. The canonical bounded
tools/lifecycle_trace_report.py consumes cleanup attempt packets through
tools/cleanup_ownership_observation.py; this is not an Engine session ledger.

Core `os/core/jce_async_session_internal.h` and `jce_async.c` own the private
Engine-session lazy-creation latch using the existing default-executor lock,
independently of transient shutdown's `g_default_closing`. Application
`jce_engine.c` opens it only after owner publication and closes it before the
single DRAINING transition; it stays closed until the next admitted Engine.
Existing default borrows/pump/stats and public standalone shutdown/recreate keep
their contracts. This is not revocation or an independent-executor registry.
`tests/os/core/test_jce_async.c` covers Core compatibility; the complete-owner
`tests/application/test_jce_engine_async_retirement.c` and
`async_retirement_controls.cmake` cover actual exit/subsystem callbacks, failed
initialization, constructor borrowing, early rejects, reboot and the ordinary
old-close-wire negative. Source/registration are present; fresh Native results
are still required, rather than inheriting prior retirement-run qualification.

Application launch overrides are strictly parsed once by
jce_engine_launch_policy.h. jce_engine_frame_cap.inc.h remains the sole FPS
rate/deadline/wait owner; session binding preserves the latest requested rate
for actual terminal release. Renderer retains opaque Vsync override state and
resolves both setters through jce_renderer_launch_policy.inc.h, reached from
Application through the existing renderer lifecycle bridge. Transition-only
getter observations are CPU policy evidence, never display acknowledgement.
Nearest module charters describe complete-owner and bootstrap controls.

JCESDKHelpers.cmake owns the installed consumer IPO probe and its private
success-only fingerprint cache. Unknown toolchain/module/launcher identities
and failures retain the real probe. Compiler/module content, effective flags,
policy and process environment participate; test_ipo_probe_cache.py's counted
NONE-language controls do not replace actual same-cache build verification.

Current-executable lookup is the public filesystem API
jce_fs_host_get_executable_path. Its small Core owner forwards to the single
private Platform adapter; it does not guess from argv, cwd or a directory scan.
Windows/Linux implementations reject truncation and invalid UTF-8. Other hosts
currently return unavailable. Actual path and different-cwd controls extend the
existing filesystem-host fixture; supported source branches are not device proof.

Each accepted central renderer submission publishes its CPU scope's retained
context generation, backend serial before/after, and submission ordinal before
the call through jce_renderer_submission_trace_internal.h. Disabled capture
publishes nothing. These fields identify API submission, not GPU completion or
presentation; scope parenting keeps startup/exit separate from normal host frames.

tools/renderer_submission_observation.py is the bounded typed packet consumer
inside lifecycle_trace_report.py's existing streaming/B/E owner. Explicit
--renderer-submissions checks every four-field packet, exact integer ranges,
active identity/thread/category, order and successful closure; it retains only
first/last packet witnesses and counts. It never creates a GPU/presentation join.
ES full-lifecycle regression requests this consumer; older traces without the
packet cannot satisfy that request. tools/tests owns ordinary missing-wire and
malformed/zero/wide-integer/wrap controls.

tools/offscreen_configure_observation.py owns opt-in configuration packets under
the existing canonical lifecycle reader; empty shared view scopes stay unclassified.

The Editor Game input bridge owns separate explicit virtual pointer state for
automation. It shares the Runtime commit order and coordinate mapping while
retaining ordinary OS focus/hover/capture admission. Hidden/changed viewports,
owner changes and Stop clear the virtual state. Synthetic acceptance must be
reported separately from physical mouse/keyboard or whole-UI accessibility proof.

tools/runtime is the public-SDK stock application consumer; tools/build/stock_runtime.py
owns the fixed-cache unsealed component producer. tools/build/stock_app.py owns
terminal-bound publication and offline envelope sealing with outside inspection
work/receipts; the final application directory contains only its EXE. The C reader retains its
borrowed file mapping through default-runtime teardown. Typed Lua/JS-only Bundle
admission and packaged-only content policy do not provide a host security sandbox.
Native compiler payloads, Editor discovery and zero-environment acceptance remain
separate delivery work; a valid manifest is not execution evidence.

The stock v2 envelope holds one existing public secure PAK entry containing the
exact inspected inner Bundle. Public archive authentication, encrypted-entry
flags, uniqueness and decompression budgets are enforced before its normal typed
reader. FNV remains corruption detection only. Destroy runtime/Bundle borrowers,
then the decrypted buffer, then the executable mapping. Producer manifests bind
the owned-cache build key by path/hash; publication does not copy the raw key.
Sealing takes that explicit local input, so this is not zero-configuration
builder distribution. Eight native CPU controls remain the producer's explicit
SDK-consumer gate, separate from root Engine/Editor test domains.

Configure-generated test source is written by tests/cmake/JceGeneratedFixture.cmake
through one content-aware helper. Callers keep their paths, exact mutations and
configure dependencies. Equal content preserves mtime; native no-op claims need
actual frozen-input byte/mtime snapshots and unchanged compile/link actions.

The renderer's private jce_ktx2_writer bridge owns checked RGBA8/RGBA16F KTX1
normalization and readback layout conversion. It preserves the exact legacy
pinned-bimg zero-type convention for reads; new owned output uses typed headers.
Single/full chains and edge/upload/encoder byte limits are explicit, and the
entire payload is checked before decode allocation. CPU controls and ordinary
mutants live in tests/renderer/ktx_container_controls.cmake. They do not certify
external tooling, GPU consumption, opaque metadata semantics or OOM behavior.

KPI CSV encoding remains owned by application/jce_frame_kpi.c. One bounded
22-byte stack prefix per record reuses its frame index without changing rows,
four-decimal duration formatting, context encoding, budgets or buffer ownership.
Actual complete-owner byte/budget/failure controls and the old-per-row ordinary
control live under tests/application/frame_kpi_prefix_controls.cmake. Its optional
alternating-order CPU benchmark includes encode/byte-comparison observation cost;
it is not graphical product performance or a slow-frame cause attribution.

Reflection-probe capture uses the existing canonical renderer readback queue.
scene/jce_scene_probe_capture.c owns source-format selection after target prepare
and authored HDR/LDR conversion. Its radiance renders use the additive public
jce_scene_renderer_render_linear entry; the existing config layout and render
signature remain unchanged. That SceneRenderer owner saves/restores its private
force and derived shader-output flags around the original render call, including
early returns and nested calls. The shared PBR shader flag suppresses output
gamma/dither for this shaded scope; diagnostic/material-preview modes keep their
own output policy, and custom/effect shaders require independent qualification.
Capture requests a reduced pass. Actual wrapper/capture
owner controls live under tests/middleware/scene, separate from GPU radiance.
Renderer readback owns pending CPU destination
lifetime, private non-submitting deferred destroy, bounded retiring admission,
central-submit ready collection and backend-stop fallback. Cancellation does not
imply reclamation or render submission. Actual-owner controls and ordinary
old-free/missing-collect/old-format variants live under tests/middleware/scene;
related queue/central-submit controls remain under tests/renderer. CPU fixtures
do not establish actual GPU transfers or convolution pixels.

Runtime CPU create/destroy/tick and existing phase spans are owned by
application/jce_runtime.c with private jce_runtime_trace_internal.h. Renderer
resize/rebind/vsync/MSAA CPU reset invocation details are owned by
renderer/jce_renderer_reset_trace_internal.h and the existing renderer entries.
They observe call arguments and normal return, not acknowledged backend size
or GPU completion. Complete Runtime/actual reset-entry controls and ordinary
missing-wire/detail variants live under tests/application and tests/renderer.

Editor CPU operation/Play parent scopes are owned by core/jce_editor_cpu_scope.h
and jce_editor_operation_metrics.h; viewport policy/called/admitted/final-reason
observations live in jce_editor_viewport_trace.h and the existing Scene/Game
panel/render owners. Policy decisions and render outcomes use separate enums.
The real-ring/policy/exact admission-prefix controls live under tests/editor;
they do not execute all late renderer or Play owners. Existing cadence and draw
policy are unchanged. Reflection-probe maintenance is owned by
scene/jce_editor_probe_maintenance.h and the Scene renderer wrappers. The admitted
host draw pumps it before layout, independently of Scene panel invocation. A
current-frame busy latch reserves the capture view band through completion,
failure and layout cancellation; Scene/Game resume on the following host pump.
Clear/history restore/shutdown and the Inspector capture Cancel use the same
non-submitting Engine cancellation owner. Both Editor capture entry points use
its success-issued request token and Scene/entity match; terminal/cancel retires
identity, and exhausted generations refuse reuse. An old Inspector request cannot
cancel a newer batch-panel request. Arbitrary SDK starts bypassing this Editor
owner are outside that local identity guarantee. Actual host, destructive and Inspector
branch controls remain under tests/editor; CPU ports do not certify physical UI,
GPU readback or the later convolution handle that capture does not expose.

Current-capture grass colour-branch admission summaries are owned by scene's
jce_sr_grass_gate.c and jce_sr_grass_gate_internal.h, with fixed per-renderer
state in jce_sr_internal.h and actual pass wiring in jce_sr_draw.c. Eight view
summaries rebaseline on capture/resource epochs and publish first/changed values;
overflow explicitly limits coverage. Existing admission/logging remain separate.
Actual-owner, missing-publication and old-history-mask controls live under
tests/middleware/scene, owned by the scene/test charters.

Optional selected-page classification is consumed by tools/memory_commit_probe.py
through the existing windows_memory_reader.py native query port. Actual committed
spans preserve reserved holes; ordered page batches, raw flags, unknown/invalid/
valid counts, identity/QPC and coverage stay separate from OS RSS/private commit.
The complete fixture and CLI controls live in tools/tests/test_memory_commit_probe.py
under the tools charter. No target contents, suspension or page trimming are used.

Allocation-counter synchronization is owned by os/core/jce_memory.c with the
private os/platform/jce_native_atomic_word_internal.h compiler adapter. Static
native-word operations add no heap/SDL/lock/lifetime; shared counters and peak
CAS are individually atomic, not a global snapshot. One frame-delta consumer
owns its baseline. Actual Debug/NDEBUG-disabled-tracking worker fixtures and the
single-thread old-plain protocol control live in tests/os/core under the core,
platform and test charters. Public allocator signatures are unchanged.

Partial component JSON ownership and checked recursive merge are owned by
engine/src/middleware/scene/jce_scene_json_patch_internal.h, included by the
scene JSON TU. The shared private jce_scene_line_colors_internal.h owns the
LineRenderer colour schema and conservative existing-component colour-only
admission; full parsing and the partial route share conversions. Live storage
is borrowed only for the call, then copied through the typed setter; generic
fallback and parser canonicalization remain authoritative. Actual-owner controls
cover retained fields, OnSet, allocation work and ordinary negative variants.
Existing serializer-owned trees transfer without whole-tree cloning. Explicit construction/merge failure refuses before parse/set; void
serializer/parser internal failures remain outside that guarantee. Actual-owner
and ordinary old-clone/fail-open controls live in tests/middleware/scene under
the scene/test charters.

PointLight cull-write classification is privately owned by scene/
jce_scene_point_light_write_internal.h and consumed only around its existing
typed setter in jce_scene.c. Proven independent intensity-only writes retain
the entity-cull generation while real ECS set/OnSet remains unconditional.
Complete Scene/Flecs, actual selection and narrow current uniform-payload
controls live in tests/middleware/scene/test_jce_point_light_cull_write.c and
point_light_cull_write_controls.cmake under the scene/test charters.

Bounded text-run caching remains owned by renderer/jce_text_shape.c: 256 entries,
four ways per set, 64 admission cursor bytes and previous-run borrowing protection.
Deterministic collision and actual hit/miss-verified CPU timing controls live in
tests/renderer/test_jce_text_shape_cache.c; Debug timings are not product FPS.

Custom PBR program provenance is owned by renderer/jce_pbr_material.c and its
private jce_pbr_custom_trace_internal.h. Cache-miss load spans retain request,
resolved stage and actual host-API/VFS-API/engine-PAK route identities, observed
fallback stages and program-handle admission. Cache hits retain existing behavior
without new observations. Stage-read children do not cover preceding path
resolution; the parent covers both. Actual-owner CPU tests and publication
omission controls live in tests/renderer/test_jce_pbr_custom_trace.c and
pbr_custom_trace_controls.cmake, under the renderer/test charters.

Legacy environment value policy: public os/core/jce_env.h, consumed by api_core,
application trace policy, renderer optional stats and editor perf restoration.
The pure inline predicate borrows a stable string and has no getenv/IO/state.
NULL, empty and initial '0' are false; other nonempty values retain legacy truth.

Private instruction-pointer sampler: engine/src/os/core/jce_sampler.c/.h,
owned by the core charter. Its confirmed-join/recovery barrier precedes dependent
owner teardown in application/jce_engine.c. Tests share authored native controls
through tests/os/core/stubs/jce_sampler_test_owner.h and compile the actual owner.
The bounded roster capacity control in tests/os/core/sampler_capacity_controls.cmake
restores only the prior cap in the complete owner; direct CPU cases cover late
thread entries, high-index histogram isolation and retained-history overflow.
Historical identity and logical release do not certify native OS recovery,
physical residency, GPU work or timestamped hitch attribution.

Application resource observations: engine/src/application/jce_engine_resource_observation.h is owned by application; tools/lifecycle_trace_report.py and resource_trace_observation.py share the canonical bounded event/footer authority under tools. Optional process/thread and Windows GPU Engine observations are owned by tools/process_sampler.py with the lazy windows_gpu_observation.py PDH adapter; memory_residency_probe.py consumes that same sampler. Explicit PID/birth subjects and per-engine rates do not establish presentation or complete GPU ownership.

AGENTS.md is the root engineering authority. Each charter below owns the
rules for its module; follow the closest applicable charter. This index is a
maintained map, not a delivery plan or a snapshot of feature counts.

Editor positive FPS-bound composition is owned by
editor/src/core/jce_editor_frame_rate.h under the core charter, consumed by
authoring cadence and the existing Play presentation/host adapter. The engine
deadline and shared Runtime lifecycle-policy owners retain their existing scope.

Editor native authoring image leases remain in
editor/src/core/jce_editor_script_backends.cpp under the core charter. They
consume the shared public exclusive host-copy owner and existing C/C++ module
registry; original module paths retain project identity. Normal Editor shutdown
closes leases only after state/instances are released.

| Scope | Charter |
| --- | --- |
| Repository | [AGENTS.md](../AGENTS.md) |
| cmake | [cmake/AGENTS.md](../cmake/AGENTS.md) |
| engine/cmake | [engine/cmake/AGENTS.md](../engine/cmake/AGENTS.md) |
| conan | [conan/AGENTS.md](../conan/AGENTS.md) |
| editor | [editor/AGENTS.md](../editor/AGENTS.md) |
| editor/resources | [editor/resources/AGENTS.md](../editor/resources/AGENTS.md) |
| editor/src/core | [editor/src/core/AGENTS.md](../editor/src/core/AGENTS.md) |
| editor/src/dialogs | [editor/src/dialogs/AGENTS.md](../editor/src/dialogs/AGENTS.md) |
| editor/src/game_modules | [editor/src/game_modules/AGENTS.md](../editor/src/game_modules/AGENTS.md) |
| editor/src/gizmo | [editor/src/gizmo/AGENTS.md](../editor/src/gizmo/AGENTS.md) |
| editor/src/io | [editor/src/io/AGENTS.md](../editor/src/io/AGENTS.md) |
| editor/src/panels | [editor/src/panels/AGENTS.md](../editor/src/panels/AGENTS.md) |
| editor/src/panels/material_graph | [editor/src/panels/material_graph/AGENTS.md](../editor/src/panels/material_graph/AGENTS.md) |
| editor/src/scene | [editor/src/scene/AGENTS.md](../editor/src/scene/AGENTS.md) |
| editor/src/shadergraph | [editor/src/shadergraph/AGENTS.md](../editor/src/shadergraph/AGENTS.md) |
| editor/src/ui | [editor/src/ui/AGENTS.md](../editor/src/ui/AGENTS.md) |
| editor/src/viewers | [editor/src/viewers/AGENTS.md](../editor/src/viewers/AGENTS.md) |
| editor/src/widgets | [editor/src/widgets/AGENTS.md](../editor/src/widgets/AGENTS.md) |
| engine/include/jce | [engine/include/jce/AGENTS.md](../engine/include/jce/AGENTS.md) |
| engine/include/jce/middleware/net | [engine/include/jce/middleware/net/AGENTS.md](../engine/include/jce/middleware/net/AGENTS.md) |
| engine/include/jce/middleware/physics | [engine/include/jce/middleware/physics/AGENTS.md](../engine/include/jce/middleware/physics/AGENTS.md) |
| engine/include/jce/middleware/scene | [engine/include/jce/middleware/scene/AGENTS.md](../engine/include/jce/middleware/scene/AGENTS.md) |
| engine/include/jce/renderer | [engine/include/jce/renderer/AGENTS.md](../engine/include/jce/renderer/AGENTS.md) |
| engine/include/jce/runtime | [engine/include/jce/runtime/AGENTS.md](../engine/include/jce/runtime/AGENTS.md) |
| engine/java | [engine/java/AGENTS.md](../engine/java/AGENTS.md) |
| engine/resources | [engine/resources/AGENTS.md](../engine/resources/AGENTS.md) |
| engine/shaders/graph | [engine/shaders/graph/AGENTS.md](../engine/shaders/graph/AGENTS.md) |
| engine/src/application | [engine/src/application/AGENTS.md](../engine/src/application/AGENTS.md) |
| engine/src/middleware/ai | [engine/src/middleware/ai/AGENTS.md](../engine/src/middleware/ai/AGENTS.md) |
| engine/src/middleware/animation | [engine/src/middleware/animation/AGENTS.md](../engine/src/middleware/animation/AGENTS.md) |
| engine/src/middleware/audio | [engine/src/middleware/audio/AGENTS.md](../engine/src/middleware/audio/AGENTS.md) |
| engine/src/middleware/net | [engine/src/middleware/net/AGENTS.md](../engine/src/middleware/net/AGENTS.md) |
| engine/src/middleware/physics | [engine/src/middleware/physics/AGENTS.md](../engine/src/middleware/physics/AGENTS.md) |
| engine/src/middleware/save | [engine/src/middleware/save/AGENTS.md](../engine/src/middleware/save/AGENTS.md) |
| engine/src/middleware/scene | [engine/src/middleware/scene/AGENTS.md](../engine/src/middleware/scene/AGENTS.md) |
| engine/src/middleware/ui | [engine/src/middleware/ui/AGENTS.md](../engine/src/middleware/ui/AGENTS.md) |
| engine/src/middleware/video | [engine/src/middleware/video/AGENTS.md](../engine/src/middleware/video/AGENTS.md) |
| engine/src/middleware/world | [engine/src/middleware/world/AGENTS.md](../engine/src/middleware/world/AGENTS.md) |
| engine/src/os/core | [engine/src/os/core/AGENTS.md](../engine/src/os/core/AGENTS.md) |
| engine/src/os/platform | [engine/src/os/platform/AGENTS.md](../engine/src/os/platform/AGENTS.md) |
| engine/src/renderer | [engine/src/renderer/AGENTS.md](../engine/src/renderer/AGENTS.md) |
| engine/src/resource | [engine/src/resource/AGENTS.md](../engine/src/resource/AGENTS.md) |
| engine/src/runtime | [engine/src/runtime/AGENTS.md](../engine/src/runtime/AGENTS.md) |
| engine/tools_include | [engine/tools_include/AGENTS.md](../engine/tools_include/AGENTS.md) |
| engine/ui | [engine/ui/AGENTS.md](../engine/ui/AGENTS.md) |
| examples/caged_kingdom | [examples/caged_kingdom/AGENTS.md](../examples/caged_kingdom/AGENTS.md) |
| tests/consumers/benchmark | [tests/consumers/benchmark/AGENTS.md](../tests/consumers/benchmark/AGENTS.md) |
| tests/consumers/elemental_serenity | [tests/consumers/elemental_serenity/AGENTS.md](../tests/consumers/elemental_serenity/AGENTS.md) |
| tests/consumers/minesweeper | [tests/consumers/minesweeper/AGENTS.md](../tests/consumers/minesweeper/AGENTS.md) |
| tests/consumers/render_liveness_lab | [tests/consumers/render_liveness_lab/AGENTS.md](../tests/consumers/render_liveness_lab/AGENTS.md) |
| tests/consumers/snake_seven | [tests/consumers/snake_seven/AGENTS.md](../tests/consumers/snake_seven/AGENTS.md) |
| tests/consumers/td_lab | [tests/consumers/td_lab/AGENTS.md](../tests/consumers/td_lab/AGENTS.md) |
| scripts | [scripts/AGENTS.md](../scripts/AGENTS.md) |
| tests | [tests/AGENTS.md](../tests/AGENTS.md) |
| tests/editor | [tests/editor/AGENTS.md](../tests/editor/AGENTS.md) |
| tools | [tools/AGENTS.md](../tools/AGENTS.md) |
| tools/audit | [tools/audit/AGENTS.md](../tools/audit/AGENTS.md) |
| tools/build | [tools/build/AGENTS.md](../tools/build/AGENTS.md) |
| tools/coverage | [tools/coverage/AGENTS.md](../tools/coverage/AGENTS.md) |
| tools/lint | [tools/lint/AGENTS.md](../tools/lint/AGENTS.md) |

Native standalone editor build and PE verification: tools/build/standalone.py,
owned by tools/build/AGENTS.md; editor selection: JCE_EDITOR_STANDALONE.
The shared PE reader retains normal and delayed DLL/symbol imports, including
explicit unknown ordinal coverage. Default Windows delivery rejects four known
post-Windows-7 API dependencies; this bounded check is not an OS certification.
Core sampler/crash names use the private Platform
jce_thread_description_native_internal.h borrowed-module runtime resolver;
missing capabilities retain thread-ID labels and caller-owned LocalFree.
Isolated native embedded SDK outputs, request/cache admission and atomic retry
states: tools/build/sdk_outputs.py under the same charter. SDK target metadata
remains owned by cmake/JCESDKTarget.cmake and tools/build/sdk_target.py; command
port controls are tools/tests/test_sdk_outputs.py, separate from native proof.

Model-local CPU/GPU texture ownership: engine/src/renderer/jce_model_texture_pool.c/h;
owned by engine/src/renderer/AGENTS.md; material slots borrow unique resources.

PostFX attachment ownership: engine/src/renderer/jce_postfx.c under the same
renderer charter. TSR history/motion and exposure metering framebuffers borrow
pipeline-owned textures;
tests/renderer/test_jce_postfx_tsr_ownership.c compiles the actual first-party TU
with counted attachment providers and Noop constructor resources. Its scoped
pinned-header exception is documented in tests/AGENTS.md; it is not GPU-fence
or physical-memory evidence.

Core-5 editor measurements: tools/benchmark_suite.py and tools/benchmark_scene.py;
process telemetry: tools/process_sampler.py; owned by tools/AGENTS.md.

Editor host exit sequencing: editor/src/jce_editor_main.cpp uses the existing
core/jce_editor_play_transaction.h helper and jce_state_stop before viewport/UI
teardown. The editor charter and application-lifecycle contract own this order;
tests/editor/test_jce_editor_play_transaction.cpp checks counted callback order,
while actual running-editor closure verifies script destruction separately.
Native module dependency loading: engine/src/os/platform/jce_library.c;
regression: tests/os/platform/test_jce_library.c and its sibling DLL fixtures.

Public ownership and ignore boundaries: [source-layout.json](source-layout.json).
Public skill: [JCE en-US](../skills/jce/SKILL.md);
[zh-CN translation](../skills/jce-zh-cn/SKILL.md). Locale inventory: [skill-locales.json](skill-locales.json). `docs/`, `.docs/`, and the retired ignored `private/` are local-only;
they never supply public checks or build inputs. Review any relevant old
implementation and place it under the ordinary public engine/, tools/, or
tests/ owner before staging and committing it.

Shared editor/Player stress fixtures: engine/src/middleware/scene/jce_scene_stress*.c;
public API: engine/include/jce/middleware/scene/jce_scene_stress.h.
Volumetric fog look authoring and wind: engine/src/middleware/scene/jce_scene_fog_look.c.
Physical cloud full/half-resolution targets, temporal history and opaque composition: engine/src/middleware/scene/jce_sr_cloud_temporal.c.
The same owner publishes only the current scene/camera/view/frame cloud texture
for water. `jce_sr_water_clouds.h` binds it through the otherwise unused planar
slot. `shaders/standard/sky_cloud_geometry.sh` shares shell intersection with
`shaders/pbr/water_cloud_layer.sh`, which owns complete-layer straight-alpha
composition. Crossing surfaces and nonlinear sky grades retain their old path.
Modern water near-grid spacing: `engine/src/middleware/scene/jce_sr_water_mesh.h`;
scene-owned nearshore resolution and total-cell admission: `jce_water_nearshore_config.h`,
`jce_scene_water_nearshore.c`, and `jce_water_field.c`. Same-revision stage-13
height/depth/foam/velocity packing and CPU render-height extension live in
`jce_water_shore_atlas.h`; `jce_sr_water_shore_gpu.c` owns texture lifetime.
One dry texel extends the adjacent wet surface relative to the still plane,
without editing solver bed/depth/flow; `jce_water_shore_state.c` uses the same
height rule for CPU queries. Actual packer filtering/padding controls extend
`test_jce_water_field.c`. Continuous macro fragments, smooth fBm foam pores,
monotone coverage quantiles, independent footprint filtering and bounded flow
phases live in `shaders/pbr/water_froth.sh`; their input remains physical density.
Unresolved bands preserve mean coverage and use caller-computed derivatives.
CPU budget/clock/radial-spacing controls extend `tests/middleware/scene/test_jce_water_field.c`.
The private fog renderer publishes the scene snapshot to the existing fog march.
Scene animation storage/lifecycle: engine/src/middleware/scene/jce_sr_anim_storage.*
and jce_sr_anim_lifecycle.c own growing generation-keyed instance records and
their retirement. Per-frame evaluation remains in jce_sr_anim.c.
SSGI idle retention: engine/src/middleware/scene/jce_sr_ssgi_lifetime.c/.h
owns complete observed renderer CPU frame accounting and reset/release. Reduced
passes do not advance it; ordinary views OR their demand. Gaps reset the proof;
entity-cache reset retains the module. CPU owner controls live in
tests/middleware/scene/ssgi_lifetime_controls.cmake; draw/composite order, GPU
release and whole-frame performance require separate evidence.
Scene hardware-query history validity: engine/src/middleware/scene/jce_sr_occlusion_history.h.
Velocity world-transform snapshots: engine/src/middleware/scene/jce_sr_velocity_world.c
owns full-entity history, host-generation latching and probe-chain-preserving retirement;
the cull prepass consumes it without advancing history per viewport.
Shared camera-depth identity and TAA-independent Hi-Z matrix:
engine/src/middleware/scene/jce_sr_depth_history.c.
Optional shared color-model GPU survivor export: engine/src/middleware/scene/jce_sr_gpu_diagnostics.c;
GPU snapshot generation stays in engine/src/renderer/jce_gpu_scene.c and uses
the existing typed asynchronous render-readback queue for CPU transfer.
SDK-only Player host and capture policy: tests/consumers/benchmark/src/main.c.
Stock SDK host effect witnesses: tests/consumers/benchmark/src/default_main.c and
tools/postfx_suite.py, postfx_transition_suite.py and report_postfx.py under
that project. Typed stock target readback: src/default_rt.c; custom host target
sampling: src/rt_witness.c. No renderer implementation is copied there.
Complete-frame CSV encoding and scratch: engine/src/application/jce_frame_kpi.c/.h.
The private jce_frame_kpi_writer.c/.h owns copied bounded queues, physical IO
and actual async completion/close lifetime over a borrowed executor; threaded
and cooperative capabilities remain distinct. jce_engine_kpi_capture.c/.h
owns per-engine orchestration and terminal qualification. Completed render
capture omits initialization and includes the terminal current interval through
the existing private hitch/phase boundary owner before writer finish. Clock/interval
continuity remains with jce_engine.c and jce_engine_perf_hitch.c. Owner controls
live in tests/application/test_jce_frame_kpi.c, test_jce_frame_kpi_writer.c,
test_jce_engine_kpi_capture.c and test_jce_engine_kpi_lifecycle.c. Safe cleanup
is not capture success; logical budgets and CPU controls do not certify GPU
completion, physical memory residency or storage latency.

Explicit capture queue budgets: jce_engine_kpi_capture.c/.h owns the latched
JCE_KPI_WRITER_QUEUE=bytes,records selector. Empty preserves existing normal
and LOW_MEMORY defaults; positive decimal capacities are bounded at64MiB/65536
and validated atomically before file IO/allocation. Writer FULL/retirement
semantics stay with the existing writer; payload, descriptor and encoder scratch
budgets remain distinct. This option does not bound synchronous sink latency.

Explicit private frame CSV context: JCE_FRAME_LOG_CONTEXT=1 selects the fourth
context_json column. jce_engine.c samples begin/end window/native dimensions
and backend indices, sealing only a completed render for the existing delayed
export. Default captures keep legacy bytes and perform no extra window queries.
Actual helper/admission wires and ordinary missing-wire controls live in
tests/application/test_jce_engine_frame_context.c and test_jce_engine_context_admission.c.
tools/benchmark_suite.py validates all rows and retains aligned packets;
tools/perf_bench.py matches requested schema and actual header/payload bytes.

Project metadata persistence: engine/src/application/jce_project.c merges only
its public structure's fields into strictly admitted existing JSON, preserving
unknown nested nodes. Invalid/unreadable/ambiguous roots refuse publication;
exclusive creation and existing atomic replacement retain their FS owners.
Load/new refuse partial owned allocations and release constructed prefixes.
editor/src/core/jce_editor_project.cpp transfers edited slots only after save,
preserving unrelated borrowed objects/strings and failed-save cache contents.
Its private source-semantic baseline refuses stale/unverifiable known-field
updates with a reload diagnostic; formatting/extensions are excluded. Refresh
after publication follows decoded disk semantics, not cached cleared values.
This is not a manifest lock or compare-and-swap.
Actual owner controls: tests/application/test_jce_project_script_modules.c,
test_jce_project_allocation.c and tests/editor/test_jce_editor_project_persistence.cpp.
Typed allocation controls cover this owner's allocations, not all heap/JSON OOM.

Normal/albedo shader encoding: engine/shaders/pbr/fs_gbuffer_body.sh; shared
current/previous skin math: vs_gbuffer_skin_body.sh in that directory. Minimal
normal-only and velocity wrappers declare their own complete IO interfaces.
Original Box/Fox normal-buffer controls: tests/consumers/benchmark/tools/gbuffer_asset_suite.py.
Normal-buffer evidence export: tests/consumers/benchmark/tools/report_gbuffer_assets.py.
Root AGENTS.md owns graphical low-end, full single-application and public AI CLI
consumer boundaries; synchronized Skill references provide their procedures.

Global LOD renderer eligibility: engine/src/middleware/scene/jce_sr_draw.c.
SDK global-LOD route witness: tests/consumers/benchmark/src/lod_witness.c/.h and
tests/consumers/benchmark/tools/lod_suite.py (fixture correctness, not streaming).

Global LOD evidence export: tests/consumers/benchmark/tools/report_lod.py.

Clustered-light camera projection: engine/src/renderer/jce_light_cluster.c.
Forward+ fast-path admission: engine/src/middleware/scene/jce_sr_draw.c and
jce_sr_environment.c (unsupported fragment families use existing fallbacks).
Independent light/shadow pixel controls: tests/consumers/benchmark/tools/light_suite.py
and shadow_suite.py; these are consumer correctness fixtures, not full timing.
Light participation evidence export: tests/consumers/benchmark/tools/report_lights.py.
Public Forward+ warm transitions: tests/consumers/benchmark/src/light_transition.c/.h.

Shared directional receiver depth bias: engine/shaders/pbr/shadow_bias.sh.


Static cascade inspection: engine/src/middleware/scene/jce_sr_debug_targets.c.
Cooperative build resource ownership: tools/build/cache_lock.py, integrated
by tools/build/jce.py; original dependency trees remain untouched.

Native Windows IDE preparation: tools/build/ide.py reuses the shared driver,
MSVC/Conan/layout owners and cache leases. tools/build/ide_jsonc.py owns local
JSONC preflight, exact backups, guarded publication/rollback and generated-entry
identity; tools/build/ide_modkit.py resolves one selected build's shader paths.
tools/build/conan_msvc.py admits the active target compiler/linker and optional
developer-shell markers, then binds normalized cl/VS paths in Conan host-context
configuration. Shared clean MSVC activation requests the same toolset prefix
through vcvarsall. The shared generation fingerprint owns reuse; no separate
compiler discovery or build-context override is introduced. Counted path and
command controls are registered in tools/tests/test_build_driver.py; they do
not certify Windows execution or package ABI compatibility.
tools/build/ide_cache.py owns bounded source/generator admission and selective
retirement of stale CMake system/compiler descriptions across retained versions.
It reuses ide_jsonc's byte/alias checks and transaction with original cache and
file backups. The existing rollback-local command dispatches the metadata receipt
owner and guards current identities; target objects, executables and dependency
caches are outside retirement. Real CMake/Conan controls qualify the recovery
mechanism separately from native Windows/IDE execution.
Local CMakeUserPresets never enter public source. Root CMakePresets.json owns
the visible native target/variant menu and external architecture/toolset;
layout_contract.py validates its paths, inheritance and effective host lanes.
ide.py prepares that canonical cache, hides only unchanged owned legacy aliases,
and binds VS Code native tasks/debugging to the active CMake preset/target.
Actual Python controls are tools/tests/test_ide_build.py, test_ide_modkit.py and
test_build_layout_contract_v10.py; real CMake parser/build fixtures and native
compiler/IDE execution have separately declared scopes. The charter is
tools/build/AGENTS.md.

Exact authored camera pose and interactive FPS policy: engine/src/renderer/jce_camera.c;
public contract engine/include/jce/renderer/jce_camera.h. Explicit forward/up
remain authoritative; FPS operations own their pitch limit and safe basis.
The public `jce_camera_clone` copies complete value state into independently
owned storage in that same camera owner. Editor `scene/jce_editor_game_render`
uses it for Play without mutating stopped preview state; `core/jce_editor_play`
owns admission/rollback/Stop ordering. Renderer and Editor fixtures exercise
these separate responsibilities; `tests/sdk_smoke` consumes the installed C ABI.
Consumer particle diagnostics: tests/consumers/benchmark/src/particle_witness.c and
tests/consumers/benchmark/tools/particle_suite.py; no second particle renderer.

GPU particle free-slot allocation and optional typed population diagnostics:
engine/src/renderer/jce_gpu_particles.c, jce_gpu_particles_internal.h and
engine/shaders/particles/cs_particle_free.sc, cs_particle_stats.sc.
Scene particle limits: engine/src/middleware/scene/jce_sr_particle_limits.h;
public existing debug-target enumeration owns the consumer inspection boundary.
Public SDK load-phase consumers: tests/consumers/benchmark/src/load_witness.c/.h and
async_load_witness.c/.h; synchronous and worker paths share CPU decode helpers.
Rendered residency routes: tests/consumers/benchmark/src/stream_route.c/.h and its
project-owned grid fixture tools; tracked model bytes are not physical VRAM.

CPU particle instance upload adapter: engine/src/middleware/scene/jce_sr_particle_batch.c
and jce_sr_particle_batch.h consume the renderer shared instance owner below;
there is no independent particle arena or capacity planner. Scene diagnostics
keep simulation and accepted billboard totals separate in jce_sr_particles.c /
jce_sr_debug_targets.c.

Verified single-blob shader compilation cache: tools/shader_cache.py, called by
tools/compile_shaders.cmake; tools/build/cache_lock.py owns kernel coordination.
Native output directories remain isolated and compiler/input bytes own validity.

Early allocator boot policy: engine/src/application/jce_engine_memory_policy.h;
owned by engine/src/application/AGENTS.md. JceEngine owns the resolved mode and
periodic trim clock; tests/application/test_jce_engine_frame_cap.c guards precedence.
Bootstrap window defaults: engine/src/application/jce_engine_window_policy.h;
the existing engine/src/os/core/jce_config.c owner parses/persists explicit
window modes. Application defaults precede user keys; local hidden/fullscreen
creation geometry does not replace saved windowed restore dimensions. Shared
controls live in tests/os/core/test_jce_config.c. No second parser or project-
specific window policy is introduced; OS geometry/focus still needs real tests.
Generic editor relocation probe: tools/editor_portability_probe.py; owned by
tools/AGENTS.md. tools/tests/test_editor_portability_probe.py validates complete
frame sequences and explicit bounded run controls without claiming low-end readiness.

Public bundle command: tools/jce_bundle_pack.c; owned by tools/AGENTS.md.
Driver composition: tools/build/bundle.py; owned by tools/build/AGENTS.md.
CPU argument and SDK failure controls: tools/tests/test_bundle_command.py.

Private UTF-8 host-tool bootstrap: tools/cli/jce_tool_main_sdl.c and jce_tool_main.h;
owned by tools/AGENTS.md. It serves bundle/pak/cook/bin2obj. Private
stream bridge tools/cli/jce_tool_io_sdl.c and jce_tool_io.h owns UTF-8 host IO;
directory enumeration and atomic replacement reuse existing public filesystem APIs.

Delivery profile authority: contracts/delivery-profile.json; shared configure
policy: cmake/JCEDeliveryProfile.cmake; selected public content-graph admission:
tools/build/delivery.py, owned by tools/build/AGENTS.md. SDK consumer admission
is exported by cmake/JCEConfig.cmake.in; static native configuration reuses
tools/build/standalone.py without a second dependency recipe.
Optional bgfx CPU profiler build selection remains in the existing
tools/build/jce.py bgfx argument owner and conan/hooks/hook_bgfx_wasm_fix.py;
conan/AGENTS.md and tools/build/AGENTS.md define its default-off identity,
actual compiler flags and generated compiled-selection metadata. The existing
Conan/build-driver/standalone Python fixtures cover configuration and reuse,
separate from an isolated dependency compilation and observed backend callbacks.
Windows desktop PhysFS API floor configuration belongs to the existing
conan/hooks/hook_cmake_policy_fix.py, under conan/AGENTS.md. Its generated
compiler/directory definitions and package-identity tags keep higher-floor
cached archives distinct. Dependency source and installed original caches stay
untouched; actual archive/PE imports and target execution remain separate proof.
tools/build/conan_config.py owns Native prepare_configuration_home and Web
prepare_toolchain_home through one bounded configuration/store preparer. Native
retains settings bytes; Web alone extends the actual observed Clang major.
The existing test_conan_config.py directory controls cover preservation and
hook synchronization without installing dependencies or copying the store.
Its bounded configuration_digest is consumed automatically by the shared Conan
generation fingerprint for the effective Native/Web/default home; named config
and profiles are distinct from untouched package/auth/database contents.
The exported deferred scripting hook reads the target declarations successfully
validated by JCEScriptEnable, merges required languages and reuses that owner
with the declared factories. It never invents providers from required metadata.
Repeated enable retains the original shim directory and one generated source;
missing declarations and factory mismatches still reject configuration.

The SDK installation implementation in engine/cmake and its shared helpers in
cmake remain owned by the engine build authority. tools/assets owns immutable
stock generation and provenance verification; tools/build owns delivery
admission. Application lifecycle and scene serialization stay with their
existing application/scene charters rather than a project-specific workflow.

Shared synchronous service trace scopes: engine/src/os/core/jce_trace_scope_internal.h,
owned by core; application aliases reuse it. Renderer lifecycle observation:
engine/src/renderer/jce_renderer_trace.c and jce_renderer_lifecycle_internal.h,
owned by renderer. Their CPU spans never certify GPU completion/presentation.

ShaderSet owner identity/retirement: engine/src/renderer/jce_shader_set_owned_internal.h.
Actual stock pair loading/acquisition batch: jce_shader_stock_table_internal.h;
production and CPU controls share the same table algorithm.
Renderer startup shader-root injection is owned by
jce_renderer_shader_dev.inc.h and the existing lifecycle private header. The
Engine forwards its one JCE_SHADER_DEV_DIR snapshot; the admitted Renderer
owns a copy and the existing device state borrows only that actual instance.
Matching destruction detaches and frees it. Grass, grass-shadow and foliage
lazy programs use the existing named overlay loader, with explicit PAK loading
preserved for uninjected roots. typed actual-body and Noop loader consumers live
in test_jce_renderer_shader_dev.c and test_jce_shader_link_owned.c. This covers
startup/restart; existing valid Scene programs retain their Reload behavior.
Relative roots retain the host CWD contract; the Editor keeps CWD unchanged.
Renderer startup pool selection: jce_renderer_budget_internal.h. Both are owned
by renderer. Runtime PlayerLoop retains its own registry and uses core service
scopes, allocation-free identity-ceiling dispatch and shutdown epochs.

Filesystem IO lifecycle adapters stay in existing os/core filesystem owners;
resource asset loaders emit their actual slot transitions through the same
private core trace scopes. Standalone packers consume core trace/timer ownership.

Host OPEN-failure detail stays in jce_filesystem_host.c and its private header.
The Engine object owns the diagnostic budget context. Its admitted boot injects
one borrowed context into the existing host-read service; quiescent retirement
detaches it before the summary and owner free. Active VFS changes preserve that
borrow. Repeated or foreign begin/end cannot reset or detach the current owner.
JCE_FS_OPEN_FAILURE_DETAIL=1 selects the context only for that Engine session. Default-off preserves the numeric trace
packet and avoids error/path copying; enabled failures snapshot SDL error before
other diagnostic calls. The existing logger receives two bounded hex prefixes
per admitted group, with lengths, truncation, scope/parent, generation and path
hash, capped at64 groups/session. These are attempted diagnostics, not delivery
ACKs or optionality/errno facts. Synchronous sink recursion is suppressed; async
sink reentry is not covered. Actual-owner Core fixtures cover both read paths,
the real formatter, concurrent budget and ordinary delayed-copy/prefix mutants.

Continuous trace capture: engine/src/os/core/jce_trace_capture.c and
jce_trace_capture_internal.h consume the existing trace ring and shared Chrome
encoder. The engine owns the optional handle; native workers and cooperative
hosts use one cursor. Thread-local suppression belongs to the existing trace
owner and excludes diagnostic IO only, retaining other producers. Stop records
producer joins, capture-worker exit and final drain before publishing explicit
loss/completeness metadata. Sampler report IO precedes capture closure. This is
CPU lifecycle evidence, not GPU completion or all-service coverage.

Trace timestamps use `engine/src/os/core/jce_trace_clock_internal.h` from the
actual `jce_trace.c` producer: exact performance-counter conversion survives
SDL restart while preserving the prefix. `tests/os/core/trace_clock_controls.cmake`
registers actual restart/conversion/old-clock controls. The lifecycle analyzer
owns bounded per-thread chronological validation; cross-thread publication order
is not a global timestamp order. Writer diagnostic intervals retain a separately
declared SDL-lifetime clock and cannot be directly joined to this event axis.

Tower Defense public-SDK consumer source, authoring data and dedicated validation
tools live in tests/consumers/td_lab. examples/td_lab retains only its single-file
Bundle catalog, upstream license and provenance; old ignored evidence remains
at its original path. dist/apps/td_lab publishes only final EXE/HTML products.

Bounded mounted reads: engine/src/os/core/jce_filesystem_read_internal.h and
existing filesystem owner. Runtime script/data/cursor readers honor active VFS
isolation before resolving loose host paths; no second filesystem authority.

Offscreen view CPU tracing belongs to renderer/jce_offscreen_target.c over the
shared private core scope authority; keep/overlay outcomes and delegated single
recording are verified by tests/renderer/test_jce_offscreen_trace.c on Noop.

Screenshot file admission/result, identity matching and bounded PNG writer
lifetime belong to renderer/jce_renderer_bgfx_callback.c. The renderer starts
the service only for an active graphical backend and releases GPU-borrowed CPU
buffers after backend shutdown. Application/jce_screenshot.c exposes the same
latest result to the stock host schedule; it does not own another writer.
Production-path controls: tests/renderer/test_jce_screenshot_completion.c and
tests/application/test_jce_screenshot_schedule.c. Successful file writing does
not certify image fidelity, filesystem durability or GPU idle.

Editor backbuffer request ownership: `editor/src/core/jce_editor_screenshot_completion.h`
(via `jce_editor.cpp` lifecycle and `ui/jce_editor_layout.cpp` F12), documented in
`editor/src/core/AGENTS.md`. Production-helper controls live in
`tests/editor/test_jce_editor_screenshot_completion.cpp`; terminal notifications
consume the editor owner once while the SDK poll remains non-consuming.

The same editor screenshot helper leases the shared file slot for Scene/Game
texture captures via `editor/src/scene/jce_editor_viewport_common.h` and their
matching poll wrappers; pending and unconsumed terminal requests cannot be
replaced by another editor capture actor.

The same shared viewport helper owns conservative PostFX bridge-copy elision:
D3D11/top-left, independent output, no Canvas component writer and no late
bridge consumer. Scene scaled TSR/RCAS and Game physics debug retain the copy;
Canvas lifecycle still runs. Existing getters/capture helpers borrow the
pipeline output without transferring bridge or temporal ownership. Actual
helper and complete caller-through-Canvas CPU controls live in
tests/editor/viewport_composite_controls.cmake; native pixels and performance
remain separate evidence.

Editor maintenance event decoding belongs to
`editor/src/core/jce_editor_event_fixture.h`, called by editor main through the
normal key processor and existing virtual Game pointer bridge.
`editor/src/core/jce_editor_event_fixture_schedule.h` owns the once-loaded capped
buffer and opt-in elapsed/late-marker scheduling in the existing GUI host
lifetime. The legacy visible-Playing frame clock remains the default; elapsed
activation pins current
runtime and geometry, cancels on admission loss, and never overrides focus or
input ownership. Headless production-helper controls live in
`tests/editor/test_jce_editor_event_fixture.cpp`; software fixtures do not measure
physical input latency. `tests/editor/event_fixture_schedule_controls.cmake`
registers the complete main dispatch/scheduler/bridge CPU fixture and ordinary
old-frame/no-start control from
`tests/editor/test_jce_editor_event_fixture_schedule.cpp`; graphical acceptance
remains separate.

Loose scene content addressing belongs to the existing Editor scene render
owner and pure scene/jce_scene_content_context policy. File loading reuses the
state owner's Scenes-parent rule before deserialization; manifest-free content
does not become a build project. Scene/Game/Play consume its one private borrowed
root, with isolated VFS and real manifest priority. Project retirement and
publication remain the existing transaction. Focused actual-owner CPU controls
and ordinary old-wire targets live in tests/editor/loose_scene_content_controls.cmake;
render-settings/VFS controls cover loose authored quality independently of GUI,
font rendering, complete loader rollback or GPU execution.

`tools/editor_lifecycle_probe.py --trace` optionally captures continuous CPU
evidence in its isolated output root. `tools/lifecycle_trace_report.py` remains
the separate authority for explicitly required, actually executed phases.
The probe's bounded schedule owns environment selection, contiguous phase
validation, scene snapshot names and result metadata. Its --stop-at counts
PLAYING ticks; other selectors count editor ticks. The 3..16 increasing
observations retain first BEFORE/last AFTER and validate every middle DURING.
The nominal earliest Stop is play_at+stop_at; deferred Play admission can delay
it, and actual positive phase rows remain authoritative. Extended CLI controls
use actual Python readers with only the native child port replaced.

Offscreen replacement retirement: engine/src/renderer/jce_offscreen_retirement_internal.h
and jce_offscreen_target.c, owned by engine/src/renderer/AGENTS.md. Collection
uses actual renderer submissions even for hidden targets; renderer teardown
detaches GPU resources before backend shutdown. Noop resource-count controls
live in tests/renderer/test_jce_offscreen_trace.c. The private seam is not a
consumer API and submitted-frame age is not a GPU-completion fence. A full queue
rejects new resize requests; unchanged-extent preparation remains valid.

Resolved directional shadow ownership: engine/src/middleware/scene/jce_sr_shadow_targets.c/.h,
owned by engine/src/middleware/scene/AGENTS.md. Targets are created for the
resolved draw requirement rather than unused global defaults. Transactional
texture/framebuffer pairs preserve retry and partial-failure cleanup; same-size
cascade capacity remains bounded for alternating views. Production-helper
controls live in tests/middleware/scene/test_jce_shadow_target_lifecycle.c;
controlled allocation failures are not physical GPU fault injection.

Startup transient pool policy: engine/src/renderer/jce_renderer_budget_internal.h.
Typed backend snapshot emission: jce_renderer_snapshot_internal.h in that same
module; both are private renderer-owned seams with graphics-free controls in
tests/renderer/. Selected adapter IDs and delayed frame statistics are observed
capabilities, not requested device policy or a GPU-completion fence.

Static foliage roots-buffer ownership: engine/src/middleware/scene/jce_sr_foliage_buffer.c/.h,
owned by engine/src/middleware/scene/AGENTS.md. The existing scene cache keys
resident roots by placement identity, instance count and compute-read capability.
Replacement publishes only after CPU upload acceptance, preserves the previous
owner on failure and never binds stale contents for a new identity. Ordinary
static roots can be shared across views; camera-dependent visible/LOD buffers
retain their separate owners. Exact production-helper controls live in
tests/middleware/scene/test_jce_foliage_buffer_lifecycle.c. Upload acceptance
is not GPU completion or proof of rendered instance count.

Shared CPU instance upload ownership: engine/src/renderer/jce_instance_upload.c/.h,
owned by engine/src/renderer/AGENTS.md. Renderer context identity and submission
epochs come from the existing device lifetime owner. The qualified 32/64/80-byte
families share one bounded logical budget, including unpublished candidates and
retired handles. Reservations and commits run on the owner thread; committed,
identity-bearing bindings can be consumed by encoders in the same epoch.
Production-provider controls live in tests/renderer/test_jce_instance_upload.c.
Logical capacity, CPU upload acceptance and deferred destruction are distinct
from physical GPU residency, hardware completion and rendered population.

Current foliage identity and shadow admission: engine/src/middleware/scene/
jce_sr_foliage_identity.c/.h, jce_sr_foliage_shadow.c and
jce_sr_foliage_shadow_key.c/.h, owned by that module's AGENTS.md. Authored
placement/geometry fingerprints and current preparation readiness invalidate
CSM reuse; every caster is visited even when a previous preparation fails.
The bounded roots cache and per-cascade submission remain scene owners.
Provider controls live in tests/middleware/scene/test_jce_foliage_shadow_key.c;
these CPU controls do not certify GPU shadow pixels or allocation-fault recovery.

Instance resource correlation reuses private core scoped counters in
jce_trace_scope_internal.h and the shared renderer upload owner. The optional
256-lease staging observation state retains its allocator context beyond owner
shutdown for backend release callbacks. Metadata gaps remain explicit and never
block normal rendering. tools/instance_upload_trace_report.py validates only the
captured common-owner resource subset, separately from lifecycle_trace_report.py
CPU phase/counter controls. Logical closure and backend callbacks are not GPU
completion or physical-residency proof.

Editor authoring context selection: editor/src/core/jce_editor_project_context.h/.cpp,
owned by editor/src/core/AGENTS.md, reuses public JceProject parsing and ancestor
resolution without storing project state. Core startup and io scene/Bundle
loading consume it before the existing project transaction and isolated content
mount. Real host-FS CPU controls live in tests/editor/test_jce_editor_project_context.cpp;
actual deep-directory Bundle Play/Stop still requires a graphical host.

Completed-interval perf diagnostics: engine/src/application/jce_engine_perf_hitch.c
and its private header own one bounded paired raw-wall/comparison ring,
window/hitch state, interval continuity and phase-gate eligibility. The private
jce_engine_frame_cap.inc.h keeps the existing process-global cap state, deadline
policy and sole wait, included once by engine.c. Its actual selected FPS and
planned/returned wait witness belongs to the completed interval; comparison
subtracts valid planned wait, preserving actual oversleep. Invalid clock or
metadata gives no exemption. Engine iterate remains the sole phase-tick/caller
authority; windows/percentiles and raw KPI/CSV values retain raw wall and the
same continuity decision. Residual comparison is not CPU execution time. Tests
live in tests/application/test_jce_engine_perf_hitch.c,
test_jce_engine_frame_cap.c and test_jce_frame_kpi.c; typed clock/sleep and an
actual missing-witness wire control do not certify native scheduling or GPU output.

Static-profile Python binding test transport: tests/scripting/python/CMakeLists.txt
compiles the actual scripting/c_abi sources into a private test-only shared
fixture with the generated export authority. The existing C-ABI loader/export
test and Lua/Python differential validate it; shipped static linkage is unchanged.

Engine callback lifetime uses the existing per-engine iterate gate and a nested
event-dispatch guard in application/jce_engine.c. Callback destruction requests
retain host ownership and request quit; refused async shutdown retains owners
for a later host retry. The real public lifecycle fixture covers six isolated
headless modes. Neither these controls nor a CPU trace certify graphical resize
callbacks or arbitrary use of an already freed raw engine pointer.

Canvas text resources: engine/src/middleware/scene/jce_ui_canvas_resources.c owns
the configuration/invalidation generation; jce_ui_canvas_font_cache.c owns each
Canvas's bounded font objects, main/leaf fallback roles and VFS/policy binding.
jce_ui_canvas_text.c owns shared plain/rich/math measurement and submission.
Best Fit uses one maximum-size atlas and applies the fitted scale at submission.
Editor asset-index completion and explicit asset-browser refresh consume the
public jce_ui_canvas_invalidate_resources API; a same-path external replacement
requires an observed change/explicit refresh, not an assumed filesystem epoch.

Test metadata: tests/cmake/JceTestMetadata.cmake owns CTest domain and kind labels;
per-directory finalization precedes the recursive completeness audit. The build
driver selects all registered tests by default, or one explicit ownership domain.
Counted CPU providers, Noop, native GPU and actual editor UI evidence remain
distinct. Registration alone does not certify a feature.

Grass preparation: engine/src/middleware/scene/jce_sr_grass_prepare.c owns current
root placement, resident buffers and terrain/mask-aware preparation.
jce_sr_grass_shadow_content.c owns production placement/geometry keys and the
complete caster inventory admission. Shadow submission streams that inventory
per cascade rather than treating the 16 resident-cache slots as the full scene.
Colour and depth share the stock grass vertex deformation; the private depth
program pairs vs_grass/fs_grass_shadow with matching full stage interfaces and
uses the public program owner. The new depth fragment stage enters the ordinary
shader recipe and embedded PAK; explicit configure is needed after adding a
shader file because the production shader-source glob is not CONFIGURE_DEPENDS.
A failed current prepare/submit invalidates the shadow cache. CPU preparation and
submission acceptance do not prove GPU completion or visual correspondence.

Native project target naming and SDK archive-contract admission are implemented
in editor/src/core/jce_project_build_target.cpp; the private header owns native
architecture aliases. cmake/JCESDKTarget.cmake owns compiler-target identity and
per-configuration metadata; JCESDKInstall publishes it beside each archive.
JCEConfig, tools/build/sdk_target.py and the editor consume those same files.
Only declarations and archive presence are admitted; actual compiler/link,
artifact ABI, target execution and cross-platform shipment remain separate. jce_build_package.cpp owns output admission and verified single-file
publication; it never deletes an arbitrary output directory.

- Application quit result: `engine/src/application/jce_engine_quit.c/.h` owns
  the main-thread request latch; the additive public result API preserves failure
  until actual session retirement. `jce_engine.c` retains frame/KPI/lifecycle and
  final teardown ownership. Headless editor receipt failures consume this public
  result path; fake-port/CPU tests do not establish an SDL process exit receipt.
- Terrain upload boundary: `engine/src/middleware/scene/jce_sr_terrain_vertices.c/.h`
  owns the explicit TerrainVertex-to-MeshVertex transaction. Terrain and Mesh
  public layouts are not interchangeable. Existing mesh creation owns generated
  tangents and copied GPU data; temporary converted CPU data retires once.
- AI panel visible session: `editor/src/core/jce_editor_ai_panel_session.h`
  owns dry-run opt-in reset. Closing/reopening resets it; collapsing a visible
  panel preserves it. Test argv locally without invoking an external provider.

Embedded service atomics: engine/src/os/core/jce_atomic_storage.h shares the
existing private JceAtomicI32 storage with jce_thread.c. Renderer callback
device-loss notification consumes the public atomic operations with embedded
static lifetime; it never uses the heap-owning atomic destructor.

Impostor bake ownership: engine/src/renderer/jce_impostor.c and the private
jce_impostor_internal.h retain one existing bake slot and CPU readback destination.
Shutdown retires GPU handles while bgfx is alive and retains a submitted destination
until bgfx_shutdown joins. jce_renderer_destroy_impl then calls the CPU-only
jce_impostor_after_backend_shutdown before jce_rcb_after_backend_shutdown.
Finalization frees once, resets IDLE and invalidates handles; repeated cleanup is
total. Exact-TU CPU controls cover ownership; GPU completion and pixels remain
separate acceptance. There is no additional queue or public ABI.

Font glyph publication: renderer/jce_text.c owns static bitmap admission and
dynamic atlas pixel/map/texture transactions. Failed candidates do not publish
glyphs or shelf state; dynamic failures remain retryable with per-stage first
diagnostics. Canvas image-cache capacity remains 64 including negative entries;
jce_ui_canvas.c owns a per-generation capacity/fallback diagnostic. Exact-owner
CPU failure controls remain separate from rendered glyph/image acceptance.

Mesh creation: renderer/jce_mesh.c and jce_mesh_create_internal.h own staged
CPU/GPU acquisition and symmetric rollback, retaining immutable private provider
identity until destruction. Grass colour: scene/jce_sr_grass_submit_internal.h
owns visible requested/accepted/rejected receipts. scene/jce_sr_grass_colour.c/h
owns the actual admitted range emission and complete final grass draw state once
per ALL-discard submit, including transient splits and later resident cell runs.
The environment retains visible range/root ordering and per-field receipts.
The unchanged geometry binder remains shared with grass shadows and owns camera
xyz/w=0. renderer/jce_lighting_system_internal.h exposes the directional/ambient
subset over the ordinary shared pack/layer selection, with full light-count and
shadow-slot identity and no second camera write. The existing view-mode binder
owns the sole {0,0,mode,0} normal-scale write. scene/jce_sr_colour_output.h shares
the neutral output pack with both PBR paths; the existing fog, cloud and frame
shadow owners remain shared. Default PBR material state is unused and omitted
from grass. First-reason logs remain bounded and per-field traces current.
Exact-owner typed CPU controls count named-handle aliases and sampler writes,
compare directional values with the ordinary full upload and reject duplicate,
camera-alias and first-batch-only mutations. These are CPU command-state and
admission results, not GPU completion, pixels or performance.

Editor tool execution: editor/src/core/jce_build_tool_paths.cpp consumes the public jce_toolchain authority for detection and actual native execution. jce_project_build_target owns edited Target/Exe, host/selected architecture, variant and build-output prediction; jce_build_package remains the separate final dist/apps publisher. tools/build/jce_accept.py selects actual modern app/architecture/variant leaves and only admits legacy VERSION declarations as compatibility metadata, never compiler ABI authority.

Editor Build completion: core/jce_build_process_outcome.h and the existing
jce_build_manager poll latch Stop before release. Exit zero cannot advance,
finish or publish a cancelled request; compatible FAILED status retains the
actual exit code and cancellation reason. Static-name tool-completion traces
report CPU transition success/failure/cancellation, not external child runtime.
Actual-body/trace/no-draw controls and omitted-Stop negatives live in
 tests/editor/build_completion_controls.cmake; OS children, native hidden
polling and real publication require separate acceptance.

Editor tool-version admission: editor/src/core/jce_build_tool_probe.cpp/.h uses
the existing public process/clock/thread owners with a cooperative 2 s deadline,
fair bounded dual-stream reads, independent 1023-byte diagnostic prefixes and
64 KiB combined output admission. Stdout alone supplies an exit-zero observed
completion identity; stderr remains failure context. Public destroy owns final
kill/reap, and canonical static-name CPU trace services describe only this query.
Complete-owner counted wire controls live in tests/editor/tool_probe_controls.cmake;
actual native tools, UI operation and builder delivery remain separate evidence.

Editor first startup: core/jce_editor_renderer_startup.h consumes the config loader filled value even when source-found is false. A missing preference file is valid platform defaults. Apply before engine/device creation and preserve explicit Auto and environment fallback. Actual-loader CPU controls and old-guard mutation do not certify the active graphics device.

Installed font selection: os/platform/jce_host_font.c and the public jce_host_font.h own automatic Latin/CJK/Symbols roles and installed family lookup; Canvas, document UI and ImGui consume the appropriate roles at startup/resource-change boundaries, not each frame. Default Canvas fallbacks use the same cache epoch, glyph coverage, shaping and atlas owner; explicit font/default/fallback paths retain their authored policy. engine/ui/fonts/Aileron-Regular.ttf is a licensed Latin-only fallback with hash/provenance. User JCE.ttf handwriting remains explicit optional content; RmlUI consumes the same roles through its documented font-retention owner.

Dist selected-archive closure: tools/build/jce.py `_dist_prebuilt_assets` delegates scene cooking and dynamic dependency closure to the SDK public Bundle packer; tools/jce_pak.c consumes one immutable public Bundle/Archive snapshot per selected input. Final secure BOM/hash/deep verification and equal-byte overlap consolidation do not prove runtime parity or minimal stock. tests/os/resource/test_jce_pak_selected_archive.c exercises actual owners; its runtime gate is separate from mocked driver controls. Public legacy Bundle compatibility remains explicitly scoped.

Font policy update: installed Ink Free is the preferred automatic Latin face, then system defaults and licensed stock fallback; explicit overrides win and CJK remains separate. editor/src/ui/jce_editor_font_picker.cpp/.h supplies searchable selection, truthful custom paths and the Preferences deferred-before-NewFrame reload consumption. OS discovery keeps its shared public owner; renderer font-source sharing follows the ownership rules below.

Document UI fonts: middleware/ui/jce_ui_rmlui.cpp consumes shared host font
roles into logical JCE-UI/JCE-UI-CJK/JCE-UI-Symbols families and licensed PAK Latin fallback;
jce_rml_font_bytes.c/.h owns borrowed-span retention before font-engine
admission and releases only after Rml Shutdown. Resolver ports and Noop font
layout/atlas controls belong to tests/middleware/ui; they do not certify CJK
glyph coverage or physical GPU rendering. Explicit project fonts remain primary.

Model-process lifecycle: middleware/llm/jce_llm.c retains accepted request trace
identity across frames to actual completion, failure, timeout or cancellation,
joining the provider before retirement. Video lifecycle: middleware/video/
jce_video_lifecycle_trace.h uses the common bounded trace for decoder open/read,
actual queue waits, serial seeks, dropped/failed work and worker join;
jce_video_slot_internal.h owns private slot declarations only. These timelines
are CPU ownership and worker evidence, separate from audio and GPU acceptance.

Installed SDK test consumers: tests/consumers/sdk_smoke_gate.py delegates to
tools/build/jce.py smoke authorities without a second build recipe or engine
source linkage. CMake classifies them consumer/integration, explicitly selects
archive/profile/target and preserves SKIP 77. Exact native success markers plus
exit success are required; protocol ports and metadata alone do not prove a run.

Scripting availability: scripting/cmake/JCEScriptingInstall.cmake derives catalog
and compiled-language sets from the existing roster/TARGET loop;
JCEScripting.cmake.in publishes schema 1 alongside installed backend declarations.
The shared SDK smoke validates the selected Release closure against that set.
Optional uncompiled languages are unavailable, while missing compiled payloads
fail admission. tools/tests/test_sdk_consumer_gates.py owns positive and negative
protocol controls; native SDK consumers remain separate execution evidence.

Native CLI packaging: tools/build/jce.py cmd_package_game is a compatibility
entry to cmd_build_project, both delegating to the editor headless owner. Shared
read-only output preflight protects source/build/SDK roots before child launch;
the native jce_build_package owner remains publication authority. Final output
defaults to project/dist/apps, with no arbitrary recursive deletion or legacy
loose-file pipeline. tools/tests/test_build_driver.py covers actual filesystem
admission and removal of the admission wire.

Build transcript ownership remains in core/jce_build_manager and its private jce_build_log helper. Bound observed lines and deferred worker replay independently; no worker Console access. The headless terminal owner writes the accepted invocation's normalized transcript with public atomic host IO, discloses missing/truncated/incomplete/write-failed diagnostics separately, and preserves the actual build result. A refused start never reads a previous snapshot. Borrowed text expires at the next log mutation and is consumed immediately. Observed-line completeness is not proof of pipe EOF, byte-exact child streams or GPU completion. The existing headless driver target compiles actual owner-body controls and an ordinary missing-capture mutation.

The private Dist content graph borrows the selected SDK stock root through the
public Bundle packer resolver. Keep existing mounted/project asset precedence;
only normalized relative shader binary addresses may fall back to SDK stock.
Resolve relative SDK configuration through the existing absolute host-path
owner before the callback. Headless graph generation must not depend on a GUI
VFS mount. Preserve the producer first ERROR and dependency chain in the build
result; replay worker messages and persist terminal failure only on the owning
completion thread. Host-file/Archive controls are separate from actual shader
execution, target builds and reopened gameplay.

Offscreen resource identity: renderer/jce_offscreen_target_internal.h stores
origin capture generation and creation serial in the existing current/retired
owners, along with actual attachment handles/formats/dimensions. The existing
trace records create/rollback/retire/destroy-submit transitions, reason and
logical attachment estimates. Capture gaps are explicit and destroy submission
is not GPU completion. Private immutable allocation ports use the same
production ensure/cleanup chain; tests/renderer/test_jce_offscreen_trace.c
combines real Noop submission with counted failure/identity controls.

Editor key submission: core/jce_editor_key_event.h is the single existing
event-to-ImGui modifier/key owner. core/jce_editor_hotkey_diag.h supplies
explicit opt-in bounded event/gate/query observations without routing changes.
Preferences menu labels consume the actual HotkeyRegistry binding. Real ImGui
context positive and no-modifier-wire negative controls live in tests/editor/
test_jce_editor_hotkey_input.cpp; the shared test context owns CPU frame setup
only, and does not certify native GUI focus or popup reachability.

Scene View navigation: panels/jce_scene_view_cube.cpp registers real ImGui
navigation items before the existing full viewport item; input_policy.cpp
owns shared clipped overlay geometry. The same production register function
feeds scene selection and existing drop/gizmo owners. Cube/axis drawing acts
only on item-owned clicks. tests/editor/test_jce_scene_view_navigation.cpp
uses a real CPU ImGui context and a removed-item-wire contract control. These
controls are not substitutes for native pixels or asynchronous GPU picking.

The renderer private `jce_bgfx_allocator_trace_internal.h` observes the existing allocation bridge only with explicit `JCE_BGFX_ALLOC_TRACE`. The same device owns a fixed 4096-slot address ledger; addresses stay private while backend epochs, stable allocation IDs, revisions and requested-byte transitions feed the existing core ring-only counter owner. Provider and trace calls run outside the metadata gate. Failed realloc preserves the old identity and bytes, and address reuse receives a new identity. Operations no longer stop at a 512-request prefix. Opted-in tracking continues while capture is paused, but gaps, unknown inputs, duplicate addresses, capacity/identity saturation or refused boundaries invalidate the relevant completeness witness. Init failure and actual backend join own finalization; periodic snapshots do not close an attempt. Require the ledger witness together with the existing loss-checked capture footer and matched resource transitions. Requested-byte accounting and CPU submission are distinct from OS memory, physical GPU residency and GPU completion. Tests consume the private observer, actual allocation ports and exact trace TU; their receipts remain separate from graphical acceptance.

ES consumer portable staging preserves selected-script dependency sidecars, with one explicit portable director declaration alias to the original sidecar. The original byte/hash provenance and canonical staged script-sidecar address remain observable; orphan sidecars are excluded and missing required metadata is refused. The public packer still owns typed closure; actual staging CPU evidence does not prove fresh exports or runtime pixels.

The renderer device atomic admission is shared by both public bgfx create adapters and remains held through actual shutdown join and tail cleanup. Duplicate/pending and registered-foreign requests refuse before mutating backend/platform/caps/allocator state, with a ring-only reason counter tied to the existing create task. Failure and fallback/null destruction retain distinct lease boundaries. CAS does not grant arbitrary-thread bgfx API permission. Opted-in request observation warms the existing timer on the API owner; capture reset requires the existing quiescent protocol and gap records cannot certify atomic grouped fields.

Scene View post-Image navigation registration delegates cursor advancement to its final real full-viewport item. Restoring a cursor beyond submitted bounds triggers an actual ImGui layout error. The context fixture consumes Image/fallback/modal through End and Render, rejects all recorded errors, and retains the previous click/drag ownership controls plus old-behavior negatives.

Installed DIST SDK smoke: tools/build/sdk_smoke_assets.py coordinates the existing
application asset builder for both C99 and scripting consumers. Native SDK
`--smoke` forwards explicit variant/delivery and requires actual runs; historical
raw-resource smoke is not authenticated DIST proof. The formal consumer loader
registers its imported build owner before execution; production prebuilt admission
remains intact and no second cooker/archive parser is introduced.
tests/sdk_smoke/stage_dist_fixture.py owns bounded fresh regression inputs,
source identity, fixture-kind pointers and public typed graph/authenticated deep
BOM cross-checks. The scripting input retains scripts/smoke_py.py as script /
python.source; C99 retains cooked PNG/WAV controls. test_stage_dist_fixture.py
owns staging/proof CPU controls, while tools/tests/test_sdk_consumer_gates.py
checks actual driver sequencing/refusal with counted compiler/proof/runtime
ports. These controls are separate from native decompression/backend lifecycle
markers/exit success and GUI, relocation or device presentation; regression
fixture success does not certify a shipping minimal closure.

Timed KPI clock ownership: application/jce_engine_kpi_clock.h owns the shared ordered wall/dt subtraction (zero counter origins are valid) and borrows the existing engine monotonic counter and frequency for accepted render-row continuity, overflow and terminal checks. engine.c supplies actual boundaries and one opt-in versioned witness after writer finalization. Service-init budget and completed render coverage stay distinct; no new timer, public ABI or initialization frame is introduced. Missing, truncated or gapped observations cannot certify a complete timed capture.

Native build environment forwarding: tools/build/jce.py::msvc_env preserves the
existing JCE_SHADERC_EXECUTABLE and BGFX_SHADERC selectors through pristine MSVC
activation. tools/tests/test_build_driver.py exercises exact values, unset
selectors and unrelated environment isolation in both activation branches.
Tool discovery/probing and compiler/include cache identity retain their existing
owners; selecting a host compiler leaves target runtime configuration intact.

Toolchain overrides: os/core/jce_toolchain.c shares executable and SDK-marker admission with automatic discovery. Only expected executables, canonical VS/vcvarsall roots and declared SDK markers mark an override present; invalid overrides preserve the existing automatic fallback without claiming override provenance. tests/os/core/test_jce_toolchain_override.c consumes the exact owner with real temporary filesystem controls. Discovery is distinct from executable toolchain, target ABI, SDK payload and relocated-build acceptance.

Installed scripting runtime staging reads the shared backend rosters. A static SDK that declares no external runtime has an explicit status, while unfulfilled external staging retains its warning. Neither message replaces actual consumer link/run or delivery checks.

Private Pak/FS inspection ownership: engine/src/resource/jce_pak_mount_internal.h
owns the Pak inspection marker and canonical filesystem-provider registration;
engine/src/os/core/jce_filesystem_pak_internal.h owns the dependency-inverted
registration of that tuple and its resource-supplied mount predicate. The actual
filesystem.c owner holds the active provider, registered tuple and both predicates
in one private static service structure under the existing serial contract. The public
unversioned find/size/decompress three-callback provider ABI and unrelated opaque
custom providers remain unchanged. These paths describe source ownership and
same-handle SDK admission, not an independent sandbox or completed boundary
verification; final resource acceptance remains separate.

Declared Bundle DIST admission: tools/build/delivery.py consumes the actual host-tool typed Bundle inspection and ordered asset union; tools/build/jce.py owns the shared authenticated PAK, runtime boot and existing cache leases. tools/jce_pak.c validates internal manifests through the public Bundle reader, preserves optional sidecar compatibility, and merges bounded selected archives through the existing byte-conflict owner. Source snapshots remain checked through linking. Catalog records and sidecars do not replace actual archive bytes or native/graphical consumer verification.

Default worker budgets: core/jce_worker_budget_internal.h owns the pure default and existing explicit-pin policy. jce_config.c publishes effective boot budgets to the existing jce_thread.c shared-frame owner before lazy creation, while public machine-class/job-worker getters retain authored values. jce_async.c consumes that same budget with its existing eight-worker cap and cooperative fallback. The engine resolves this lifecycle's low-memory override before publication; it never inherits a prior lifecycle's effective classification. LOW defaults to one worker, ordinary defaults to cores-minus-one capped at eight, and valid explicit pins retain their existing owner limits. These are scheduler budgets, not process thread counts, CPU affinity, frame pacing or physical-device certification. Actual pool/executor and headless engine restart controls live under tests/os/core and tests/application.

The existing Editor Play host owns temporary presentation changes in private `jce_editor_play_presentation.h`. It resolves the shared effective-render-settings owner after the scene snapshot and before runtime creation, applies public renderer VSync and engine target-FPS APIs, and restores captured editor values on create failure and normal Stop. Runtime/stats and presentation remain members of the same host; there is no additional global owner. Explicit project FPS remains retained as the private preview cap input; missing composition still begins the FPS transaction while preserving entry VSync. Fallback/null renderers never call bgfx. Exact-callsite runtime controls and native helper/transaction controls are separate from actual device pacing evidence.

SceneRenderer owns its lazy static-instance velocity program, attempted-load state and render queue. The private `sr_velocity_inst_prepare/release` functions in `jce_sr_cull.c` use the existing shader-program and render-queue owners; create marks the handle invalid before failure paths, queue admission failure retires the loaded program, and scene destruction releases both exactly once. Separate view renderers never share those mutable resources. Existing core trace scopes cover preparation and retirement; complete backend allocation closure must be checked on actual rebuilt artifacts. The lazy depth-prepass queue is also per SceneRenderer, through sr_depth_prepass_queue_prepare/release: it reuses the existing queue owner, clears before each admitted pass, keeps inline rendering on allocation failure, and retires on scene destruction. Constructor rollback frees both cull-byte buffers if either allocation fails. Actual constructor failure matrices, compiled old-behavior negative controls and Noop queue recreation controls remain distinct from device, process-heap and presentation evidence.

Renderer font-source ownership: renderer/jce_text.c shares immutable source bytes inside its existing same-thread font subsystem. Every open reads current host/PAK/VFS content, uses the public archive content-hash owner and full byte comparison, and retains independent FT/HB faces, sizes, SDF settings and atlases. Caller memory remains caller-owned. Only live references remain; the last close destroys HB/FT before unlinking and freeing source bytes. Stable blob identities, references, unique live bytes and explicit rejection counters use existing trace scopes. Overflow refuses admission, and shutdown cannot reset live ownership counters to claim closure. Font atlas and process/GPU residency measurements remain separate; temporary reads and peak/RSS savings require actual observation. ImGui and document-font retention keep their own existing owners.

TAA first-projection policy: public scene request synchronization has private
per-renderer TAA-seen state independent of shadow/MSAA latches. Scene View,
standalone begin-frame and Game View consume it before Halton jitter; current
PostFX HDR/RGBA16F format eligibility is shared through its public query.
Stable policy preserves console overrides. Actual-body CPU controls reject old
late-sync and missing-format wires as ordinary failures; real Noop pre-render
controls are registered separately. Neither proves resolved temporal pixels.


Test Runner child workflow: editor/core/jce_editor_test_run.cpp owns the existing panel's single public process child, bounded stdout/stderr drain, observed-completion deadline and finalized log. The Editor's existing pre-render should_quit callback now owns the audited CPU service poll, including this child; layout no longer polls it. CPU controls and real child completion remain separate from actual OS minimization/restore and all-panel GUI acceptance. Panel shutdown uses the same child close/reap authority. Public os/core `jce_fs_host_write_new` delegates exclusive creation and complete write/CloseIO to the existing filesystem owner. Refusal never overwrites an old log or replaces prior result identity; a failed fresh write can leave explicitly diagnosed partial evidence. No second process, filesystem, shell or async worker owner is introduced. `test_jce_editor_test_runner` is the real self-child integration control; `test_jce_editor_test_runner_old_exit_status` and `test_jce_editor_test_runner_old_completion_deadline` separately remove the actual exit-code and observed-deadline wires as ordinary runtime contract failures. Registered/compiled controls are distinct from their actual command results and GUI cancellation/hidden/restart acceptance. Actual GUI terminal-workflow evidence retains the selected profile/build/editor-test-runs child logs, literal argv and invocation identity for PASS, explicit exit failure, observed-deadline TIMEOUT and user CANCELLED. A later native relink or atomic Editor package does not replace the historical execution hash; hidden polling, restart, text focus and visual reachability remain separately scoped observations.

Presentation observation: tools/presentation_report.py is the bounded reviewer
for pinned PresentMon QPC-ms CSV and explicit process receipts. Keep actual
process/swapchain identity, normal exits, clock window and unavailable metrics;
never infer hardware utilization, optical refresh or whole-lifecycle completeness.
CPU parser controls live in tools/tests/test_presentation_report.py and the
existing tools/lint/run_all.py aggregate.

Explicit runtime script logging: application/jce_rt_script.c routes the shared
JceScriptHost.log callback through public jce_log_write even in DIST, retaining
runtime filters/sinks and literal message formatting. Internal informational
macros remain compile-time gated. tools/tests/test_script_runtime_log.py uses
the actual callback and a DIST old-macro ordinary runtime failure control.

Default editor packaging excludes the unused user-authored fonts/JCE.ttf through the existing PAK suffix filter. Source bytes remain intact for explicit authoring; system Ink Free discovery redistributes no system font. Check the actual Editor BOM for absence and stock-font retention after rebuilding.

Acceptance evidence separates configured native engine/editor/consumer and unit/integration/contract results from unavailable/ignored paths, direct-GUI outcomes and later artifact identities. Complete source-gate failures remain failed after focused controls pass; ordinary runtime negative controls need successful independent links and their exact expected exit outcomes. GUI tooling permission/activation refusal and operator-forced stop are unavailable observations, not product-crash evidence. Refer to scoped phase receipts under ignored build/tests rather than promoting registry counts or a baseline ratchet to full product reliability.

Current host lifecycle gaps remain explicit: application/jce_engine.c treats desktop minimize as lifecycle PAUSE and returns at the render-paused gate before app update. application/jce_runtime_host_policy.inc.h blocks that paused state even with run_in_background=true. True OS suspend and desktop hidden-render policy are not yet distinguished. Project Quality vsync_count=2 is currently collapsed to boolean presentation in jce_editor_effective_render_settings.cpp and stock/Play renderer bridges; it is not implemented as every-second-vblank presentation. Do not fix either claim with a nominal FPS cap or by dropping pause safety.

Script cost attribution reuses JceRuntime's per-instance script-cost owner and the existing Profiler Scripts panel. ES Director's component/render JSON read/merge/write transport is a source-supported candidate, while typed rotation/position/particle ports are distinct. A portable-policy functional receipt or a reading during parallel compilation does not establish per-script performance. Windows memory tools preserve one identity-checked query handle; optional mapped-file names attach allocation metadata, not resident/private/heap/GPU ownership. PDH per-engine counters, PresentMon events and OS memory observations keep separate clock windows and metrics.


Editor configuration scope: core/jce_editor_renderer_startup.h binds the existing engine config-path override to the validated Editor profile/editor-runtime.ini before device creation, in both GUI and headless descriptors. A project working-directory .config/jce.ini must not override Editor window or presentation defaults. Invalid or overflowing profile paths fail startup before window/device creation; project configuration and explicit valid Editor profile contents remain separate authorities. The real loader/path fixture and two independent ordinary missing-wire controls live in tests/editor/test_jce_editor_renderer_startup.cpp. Current command receipts distinguish three targeted CTest outcomes from the complete registered suite. The app consumption baseline is tightened only for the newly consumed public setter.

bgfx C99 view labels require the explicit third length argument; INT32_MAX is the documented NUL-terminated sentinel. A negative integer length is unsafe, even if a Noop SDK smoke passes. tools/lint/check_bgfx_view_name_length.py checks first-party call sites with bounded syntax handling, with actual Python mutation controls in tools/tests; dynamic unresolved expressions are not certified. Native Splash and ImGui startup witnesses are required separately from source/build success. Historical real crash dumps, UI-tool refusal and forced test stops remain distinct evidence.

ImGui clip admission belongs to middleware/ui/jce_imgui_clip_internal.h and the
existing renderer bridge. setup_view caches its actual drawable pixel extent;
draw intersects finite clips before scissor/texture/state/submission and uses
checked integer conversion. The existing optional ui.clip.draw.rejected counter
aggregates normal culling and invalid clip rejection without per-command logs.
Complete draw/setup CPU ports and ordinary old-limit/missing-extent controls
live in tests/middleware/ui; they do not certify physical pixels or GPU savings.

Renderer GPU timing provenance stays in renderer/jce_renderer_snapshot_internal.h and the existing renderer.c snapshot node. The actual bgfx API frame number, host submission ordinal and upload-owner device context generation identify the observation separately from bgfx global/per-view gpuFrameNum. Preserve signed raw timer begin/end/frequency and explicit interval availability as decimal integer trace counters; device epoch zero or outside signed counter range is unavailable. Repeated latched intervals do not become new GPU work when the CPU advances. Borrowed provider views are emitted synchronously at first submission and the existing 60-frame cadence; detailed view fields require trace-on, and a single provenance log requires explicit PERF_LOG. Existing public GPU POD layouts remain unchanged; the independent bounded GPU timing observation surface is described below. The actual trace exporter retains int64 integer spelling beyond double precision; downstream readers must preserve it. None of these counters certifies full-device completion, presentation or resource retirement. The existing renderer snapshot fixture and ordinary missing-source-frame wire contract target cover the private transport independently of hardware observation.

Renderer snapshot names: the private snapshot helper formats a bounded whole-entry percent-encoded view map into the existing explicit PERF_LOG sample. It records API/submission/device epoch, provider source frame, actual/listed counts, rotated start and truncation/unavailability. No registry, heap history or asynchronous profiler names are introduced. Ordinary Main and final offscreen composite labels use the documented static INT32_MAX name length. Each sampled subset is partial unless its completeness fields say otherwise.

Windows native memory page metadata: tools/windows_memory_reader.py owns the explicit working_set_pages opt-in and raw K32QueryWorkingSetEx port alongside its existing VQE/name/sample identity handle. A call is limited to 4096 ordered aligned addresses and fixed x64 fields. Existing tools/tests/test_memory_commit_probe.py covers default-off/minimum rights, invalid bounds, unavailable/failed/closed APIs and response identity. Selected valid-page flags are non-atomic metadata, including possible AWE/large pages outside the ordinary working set; they are not an allocator ownership certificate or permission to subtract a mapped file from RSS.

Editor host CPU services: core/jce_editor_host_services.h is the single private audited poll list, called once by main.cpp editor_should_quit after successful initialization and disabled before exit teardown. Run owns Build polling during its BUILDING transition; the standalone Build poll is skipped for that entry state. Headless keeps Build plus its existing driver only. TestRunner, scene-serial/index finalization and debounced config/project-state writes no longer depend on layout/draw. The existing host counter is sampled once and reused by visible simulation; hidden UI refreshes its existing last_time. Automation/AI/dialog actions, scene/autosave, quality/font and gameplay/video remain with the visible owner; their minimized continuity, genuine background simulation, true OS pause separation remain gaps. The existing private auto-stop now checks elapsed time after a paused host yield without counting a completed iteration; its exact production timer/branch controls are separate from actual OS minimize acceptance. Timed/forced project-state writes now retain dirty state after failed publication and retry through the existing debounce/forced owner. The private root setter now refuses A-to-B and close when outgoing publication fails, retaining A identity and dirty values; dialog and scene-context callers must propagate refusal before rebinding. Same-project scene admission and parse failure boundaries have separate controls. The actual production should_quit body and an ordinary missing-pump mutation are checked separately from GUI/OS rendering.

Paused auto-stop ownership: application/jce_engine.c retains the only elapsed-limit origin and frequency. Its private completed_iteration input distinguishes existing full/headless accounting from a paused host poll; only the latter passes false. tests/application/test_jce_engine_minimized_autostop.c compiles the complete actual private timer and paused branch extracted during configuration, with narrow counted host ports and separate ordinary missing-exit/counting-paused-work mutations. No public engine API, generic background update, scheduler, OS focus adapter or renderer owner is added. Native execution receipts and real OS minimize remain distinct.

Editor RenderSettings admission: core/jce_editor_effective_render_settings.cpp requires the actual current ISOLATED filesystem for isolated paths and uses public open/size/read/close plus the existing JSON-memory parser. A missing filesystem, wrong policy, missing file or rejected JSON cannot fall through to CWD host data. Valid VFS content remains usable without the legacy convenience-reader callback. The one MiB limit precedes this caller copy allocation/read; a PAK-backed open already eagerly decodes and is not covered by that total-memory bound. HOST custom-source/cooked fallback and authored quality overrides retain their existing owner. Once-per-context scene diagnostics distinguish file, quality-only and defaults in DIST. Real ArchiveWriter/PAK/VFS epoch/CWD regressions and the ordinary old-host dispatch mutant are separate from GUI/project switching and pixel acceptance.

Frame attribution keeps async completion dispatch and the sole engine_frame_cap_wait as existing owners; jce_engine.c now wraps their actual iterate callsites in engine.async_completion_pump and engine.frame_pacing spans. The spans measure elapsed intervals, with no scheduler or cap-policy change. A returned pump is not an all-work completion witness, and a pacing interval is not CPU execution time. Keep independent OS CPU, presentation and GPU engine samples separate from trace-enabled captures.

Editor UI explicit-font refusal: ui/jce_editor_style.cpp keeps explicit files authoritative on successful atlas admission. Missing/nonfile overrides, source-read/size refusal and atlas-admission refusal emit distinct WARN reasons per reload. A refused override retries the existing shared role (Ink Free-first Latin or separate CJK role) before unchanged outer compatibility/stock/builtin fallbacks; an identical rejected resolved path is not reread. The retry is bounded to two attempts and uses the existing font-buffer transfer/free owner. Actual source bodies/host role/buffer ownership controls use typed file/logger/atlas-admission ports; they do not certify real glyph parser/raster/GPU output or physical path aliases.

Editor focus admission: core/jce_editor_update synchronizes real public window flags into the existing ImGui focus event queue immediately before NewFrame through stateless viewport_work.h. INPUT_FOCUS requires neither HIDDEN nor MINIMIZED. The pinned ImGui AppFocusLost field is cleared at EndFrame; it is not persistent native focus storage. Continuous unfocused frames therefore reobserve window flags rather than rely on a past loss event. Flag-query failure supplies no invented focus event and certifies no native focus fact. Captures still bypass the positive-layout focus/clip gate. Exact actual preframe callsite, real queued ImGui events and an ordinary missing-sync mutation remain distinct from OS UI/presentation/GPU acceptance.

Lifecycle report required-service timing uses bounded per-name aggregates of inclusive elapsed milliseconds and exact exported integer measured nanoseconds. No cross-phase sum is a CPU duty measurement because nested services overlap; preserve both clocks, min/max/sum and normal outcomes. The exported ns integer is retained exactly, not a lossless original QPC conversion certificate. Full trace EOF, hash stability, history/active/name limits, zero-loss footer and paired ownership continue to control acceptance.

Editor font input ownership: ui/jce_editor_font_data.h invokes only an owning AddFont callback (FontDataOwnedByAtlas=true). The pinned ImGui 1.92.6 source-init failure path rolls back and frees registered input before returning NULL; the helper never performs post-call cleanup. Accepted sources remain owned until atlas Clear/destruction. The existing native font-data target exercises actual invalid 128-byte fonts in both ordinary and merge modes, once-only release, accepted-source retention and teardown; the existing host-font Python module checks the actual helper with an ordinary old-double-free negative. These controls do not certify glyph coverage or GPU pixels.

Editor cadence policy: main.cpp and its existing private cadence rates derive STOPPED background 5 Hz and Play background 15 Hz from the same missing/invalid saved setting. Valid authored settings remain authoritative for both. The exact main Play argument, actual Play receiver and lifecycle owner have positive and ordinary wrong-argument controls; policy tests are distinct from native presentation and GPU measurements. The independent editor.frame_rate.play preview cap defaults to 60 and composes with a positive project FPS by taking the lower positive limit; 0 removes only the Editor cap. The Play presentation owner retains the original project FPS, applies the current preference before host begin and updates the existing running_fps restore value during temporary modes. Native runtime/project settings, fixed-dt and viewport capture admission remain unchanged. Initial config/apply and live main/receiver preview wires have separate ordinary missing-wire controls; actual quiet GPU/DWM acceptance remains separate.

Editor requested benchmark evidence: core/jce_editor_state.cpp emits the existing benchmark-spawn marker through common jce_log_write so DIST macro stripping cannot remove requested workload admission. Requested and actual spawned counts are retained, including partial/failure results; runtime logger filtering remains authoritative. tools/tests/test_editor_benchmark_log.py extracts the complete production function with counted scene/logger ports and an ordinary old-macro negative. A marker alone is not visual or performance acceptance.

Editor project-switch admission: dialogs/jce_dialog_project.cpp snapshots and force-publishes outgoing overlays only for a different normalized root or close. The stateless existing project-context header owns path identity (Windows case-insensitive, POSIX case-sensitive); dialog and SceneSerial root/asset-base comparisons share it. Failed publication retains outgoing identity/dirty state and prevents rebinding; same-project or preserved-root scene admission does not require unrelated writes. Scene sync/async/Bundle entrances preflight before teardown, while full root/asset follow remains after read/JSON/schema admission. Sync uses one host-read byte buffer for preflight and the real scene loader. Existing project-context fixtures consume actual selection/preflight/dialog bodies with real resolver/host filesystem and a typed publication port; the ordinary always-flush mutation is separate from actual persistence IO tests. This is not full ECS/stream rollback or GUI acceptance.

Desktop GL 2.1 environment fallback uses the stock GLSL 120 cooker and the
existing scene renderer. Physical sky uses the analytic base with a two-sample
RGBA8 density atlas from `middleware/scene/jce_sr_cloud_atlas.c`. The authored
stylized dome remains a separate mode. Gerstner water reuses analytic
displacement, and legacy FFT water reads the existing scene-owned field into
bounded transient vertices. Terrain submits only its base PBR layer when the
layered program is missing. `middleware/scene/jce_sr_grass_expanded.c` owns
bounded grass buffers and cell ranges. Foliage and particle submission retain
their respective expanded geometry owners when instancing is absent.

FoliageCluster transient admission: middleware/scene/jce_sr_fcluster_submit_internal.h shares the existing grass receipt shape and checks whole-cluster capacity, allocated data/count and copy bounds before touching instance bytes. jce_sr_environment.c retains its whole-cluster rendering semantics and publishes accepted only after the existing bgfx_submit. The renderer has one bounded reason bitset; each rejection reason warns once per owner lifetime and trace-on submissions retain scoped requested/accepted/rejected/reason and owner/view/entity. This is CPU command admission, not GPU completion or pixels. The dedicated eight-case provider fixture and ordinary old-short-allocation-guard mutation do not certify shader/cache/alpha-mask fallback paths. Those earlier refusal/fallback paths retain their existing owners and separate acceptance needs.

Editor GPU vendor preference: core/jce_editor_renderer_startup.h consumes the existing optional UI-int key editor.gpu_adapter (0 Auto, 1 Intel, 2 NVIDIA, 3 AMD). A nonempty per-launch JCE_GPU_ADAPTER overrides saved values without changing them. Otherwise valid saved values call the public preference setter once before device creation; missing or invalid values retain the engine default. Preferences only stores the choice for the next launch, labels vendors rather than physical adapter identities, and does not reconfigure the running renderer. Invalid saved choices warn once during startup; actual selected device/fallback remains the renderer log authority. The existing startup fixture covers real config KV admission and typed setter calls with an ordinary omitted-GPU-wire contract plus strict actual-main wiring. Native GPU selection, power, memory and visible UI remain separate acceptance.

Native Windows D3D Auto prefers the first hardware adapter ranked by optional DXGI high-performance enumeration, admitted only when its LUID maps to the exact first classic bgfx PCI key within Caps::gpu. Failed preferred initialization retires that attempt before one same-backend retry with the legacy choice. Explicit vendor/device, headless and non-D3D retain their policy. The fallback and diagnostic display identity are owned by os/platform/jce_window_adapter.c and its private header: SDL HMONITOR -> GetMonitorInfoW GDI name -> active CCD source-name packet/source LUID -> pinned bgfx classic DXGI protocol (CreateDXGIFactory1 with IID_IDXGIFactory, EnumAdapters, GetDesc). GetDesc1 supplies only software flags. Production never uses DXGI Output.HMONITOR to infer physical output ownership. Two bounded complete path/mode observations and repeated window/GDI identity reject observed topology changes; explicit API/header/string/count/capacity/allocation/unsupported-path reasons preserve fallback. Clone paths may repeat one exact source identity. Distinct LUIDs sharing a key remain distinct; only the backend first key match can represent the CCD candidate, with backend_key_mismatch otherwise and actual Caps::gpu range refusal separately. Refusal retains validated CCD identity/counts and an explicitly predicted backend-expected LUID/index, never actual selected ownership. The renderer reuses per-create observations across attempts and reports these diagnostics with requested/observed/selected keys in its existing startup scope. Complete-owner counted C99 ports and wrong-LUID/former-DXGI-output/old-strict-key/blind-first-key mutations are CPU policy/cleanup evidence; native caps, process-bound GPU LUIDs, Copy traffic, DWM attribution and physical-device acceptance remain separate.

Editor font refusal before dependency assertions: host font adoption now rejects at most 100 bytes before transferring ownership, matching the pinned ImGui ordinary input-size precondition. Stock fonts reject that original-size range before allocation and log the actual size/path. Both actual owning AddFont nodes use a private local RAII helper to permit only recoverable user-error refusal during that call and restore the exact prior IO setting on return. Internal assertions and diagnostics remain active. Real ordinary/merge parser rollback, true/false setting restoration and tiny source once-only release are separate from arbitrary malformed-table safety; no second font parser or universal validation claim is introduced.

Preferences category persistence: panels/jce_panel_preferences.cpp restores preferences.active_tab from the existing session UI-int store only inside its existing config-load guard. Valid IDs use the existing tab inventory; missing/invalid values choose General and invalid input warns on load. User category clicks write through the same KV/config owner, without per-frame writes or another cache. Appearance is ID 1. Profile-restored panels and the official full-backbuffer screenshot route exercise real application layout/draw, but do not prove OS mouse/keyboard delivery or menu/hotkey reachability.

Project prepared configurations: editor/src/core/jce_project_prepared_assets.{h,cpp} is the private shared Bundle Browser/Dist graph adapter for the finite canonical render_settings.json, physics_layers.json, settings/project-settings.json, settings/render_pipeline.rp.json and settings/input_actions.json roots. Existing configurations become required public Bundle roots and retain descriptor/LUT closure; absent optional configurations add no requirement. The public packer keeps VFS/source precedence, with prepared canonical files before Settings/.jce resolver fallbacks. Relative resource-root spelling remains stable for scene address derivation; resolver host paths are absolute. The build manager forwards its actual prepared directory and Dist skips the legacy separate input append. Source/prepared byte-conflict, real host closure and selected-file disappearance controls plus actual omitted-roots and project-settings omission mutations are separate from native runtime setting and pixel acceptance.

Independent runtime viewport presentation: engine/src/application/jce_rt_viewport.c
owns copied, bounded extent and sized initial creation options before on_start;
jce_rt_script.c exposes the appended ScriptHost reader and existing optional
resize-message dispatch, with exact float bits in publication traces. Editor
core/jce_editor_play_viewport.h owns one pending request in the existing Play
host; GameView resolves through the current public pipeline extent before
creation/drawing and maps valid pointer coordinates into the same Canvas/FBO
pixels. Public RuntimeDesc and invalid-pointer semantics are unchanged. Controls:
tests/application/test_jce_runtime_viewport.c and
tests/editor/test_jce_editor_play_viewport.cpp, with missing actual-owner wires.
CPU controls are separate from physical first-click, scaled-render and DPI evidence.

- Native DPI pointer units: `os/platform/jce_window.c` owns cached actual SDL
  window points and point-to-drawable conversion; default host reuses it for
  ScriptHost and Canvas. `test_jce_window_coordinates` / `_old_scale` are exact
  owner CPU controls; actual desktop DPI and click/selection require GUI evidence.

Private scene model admission diagnostics: engine/src/middleware/scene/
jce_sr_model_diagnostics.c/.h, owned by the scene charter. They consume the
existing path-keyed cache and shared logger/trace; no second asset registry.
Grass shader rearm stays in jce_sr_grass_prepare.c, invoked only through the
existing entity-cache reset in jce_sr_environment.c. Retained success and
explicit-reset failure retry share the renderer's ordinary shader owner.

Camera MRT ownership: scene/jce_sr_prepass_targets.c/.h owns the bounded
view-keyed entries, staged replacements, generation and one-time retirement.
jce_sr_prepass_target_adapter.c provides typed backend operations and current
borrowed aliases. Cull, reset and SceneRenderer destruction use that same owner.
Counted owner controls and Noop scene integration remain separate from actual
GPU completion, pixels and Copy-engine measurements.

Build request admission stays in `editor/src/core/jce_build_manager.cpp/.h`;
its private preflight refusal shares native/script request ownership. Idle
refusal clears previous output identities and publishes FAILED; busy reentry
preserves in-flight state/error/outputs/log. Build Profiles consumes dispatch
acceptance before Play progression and defaults user-project Pack to Dist;
explicit Release remains a development package. The existing Dist publisher
owns final single-executable publication. `tests/editor/test_jce_build_request_admission.cpp`
extracts actual request/Play bodies with CPU ports; old-state and old-acceptance
contract variants must compile and fail unchanged assertions. No compiler,
native process, GUI, GPU or relocation result is inferred from these controls.

Readback CPU lifetime stays in renderer/jce_render_readback.c and its private
jce_render_readback_internal.h hooks, called by the actual renderer create/
shutdown owner. renderer/jce_renderer_frame_serial_internal.h supplies uint32
serial order to queue, callback and impostor readiness without a second frame
owner. Bounded queue destroy uses central submit and retains unresolved borrowed
destinations until backend stop. Transition trace counters describe attached/
retiring queue and owned CPU-buffer counts/bytes; after-stop CPU release is not
GPU residency/completion. Exact-owner readback controls extract the complete
central submission body, with old-direct-frame/old-raw-compare ordinary runtime
contrasts. Screenshot and impostor wrap controls plus actual Noop queue restart
consume the same lifetime wiring. Public ABI shapes and upstream bgfx remain
unchanged; submitted/latched/readback-ready do not imply presented/device-idle.

The same readback fixture executes the complete production renderer destroy body
with pending/cancelled destinations and typed counted backend-return ports.
The real-source configure order guard stays separate from the ordinary extracted
body early_backend_stop control, which must compile and fail guarded-free runtime
assertions. This closes CPU borrow/call-order coverage only, not GPU completion.

Cooked texture failure transport stays in renderer/jce_texture.c's opaque CPU
object; renderer/jce_texture_diagnostics_internal.h uses the same log/trace owner
for stage, path/source hashes and observed-capability diagnostics. Existing UI/
scene caches own retries. resource/jce_cook_policy.h is the shared format default:
Web/AUTO RGBA8, desktop BC and mobile ASTC; explicit import overrides stay local.
tools/jce_cook.c revision 3 and resource/jce_bundle_pack.c writer revision 4 own
incremental invalidation. Complete texture-TU controls with real cooked/Pak bytes
and actual producer/cache controls remain separate from physical Web acceptance.

Web sample-query ownership stays in os/platform/jce_webgl_query_internal.c/.h
and jce_webgl_interop.c/.js. The sole C gate delegates legal sample queries to
pristine Emscripten and initializes only the owned first output; the JS bridge
observes current context/extensions and forwards errors to the existing GL owner.
cmake/JCESDKHelpers.cmake links the wrap/bridge only for Web applications;
engine/cmake/JCESDKInstall.cmake carries the same JS file for SDK relocation.
Complete C/JS source-owner controls and a real Emscripten pointer probe are CPU
wiring evidence, separate from rendered Web acceptance and physical GPU caps.

Scene-video phase ownership: middleware/scene/jce_scene_video.c owns the public
decoder-only `jce_scene_video_advance`, combined compatibility update and private
`jce_scene_video_internal.h` render preparation. The real scene renderer admits
preparation after its renderer/scene guards. Existing component handles and the
last uploaded counter retain single ownership, final-frame eligibility and
multi-view idempotence; failed replacement preserves the prior image and retry.
Path edit and ECS lifetime keep one retirement command, not a GPU completion
claim. Common scoped CPU records identify advance, accepted upload and retirement.
Full owner plus real ECS counted-port controls and the exact renderer prefix
are separate from physical background/presentation/video acceptance.

Actual Web API admission: os/platform/jce_webgl_context_internal.h transports
API/GLSL/renderer and one current-context epoch from the existing interop cache.
renderer/jce_renderer_web_api_internal.h owns strict admission over the existing
renderer_caps version authority; real renderer post-init calls it before active
backend acceptance. Unknown/mixed/below-floor observations refuse through normal
cleanup; API info, running name and begin-frame clear stale verified state.
Re-admission requires renderer lifecycle recreation, with no automatic resource
reconstruction or whole-host GPU-command blocking claim. The private lifecycle
frame gate consumes this same status in Begin/View/End and central submission;
refusal is FAILED and does not advance driver/submission/retirement/upload work.
Engine entry/event/watch and post-callback checks terminate lost-context frames
through existing application failure/cleanup, independently of background CPU
permission. Native policies/caps stay independent. Actual branch/central-owner
CPU controls and JS cache controls do not replace physical Web context
loss/recovery, resource reconstruction or GPU-completion acceptance.

Editor Bundle inspection: panels/jce_bundle_inspection_internal.h owns the
shared public file/content-snapshot summary and async work; Bundle Browser Open
owns explicit target selection, bounded cached bytes and request-generation
publication. The existing panel shutdown disarms inspection completion before
release. File sidecars and snapshot-only content are distinct evidence; neither
path mounts a VFS, changes the current scene or certifies runtime rendering.

Scene resource preparation: public `jce_scene_renderer_prepare_resources` is
owned by SceneRenderer and calls existing video/texture/model upload owners.
Private per-round facts and observed terminal jobs guard reuse; Scene/Game use
only the public SDK stage after their asset finalize. Actual bounded CPU
controls and ordinary negatives live in `tests/middleware/scene`; the fifteen
video owner groups remain. Full animation/water/render maintenance and Editor
colour cadence are separate remaining work, not a delivered live-preview fix.

Public GPU timer transport stays in renderer/jce_gpu_caps.c with a private
existing-device identity bridge in renderer.c. The independent
JceGpuTimingObservation/getter exposes signed raw ticks/frequency, explicit
unavailability, read-time context/host-frame and backend sample-frame identity;
it allocates no registry and advances no frame. It does not modify the older
JceGpuStats layout or establish an engine/GPU frame join or completion fence.
Profiler and Profile Analyzer consume the same public admission and the common
Profiler observation helper; unavailable values cannot become zero-cost GPU
statistics or CPU intervals reduced by other-thread waits. Actual-owner renderer
and Editor-helper CPU controls remain distinct from SDK and device validation.
Profile Analyzer's private `jce_panel_profile_analyzer.h` owns complete bounded
CSV encoding and public atomic publication under the selected Editor profile.
`jce_panel_profiler_trace.cpp` owns local event-consumer registration qualification
and raw-counter display; it creates no global thread registry. Their fixtures
consume actual production bodies with ordinary old-behavior controls. Missing
registration prefixes and backend memory sentinels do not establish OS exit or
measured zero. Physical UI/export and performance validation remain separate.
Profiler derived row presentation and Frame Debugger lazy refresh share the
existing sample/epoch owner; covered same-host CPU history does not copy view
rows. tests/editor/profiler_chart_controls.cmake owns complete native controls
for sample retention, actual row conversion and both presentation consumers.

Backend CPU callback consumption belongs to renderer/jce_renderer_bgfx_callback.c
and its private jce_renderer_bgfx_profiler_internal.h. A bounded compiler-TLS
stack preserves copied names, calling-thread IDs and parent scopes without
inventing external thread registration or exit. The private capability witness
separates compiled selection (-1 unknown/0/1), bridge support and saturating
actual begin observations. Init-attempt and owner-shutdown publication does not
prove backend success or GPU completion. Actual callback and old-no-op controls
live in tests/renderer/test_jce_bgfx_profiler_trace.c; the existing renderer
admission fixture consumes real Noop init/frame/destruction facts. Editor trace
consumers retain unregistered TIDs with Unknown registration rather than dropping
their work. Configuration, actual callback observation, graphical execution and
stable frame performance remain separate evidence.
Failed init, API refusal and renderer-allocation cleanup finalize their attempt
at the actual quiescent/owner-shutdown boundary. Preserve failed outcomes and
counts before fallback reset; an unfinalized superseded attempt stays failed and
does not prove join. tests/renderer/test_jce_bgfx_profiler_attempts.c and its
existing-owner extraction controls consume the real failure branches.

Drawable admission observations belong to application/jce_engine.c and private
jce_engine_host_lifecycle.h. engine.drawable_transition emits only the first
actual observation, capture-generation change or changed decision inputs.
Keep native-query validity separate from known zero extent; transition flags
are sampled only during publication, not continuous focus or OS occlusion.
The observer does not control rendering and commits its cache only after an
admitted trace scope. The existing test_jce_engine_background actual-owner
fixture checks all state changes and ordinary omitted/unknown-as-zero controls.

Offscreen target extents belong to renderer/jce_offscreen_target.c. The public
read-only jce_offscreen_target_get_extent reports the live allocation's admitted
size, independently of retained content or current-frame submission. The shared
Editor viewport screenshot adapter consumes it only for bridge texture sources;
PostFX outputs retain their own dimensions. CPU owner/capture controls do not
replace pixel readback, frame geometry or GPU-completion evidence.

Exclusive host copy: public os/core/jce_filesystem.h::jce_fs_host_copy_file_new
is implemented by the existing jce_filesystem_host.c exclusive stream creator,
bounded64KiB transfer and shared close owner. Required out_created retains
destination ownership even on later failure; existing paths never authorize
cleanup. Optional fs.host.copy_file_new records transfer/close stages. Existing
host-atomic real-IO controls include created/close/overwrite ordinary mutations;
SDK C99 smoke consumes the installed symbol. This is neither fsync nor a
concurrent-source snapshot. No added worker or file-ownership registry.

tools/gpu_view_observation.py owns opt-in --gpu-view-observations inside the
existing lifecycle_trace_report.py canonical reader/B/E lifecycle. It validates
the nine ordered gpu.view.<uint16_view_id> fields under the actual successful
renderer.submit_cpu owner, active TID/category/id/parent and typed ranges.
Sparse latched samples keep each view's source_frame separate from CPU/API
frames. Known epochs partition bounded view aggregates; unknown epochs stay
ungrouped and break repeat continuity. One last signature qualifies only
adjacent repeats, not global uniqueness. Valid zero, unavailable timing and
exact wide integers remain distinct. Explicit requests need packets plus full
EOF/capture/loss/closure/fingerprint and reader/observer qualification; default
reports stay compatible. tools/tests/test_gpu_view_observation.py owns typed
and ordinary missing-wire controls. These records provide no full-frame join,
presentation, fence or GPU-completion proof.

The optional Engine frame-context-v2 report copies bounded public host PODs
inside the active owner-thread app.draw and seals them with that completed
interval. The Editor's private Game observer resets before layout and separates
requested/allocated/display dimensions, policy/refusal, prepare, CPU tail and
Image facts. The shared public ImGui draw observer counts final queued commands
and actual main/recording view submit calls; main-view calls may target a
screenshot FBO and do not prove GPU execution or presentation. Default/v1 paths
retain their schema and unobserved behavior.

Explicit frame-context-v3 reuses the same capture, bounded encoder and host
viewport admission. application/jce_engine.c supplies its already measured
completed interval start/end and frequency to jce_engine_kpi_capture.c; no new
clock read, service or public ABI is introduced. jce_frame_kpi.c encodes exact
uint64 decimal clock strings, and tools/benchmark_suite.py plus perf_bench.py
validate the requested schema, duration and ordered counter intervals. Legacy,
v1 and v2 bytes remain unchanged. Counter correlation does not prove GPU
execution, presentation or performance causality.


Filesystem read diagnostics: tools/filesystem_read_observation.py owns the
opt-in15-field packet view inside the existing lifecycle_trace_report.py
canonical reader. It bounds active state and retained failure/anomaly details,
keeps unknown counts, and uses submitted caller correlation rather than the
service B/E self-owner. Its charter/tests are tools/AGENTS.md and
tools/tests/test_filesystem_read_observation.py. No runtime producer/API,
path-text output, errno/optional inference or additional trace reader is added.

Standalone default-entry frame observations: the static SDK include
engine/include/jce/application/jce_default_frame_observation.inc.h is consumed
only by jce_default_main.inc.h. It reuses the public same-interval Engine report;
no renderer, collection service or exported ABI is added. Request, prepared
allocation and CPU-call-return are separate; direct fallback resets bridge
facts. Display, final texture and ImGui counts remain unknown. Actual draw-prefix,
complete scene-draw branch and final publication controls share
tests/application/test_jce_default_cache_deferred.c and its CMake owner, with
ordinary missing-early-publication and missing-CPU-return controls.


Audio master final ownership: `engine/src/middleware/audio/jce_audio_master.inc.h`
retains the existing device/slot/tap owner; `jce_audio_master_gate.h` provides
lock-free callback admission, actual borrowers and current-callback refusal;
`jce_audio_master_lifecycle.inc.h`/`_internal.h` expose private Engine-session
open/close/final-retire entries, including the actual disabled-hook definitions.
`engine/src/application/jce_engine_audio_retirement.inc.h` owns only its Engine
trace adapter. Separate `tests/middleware/audio/master_lifecycle_controls.cmake`
and `tests/application/audio_retirement_controls.cmake` register actual-owner,
complete-Engine and ordinary negative controls; native device and physical Web
claims require separate runs. No public audio/Engine ABI is added.

The shared-master OOM control forwards actual atomic allocation/log calls and
latches one failure per session. `test_jce_audio_offline_owner_thread.c` compiles
the complete audio TU and observes real worker-owned offline graph release after
mark_main; public serialized/offline control behavior has no new global-main
restriction. Private Engine lifecycle ownership remains main-thread only.

GPU annotation dependency selection belongs to the existing shared build
driver and conan/hooks/hook_bgfx_wasm_fix.py, with auto/on/off request, effective
upstream option and Debug/WindowsStore compiled-state metadata kept distinct.
Unchanged compiled defaults retain exact historical bgfx package IDs; request
metadata alone does not rebuild dependencies. Only changed/unknown compilation
adds annotation identity keys, with equivalent enabled requests sharing a package.
The existing renderer callback attempt publisher owns the bounded log/trace
fields. Configuration controls in tools/tests/test_conan_configuration.py use
the actual hook and optional selected upstream CMake expression plus real
Conan variable rendering; renderer controls use the complete callback owner.
No GPU completion, native-marker availability or performance gain follows from
these compile-capability observations.

`tests/editor/host_services_controls.cmake` owns complete actual event,
should_quit and CPU-maintenance fixture extraction, host target registration and
the existing Recorder-control handoff at the same CMake registration boundary.
editor_app_event sends public QUIT and WINDOW_CLOSE to the existing layout
quit-confirmation owner, retaining headless exclusion, cadence and input forwarding.
The same fixture's old_quit_only control removes only native-close admission and
must compile then fail ordinary assertions; OS close/modal/cleanup evidence is
separate from these typed CPU controls.

Editor LAN client lifecycle belongs to `editor/src/core/jce_editor_lan_discovery.cpp/.h`.
Refresh alone opens the existing public transport's scan. The ordinary initialized
CPU-maintenance owner pumps expiry independently of UI drawing; shared panels
shutdown stops/clears the client before UI teardown. No background thread, startup
listener or duplicate scan state is introduced. Complete actual service/transport
controls and strict actual lifecycle-wire mutations live in
`tests/editor/lan_discovery_controls.cmake`; OS UDP/firewall/UI and native retirement
remain separate validation.

PBR early alpha admission belongs to `engine/shaders/pbr/fs_pbr_main.sh`, shared
by stock and generated material entries; `engine/shaders/pbr/AGENTS.md` records
its order contract. The complete material/view/alpha block follows graph and
fade-dither, preceding the unchanged shadow calculation/contact/cloud owner.
`tests/renderer/pbr_alpha_order_controls.cmake` and its actual-fragment fixture
own semantic/output parity and typed dependency-order controls, including an
ordinary old-order mutation. Shader compilation, GPU helper lanes/derivatives,
backend pixels and timing remain separate evidence; no public API is added.

Exact-zero stock point attenuation admission is owned by the same shared
`engine/shaders/pbr/fs_pbr_main.sh` point loop. It preserves positive squared
falloff, packed light/shadow indices and finite well-defined contributions before
skipping zero-attenuation shadow/BRDF dependencies. Spot, area, Forward+ and
quality policy retain their existing owners. Complete actual-loop/direct-BRDF
CPU controls and ordinary old-full-work/epsilon mutations belong to
`tests/renderer/pbr_point_attenuation_controls.cmake` and
`test_jce_pbr_point_attenuation.cpp` under the shader and renderer-test charters.
Actual backend compilation, pixels and timing remain separate validation.

Local shadow atlas depth fetches belong to the complete shared
`engine/shaders/pbr/fs_pbr_decl.sh::sampleAtlasTile` owner. Explicit LOD0
uses the existing one-mip depth target and inherited MIN/MAG point sampler;
normal bias, projection, tile clamps and hard/3x3 PCF quality stay unchanged.
Backend compilation and actual sampler/image/native-frame validation remain
separate requirements under the shared PBR shader charter.

Console presentation admission is private to `editor/src/ui/jce_editor_layout.cpp`
and `editor/src/panels/jce_panel_console.cpp`: the frozen frame owner and actual
current/main viewport IDs suppress only covered same-host content, preserving
Begin/End and Clear-on-Play before the gate. Independent host log drain and
retention remain unchanged; the no-argument content wrapper admits old callers.
Existing Console and layout-route fixtures consume the complete actual owners,
with an ordinary missing-presentation-gate control. Only unrelated About PAK
calls are typed unavailable ports, removing that CPU fixture's shader-cook link.
Native pixels, panel-phase attribution and frame-time improvement remain separate.

Editor pointer event submission: core/jce_editor.cpp retains the complete
jce_editor_process_event owner. Recognized uncaptured button events submit their
own host-window position before the edge, matching motion coordinates without
panel/desktop offsets; existing Game mouse capture still rejects UI pointer
submission. tests/editor/mouse_input_controls.cmake compiles the complete owner
with genuine ImGui queue/NewFrame/SmallButton and narrow destination ports.
Its ordinary old_button_position variant removes only that actual position
wire. CPU controls cover no-motion/stale position, same-pump press/release,
outside release, capture recovery and admission refusals; they do not certify
native input or the cause of a particular desktop Max failure.

Editor launch diagnostic snapshots retain their existing private owners:
`editor/src/core/jce_editor.cpp` stores overview/static-bake presence and one-shot
counters in `s_editor`; `EditorInternalState::operation_metrics` stores its
first-use observation-presence snapshot; `editor/src/jce_editor_main.cpp` stores
the cadence value-predicate snapshot in `EditorState`. Init/exit/state-reset
boundaries refresh them without retaining environment pointers or freezing
live rendering/cadence policy. `tests/editor/launch_diagnostics_controls.cmake`
and `test_jce_editor_launch_diagnostics.cpp` compile actual helpers/consumer
branches and lifecycle wires with counted environment ports and an ordinary
old-scans negative. These CPU controls do not establish native GUI execution,
GPU/presentation completion or a performance improvement.

tools/build/windows_build_job.py owns explicit build-only Windows kernel Job containment and bounded cleanup for caller-exclusive process trees/services. It is not integrated with jce.py/default/runtime runners. Exact helper image policies, held FILETIME/membership observations, planned vs emergency actions and atomic cleanup receipts are validated by tools/tests/test_windows_build_job.py. Shared MSVC brokers remain unsupported pending separately proved instance isolation.


Editor live trace history: editor/src/panels/jce_panel_profiler_trace.cpp owns bounded retained thread/counter/active-span caches and saturating local refusal-event observations. Existing-key updates remain admitted at capacity; unmatched ends do not decrement another tracked begin. Clear resets only the local view, while prefix-loss qualification and canonical Core producer/capture/export remain separate. Existing complete-consumer fixture tests/editor/test_jce_profiler_trace_display.cpp and its ordinary old_unbounded variant constrain admission/accounting; CPU evidence is not native UI, physical memory or frame-performance proof.

Renderer private device launch diagnostics: engine/src/renderer/jce_renderer_launch_policy_internal.h/.inc.h own strict bounded ASCII snapshots for JCE_GFX_FLIP_AFTER_RENDER=0|1 and JCE_GFX_MAX_FRAME_LATENCY=0..3. jce_renderer.c admits one immutable request before windowed device acquisition, rejects invalid requests, and shares it across actual fallback attempts. Absent/empty values preserve ctor/reset defaults; explicit flip scheduling is independent from DXGI flip-model and survives actual reset owners. Pinned bgfx 1.146.9306-550 forwards init latency only for D3D11/Vulkan, preserving unsupported D3D12/GL/other defaults; headless/software paths qualify inapplicability. Bounded opt-in CPU request/forwarding/init observations retain backend ACK/GPU completion unknown. tests/renderer/bgfx_profiler_attempt_controls.cmake and test_jce_bgfx_profiler_attempts.c retain exact pre-acquisition prefixes, the full actual attempt, failure cleanup/publication and an ordinary missing-forward-wire control; test_jce_renderer_reset_trace.c retains actual reset entries. These CPU fixtures do not certify physical queue depth, dynamic pixels or any frame-time/latency improvement. Full recreation is required for another latency request.

Profiler Threads and Tasks covered presentation: editor/src/panels/jce_panel_profiler_trace.cpp keeps poll_trace ahead of same-host GameMax admission, preserving its bounded passive cursor/history while skipping all presentation queries/widgets. Existing tests/editor/test_jce_profiler_trace_display.cpp and its exact-owner CMake block consume the real Core ring, full content entry and all display functions; missing covered/poll wire controls must fail ordinarily. No Core producer/async ownership, public ABI or native performance claim changes.

The existing Editor viewport admission owner independently applies explicit
FrameLog/DrawLog natural selectors; an active Game capture retains precedence.
It reuses the strict private parser and real focus/clip/finite-layout gates.
Defaults retain capture behavior. DrawLog remains a latched whole-frame observer
including UI, not a single-viewport or same-frame submission certificate.

Core's existing DrawLog row owner appends a statistics-availability bit from its
one original public GPU-stats query, preserving the old numeric count fallback
and CSV prefix. Full init/update CPU controls consume real file bytes; the bit
does not join shared cull/RQ data or establish GPU execution/presentation.

The lifecycle probe owns post-filter opt-in admission for the existing Renderer
shader and Core FS open-failure diagnostic producers. Its ACK and explicitly
requested no-ACK analysis routes reuse the canonical lifecycle reader once and
reuse its identity hash. Shader provenance and typed FS-read observers keep
their existing owners; raw bounded FS WARN text is not a new parsed protocol
or an all-lifetime/GPU completion claim. Defaults do not enable these observers.

Renderer prepared-draw rollback: engine/src/renderer/jce_render_encoder.h discards
ALL state on its captured explicit/implicit owner after deferred resolution
fails. Successful routing is unchanged. tests/renderer/render_encoder_abort_controls.cmake
and its complete-header fixture cover all three existing routes, next-draw
reuse, TLS isolation and the ordinary missing-abort control. This is CPU state
retirement, not GPU completion or an attributed application failure.

Lifecycle renderer selection: tools/editor_lifecycle_probe.py owns explicit
normalized backend/vendor/tier requests after ordinary environment filtering.
Fresh owned startup preferences/session avoid overriding those requests. Missing
options preserve defaults; failed runs retain requested-only metadata. Actual
backend/device, tier, extent and pixels still need separate native observation.

Colour submission ownership: scene/jce_sr_draw.c prepares CPU light/GI inputs
without upfront uniform seeds; admitted material/model/grass producers retain
their binding owners. Active-GI centroid/restore uploads are a separate legacy
ownership gap, not repaired by deleting seeds. scene/jce_sr_fcluster_colour.c/h
owns whole-cluster admission and complete current frame inputs; environment
retains cache/resource receipts. scene/jce_sr_shadow_frame.c/h supplies shared
canonical shadow selection/splits to full and split-only binders. Renderer
lighting owns the private actual-camera upload; model owns per-band material
fallback. CPU controls and native graphics/performance evidence remain separate.

Toon hair depth: scene/jce_sr_toon_hair.c owns bounded per-viewport eye-depth
and raster-depth targets and schedules their view before opaque colour.
The direct skinned renderer scopes caster submission to a tagged glTF
material and reuses the current animated palette and alpha cutoff. The
receiver binds eye depth only on the eligible face's otherwise unused
metallic-roughness slot. Non-admitted draws retain the existing face atlas.
The Scene/Game/runtime view windows share neither framebuffer contents
nor a public material field for this pass.

Water colour binding: scene/jce_sr_environment.c admits geometry before its
sole default-PBR/global binder, then keeps cloud/depth/planar and final submit
ownership. Later duplicate shadow and view-mode writes are removed. Actual
source-region controls test named-handle uniqueness and refusal without pending
state; water simulation, pixels and throughput remain separate acceptance.

Lifecycle disk-history forwarding: tools/editor_lifecycle_probe.py validates
explicit storage settings through trace_history_storage.history_options and
forwards them to the sole terminal/canonical reader. Fresh-path admission
precedes launch; both ACK and history-only direct validation use the same
exact store. Requested settings and actual closed-store evidence are separate.

## Built-in shader stock policy

`engine/shaders/stock_profiles.json` is the generated catalogue shared by runtime
loaders and `tools/shader_stock_policy.cmake`. Built-in root cook calls opt into
STOCK; generic SDK calls retain unrestricted project shader names. Root CMake
owns sibling policy-qualified asset directories and exact cache metadata; SDK
configuration/install owns relocated catalogue/helper paths. `xp_editor.py`
admits the current catalogue identity and output directory. Configure controls
live in `test_shader_compatibility.py`, `test_shader_stock_policy.py` and
`test_shader_artifact_root.py`; actual driver linking/pixels remain separate.

## Compatibility skeletal geometry

The scene animation instance owns model-local CPU-deformed buffers on the
compatibility GL/ES tier. `jce_sr_morph.c` applies morph weights before
skinning and shares the prepared geometry across model colour, shadow,
selection wireframe and ID passes. Renderer mesh helpers own the skin
equation and source vertex retention. The scene cache keys pose by scene,
entity and exact model and releases buffers on instance retirement or rebind.

## CPU model click picking

The renderer retains pick positions and indices only for the no-readback
path. The scene picker borrows a matching entity pose and performs a finite
ray query on model triangles, with separate hit, miss and unavailable
results. Scene View uses the nearest hit for single clicks when GPU ID
picking is unavailable. GPU marquee and synchronous drag retain their own
input contracts.

## Windows owned-child QA transport

`tools/build/windows_owned_launcher.py` + `tools/build/native/` own the explicit
XP-compatible C99 helper producer. `tools/build/xp_vm_owned.py` owns its bounded,
nonce/identity/byte-bound observation admission; `xp_vm.py` retains pinned remote
transport and default behavior. The public driver forwards opt-in owned-launcher
and terminal-ACK selectors. `tools/terminal_ack_observation.py` remains canonical.
Pure controls live in `tools/tests/test_windows_owned_launcher.py`,
`test_xp_vm_owned.py` and `test_xp_vm_terminal.py`. Native compile, held-child
retirement, active desktop, Editor GUI and whole-tree outcomes are separate scopes.
The helper is test transport, never an Editor delivery dependency.
Explicit flat scene/asset inputs belong to `tools/build/xp_vm_inputs.py` and
reuse the bounded verified uploader; `tools/tests/test_xp_vm_inputs.py` and
`test_xp_vm_window.py` cover source/collision budgets and explicit visible-window
policy. Neither inputs nor visible launch certify rendered viewport content.

## Experimental Windows XP probe boundary (A1)

See [Windows target compatibility](windows-targets.md) for the explicit legacy
Editor lane and its separate build, import, guest and graphics acceptance.

- `tools/build/xp_compat.py` + `tests/compatibility/xp_core`: independent
  LLVM-MinGW-XP C99/C++20/PE static-import probe, outside JCE SDK/Editor
  and Conan's Modern MSVC binary graph. Driver entry: `jce.py xp-probe`.
- `cmake/toolchains/windows-*-xp-llvm*.cmake`: XP-only compiler selection;
  separate from Conan-generated Modern toolchains.
- `engine/src/os/platform/jce_entropy.c`: XP CryptoAPI adapter behind explicit
  `JCE_WINXP_LEGACY` flag; normal CNG path preserved. No full-engine XP claim.
- `conan/profiles/windows-*-xp-llvm`: A2-only reserved host identities, not
  activated in A1.

Generated-output placement and prospective default migration are documented in
contracts/build-output-layout.md. Existing lint/audit owners keep gate logs in
build/reports; the benchmark owner uses build/validation/benchmark with its
existing unique IDs. Native/Conan caches and final delivery defaults stay with
tools/build; scripts remains forwarding-only. Default-path/explicit-override and
retained-history CPU controls live in tools/tests/test_generated_output_defaults.py.

Cookie array compatibility/ownership: `renderer/jce_light_cookie_internal.h`
inside the existing lighting `s_lu` owns initialized atlas/white-array creation,
stage-13 binding and destruction. Shared renderer shader-profile policy excludes
compatibility GL/ES; shared PBR declaration/main compile out the same cookie
interface/blocks. `tests/renderer/light_cookie_texture_controls.cmake` and
`test_jce_light_cookie_texture.c` consume the exact private owner with typed CPU
ports and complete-owner negative controls. See renderer and PBR charters.

Composite LUT resources/settings: `renderer/jce_postfx_grade_internal.h` is
the actual private per-pipeline owner consumed by postfx create/set/get/submit/
destroy. It keeps authored LUT data borrowed, uses shared compatibility policy
and diagnoses unsupported requests once. `shaders/postfx/fs_composite.sc`
removes the matching 3D interface/block in compatibility builds; see its shader
charter. `tests/renderer/postfx_grade_owner_controls.cmake` and its typed fixture
cover CPU ownership/admission with complete-owner negative controls.

Water spectrum authoring uses middleware/scene/jce_scene_water_spectrum.h,
scene.c's existing ECS ownership and components_terrain.c's JSON domain.
water_fft_internal.h owns shared pure descriptor validation; WaterField retains
FFT resource ownership and transactional replacement. Inspector uses the existing
component registry/history, not a separate water configuration store.

Water light-normal mip packing lives in scene/jce_water_gradient.h; its render-only texture/scratch lifecycle lives in jce_water_gradient_gpu.c. Scene cache slots own those objects; sr_water_destroy centralizes the existing water teardown. The private core jce_half_internal.h reuses the original net encoder, with the published network symbol retained as a forwarding wrapper.

Temporal water foam: middleware/scene/jce_scene_water_foam.h owns the optional
public authoring component. jce_water_foam.c/.h owns private history in the
existing WaterFieldSet slots and shared driver clock; jce_water_foam_gpu.c/.h
owns render-only density/detail mips. jce_scene_components_water_foam.c owns
JSON adapters through the existing component registry. test_jce_water_foam and
test_jce_water_foam_gpu cover state/authoring and typed resource controls.

jce_scene_water.c owns water component defaults and authoring-to-field/history
resolution using existing scene and WaterFieldSet APIs. ECS storage remains in
jce_scene.c.

WaterCrest authoring lives with the scene water component registry. Its renderer binds a two-vector optional constant, reuses the dominant sun and its shadow, and applies Fresnel-weighted transmitted crest light in the modern physical water fragment.

Underwater scene-depth ownership: the stock runtime and Editor bridge pass
depth independently of fog. renderer/jce_underwater.c consumes the existing
renderer-owned color alias only when sampling the depth attached to a managed
destination. renderer/jce_offscreen_target.c owns alias allocation, reuse and
bounded retirement with that exact parent generation. External framebuffers
retain caller-owned attachment constraints. Scene traversal orders the two
sequential RGB blends before later consumers. CPU alias and draw receipts do
not substitute for native SRV/RTV/DSV binding checks.

WaterCaustics uses the scene component registry and a private bounded RGB atlas owner. The atlas uses one optional pre-colour view at base+69, padded periodic tiles and primary FFT displacement. The consumer reconstructs the opaque receiver through the shared screen reconstruction helper.

WaterNearshore derives a complete bounded bed through the shared scene terrain
projection. The scene WaterFieldSet owns the conservative shallow flow and
advected shore foam on the same FFT clock. The modern water draw uploads both
nearshore and wake through a padded stage 13 atlas. Runtime buoyancy and camera
submersion sample the published scene surface through the additive C API.

FFT WaterMotion is authored in the scene ECS. jce_scene_water.c resolves the
enabled motion into the existing shared field acquisition. The water field
owns phase, cap and combined CPU query state. The FFT owner applies horizontal
motion before packing, so the existing renderer, nearshore and caustic
consumers all read the authored displacement.

WaterNormalDetail authoring belongs to middleware/scene's versioned water ECS
and dense registry. jce_scene_water_normal_detail.h is the additive public
surface; jce_water_normal_detail_config.h and
jce_scene_components_water_normal_detail.c own validation/defaults/JSON.
The existing water_spectrum_authoring target and SDK smoke own storage and
public-consumer controls, separately from water draw/phase and GPU evidence.

Camera JSON parsing/writing belongs to scene/jce_scene_components_camera.c,
with the original function bodies and one central component registry row.
No public symbol or camera wire-format change accompanies this extraction.

Water normal-detail transport: `middleware/scene/jce_water_field_internal.h`
exposes the field's evaluated spectral phase without a second clock;
`jce_sr_environment.c` binds an optional WaterNormalDetail vec4 per modern draw
with explicit zero reset for missing, disabled and non-FFT bodies. Renderer
creation/water lazy initialization/destruction own that uniform. Rain/Gerstner
retain the independent scene-time input. `jce_sr_water_optics.c` loads trace and
resolve through the existing named-program resolver; development shader
replacement and packaged asset policy share that owner. Actual GPU shader and
resource observations remain separate from CPU/compile controls.

Shared water normal reconstruction: `shaders/pbr/water_surface_normal.sh` owns
modern colour/optical FFT filtering, shallow retention, micro/rain/ice normal
composition and pre-flip facing; `water_surface_noise.sh` supplies the exact
common interpolant. Optional `water_normal_detail.sh` adds two independently
filtered, phase-advected normal bands using existing gradient/sampler resources
and the renderer's WaterNormalDetail uniform. Absence preserves the static old
path, explicit zero strength suppresses micro slopes, and phaseRate 0 freezes
both FFT and detail without freezing rain/scene time. Lighting and screen-space
ray traversal remain in their original passes. CPU/compiled/native/GPU/performance
claims retain separate evidence boundaries.

Continuous buoyancy/wake contact: application/jce_runtime.c supplies actual fixed
dt and body velocity to middleware/scene/jce_water_ripple_internal.h. The existing
ripple solver owns bounded local relaxation and the unchanged public one-shot
impulse through one footprint traversal. Runtime and solver regression targets
own long-duration/cadence and local semigroup controls; no public ABI is added.

Modern water incident lighting: `engine/shaders/pbr/water_lighting.sh` owns
RGB/intensity ambient composition, mean-interface directional incidence and
the homogeneous absorption source used by `fs_water_body.sh` and
`fs_water_optics_trace.sc`. Reflection/opaque samples and direct glints retain
their existing radiance ownership. Zero illumination cannot create water or
foam emission; existing GL2.1 shading is a separate legacy path.

Visible-sky water fallback: `shaders/pbr/fs_water_optics_trace.sc` owns the
upward, in-viewport, four-exact-clear-neighbour sample of completed opaque sky
after unsuccessful geometry SSR. `scene/jce_sr_water_optics.c` owns the private
sky-success vec4 lifecycle and per-viewport reset; `jce_sr_environment.c`
publishes actual sky-submit success. Existing colour/depth resources are reused;
this is neither offscreen IBL nor proof beyond the finite geometry march.

Optional document HUD overlay: engine/include/jce/application/jce_rml_scene_overlay.h
maps scene component values to RML element ids through an opt-in, project-owned
ui/hud.bindings manifest. Editor Game View uses its own offscreen render view;
the default host uses the backbuffer UI view. Legacy scenes remain unchanged.

Rendering correction owners: scene/jce_sr_tod_key.h delegates sun/moon key
selection to the existing environment predicate while scene-renderer writeback
retains solar identity. scene/jce_sr_foliage_lighting.h supplies bounded incident
energy to the existing ramp binder. renderer/jce_pbr_cull_override.h is the
private two-sided scope consumed by final geometry and PBR normal state; the
scene draw/cull owners bracket it and avoid incompatible instance batches.
renderer/jce_underwater.c admits complete resources; scene renderer/environment
own bounded shared-resource retries and foliage-uniform lifetime.
pbr/csm_filter.sh supplies balanced deterministic 9/25-tap static CSM filtering;
renderer/jce_csm.c owns the invariant fit and scene/jce_sr_shadow.c derives its
cull basis from the actual cascade VP. CPU controls and native graphics/perf
acceptance remain explicitly separate.

Mesh index storage: renderer/jce_mesh_index_data.h packs triangle and wire
topology into disjoint spans of one immutable static IB. Mesh/skinned/model
submitters and scene/jce_sr_water_caustic_atlas.c bind explicit spans; reduced
LOD and meshlet buffers retain separate owners. renderer/jce_gltf_loader.c
rejects incomplete required geometry uploads and unwinds CPU/GPU ownership.
scene/jce_sr_water_mesh_cache.h gates failed water geometry creation against
device frames, keeps matching resident entries usable and commits replacements
only after allocation. Existing scene reset clears the refusal latch.

Editor Scene/Game viewport admission consumes the existing deferred scene-load
state before resource work. io/jce_editor_scene_serial.cpp still owns path repair
and finalization; main update polling and the existing layout loading overlay
remain independent. core/jce_editor_viewport_trace.h records SceneLoading as a
private refusal reason, without changing existing reason identities.

Imported MASK depth coverage: renderer/jce_pbr_depth_internal.h declares
material-owned depth/normal/velocity programs; jce_pbr_material.c owns lazy
loading, alpha binding and shutdown. jce_model_depth_bind.inc.h selects the
effective per-primitive material; jce_model_depth.inc.h owns shared depth and
motion submissions, while remaining model GPU/morph routes reuse the selector.
renderer/jce_model_depth_internal.h exposes only the private cutout query to
scene/jce_sr_cull.c so material-blind velocity queues use per-object fallback.
pbr/material_depth_alpha.sh owns stock texture-times-factor alpha with the
same UV transform and strict cutoff. Opaque and procedural foliage paths keep
their existing behavior; custom graph/tint alpha and imported double-sided
shadow policy are outside this change. Backend compilation and native cloud,
tree-shadow and motion witnesses remain distinct from CPU binding controls.
# Procedural sky IBL numeric identity

The private `engine/src/middleware/scene/jce_sr_ibl_refresh.h` owns admission
against the last accepted numeric sky state. SceneRenderer stores this small
snapshot; the environment owner invalidates it on queue/bake/upload failure
or HDR transition. Formatted log keys are not cache authority. The existing
worker and GPU upload lifecycle is unchanged. Direct CPU owner and actual
admission/completion tests do not establish target-machine frame-time gains.
The environment owner admits new procedural work only while scene IBL is
enabled. Already accepted tasks keep the same poll/retirement owner, and the
last admitted snapshot survives disable for correct re-enable/cache reuse.


Sandbox text loading: middleware/script/jce_script.c captures whether the caller supplied a non-nil fourth argument before luaL_loadbufferx pushes its compiled chunk. This preserves ordinary one/two/three-argument factories and existing explicit environments without weakening text-only restrictions. The existing script sandbox CTest adds delayed-global factory, isolation and refusal cases. Selected-C/Lua integration is separate from a complete native Editor build.

Static cooked triangle shapes: middleware/physics/jce_physics_bullet.cpp preallocates known signed-capacity storage and appends the original valid ordered triangles without Bullet's linear vertex-weld scan. Shape ownership and BVH construction remain unchanged; duplicate vertex storage is an explicit memory tradeoff. Native Bullet coordinate and BVH-ray comparisons are separate from JCE contact-solver and target-host performance acceptance.


Document HUD tag resolution: application/jce_rml_scene_overlay.h owns bounded
operation-local tag-ID/first-entity snapshots for values, visibility and input.
Each snapshot uses the public entity order, includes duplicate-tag first-match
semantics and expires before the next operation; no scene pointer/handle cache
survives retagging, deletion or a scene swap. Key-edge mailbox and release
quarantine keep their existing owners. Tests/application/rml_scene_batch_controls.cmake
registers complete-header C99/C++20 CPU controls with actual tag-helper source
and typed dependency ports; its per-binding mutation restores repeated scans.
Editor GameRender adds six phases through the existing perf-phase owner for document input/focus,
value sync, document draw, Canvas and physics debug. Native SDK, full Editor
compilation, pixels and whole-frame performance require separate validation.


Cloud-noise integer reuse: renderer/jce_cloud_noise.c shares XY-prefix and
Z-tail hashing inside the existing cellular-noise owner. Wrapped hash identity,
all cell visits and floating distance order remain unchanged; cloud resolution,
slicing and publication remain with their existing owners.

Active composite AgX output: shaders/postfx/fs_composite.sc returns display-linear
color before the shared output gamma. tests/renderer/postfx_agx_controls.cmake
and test_jce_postfx_agx.cpp execute complete actual source with typed CPU ports
and an ordinary missing-transfer negative. See postfx and renderer-test charters;
CPU acceptance is distinct from native shader compilation and visible output.

Optional exact-path script cost partition: application/jce_rt_script_cost.c
now owns the existing public runtime cost iterate/reset implementations and the
private bounded process-latched selector in jce_rt_script_cost.h. The existing
rt_tick_gameplay callback timer supplies current on_update ms to two fixed
literal phases, script_focus/script_other; no public ABI, timer, registry or
per-entity phase names are added. Default capture remains unchanged.
tools/editor_play_diagnostic.py explicitly forwards --script-path/--ecull-detail
and distinguishes detail_request from actual detail_observation: exact native
selector receipt, finite zero-inclusive partition rows and same-frame sum
qualification; missing evidence stays unavailable independently of raw capture
completion. Ecull detail remains six existing serial miss-path subphases.
The existing runtime script-cost native target and diagnostic Python suite own
regressions. Source/CPU control success does not certify rebuilt native Lua,
Editor/GPU output or performance improvement.
