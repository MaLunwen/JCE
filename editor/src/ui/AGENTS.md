# editor/src/ui/ — Editor Chrome, Layout & Theme

## Identity
- **Language**: C++20 + ImGui
- **Role**: docking layout, panel registry, global style/theme/palette. The "frame" around `panels/`.

## File map
- `jce_editor_layout.{h,cpp}` (113 KB)   — dockspace, workspaces (Default / Animation / Material), layout serialize/restore. **OVERSIZE — split candidate**.
- `jce_editor_layout_scene_commands.{h,cpp}` — testable scene command shims used by layout/menu/command palette (e.g. New Scene creates an untitled default scene, then focuses Scene View).
- `jce_editor_panels.{h,cpp}` (64 KB)    — panel registry (`Register/Open/Close/Toggle`), main-menu + view-menu generation.
- `jce_editor_style.{h,cpp}` (39 KB)     — `ImGuiStyle` configuration + spacing/rounding tokens.
- `jce_editor_ui_state.{h,cpp}`          — per-user UI state helpers for panel-internal tabs and lightweight chrome state.
- `jce_editor_colors.h`                  — semantic color tokens (text-primary, panel-bg, accent…).
- `jce_theme_palette.h`                  — light/dark/high-contrast palette tables.

The About dialog resolves the current executable's complete UTF-8 filename
through the public host filesystem API each time the modal opens. Its existing
scroll region wraps the path and offers the normal clipboard action. A failed
or over-budget query displays localized Not Available and disables copying;
it never substitutes the working directory, a guessed filename or truncation.
This is a diagnostic UI consumer, not a new path-discovery owner. Native visual
and clipboard acceptance still require the rebuilt Editor.

## Rules

The shared panels shutdown sequence closes Console after the other panel
owners, retaining diagnostic delivery during their teardown. The Console owner
disarms its null-userdata command callbacks, synchronously removes its existing
log sink, clears output and releases session, derived UI cache and mutex. It
does not shut down the process-wide console registry. Repeated empty teardown
emits no cleanup work; actual owned teardown publishes the existing CPU trace
scope and bounded ownership counters. Its complete-owner CPU controls are
separate from native callback races, rendered output and GPU lifetime evidence.

Startup Welcome resolves once from admitted content: an authoring project,
a nonempty current scene path (including read-only Bundle preview), or an
in-flight scene load. A launch environment string or an empty default scene
pointer is not evidence of admitted content. With no content, preserve the
existing LAST/EMPTY/PICKER and known-project fallback. This startup decision
does not change the manual Help > Welcome command or later visibility.
The welcome-startup CPU fixture compiles the real pure policy and complete
layout/history admission bodies with typed state/configuration ports; native
window/rendered Welcome coverage still needs the rebuilt Editor.

Viewport maximization uses one frame-local snapshot of the existing maximized
panel slot for both docked content and the later overlay. Toolbar changes take
effect on the next frame so activation cannot submit the Game body twice.
Keep both dock windows' Begin/End calls while an overlay is present. Their
covered content does not render; only the overlay owner admits its content.
Scene's covered path still services pending CPU results and closes interactions;
do not cancel Scene's own live interaction through its empty dock shell.
The probe maintenance pump remains before layout and retains its existing
focus/drawable admission. No viewport optimization may replace its lifecycle.
The existing layout-routes Native fixture consumes strict actual state, dock,
frame-snapshot/overlay and Scene entry-prefix extracts with counted UI ports.
Its old-covered guard is an ordinary failing control. These prove CPU routing
and cleanup calls, not docking persistence, rendered pixels or GPU savings.

1. **Single dockspace** owned here; panels never call `ImGui::Begin` outside their registered dock target.
2. **All colors via `jce_editor_colors.h`** semantic tokens — never hardcode `IM_COL32(...)`.
3. **Theme switch live**: `jce_editor_theme_set(THEME_DARK)` re-applies palette without restart.
4. **Layout persistence**: ImGui docking/window/table state lives in `.jce/imgui.ini`; panel-internal UI state uses `jce_editor_ui_state` backed by `jce_editor_config.cpp`.
5. **DPI-aware**: every spacing/size pulled from `editor_dpi_scale()`; no raw pixel literals.
6. **Workspaces** = named layout presets; defaults registered here, user customizations stored in config.
7. **Mobile-class fallback**: layout MUST collapse to single-column tab strip if window < 800 px (baseline HW = small screens).

## Don't
- Don't put per-panel UI here — only chrome.
- Don't import panel headers; use the registry pattern (string IDs).
- Don't use `ImGui::PushStyleColor` outside `jce_editor_style.cpp` — wrap into named helpers.

## Common tasks
- **Add a workspace** → preset in `jce_editor_layout.cpp` + menu entry.
- **Re-skin** → edit `jce_theme_palette.h`; verify against `jce_editor_colors.h` mapping.
- **Refactor `jce_editor_layout.cpp`** → split into `layout_dockspace.cpp` / `layout_menu.cpp` / `layout_workspaces.cpp`.

