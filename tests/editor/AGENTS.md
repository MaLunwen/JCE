# tests/editor — Editor regression ownership

viewport_composite_controls.cmake consumes the actual shared composite and
screenshot helpers plus complete Scene/Game composite-through-Canvas blocks.
Typed public SDK ports cover D3D11/top-left/empty-Canvas admission, conservative
any-Canvas and unknown-owner refusal, backend/origin fallback, Game physics
debug, Scene scaled TSR/RCAS and borrowed-input no-self-copy. Canvas render and
UI-event retirement stay observable when no copy occurs; captures retain the
chosen source, extent and yflip. The old_copy, missing_canvas_guard and
missing_tail_guard controls remove actual production decisions; old_direct_flip
restores the former PostFX-only unconditional flip. All must compile
then fail ordinary unchanged assertions. WILL_FAIL alone is not an execution
receipt. These CPU controls do not certify GPU pixels, sampler precision,
presentation or whole-frame benefit, including any scene that contains Canvas.
The fixture also consumes both actual PostFX create blocks, with distinct
factory results and distinct Scene/Game output/bridge witnesses; it must not
hide a lifetime alias behind one fake shared pipeline or output handle.
Direct D3D11 screenshots require no flip; direct OpenGL and composited OpenGL
screenshots retain the bottom-left flip. Bridge captures retain allocation size.

Game-only output-target controls additionally consume the public allocation
descriptor with typed SDK ports and the complete actual Game tail. They verify
Canvas destination, retained display/capture source, format/extent/alias,
matching-but-stale retained targets, valid zero handles, real physics state and
non-overlay Canvas fallback. old_game_copy, missing_overlay_wire, missing
physics/alias/format/current-extent guards, reject_zero_target and
bridge_capture_source must compile and fail ordinary assertions. The first
unchanged Game tail RED is separate from these mutants. The Scene wrapper
provides its current content_dt_sec local without changing Scene production.

loose_scene_content_controls.cmake consumes complete actual Scene project
preflight/follow, directory update, content apply/setter and Game content bodies.
Manifest/ancestor/filesystem/path decomposition owners are real; project commit,
Canvas/particles and renderer destinations are typed ports. It covers loose
content before entity creation, real file presence at the selected font address,
stale project retirement/refusal, same-owner replacement, manifest priority and
isolated Bundle policy. A counted project-transition retirement port refuses a
late root close that would destroy an already opened Bundle without a manifest.
The complete Asset Browser open entry routes through its counted loader and
must not overwrite a successful or refused load's content base afterward.
Configure also checks both actual host loaders' admission,
publication, retirement, follow and entity-clear ordering. These checks do not
execute the complete file loader, rasterize that font, run gameplay or prove GPU
pixels. Ordinary old-preflight, old-Game-root, missing-early-binding, old-asset-entry
and late-Bundle-retirement targets
must compile then fail unchanged assertions. The real render-settings/VFS suite
adds loose authored MSAA/shadow preservation and no unrelated quality load;
its old-quality target restores only that complete owner's former quality gate
and must fail ordinary assertions. Keep initial unchanged-owner RED evidence
separate from later controls. Root owns serialized native and real GUI validation.

Console viewport controls compile the complete Console owner and complete
shared panels owner with real ImGui, bounded log rings, registry/session and
Core trace ring. Only unrelated Editor owners, clipboard and narrow widget
observations are ports. Every original Selectable/Popup remains reachable;
only offscreen Text submission is omitted. The same actual full-Text body is
the fractional-pixel row/ScrollMax reference, and actual AddKeyEvent Home/End/
Page events exercise native navigation. Context-menu Clear must preserve its
active frame snapshot. Actual panels shutdown must remove the sink before
resource destruction, preserve unrelated core registrations, release cached
storage, remain idempotent and reinitialize. Real scope/counter events must
restore the parent and leave no active CPU work. Ordinary full-Text, ignored
child, early snapshot clear and missing shutdown wire controls must terminate
normally and fail unchanged assertions; no compile/setup crash is evidence.
Cold-start and hidden-Console regressions use the real panels-init owner, Core
WARN emission/flush and the actual shared host CPU poll before any Console draw.
Only unrelated manager destinations are ports. Complete panels and host copies
carry separate ordinary missing-startup and missing-hidden-drain wires; both
must compile then fail the same regression assertions. The host-services fixture
counts Console polling and its canonical trace while retaining headless exclusion.
Fresh warning filters and disabled
Clear-on-Play distinguish missing subscription/drain from intentional filtering.
Retain the existing bounded pending/display rings and shutdown barrier; these
controls must not add a second logger, replay a file or invoke an absent API to
manufacture an initial failure. Preserve ordinary unchanged-owner RED evidence
before adding production lifecycle wires.
These CPU controls do not certify native GUI pixels, callback races, full heap
accounting, GPU completion or complete-scene performance gains.

scene_overview_controls.cmake compiles complete actual Overview, entity focus
bounds, orbit/clip, reset and focus-animation update bodies with real Scene,
component-registry, transform and CPU Camera owners. The asset-cache port
returns no loaded mesh; only that real focus fallback is covered. Registry and
metadata controls exclude disabled components/entities and non-renderable logic,
light and sky outliers. Preserve streamed chunk union/vertical widening, zero
chunks, finite bounds, empty reset and uninitialized no-op controls. The original
hardcoded owner must reach normal doctest assertion failure with the same tests;
retain its ordinary EXE/terminal evidence. CPU framing does not certify loaded
mesh envelopes, native toolbar delivery, rendered pixels or GPU performance.


hotkey_persistence_controls.cmake compiles the complete actual registry with
public JSON/host IO and a real ImGui CPU context. It checks legal/unbound and
physical modifier-key save/load through shutdown/init, unknown action and
missing-key compatibility, strict malformed input refusal without partial live
mutation, unsafe setter/label admission and real write-failure retry. The
old_load and old_set complete-owner variants change only the original unchecked
loader or setter admission and must fail unchanged ordinary assertions after
compilation. Test files use isolated process-owned profiles. These controls do
not certify new-process GUI reopening, OS input, rendered UI or workspace INI.

pipeline_panel_controls.cmake compiles the complete actual Render Pipeline
panel state/init/content/tick with real RP and temporary Bundle owners. Typed
widget observations select only the requested checkbox/preset. Controls cover
panel initialization before project/Bundle changes, an unrelated checkbox
retaining all other author fields, LOW effective clamps without losing HIGH
requested intent, independent unsubmitted path/status drafts, and same-draw
intentional presets. The applied-policy summary is observed through typed text
ports; real LOW-tier clamps remain separate from author state and the actual
Save/load round trip. All four readouts must use normal text rather than the
disabled-text port. Same-draw edits precede the readout. Old cached-init,
effective-getter, mirrored-readout and dim-readout variants must compile then fail ordinary
assertions. These CPU controls do not certify physical ImGui
interaction, native Bundle UI, shader output or GPU performance.

