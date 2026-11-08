# Editor integration

Selected TriggerVolume outlines use the runtime registration coordinate rule.
Drag a Hierarchy row into an Entity-kind Script parameter to bind a full live
entity handle through the normal undo path. The viewport line represents an
authored reference. Event callbacks, overlap transitions and actions require
a Play or runtime witness. Use the Trigger Graph Lab consumer to exercise the
parented zone, remapped target and portable Lua callbacks.

Set `JCE_EDITOR_PROFILE_DIR` to an explicit absolute directory before startup
when validating an isolated editor instance or a portable configuration. The
shared resolver covers editor preferences, session and ImGui layout. Automated
preview tools must confirm that the child selected this directory; do not
snapshot/restore the user's HOME settings around a test. Use a fresh profile
for each independent trial or explicitly supply a seeded profile as an input.

Read editor/AGENTS.md and the closest panel/core charter. Search for an existing panel, shared widget and public SDK mechanism before adding another UI path.

Panel IDs are persisted: preserve existing enum values and append new IDs. Trace registration, dispatch, configuration serialization, menus, tabs and visibility; adding a switch case alone does not wire a panel.

UI text uses the editor i18n mechanism. Register component defaults, serialization and inspector/undo behavior through the existing owners. Do not add a separate component schema or bypass SDK boundaries.

Use display-only `jce_editor_i18n_or` for formatted text and tooltip payloads.
Reserve ID-bearing lookups for widgets that consume ImGui IDs; raw `##`/`###`
suffixes must not appear in user-visible caption or tooltip formatting. Preserve
existing translation keys and widget IDs when correcting the display boundary.

For input and Play changes, verify activation, focus, mouse capture/release, keyboard delivery, stop/restore and entity selection across repeated cycles. A temporary editor-input gesture must not accidentally drag dock panels or leave stale runtime handles in the inspector.

File viewing tests include activation on open, session restoration, tab selection and background audio/video suspension. Runtime resources and play state require their matching teardown paths.

```bash
python tools/lint/check_editor_consumer_purity.py
python tools/lint/check_inspector_undo_scope.py
python tools/lint/check_editor_consumption.py
```

Run the relevant interaction/runtime regressions and the required editor build. Split large files by responsibility instead of duplicating panel state.

Build Stop remains non-success even if the interrupted child exits zero. The
  existing manager latches cancellation before release clears stopping, retains
  the real exit code and an explicit cancellation diagnostic in the compatible
  FAILED status, and never advances queued tools or runs finish/publication.
  `editor.build.tool_completion` records the CPU completion transition, including
  next-spawn/finish failure and CANCELLED, not child execution time or physical
  cleanup. `test_jce_build_process_completion` consumes actual owner bodies, the
  real trace registry and no-draw host-maintenance dispatch with counted ports;
  its omitted-Stop wire must fail ordinary assertions after compilation. Keep
  native Stop/hidden polling, UI reachability and published artifacts as separate
  acceptance; public headless build execution does not exercise the tool-probe
  Recheck UI or native window gestures.

Covered panel content must retain required sampling and context/history invalidation,
while deferring derived presentation that has no consumer. The Profiler CPU tab
keeps rolling samples under the same-host Game overlay; it builds view rows only
when visible. Detached windows and Frame Debugger retain their refresh paths.
Threads and Tasks keeps its existing trace cursor polling under that overlay;
defer presentation without pausing or resetting the producer or capture.
Use actual owner CPU controls for work omission and lifecycle semantics; native
GUI behavior and whole-frame benefit need separate current-artifact evidence.
In File > Open Bundle, the path field accepts pasted files and directories.
Submitting a file opens it. A directory with a bundle catalog keeps the
catalog route. An ordinary directory seeds the native file picker, as does
Browse on a populated field. Verify the host modal separately from ImGui
input routing and archive loading.

## Product acceptance

