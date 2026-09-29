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
