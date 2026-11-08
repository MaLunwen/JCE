# editor/src/core/ — Editor Singletons & State

Scene replacement consumes the boolean stop_play_before_scene_swap gate. It
also retries a STOPPED session's retained authoring snapshot; false forbids
root/mount/scene replacement and preserves the snapshot for retry. Only final
state destruction calls jce_editor_play_finish_shutdown: Stop is attempted only
for active/pending Play or a retained restore point; an already retired owner
does not create a duplicate Stop transaction. When restoration is attempted, a
failed restore is logged and traced FAILED, then its now-terminal restore point
is released. This is not restoration success and ordinary transitions must not
use the terminal discard path.


jce_state_close_bundle_preview retires Play/pending Play and scene streaming
before the Bundle mount and temporary RP. Project identity transitions call this
while the outgoing root still owns modules, before new-root publication; a
same-root transition does not close. This entry is idempotent and does not promise
to restore a live preview after a later project-state publication failure.

The Game input bridge has an explicit virtual-pointer producer alongside native
events. Its private fixed-size state feeds the same pointer/position/touch/action
commit before the runtime step; it never changes ImGui, viewport focus/hover or
the OS cursor. Native admission remains focused runtime ownership plus hover or
capture. Virtual admission requires a visible runtime-owned current finite
display/render geometry and an inside cursor. Its explicit source takes
precedence until cancelled, holds buttons across commits, drains deltas/wheel,
and requires a cursor for wheel. Outside UP cancels instead of clicking at an
old position. Hide/Stop/owner change, any display/render geometry change, reset
or explicit clear cancels the virtual state. Same-geometry focus changes do not.
Inactive virtual state adds only a bounded branch/storage; no allocation, worker
or trace stream is created. This is not a claim of zero instruction overhead.
The opt-in main Game fixture marks synthetic virtual-pointer admission separately
from ordinary key dispatch. EOF/error clearing is deferred until the next draw,
after the last event has had its normal runtime commit. No virtual producer is
enabled by the normal platform-event handler. INFO fixture records report source,
acceptance and observed focus/hover/capture; they do not certify physical input.
Explicit fixture rows use gated direct INFO publication so Dist macro stripping
cannot remove requested evidence. The Canvas uses a non-consuming virtual peek
through the same mapping as the runtime sample. It requires the current panel
and FBO geometry to match the admitted source; hidden, changed or non-runtime
views refuse. It does not drain runtime transients or alter native admission.

## Identity

`JCE_EDITOR_PROFILE_DIR` selects an explicit absolute user-configuration
directory before startup. The shared dotjce path resolver covers preferences,
session, ImGui layout and editor caches. Invalid explicit paths never fall back
to the user's HOME; default interactive behavior remains HOME/.jce. The pure
profile path helper rejects escaping names, drive-relative roots and overflow,
and retains UNC roots through the engine path facade.
- **Language**: C++20
- **Role**: per-process editor state — owns config, history, hotkeys, i18n, reflection, asset DB, build pipeline.
- **Consumed by**: every other `editor/src/*` module.

## File map
- `jce_editor.{h,cpp}`              — top-level Editor application; bootstraps subsystems.
- `jce_editor_state.{h,cpp}`        — **central state** (selection, mode, dirty flags, active scene/project). 67 KB — split-candidate.
- `jce_editor_state_internal.h`     — implementation details for state subsystems.
- `jce_editor_component_registry.{h,cpp}` — editor-side component identity table; maps legacy `JCE_COMP_FLAG_*` and synthetic flagless slots (e.g. Compound Collider) into one Add/Inspector ordering path.
- `jce_editor_config.{h,cpp}`       — user preferences (recent projects, window layout, theme key).
- `jce_editor_project.{h,cpp}` — loaded public Project adapter. Field/bundle saves
  stage only their owned replacement, preserve the cached object and unrelated
  borrowed fields, and transfer ownership only after successful publication.
  Failed saves preserve both disk and cache. Project activation/close still owns
  complete cache destruction; editing a field invalidates only that field's
  prior borrowed value. The public saver retains unknown nested manifest nodes.
  A private JSON signature retains the public Project's known source semantics;
  field/Bundle saves compare a current absolute-host decode before preparation.
  Changed known values or unavailable source verification refuse with an explicit
  reload diagnostic. Unknown extensions and formatting are excluded; existing
  public-loader filtering/defaults remain authoritative. Successful publication
  refreshes the source baseline independently of cached cleared values. Failed
  refresh retains the true successful save result but blocks a later save until
  reload. This preflight does not lock the file or eliminate check/save/refresh
  races; cooperating writers still serialize their edits.
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
- `jce_editor_effective_render_settings.{h,cpp}` — shared viewport/Play/package
  authored-file and quality composition. The optional private loose-content
  argument supplies a content base without creating a project. Explicit loose
  scenes consume their own authored file and skip cached/default project-quality
  overrides; ordinary manifest, isolated Bundle and default package calls retain
  existing semantics. This does not clear the general project-settings cache or
  certify unrelated Play Audio/Time/Physics state.
- `jce_editor_project_render_pipeline.{h,cpp}` — isolated `<project>/Settings/RenderPipeline.rp.json` loader; failed project switches return a zero descriptor so the caller explicitly reapplies the current hardware-tier preset instead of retaining another project's RP state.
- `jce_editor_alloc.h`              — editor's allocator selector (mimalloc via `jce_alloc_*`).
- `jce_editor_quat.h`               — editor-local quaternion helpers (uses `jce_math`).
- `jce_hotkeys.{h,cpp}`             — global accelerator table; resolves `Ctrl+S`, etc.
- `jce_project_settings.{h,cpp}` — project-scoped `.jce/project-settings.json`
  persistence and cached snapshot. Root selection consumes the shared identity
  rule for normalized paths: changed roots retire the snapshot; same-root edits
  survive. NULL/empty share one no-project identity. Selection performs no IO
  and does not reset live engine registries/physics; explicit load/save preserve
  the existing no-project CWD fallback. Re-population belongs to load/save/apply.
  Whole-owner controls retain ordinary old-setter assertion failures separately
  from GUI/Play and already-applied engine-state acceptance.
- `jce_reflect.{h,cpp}`             — runtime reflection registry (component fields → ImGui widgets).
- `jce_reflect_builtin.cpp`         — registers built-in `JceXxx` components into reflection.
- `jce_assetdb.{h,cpp}`             — editor-side asset database (GUID ↔ path ↔ meta).
- `jce_build_manager.{h,cpp}`       — editor-owned build pipeline: in-process asset cook/pack/BOM/embed generation, then CMake/Ninja/MSVC orchestration.
- `jce_build_tool_probe.{h,cpp}` — private tool-version admission using the
  public process, clock and thread owners. Its cooperative 2 s budget is checked
  before every nonblocking pipe read, before completion admission and after
  cleanup. Fair 256-byte stdout/stderr rounds independently retain at most
  1023 bytes per stream; combined output above 64 KiB refuses admission. Only
  an observed exited/drained, exit-zero query with a nonempty stdout line gives
  a version. An empty pipe round must occur after observing exit so queued final
  bytes cannot be skipped. Bounded stderr and current spawn errors remain separate failure
  diagnostics. Every spawned handle is destroyed once through public kill/reap;
  a failed explicit kill still delegates destroy cleanup. Synchronous spawn and
  OS cleanup are not hard real-time bounded by the cooperative query budget.
  Optional canonical `editor.build.tool_probe` CPU service records include
  outcome, bytes, truncation, elapsed time and kill acceptance; they do not prove
  external compiler success. Full-owner counted controls and ordinary wire
  negatives are distinct from native GUI/tool execution.