Verify the selected target against the actual SDK/toolchain and linked artifact,
not the dropdown label or a predicted filename. Native host defaults must follow
the host architecture; unsupported cross targets fail before cooking or deleting
outputs. A parameter-admission unit test is not a cross-compilation certificate.

Build a current editor before certifying UI fixes. Historical executable
screenshots are baseline observations only. Exercise project open, asset import,
save/reopen, selection, component edits, Undo/Redo, Play/Pause/Stop, focus/capture,
build/export and relocated output. Attach asset keys, requested features,
backend/tier, actual program availability, outcome and artifact identity to
acceptance evidence. A menu item or textual API identifier is not execution.

Use shared runtime owners for fonts, foliage and shadows. Test first-use,
resource replacement, active-VFS switches and teardown. An authored missing
mask or shader must not silently become a successful unmasked/default render.
Record rejection reasons once per owner/state, preserve warnings/errors, and
avoid frame-by-frame repetition. Require actual pixels for graphical claims.

Build Profiles consumes the public toolchain discovery authority for both detection and execution. SDK-native builds require CMake/Ninja; Conan is an engine-workspace producer concern. Project target/exe defaults come from that project, and edited architecture/variant predict the real build artifact separately from the final dist/apps package. Fresh-profile renderer startup consumes filled platform defaults even when no saved preference file exists. Default editor typography uses system/common fonts; the user handwriting jce.ttf is an explicitly chosen project asset, not the default typography.

The current ordinary-project Build/Pack GUI uses the host-native platform and
offers `host`, `x86_64`, `i686`, `aarch64` and `armv7` architecture entries;
catalog entries still need host/SDK admission. Workspace CMake preset builds
are a separate route: a Web preset or CLI Web support does not establish an
ordinary-project Web target selector or accepted GUI Web delivery. Add target
UI as a request adapter to the shared `tools/build/jce.py` application owner,
its cook/native-registry and delivery proof, without copying a Web recipe into
Editor. Verify actual SDK target metadata and compiler prerequisites; an absent
bundled builder/compiler/sysroot remains an explicit delivery gap, not proof of
zero-environment construction. A linked HTML alone is incomplete without its
actual registry execution proof.

Project Open restores the existing per-project build/run overlay. Ordinary
project Build derives target/executable from its manifest, while an explicitly
configured external Run path/target can retain priority in the Run resolver.
Test Project Open, Build and external launch with actual artifact identity
before classifying a stale saved field as an executed behavior defect; Editor
simulation is a separate host. Preserve authored project data during generic
fixes and make profile provenance explicit. Fresh ordinary-project overlays
start with neutral external paths/presets and that manifest's target/root;
existing saved overrides and engine-workspace migration retain their values.
External Run's derived fallback consumes manifest `output_exe` and the shared
project build layout, while an explicit saved external override keeps priority.
CPU owner controls do not replace disk persistence, GUI or actual process tests.

Font preference policy: explicit paths win; automatic Latin prefers installed Ink Free, then the host system default, licensed stock Latin and the built-in fallback. CJK resolves its own covered system face. For the editor's Simplified Chinese locale on Windows and macOS, automatic CJK first tries the installed KaiTi family before the shared CJK role. Explicit choices win, and unavailable or refused candidates fall through without rereading an identical rejected path. Default Canvas and document UI also use an installed Symbols fallback through the shared host-font role; ordinary glyph coverage and shaping choose the actual leaf. Keep explicit project/component paths and fallback lists authoritative. A missing/nonfile explicit path emits a reasoned WARN per reload; explicit source-read/size or atlas-admission refusal retries its shared role before compatibility/stock fallbacks and never rereads the same rejected resolved path. Atlas admission is separate from actual glyph parsing, rasterization and pixels. Missing local faces preserve the remaining fallback chain; this is not universal Unicode coverage. Preferences uses the shared searchable font picker and applies choices through the existing deferred reload before NewFrame. Preserve and display custom/unlisted explicit paths; never show them as Auto. System fonts are discovered locally, not redistributed.

