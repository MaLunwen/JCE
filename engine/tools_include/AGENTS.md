# engine/tools_include/ — Editor-Only INTERFACE Targets

## Identity
- **Layer**: side-channel between engine and editor
- **Visibility**: this include directory is added **only** to `JCE_EDITOR_*` CMake targets (never to the public `JCE` interface).
- **Language**: C++ (ImGui is C++) — guarded by `JCE_EDITOR_BUILD`.

## File map
- `jce/tools/jce_imgui.hpp`           — public-to-editor ImGui facade (context, theme, helpers).
- `jce/tools/jce_imgui_internal.h`    — internal extensions / forward decls (editor & renderer only).

## Rules
1. **NEVER include from `engine/src/` (game runtime)** — these headers exist so editor and bgfx-backed imgui renderer (`engine/src/middleware/ui/jce_imgui_renderer.cpp`) can share decls **without** leaking ImGui into the game API.
2. The CMake target name is `jce_tools_imgui` (INTERFACE) — link from editor and from `jce_imgui_renderer.cpp`; nothing else.
3. **Editor-only**: any TU including these headers MUST be excluded from the game DLL/static lib via CMake `target_sources(... PRIVATE)` gated by `JCE_EDITOR_BUILD`.
4. ImGui is **vendored**; never link a system imgui.

## Don't
- Don't add this folder to `target_include_directories(JCE INTERFACE ...)`.
- Don't add gameplay HUD helpers here — game UI uses RmlUI (`engine/src/middleware/ui/`).
- Don't expose ImGui types in `<jce/...>` public headers, ever.

## Common tasks
- **Add an editor-only widget helper** → declare in `jce_imgui.hpp`, define in `editor/src/ui/` or `engine/src/middleware/ui/jce_imgui_renderer.cpp`.