- `jce_build_process_outcome.h` — private terminal admission for the existing
  Build process owner. `poll_state` latches stopping before public release clears
  it. A Stop request wins over exit zero: retain the real child exit code, use
  the existing non-success FAILED state with an explicit cancellation diagnostic,
  and refuse queued-step advance, finish verification and publication. The
  existing public package remains untouched. Ordinary exit-zero/nonzero and
  failed next-spawn/finish behavior keep their existing ownership. The canonical
  `editor.build.tool_completion` CPU service records this observed completion
  transition, including release/next-step/finish, as SUCCEEDED/FAILED/CANCELLED;
  it is not child execution time or an OS kill/reap/publisher certificate. Real
  trace parent closure and actual manager bodies are tested through counted
  process/publisher ports and the shared no-draw host-maintenance owner. The
  same-body omitted-Stop control must compile and fail ordinary assertions.
- `jce_build_log.h` — private transcript storage for the existing Build owner:
  Tool stdout/stderr are origins, not severity. The manager classifies common
  compiler/CMake/Ninja diagnostics independently of the pipe, retains stderr
  origin in normalized lines, and leaves unclassified output informational.
  Classification never changes exit/Stop/queue/publication admission. Newline,
  fragmented and EOF partial lines share one classifier; EOF incompleteness
  remains explicit. Source paths containing error/warning are not diagnostics.
  1 MiB of normalized owner-observed lines, independently bounded worker replay
  entries, and explicit truncation/incomplete-line flags. It is not byte-exact
  stdout/stderr or cross-channel clock ordering. Worker logging never touches
  Console/ImGui; main-thread completion replays through the same `log_line`.
  The private borrowed snapshot lasts until the next append, reset or shutdown
  and is consumed immediately on the owner thread. Headless terminal publication writes `<result.json>.build.log` once
  through public host atomic IO; result v1 adds path, bytes, format and independent
  completeness/status/error fields. An empty/unavailable snapshot, incomplete
  line, truncation or failed write cannot certify complete evidence or reuse an
  old log as current. Only an accepted current start may query/publish the
  snapshot; synchronous start rejection leaves prior log bytes untouched and
  publishes unavailable diagnostic fields. Diagnostic failure preserves the
  real build state/exit/error.
  The existing driver fixture consumes strict extracts of actual log/pipe/snapshot
  bodies and the actual terminal writer, plus a compiled capture-wire removal.
  Its optional old-wire mode must fail ordinary doctest assertions; typed Console
  ports and public host files are not a native compiler or GUI completion proof.
- `jce_binary_embed.{h,cpp}`        — deterministic C/COFF/asm wrappers for embedding cooked archives into a single executable.
- `jce_dist_audit.{h,cpp}`          — fail-closed Dist verifier: GUI subsystem, exactly-one embedded secure JPAK, authenticated entry reads, explicit confidential-name scan, and one-file public package policy; writes a private JSON report.
  Windows Build binds the captured requested architecture to PE machine before
  both pre-stage and staged audit. Nonzero expected machine consumes full PE32 /
  PE32+ ordinary and delay import tables, including bounded file-section RVA and
  PE32 VA mapping; mismatched/unknown target or external dependency fails. The
  OS-owned DLL list and exact API-set prefix/case semantics mirror the existing
  CLI standalone verifier and have a CPU source parity control. JSON records
  observed/requested machine and unique imports independently of PAK verification.
  Zero expected machine retains the existing subsystem-only primitive fixture
  scope; GUI never selects it. Full-owner old-machine / old-import mutations must
  compile and fail unchanged ordinary assertions, not crash. Constructed PE bytes
  do not prove execution on another architecture or a zero-environment builder.
  Payload encryption/authentication/deep verification and Bundle graph parity
  remain mandatory. Explicit `protected_paths` occurrences outside the project
  PAK are fatal; other selected graph address literals are separate observations
  and do not establish plaintext payload leakage or runtime use. No public stock
  name or current consumer spelling is an implicit exclusion or confidential name.
- `jce_dist_name_policy.{h,cpp}` — private name-confidentiality declaration reader
  for optional `confidential_asset_paths` in the same project manifest. Public
  host capped IO and strict JSON enforce a 1 MiB manifest, 256 canonical exact
  addresses, 1023 bytes per address and 64 KiB total; duplicate keys/addresses,
  encoded NUL, wildcard, escape and noncanonical spelling reject admission.
  The graph owner admits only exact selected-graph members and publishes one
  sorted `confidential_paths` set. The actual manager adapter consumes that set
  for `item.protect_path`; it never changes mandatory payload encryption.
  The public project saver preserves unowned original JSON nodes on unrelated
  edits without adding public ABI fields or schema owners; duplicate root keys
  refuse the save rather than repairing an ambiguous declaration.
  Its preservation reader uses original host bytes and strict JSON; raw/active
  encoded NUL and non-JSON suffixes cannot be losslessly rewritten and refuse
  the save without touching original disk bytes. Literal escaped backslash text
  is preserved; this checks serialization admission, not private path schema.
  Graph/audit fixtures use real host Bundle/Archive/secure PAK APIs and compiled
  address literals in synthetic PE bytes. A strict extract of the actual manager
  adapter and structs has a missing-confidential-wire runtime negative using the
  same ordinary assertions. These CPU controls do not certify a linked shipped
  executable, GPU operation or any target release acceptance.
- `jce_project_prepared_assets.{h,cpp}` — private shared Bundle/Dist canonical
  runtime-config root adapter using public pack options. Both producers bind the
  finite `render_settings.json`, `physics_layers.json`,
  `settings/project-settings.json`, `settings/render_pipeline.rp.json` and
  `settings/input_actions.json` roots when present; selected roots are required
  and follow descriptor/LUT closure. Public VFS -> source -> resolver precedence
  remains; cooked canonical fallbacks precede Settings/.jce fallbacks and never
  override canonical source bytes. A manifest fills absent roots; the manager
  forwards its actual prepared-root override and Dist skips the legacy separate
  input append. Actual host conflict/closure, optional absence and selected-file
  disappearance controls are separate from native settings/pixel evidence. The
  omit-roots control mutates the actual shared adapter's root bindings; a
  separate project-settings omission control catches colour-space and other
  authored runtime settings disappearing from the selected graph.
- `jce_dist_content_graph.{h,cpp}`  — UI-free Bundle/Dist parity bridge: builds the startup-scene dependency graph in-process, extracts its cooked bytes, and preserves the approved graph snapshot for Dist audit.
  The build manager passes the admitted SDK root to this private bridge. Its
  stock-shader resolver follows the installed tool's `--shader-root` address
  policy (normalized relative `shaders/*.bin` only) and is the public packer's
  final fallback after its existing VFS/project reads. SDK roots are normalized
  through public current-directory/join/path operations, as in the SDK tool,
  before the callback exposes an absolute host path; resolution/overflow failure
  rejects graph creation. Relative UI SDK paths remain supported; headless graph
  creation does not require a GUI-mounted stock VFS. Cooking/platform/graph
  ownership stays in the public Bundle packer. The first actual producer ERROR,
  including its dependency chain, survives in the build result; owner-thread
  failed completion writes one common ERROR witness in DIST/headless builds.
  The existing graph fixture covers absolute/relative SDK fallback, project-byte
  precedence and missing-stock rejection with real host files and Bundle/Archive
  operations. Fixed shader-byte controls explicitly select the Windows profile
  on every host; those bytes do not certify shader parsing or GPU execution.
