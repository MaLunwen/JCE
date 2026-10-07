# Editor/runtime parity

Editor Play and a shipped game must use shared runtime, scene and input services rather than duplicate game logic. Trace each host's construction, per-frame submission and teardown through its public API.

Compare effective runtime descriptors, camera settings, input actions, script tick order, entity changes, resource mounts, navigation, save paths and streaming ownership. A matching struct definition does not prove both hosts populate it the same way.

Resource lookup may differ between loose project files, cooked directories and embedded PAKs. Test a relocated package without source-tree or loose-asset fallback before claiming embedded portability.

Match render backend and effective rendering settings before a pixel comparison. Verify that temporal passes execute and use the same clock; similar call traces do not guarantee equivalent images.

```bash
python tools/lint/check_runtime_desc_parity.py
python tools/lint/check_project_settings_consumed.py
python tools/lint/check_play_mode_isolation.py
```

Test repeated Play/Stop cycles and language backends in both hosts. Runtime shutdown must not invalidate authoring-scene selection or leak resources into the next simulation. Report the tested language/backend pairs, not merely that a scene opened.

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
