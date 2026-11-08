# Tower Defense SDK consumer

Use public JCE SDK APIs only. Shared C scene scripts own gameplay in both
Editor Play and the standalone application. Project assets, balance,
reference acquisition, evidence and packaging tools belong here.
Preserve upstream source bytes in the ignored reference workspace.
Build through scripts/jce.py cook and app with an explicit installed SDK. Generated native modules,
cooked assets, captures and reports are ignored output.

The maintained file map is README.md. Do not claim reference or editor parity
without actual state traces and inspected captures from all applicable hosts.

The authored scene contains 104 static/UI entities. Game actors use public prefab spawn lazily during update and reuse prior slots; never author maximum-capacity actor arrays into the scene. A selection change while the build menu remains open updates the selected cell without restarting its entrance animation.

HUD dirty refresh builds one TD-owned desired presentation before publishing.
Its fixed 96-slot snapshots keep successful UIImage, UIText and UIButton
publication independent. Publish at most one patch per changed component;
unchanged slots skip formatting and callbacks. Do not first move every live
slot offscreen. Unused tail geometry/actions are retired in the final publish.
Only accepted bool callbacks commit their fields; refused components retry on
the next refresh. Void text/action/enabled callbacks have no success witness.
Unchanged authored image paint is not republished for unrelated value changes
or geometry-only viewport reflow, preserving runtime hover/pressed tint.
Menu role changes and reappearing slots publish their authored paint again.
This is bounded consumer presentation state, not a second engine UI/JSON cache.
The actual td_view_test host callbacks check final geometry, sprites, feedback,
hit/actions, initial/menu/resize/cards transitions, no redundant publication and
separate component-refusal retry. These CPU assertions do not certify native or
Web pixels, editor interaction, GPU completion or performance improvement.

td_view_ui returns the conjunction of actual requested bool host writes; unused
paint/feedback fields need not have valid cache bits. The controller's start,
ordinary update and viewport-message paths preserve ui_dirty on refusal, so a
static menu retries on its next ordinary update without new input or simulation
time. Successful unrelated component publications remain cached. The clock
fixture's headless presentation port returns true and retains its separate
runtime/VM clock scope. td_controller_ui_retry consumes the complete controller
and actual view through direct lifecycle callbacks and counted public host
ports; game transition, viewport and initial-publication cases verify rejection,
next-update retry of only the failed component, then zero redundant requests.
Its whole-controller old_clear control restores all three unconditional dirty
clears and must fail ordinary assertions. These callbacks do not certify native
VM dispatch, graphical execution or thread scheduling.



HUD geometry uses measured original CSS at canonical DPR=1 (5px bevel, 41.421875px rows); earlier headed-browser fractional-DPR measurements are retained only as diagnostic evidence, not the portable layout authority.


Finishing a left-button camera drag hides the active build selection, matching the pinned original canvas input. It must not leave tower cards open or turn a drag into a tile-selection action. Verify this with an existing selected tile, not only an initially empty selection.

Single-finger taps select world tiles through the same view picker as mouse clicks; UI hits belong to Canvas. Touch gestures consume only real touch input and must not reapply synthetic mouse deltas. Multi-finger gestures never fall through to a tile tap on release.

Normal gameplay accumulates the runtime-supplied simulation dt at 60 Hz for movement, shooting and waves, independent of render FPS. Never resample wall time to replace dt: runtime pause, zero time scale, slow motion and host suspension must remain authoritative. At most 0.25 seconds / 15 simulation steps are accepted per update; excess supplied simulation time is reported and never queued later. Menus do not consume simulation dt. Legacy per-render-frame wall behavior is explicit diagnostic policy only; frozen reference fixtures have their own bounded clock and hold on zero simulation dt. TD_TIMING only observes; TD_TIMING_START explicitly starts the single-enemy clock control. Keep actual runtime/native C callback checks for pause, time scale, host resume, 30/60/120/144 Hz state-equivalence and hitches, with an ordinary-failing old-wall control.

