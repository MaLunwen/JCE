# editor/ — JCE Editor (Consumer, L7)

> Sub-memory for the editor. See repo-root `AGENTS.md` for global rules.

## Identity

- **Layer**: L7 (Consumer). The editor is a *consumer* of the engine SDK, not part of the core engine.
- **Language**: **C++20**. ImGui (and ImGuizmo-style drawing) is reachable ONLY through the `jce_tools_imgui` INTERFACE target (`engine/tools_include/`).
- **Entry point**: `editor/src/jce_editor_main.cpp`.
- **Output**: `jce_editor.exe` (or platform equivalent).

## Directory map

```
editor/src/
├── core/         Editor lifecycle, history, hotkeys, i18n, state, presets, reflection, config
├── dialogs/      Modal/popup dialogs (asset picker, build, project, preferences, ...)
├── game_modules/ Editor-side game module demos (e.g. jce_fps_demo_module.cpp)
├── gizmo/        Translate/rotate/scale gizmo (own implementation, no ImGuizmo)
├── io/           Scene/prefab parse + serialize for editor
├── panels/       Every dockable panel — file naming `jce_panel_<thing>.cpp`
├── scene/        Editor-side scene asset cache, render glue, model loader (assimp for import only)
├── ui/           Editor chrome: layout, panel registry, style, theme palette
├── viewers/      Asset preview/inspector viewers (image, audio, video, model, code, hex, material)
├── widgets/      Reusable ImGui widgets (timeline, ...)
└── jce_editor_main.cpp
```

## Hard rules (editor-specific)

1. **NO `#include <SDL3/...>` or `#include <bgfx/...>` in panel/dialog/widget code.** Use `<jce/...>` public APIs.
2. **ImGui is editor-only.** Never appear in any `engine/src/...` outside `middleware/ui/jce_imgui_renderer.cpp` (which is the runtime backend the editor itself uses, gated behind the tools include).
3. **Panel registration flow**:
   - Implement panel in `editor/src/panels/jce_panel_<x>.cpp` + matching `.h` (or use the shared `_internal.h` pattern when state crosses several TUs, like hierarchy/assets).
   - Register in `editor/src/ui/jce_editor_panels.cpp`.
   - Dock default position in `editor/src/ui/jce_editor_layout.cpp`.
4. **Hotkeys**: declare/handle in `editor/src/core/jce_hotkeys.cpp/h`.
5. **i18n**: every user-visible string goes through `editor/src/core/jce_editor_i18n.cpp` (no raw English literals in UI).
6. **Theme / colors**: only via `jce_editor_style.cpp` and `jce_theme_palette.h` — no per-panel hard-coded colors.
7. **Reflection**: editor uses its own runtime reflection (`jce_reflect.cpp`, `jce_reflect_builtin.cpp`) for inspector drawing. Add new component reflection there, not in engine code.
8. **History / undo**: every destructive scene op goes through `jce_editor_history.cpp`.
9. **Memory**: editor-side allocations via `jce_editor_alloc.h` (which routes to engine `jce_alloc` / mimalloc).
10. **Assimp** is *import-only* (`jce_model_loader_assimp.h`). Runtime mesh loading still goes through engine glTF loader; assimp is for editor's "Import…" path.
11. **Math types**: prefer `jce_math` (`jce_vec3`, `jce_mat4`); `jce_editor_quat.h` is a thin convenience wrapper for editor-internal quaternion ops.
12. **Play mode**: routed through `jce_editor_play.cpp` + `jce_run_manager.cpp` — do NOT spawn child processes ad hoc.

## Adding a new panel — checklist

- [ ] Create `editor/src/panels/jce_panel_<name>.cpp` (+ optional `_internal.h` for >2000-line panels).
- [ ] Implement a `void DrawPanel<Name>()` (or class) that uses ONLY `<jce/...>` and `<imgui.h>` (via tools_include).
- [ ] Add to `jce_editor_panels.cpp` registry.
- [ ] Add default dock target in `jce_editor_layout.cpp`.
- [ ] Add menu entry / hotkey via `jce_hotkeys.cpp`.
- [ ] Localize every label through `jce_editor_i18n.cpp`.
- [ ] Wire any new state into `jce_editor_state.cpp` if it crosses panels.

## Common tasks

| Task | Where |
|------|-------|
| Add inspector drawer for a component | `editor/src/panels/jce_panel_inspector.cpp` + `core/jce_reflect_builtin.cpp` |
| Add a file-viewer type | `editor/src/viewers/jce_fv_<kind>.cpp` and register in `jce_file_viewer.h` |
| Add a build target preset | `editor/src/panels/jce_panel_build_profiles.cpp` + `core/jce_build_manager.cpp` |
| Add an asset import preset | `editor/src/panels/jce_panel_import_presets.cpp` + `core/jce_editor_presets.cpp` |
| Add a dialog | `editor/src/dialogs/jce_dialog_<x>.cpp` + register in `jce_editor_dialogs.h` |

## Don't

- Don't introduce a new editor UI framework — ImGui only.
- Don't talk to bgfx/SDL/flecs directly from panels.
- Don't write game logic into the editor — gameplay lives in `examples/caged_kingdom/` or pluggable game modules.
- Don't make panels >3000 lines — split with `_internal.h` like `panels/jce_panel_assets_*.cpp`.
- Don't hard-code OS paths — use `jce_editor_path_util.cpp` and engine `jce_path` / PhysFS.

## Native standalone distribution