welcome_startup_controls.cmake compiles the complete actual once-only layout
and known-project-history helpers with the real pure Welcome policy. Typed
current project/path/loading, preference and config ports cover Bundle/scene
without a project, pending loading, empty/failed admission fallback, history
and later manual visibility. old_project_only changes only the policy input
back to the old project-root wire and must fail unchanged ordinary assertions.
These CPU decisions do not certify scene loading, physical Help clicks or
rendered Welcome visibility; Root owns actual Native and Editor validation.

play_swap_retirement_controls.cmake registers actual Stop, terminal shutdown,
complete snapshot capture/full-scene restoration and seven exact scene admission
prefixes. The fixture uses real ECS/SceneSerial, history identity and its existing
budget fault hook, plus an actual isolated FS; only UI/render borrowers and project
persistence are typed ports. It covers playing/stopped retained failure, unchanged
A content/mount/root, successful real serialized recovery, pending without runtime,
and terminal-only discard with FAILED trace. Old skip-retained, ignored-refusal
and false-terminal-success controls must compile then fail ordinary assertions.
Terminal finish also covers never-started, actual Stop already completed, pending
without runtime and active Play: repeated terminal calls must retain balanced
finish scopes without inventing extra Stop transactions. The old unconditional
terminal Stop control mutates only that production admission wire and must fail
its exact Stop begin/end count assertions.
Prefix controls stop after admission; configure checks guard ordering before real
unmount/clear/stream operations. These are CPU ownership controls, not a desktop,
GPU, runtime VM or complete later scene-load qualification. The existing render
trace fixture's terminal Play port covers only probe-cancel ordering.
These targets link the public `JCE` facade because actual Stop pulls the full
Runtime/Engine dependency closure. Their executable supplies the standard
zero-size embedded asset blob; this is the no-assets host contract, not a
replacement for Runtime, AI/navigation, localization or scene restoration.


`test_jce_editor_project_bundle_retirement` consumes exact production project
normalization/preflight/commit code through RP publication, the close-preview
entry and pending-Play guard, plus the actual Bundle override and RP owners.
Typed runtime/stream/root/module ports test Play/paused/pending retirement before
root/module changes, no-last-scene transition, repeated close, project close,
missing-RP tier fallback, same-root no-op and admission/late publication refusal.
The old_missing_retire and old_close_before_stop mutations must compile and fail
unchanged ordinary assertions. Register project_bundle_retirement_controls.cmake.
The registration is function-scoped and verifies the selected mutation in the
actual written compiler input, including exact wire counts. Its successful Stop
port clears the retained-snapshot flag; actual restore failure/retry belongs to
the dedicated Play/history fixture, not this transition-order fixture.
The source excerpt ends before localization/general settings; these controls do
not certify UI navigation, actual native module unload, ECS rollback or rendering.

`test_jce_editor_bundle_pipeline` compiles the actual temporary RP owner and
actual pipeline with real Archive/PAK/VFS inputs and controlled hardware caps.
It checks read-only prepare, apply, once-only restore, pending project edits,
cross-tier requested intent, absence-only tier fallback, strict malformed and
over-limit refusal, cleared failure outputs and null admission. The actual
scene loader wires are checked for prepare/isolate/apply-before-deserialize
and close restoration, preserving existing project-context source contracts.
The real Core ring additionally verifies balanced prepare/apply/restore scopes,
explicit refusal outcomes, successful absent fallback, caller restoration,
actual RP.publish child ids and effective-scale counters, and exactly one
restore despite repeated close. Active-owner admission refusal preserves its
policy without publication. Whole-owner missing_apply and old_active_snapshot
controls must compile and fail unchanged ordinary assertions; missing_apply
also removes the actual publication child and must fail that count. Registration
checks the unique mutation wire and actual written compiler input. Register via
bundle_pipeline_controls.cmake.
These are CPU ownership/consumption controls, not native Bundle UI, full ECS
rollback, pixel equality or GPU performance acceptance.

The complete Game input bridge fixture retains every native focus/hover/capture,
position-prime, mapping and commit assertion. Its virtual-source groups exercise
unfocused native rejection versus explicit virtual admission, independent state,
held buttons/UP, bounded wheel/cursor validation, source precedence, geometry and
owner/reset cancellation, and the actual P/V/T/A/S callback sequence. It uses no
ImGui/SDL/renderer. Same-owner old-virtual-consume and old-commit-order mutations
must compile and fail ordinary assertions alongside the retained old-prime
control. Controlled payload/callback observations are not physical mouse, OS
focus, GUI reachability, graphics, zero-overhead or performance certificates.

`test_jce_editor_game_canvas_pointer` strictly extracts the complete actual Game
panel Canvas-pointer block and links the real bridge. Typed ImGui/setter ports
cover hidden OS cursor with virtual held/release, non-consuming runtime delivery,
native text/capture/Play gates and changed/covered geometry. The old-virtual
variant removes only its actual peek admission and must compile then fail
ordinary assertions. The bridge fixture separately covers repeated peeks,
transient preservation, cancellation and invalid current geometry. These CPU
observations are not a native widget click or rendered-game certificate.
Registration lives in game_canvas_pointer_controls.cmake; retain the existing
native/virtual/commit-order fixture assertions and controls.

This charter extends [the parent test contract](../AGENTS.md). The domain is
Editor source ownership, independent of assertion language. The CMake metadata
owner classifies each target; graphical evidence requires an explicit graphical
fixture rather than inferring it from an Editor file name or a Noop renderer.

The existing layout-routes fixture also consumes the actual maximized viewport
state functions, complete Scene/Game dock blocks, same-frame snapshot/overlay
and Scene entry prefix, including its real deferred-pick/gizmo cleanup bodies.
Counted typed UI/content/history/brush ports check one content owner per frame,
retained Begin/End calls, covered cleanup, own Scene overlay continuity and
activate/restore/hidden terminal paths. Its old-covered contract restores the
former Scene and Game dock guards and must fail ordinary assertions. Existing
preset routing assertions remain unchanged. CPU ports do not certify physical
input, saved docking state, GPU work, probe progress or a performance improvement.

Use scripts/jce.py with explicit targets and a matching filter. Keep generated
fixtures/reports under build and preserve failing raw evidence.

Play viewport request/publication controls retain their actual source slices and
compile the real private CPU scope through `jce_core`, with C++20 scoped to the
three viewport wire targets. Their ordering/layout assertions remain separate
from the dedicated real-ring trace controls; they certify no GPU or physical UI.