Searches over editor i18n labels use the shared key-aware matcher. Match the label from any installed locale, a partial English fallback, the key leaf or a qualified key reaching its leaf. The common namespace alone does not select every entry. Reuse the compact locale tables built during discovery and cache aggregate result lists when a query is stable. Search ASCII without case sensitivity and compare other UTF-8 bytes literally. Keep authored project text, asset paths and runtime diagnostics in their existing search domains. Verify language changes and clearing the query in the real panel.

Viewport chrome must claim pointer input through the existing UI ownership
mechanism before scene picking, selection, painting or new gizmo operations.
Share drawing/hit geometry and preserve already-started gesture release.
Verify actual UI controls and continuous/release behavior with fresh captures;
real-context CPU controls do not certify OS input delivery. Explicitly requested
diagnostics must survive DIST log macro removal while preserving logger policy.

Scene View navigation registration must leave layout advancement to the final real viewport item. Do not restore an unsubmitted post-Image cursor past CursorMaxPos. Context regressions include the Image, End and Render sequence and require zero ImGui errors; graphical verification remains separate.

Latch maximized viewport ownership before drawing dock content, and use that
same frame state for the overlay. Toolbar changes take effect on the next
frame. Keep dock Begin/End membership while suppressing covered Scene/Game
render work; Scene suppression still flushes pending CPU results and releases
stale interactions. Verify both activation and restoration, including exactly
one admitted Game content call and no covered Scene rendering. Counted source
controls establish routing; a current graphical host and trace are separate
evidence for actual viewport consumption and cost.

## Hotkey capture

Rebinding uses `editor/src/core/jce_hotkeys.cpp` and its
`jce_hotkeys_capture_begin/id/poll/cancel` interfaces. Keep capture state in the
existing registry owner; panels consume it instead of scanning keys or adding
another global. Every action query must stop during capture and throughout the
frame capture commits or cancels, regardless of query order. Preferences
cancels on tab-away, close/hidden state and root-and-child focus loss. Preserve
the existing red conflict warning and first-query consumption policy.

Use the compiled registry with real ImGui events for capture, modifier/Esc,
held-key, fresh-edge and session-reset controls. These CPU controls do not
prove native OS input, actual modal/tab/focus consumption, saved bindings or
pixels. Verify those using the rebuilt editor and an isolated profile.

Play presentation changes belong to the existing Play host and the private
`editor/src/core/jce_editor_play_presentation.h` helper. Apply the shared effective
render settings before runtime creation; restore the captured editor VSync and
target-FPS values on create failure and Stop. Preferences stores
`editor.frame_rate.play`: default 60, 0 means no additional Editor cap. Positive
preview and authored project limits compose by their minimum; a project FPS of
0 uses the positive preview limit, and preview 0 retains the project value.
Preserve authored VSync and project values. Missing composition still starts
the same FPS transaction while retaining entry VSync. NULL/fallback renderers do
not call bgfx. Actual wiring controls cover apply, live preview rates, missing
composition and both restore paths; device frame pacing requires a current
artifact and independent runtime evidence.

```bash
python scripts/jce.py test --suite editor --filter "^test_jce_editor_(play_presentation|cadence(_old_apply|_old_shared_background|_missing_preview_cap)?)$"
python -B -m unittest tools.tests.test_editor_play_presentation -v
```

Every Play request waits for actual Game View layout before runtime creation.
The existing Game render owner resolves the requested extent through
`render_scale`; initial `JceRuntimeCreateOptions`, later viewport publication,
Canvas, pointer mapping and final FBO use those same render pixels. Initial
extent is available to script `on_start`, without waiting for a hover/click.
Changed extent invokes optional `on_viewport_resize`; read
`jce.get_viewport_size()` to relayout rather than adding a simulation tick or
fabricating an input event. Invalid or unavailable layout leaves Play pending.
Keep actual layout/resize controls separate from GPU pixels and OS interaction.

```bash
python scripts/jce.py test --suite editor --filter "^test_jce_editor_play_viewport(_old_begin|_old_extent)?$"
```

