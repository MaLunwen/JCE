# editor/src/core/ — Editor Singletons & State

## Identity
- **Language**: C++17
- **Role**: per-process editor state — owns config, history, hotkeys, i18n, reflection, asset DB, build pipeline.
- **Consumed by**: every other `editor/src/*` module.

## File map
- `jce_editor.{h,cpp}`              — top-level Editor application; bootstraps subsystems.
- `jce_editor_state.{h,cpp}`        — **central state** (selection, mode, dirty flags, active scene/project). 67 KB — split-candidate.
- `jce_editor_state_internal.h`     — implementation details for state subsystems.
- `jce_editor_component_registry.{h,cpp}` — editor-side component identity table; maps legacy `JCE_COMP_FLAG_*` and synthetic flagless slots (e.g. Compound Collider) into one Add/Inspector ordering path.
- `jce_editor_config.{h,cpp}`       — user preferences (recent projects, window layout, theme key).
- `jce_editor_defaults.h`           — factory-default constants.
- `jce_editor_history.cpp`          — undo/redo stack (capped memory).  **TWO RECORD KINDS on one stack**: a full-scene record (serialise everything) and an ENTITY-SCOPED one (`entity_id != 0`) holding one entity's components plus its component SET.  Scoped is what the inspector's value edits push, because the full path serialises the whole scene TWICE per edit -- once to push, once to compare -- which is ~1.4 s and a 26 MB string comparison at 50k entities, and its restore CLEARS the scene, reissuing every handle and dropping the selection.  The component SET is not optional: `jce_scene_parse_entity_json` only adds and overwrites, so without it an undo cannot remove a component the edit added.
- `jce_editor_history_order.h`      — pure chronological selector shared by scene and external undo providers.
- `jce_editor_recorder.{h,cpp}` — editor screen recording (F9) and WebM muxing.
- `jce_editor_i18n.{h,cpp}`         — string table loader (JSON) + `editor_t("key")`.
- `jce_editor_play.cpp`             — Play-mode (`Ctrl+P`): snapshot scene → run game module → restore.
- `jce_editor_toast.{h,cpp}`        — transient notifications (info/warn/error stack).
- `jce_editor_automation.{h,cpp}`   — **the one door** to the Automation API (`private/tools/automation/automation_cli.py`), spawned through `jce_process.h` and pumped once per frame.  Never blocking (a `physics.probe` runs a real solver for seconds) and ONE CALL IN FLIGHT, because that layer takes a single-instance lock and two concurrent calls through it exit without writing a result — which both call sites translate into `INVALID_ARGS`, about arguments that were never wrong.  REQ-ARCH-02's call sites: `Tools > Validate Physics` runs `physics.probe` READ-ONLY, and `Tools > Pin physics thresholds` runs `physics.save_thresholds` through `jce_editor_automation_write()`.  **A WRITING CALL IS THREE CLI INVOCATIONS** — `changeset.begin` → the tool → `changeset.commit`, rolling back on any failure — because `api.call` does `if spec.writes: require_changeset()` and the CLI has no single-shot form.  Its undo is `changeset.rollback`, NOT Ctrl+Z, and that is the ownership rather than a limitation: a tool that writes a file acts on the disk layer, so wrapping it in `jce_state_begin_batch_edit` would push an undo record for an in-memory change that is not happening.  `tools/lint/check_editor_automation_undo.py` enforces both the one-door rule and the scope rule.
- `jce_editor_presets.{h,cpp}`      — import / build preset registries.
- `jce_editor_path_util.cpp`        — path normalization (PhysFS-safe).
- `jce_editor_scene_rendering_defaults.{h,cpp}` — project rendering defaults → scene rendering settings seed bridge.
- `jce_editor_project_render_pipeline.{h,cpp}` — isolated `<project>/Settings/RenderPipeline.rp.json` loader; failed project switches return a zero descriptor so the caller explicitly reapplies the current hardware-tier preset instead of retaining another project's RP state.
- `jce_editor_alloc.h`              — editor's allocator selector (mimalloc via `jce_alloc_*`).
- `jce_editor_quat.h`               — editor-local quaternion helpers (uses `jce_math`).
- `jce_hotkeys.{h,cpp}`             — global accelerator table; resolves `Ctrl+S`, etc.
- `jce_project_settings.{h,cpp}`    — per-project `.jceproj` schema + JSON load/save.
- `jce_reflect.{h,cpp}`             — runtime reflection registry (component fields → ImGui widgets).
- `jce_reflect_builtin.cpp`         — registers built-in `JceXxx` components into reflection.
- `jce_assetdb.{h,cpp}`             — editor-side asset database (GUID ↔ path ↔ meta).
- `jce_build_manager.{h,cpp}`       — editor-owned build pipeline: in-process asset cook/pack/BOM/embed generation, then CMake/Ninja/MSVC orchestration.
- `jce_binary_embed.{h,cpp}`        — deterministic C/COFF/asm wrappers for embedding cooked archives into a single executable.
- `jce_dist_audit.{h,cpp}`          — fail-closed Dist verifier: GUI subsystem, exactly-one embedded secure JPAK, authenticated entry reads, protected-path leak scan, and one-file public package policy; writes a private JSON report.
- `jce_dist_content_graph.{h,cpp}`  — UI-free Bundle/Dist parity bridge: builds the startup-scene dependency graph in-process, extracts its cooked bytes, and preserves the approved graph snapshot for Dist audit.
- `jce_run_manager.{h,cpp}`         — launches built binaries / Play-mode subprocess.
- `jce_editor_script_backends.{h,cpp}` — registers the language VMs this editor was LINKED with, and owns the project's native (C/C++) script modules. Two reload entry points and they are not interchangeable: `jce_editor_script_modules_reload()` runs from the project-open seam and SKIPS a path it already holds, so calling it twice on one project is a no-op; `jce_editor_script_modules_reload_native()` is the explicit File-menu command, which refuses by name while any script instance is live, then unloads everything and re-enters the first one. Proven to pick up a rebuilt image rather than re-attach to the resident one (`tests/scripting/cpp/test_jce_script_vm_cpp_modules.cpp`).
- `jce_pak_key.{h,cpp}`             — emits the two XOR shares of the project asset key as a C translation unit. The engine default (`engine/src/application/jce_pak_key_default.c`) and the CLI emitter (`scripts/jce.py`) write the same two symbols; `tools/lint/check_pak_key_shares_contract.py` holds all three to the same names and the same 64 bytes, because a drift there links fine and decrypts nothing at a customer site.