Project manifest persistence uses the complete jce_editor_project.cpp adapter
and real public Project/JSON/host IO. test_jce_editor_project_persistence covers
SDK edits, unknown nested metadata, cached object/unrelated borrowed identity,
bundle replace/clear, corrupt input and actual atomic staging refusal. Failed
publication cannot mutate the cache or original bytes. These CPU controls do
not establish native Save SDK button reachability; current GUI evidence is
separate. The Engine's test_jce_project_script_modules owns generic round-trip
and existing/new/ambiguous manifest admission.
The same complete-adapter fixture covers external known startup/Bundle edits
being refused without changing current source bytes or cache, explicit reload,
external unknown extension/format changes remaining editable, repeated saves
and intentional clears with loader defaults. These are stale-source semantics,
not OS compare-and-swap, concurrent-writer or every signature-allocation-failure
certificates. The original owner must fail the two external-known groups through
ordinary assertions; compilation failure or missing execution is not a control.

## Build cancellation coverage

The existing Build owner now retains Stop before release clears stopping.
A stopped child's exit zero remains non-success: compatible FAILED status,
real exit_code and an explicit cancellation diagnostic. Cancelled requests
cannot advance queued tools, run finish verification or publish. Ordinary
success/failure and failed next-spawn/finish retain their existing behavior.
The canonical editor.build.tool_completion service records the CPU completion
transition with success/failure/cancellation; it is not child execution time.

`test_jce_build_process_completion` consumes strict extracts of the production
Build/Queue/Finish structs, log/drain/release/spawn/Stop/poll and public poll
wrapper, plus the real outcome, trace and host-maintenance owners. Typed process
and publisher ports are counted. Its behavior groups cover:

- Stop with zero/nonzero exit and queued/unqueued continuations.
- Ordinary success and nonzero failure with finish/publication admission.
- Successful next step and actual failed-spawn handling.
- No-draw host maintenance driving the actual Build poll.
- Graceful refusal and overdue kill preserving cancellation.
- Trace success/failure/cancellation and failed next-spawn/finish transitions.
- Trace-off preserving the caller identity without emitted observation.
- Actual pipe diagnostic severity independent of stdout/stderr origin, chunked
  CRLF/EOF handling, compiler/CMake/Ninja forms and non-diagnostic source paths.
  Classification is separate from actual exit-based publication admission.

The same-body `test_jce_build_process_completion_old_stop_wire` removes only the
captured Stop input. It must compile and fail unchanged ordinary assertions;
compiler failure, missing execution or a crash is not an accepted negative.
The same-body `test_jce_build_process_completion_old_output_severity` replaces
only the real classifier dispatch with the incoming stream level. It must also
compile and fail ordinary severity assertions; its origin metadata is retained.
The current local build-cancel598 evidence is two CTest targets accepted:
positive 7/7 groups and 963/963 assertions, negative 5/7 groups failed with 113
ordinary assertions. Source fixtures and supported commands are rerunnable;
ignored local receipts are not shipped dependencies or clone-wide guarantees.

```bash
python scripts/jce.py test --suite editor --variant debug --arch x64 --jobs 2 --target test_jce_build_process_completion --target test_jce_build_process_completion_old_stop_wire --filter "^test_jce_build_process_completion(_old_stop_wire)?$"
```

No-draw counted controls are not physical hidden-window/input tests. Native
Stop gestures, OS child kill/reap, real published files and current Editor UI
reachability require separate artifact-bound acceptance. The formal headless
build-project consumer invokes the real Build pipeline but does not exercise
Build Profiles Recheck or certify its tool-probe UI route.

## Render settings isolation coverage

The real Archive/PAK/VFS fixture installs its transparent public PAK reader
provider before constructing a mounted filesystem, as required by the public
filesystem contract. Mounted owners release before the scope restores the
previous provider. Preserve the eight behavior groups and the ordinary
old-host-reader control; provider setup failure is not a settings assertion.

Bundle inspection controls compile the complete private summary/work owner with
actual Bundler, cooperative Async, host reads and Bundle inspectors. They cover
exact targets, optional sidecar refusal, cached bytes after source removal,
capacity, cancellation, unchanged active VFS and stale request generations.
Per-target owned directories isolate concurrent ordinary target-bypass mutants.
Both missing-file-target and missing-memory-target contracts must compile and
fail unchanged ordinary assertions. No ImGui interaction, browser or target
runtime execution is certified by these CPU controls.

## Shared panel lifecycle coverage

`test_jce_editor_panel_state` links the complete shared panel owner and binds
its existing public Bundle Browser shutdown signature to a counted port. The
Console init signature also has a counted port: each panels init calls it once,
shutdown adds no init, and a later panels lifecycle calls it once again. The
fixture does not link the real Console owner or certify its subscription work.
The lifecycle group checks exactly one call to each teardown owner and their real
Bundle Browser, Test Runner, hierarchy, file-viewer, Console order, while preserving all
visibility and Console assertions. These ports verify shared dispatch, not
actual Bundle Browser task cancellation or snapshot release; those belong to
its inspection owner and separate integration/UI acceptance. Missing fixture
symbols are build failures, never ordinary assertion-control evidence.

## Adaptive resolution identity controls

`test_jce_editor_dynres_gpu_timing` compiles the complete private production
policy and a strict extract of the real Scene bounded-getter/consume wire.
The typed public getter controls raw/context/sample identity without creating a
renderer. Nine cases cover real wire consumption, repeated high/low latched
samples, existing 3/12 steps, dead zone, invalid observation refusal, repeated
identity after an unavailable read, context replacement, raw source-frame wrap
and bounded scale/streak endpoints. Two complete-owner contract targets remove
only the identity guard or the actual getter wire; unchanged ordinary assertions
must fail. A compiler failure or crash is not a passing negative control. These
CPU cases do not certify GPU completion, frame joins, actual reallocations or
DWM performance.

## Benchmark wall observation controls

`test_jce_benchmark_observation.cpp` consumes the complete private observer and
uniquely extracted actual panel sampling wire. Typed clocks/renderer observations
and the real Lifecycle registry exercise zero-origin wall time, long intervals,
unique UI/host identity and wrap, explicit missing coverage, clock/context/profile
changes, register failure/retry and paired WILL_QUIT/destructor cleanup. GPU timer
absence must not invalidate a valid CPU wall interval. The v1 baseline codec
rejects legacy/schema/profile changes, incomplete coverage and truncation. Two
whole-owner ordinary controls omit the wall clock or profile match; unchanged
assertions must fail normally, not through compiler errors or crashes. These CPU
controls do not certify hardware timings, selected adapter, presentation or GPU
completion. Real native/Editor/Web host acceptance remains separate.

## Play mixer source selection controls

`test_jce_editor_play_mixer` consumes a strictly bounded extract of the actual
Play mixer descriptor segment with typed public presence and profile-path ports.
Eight groups cover project priority, uncertain authored/legacy paths, confirmed
absence, a projectless profile, resolver failure and repeated Play selection. The
ordinary `missing_fallback_check` contract removes only the actual legacy
presence guard; unchanged absence and probe-count assertions must fail normally,
not through a compiler error or crash. No Runtime, audio device or file is
created by these ports. Native metadata, actual Play playback, package closure
and physical Editor access require separate execution evidence.