- `jce_run_manager.{h,cpp}`         — launches built binaries / Play-mode subprocess.
- `jce_editor_script_backends.{h,cpp}` — registers the language VMs this editor was LINKED with, and owns the project's native (C/C++) script modules. Two reload entry points and they are not interchangeable: `jce_editor_script_modules_reload()` runs from the project-open seam and SKIPS a path it already holds, so calling it twice on one project is a no-op; `jce_editor_script_modules_reload_native()` is the explicit File-menu command, which refuses by name while any script instance is live, then unloads everything and re-enters the first one. Proven to pick up a rebuilt image rather than re-attach to the resident one (`tests/scripting/cpp/test_jce_script_vm_cpp_modules.cpp`).
- `jce_pak_key.{h,cpp}`             — emits the two XOR shares of the project asset key as a C translation unit. The engine default (`engine/src/application/jce_pak_key_default.c`) and the CLI emitter (`scripts/jce.py`) write the same two symbols; `tools/lint/check_pak_key_shares_contract.py` holds all three to the same names and the same 64 bytes, because a drift there links fine and decrypts nothing at a customer site.

## Rules
1. **All editor state goes through this folder** — no globals in `panels/`, `dialogs/`, etc.; they take pointers/refs to state owned here.
2. **C++20**: follow the root language contract. Prefer portable facilities such as `std::filesystem`, `std::string_view`, and `std::optional`. Do not require `std::format`; standard-library availability must preserve the supported runtime floor.
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

Operation metrics retain their existing main-thread phase buckets and also use
the private `jce_editor_cpu_scope.h` adapter to the public canonical trace ring.
Static scope names, one synchronous B/E pair and caller-task restoration do not
prove a whole application lifecycle. Normal return is the default outcome;
known Play capture/create/restore failures mark FAILED. `editor.play.request`
ends while layout can still be pending; `editor.play.begin` and
`editor.play.stop` cover their separate synchronous owners. Trace capture/reset
must retain the existing quiescent-boundary contract.

`jce_editor_viewport_trace.h` owns private decision enums and CPU samples.
The render span counts calls; `.admitted=1` marks passage of the original early
guards; exactly one final `.reason` can report a later target/resource refusal.
These samples are not cumulative counters or drawn/presented-frame witnesses.
The existing viewport-work policy remains authoritative and its traced overload
only explains admission. The CPU trace adapter does not allocate, log or read a
CPU clock when disabled; independent enabled phase collection keeps its clock.

The existing visible update invokes modal probe maintenance after NewFrame and
before layout; it stays outside pre-render CPU services. Whole-scene replacement,
accepted history clear and state shutdown cancel before releasing/rebuilding
scene content. Scene pointer stability during a clear is not capture continuity.
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

The Benchmark host consumes jce_scene_stress_spawn/motion. It owns history,
isolation and hierarchy bookkeeping; fixture/material/physics generation belongs
to the shared C99 scene diagnostic API, not a duplicate C++ implementation.

Project render/physics/pipeline configuration export precedes collecting the embedded PAK inputs; writing only in the package finish step makes shipped settings one build stale.

D01 operation observations use jce_editor_operation_metrics and the existing
main-thread phase accumulator. Inspector sync requests have generations;
coalesced/invalidated requests are not successful draw completions. Explicit
JCE_EDITOR_OPERATION_METRICS enables log observations. JCE_DBG_RENAME and
JCE_DBG_REPARENT use source:target@frame (ROOT detaches); JCE_DBG_SCROLL uses
entity@panel-frame and the existing reveal/clipper path. These maintenance
controls measure programmatic operations, never physical input-to-display.
Loaded entity count, flattened rows and display capacity remain distinct.

JCE_DBG_SCENE_LOAD_SYNC=1 opts maintenance runs into the existing synchronous
startup scene opener before UI measurement frames. Default interactive startup
retains time-sliced opening. This is setup policy, not an alternate loader or
permission to operate on an incomplete scene; load wall time is not UI latency.

Rename uses the existing entity-scoped history snapshot: EditorMeta name is
serialized/restored with its component set and component order. Reparent remains
full-scene history because scoped records do not yet preserve parent relations.
JCE_DBG_RENAME_VERIFY=undoTick,redoTick observes actual identity/name/parent/
focused selection/population after existing Undo/Redo hooks; it restores nothing.

Undo and Redo capture inverse records with the target record's granularity.
An entity-scoped target must produce an entity-scoped inverse, preserving live
handles and selection across both traversals. Full-scene records retain their
existing scene-recreation and ID-remap path; failed capture leaves stacks intact.

History identity is private, session-scoped monotonic tokens bound to complete
ECS handles. Full snapshots preserve complete serializer bytes but retain only
already-tracked source-ID/token pairs; scoped edits allocate tokens lazily. Full
restore accepts a unique tracked subset of serialized source IDs, including an
empty subset, and observational logs never create identities. Inverse full
snapshots retain newly tracked tokens before an older full restore drops them; successful restores rebind tokens to the loader creation roster. Entity
records resolve tokens, never names or recycled indices. History clears advance
an epoch without reusing tokens; retained Play snapshots from an earlier epoch
receive fresh bindings. Invalid/duplicate rosters are rejected before scene clear.
Roster storage and conservative map-node/bucket estimates count toward the
existing history budget; full capture prunes dead live bindings. Maps are not
allocator-exact accounting. Nested cross-entity scoped edits cannot be promoted
after mutation: rename fails closed; SDK-style manual callers must use an outer
full-scene batch for mixed entities. That void begin/end contract cannot prevent
callers from mutating after a refused begin; do not claim arbitrary nested safety.

Identity restore prepares map nodes, buckets and extraction capacity before
clearing the scene; commit rekeys all extracted nodes without new allocation.
A pre-clear refusal reports no mutation, so Undo/Redo preserve the target without
redundant rollback. The bounded diagnostic identity budget is honored only with
operation metrics enabled; default capacity remains 64 MiB. Failed Play/Stop
restoration retains its snapshot for Stop retry and refuses a new Play that would
overwrite it. Existing engine loader failure rollback remains necessary; this
does not assert complete allocator-failure atomicity of the scene loader.

Full history capture and restore walk entity JSON with the public first-child /
next-sibling facade. Indexed access traverses the underlying linked list and
made four full-array passes quadratic at large scene counts. Preserve source
order and loader creation-index correspondence; do not replace identity checks
with names or drop count/roster validation to accelerate restore.

Scoped snapshots preserve a captured-parent marker and a logical parent token
(root uses zero), alongside the existing local component JSON and component set.
Parent identity is checked alive and cycle-free before mutation. The private
UI-free history entity service calls public reparent with preserve_world=false,
then applies the exact recorded local components. Inverse capture and change
comparison include parent identity, so a parent-only edit cannot disappear.
Full-scene batch semantics remain unchanged. No stable sibling-order guarantee
is added: current public children iteration and full-scene history do not retain
a sibling ordinal. JCE_DBG_REPARENT_BATCH=1 exercises the existing public outer
batch in maintenance mixed full/scoped tests; ordinary reparent timing is scoped.

Operation observations are owned by EditorInternalState::operation_metrics on
existing state s. Inspector generations/pending requests, maintenance frame count,
rename witnesses and one-shot observation flags reset with the normal editor
state lifecycle. Do not introduce independent observer globals or hide them in
function statics. Diagnostic observers retain ordinary UI/API routing and cannot
mutate identity/history by an alternate path.

- `JCE_KPI_GAME_EVENTS` is an explicit normalized QA event producer in editor main. Pointer rows use the existing virtual Game bridge; keys use the normal event processor. `JCE_KPI_GAME_EVENTS_CLOCK` absent/empty/`frame` retains visible-Playing frame scheduling, while explicit `elapsed_ms` selects monotonic milliseconds. Optional `JCE_KPI_GAME_EVENTS_START` is an initially absent absolute host marker path for elapsed mode only. The existing GUI host snapshots these options once; it never forces runtime state, focus, hover or capture and is not physical mouse validation.