## Rules
1. **All editor state goes through this folder** — no globals in `panels/`, `dialogs/`, etc.; they take pointers/refs to state owned here.
2. **C++17 only**: use `std::filesystem`, `std::string_view`, `std::optional`. **No `std::format`** (not in libc++ on macOS 10.13).
3. **No engine private headers**: editor links against `JCE` (public) + `jce_tools_imgui` (INTERFACE) only — never `engine/src/**`.
4. **Allocation**: all `new`/`malloc` route through `jce_editor_alloc.h` → `jce_alloc_*`.
5. **Logging**: `jce_log_*` (engine), not `printf`. Editor adds a sink that pushes to `jce_panel_console.cpp`.
6. **Threading**: editor is single-threaded on UI; background work (cook, import) uses `jce_thread_pool_*` (engine/include/jce/os/core/jce_thread.h).
7. **i18n**: every UI string flows through `editor_t("key")`. Hardcoded strings caught by `tools/lint/i18n_hardcoded.py`.
8. **Reflection-driven inspector**: components register field descriptors here once; `panels/jce_panel_inspector.cpp` auto-renders.
9. **Hotkeys** centralized in `jce_hotkeys.cpp` (one source of truth) — panels MUST NOT capture keys directly.
10. **File size**: `jce_editor_state.cpp` (67 KB) and `jce_project_settings.cpp` (36 KB) approach the 2000-line soft cap — split before adding new domains.
11. **Packaged-scene lifecycle**: every transition away from Bundle Preview, including New Scene and shutdown, must close the isolated VFS before source/default content is used.
12. **Project RP ownership**: opening or clearing a project must replace live RP state. A missing/malformed project RP falls back to `jce_render_pipeline_preset_for_current_tier`; it never inherits the previous project's descriptor.
13. **Never unload a native script module with live instances.** Unloading unmaps the code those instances dispatch into, and the crash lands in the caller rather than here. The safe point is Play stopped; ask `jce_script_vm_cpp_live_instances()` and refuse by name rather than assuming.

## Don't
- Don't talk to ImGui from headers exposed to non-editor code.
- Don't use raw `std::ofstream` / `fopen` — go through `jce_fs_*` (PhysFS-backed) for any path the runtime may see.
- Don't crash on missing translation keys; fall back to the key string.
- Don't bypass `jce_editor_history` for state changes — every mutation must be undoable or explicitly "non-undoable".
   **This includes a mutation that arrives from the Automation API.**  Decided
   2026-09-20 (REQ-ARCH-02), because it had been left open on the belief that
   the undo stack and the changeset were two provenance systems competing for
   one job.  They are not — they own different things, and the seam is the
   SAVE:

   | layer | owns | undone by |
   | --- | --- | --- |
   | `jce_editor_history` | the in-memory scene between saves | `Ctrl+Z` |
   | changeset (`private/tools/automation/`) | bytes on disk between commits | `changeset.rollback` |

   So an editor call into the Automation layer wraps itself in
   `jce_state_begin_batch_edit` (or `begin_entity_edit`) like any other edit,
   and `Ctrl+Z` never becomes a changeset revert.  That is Unity's
   `Undo.RecordObject` rule, Unreal's `FScopedTransaction` rule and Godot's
   `EditorUndoRedoManager` rule unchanged; in all three, version control is a
   separate file-level layer acting on what is on disk after a save.
   `tools/lint/check_editor_automation_undo.py` holds the line.