## Profiler historical chart consumption controls

The normal test_jce_profiler_observation retains its seven timing-admission
groups and also consumes strict extracts of the complete actual Profiler
state/sample/stats/ring/chart bodies and real series draw-call block. Nine chart
groups use counted drawing/public-observation ports for unknown gaps, latest
unavailable history, known zero, ring wrap, context replacement/loss/restoration,
same-context missing timers and empty history. Context invalidation must not
erase UI intervals or query the public GPU observation more than once per sample.

The test_jce_profiler_chart_missing_mask and
test_jce_profiler_chart_missing_gap contracts remove only the actual GPU ring
availability or adjacency guard. Both must compile and fail unchanged ordinary
assertions. The preexisting old-wait-math and old-GPU-wire controls remain
separate. Counted geometry/text ports do not certify native UI reachability,
pixels, physical timing, presentation, selected hardware or GPU completion.

The same fixture consumes the strict actual CPU draw prefix through all six
ring reductions. Three groups prove same-host maximized Game View omits only
CPU presentation while phase capture, current-frame samples, stale UI history
reset and backend epoch invalidation remain live. Ordinary and detached Profiler
views keep presentation. The missing_covered_gate contract removes only that
actual return and must compile and fail ordinary unchanged assertions. The
fixture does not execute the later table widgets or process-memory query; its
strict prefix boundary places those after the tested presentation admission.
Other Workbench tab bodies and their maintenance are unchanged by this gate.

profiler_chart_controls.cmake owns the existing complete chart extracts. The
fixture also consumes actual row refill and exported Frame Debugger refresh;
only later table widgets and narrow formatting observations are ports. Covered
CPU history performs no row conversion; visible/detached views retain fresh
rows, and context/reopen/missing stats retain invalidation. Eager-copy, missing
visible-copy and missing debugger-copy controls must fail ordinary unchanged
assertions. These are CPU ownership controls, not a native frame-gain claim.

## Preset docking consumption controls

`test_jce_editor_layout_routes` consumes the strict complete production slice
containing the shared extension helper, all ten preset functions and their
existing dispatcher. Typed DockBuilder ports use stable window IDs and retain
the final route after repeated assignments. Six groups verify the Profiling
upper-left Profiler/Network/Physics cluster, unchanged other extension routes,
Default proportions, separate Two-by-Two Scene/Game nodes, the other seven
presets and unknown-preset fallback. No alternate layout policy is reproduced.

`test_jce_editor_layout_routes_old_routing` moves only the actual Profiling
extension call back after its explicit cluster. It must compile and fail the
unchanged ordinary assertions; missing extraction bounds, compiler errors or
crashes are failures of the fixture, not accepted negative evidence. Counted
CPU destinations do not prove actual ImGui pixels, scrolling, dock dragging,
saved layouts or native interaction. Root must execute both native targets and
validate the rebuilt Editor separately before recording acceptance.

```bash
python scripts/jce.py test --configure --suite editor --variant debug --arch x64 --jobs 2 --target test_jce_editor_layout_routes --target test_jce_editor_layout_routes_old_routing --filter "^test_jce_editor_layout_routes(_old_routing)?$"
```

## FrameLog viewport selection controls

`test_jce_editor_render_trace` consumes the complete private CPU/operation/trace
helpers, real canonical ring and real ImGui policy, plus strictly extracted
actual Scene/Game early-admission prefixes. Counted public probe/bake ports
preserve original guard precedence and one Scene-owned poll, with no Game poll.
Controls cover caller restoration, nested/early-return closure, independent
phase collection, disabled tracing, invalid/focus/clip/capture decisions,
unavailable/extent/bake/probe/Play-owner refusal and separate admission/reason
samples. Later target/resource reason encoding is helper-level coverage, not
execution of the complete render body. Play scope-placement checks and CPU
adapter tests do not create a Runtime or certify GUI lifecycle/input/pixels.

The same fixture consumes the exact production probe wrappers, pre-layout
maintenance wire, scene-clear/history-clear/shutdown prefixes and complete
Inspector bake branch. Counted public capture/bake/scene/GUI ports preserve
Scene-hidden progress, Game yield, DONE/FAILED-frame latching, layout-started
deferral, impostor priority, current focus/explicit-capture/positive-extent
admission, missing-owner cancellation, pre-release ordering and capture/bake
Cancel routing. The real ring checks nested parent closure, statuses and
trace-disabled behavior. Engine tests separately certify pending destination
retirement; these ports do not exercise actual GPU buffers or native clicks.

Three additional complete-source controls omit only the actual host pump,
current-frame latch or wrapper cancellation wire. They must fail ordinary
unchanged assertions. Post-capture convolution handle/progress is not exposed
by the current capture API and is not certified by this Inspector control.

The actual batch-panel queue/begin body also shares the fixture. Inspector A's
unchanged component address cannot claim B after A completes, fails, is cancelled
through the owner or is externally cancelled and another Editor entry restarts
in the same frame. Successful begins sign fresh tokens; refused begins preserve
the active identity, wrong Scene/entity and zero tokens cannot match, and UINT64
generation exhaustion refuses without another Engine call. Destructive prefixes
and missing-owner cancellation retire a signed request. The complete actual
Inspector `old_capture_identity` control replaces only its token match with the
former global in-flight predicate; cross-entry ordinary assertions must fail.

The complete-helper `missing_cpu` and actual-prefix `missing_admission` targets
must compile and fail the same ordinary assertions. A compile failure, missing
execution or crash is not an accepted negative. Root-owned native/source gates and
a newly identified Editor artifact are required after these source edits.

The existing `test_jce_editor_viewport_work` retains its real ImGui clip/focus
and actual pre-NewFrame synchronization groups. Six added groups cover strict
Capture/Natural/Invalid decoding, missing/empty FrameLog, capture-default and
unknown selectors, natural focus/clip admission, active Game/DrawLog precedence
and unchanged Replay/backbuffer-shot qualification. Test environments restore
their original process values; no CSV, native window or GPU is created.

`test_jce_editor_viewport_work_missing_frame_log_policy` uses the complete
production header with only the actual selector-to-capture wire removed.
It must compile and fail unchanged ordinary assertions; compiler errors and
crashes do not count. Existing old-visibility and missing-focus-sync contracts
retain their original scope. A CPU policy PASS is not a natural performance,
OS visibility, actual workload or complete wall-CSV certificate.

```bash
python scripts/jce.py test --configure --suite editor --variant debug --arch x64 --jobs 2 --target test_jce_editor_viewport_work --target test_jce_editor_viewport_work_old_visibility --target test_jce_editor_viewport_work_missing_focus_sync --target test_jce_editor_viewport_work_missing_frame_log_policy --filter "^test_jce_editor_viewport_work(_old_visibility|_missing_focus_sync|_missing_frame_log_policy)?$"
```

## Analyzer CSV producer controls

