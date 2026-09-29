# editor/src/panels/material_graph

P2-(2) Phase B output: the Material Node Editor panel split into
cohesive translation units.  Public C ABI lives in the parent
`jce_panel_material_graph.cpp` and is unchanged.

## Layout

| File | Lines | Owns |
|------|------:|------|
| `jce_material_graph_state.h`     | ~170 | namespace aliases, shared `extern` decls, wrappers over `jce_sg::`, fwd decls of cross-TU entry points |
| `jce_material_graph_state.cpp`   |  ~95 | single-definition site for all extern globals + undo/redo/log/link-validate/pin geometry helpers |
| `jce_material_graph_eval.cpp`    | ~270 | recursive CPU PBR evaluator + `compile_to_material()` (writes `.mat.json`) + Phase C `generate_shader()` (Graph → `fs_<name>.sc` via `jce_sg::codegen()`) |
| `jce_material_graph_edit.cpp`    |  ~95 | `delete_selected` / `copy_selection` / `paste_clipboard` |
| `jce_material_graph_canvas.cpp`  | ~390 | `draw_node` + `draw_canvas` (links, drag, box-select, popups, quick-add palette) |
| `jce_material_graph_preview.cpp` | ~140 | `draw_preview_sphere` + `draw_preview_pane` (compile log readout) |

Parent panel TU (`editor/src/panels/jce_panel_material_graph.cpp`)
is now ~80 lines: toolbar + 2-column layout + `extern "C"` entry.

## Conventions

- All TUs share `namespace jce_mgp { ... }`.  No anonymous
  namespaces (would force per-TU duplicates of shared globals).
- Shared mutable state (`s_g`, `s_undo`, `s_redo`, `s_clipboard`,
  `s_has_clipboard`, `s_log`, `s_prev`, quick-add palette globals)
  is declared `extern` in `jce_material_graph_state.h` and defined once
  in `jce_material_graph_state.cpp`.
- `PreviewState` is plain POD (no bgfx handles), safe to live in the
  shared header.
- Pure model ops go through `jce_sg::` (editor/src/shadergraph/).
  The thin inline wrappers in `jce_material_graph_state.h` exist only to
  keep call sites at parity with the legacy single-file panel and to
  add Undo bookkeeping at the right moment.

## Build

`editor/CMakeLists.txt` uses `GLOB_RECURSE src/*.cpp`, so files in
this directory are picked up automatically — but a CMake reconfigure
is required after adding/removing files (run `cmake -S . -B
build/desktop/windows-x64-debug`).