Bundle acceptance uses the installed SDK packer and its explicit stock shader root.
TdController native authoring context is required for Editor Play; a content Bundle
is not independent native machine code. Do not copy DLL kits into published final
products or treat contextual Play as a stock-host portable gameplay proof.

Dynamic script asset dependencies include every authored UI sprite, imported
texture and license notice, in addition to lazy prefabs. Runtime string names
must not depend on accidental source-file fallback. UI authoring defaults to
Pillow's bundled Aileron font, with recorded hash/terms; restricted original-font
glyphs stay in ignored reference evidence. Typography differences are explicit.

Responsive layout consumes the public ScriptHost viewport_size service in
on_start, ordinary update and the optional on_viewport_resize message. Canvas
reference dimensions, HUD/menu placement and world picking use that independent
presentation extent. Pointer availability never supplies the layout size; zero
pointer dimensions still release borrowed input and cannot create mouse actions.
Initial menu layout is published before its first Canvas draw. Resize reflow
updates presentation only and does not tick gameplay or entrance animation.
After a viewport message actually changes the extent and applies camera/UI
reflow, emit the existing TD_TRACE-gated camera trace once. Unchanged or invalid
extents emit no new camera trace. Resize-only browser acceptance must observe
that message path without an extra drag, wheel event or simulation tick; retain
the original input assertion rather than accepting CSS/backing size alone.

## Same-window Web work census

`src/qa_update_census.inc.h` is consumer QA, not an Engine/VM service. Only
`TD_UPDATE_CENSUS=1` enables admitted actual `TdController.on_update` entry and
its final normal `JCE_C_OK` return. Invalid self/host is rejected before credit;
exception/unwind cannot reach the return marker. Rules may intentionally remain
frozen in the initial fixture. Disabled probing still has conditional overhead.
The static eight-uint32 record is version, instance generation, alive, enabled,
entered, normal-returned, saturated, invalidated. Instance creation changes the
generation; destruction clears alive; counters/generation never wrap silently.
A second instance or invalid host invalidates the witness, without adding a VM.

Web builds use public Emscripten `EM_JS`/`EMSCRIPTEN_KEEPALIVE` to publish
`_td_qa_update_census_snapshot` and `Module.jceQaUpdateCensusRead(Uint32Array(8))`.
This explicitly publishes only the TD-owned fixed record, never private Engine
addresses. The reader fetches current `HEAPU32` after memory growth and rejects
wrong types/length, dead/zero generation, saturation and invalidation. The binding
refuses another owner of the same Module name; the record outlives each instance.
The native-only dedicated census selection reuses the whole actual controller,
public runtime and native C VM. The original 13 clock cases remain separate.

`verify_web_performance.js` keeps publication-v2 as raw callback interval/CPU
observation. Explicit `v3-work-consumed` adds seven off/off/on/on source/JCE
repeats on the same per-host artifact and workload. On rows require one actual
source `TowerDefense.counter` update or one TD entry/normal return per sampled
callback. Source rows do not claim TD instance/generation fields. World calls
must use actual GL program objects identified during setup; JCE fragment hashes
must match separate same-artifact linked ground-quality evidence. A 36-index
count alone is insufficient. Relink/deletion and missing/foreign world draws
refuse. Sampling performs no GL query or readback.

Keep both off and on raw metrics, all spikes and failed partial records. Census
reads and witness construction are outside the callback CPU span; retained GL
wrappers/counter increments are inside it, and RAF intervals include observer
bookkeeping. Off/on deltas are workload observations, not an exact isolated cost
or a correction to subtract. Neither protocol certifies CPU submission, GPU
completion, uncapped throughput, pixels or physical-device industrial parity.
`web_performance_validation.py` recomputes v3 window/census/program and pair
admission. Native actual-owner controls, EM_JS/observer CPU ports, and real Web
compilation/browser evidence remain separate. Missing evidence is unavailable.