`test_jce_profile_analyzer_export` compiles the complete private CSV producer
and actual Panel State/push/at/export/result-key/toolbar slices. Real profile
resolution, capped host reads and atomic publication use the existing public
owners; counted allocation/write faults, logs and UI ports expose admission and
result consumption. Eight groups cover unavailable versus valid zero, ring
wrap/order, bounded failure without partial CSV, full finite float/UINT64_MAX
encoding, invalid/short paths, preservation of a prior complete report, first
failure followed by retry, empty data and allocation refusal.

Four same-body contracts restore the wrong profile route, ordinary writes or
partial-prefix publication, or remove only the actual export-button call. All
must compile and fail unchanged ordinary assertions. Compilation/setup errors,
aborts and crashes are not accepted negative witnesses. This fixture does not
consume the unchanged sampler or certify native UI input, pixels, performance,
GPU completion or an engine/GPU frame join. Root separately validates the
rebuilt Analyzer button, visible result/path and the decoded report.

```bash
python scripts/jce.py test --configure --suite editor --variant debug --arch x64 --jobs 2 --target test_jce_profile_analyzer_export --target test_jce_profile_analyzer_export_old_profile --target test_jce_profile_analyzer_export_old_nonatomic --target test_jce_profile_analyzer_export_old_partial --target test_jce_profile_analyzer_export_missing_button --filter "^test_jce_profile_analyzer_export(_old_profile|_old_nonatomic|_old_partial|_missing_button)?$"
```

## Trace display qualification controls

`test_jce_profiler_trace_display` consumes complete strict extracts of the
actual panel model/poll/Clear, toolbar and counter/thread draw owners, linked to the real
public core trace producer. Typed ImGui ports retain submitted text without a
GUI or GPU context. Ten cases cover registration/unregistration, Clear and
late-open prefixes, actual ring overwrite, qualification recovery, signed
backend-memory sentinels, valid zero/positive and unrelated negative values,
and paused consumption. The toolbar consumes actual enabled/auto-refresh four
combinations and retained thread/task/counter history, including toggling the
real refresh widget. Counted ports reject producer set/reset and implicit
export; real producer statistics and history must remain unchanged.
Producer registration counts and historical row
observations intentionally have different scope.
The Clear endpoint group controls only the public stats observation at zero,
normal, MAX-1 and MAX. It then polls the actual retained ring twice and refuses
replayed history, producer mutation or a cursor wrapping to zero. This finite
endpoint test does not emit billions of events or seed private Core state.

Same-owner `old_state`, `old_gap`, `old_memory`, `old_recording` and
`old_clear_unsigned_wrap` contracts retain all
assertions and must fail ordinary assertions: the old false-to-Exited label,
omitted cursor-loss invalidation, omitted qualified memory display and the old
toolbar that omitted the recording-off state and retained-history hint.
The old Clear addition uses defined unsigned wrap and must fail the unchanged
endpoint and actual no-replay assertions after compilation. Build errors,
crashes or unexecuted targets do not count as controls. CPU tests do not
certify OS thread liveness, physical UI readability or GPU memory residence.

```bash
python scripts/jce.py test --configure --suite editor --variant debug --arch x64 --jobs 2 --target test_jce_profiler_trace_display --target test_jce_profiler_trace_display_old_state --target test_jce_profiler_trace_display_old_gap --target test_jce_profiler_trace_display_old_memory --target test_jce_profiler_trace_display_old_recording --target test_jce_profiler_trace_display_old_clear_unsigned_wrap --filter "^test_jce_profiler_trace_display(_old_state|_old_gap|_old_memory|_old_recording|_old_clear_unsigned_wrap)?$"
```

The trace-display fixture's typed i18n port resolves the registration-scope
key to distinctive localized text and counts the actual lookup. Its unchanged
registration group requires that text and refuses the former literal fallback.
This proves the production hint's key consumption, not native translated pixels.

## Game camera lifecycle consumption controls

`test_jce_editor_game_camera` consumes strict extracts of the actual GameRender
state, camera helpers, begin/end, getter/reset, initialization admission prefix,
complete shutdown and camera-resolution/render prefix, plus actual Play create
transaction and detach/retry wires. Public camera creation, clone, view and
projection math are real; renderer, Runtime, primary/VCam, history and PostFX
ports are typed/countable CPU controls. Thirteen groups cover complete rolled
perspective/fixed-orthographic/dynamic-height preservation, repeated Play/Stop,
camera-less initial navigation, hidden Game, allocation/runtime failure and
retry, missing-camera refusal, release and deferred zero-feedback camera cuts.

The old-selection, old-default-seed, missing-release/admission/cut contracts
mutate only actual production wires and must compile then fail unchanged
ordinary assertions. Camera allocation OOM is a typed Editor admission port;
the actual Engine clone allocation branch has separate camera-owner controls.
These tests do not execute complete GPU initialization, Runtime gameplay, real
scene-camera resolution, mouse input, rendered TAA, native UI or GPU completion.

## Viewport bridge capture extent controls

`test_jce_editor_viewport_capture_extent` consumes the exact shared Scene/Game
screenshot-submit body and its actual view offset, with typed public extent,
source, orientation and request ports. Asymmetric admitted dimensions distinguish
the bridge from its request; independent PostFX source dimensions are retained.
Both direct PostFX and bridge requests preserve top-left/bottom-left orientation.
Missing targets, resize, uint16 narrowing refusal and submission failure remain
ordinary outcomes. The old-requested-extent contract removes only the actual
getter/publication wire and must compile then fail unchanged assertions. The
renderer offscreen-owner fixture separately verifies real allocation clamp,
resize failure/retention and renderer-detach query semantics. These CPU controls
do not prove capture file completion, rendered pixels or per-frame GPU work.

## Cadence cap composition controls

`test_jce_editor_cadence` consumes the complete authoring cadence header, actual
main/Play host receiver, complete Pause receiver and actual initial policy-begin
expression, with the real canonical config and lifecycle registry. Counted FPS
ports cover project/preview 10 FPS, active 15 with idle 30, background above a
running or paused cap, independent zero bounds, initial loss/system pause,
live preview changes during Pause and exact running/Stop restoration. Temporary
requests remain raw inputs to recomposition; stable ticks do not rearm pacing.

The old-limits, old-temporary-caps, old-initial-caps and old-pause-caps contracts
restore only their former production wires and must compile then fail unchanged
ordinary assertions. Existing old-apply, old-shared-background and missing-preview
controls retain their scope. Compiler/setup errors and crashes are not ordinary
negative evidence. These CPU controls do not establish native focus, delivered
presentation cadence, GPU work or physical power reduction. The separate
`test_jce_editor_play_presentation` retains authored/preview/VSync transactions.

## Native module image ownership