The default Editor PAK excludes unused user-authored fonts/JCE.ttf via the existing packer filter; preserve its source bytes and explicit user project choices. Verify actual BOM membership after build.

Audited CPU services use the existing Editor should_quit callback before render admission, gated by successful initialization and disabled before exit. TestRunner/Build/Run/index/serial/config-state polls execute once per host iteration, with Run retaining ownership of a pending Build poll. Layout/draw does not duplicate them; hidden UI refreshes the existing clock. Scene/dialog/Automation/AI/quality/font/gameplay/video work stays with its visible owner. Source/native CPU child controls do not prove actual OS minimization/restore or all-panel GUI reachability.

Project-state timed/forced publication failures preserve dirty values for the existing retry pump. Validate with actual destination blocking, unchanged prior JSON and recovery without another setter. Outgoing-project switch failure retention remains a separate gap; successful atomic publication does not certify every filesystem or power-loss durability.

Paused host polls reuse the existing Engine elapsed-limit owner after input/yield/capture, passing completed_iteration=false. They do not count toward MAX_FRAMES or run generic simulation/render work. Exact-body CPU controls remain distinct from actual OS minimization. For an ordinary timed window receipt fully unset JCE_MAX_FRAMES; even an empty variable currently selects the resize-watcher benchmark exception.

Isolated RenderSettings must validate the actual active ISOLATED VFS and consume its public file bytes through the existing JSON-memory parser. Refused admission must not retry a host CWD file; a valid VFS needs no legacy convenience-reader callback. Keep HOST source/cooked fallback and quality overrides. Distinguish file, quality-only and defaults once per context in DIST diagnostics. The one MiB caller-copy limit does not bound eager PAK decompression during open. Test real PAK content, conflicting CWDs, actual active-FS replacement, missing/bad content and the ordinary old-host dispatch mutation; these are CPU content evidence, not GUI/pixel proof.

Immediately before NewFrame, the existing Editor update synchronizes public window flags through the stateless viewport-work helper into ImGui AddFocusEvent. Pinned AppFocusLost is cleared by EndFrame, so it cannot cache persistent native focus; continuous background frames must reobserve it. A failed window query does not invent focus, and explicit positive-layout captures retain their gate. Test the exact production preframe wire with real ImGui queues plus ordinary missing-sync and old-visibility mutations. Actual OS interaction and measured GPU/presentation remain separate.

Stopped authoring defaults to 60 Hz active, 30 Hz idle and 5 Hz in the background;
idle begins after two seconds without input.
unfocused Play retains its 15 Hz background default. Both background modes
consume the same optional saved setting, and valid explicit settings override
both. Keep the actual Play call routed through its preview and background rates.
Temporary focus/pause caps restore the composed running FPS, including live
preview changes made while temporarily capped. Stable rates must not invoke the
target setter each frame and rearm its deadline. Preserve authored VSync, project
FPS intent and the existing fixed-timestep/capture protocols. Idle, Pause and
background restrictions compose with the minimum effective positive active or
project/preview cap, including initial begin and manual Pause-to-background
transitions. Zero adds no restriction and cannot erase another positive cap;
all-zero remains uncapped. Recompute from raw preferences and the composed
running target, never from a temporary target. Generic Runtime policy and saved
preferences retain their ownership. Native presentation and GPU costs require
separate measurements.

ImGui font source transfer invokes an owning AddFont operation. Its pinned parser rollback already frees rejected owned input; callers must not free it again on NULL. Accepted dynamic sources stay alive until atlas Clear/destruction. Keep ordinary and merge parser-refusal tests separate from glyph/pixel acceptance. Requested benchmark markers use common runtime logging so DIST retains actual requested/spawned counts without introducing per-frame logs.

Project switch admission force-publishes outgoing editor overlays before a different root or close; refusal retains the outgoing project and dirty values. Same-project and preserved-root scene loads do not require unrelated writes. Use the shared project-context path identity policy (Windows ignores case, POSIX preserves case). Scene read/JSON/schema admission precedes full project/asset rebinding; keep these transaction controls separate from full ECS/stream rollback and GUI acceptance.

