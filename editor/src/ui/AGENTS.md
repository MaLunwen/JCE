# editor/src/ui/ — Editor Chrome, Layout & Theme

## Identity
- **Language**: C++17 + ImGui
- **Role**: docking layout, panel registry, global style/theme/palette. The "frame" around `panels/`.

## File map
- `jce_editor_layout.{h,cpp}` (113 KB)   — dockspace, workspaces (Default / Animation / Material), layout serialize/restore. **OVERSIZE — split candidate**.
- `jce_editor_layout_scene_commands.{h,cpp}` — testable scene command shims used by layout/menu/command palette (e.g. New Scene creates an untitled default scene, then focuses Scene View).
- `jce_editor_panels.{h,cpp}` (64 KB)    — panel registry (`Register/Open/Close/Toggle`), main-menu + view-menu generation.
- `jce_editor_style.{h,cpp}` (39 KB)     — `ImGuiStyle` configuration + spacing/rounding tokens.
- `jce_editor_ui_state.{h,cpp}`          — per-user UI state helpers for panel-internal tabs and lightweight chrome state.
- `jce_editor_colors.h`                  — semantic color tokens (text-primary, panel-bg, accent…).
- `jce_theme_palette.h`                  — light/dark/high-contrast palette tables.

## Rules
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