`test_jce_editor_native_module_images` compiles the complete actual Editor native
module owner with the real C++ registry, Windows DLLs, host filesystem and sibling
dependency. Only unused linked-language registration is a typed port. A live v1
instance remains callable after the original DLL is overwritten by v2; refused
reload and same-project reload retain the old image even without live instances.
Explicit safe reload executes v2 and retires its old image; project close leaves
no owned images. A strict actual state/module/i18n shutdown slice proves instance
release precedes registry unload and image deletion. The old-direct-load and
missing-shutdown variants must compile and fail ordinary unchanged assertions.
Missing dependencies, setup failures and crashes do not count as those controls.
This is Windows loader/file ownership evidence, not Editor UI or GPU acceptance.
Registration and strict actual-owner mutations live in
`native_module_images.cmake`, included by this module's CMakeLists.

## Scene TSR history admission

`test_jce_editor_tsr_history` uses Unity and the actual SceneRenderState header,
uniquely extracted production TSR prepare/resolve blocks, scene invalidation
entry and direct camera/projection entries. Camera, Halton, matrix math and cvar
storage are real; typed scene/renderer boundaries record submitted parameters.
The fixture checks continuous motion, same-address scene replacement, camera
cut, Play/Stop, input/output extent, disabled/re-enabled gates, skipped/context
submissions and unsuccessful resolves. It must report complete ordinary Unity
results; compiler errors or crashes do not count as a RED/negative control.
The initial unchanged-owner run must fail ordinary transition assertions while
continuous motion passes before production changes. Strict old-admission and
early-commit controls restore those individual old wires and must compile then
fail the unchanged Unity assertions. It does not certify GPU images, the actual
TSR gate on hardware, camera ghosting or GPU retirement. The existing renderer
TSR ownership suite remains the attachment allocation/retirement authority.

The same fixture links the real lifecycle registry and consumes strict extracts
of the actual listener helpers, init admission/tail and complete shutdown/view
destruction bodies. Successful GPU-constructor outputs and public destruction
requests are typed boundaries; this is not complete GPU initialization. Six
focus/pause/device events invalidate at unchanged submission index and extent;
an unrelated memory event retains continuous history. Repeated init keeps one
token. Explicit shutdown checks token clearing/unregistration before every
release boundary, complete ownership cleanup, repeated shutdown and reinit.
An explicit registration-return-zero port verifies full rollback and retry; it
does not inject allocator OOM, whose registry branch has separate Engine tests.
Missing-lifecycle and missing-unsubscribe contracts remove only their actual
production wires and must fail unchanged ordinary Unity assertions. Original
twelve admission/resolve tests and their two controls retain their assertions.
No event port certifies OS delivery, GPU retirement, dynamic pixels or physical
performance. Native execution remains required before acceptance is recorded.

Overview controls also destroy/recreate a real ECS entity and require a reused
index with a new live generation; failure to obtain reuse fails, never skips.
Direct invalid-bounds assertions consume the real helper before Overview.
Complete-helper old_index and old_invalid controls retain the same cases and
must fail ordinary direct-executable assertions with normal doctest termination.
Their CTest WILL_FAIL wrappers are registration expectations only, not evidence
of that ordinary behavioral failure. Keep the earlier hardcoded-owner RED EXE
and receipts unchanged; none of these CPU controls certifies rendered pixels.


## Project-settings root ownership

`test_jce_editor_project_settings` compiles the complete actual settings owner
with real engine dependencies. Memory-only controls cover A-to-B-to-A retirement,
null/empty project close, same-root edits, Windows case identity and explicit
no-project apply. Root selection never reads CWD; explicit load/save retain their
existing fallback. `project_settings_scope_controls.cmake` compiles the complete
same owner with only the former root setter restored. Its unchanged assertions
must fail normally after compilation; setup/compiler failures and crashes do not
count. CTest WILL_FAIL is registration policy, not evidence of ordinary failure.
These controls do not certify disk persistence, native UI, Play audio/physics,
reset of already-applied engine registries/matrices or GPU performance.

## Same-interval frame observations

`test_jce_editor_frame_observation` compiles the complete private observer and
strict unique slices of actual Game guards, prepare/refusal/tail, panel request
and Image wires, and core reset/report integration. Typed public renderer/UI
ports preserve clamped versus requested extent, retained allocation, unknown
PostFX extent, unavailable UI observations, disabled/default behavior and next
interval reset. The missing-reset and missing-completed targets compile the same
assertions with only those real wires omitted and must fail normally. Existing
render-trace and camera fixtures consume the actual new helper as well. These
CPU controls are not a complete Game renderer execution, native input, GPU
completion, screenshot or performance certificate.


## Normal host recorder retirement

`editor_exit_recorder_controls.cmake` extends the existing Play transaction
fixture with the complete uniquely extracted actual editor_app_exit body.
Typed public/editor boundaries cover active/paused Play, Edit, inactive recorder,
repeated exit, prior recorder Stop, invalid startup and the headless route.
Assertions require recorder retirement before Play/audio borrowers or Game,
Scene and UI shutdown. The missing-wire variant removes only the actual Stop
call and must compile then fail ordinary doctest assertions, not crash.
Configure-generated inputs use the shared write-if-changed owner. These are CPU
call-order controls; the typed recorder boundary does not certify an actual
encoder join/file finalization, audio callback quiescence or GPU retirement.

## Recorder actual buffer owner

`editor_recorder_controls.cmake` compiles the complete actual Recorder TU through
`test_jce_editor_recorder_ownership.cpp`. Typed capture/encoder/allocation ports
retain real public SDK I32/TLS, mutex/condition, dedicated worker and joins. The
seed + detached + queued schedule recreates 25 raw payloads under the ordinary
old-vdepth admission control; current admission must remain at 12 and reject
before allocation/copy. A free-port pause proves credits stay owned until the
actual payload free returns. Video and audio producers pause before malloc, at
copy and before queue publication: Stop must retain its session mutex/worker
until their actual borrows return, not unregister then wait an arbitrary deadline.
Node/payload OOM, throwing allocation/copy/encode providers, recursive callback
Stop, saved late callbacks and normal single-frame/audio-track settings exercise
this same owner. The old_partition target must compile then fail ordinary budget
assertions, not crash or time out. Web reports these native controls unavailable.
These CPU ownership controls do not encode real VP9/Opus, join the renderer source,
identify old-source/new-session screenshots or certify GPU/native tail retirement,
whole-process memory, FPS or quality. CTest WILL_FAIL alone is not evidence of an
ordinary negative; retain the direct executable failure and assertion output.

The destroy port reads completed counters at the actual session mutex retirement
boundary and attempts synchronous Start/Stop reentry: inactive publication must
already be visible while the stopping latch retains the owner. A free-credit
case also captures again after actual free/credit return. The closed-to-release
producer schedule checks retained ownership, but without an independent missing-
wait control it does not deterministically certify omission of wait_borrowers.