Script quit requests are consumed after runtime callbacks return, in Play or
single-step: they call existing Stop and never quit the editor process. Optional
runtime platform services are owned by the grouped Play runtime host only when
trusted JCE_RUNTIME_STATS_DIR is explicitly configured. Destroy runtime before
checked flush/shutdown; no config means unavailable and no duplicate LOCAL store.

## Play failure atomicity

Play captures a complete restore point before constructing audio/runtime state.
Capture failure refuses simulation; constructor failure tears down owned services
and restores authored history. A failed restore retains the restore point for
Stop retry and blocks another Play. The private transaction helper is used by
production and headless failure-sequencing tests. Existing operation metrics
separate capture, create, teardown and restore phases; no new global tracer.

Explicit Game event fixtures record actual viewport owner/hover routing at
direct INFO level so Dist validation retains this witness. Normal sessions emit no
fixture records; this diagnostic does not force focus or gameplay state.

## Backbuffer screenshot completion

`jce_editor_screenshot_completion.h` owns one bounded main-thread request in
`s_editor`. All editor backbuffer submissions share it. Scene/Game texture
completion channels are distinct, but submissions share the same renderer
file-slot lease. Acceptance logs submission only. Application poll terminal
states notify the owner once: interactive success follows actual file
writing; maintenance requests log completion/failure. Shutdown drains only
already queued CPU PNG writes through the existing public renderer flush before
its final completion poll. Undelivered GPU requests and callbacks not yet queued
remain explicitly unresolved; this neither closes backend admission nor waits
for GPU delivery. The headless `test_jce_editor_screenshot_completion` compiles
this helper plus the actual complete poll and shutdown-observer block. Its
missing/late-flush controls must fail ordinary unchanged assertions; the separate
real Renderer PNG fixture covers CPU drain, continued admission and pending GPU
requests. These controls do not certify GPU completion or file durability.

Backbuffer, Scene View and Game View share a renderer file-slot lease in
`s_editor`. A pending or unconsumed terminal owner blocks every other editor
submission. Only the matching viewport poll may consume a legacy result;
backbuffer completion notifies once before another request can acquire ownership.

`jce_editor_event_fixture.h` decodes explicit maintenance input rows into public
`JceEvent` payloads, without focus or input-owner overrides. M/B/W retain their
motion/button/wheel mappings; K uses integral scancode 1..JCE_KEY_COUNT-1 and
exact down 0/1, with no modifiers or repeat. x/y are ignored for K. Invalid
used values or frame numbers emit no event. This is QA software input, not a
hardware-input claim; the application retains Play/viewport scheduling.

`jce_editor_event_fixture_schedule.h` owns the capped one-read event buffer and
optional elapsed scheduler in `g_game_events`, reset by the existing GUI host
initialization/exit lifecycle. The default frame path adds no clock reads or
marker polls. Elapsed time uses the public performance counter/frequency
independently of simulation dt; equal
counter readings are legal and a backwards reading or zero frequency refuses
the fixture. With a start path, the file must be absent at initialization and
the first eligible admitted visible Playing runtime-owned viewport poll. Waiting
polls are at most 10 Hz, retaining their gate across invalid admission so rapid
hide/show transitions cannot increase the rate. A return to valid admission may
wait until the next eligible poll. A regular file starts the origin at its
observing poll.
Before activation, docked/Max geometry and runtime changes remain admissible
while the marker is absent. A marker present at that eligible poll after an
observed invalid interval refuses activation. After activation, Stop/Pause,
hidden/invalid viewport, runtime identity/owner or display/render geometry
change cancels the remainder and clears virtual state; reopening does not
restart it. Elapsed-only `validate_active` also checks the current runtime and
geometry before the ordinary pre-step commit. Cancellation observes normal host
update/draw boundaries and the existing runtime pointer; there is no Play
generation. A Stop-to-Play cycle that reuses the same pointer entirely between
observations is outside this evidence. EOF/error retains the existing next-draw
clear after the last row's ordinary pre-step runtime commit opportunity, without
a commit acknowledgement; zero real dt can skip that commit. The retained row
remains pinned to its admitted runtime/geometry after the file buffer is freed
or decoding rejects a tail; the actual pre-commit guard still validates it.
Admission loss clears retained state immediately; normal drain retires the
active/pending admission so later ticks do no fixture clock work. Native input
ownership stays with its bridge.

Validated scene startup restores a declared authoring project only when an
actual scene/Bundle restore is selected. Bind it after editor state, VMs,
panels and managers initialize and before project settings warmup or scene
loading. Reuse set_current_project_root's existing full transaction; never
replace it with a root-string write or a second native-module loader. Empty,
picker and forced-demo startup retain their existing policy without a restore.
The stateless jce_editor_project_context.h/.cpp helper uses public project
parse/find-root and host path APIs. It validates an exact canonical directory
or manifest, rejects malformed/missing/oversized input and retains no globals.
Bundles preserve a valid independent authoring context; ordinary source scenes
select their own nearest project. Path bounds remain explicit; a fixed ancestor
depth is not a relocation contract. CPU controls live in
tests/editor/test_jce_editor_project_context.cpp (headless, build-owned fixtures).

## Native project target admission

`jce_project_build_target.h` owns canonical native architecture names and host
defaults used by the native build manager and Build Profiles prediction.
Unsupported targets fail before asset preparation or build-cache mutation.
Windows ARM32 cannot silently use the x64 compiler; Linux cross targets need
an explicit supported cross toolchain. macOS configures the selected native
compiler architecture. The SDK metadata admission below validates the selected
archive declaration; neither check proves linked artifact architecture, relocation
or a cross-platform shipment.

`jce_build_package.cpp` owns native package filename/path admission and the
reports candidate -> audited single-file publication. Unknown output entries
are preserved and rejected; publication failure must preserve a previous complete
application. `jce_project_build_target.cpp` owns artifact naming and selected
SDK metadata admission. Read only lib/<CONFIG>/jce-sdk-target.json with the
public capped host reader (16 KiB max, not stat then unbounded allocation)
and require the matching archive, system, architecture, pointer width, variant,
delivery and CRT. Reject missing/malformed/duplicate or escaped generated metadata before cooking,
cleaning or changing artifacts. VERSION.txt is explanatory and prefix names
carry no ABI authority. GUI debug explicitly uses authoring; release/dist
explicitly use embedded delivery, with a visible matching-SDK/refresh error.
The manager forwards that policy to CMake; never silently lower it to authoring.
Exact helper tests use real temporary host files, not GUI or compiler proof.

Headless receipt publication is part of build success: reset an owned stale
receipt before starting, reject an unremovable prior result, and propagate write
failure to terminal FAILED plus the public engine failed-quit path. Keep phase,
reason and actual result path in diagnostics. Do not bypass normal teardown with
process exit. Selected project asset roots also reach effective render settings
and the packager; the default-root compatibility arguments are not a substitute
for reading the current project's manifest.


`jce_build_tool_paths.cpp` is the editor executable-path adapter for the public
`jce_toolchain` discovery/override owner, with host PATH as a fallback. Tool
probing, direct spawns and native project queues consume the same resolved
paths; the queued CMake invocation pins the captured Ninja via CMAKE_MAKE_PROGRAM.
Native SDK builds require CMake/Ninja, while workspace builds also need Conan.
Missing native tools refuse before cooking or cache mutation. A bundled builder
can establish public toolchain overrides; this adapter does not invent a second
compiler installer, Python discovery rule or machine-specific path catalog.
`jce_project_build_target` also owns build root/output layout, initial target
selection and variant/tool admission shared with Build Profiles. The global
workspace target never overrides a user-project manifest. Successful Run path
updates use the manager's actual artifact status before any UI prediction.