## 2026-09-01 — `draw_menu_bar` 已移出 `jce_editor_layout.cpp`

`jce_editor_layout.cpp` 曾 3461 行（超 3000 上限、被体量门冻结），移出 757 行后是
**2693 行——低于上限**，不再在冻结名单上。

新增两个文件：`jce_editor_menu_bar.cpp`（函数本体）与 `jce_editor_layout_internal.h`
（两者共享的边界）。**边界是刻意做到最小的**：10 个模态标志 + 6 个辅助函数 + 2 个枚举。
之所以必须共享，是因为菜单栏**打开**模态、而模态的绘制留在 layout 里
（`s_show_save_as` 在菜单栏内被碰 1 次、外面 7 次）。

那 10 个标志**保留 `s_` 前缀**：改名要动约 80 处调用点、零行为收益，
而一个 diff 里大半是重命名的重构没人能审出行为变化。

注意：`check_file_size.py` 的基线里仍有这个文件 3461 行的旧条目。**没有重设基线**——
`--update-baseline` 会把并发会话让 `jce_renderer.c` 长到 3247 的增长一并当成批准过的数字。
那条旧条目无害（门只禁止「超上限的文件再长」），等树安静时随手清掉。

## Font source ownership

`jce_editor_font_data.h` transfers the host-reader buffer directly into the
ImGui atlas. Host IO and ED/ImGui default allocation share the tracked mimalloc
heap; no second font-size allocation or memcpy is needed. Reject empty or
greater-than-INT_MAX sources before ImGui's signed-size API. Calling the owning
AddFont operation (FontDataOwnedByAtlas=true) consumes input on every return:
its source-init NULL rollback already releases once. The transfer helper never
frees input after that call. Successful dynamic ImGui 1.92 fonts retain source
bytes until atlas Clear/destruction. Do not clear input data after GPU upload. This reduces
loading peak only; it does not establish total editor RSS or device acceptance.
The headless `test_jce_editor_font_data` checks actual allocator pairing, source
pointer identity/no extra allocation, failed consumption and atlas reload.

F12 routes through the core backbuffer request owner. Do not toast success at
submission: the application screenshot poll must first confirm file writing.

Default typography consumes explicit user override, the shared host UI role (Ink Free Regular before system Sans), licensed stock Latin, then ImGui fallback. Stock source bytes use ed_font_transfer ownership and retire when the font atlas closes. Missing CJK is reported; the user handwriting JCE.ttf is not the automatic fallback. System-font discovery belongs to the public OS/platform owner.

`jce_editor_font_picker.cpp/.h` owns searchable editor font selection only, not discovery or font buffers. Preserve explicit unlisted paths, compare Windows path separators/case and keep POSIX path case. Host default Latin prefers installed Ink Free Regular before system Sans; CJK is separate. UI font changes use the existing deferred-before-NewFrame style owner.


Preferences uses the existing `edit.preferences` hotkey registry entry and the
normal global panel-toggle dispatcher. Its Edit-menu shortcut label reads the
current registry chord, including user rebinding/unbinding; never hard-code the
default label independently. WantTextInput, active Game View/captured mouse and
modal-interaction gates retain their existing scope. Registry CPU controls in
`test_jce_hotkeys` cover Ctrl+Comma, modifier/key mismatch, once-per-frame
consumption and rebind/unbind labels. They do not certify OS event delivery,
foreground window binding or visible popup opening; those require real GUI input.


The optional bounded `JCE_EDITOR_HOTKEY_DIAG=1` observation at the existing
shortcut gates reports ready/text_input/game_input_or_capture/modal for a
pending Preferences key. It neither lowers these gates nor manufactures key
input. Event and registry observation share the core private diagnostic owner.

Shared panel initialization resets every visibility slot before applying factory defaults and saved append-only bits. Opening an unsaved specialized panel must not leak into a later lifecycle. The complete panel owner has CPU controls for full-width masks, saved-count defaults, refused visibility writes and Console ring rollover/severity/log-sink/toast boundaries. These ports do not certify panel drawing, docking or OS input.

The shared New Project entry lives in `jce_editor_layout_scene_commands.cpp` and requests PGA_NEW_PROJECT through the existing unsaved-change state machine. Menu, Welcome and command palette consume that entry; they never bypass protection by setting modal visibility directly. CPU gate-entry controls do not replace actual Save-As/Cancel GUI acceptance.

Test Runner polling belongs to the main host's pre-render poll-and-quit owner,
not `jce_editor_layout_draw`. Layout renders current child state only. Service
controls use real Test Runner children and counted window/manager ports, not
actual OS minimization or visible panel/GPU proof.


Explicit font paths remain first priority. A missing/non-file override is
reported by the existing WARN logger; a read/size failure or atlas admission
rejection reports that specific phase. The current reload call then retries the
same shared host role with no override, so Latin still attempts installed Ink
Free before compatibility Sans, stock Aileron and built-in fallback. Each
explicit attempt reports once in that reload; no global warning/cache/atlas
owner or per-frame retry is added. Accepted buffers belong to the existing ImGui
atlas; rejected admission releases input inside the owning AddFont rollback,
and `ed_font_transfer` performs no post-call cleanup. Source rejection before
AddFont uses the existing host-buffer owner. AddFont admission is not certification of
all later dynamic glyph parsing or GPU upload.