Recorder resize controls keep the complete actual TU and all prior 12-credit,
borrow/free/reentry controls. Small-to-large before seed consumption and
large-to-small after fixed encoder construction must reject before copying,
retain the callback owner, retire on the ordinary main update, drain only valid
old-format frames and allow a subsequent Start at the new dimensions. Short,
excess, null, invalid-flip and overflow geometry must allocate/copy/encode nothing;
padded odd geometry remains supported. Corrupted declared owned extents are
rechecked before seed creation and before codec push. A typed codec port computes
its fixed I420 converter's last possible source byte against the actual tracked
allocation and refuses unsafe access without reading it. The old_shared_format
control restores only mutable shared geometry and bypasses session-format
rejection while retaining byte checks, borrow fences and real cleanup. It must
terminate normally and fail the same assertions; an actual OOB is never a control.
A typed logging observation counts one diagnostic after join and attempts
synchronous lifecycle reentry while the stopping latch still retains ownership.

`host_services_controls.cmake` owns complete actual event, should_quit and
CPU-maintenance fixture extraction, normal/missing-wire/old_quit_only target
registration and the existing Recorder host-control handoff. CMakeLists.txt
includes it at the same registration point; target flags, ports and generated
paths stay with that focused owner. Typed public event/layout destinations and
the real cadence helper cover QUIT and WINDOW_CLOSE once per unconfirmed event,
already-confirmed/headless exclusion, valid NONE and ordinary forwarding, and
activity before the original input payload is forwarded. The old_quit_only
control removes only the actual WINDOW_CLOSE admission; its complete generated
input is checked and must compile then fail unchanged ordinary assertions.
WILL_FAIL registration alone is not its execution receipt. These CPU controls
do not deliver an OS close event, operate the unsaved modal or prove cleanup.

The existing complete-body host-services fixture adds one counted Recorder
update destination: exactly once for visible/hidden/minimized CPU maintenance,
zero before initialization and in headless-build polling. Its real canonical
recorder service span nests under maintenance, preserves the caller parent and
closes after the synchronous call returns; trace-off produces no events. The
no_recorder control removes only that actual main-service wire and must fail
ordinary counts/spans, independently of the existing missing-all-services wire.
These typed CPU controls do not operate F9, resize a native window, encode media,
prove native/GPU completion or measure whole-frame performance.


The complete actual editor/src/jce_editor_main.cpp source must contain zero
direct Recorder-update calls; CPU maintenance is the unique wire. CMake passes
that actual-source count into the existing fixture, separately from execution
of the extracted maintenance body. The old_duplicate_main_wire control restores
only the former app-update line in a retained complete-main input and must fail
the unchanged zero-direct-call assertion. This is a source-wiring control, not
execution of the complete app-update body or a mirrored host loop.

`event_fixture_schedule_controls.cmake` registers
`test_jce_editor_event_fixture_schedule` and the ordinary
`test_jce_editor_event_fixture_schedule_old_frame_no_start` and
`test_jce_editor_event_fixture_schedule_old_retained_admission` contract controls
from `test_jce_editor_event_fixture_schedule.cpp`. They consume the complete
actual main dispatch body and pre-commit guard, private scheduler, shared M/B/W/K
decoder and complete Game bridge pre-step commit. Counted typed clock and marker ports
forward to real host marker files and cover 60/600 Hz independence,
late/fresh/stale marker admission, docked-to-Max
waiting, equal/backwards readings, invalid clock configuration/frequency,
Stop/Pause/hidden/owner/runtime/geometry cancellation and deferred EOF held
state. The controls deliver final held DOWN followed by EOF, an invalid row or
an invalid event through one same-owner ordinary commit opportunity, then clear.
This is next-draw retention, not a runtime commit acknowledgement: zero real dt
may skip the host commit before draw clears the row. All three tails also
cover thirteen admission-loss combinations before that commit, after the data
buffer has been freed. Cancellation/drain retire retained admission; later ticks
perform no fixture clock work. Default frame mode retains its visible-Playing
counter and performs no clock reads or marker polling. Both ordinary old controls
must compile then fail unchanged behavioral assertions; CTest WILL_FAIL alone is
not a direct failure receipt. The host init/exit wires remain separate observations. These
CPU controls do not certify physical input, native event timing, GPU completion,
rendered motion or a performance gain.

The Game input replay fixture records a real JIRC through the public writer,
replays it through the actual input owner and syncs the complete Editor bridge
into the real Runtime/Lua host. Lua get_pointer_delta/is_pointer_down publishes
answers through public Scene transforms; the sentinel proves script execution.
Down/hold/Up, public pressed/released reconstruction, asymmetric display/render
scaling, repeated commit transient drain, EOF, native interference, geometry
cancellation and independent explicit virtual fixtures remain distinct.
Positive and ordinary old-replay-source cases have isolated host directories.
The old source mutates only the complete bridge's actual origin gate and must
compile then fail the same runtime assertions, not setup or linkage.
This is CPU script consumption, not actual mouse, same-frame camera poses,
rendered pixels, native replay acceptance or a throughput claim.

## LAN discovery client lifecycle

`lan_discovery_controls.cmake` registers `test_jce_editor_lan_discovery` and
ordinary `missing_update` / `missing_shutdown` controls. They compile the complete
actual private Editor service and complete Engine LAN transport. Typed ENet
socket/refcount and clock ports avoid real networking while actual packet
consumption, scan expiry, results retention/clear, lazy admission, explicit Stop,
Refresh replacement, create/bind failures and retry remain production behavior.
Strict unique complete CPU-maintenance and shared-panels-shutdown bodies supply
the real lifecycle wires; the controls omit only one wire and must compile then
fail unchanged ordinary doctest assertions. RAII cleanup directly retains the
actual service shutdown so negative controls cannot leak their fake socket.
Never-started shutdown and repeated Editor-session admission exercise the real
transport's inactive guards; the fixture does not execute complete Editor Init.
The existing host-services fixture separately checks one LAN destination and its
canonical nested service span in visible/hidden/minimized host polls, and no LAN
poll before initialization or in headless-build mode. These CPU controls do not
certify actual UI events, UDP broadcast/firewall behavior, network connectivity,
GPU completion or process-wide cleanup. Web reports this native fixture unavailable.

```bash
python scripts/jce.py test --configure --suite editor --variant debug --arch x64 --jobs 2 --target test_jce_editor_lan_discovery --target test_jce_editor_lan_discovery_missing_update --target test_jce_editor_lan_discovery_missing_shutdown --target test_jce_editor_host_services --target test_jce_editor_panel_state --filter "^test_jce_editor_(lan_discovery(_missing_update|_missing_shutdown)?|host_services|panel_state)$"
```

CTest WILL_FAIL is registration policy, not evidence that an ordinary negative
ran. Preserve direct executable failures and assertion output for both controls.

## Covered Console controls