Startup renderer requests consume the config loader filled value even when its source-found result is false. An absent profile is valid platform defaults. Apply the same request before engine/device creation that Preferences/Game View display; preserve explicit Auto, unknown-name refusal and environment/backend fallback. jce_editor_renderer_startup.h shares production selection with actual-loader controls; the old-found-guard mutation must fail on fresh profiles. CPU request identity is distinct from device admission and pixels.

Both GUI and headless Editor factories bind the engine INI to the validated
profile's `editor-runtime.ini` before engine creation using public
`jce_engine_set_config_path`. Resolve the shared profile root and validate the
joined filename within that setter's 512-byte capacity; no game INI is copied
or implicitly migrated. Existing profile runtime window/backend values remain
authoritative in their own engine config. Invalid, unavailable or overflowing
roots never invoke the setter: the descriptor selects headless and app init
returns a logged failure before Editor owners start. GUI renderer JSON defaults
and explicit preferences retain their existing request policy. Startup controls
use the actual config/path/host-file owner and counted setter port; the removed
binding-wire contract must fail ordinary runtime assertions. Real startup from
alternate game-config CWDs remains a separate windowed acceptance requirement.


`jce_editor_key_event.h` is the existing key-event bridge's ImGui submission
owner: queue each event's modifier snapshot before its mapped named key. The
existing scancode translation table remains in jce_editor.cpp. Actual-context
controls in test_jce_editor_hotkey_input consume real ImGui NewFrame/fast-input
trickling and the compiled hotkey registry; the no-mods runtime mutant removes
only this forwarding and must fail matching-chord assertions after compiling.
This does not certify OS input delivery, foreground focus or popup presentation.
`JCE_EDITOR_HOTKEY_DIAG=1` enables private observation only, capped at 32 log
rows per process. Relevant Ctrl/Preferences key events report raw scancode,
modifier snapshot, mapped key and active/initialized state. A pending key-down
allows at most one gate and one query row per frame for eight frames; it never
feeds another event or changes ownership/focus. Text, game/capture and modal
refusals remain authoritative. Query rows report actual current ImGui modifiers,
edge/chord match and prior chord consumption. No text, paths, credentials or
pixel payload is logged. Unset/other diagnostic values create no capture rows;
normal input dispatch and the font/render pipelines are unchanged.


Explicit hotkey diagnostics call public jce_log_write(INFO) directly, like the
existing shutdown diagnostic. DIST INFO macro stripping must not remove an
opt-in observation; runtime logger level and sink filtering still apply. The
private test_jce_editor_hotkey_input_dist_diag target defines JCE_DIST only for
its fixture/actual owner and verifies ordinary LOG_INFO is stripped while real
logger event/gate/query records retain their fields, duplicate suppression and
32-attempt cap. No global compile-min policy or public log ABI is changed.

Explicit isolated JCE_EDITOR_PROFILE_DIR confirmation (once per process) and
JCE_DBG_AUTOPLAY's actual state_play call confirmation use public
jce_log_write(INFO), preserving source and runtime logger filtering in DIST.
Default startup is not made verbose. Profile validation and Play/input ownership
are unchanged; these opt-in records do not replace actual profile bytes, window
observations, Play state or rendered output in acceptance.

## Hotkey rebinding ownership

Hotkey persistence reads at most 1 MiB through public capped host IO and strict
JSON. Root/array/entry types, finite exact integer keys and modifier bits must
pass admission before the live table is changed. Unknown action ids remain
ignored and a known row without key remains a no-op; absent mods means zero.
One bounded pending table makes malformed loads transactional. Setter, dispatch
and key labels share named-key safety: zero stays unbound, physical named
modifier keys remain valid, reserved modifier pseudo-keys and values outside
the named range refuse. Capture's modifier/Escape exclusions remain its own
policy. The real-owner persistence fixture consumes JSON, actual host files and
an ImGui CPU context, including shutdown/init reload and failed-save retry.
This is not fresh-process GUI or native workspace persistence qualification.

`jce_hotkeys.cpp` owns capture in its existing `HotkeyRuntimeState`, through
`jce_hotkeys_capture_begin/id/poll/cancel`. These are private editor interfaces
for the UI thread and active ImGui context. Panels must not store a capture
target or scan keys themselves. `jce_hotkey_pressed` suppresses every action
before edge/chord queries while capture is active and throughout its completion
or cancellation frame; ending capture does not release later consumers that
frame. A later fresh key edge restores ordinary dispatch. Poll commits once,
ignores pure modifiers, and Esc cancels without changing the binding. Shutdown
clears capture and per-frame consumption state. Duplicate chords retain the
existing conflict marker and first-query consumption policy.

`test_jce_editor_hotkey_input` uses the compiled registry and real ImGui queue
to cover dispatch-before/poll-first orders, completion/cancel barriers, held
keys, new edges and session reset. Its native CPU result does not certify OS
key delivery, modal focus/tab/close consumption, persistence or rendered UI;
those need the current editor artifact and isolated-profile GUI verification.

## Play presentation ownership

The existing `s_play_host` in `jce_editor_play.cpp` owns both its concrete
`JceRuntimeStatsHost runtime_stats` and private presentation state. Calls pass
`&runtime_stats` without casts; no new process singleton is introduced.
After successful scene capture, Play resolves the same effective render settings
and source/cooked roots as the packager, then applies authored VSync and an
Editor-only preview cap through public renderer/engine APIs before creating the
runtime. Missing or failed composition still begins the same FPS transaction
from initialized defaults and preserves entry VSync; no separate renderer query
or settings owner is introduced. The canonical `editor.frame_rate.play` preference defaults to 60;
0 removes only the Editor cap. Positive project FPS remains an upper bound:
two positive limits use the smaller value; project FPS 0 plus preview 0 is
uncapped. Native runtime settings and authored project/Bundle bytes are unchanged.
The private presentation state retains the original project FPS independently
of the applied rate. Live preview edits update the existing host policy's
running_fps; an owned temporary Pause/focus cap remains in place until release.
Unchanged effective rates do not repeatedly call the engine setter. Fixed-dt QA,
viewport capture exceptions and host invalid-pointer admission retain their owners. The existing boolean vsync consumer still does not implement every
second VBlank.
`jce_editor_play_presentation.h` captures the entry renderer/FPS state once.
Failed runtime creation restores it before scene rollback, including when
rollback retains a snapshot; normal Stop restores through the same idempotent
owner before runtime/view shutdown. The renderer is borrowed and must remain
alive until that release. NULL/fallback skips all bgfx-dependent vsync queries
and setters; the process-global target-FPS API is valid without an engine.
`test_jce_editor_play_presentation` covers this owner with counted public CPU
ports. `tools/tests/test_editor_play_presentation.py` also compiles the actual
capture/apply/create-failure/Stop callsite fragments and runtime negatives with
independent actual wires removed and the old unresolved-settings gate. These
are CPU consumption controls; actual
Editor/GPU pacing and visual parity require a rebuilt application capture.

Configuration persistence retains pending edits when either category publication fails. Both timed and forced flushes clear dirty state only after a successful two-category attempt; the timed retry observes the same quiet period to avoid per-frame errors. Isolated real-filesystem controls cover migration, category separation, full-width panel bits, debounce and failed-write recovery. A compiled old dirty-clear owner is a runtime failure control, not a GUI result.

Default-scene builders share `jce_editor_default_camera.h`: their unique Main Camera is primary; the general Add Camera default remains non-primary. Project creation selects the 2D orthographic or existing 3D default through `jce_state_new_default_scene_for_project`, leaving Bundle Preview and clearing prior scene/selection/path via the existing scene lifecycle. `jce_editor_new_project` continues only after the shared unsaved-change gate, reuses public template/pipeline/project owners, saves Main.scene and publishes startup_scene relative to source_assets. Failed stages keep explicit partial output without complete-success or recents. Empty game localization catalogs detach previous sources; the first successful catalog save reattaches the new host source. Malformed present catalogs remain diagnosed and are not advertised as available.