JCE_EDITOR_STANDALONE uses the ordinary editor sources and embedded editor PAK.
It embeds LICENSE and THIRD_PARTY_LICENSES.md as native RCDATA resources.
It links Lua/C/C++/JavaScript explicitly, requires a complete static-CRT graph
and stages no runtime files. Python/Java/C# remain in the full language build.
No editor capability may be removed just to hide a dependency. Packaging and
PE validation belong to tools/build/standalone.py, not editor business code.


Runtime output paths contain the executable. Link/compiler debug symbols stay
under the existing editor build cache's symbols/<configuration> directory.
DIST hash receipts stay under build reports/artifacts/<variant>, independent
of runtime relocation. None is required beside the single editor executable.

Explicit `JCE_KPI_GAME_EVENTS` QA rows are six columns: Play-frame, M/B/W/K,
x, y, a, b. K routes ordinary editor key events (`a` scancode, `b` down 0/1),
including F12; x/y are ignored. Invalid rows log ERROR and are not dispatched.
The stateless decoder is in core/jce_editor_event_fixture.h; it never changes
viewport focus, application input ownership or OS keyboard state.
The host-owned core/jce_editor_event_fixture_schedule.h snapshots the event
path, `JCE_KPI_GAME_EVENTS_CLOCK` and `JCE_KPI_GAME_EVENTS_START` once at GUI
initialization and releases its state at host exit. Absent/empty/`frame` clock
retains the visible-Playing frame counter without clock reads or marker polls.
Explicit `elapsed_ms` interprets the same first integer as elapsed monotonic
milliseconds; an optional initially absent absolute regular-file start marker
delays activation until admitted Play observes it. Configuration errors refuse
only this synthetic fixture. See the evidence reference for marker freshness,
admission cancellation and the CPU-only verification limits.

Normal GUI host exit first calls the existing idempotent recorder Stop while
capture, master-audio tap, encoder worker and borrowed view/UI owners are live.
This precedes Play Stop; headless/invalid-startup routes borrow no GUI recorder.
Normal host exit reuses jce_state_stop for active or paused Play before releasing
Game View, Scene View and editor owners. The private Play transaction helper only
orders existing callbacks; it owns no second runtime or play state. Ordinary Edit
exit skips simulation Stop. Require actual script teardown evidence in addition
to the headless sequencing controls.

Editor build asset readers (`CookEditorAssets` and `PackEditorAssets`) depend
on the i18n normalization writer individually. A prerequisite only on the final
editor/packer does not order the cooker; concurrent scans can race source JSON
rewrites. Source graph controls live in tools/tests/test_i18n_pack_order.py.

The existing `should_quit` callback owns one audited Editor CPU-service poll per
host iterate before headless/minimized/render admission. `cpu_services_ready`
starts only after successful app initialization and clears before app exit. It
does not dispatch generic gameplay/update/PlayerLoop callbacks. Headless mode
retains Build + headless-driver completion only; GUI service membership is
shared in `core/jce_editor_host_services.h`. Real child/actual-body controls are
`test_jce_editor_host_services` and its ordinary missing-wire contract control.
Actual native minimize/restore, OS suspend and GPU suppression still require
windowed acceptance; MAX_FRAMES benchmark mode is a distinct exception.

The actual editor_app_event callback routes public QUIT and WINDOW_CLOSE through
that same layout quit-confirmation owner. Headless events return before cadence,
confirmation or input forwarding; GUI events preserve cadence-before-forwarding
and each unconfirmed close event requests the existing idempotent layout action.
The complete callback is consumed by the host-services CPU fixture; its ordinary
old_quit_only control removes only WINDOW_CLOSE admission. Native OS close,
unsaved modal behavior and terminal cleanup require current graphical evidence.

The audited CPU service poll and its selected members emit bounded canonical
static-name spans only when existing trace capture is enabled. These scopes
cover CPU polling, not external tool success or GPU completion. The normal
host-services fixture retains its seven behavior groups plus trace-on parent
closure and trace-off controls; its missing-wire mutant stays a runtime failure.

Editor scene/video simulation consumes public scene_video_advance, while admitted
SceneRenderer preparation owns its upload. editor_app_update discards a zero
engine resume interval before its separate fixed-dt Play override can repay
suspension time; the pre-render/should_quit CPU maintenance owner remains in its
original position. Do not move the whole editor update or ImGui/draw work into
maintenance. Play host policy publishes effective background CPU permission
through the common Engine API, including manual/OS pause and conditional restore.
Physical minimize/restore, authored background intent, child process maintenance
and presentation still need rebuilt GUI evidence. Foreground auxiliary engine
hooks remain independent of runtime pause; do not claim this phase fixes that
separate consistency boundary.


Recorder format-failure retirement belongs only to the initialized pre-render
CPU-maintenance service in core/jce_editor.cpp. editor_app_update has no second
Recorder-update wire. This preserves one poll per ordinary host iteration and
allows rejection retirement while UI admission is skipped. The actual-body
fixture executes that maintenance call; a separate full-main source-wire count
and ordinary restored-line control prevent a duplicate direct app-update call.
That wire count does not execute editor_app_update or certify native scheduling.

The main `EditorState` snapshots only the launch diagnostic selector
`JCE_EDITOR_CADENCE_DIAG` at host initialization, using `jce_env_flag_enabled`
(empty/leading-zero disabled). Exit clears admission; reinitialization resets
its existing bounded transition-log counters. Cadence rates and live engine
policy retain their ordinary per-generation/per-tick owners.