The complete actual Console fixture observes a presentation gate after real
Clear-on-Play/last-play state and before any toolbar or output-child work.
Covered frames must have zero toolbar, child, row, text and text-metric calls,
retain derived row/cache snapshots while actual Core WARNs continue through
the existing host drain, and restore filter/collapse/history and real keyboard
navigation. Clear-on-Play still runs on the covered content visit, after host
drain, once per transition; the host poll itself never clears. The ordinary
`test_jce_editor_console_viewport_old_presentation_gate` removes only the complete
owner's admission return and must compile then fail unchanged assertions.

The shared panels TU remains complete. Only its unrelated About-dialog
`jce_pak_find` and `jce_pak_decompress` dependencies use exact public typed ports;
the existing null archive prevents consumption, and any unexpected call fails.
The Console fixture links real Core and ImGui without the unrelated Resource
shader-cook chain. It does not certify PAK/license loading or GPU shader assets.

The existing actual layout-route fixture also extracts the complete Console
Begin/content/End caller. It checks normal, Game/Scene-maximized, main/different
viewport IDs, hidden/collapsed windows and frozen same-frame maximize/restore
policy, retaining balanced Begin/End. No docking policy is reimplemented in
the fixture. Existing route/covered-window ordinary controls stay intact.
These CPU observations do not apportion an entire panel phase to Console,
certify native detached-window pixels or establish a whole-frame timing gain.

mouse_input_controls.cmake compiles the complete actual Editor event consumer
with genuine ImGui queue, NewFrame and SmallButton behavior. Only Game bridge,
capture/relative/button destinations and unrelated keyboard diagnostics are
typed ports; there is no copied mouse submission policy. The shared CPU context
retains default trickling and the fixture uses actual integer host coordinates.
Controls cover missing motion, an old different widget, same-pump short press,
release outside the active button, captured refusal then uncaptured recovery,
nonzero panel origin, unknown buttons and inactive/uninitialized admission.
The old_button_position complete-owner variant removes only the actual button
position queue call and must compile then fail unchanged ordinary assertions.
Preserve initial unchanged-owner RED, including any fixture correction, separately
from final RED/GREEN and direct mutant results. CPU widgets do not certify OS
input, native capture, multi-window backend support, rendered pixels or the
specific Max failure observed during a performance run.

`launch_diagnostics_controls.cmake` compiles actual private launch helpers,
overview/foreground/static-bake branches, complete cadence update and hierarchy
observation bodies, plus their actual init/exit/reset wires. Counted environment
ports cover absent/empty/zero/enabled values, thousands of calls, in-session
mutation and fresh lifecycle snapshots. The same-source `old_scans` control
restores the four repeated reads and must compile then fail ordinary unchanged
assertions. Typed initialization/SDK destinations do not certify native GUI
startup, whole-application cleanup or a frame-time improvement.

## Bounded trace-panel cache controls

The existing test_jce_profiler_trace_display fixture consumes the complete actual
panel cache/model/poll/Clear and UI bodies. Bounded synthetic public event packets
exercise valid thread/counter/begin churn directly through that consumer; existing
registration/loss/recording controls still use the real Core ring. The cache cases
pin all three named limits, repeated existing-key updates after capacity, saturated
refusal counts, matched/unknown/wrong-kind end accounting, owner transfer, prefix
gap qualification and explicit Clear after quiescent producer reset. Local refusal
must show a partial-view indicator and leave producer stats/loss unchanged. There
is no private Core generation seed or fake OS thread roster. The ordinary
old_unbounded complete-owner variant removes only the three actual admission
guards and must compile then fail unchanged cap assertions; CTest WILL_FAIL alone
is not a direct execution receipt. These CPU controls do not certify native UI
pixels, full heap/RSS bounds or a frame-performance gain.

The existing TraceDisplay control imports the complete production trace content
entry and all summary/GPU/counter/thread/work display functions, beside its
actual bounded model and passive poll. Narrow UI/GPU-stat ports count only
presentation; real Core events must still advance the covered cursor/history.
Covered, revealed and detached cases retain recording/live independence and
current data. Ordinary missing-covered-gate and missing-poll variants omit
unique actual entry wires; they must exit nonzero with ordinary assertions.
CPU controls do not certify GPU cost, native pixels or overall frame gains.

Scene content-clock controls import the actual Scene wall-clock block, exact
navigation/four content sink statements and actual Game delta expressions.
Typed CPU ports verify clocks and every forwarded call independently, including
STOPPED preview, pause/resume, accepted scaled deltas and first/revealed views.
Ordinary mutations restore wall content arguments, freeze navigation or remove
STOPPED preview. This bounded real-body routing fixture is not the complete
SceneRender function, renderer multi-view integration or graphical acceptance.
Existing single-step simulation does not publish a separate render delta; that
unchanged limitation is outside this routing repair.

The existing viewport-work fixture also clears/restores the independent DrawLog
selector. Actual-header groups cover missing/empty logger, legacy/invalid
selector fallback, natural real-ImGui focus/clip/layout refusal, independent
FrameLog/DrawLog precedence and Game capture dominance. Adding these assertions
before the production wire must yield an ordinary RED; unchanged test bytes
then qualify GREEN. No native window, CSV, draw stats or GPU query is created.

DrawLog availability controls compile the complete actual initialization and
row-writing blocks through six typed renderer/capture ports, using real Core
host filesystem writes and std::FILE readback. Controls distinguish unavailable
from available zero/positive draws, preserve all nine legacy columns before the
appended bit, and require exactly one stats getter per admitted row. Default-off,
missing shared renderer, limits, explicit screenshots and actual header-write
failure keep their existing behavior. Real readback bytes are retained in test
stdout. The same tests must yield ordinary OLD RED then GREEN; forced-zero,
forced-one and duplicate-getter controls must fail normally. These CPU/file
controls do not prove native renderer statistics, GPU work or full Editor input.

Game Stats coverage extends that same actual-callsite fixture with the complete
Stats HUD, current-frame extent stamp and CPU-tail completion. Counted public
ports cover requested versus committed dimensions, supersampling, hidden Stats,
viewport refusal, same-frame failed preparation, later-frame staleness and
resource/owner failure without enabling capture observation. The ordinary
missing-Stats-reset, missing-Stats-completion and requested-extent controls must
compile then fail the unchanged assertions; the unchanged-owner OLD regression
is retained separately. This is CPU metadata/UI formatting coverage, not GPU
completion, native appearance or a performance certificate.

project_run_profile_controls.cmake compiles the complete build-profile owner,
actual Run gatherer and shared native artifact-name/layout bodies with typed
configuration, project-store, metadata and existence ports. Fresh ordinary
profiles cannot inherit another project's launch/preset state; saved explicit
profiles and engine-workspace migration remain authoritative. Current-project
Run consumes manifest output_exe and shared host-native dist/release/debug
paths, after explicit overrides. Ordinary old-seed, missing-layout and ignored
manifest-name variants must compile then fail unchanged assertions. These CPU
ports do not perform actual project-state disk IO, launch a child, draw a UI
or certify relocation and target execution.