Default-scene replacement detaches the outgoing file path, scene directory, history and open transaction after Stop/streaming/Bundle teardown and before clearing or creating entities. A failed factory leaves an unsaved dirty partial scene; it cannot Save or Undo through the outgoing project identity. New-project completion additionally reads the public manifest back and requires its startup_scene and the active cached project/root to match. Successful publication followed by a lost cache reload remains an explicit failure.

`jce_editor_test_run` is the private Test Runner child owner. It uses public non-blocking process/pipe/exit APIs on the UI thread, with no worker or shell. A child remains owned through cancellation/timeout until observed exit and bounded pipe drain, then is destroyed once; editor shutdown uses the same common process reap. Fresh profile/build logs use public `jce_fs_host_write_new` exclusive creation, never exists/stat followed by overwrite. Existing destinations and incomplete write/close refuse spawning without replacing prior status/output/log identity; a failed creation may retain partial evidence at the diagnosed candidate path. The timeout is an observed-completion deadline: elapsed/clock-backward state is latched before exit polling, including late observation of an already exited child and outstanding pipe drain; no exact native child exit timestamp is inferred. Per-poll reads are capped at 64 KiB, retained child evidence at 1 MiB; overflow, write/footer failure, cancellation and timeout never PASS. Trace counters record requested test identity/state only; child wall duration is not host CPU/GPU timing.


## Authoring cadence

`jce_editor_cadence.h` owns only the EditorState authoring policy: default active
60 FPS, idle 30 after two monotonic seconds without input, background 5 when
actual public window flags report lost focus, hidden or minimized. The canonical
config ui_int keys `editor.frame_rate.active/idle/background/play` expose positive
rates and explicit 0 through Preferences; Play's 0 means no additional Editor
preview limit while project positive FPS/VSync still apply. Generation changes refresh the mirror;
no other config singleton, timer, sleep, renderer or presentation owner is added.
The one public engine target-FPS API still owns deadline waiting. Playing and
Paused suspend authoring writes, preserving the existing Play presentation
transaction; Stop resumes and exit releases the authoring value once. Generic
application default FPS and config VSync intent are unchanged. Opt-in
`JCE_EDITOR_CADENCE_DIAG` records at most 32 mode/rate transitions via the common
logger, including DIST; engine initialization reports the actual configuration
file once under this flag or JCE_PERF_LOG. CPU counted-port and real ImGui controls
are distinct from actual PresentMon, DWM/adapter-copy and GPU-cost verification.
The actual main/Play receiver has a missing-preview-cap argument control, and
the actual initial config/apply/create transaction has a separate missing-config
wire control. Default 60, explicit 0, positive project/preference limits, stable
setter counts and Pause/focus/Stop restoration must be checked independently;
CPU controls do not establish lower hardware load or desktop responsiveness.

The private pure `jce_editor_frame_rate.h` composes positive FPS bounds by their
minimum; 0 contributes no bound and only an all-zero combination is uncapped.
Authoring idle retains the active bound, and background retains the applicable
active/idle bound. Play composes authored and preview caps before applying its
Pause/background bounds, including the first lifecycle-policy begin. The same
config read supplies that initial preview and temporary-rate request. The Play
host retains raw temporary requests so a changed preview cap can be recomputed
without inheriting a previously lowered temporary target. Manual Pause applies
its bound before a focus callback can observe background pacing; Resume removes
that extra bound and restores the latest running composition. Generic Runtime
host policy and saved user values remain authoritative in their existing scopes.


Play owns `JceEditorPlayModule` inside the existing `EditorPlayHost`, independent
of Game View visibility. The actual engine services are borrowed for the session;
init attempts pair with one exit even on partial failure, before runtime/renderer
teardown. Custom update runs once after the runtime step; the stock ECS-only
module update is skipped because the runtime already advances that world. Draw
remains admitted viewport work. Stop, failed host/module construction and module
replacement use the same release; callbacks cannot resurrect an init torn down
reentrantly. GameRender delegates selection and drawing, owning no module lifecycle.

The same host embeds the implementation-only SDK `jce_runtime_host_policy.inc.h`.
It consumes the existing lifecycle focus/pause getters and one removable listener,
not a second event registry/clock. Authored foreground FPS/VSync remain unchanged;
Pause temporarily uses the idle rate, native focus loss uses the background rate,
then the exact running FPS is restored through the same public deadline owner.
`run_in_background=false` blocks simulation/audio while unfocused; true continues
unfocused simulation at the temporary cadence. Native OS pause/minimized still
uses the engine's existing no-render path. Resume discards one observed interval
rather than catching up inactive wall time. Manual Pause/focus audio ownership
shares one latch. Listener OOM refuses session construction; nested focus or Stop
cannot reapply an obsolete policy. Close detaches before runtime destruction.
No generic VSync every-second-VBlank support is claimed by this policy.

`jce_editor_host_services.h` groups only the audited Build/Run child pipeline,
Cook/scene-serial compatibility polls, asset-index publication, Test Runner and
the already initialized Console pending-ring drain. Console collection does not
require a dock draw, performs no ImGui work and is not added to headless polling.
`jce_editor_tick_cpu_services` consumes a pending Recorder format-failure Stop
on its serialized main owner, then flushes existing config/project-state debounce
stores without scene autosave/save hooks, ImGui or GPU calls. Hidden/minimized
UI refreshes its existing delta clock; visible UI keeps the full interval. The
main host measures its existing update counter once and reuses that interval.
Automation/AI completion, dialogs, scene load/watch, quality/font reload, Play
and video-capture initiation stay with their visible owner; their minimized
continuity and general background simulation are not certified by the child-service
controls. Recorder failure retirement is the explicit main-service exception: its
worker drain/join may block this owner until actual accepted work returns.

CPU service tracing uses static `editor.cpu_services.*` names and the public
canonical submit/begin/end APIs under the existing enabled flag. The stack
adapter restores the caller identity, adds no registry/thread/capture policy
and records no IDs or clock reads when trace is off. Poll completion means the
synchronous call returned, not child/job/build/file publication success. Run's
BUILDING delegation belongs to its Run span; a separate Build span is emitted
only for an actual direct Build poll. Recorder retirement and config/state flush
spans remain nested in maintenance. Existing real-body fixture controls cover parent/closure and
trace-off behavior; native/GUI acceptance remains a separate run.

Project-state persistence timed and forced flushes retain dirty state on failed atomic publication, resetting the existing quiet period before retry. The private forced flush returns the actual publication result; the root setter consumes it before replacing the single state table or published root. Failed A-to-B switches and project close retain A's dirty values and identity, reject oversized roots, and can retry without another setter. Dialog, scene-follow, restore and headless callers must refuse asset/project rebinding and success reporting on false. Shutdown logs unpublished state but does not block process exit. `tests/editor/test_jce_editor_pstate_persistence.cpp` links the actual owner and JSON/file writer and checks prior files, A/B isolation, refusal and retry. The existing ordinary old-clear-dirty mutation also makes switch admission incorrectly succeed, so these controls detect that failure without a second persistence owner. The activation fixture separately checks that the actual dialog activation body stops before asset repoint on refusal. Typed activation ports are not GUI proof; atomic publication is not universal power-loss durability.