- **Two defects lived in the door itself and no gate could see either**
   (found 2026-09-20, while wiring `physics.author_apply`):

   1. **It could never have started the CLI for a project that is not the
      repository.**  It ran `private/tools/automation/automation_cli.py` with the
      working directory set to the *open project*, and
      `examples/caged_kingdom/tools/` does not exist — so the toast said *"Could not
      start python"*, naming the interpreter for a path bug.  The lint checks
      call structure, the compiler accepts a wrong relative path, and nothing
      in the suite starts an editor and opens a menu: **every green light was
      reporting on something real, and none was reporting on whether it
      works.**  `automation_cli_dir()` now walks up from the project and then
      from the executable, and refuses with a named reason when there is no
      engine tree — which is a real configuration, not an error.
   2. **Arguments were silently truncated at 1023 bytes**, in three places at
      once: `Call::args`, `spawn_step`'s `argv`, and `split_args()` in
      `jce_process.c`, which copies each token into a `char[1024]`.
      `physics.author_apply` takes the whole `physics.author_plan` result —
      **2671 bytes for a one-part prop** — so 1533 bytes vanished unlogged.
      Arguments now go through a **file** (`--json-file`), which also retires
      the old quoting rule: JSON no longer has to avoid containing a quote,
      because it no longer crosses the splitter.

   ⇒ **A payload that matters does not cross `argv`, and a relative tool path
   is a claim about the working directory.** Truncated JSON only *usually*
   fails to parse; the case that matters is the well-formed remainder that
   means something else.

- **The ONE DOOR gate was made LOOSER by a good refactor and reported OK**
   (2026-09-20, its fifth iteration and the third such loosening).  Factoring
   the three appliers onto a shared `apply_kept_plan(planner, tool, ...)` meant
   they stopped naming `jce_editor_automation_write` on their own line, so the
   gate stopped seeing them: **`2 mutating call(s)` → `1`, exit 0, while two
   writing tools were being added.**

   The only signal was a count moving in a direction the change could not
   explain.  The repair: a *static* helper inside the door that reaches the
   transport is itself a transport, and **its tool names belong to whoever
   reaches it** — without that second half the per-frame `poll()` became a
   finding, asking for an undo scope around a read.

   ⇒ **The mutation control for this class cannot be an exit code.**  The
   defect's signature is under-counting, not failing: deleting the inheritance
   gives `5 → 1` with `rc` still `0`.  Assert on the printed count.

   ⇒ **A fixture that reads any part of the subject is not a fixture.**
   `--self-check` was passing `classify()` the *live* `door_helpers()`, so
   every fixture's expected value depended on what the door happened to
   contain — editing the door flipped one, and `classify()` had not changed at
   all.  The fixtures now carry their own `FIXTURE_HELPERS`; the live door is
   checked separately and by property (it must resolve at least one helper
   that names a tool).

- **Adding a plan-driven tool: use the `PLANNERS` table and `plan_tool`.**
   One plan slot serves `physics.author_plan`, `physics.ragdoll_plan` and
   `scene.compile`.  Three slots would be three chances to apply a stale one,
   and **every applier takes an `object`** — so feeding `ragdoll_apply` a
   collider plan is a *type-correct* call that writes nonsense.  `plan_tool`
   makes that unrepresentable; `plan_subject` does the same one level down for
   "planned model A, selected B".

- **Address the scene document by NAME, not by the selection id.**
   `jce_state_to_ecs_entity()` resolves an editor id as a scene **index**;
   the tools' `entity` is a scene **document** reference (`"id": 1001`).  Both
   are small integers, so the wrong one authors onto a plausible wrong entity
   and nothing reports it.  The tools' `find_entity` refuses an ambiguous name
   rather than taking the first match — a better answer than any
   disambiguation the editor could invent.

## Common tasks
- **Add a hotkey** → register in `jce_hotkeys.cpp`, document in `editor/AGENTS.md`.
- **Add a config field** → schema in `jce_editor_config.h`, load/save in `.cpp`, default in `jce_editor_defaults.h`.
- **Expose new component to inspector** → add `JCE_REFLECT_FIELD(...)` calls in `jce_reflect_builtin.cpp`.

## 2026-09-21 — 推送物理层矩阵**只有一处**，构建路径曾经自己抄了一份

`jce_project_settings_push_physics_layers()` 是**唯一**的推送点，
`jce_editor_play.cpp:435` 与 `jce_build_manager.cpp` 都调它。

在此之前 `jce_build_manager.cpp` **把同一段 names+matrix 循环内联抄了一份**。
后果不是重复本身，而是：**二维矩阵要想进到出货构建，必须在两个地方分别被记起来**，
而 cook 路径恰恰是决定「打包出去的游戏拿到什么」的那一条。
一个推送有两份实现 = 关于它的两个陈述。

⇒ 往矩阵里加任何东西（第三张矩阵、每层的别的属性），**只改那一个函数**；
如果发现自己在第二个地方写同样的循环，那第二个地方应该改成调用它。

**注意 `check_editor_consumption` 分不出「编辑器调用了它」和「编辑器提到了它」**——
它扫的是编辑器树里的符号出现。所以一个**按名字转发、实际什么都不做**的桥
照样能让那道门变绿。判据要放在**测试**上，不是那个计数上。