Ordinary project field or Bundle-list saves preserve unknown nested manifest
nodes through the shared project owner. Existing unreadable, malformed or
duplicate-root-key manifests refuse overwrite. Editor cache updates commit only
after successful atomic publication; unrelated borrowed Project/root/string
pointers remain valid. Load/new allocation failures must release constructed
prefixes and return failure, never a partially valid project ready to save.
Keep real filesystem, owner allocation controls and physical Save SDK/Build
panel acceptance separate; preserving JSON nodes is not byte-identical editing.

Before an Editor field/Bundle save, compare the current decoded known source
semantics with the loaded baseline. Refuse changed or unverifiable known values
with a reload diagnostic; unknown extensions and formatting do not conflict.
Refresh the baseline after publication independently of cached cleared values.
This preflight does not lock the manifest or eliminate concurrent check/save
races. Keep the public loader's filtering/defaults authoritative.

Preferences may save a GPU vendor request (Auto, Intel, NVIDIA or AMD) for the next launch through the existing UI-int setting. An absent setting preserves the environment/driver policy; explicitly saved Auto overrides the environment with automatic selection. Invalid saved values warn and preserve fallback. These are vendor requests, not physical-device IDs or universal power tiers; verify the renderer actual-device log and workload before comparing GPU or memory costs.

Reject font sources of at most 100 bytes before the pinned ImGui input-size assertion and before ownership transfer. Only the owning AddFont call uses local recoverable user-error handling; restore the previous IO setting immediately afterward and retain internal assertions. Keep tiny-source ownership, real parser refusal and fallback diagnostics distinct from claims about arbitrary malformed font-table safety.

Preferences preserves the last category through the existing session UI-int key preferences.active_tab (Appearance=1); missing or invalid IDs fall back to General. The existing panel restore and official JCE_SHOT_FRAME/JCE_SHOT_PATH full-backbuffer capture can exercise actual application layout without OS input injection. Require a completed, decoded, identified PNG; this route does not prove menu/hotkey or physical input delivery.

## Profiler observations and memory reports

Profiler consumes the existing backend stats through the private panel observation
owner. Keep the backend CPU interval, API-thread render wait, render-thread
submission wait and latched GPU duration separate. Cross-thread waits must not be
summed or subtracted to invent CPU work. These observations have no exact engine
frame join; the panel reports submission wait, backend wait or undetermined rather
than CPU/GPU bottlenecks. A submission wait does not establish planned FPS pacing.
Missing frequencies or timestamps display N/A and remain excluded from averages;
an available zero duration is distinct from missing data. Keep the scope caption
and clipboard availability fields with exported observations.

Memory Profiler uses available current-process RSS for its 512 MiB target bar.
This is an application RSS target, not device capacity or a hardware acceptance
threshold. Process RSS, private commit, allocator counters and voluntary tagged
allocations remain separate. Missing allocator counters show N/A. No tagged rows
or zero tagged bytes do not establish zero process usage or complete allocation
coverage; retain the explicit partial-scope caption.

Snapshot refreshes the observation and publishes a complete
`jce.editor.memory_snapshot.v2` JSON through the existing profile resolver under
`reports/memory/`. It honors a valid absolute `JCE_EDITOR_PROFILE_DIR` and never
falls back to the caller's CWD for an invalid explicit profile. Keep unavailable
process/allocator counters as JSON null with availability flags, all captured tag
rows, exact unsigned byte values and the RSS-only budget scope. Bounded encoding
failure must not publish a truncated prefix. Use the public atomic host-write
primitive and report the resulting path or failure; preserve earlier valid
reports. `complete: true` means complete serialization/publication, not a
simultaneous OS memory sample or power-loss durability guarantee.

The CPU owner controls cover interpretation, real profile resolution and file
publication, with ordinary assertion-failing mutations for old wait subtraction,
tag-based budgets, CWD paths, non-atomic writes and truncated publication:

```bash
python scripts/jce.py test --suite editor --filter "^test_jce_(profiler_observation(_old_wait_math)?|memory_snapshot(_old_tag_budget|_old_profile_path|_old_nonatomic|_old_truncate)?)$"
```

Separately inspect the rebuilt Editor with an isolated profile: verify waiting or
undetermined labels, missing timers, process/tag scope captions, snapshot status
and the actual decoded report. Check the translated UI in the selected locale.
CPU controls do not prove physical UI reachability, GPU completion or full
allocator coverage.

## Scene restore and Canvas ordering

Scene owns authored entity order independently of ECS handle generations.
`jce_scene_order.c` and its private header share it with canonical full/prefab
save and Canvas layout, painter and hit order. Save emits authored array order;
existing flat/streaming loaders and Editor History recreate/remap that array.
Component migration/reparenting keeps the ordinal. Explicit Canvas priority
wins; equal-priority ties keep authored order. Dead/unmanaged entities and
changed rosters are refused, without handle fallback or clearing all caches.
No new public ABI or JSON field is required. Nonzero stale subtree roots are
refused; root zero retains the historical empty envelope.

The real-ECS, persistent headless Canvas control consumes seven groups with
six internal restore cycles, including canonical/stream/prefab paths and wide
heap-backed snapshots. Complete old-handle and unordered-save owners must
compile and fail ordinary assertions. On MSVC these complete-owner test TUs
need the same private `/Zc:preprocessor` option as other bx consumers.

```bash
python -B scripts/jce.py test --suite engine --target test_jce_ui_restore_order --target test_jce_ui_restore_order_old_handle_order --target test_jce_ui_restore_order_old_unordered_save --filter "^test_jce_ui_restore_order(_old_handle_order|_old_unordered_save)?$"
```

CPU layout/hit/save evidence does not prove physical repeated-Play pixels,
font/image upload, GPU completion or performance. Inspect the actual component
and emitter first: apparent text can be UIImage sprites rather than glyphs.

## Authoring workload measurement

Stopped active60/idle30/unfocused5Hz defaults are frame caps, with idle after
2s without input and saved overrides respected. Visible, focused Scene and Game
viewports render independently on each admitted UI frame even outside Play;
this is not dirty-only rendering. Background dock tabs whose ImGui Begin returns
false do not run viewport content. Normal unfocused frames reject viewport 3D
through AppFocusLost while UI submission can continue at the background cap.
Minimized, OS-paused or zero-drawable iterations skip the complete render path;
CPU process/build/test maintenance, input and trace polling remain separate.

Nonempty `JCE_KPI_FRAME_LOG` or `JCE_KPI_DRAW_LOG`, and explicit Game captures,
request viewport work despite clipping/focus loss. They therefore change the
normal background workload. Keep those capture runs separate from ordinary
idle/active/background/minimized comparisons. `JCE_EDITOR_CADENCE_DIAG=1`
reports bounded mode/rate/flags/VSync transitions without that viewport override.
Stable sizes reuse offscreen targets; real panel/dynamic-resolution changes
create and retire them. Offscreen serial/size/lifecycle and central accepted
submission events can identify this CPU workload; current resize handling has
no standalone canonical backbuffer-reset counter. Provider timing/resource
snapshots are latched observations, not presentation or GPU-completion fences.

Use a rebuilt, identified artifact and isolated profile for two actual
Play/Stop cycles, verifying both UIImage sprites and real text plus hit order.
Then consume Build Profiles/Recheck/Report/Stop and hidden polling, Input Manager
binding edit/save, Fonts with actual Ink Free/fallback, Asset/Bundle previews,
and authored shadow/grass routes. Menu visibility or CPU owner passes alone
cannot certify those physical operations. Keep other rendering apps closed for
matched performance sampling, bind process birth/adapter LUID, and retain DWM
identity/query availability rather than inferring a compositor cause.