Isolated RenderSettings composition consumes the current public active filesystem
only after ISOLATED policy admission, then uses public open/size/read/close and
the shared engine JSON-memory loader. The caller rejects sizes above one MiB
before allocating or reading its copy. PAK-backed open already eagerly decodes
the entry; this limit is not a total VFS/decompression memory cap. It never uses
the host convenience reader for this branch. A missing filesystem, mismatched policy, missing/empty/oversized or invalid
JSON leaves the authored file half unresolved; active project quality still owns
its existing overrides. HOST project source/cooked selection is unchanged. The
legacy reader callback can be absent while actual mounted VFS bytes remain valid;
callback availability is not a separate content-admission requirement.
`test_jce_editor_render_settings_vfs` uses the real Archive writer, PAK, VFS, JSON
and host file owners with two conflicting actual working directories and active
filesystem replacement. Only the project-quality snapshot is a counted port.
The old-host-read contract restores the actual isolated dispatch to the prior
host loader and must fail ordinary assertions; it does not imply normal isolated
Bundle reads previously consumed CWD files. Native CPU evidence is separate from
actual Bundle Preview/Play pixels and GUI project switching.


`jce_editor_viewport_work.h` synchronizes public window INPUT_FOCUS/HIDDEN/MINIMIZED
observations into ImGui's existing AddFocusEvent queue immediately before the
visible `jce_editor_update` NewFrame. Startup need not have received a focus event;
EndFrame clears ImGui's per-frame AppFocusLost, so a successful native query is
repeated for every visible UI frame. Unknown window flags leave current IO and
queued events untouched; they do not prove retained OS focus across frames.
ImGui owns same-frame deduplication. This is not part of CPU-only should_quit
services and adds no cache, event registry or public API. The existing positive
layout/capture gates remain; explicit capture may override known loss for a
positive viewport. The actual ImGui CPU fixture consumes the real pre-frame
callsite, with counted public window-query ports and an ordinary missing-wire
runtime negative. It does not certify OS focus, window occlusion or GPU work.

FrameLog's existing viewport capture override remains the default. An explicit
`JCE_KPI_FRAME_LOG_VIEWPORT=natural` removes only FrameLog's own override and
uses the unchanged positive-size, ImGui focus and clip gates. Unset, empty and
`capture` selectors retain capture-default. Other values decode as Invalid but
retain the old forced load; they must not be certified as a natural run. Active
Game capture and DrawLog retain their existing precedence. Replay and scheduled
backbuffer shots gain no independent force route. The stateless private policy
adds no frame service, allocation, logger or second CSV writer.

The existing real ImGui viewport-work fixture covers selector/environment
admission and actual focus/clip results. Its missing-frame-log-policy contract
removes only the production selector wire and must fail ordinary assertions.
Physical viewport reachability, rendered workload and complete wall CSV need
separate current Editor acceptance with an explicitly recorded natural selector.

The authoring cadence and Play host share the same optional saved background UI field. Missing/invalid values derive STOPPED background 5 Hz and legacy Play background 15 Hz in the existing cached rates value; valid saved 15, 0 or other supported rates override both. The actual main call uses `play_background`, while the existing Play presentation/host policy retains authored settings, composes the private preview cap and restores the resulting running FPS after temporary pause/focus pacing. The cadence native fixture consumes the exact main call, exact Play receiver and real lifecycle registry with typed FPS/audio ports; the old-shared-background contract mutates only the actual main argument and must fail ordinary assertions. This proves config/policy routing, not OS focus, presentation frequency or GPU power.

`jce_editor_project_context.h` owns the stateless identity comparison for normalized authoring paths: Windows ignores case, POSIX compares bytes. Project admission and scene-follow use it consistently; it does not normalize paths or resolve symlinks. Keep platform-case controls with the actual public project-context fixture.

Saved GPU vendor preference uses the existing `editor.gpu_adapter` UI-int key
(0 Auto, 1 Intel, 2 NVIDIA, 3 AMD), consumed by the same startup config request
owner and actual main wrapper before device creation. A nonempty per-launch
JCE_GPU_ADAPTER takes precedence without changing the saved choice. An absent key never
calls the public setter, retaining JCE_GPU_ADAPTER/driver policy. Unknown stored
values warn once per startup attempt and retain that policy; they neither select
Auto nor refuse normal startup. Explicit Auto is a real vendor-zero request.
Common saved-request logs remain distinct from the renderer's actual selected
vendor/device and fallback identity. Actual config CPU controls plus strict main
wiring admission and an old-setter-wire runtime negative cover request plumbing,
not physical adapter selection, copy traffic, pixels or memory differences.

Game View publishes its existing ImGui cursor/focus snapshot through the private
game-input viewport POD. The bridge primes only a missing runtime position while
focused, hovered and uncaptured; accepted native positions and button/delta/wheel
state remain authoritative. Native focus gain/loss is applied before the next
panel publication so a returning first click is admitted and held input is
released immediately. Capture, hover/owner loss and invalid-layout controls stay
with this same owner. The actual bridge fixture covers hover without a second
event, dimensions committed before the first click, native-edge precedence and
focus recovery; its build-local missing-prime wire must fail ordinary assertions.
Typed cursor snapshots prove the bridge contract, not native OS focus or pixels.

Play viewport admission uses `jce_editor_play_viewport.h` in the existing
EditorPlayHost. Play requests coalesce and consume only after Game View publishes
actual layout; Stop/scene replacement cancel a pending request. The original
creation body runs once, after presentation settings and before drawing, using
sized Runtime creation options. No guessed screen default, second runtime or
synthetic pointer is introduced. Host extent persists independently of focus,
hover and input expiry; changes dispatch presentation reflow without a simulation
tick. The shared pipeline extent resolver supplies Canvas/FBO units. The actual
request/publication adapter fixture uses typed CPU ports and explicit missing-
begin/extent variants; it does not certify physical UI, GPU pixels or native DPI.

Native build request admission runs before nullable field checks. Idle refusals
publish FAILED with stage NONE, exit -1 and fresh diagnostics, removing prior
artifact/package/BOM/audit identities and queued finish work. Busy process or
asset-preparation reentry logs a separate console refusal and preserves the
in-flight state, error, output identities and transcript. Editor-private
`jce_build_manager_reject_request` uses the same owner for GUI preflight refusal;
no public SDK ABI changes. Play must consume dispatch acceptance before entering
BUILDING, so rejected requests cannot launch the previous configured executable.

Play mixer descriptor selection uses the public conservative presence owner on
its generated absolute project and legacy-profile paths. Only ABSENT selects the
next optional source; PRESENT and UNKNOWN retain the actual requested read and
its failure diagnostics. Confirmed absent legacy content leaves the descriptor
NULL so Runtime applies its existing default-source policy. Generated profile
strings alone are not authored config, and failed path resolution is not a valid
descriptor. This does not change Runtime explicit-path or Bundle read semantics.

Play camera admission is inside the existing creation transaction, before
Runtime creation. Failure uses its same teardown and scene rollback; successful
Stop detaches Runtime before releasing the separate Game Play camera, and the
stopped snapshot-retry branch is idempotent. Neither hidden Game panels nor a
failed scene rollback can retain the Play camera. The stopped preview remains
its own complete camera object, rather than a partial pose snapshot.

## Native authoring module images

The existing `jce_editor_script_backends.cpp` owner holds each original absolute
module path as identity and a separate same-directory `.jce-image-` lease.
Public `jce_fs_host_copy_file_new` exclusively creates and completely closes
the image before the shared C/C++ registry loads it. Copy/load failure never
falls back to the mutable build output. A live-instance unload refusal retains
both handle and image; only successful unload permits deletion. Failed deletion
retains a closed lease in the same list for a later reload/close retry. Ordinary
same-project reload keeps its existing image even after the source is rebuilt;
explicit reload consumes the newer source only at the established safe point.

Normal `jce_editor_shutdown` releases state/instances, then closes module leases,
then releases i18n. No forced unload bypasses the registry refcount. This isolates
the primary authoring DLL, not every shared dependency or a concurrent compiler
snapshot. Generic library loading and project manifest identities remain owned
by their existing services. The actual Editor Build/Play loop requires separate
current-product validation beyond the real Windows loader fixture.