The existing registered `tools/tests/test_host_font_selection.py` additionally
extracts and compiles the actual style resolve/load bodies, shared host resolver
and font-buffer transfer bodies. Counted file/atlas-admission/logger ports cover
accepted explicit paths, missing paths, read/size/admission refusal, exact-once
release, corresponding Latin/CJK role retry, per-reload WARNs in DIST and no
repeat read of an identical resolved rejected path. The ordinary old-bypass
mutation removes only the actual override-to-role retry wire. These are CPU
selection/admission ownership controls, not a fake font parser or certification
of real installed face glyphs, alternate path aliases, physical pixels or GPU
upload. Existing ImGui atlas/font-data and host-metadata tests retain their
separate scopes.

Successful admission at the existing host-font load node uses common
`jce_log_write(INFO)` with role/path/source even in DIST. It reports once for
that accepted face in the current reload, follows the usual level/filter/sinks,
and creates no per-frame font log. The typed DIST logger control checks accepted
explicit versus shared-role source records and absence of success on rejected
sources; atlas admission remains separate from glyph/GPU certification.

Font-source C++ runtime negatives must compile successfully, then return the
actual REQUIRE port's ordinary exit 3 with its assertion marker on stderr.
Access violations, aborts or a missing assertion witness do not pass these
controls. This tightens test evidence without changing atlas ownership.

The existing font-data native target exercises actual ImGui invalid-font parsing
in non-merged and merged source transactions: NULL, exactly one source release,
rolled-back source lists, preservation of the accepted merged destination, and
no repeated release at Clear/context destruction. Only the documented
recoverable-user-error assert is routed to ImGui's retained debug diagnostic;
ordinary internal assertions remain enabled. Counted free ports refuse a second
physical free so a regression fails ordinary assertions rather than crashing.
The Python helper old-wire mutation uses a consuming typed port and an ordinary
exit/status marker; it does not replace or certify the real parser control.


Host font buffers of 100 bytes or fewer are refused before ownership transfer,
matching pinned ImGui's ordinary `AddFontFromMemoryTTF` size assertion. Stock
font source size refusal reports its actual asset path and bytes before allocation.
The two actual style admissions use `ed_font_transfer_recoverable`: a call-local
RAII guard temporarily disables only recoverable user-error assertions and
restores the real IO flag's prior value. Debug diagnostics and all ordinary
internal ImGui assertions remain enabled; `ed_font_transfer` itself still
performs no post-call cleanup. This handles supported recoverable parser refusal,
not arbitrary malformed font table safety or later glyph/GPU validation.
The existing native font-data fixture starts with the real default assert flag,
covers invalid parser rollback, merge, valid reload, true/false restoration and
tiny source release before AddFont. Typed CPU probes consume the actual two
style functions and guard; the missing-guard mutation must compile and fail via
an ordinary counted assertion, never an ImGui abort or duplicate physical free.

## Profiling layout routes

Profiling applies the existing shared extension-panel defaults before its
explicit upper-left Profiler, Network Stats and Physics Debugger cluster.
Later DockBuilder calls replace a stable window ID's destination, so an
extension pass after that cluster would move Profiler to the bottom and the
other two tools beside Game. Keep the other nine presets' existing order and
routes unchanged; this fix introduces no second docking owner or state.

`test_jce_editor_layout_routes` consumes strict extracts of the complete shared
helper, ten preset bodies and their existing dispatcher with typed DockBuilder
ports. It checks final destinations, split proportions, Default fallback and
the separate Scene/Game quadrants. Its ordinary old-routing contract moves
only the Profiling extension call back after the explicit cluster. These CPU
controls do not certify ImGui docking pixels, native dragging, saved layouts,
scroll access or physical UI reachability; test those in a rebuilt Editor.

The normal layout owns hidden, covered and Begin-refused Game diagnostics because
it calls Game content directly. Its existing dock/overlay branches publish the
current policy to the reset Game observation; the standalone wrapper is not a
substitute for these normal routes. The actual-layout CPU fixture consumes these
branches while retaining one content invocation and balanced dock membership.

The shared panels shutdown sequence first calls the private Editor LAN client
shutdown. It stops/clears only the discovery client, leaving the session and
server owners alone, and stays safe before Refresh and across repeated shutdown.
Console remains last so later teardown diagnostics still have their owner.

## Covered Console presentation

The normal Console caller preserves its actual Begin/End dock membership and
passes admission from this UI frame's frozen maximized owner plus current/main
ImGui viewport IDs. Game or Scene maximization covers only the same host; a
Console on another viewport still presents. Toolbar maximize/restore changes
the next frame's owner and must not switch Console policy partway through the
current frame. Timeline remains its independent existing shim.
