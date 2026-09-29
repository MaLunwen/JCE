# editor/src/dialogs/ — Modal Dialogs

## Identity
- **Language**: C++17 + ImGui
- **Role**: blocking / semi-modal popups (project, build, preferences, asset picker…).

## File map
- `jce_dialog_project.cpp` (36 KB)            — New / Open / Recent project.
- `jce_dialog_project_settings.cpp` (40 KB)   — full settings tree per project.
- `jce_dialog_preferences.cpp`                — user-global preferences.
- `jce_dialog_build.cpp`                      — build target / profile picker.
- `jce_dialog_bundles.cpp`                    — asset bundle authoring.
- `jce_dialog_open_bundle.cpp`                — bundle import.
- `jce_dialog_scene.cpp`                      — new / load scene.
- `jce_dialog_asset_picker.{h,cpp}`           — reusable asset reference picker.
- `jce_path_input.{h,cpp}`                    — path text field with PhysFS validation + dialog button.
- `jce_editor_dialogs.h`                      — public registry / open helpers.
- `jce_editor_dialogs_internal.h`             — shared decls.

## Rules
1. **All dialogs route through `jce_editor_dialogs.h`** (`jce_dialog_open("project_settings")`) — never call `ImGui::OpenPopup` from panels directly.
2. **Modal stack capped at 3** — deeper nesting is a UX bug; refactor into wizard.
3. **OS file picker**: use `jce_platform_*` (SDL3 backed) — never call Win32/AppKit/GTK directly.
4. **Validation before commit**: dialogs may not mutate state; they collect input and call `jce_editor_state_*` apply functions on OK.
5. **Cancel must restore state** — store snapshot on open, restore on cancel.
6. **i18n** all titles, labels, buttons.
7. **`jce_path_input`** is the canonical path widget; do not roll your own.

## Don't
- Don't make a dialog "always open" — that's a panel.
- Don't bypass the i18n layer.
- Don't write to disk inside `Draw()`; queue a job via `jce_thread_pool_*` (engine/include/jce/os/core/jce_thread.h).

## Common tasks
- **Add a new dialog** → `jce_dialog_<foo>.cpp` → register in `jce_editor_dialogs.h` → expose `jce_dialog_<foo>_open(...)`.