## Same-interval Game diagnostics

The core asks the public Engine observation seam during app.draw, resets the
private Game observer before layout, consumes the optional public ImGui draw
observation after Render and reports one copied source POD before returning.
The default draw still runs exactly once without queue inspection. Main-view
submit counts describe CPU calls; screenshot redirection can target an FBO. No
CPU observation establishes backbuffer presentation, GPU completion or pixels.
The context-v2 path uses the existing bounded FrameLog writer and completed
interval owner; no later live state is joined to an earlier frame.

## Recorder raw-video ownership

`jce_editor_recorder.cpp` owns one 12-slot total in-flight raw-video budget.
Reserve under the existing queue mutex before node/payload allocation and copy;
queued, worker-detached, first-frame seed and current encode buffers keep that
credit. Free the actual payload and node outside the queue lock before returning
credit. Congestion now drops an arriving capture when this whole budget is busy,
even if the worker already detached the queue. Normal codec/fps/bitrate/audio and
codec lag are unchanged; this bounds Recorder raw-video buffers, not codec/PCM,
GPU readbacks, whole-process memory or latency.

One process-lifetime public-I32/TLS admission owner remains stable across sessions
because renderer sink unregistration has no join contract. Callbacks borrow it
before touching session state; Stop closes admission, disables producers, waits
actual borrows without a timeout-and-free fallback, then drains/joins the worker
before destroying session locks. Main-thread lifecycle calls refuse callback
reentry. Start opens only after full initialization. The small fence is retained
once, not allocated per recording. This is not source/GPU retirement: old-source
screenshots arriving after a new Start have unknown generation. Provider failure
releases Recorder-owned nodes; opaque encoder create/finish exceptions cannot
certify internal encoder retirement through the current public API.

Stop retains a main-owner stopping latch across destruction and final logging;
reentrant Start/Stop cannot replace or duplicate the retiring session. Publish
inactive after the worker join, before synchronization destruction, so completed
counter queries from synchronous destroy/log observers never lock retired state.


Recorder locks width/height/pitch/yflip on the first valid admitted video
reservation. Each owned frame carries that immutable geometry and its declared
byte count. Validate width multiplication, row pitch, complete storage product,
last visible row and byte extent before allocation/copy and again before codec
creation/push. Invalid geometry or a later format change drops the arriving
frame and requests Stop; capture/audio callbacks never perform main-owner Stop.
The ordinary initialized CPU-service tick consumes that request, including when
UI rendering is skipped. Close admission, wait accepted borrowers and drain
valid old-format frames before reporting one diagnostic at the joined-worker
boundary while the stopping latch and mutex still exist. F9 can then start a
fresh session at the new size. No resizing/scratch image, codec reconfiguration,
quality/fps/bitrate/Opus change, public SDK ABI or GPU fence is introduced.
Declared extents cannot prove the capture provider owns the bytes it declares;
source callback generation and native/GPU retirement remain separate contracts.

## Canonical replay pointer consumption

The normal app-update calls Game input bridge sync_replay once before its
existing pointer/position/touch/action/runtime-step commit. Only the public
current-snapshot replay-origin query admits a JceInput capture; getenv is not
evidence and bad paths/EOF cannot authorize live input as replay. No replay
means no 2336-byte capture, allocation, worker or new clock. Replayed raw window
points use the existing explicit virtual-pointer state and render/display map
once, exactly like ordinary JceEvent points. Do not add OS DPI scaling, mutate
ImGui/OS cursor/focus/hover, or merge accumulated native deltas into replay.

The private sample distinguishes replay from other explicit virtual fixtures.
Existing visible/runtime/finite current geometry and inside-point admission,
non-consuming Canvas peek, transient drain and held-button behavior remain.
Sync is once per app-update; repeated commits drain displacement/wheel rather
than replaying them. A retired origin clears only its replay source; ordinary
native and explicit fixture routes keep their existing admission. Runtime
script-host CPU tests do not certify physical dragging, native pixels or GPU
completion.

## LAN discovery client maintenance

`jce_editor_lan_discovery.cpp/.h` owns the Editor's lazy client Refresh, CPU
maintenance and terminal stop/clear through the public LAN API. Initialization
opens no socket. The initialized `jce_editor_tick_cpu_services` owner updates it
once per ordinary GUI host interval, including hidden/minimized/non-drawn UI.
Shared panels shutdown retires the client before other UI owners; repeated
shutdown and a never-started service must not borrow an uninitialized socket.
The transport remains the sole scan clock/socket/result owner. Session and LAN
server lifetimes stay with their existing owners. Native controls compile the
complete service and transport with typed ENet/clock ports; they do not prove
real UDP broadcast, OS UI state, firewall behavior or native process retirement.

Editor pointer submission remains in jce_editor_process_event. Recognized,
uncaptured button events queue their own host-window x/y before the button
edge, using the same coordinate domain as motion without panel/desktop offsets.
A prior motion is not required for either press or release. Actual Game mouse
capture still refuses both UI coordinates and button edges and keeps its
existing gameplay button destination; bridge admission does not consume UI.
Retain ImGui's default fast-input trickling. Complete-owner genuine ImGui CPU
controls and an ordinary old-position mutation cover missing motion, stale
other-widget position, same-pump DOWN/UP, outside release, capture/release,
unknown buttons and inactive initialization. They do not prove native event
delivery, multi-window platform support or a particular desktop click failure.

The measured launch-only `JCE_DBG_OVERVIEW` and `JCE_STATIC_BATCH_BAKE` selectors
are presence booleans in the existing `s_editor` lifecycle, with their one-shot
counters. `EditorInternalState::operation_metrics` snapshots the presence of
`JCE_EDITOR_OPERATION_METRICS` on its first observation and resets with normal
state init/shutdown. Neither retains borrowed environment storage. These do not
cache project/RP/CVar policy or the dynamic next-view `JCE_BATCH_NOCULL` selector.

DrawLog separately accepts `JCE_KPI_DRAW_LOG_VIEWPORT=natural` through the
same strict private parser. It removes only an active DrawLog's capture override;
missing, empty, `capture` and invalid selectors keep the legacy capture load.
Natural FrameLog cannot opt DrawLog out, nor can natural DrawLog opt FrameLog
out. Active Game capture still wins. Positive finite layout, real ImGui focus
and clip gates are unchanged; no draw-stat query, CSV writer or cadence change
is introduced. A natural selector is admission metadata, not a GPU work witness.

DrawLog appends `gpu_stats_available` after its original nine CSV fields. One
existing `jce_renderer_get_gpu_stats` call supplies both the count and this bit;
no retry or second query is added. Failed observation preserves the legacy
numeric zero but marks availability zero, distinct from a successful real zero.
Counts remain latched whole-frame values including Editor UI. Shared RQ/cull
snapshots are not a same-frame/viewport join, and this bit supplies no backend
frame identity or GPU execution/presentation witness. Default-off, real header
write failure, row limits and explicit one-shot capture retain their owners.

Fresh ordinary-project build/run overlays start with neutral executable,
working/output paths and presets, simulation mode and the manifest target.
Only a missing per-project profile takes this path; existing custom profiles
and first-time engine-workspace migration retain their prior semantics. Run's
input gatherer resolves ordinary-project output_exe via the existing shared
native artifact-name/layout owner. Explicit external paths and target overrides
still win; ordinary-project fallback does not search engine-workspace outputs.
The pure resolver owns ordering only. CPU profile/gatherer controls exercise
real owner bodies and ordinary old-seed/missing-layout/old-name failures; they
do not certify saved disk IO, GUI state switches or the spawned executable.
