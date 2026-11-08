# editor/src/dialogs/ — Modal Dialogs

A real project transition must consume close_bundle_preview's boolean result
before root publication. Failed Play scene restoration keeps the outgoing root,
Bundle and restore point available for retry, including when Play is already
STOPPED. Persistence preflight success alone does not admit the transition.


## Identity
- **Language**: C++20 + ImGui
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

The optional path-input changed_before output is emitted only on actual change, preserves Slot.last_seen across asynchronous browse, resynchronizes ordinary external/Undo values, and rejects insufficient output capacity before field mutation. Keep the original field as the stable browse destination.

New Project consumes the UI-free `core/jce_editor_new_project` continuation. File, Welcome and command-palette entries share the existing Save/Discard/Cancel/Save-As gate before opening the modal. Complete success requires a current-tier RP asset, a fresh selected 2D/3D default scene, an actual scene save and matching startup_scene manifest publication. A partial directory is retained with a precise failure stage; it is not silently deleted or recorded as complete.

New-project activation stops Play while the outgoing project context still owns its modules; asset/project repoint and module reload follow. Scene construction failures retain partial output without an outgoing scene path, and final completion requires published and current-project startup-scene readback, not merely a writer return value.

Project-root transition returns a private boolean result. Snapshot outgoing overlays before requesting the state-owner switch; if its atomic publication fails, retain the outgoing root/table and do not change asset bindings, recents or success state. Open/Welcome leave the dialog available for retry. New-project activation may leave its newly created directory for recovery, but cannot claim activation success or repoint the asset browser after refusal.

Reject root input truncation before normalization or outgoing snapshot mutation. Normalize through the existing dialog path rules before identity admission. The private project-switch preflight accepts the candidate root: same identity (including empty-to-empty) leaves its dirty table and bindings intact, while a real change or close snapshots and force-publishes outgoing A without binding B. A failed publication refuses only that identity change. Scene loaders select a candidate with the existing public project-context resolver before teardown and bind B only after their existing read/JSON/contract admission. Repeating a clean flush for a true full single-threaded transition is intentional; there is no pending-state queue.

Normalized root identity uses the shared private project-context path comparison: Windows ignores case, POSIX preserves it. Use this same policy for admission and actual root binding; manifest-suffix spelling rules are independent and unchanged. Case folding is not a symlink/alias resolver.

After successful identity preflight, both project close and project change retire
Play (including a pending viewport request), scene streaming and Bundle preview
through jce_state_close_bundle_preview before committing the new root or loading
its modules/RP. Do this even when the new project has no last_scene. A later
scene close must not replay the outgoing Bundle's saved RP over the new project.
Same normalized identity is a no-op and preflight refusal does not retire. If
the subsequent root-owner publication refuses, retain the old root with content
safely stopped and its source RP restored; do not claim a live-preview rollback.
