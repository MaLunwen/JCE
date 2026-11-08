# editor/src/panels/ — ImGui Dock Panels

Project Settings Quality offers only the renderer's supported VSync requests:
Off and Every VBlank. Legacy positive counts display as enabled, but opening
or refreshing the panel must not normalize stored values. Only an actual
selection change writes zero or one through the existing dirty/save owner.
This control does not promise alternate-VBlank synchronization.

Asset Browser scene activation delegates content binding entirely to the normal
scene loader. The grid must not reassign the scene directory from the file's
direct parent after loading: the loader owns the shared Scenes-parent rule and
an unsuccessful load retains its previous content base. Actual whole-entry CPU
controls live in loose_scene_content_controls.cmake; file/viewer/directory ports
do not certify native double-click delivery or complete scene deserialization.

Console output keeps the two existing bounded log rings authoritative. Its CPU
viewport controls compile the complete Console and shared ring owners with real
ImGui layout. Full-history filters, collapse, selection and copy are independent
of visible-text submission. Preserve every original Selectable and context-menu
submission so native ImGui keyboard navigation can reach offscreen records.
Derived intrinsic text bounds belong to ConsoleUiState;
font/context/size/spacing/DPI and exact text changes invalidate it. Mid-row Clear
must leave that draw's borrowed records and layout alive. A rejected output child
still has matching EndChild, while the existing sink drain remains outside the
output-body admission. Ordinary full-Text, ignored-child and early-clear controls
must fail actual assertions after compilation. These controls do not establish
native interaction, dynamic pixels, GPU work or complete-scene frame gains.
The font key consumes ImGui's existing FontId and atlas identity counters as
well as context, current point size, spacing and viewport DPI; never hash the
glyph atlas each frame. Output clipping retains the full multiline height and
maximum horizontal width. Real ItemSize owns each row's fractional-pixel
rounding; do not accumulate an independent Y prefix. Clear invalidates derived
geometry but does not destroy its active frame snapshot; the next admitted draw
rebuilds it. The actual same-owner full-Text CPU reference must match fractional
row Y and ScrollMax, and actual keyboard Home/End/Page events must retain reach.

Console subscription starts through the shared panels-init owner before project
restoration, independently of whether its dock is drawn. The existing GUI CPU
host poll drains its bounded pending ring into the same display ring while the
dock is hidden; it performs no ImGui work and cannot initialize after shutdown.
Headless membership stays unchanged. Filter and Clear-on-Play preferences retain
their meanings; log collection must not silently override user visibility choices.
This observes logs emitted after subscription, not earlier Engine boot history.

Console shutdown belongs to the shared panels shutdown sequence. Disarm the
static, null-userdata command callbacks, then use the existing logger's
synchronous sink-removal barrier before destroying the hand-off mutex. Clear
the output hook, destroy the session and release the existing UI owner's cache.
The core command registry has no unregister operation; these executable-resident
callbacks must remain inert until Console reinitializes. Do not tear down the
core registry or introduce a second logger owner. Missing-owner-wire CPU
controls must fail after actual panels shutdown, including repeated shutdown
and reinit; they are not proof of in-flight native logger races or GPU release.
Owned shutdown publishes the existing Editor CPU scope
`editor.console.shutdown` and Core trace counters for sink/session/mutex/cache
ownership before detach and after each real release. Repeated empty shutdown
emits no synthetic cleanup. The CPU fixture consumes the actual public bounded
trace ring, verifies parent restoration and balanced work, and keeps these
observations distinct from heap completeness, GPU completion and native races.

Render Pipeline content and its existing tick refresh the current requested
descriptor before immediate widget edits. Never seed authoring controls from
the hardware-clamped effective descriptor or retain a prior project's policy.
Path/status drafts remain local; accepted descriptor edits still publish in the
same draw. The applied-policy summary queries the effective descriptor once
after those edits, displaying authored/applied HDR, TAA, cascades and scale.
This local read-only snapshot must never enter authoring state, Save or apply;
applied policy does not certify an active shader pass or GPU execution.
All four authored/applied values use normal text contrast; read-only data is
not a disabled widget and its display style must not change policy ownership.
Dedicated actual-body CPU controls use real RP/Bundle ownership and
typed UI observations; they do not certify physical widget input or GPU output.

Profiler CPU phase rows show independent, possibly nested wall spans. Keep the
legacy app_update aggregate distinct from its app_simulation and app_draw parts;
never sum rows for a total or normalize percentages by that sum. Bar lengths are
relative only to the longest visible candidate span, not a frame budget or a
bottleneck verdict. The localized scope note explains this in all shipped locales.

When maximized Game View covers the main host viewport, only CPU Profiler
presentation returns early. Keep the CPU tab's phase capture, stale-history
reset and one current-frame sample before that gate. Detached Profiler windows
still present normally; other Workbench tabs retain their existing maintenance.
The CPU control consumes the actual draw prefix and six ring reductions with
counted public/viewport ports. It proves presentation admission and retained
history, not whole-frame speed, native visibility or GPU work.
Per-view row conversion is derived presentation: the sampler returns its existing
borrowed frame-stats pointer and retains every history/epoch observation. Only
admitted CPU presentation copies/formats rows; Frame Debugger retains its
existing once-per-UI-frame lazy capture. No second query is added to CPU sampling.


Bundle Browser and the Dist graph producer bind the same private
`core/jce_project_prepared_assets` adapter before the public Bundle pack call.
It merges existing canonical render settings, physics layers, project settings,
render pipeline and input actions as required dependency roots; the public packer
still owns cooking and closure.
Keep VFS/source precedence and the existing asset-index fallback. Prepared
configuration discovery or canonical mappings belong in that shared adapter.

## Identity

Scene content takes explicit viewport admission from its layout caller. A
covered Scene services pending material results, cancels stale gizmo/pick and
selection state, closes terrain/foliage strokes and stops camera piloting before
returning without toolbar or viewport work. Its own maximized overlay remains
admitted once, with no preceding dock-shell cancellation. Standalone Scene
wrappers explicitly admit their existing content. Probe maintenance is owned
by the admitted host pump before panel layout, not this content path.

Game Canvas pointer input retains its existing native ImGui position/button,
Play and capture gates. An explicit virtual Game source may replace only the
parameters to that same Canvas pointer setter via the core non-consuming peek;
its admitted display/FBO geometry must match the current visible Play view.
Never consume the runtime sample again, move the OS cursor, overwrite hover or
focus, or invoke a widget/script directly. Native text and wheel retain their
original separate gates. Typed actual-block controls do not prove physical input.

Game View backend selection and restart hints consume the existing startup
name-to-backend resolver. Compare resolved enums, including aliases and case,
before persisting a different choice. Auto accepts the live selection; an
unknown request or unknown live backend does not establish a restart need.
An unrecognized or currently unlisted configured choice retains its raw preview
and no selected row instead of silently appearing as Auto. Backend changes
still take effect on the next Editor launch; the selector never hot-swaps.
The exact-source CPU controls in tools/tests/test_game_view_backend_selection.py
exercise the actual selector and helpers with the real resolver. Counted UI and
config ports plus ordinary old-comparison negatives are not physical GUI input
or backend-creation evidence.

- **Language**: C++20 + ImGui (via `jce_tools_imgui`)
- **Role**: every dockable editor window. One TU per panel; instantiated via `jce_editor_panels.cpp` registry.

## File map (selected)
- `jce_panel_scene_view.cpp` (70 KB)   — 3D viewport, gizmo host, camera nav.
- `jce_panel_inspector.cpp` (~54 KB / ~1200 lines) — Inspector dispatcher, component section chrome, ordering, multi-edit broadcast, and delete confirmation.
- `jce_panel_inspector_add_component.cpp` — registry-driven Add Component popup; uses editor component slots so flagless components such as Compound Collider share the normal add path.
- `jce_panel_hierarchy*.cpp`           — entity tree (main / menu / node split for size).
- `jce_panel_hierarchy_input.{h,cpp}`  — pure keyboard-routing helpers for Hierarchy alpha-jump vs hotkey conflicts.
- `jce_panel_assets*.cpp`              — asset browser (grid / nav / detail split).
- `jce_panel_assets_thumb.{h,cpp}`     — LRU thumbnail cache (256 entries / 64 MB, 128 px box-downscale). Decodes images via `jce_image_decode`, materials via `jce_pbr_material_load_json`. Main-thread, budget 2 decodes/frame (`jce_thumb_pump(2)` from editor loop). Eligible extensions: `.png/.jpg/.jpeg/.tga/.bmp/.hdr/.mat.json`.
- `jce_panel_console.cpp`              — log sink + filter.
- `jce_panel_profiler.cpp` (44 KB) / `jce_panel_profiler_trace.cpp` — frame profiler plus bounded live Threads & Tasks view. The trace tab consumes `jce_trace` cursors, shows queue/run/wait timing and GPU Scene/MDI counters, and exports Chrome/Perfetto JSON only on explicit request.
- `jce_panel_memory_profiler.cpp`      — memory profiler. Reads public `jce_mem_profile_*` observations; sortable per-tag table, process-RSS 512 MiB target, Reset Peaks / Snapshot (JSON) buttons, 0.1–5 s refresh combo.
- `jce_panel_systems.cpp`              — Unity-parity Systems window (P3-B.5). Two tabs: **Player Loop** (8-phase collapsible groups, per-entry enable toggle / priority / last-frame ms via `jce_player_loop_iterate`) and **ECS Systems** (sortable table over `jce_scene_iterate_systems`, columns: enabled / name / group / last_ms / matched). Top strip: filter + 0.1–5 s refresh combo. All visible strings flow through `jce_editor_i18n("panel.systems.*")`.
- `jce_panel_material_graph.cpp` (panel entry only; impl split under `material_graph/`) / `jce_panel_shader_graph.cpp` — node editors.
- `jce_panel_particle_editor.cpp` / `jce_panel_vfx_graph.cpp` — VFX authoring. The VFX Graph panel is a node front-end for the standard particle pipeline: **Export** compiles the graph to `*.particles.json` (canonical `JceParticleEmitterDesc` keys) for a Particle Emitter component's asset path; SubEmitter/Trail nodes are lossy (warned, dropped). The former `.vfx.json` interpreter (`jce_vfx_graph.c`) and the `VfxGraph` component were retired in the v0.9.9 consolidation (scene loads migrate `graphPath` → ParticleEmitter `asset_path`).
- `jce_panel_animation_editor.cpp` / `jce_panel_animator_sm.cpp` / `jce_panel_curve_editor.cpp` / `jce_panel_sequencer.cpp` / `jce_panel_timeline.cpp` — animation suite.
- `jce_panel_terrain.cpp` + `jce_terrain_history.{h,cpp}` (sparse 32x32 tile deltas, bounded memory, chronological integration with global undo/redo) / `jce_panel_tile_palette.cpp` / `jce_panel_navmesh.cpp` / `jce_panel_lightmap_bake.cpp` / `jce_panel_reflection_probes.cpp` (P3-E.3 — sequential queue-based **Bake** / **Bake All**, per-row Bake/Cancel/progress via `<jce/renderer/jce_reflection_probe_bake.h>`; writes `ReflectionProbes/probe_<entity>.cube` and stamps `baked_cubemap_path` on the component) / `jce_panel_time_of_day.cpp` / `jce_panel_postfx.cpp` / `jce_panel_lighting.cpp` / `jce_panel_lighting_settings.cpp` (Unity-parity Lighting Settings; ambient/fog/shadow/PostFX live in scene `JceSceneRenderingSettings`, while sky/IBL workbench state remains session-local until its own serializer lands; **Bake All Reflection Probes** shell button delegates queueing to `jce_panel_reflection_probes.cpp`) / `jce_panel_light_explorer.cpp` — world/lighting tools.
- `jce_panel_audio_mixer.cpp` / `jce_panel_reverb_zones.cpp` — audio authoring.
- `jce_panel_physics_debugger.cpp` / `jce_panel_frame_debugger.cpp` — runtime debug. Physics Debugger gained a "Joint gizmos (selected)" sub-toggle (P3-C.6) backed by `jce_state_get/set_show_joint_gizmos`; selection-driven joint visuals are rendered by `editor/src/scene/jce_scene_render_draw.cpp::draw_joint_gizmos` via `editor/src/gizmo/jce_gizmo_joint.*`.
- `jce_panel_inspector_physics.cpp` — 3D physics component drawers (rigidbody / colliders / character / constraint / wheel / constant force / configurable joint). Rigidbody drawer surfaces **CCD** mode + threshold + swept-sphere radius (P3-C.3) via `inspector.rigidbody.ccd*` i18n keys.
- `jce_panel_inspector_lighting.cpp` — light / IBL / reflection-probe inspector drawers. Reflection Probe drawer (P3-E.3) hosts the Bake / Cancel / Progress UI keyed off component pointer; submits via `<jce/renderer/jce_reflection_probe_bake.h>` and writes `baked_cubemap_path` on completion.
- `jce_panel_input_manager.cpp` — input mapping UI.
- `jce_panel_save_browser.cpp` / `jce_panel_search.cpp` / `jce_panel_file_viewer.cpp` / `jce_panel_status_bar.cpp` / `jce_panel_toolbar.cpp` / `jce_panel_test_runner.cpp` / `jce_panel_package_manager.cpp` / `jce_panel_build_profiles.cpp` / `jce_panel_version_control.cpp` / `jce_panel_import_presets.cpp` / `jce_panel_sprite_editor.cpp` / `jce_panel_vcam_manager.cpp` / `jce_panel_game_view.cpp` / `jce_panel_bundle_browser.cpp` / `jce_panel_build_report.cpp` (P3-A.4 — reads `build_report.json` emitted by `jce_bundle_pack`; tabs: Overview / Bundles / Entries / Duplicates / Top-N) / `jce_panel_physics_layers.cpp` (P3-C.2 — Layer Collision Matrix; auto-saves to `.jce/physics_layers.json`).
- `jce_scene_view_*.cpp` + `jce_scene_view_internal.h` — scene-viewport internals (cube, gizmo overlay, helpers, pure input ownership policy).
- `jce_panel_*_internal.h` — per-family private decls.

## Rules
1. **One panel = one TU** (`jce_panel_<name>.cpp`). Register in `editor/src/ui/jce_editor_panels.cpp`.
2. **No editor state** — read/write via `editor/src/core/jce_editor_state.{h,cpp}` only.
3. **No direct hotkey capture** — go through `editor/src/core/jce_hotkeys.cpp`.
4. **All strings i18n'd** via `editor_t("key")`.
5. **No engine private includes**; panels stay above the `JCE` public API + `jce_tools_imgui`.
6. **File size discipline**: `jce_panel_inspector.cpp` has been trimmed below the 2000-line soft cap; keep new component UI in the split `inspector_*.cpp` domain files instead of growing the dispatcher again.
7. **Internal headers** (`*_internal.h`) only included by sibling `.cpp`s in the same family.
8. **Use `jce_editor_history`** to record undoable mutations; non-undoable changes must call out why in a comment.
9. **Frame budget**: each panel `Draw()` ≤ 0.5 ms on baseline HW; heavy work goes to `jce_thread_pool_*` (engine/include/jce/os/core/jce_thread.h).
10. **No `ImGui::ShowDemoWindow()`** in shipping builds (gate behind `JCE_EDITOR_DEV`).

Use `jce_editor_i18n_id` only for widget labels that ImGui parses for identity.
Formatted text and tooltip contents use `jce_editor_i18n` or
`jce_editor_i18n_or`; these display strings verbatim and must not receive the
hidden-ID suffix. Preserve existing widget IDs when correcting display text.

## Don't

Scene/Game policy samples distinguish called wrapper visibility, collapsed
ImGui Begin, invalid layout, focus loss, clipping and explicit capture using the
shared private viewport-work/trace helpers. A hidden panel skipped by the outer
registry has no wrapper sample. Preserve existing admission, layout, pointer
release and capture behavior; policy samples do not certify render-owner calls
or GPU work. Rendering and its final refusal reason belong to the actual Scene/
Game render owners, not the panel's inferred texture availability.

Reflection Probe Inspector reuses its existing Cancel button during its own
capture phase, including a zero bake handle. It retains the shared Scene owner's
successful request token and matches that Scene/entity/token before displaying
capture controls. A stale component pointer plus another global capture is not
ownership; the batch panel also begins through the same owner. It calls the
safe cancel adapter; a different or superseded request cannot cancel it. Capture has no
public fractional progress, so display only its existing status rather than a
fabricated percentage. Existing bake handles retain their progress/cancel path.
The handle created when captured faces transfer to convolution is not exposed
by the current capture API; do not claim that subsequent UI tracking is complete.
- Don't share global statics across panels — use `jce_editor_state`.
- Don't allocate per frame with `new`/`malloc` — reuse buffers or use `jce_alloc_frame`.
- Don't call SDL / bgfx directly — go through engine public API or `jce_imgui_renderer`.

## Common tasks
- **Add a new panel** → `jce_panel_<foo>.cpp` → register in `editor/src/ui/jce_editor_panels.cpp` → add menu entry + default dock slot in `editor/src/ui/jce_editor_layout.cpp` → add hotkey in `editor/src/core/jce_hotkeys.cpp`.
- **Refactor inspector** → split by component domain into `jce_panel_inspector_<domain>.cpp` files; share via `jce_panel_inspector_internal.h`.

## Status-bar GPU tier widget (P3-E.6)
`jce_panel_status_bar.cpp` shows the renderer caps tier (LOW/MID/HIGH/ULTRA)
colored by severity and clickable to open an override popup
(`jce_renderer_set_tier_override` / `jce_renderer_clear_tier_override`).
The override is editor-session only — never persisted, mirroring Unity
Quality Settings' tier override semantics. i18n keys live under
`statusBar.gpuTier.*` in both `en.json` and `zh_cn.json`.

## P3-E.5 — Light Cookies + IES Profiles

Spot lights ship end-to-end (cookie texture + IES LM-63 photometric LUT) via new `JceSpotLight` fields (`cookie_texture` / `ies_lut_texture` / `cookie_strength` / `cookie_path` / `ies_path`) and matching `JceSpotLightDesc` fields. Directional cookie is data-side scaffolded (component, serializer, inspector, uniform upload) with the shader projection guarded `#if 0` until a CSM-aligned VP is wired. Sampler slots **13 = `s_cookie`** and **14 = `s_iesLut`**; v1 binds the FIRST spot light that has a cookie / IES (multi-cookie atlas deferred to P3-E.5b). New public header: `<jce/renderer/jce_ies_profile.h>` (`jce_ies_parse`, `jce_ies_bake_lut_from_file`, `jce_ies_bake_lut_from_memory`). **Rebuild shaders**: `fs_pbr.sc` adds samplers + `u_cookieParams` / `u_cookieSpotVP` uniforms — run shaderc against `engine/shaders/pbr/fs_pbr.sc` to refresh `_cooked/*/shaders/*/fs_pbr.bin`.

## P3-E.5b — Directional cookie inspector wiring

The directional-light inspector now labels the cookie section "Cookie (world-aligned)" (`inspector.light.dirCookie` i18n key, en + zh_cn) and routes the strength slider into the live projection path. No new picker — the file-asset input from P3-E.5 stays. See `engine/src/renderer/AGENTS.md` "P3-E.5b" for the projection-matrix shape and atlas behaviour.

## 2026-09-01 — `jce_panel_inspector_gameplay.cpp` 引用了一个 HEAD 里没有的字段

`vs->baked_placement_path` 在本文件里出现 **7 次**，而 `HEAD:engine/include/jce/middleware/scene/jce_scene.h`
**没有这个字段**——所以从干净检出编译这个分支的 HEAD 会失败。当前工作树能编，
是因为字段就在工作树里（未提交）。

**这是我造成的，不是并发会话的错。** `6608b658`（撤销作用域那一批）`git add` 了整个面板文件，
把当时工作树里属于另一个会话的这一段一起提交了；我发现后修掉了**头文件**那一半（把字段从提交里摘出来），
**面板这一半漏了**。

**不要为了「修好 HEAD」去删这 86 行**（573–658）：它们是另一个会话在途的烘焙 UI，
删掉等于抹掉别人还没提交的工作。正确的收敛方式是**那个会话提交它的头文件字段**，
分支自然自洽。

顺带：`insp_unwired_field_badge()` 已加在 `baked_placement_path` 旁边，因为**今天**确实没有读者
（`engine/src` 下零处）。等读者落地，`tools/lint/check_authored_path_consumed.py`
会因为 `authored_path_exempt.txt` 里那条豁免过期而变红，**那个标记和那条豁免一起删**。

## 2026-09-06 — 组合框的行号不是枚举值：`jce_panel_inspector_enum_maps.{h,cpp}`

Rigidbody2D 的 Body Type 组合框把 ImGui 的**裸行号**当成存储值。`JceBodyType` 是
`STATIC=0 / DYNAMIC=1 / KINEMATIC=2`，而列表读作 static / kinematic / dynamic ⇒
选「Kinematic」存进 `DYNAMIC`，选「Dynamic」存进 `KINEMATIC`。

**编辑器里没有任何东西能揭露它**：回读用的是同一个行号，所以面板在两个方向上自洽，
你挑哪个标签就显示哪个标签——只有物理不同意。

映射现在是 `jce_panel_inspector_enum_maps.cpp` 里的一张表。它**刻意不 include ImGui**，
这样 `tests/editor/test_jce_inspector_enum_maps.cpp` 能单独链接它并断言映射。
shape_type 的三项表同时从 `jce_panel_inspector_physics.cpp` 与 `_physics2d.cpp`
两份拷贝收敛到这里。

**新增一个「列表顺序 ≠ 枚举顺序」的组合框时，把映射加进这个 TU，不要写成局部数组。**
判据不是「看起来对」——是那个测试。

## 2026-09-20 — 脚本公开参数：连续控件的 `insp_track_edit()` 必须在 `if` 外面

`draw_script_params()`（Unity 的 `[SerializeField]` / Godot 的 `@export`）画一张
四列表：名称 / 类型 / 值 / 删除。形状**抄的是** `jce_panel_animator_sm.cpp:827`
的 Animator 参数表——「作者自定义的一串有类型的参数」是同一个问题，两处不该长得不一样。

**这里踩了两种相反的 undo 陷阱，各一次：**

| 控件 | 正确写法 | 为什么 |
|---|---|---|
| 类型下拉（combo） | `insp_undo_set(&pm->kind, k)` | `insp_track_edit()` 键在 IsItemActivated / IsItemDeactivated 上，而 combo 的这两帧是**弹窗开关帧**，永远不是值变化帧 |
| 名称 / 文本 / 拖动数值 / 拖动实体 | 控件照常调用，`insp_track_edit()` 放在 `if` **外面** | `if (ImGui::InputText(...))` 的**函数体**只在**变化帧**跑，而那一帧既不是 activated 也不是 deactivated |

**第一条我在上一行写了注释，然后在下面四处走进了第二条。**
`tools/lint/check_inspector_undo_scope.py` 按 `file:line` 逐条点名了它们
（那四处编辑当时既不可撤销、也不把场景标脏，且**完全静默**）。
⇒ **自己写下的一句警告不等于自己读过的一句警告。**

新增 14 个 `inspector.script.*` i18n 键，15 个 locale 全覆盖。
**句子里不写数字**：「8 个槽位都占满了」会在 `JCE_SCRIPT_PARAM_MAX` 改动那天变成谎话，
而且没有任何东西会报告它。

## 2026-09-20 — 加一个 Profiler 标签页是**两处**改动，漏掉第二处会**延迟**失败

`JcePanelTabState` 带一个 `max_tab`，它**既是有效性边界又是加载时的夹取**。
只加 `BeginTabItem` 而不抬 `max_tab`：新标签页这一次会话里画得好好的，
**下次启动时那个被持久化的选择被夹掉**——于是它不是停止工作，而是停止被恢复。
（Profiler 的 Scripts 标签页落地时 0..5 → 0..6。）

Scripts 标签页本身抄的是 ECS Systems 表（`jce_panel_systems.cpp`）：
一个引擎侧迭代回调 + 可排序表。两者回答同一类问题，不该长得不一样。

**运行时只在 Play 期间存在**（`jce_editor_play_get_runtime()` 否则返回 NULL）。
这时要**明说**，不要画一张空表：空表和「还没有东西可量」是同一幅画、两件完全不同的事。


## 2026-09-20 — 面板不再自己读 key；以及 `Key` 换成 C 结构体时**丢掉的是成员初始化器**

`load_curve` 现在调 `jce_curve_parse` 拿 key 和 channel 名，
自己只读 `visible` / `color` / `tMin..vMax`。分工不是妥协而是正确的切法：
**面板拥有曲线的外观，引擎拥有曲线本身**。

**换类型的代价要在每一个构造点付，而不是在看起来要紧的那几个。**
`Key` 从带 C++ 成员初始化器（`float tan_in = 0.0f;`）的 struct 变成
`typedef JceCurveKey Key` 之后，**只赋两三个字段的站点会读到未初始化的切线**，
MSVC 对部分赋值的聚合**不警告**——那会变成「作者没碰过的那些 key 斜率是垃圾」。
八个站点全部走 `make_curve_key()`；其中**最后两个是靠收尾断言找到的**，
它们和另一对逐字节相同、只差四个空格的缩进，所以 `count == 1` 的守卫看不见它们。

**`make_key` 这个名字被 dedup audit 拦下**（`jce_gizmo_compound_collider.cpp`
里已有一个，意思是「拼一个缓存键字符串」）。改名，不是加豁免：
**两个无关含义共用一个名字**正是那道门存在的理由，而 internal linkage
并不会让它不容易认错。


## 2026-09-21 — 解析后重新序列化：**数据无损，文件有损**

给 15 个 locale 加 10 个键，我用 `json.loads` → 插入 → `json.dumps` 写回。
JSON 进 JSON 出，脚本诚实地打印了十五行 `+10 keys`。然后 `git diff --stat`：

```
15 files changed, 150 insertions(+), 1710 deletions(-)
```

**这些文件带着 114 个空行在分组**，重新序列化把每一个都抹平了——
而这是并发会话也在改的十五个文件。

⇒ **parse-and-re-emit 对「数据」无损，对「文件」有损，而人读的是文件。**
一次保住了每一个值的往返**仍然不是往返**。
⇒ 抓到它的不是脚本——脚本对它唯一度量的东西的汇报是准确且完整的。
现在 `git diff --numstat` 是**工具内部的断言**（每个文件必须恰好 `+10 -0`）：
**读被测物形状的检查器，胜过读自己意图的检查器。**
⇒ 恢复之所以安全，是因为**先**对那十五条路径跑了 `git status --porcelain`
且输出为空，`git checkout --` 才吃不掉别人的东西。这两条要在同一口气里。

**`kind` 枚举在末尾加 `PK_AUDIO = 2`**：它是已经写进磁盘上每一个
`.import.json` 的整数，插在中间会让每个项目里的纹理 sidecar 被重读成模型。
同一条哨兵教训，只是这次数字在别人的文件里。


## 2026-09-21 — 选择项按**名字**记，因为这张列表逐帧改变长度

Frame Debugger 新增「渲染目标」一节。引擎**拒绝列出这一帧没有真实像素的目标**，
所以条目数随特性开关变化——于是记住「第 3 个」会在**恰好有人在盯着它的那一刻**
滑到另一个目标上。按名字记；这一帧不存在的名字不是错误（是特性关着），
所以回落到第一个，等特性回来再自己选回去。

**空的时候要说**为什么（哪些特性打开才会出现条目），不要画一个空盒子——
「空表」和「还没有东西可量」是同一幅画、两件完全不同的事。
这条本文件 2026-09-20 就为 Profiler 的 Scripts 标签页记过一次。

**`note` 不是装饰。** 这些大多**不是场景的照片**：velocity 是
`rg=(curNDC-prevNDC)*0.5+0.5`（中灰=静止，不是空），depth 与阴影图
原样采样**在大部分范围里接近纯白**。不带解释地显示一张 velocity 缓冲，
等于给用户看了一个灰色矩形然后什么也没说。

用 `CollapsingHeader` 而不是标签页，于是**不碰 `JcePanelTabState` 的 `max_tab`**
——那处漏掉的第二步是**延迟失败**：这一次会话画得好好的，下次启动时被夹掉。


## 2026-09-21 — 导出报告要**逐条**，不能只报总数

「导出了 7 个片段」会盖掉那个因为 channel 不可用而被拒收的片段——
而被拒收的片段留下一个**在作者那里能解析、在运行时解析不了**的名字。
所以 banner 分别报成功数与拒收数。

导出的落点**就是运行时回退查找的落点**（`<骨架目录>/<名字>.animclip.json`），
于是写端和读端**靠构造一致**，而不是靠一句文档约定——
一句约定是关于同一件事的第二个陈述。

## 2026-09-21 — `jce.py test` **编译不到任何一个 editor TU**

`jce_tests` 不包含 `editor/**` 的任何翻译单元。⇒ **一个编辑器编译错误可以
让全套 477/477 全绿。**

实测：给动画面板加导出功能时漏了 `#include "scene/jce_editor_scene_render.h"`，
`jce_editor_get_scene_renderer` 报 C3861，而**同一棵树上 `jce.py test`
是 476/477**（那一个红是物理的，与面板无关）。是并发会话跑 `jce.py editor`
才撞出来的——否则这个单元会带着一个**编不过的编辑器**被提交，
而收尾判据会说它是绿的。

⇒ **凡改动 `editor/**`，`python scripts/jce.py editor` 是必经的第三道门**，
不在 `lint` 里、也不在 `test` 里。CLAUDE.md §6 的两条命令**不覆盖它**。

**这个特定的 include 特别容易漏**：`jce_editor_get_scene_renderer` 是
**编辑器侧**的访问器，住在 `editor/src/scene/jce_editor_scene_render.h`；
而面板的 include 块里往往已经有引擎的
`<jce/renderer/jce_scene_renderer.h>`——它声明了 renderer 类型、
**长得就像该有这个访问器**，但没有。看着像齐了，所以不会去查。

D01 operation observations use jce_editor_operation_metrics and the existing
main-thread phase accumulator. Inspector sync requests have generations;
coalesced/invalidated requests are not successful draw completions. Explicit
JCE_EDITOR_OPERATION_METRICS enables log observations. JCE_DBG_RENAME and
JCE_DBG_REPARENT use source:target@frame (ROOT detaches); JCE_DBG_SCROLL uses
entity@panel-frame and the existing reveal/clipper path. These maintenance
controls measure programmatic operations, never physical input-to-display.
Loaded entity count, flattened rows and display capacity remain distinct.

Hierarchy rows/order/root arrays use demand-based transactional ED_MALLOC storage
within a combined 64 MiB peak budget, including old/new arrays during growth. Existing ImGuiListClipper still submits only
visible rows; allocation failure is visible and logged instead of a partial tree.
Wide child queries use public count/query with the stack fast path retained for
small nodes; shutdown frees panel storage before engine allocator shutdown.

Default hierarchy flatten reuse is deliberately bounded to sort=0, empty search,
no tag filter, and no streamed grouping. Stored rows contain only entity IDs,
depths and group markers; row drawing reads current metadata. Key public scene
structural/roster epochs, scene/context/storage identities, editor order revision,
and local expansion/reveal state. Parent changes, append/remove and full scene
replacement invalidate; arrow open-state transitions invalidate after drawing,
matching the existing one-frame expand behavior. Storage growth/shutdown also
invalidate. Other sort/filter/group modes rebuild every frame because mutable
public EditorMeta fields do not universally publish an observable generation.
JCE_DISABLE_HIERARCHY_CACHE=1 is a correctness A/B maintenance control. Logs
under operation metrics report actual eligible/reused/row/root observations;
rebuild phase timings count only executed rebuilds, not synthetic zero-cost work.

Play without a CharacterController still owns pointer input when it uses an ordinary primary Camera; an active VirtualCamera is not required. Displayed image coordinates and deltas map to the actual render dimensions, including scaled captures.

The Memory Profiler shows N/A for unavailable allocator-specific heap counters;
zero does not prove an empty heap. Process RSS/commit retain their process scope.

Memory Profiler aggregate current/peak use one simultaneous tagged snapshot.
Snapshot JSON separately exports the legacy per-tag peak sum, with explicit
voluntary-tagged scope; process counters and per-tag rows remain independent.

Asset-browser explicit refresh uses `request_asset_resource_refresh`; the index
owner notifies Canvas resources after completion. Ordinary folder navigation or
periodic listing must not retire font atlases unconditionally. Final project
packages use the core package path owner; build intermediates/reports stay under
build and verified application outputs publish under dist/apps.


Build Profiles uses the core native target/layout policy for edited Target/Exe,
selected architecture and release/dist previews. Its build output prediction is
separate from package output under the existing dist/apps default-path owner.
Only engine workspace mode consumes the global workspace target override and
requires Conan; SDK-native projects require CMake/Ninja. Re-check uses the same
public toolchain discovery paths as actual native execution, including explicit
overrides. CPU helper/port controls do not certify GUI input or a real compiler.

Grass continuous fields track the actual edited item on every draw; flags and reseed use discrete undo. Density-mask edits consume the canonical path widget pre-edit port, including asynchronous browse results. A CPU history-port test is distinct from real GUI Undo/Redo acceptance.

Preferences Fonts consumes the shared searchable picker. A font choice persists through the canonical config and requests the existing deferred font reload, outside the active draw frame. Explicit unlisted paths remain visible and cannot silently appear as Auto.

Scene View navigation chrome submits real ImGui items before the full viewport
input item. Cube and visible axis clicks are item-owned; reuse the existing
selection, deferred-pick, brush and cancellation gates. Canonical overlay
geometry is shared by drawing and hit admission. The viewport remains
LastItemData for the existing asset drop path. A chrome press cannot start a
new gizmo operation, while an already-active gizmo still updates and releases.
Real ImGui context controls and fresh native GUI checks are separate evidence.

Scene View click priority is helper icon, ECS UI, GPU ID, then a finite CPU
ray. The CPU query uses the rendered model's retained triangles where
available, compares them with visible proxy geometry, and preserves exact
misses. The viewport camera's clip convention sets the segment endpoints.
Marquee and drag/drop keep their independent request and hover paths.

Scene navigation registration ends with the real full-viewport item and its ItemSize cursor advance; never restore an unsubmitted post-Image cursor past CursorMaxPos. Actual-context tests cover Image/fallback, modal, End and Render with zero recorded ImGui errors; soft error recovery is not acceptance.

## Preferences hotkey capture consumption

Preferences consumes the central `jce_hotkeys_capture_*` owner. Poll once before
drawing binding rows; only a committed result saves the binding. Begin is the
Bind button's only capture mutation. Tab changes away from Hotkeys, modal
Close/X/hidden state, root-and-child focus loss and Reset cancel through the
same owner. The registry keeps the cancellation/completion frame blocked, so
background actions cannot run later in that frame. Preserve the existing red
duplicate-binding marker and tooltip; do not silently remove another action's
binding. Real ImGui CPU controls verify the registry port, while native GUI
checks must verify these actual modal/tab/focus hooks and save/reopen behavior.

Build Profiles restores its one-time persisted root before adopting the open
project. Restoring after adoption can overwrite the active root with an empty
preference and emit a false CMakePresets warning before the synthetic project
profile loads. Project mode and engine-workspace mode retain their existing
build authority; tool discovery alone is not a complete compiler/sysroot gate.

Test Runner consumes private core/jce_editor_test_run and its existing .test.json suites. Run/Run All queues one child at a time; Rescan/input are disabled while queued/running, and Cancel clears queued work plus requests child stop. The layout ticks even while hidden, and the actual editor panels shutdown closes the owner. Logs use the shared profile build/editor-test-runs directory, not CWD temp files or project output. Public JSON string values are encoded losslessly for public process argv; unsupported empty/oversized tokens fail explicitly. Case previews are bounded at 64 KiB with a visible shortened marker and retained full log; incomplete captured/logged evidence cannot PASS. Native self-child controls cover argv, stdout/stderr, real exit codes, missing executable, cancel, timeout (including a first poll after actual 2-second children exit with 0 or 17), output overflow and filesystem failures. Exclusive-create refusal preserves prior results and existing bytes; private filesystem fault controls remain distinct from real OS failure. Separate runtime mutations disable exit-code and observed-deadline gates; compilation failure is not an accepted negative. These controls do not certify GUI click/hidden/restart consumption.


Preferences General persists three authoring FPS rates through the existing
canonical ui_int/config publication. Scene and Game viewport work shares private
`core/jce_editor_viewport_work.h`: positive finite dimensions, actual ImGui clip
visibility and app focus gate offscreen rendering. The engine already skips
minimized host frames. Empty Game layout clears pointer/viewport ownership rather
than rendering a clamped 16-pixel target. Positive explicitly requested KPI
captures bypass clip/focus suppression; empty layout cannot certify a capture.
These observations do not prove OS occlusion, DWM composition or physical GPU
presentation, and playing simulation retains its existing runtime owner.


Game View's GPU admission no longer drives module lifecycle or simulation.
Module selection delegates to the existing Play host; custom update proceeds
without an admitted view while the project background policy allows simulation.
Pause/Stop/focus input cleanup still runs when viewport rendering is clipped.
Preferences idle FPS also supplies the temporary paused-preview cadence;
background FPS is temporary even for a running background-enabled project.
Foreground resume and Stop restore the session/authoring values via one deadline
mechanism, without rewriting authored FPS/VSync or lowering rendering quality.

Preferences centers within the viewport work area and uses stacked captions for frame-rate, font and GPU-vendor fields so translated labels remain readable. The GPU combo uses the stable hidden ID `##gpu_adapter` and the available content width; its help describes next-launch selection without exposing implementation settings. Auto font help names Ink Free consistently with the shared host-font owner. Verify these fields by actual GUI consumption, including a smaller work area and font reload; source strings alone do not prove readability.

Preferences Appearance authors the existing `editor.gpu_adapter` integer state
through the shared startup owner: Auto/Intel/NVIDIA/AMD are vendor requests for
the next launch, not exact devices or a live renderer switch. Missing state shows
Auto without writing or overriding environment policy; explicitly clicking Auto
stores zero. Invalid saved state stays visibly invalid until a valid choice is
made. Preserve normal debounced config publication, full-table refusal, and the
actual-device log rather than claiming the requested vendor was selected. All
four labels/help keys remain synchronized across shipped locales.

Preferences restores its last selected category from the canonical session
`ui_state_int` key `preferences.active_tab` after loading its existing config
mirror. Admit IDs from the same tab table used by the UI; missing or invalid
values open General, with invalid values diagnosed only on load. Only actual
category clicks update the key and request the existing debounced config save;
table/publication failures retain the config owner's diagnostics. Seeded-profile
restore and whole-editor captures do not prove native menu or keyboard input.

Profiler timing interpretation uses `jce_profiler_observation.h`: backend frame
intervals, latched GPU timings, API-thread render waits and render-thread submit
waits are separate observations. Missing timers show N/A. No exact engine-frame
join is available, so do not infer CPU/GPU bottlenecks from their averages or
subtract both thread waits from an interval. Wait labels describe observed waits.

Memory target bars use available process RSS only, not voluntary tagged totals
or allocator-specific counters. The target is process scope, not whole-device
usage or a physical 512 MiB acceptance claim. `jce_panel_memory_snapshot.h` owns
complete bounded JSON, exact unsigned counters, escaped tag names, availability
flags/nulls and atomic publication under the existing profile reports/memory
path. Invalid explicit profiles and incomplete serialization never publish a
successful snapshot or fall back to CWD. Snapshot requests refresh observations
before encoding; actual UI, hardware and allocator-completeness evidence remain
separate from the CPU owner fixtures and ordinary-failing wire controls.

Build Profiles defaults user-project requests to Dist, with explicit Release
selection retained on all native project platforms. Build shows its cache
executable; Pack separately publishes the verified single executable through the
existing Dist publisher. Explicit Release packages may retain development
companion files. Both direct Play and Cook-completed Play require the native or
workspace dispatcher bool before entering BUILDING; rejected preflight/start
requests publish visible FAILED through the same manager owner. Refresh the
status snapshot after GUI dispatch before showing success or rewriting Run paths.

Bundle Browser Open exposes explicit asynchronous package or cached-snapshot
inspection over the public Bundle inspector APIs and private shared summary
owner. File inspection retains target/content/available-sidecar admission.
Snapshots are content-only, capped at 64 MiB, captured only on user action and
reused without another disk read or whole-pack copy. Exact target selection is
independent of the ordinary host-runtime Open action; Auto admits only portable
content. Request generations reject stale path/target/clear results. Task cleanup
releases owned bytes, and panel shutdown disarms completion before teardown.
No scene/VFS mutation, second parser, registry or per-frame IO is introduced.
CPU ownership controls are separate from actual graphical reachability.

## GPU timing consumption

Profiler and Profile Analyzer admit GPU times only through the public bounded
`JceGpuTimingObservation`, retaining the existing localized delayed-value and
unavailable labels. Unavailable samples are excluded from summaries and written
as blank CSV values with explicit availability flags; incomplete Analyzer plots
do not draw unavailable values as zero-cost work. The common
`jce_profiler_observation.h` owns CPU interval and separate thread-wait
interpretation. Profile Analyzer must not subtract render/submit waits from the
backend CPU interval or turn missing timer frequency into denominator 1. These
changes repair measurement semantics, not the application's execution speed.

## Benchmark observation clocks

`jce_benchmark_observation.h` owns panel-to-panel monotonic wall cadence,
continuous UI/CPU host-frame/context admission and the existing Lifecycle
listener. Registration is unique, failed registration leaves no handle, explicit
Reset retries it, and WILL_QUIT/destruction unregisters once. Hidden frame gaps,
focus/pause/device events, modal states and clock/profile changes count missing
coverage; consecutive long intervals remain in the distribution. These are UI
observations, not presentation FPS, GPU completion or joined CPU/GPU frames.
Backend CPU interval and qualified latched GPU timing have separate availability.
`benchmark_ui_cadence_v1.txt` never migrates or overwrites the legacy metric file;
save/compare requires continuous coverage and the exact bounded profile. Workload
isolation means scene workload isolation, not exclusive OS/GPU access.



## Profiler interval and history scope

The Profiler header and Frame graph expose the wall interval between Editor UI
updates, while the status bar displays ImGui's rolling UI cadence. Their rates
are not presentation FPS. The clipboard uses the same localized interval/rate
labels. Keep backend intervals, latched GPU timings and the two thread waits
independent; an observed submission wait is not a GPU bottleneck verdict.

The existing per-slot CPU/GPU/API/submit availability masks remain authoritative.
Charts join only adjacent finite known values and draw an endpoint only when it
is known; a known zero remains valid. Unknown slots show a gap, explicit
unavailable text and known/total plus unavailable counts in the caption/tooltip.
A change of the existing public observation's context availability/generation
invalidates backend timing masks while preserving UI wall history. This does
not establish a CPU/GPU frame join or a GPU completion witness. Preserve the
continuous render path and the existing Benchmark wall-clock observer.

## Analyzer CSV publication

`jce_panel_profile_analyzer.h` is the private bounded CSV producer consumed by
the existing Analyzer export button. It preserves the eight CSV columns,
oldest-to-newest ring order, blank unavailable fields and available zero values.
Locale-independent numeric encoding retains three decimal places and exact
unsigned frame IDs. Invalid known metrics or any incomplete encoding refuse
publication; a fixed 64 KiB heap buffer admits all 256 finite float records.

The existing profile resolver selects `reports/profiler/profile_export.csv`;
invalid profiles never fall back to CWD. The UI thread publishes a complete
buffer using the public atomic host writer and releases it exactly once. The
existing Analyzer state retains each request's result and path. Empty input,
encoding, profile-path, allocation and write failures have localized visible
statuses and one ordinary WARN per request; success logs INFO even in DIST.
A failed write preserves the previous complete report and permits retry.
This is complete CSV publication, not GPU completion or exact frame joining.

Native controls consume the full private producer and strict extracts of the
actual ring, export, result-key and toolbar bodies with real profile/host IO.
Their UI and fault ports do not certify physical button input or rendered text.
The sampler itself is unchanged and is outside this CSV fixture's scope.

## Trace panel registration and memory qualification

The Threads & Tasks owner treats trace registration as three observations:
unknown prefix, registered, and unregistration observed. None is an OS thread
liveness result. The summary counts current producer registrations; table rows
cover this cursor's observed history. Clear starts a new prefix; cursor loss
invalidates registration/name qualification and discards unpaired open spans
before consuming the retained events. Auto refresh only polls the trace cursor;
turning it off intentionally retains history and never changes recording.
The toolbar always displays the public stats.enabled recording state. When
recording is off, a localized hint identifies retained history and explains
that automatic refresh does not enable recording. No panel polling or refresh
widget sets/resets the producer or changes application capture defaults.
Clear saturates the next public cursor sequence at UINT32_MAX, including when
the producer reports its exhausted endpoint; never wrap to the uninitialized
zero cursor and replay retained history. Clear does not reset the producer.

Most recent counters preserve signed raw trace values. Negative sentinels for
adapter-reported used bytes, texture estimates and render-target estimates
display unavailable; a valid zero and unrelated negative counters remain
numeric. This panel neither substitutes process/GPU memory nor changes the
renderer producer or the legacy performance-log memory presentation.

The registration-scope hint consumes `profiler.trace.scopeNote` directly
through `jce_editor_i18n`; all fifteen locales define that key. Keep the scope
semantics in the translations rather than a long private-wrapper fallback.

## Game observation consumption

Game View reports the current requested extent and its existing layout/focus/clip
refusal, then notes the exact Image texture after the real Image call. Hidden,
collapsed and unvisited intervals cannot inherit a previous completed render.
Do not substitute valid texture handles or capture-note callbacks for the
GameRender CPU-tail or optional renderer submit observations.

## LAN discovery draw boundary

The Discovery tab requests Refresh from the private Editor LAN lifecycle service
and displays the public transport's current scanning state. Drawing never ticks
the transport or owns expiry; switching tabs, hiding the panel or skipping a UI
frame cannot retain a scan beyond the next CPU-maintenance poll. Do not add a
second cached scanning flag or start discovery during panel initialization.

## Console presentation admission

`jce_editor_panel_console_content_admitted` retains initialization, actual log
drain and Clear-on-Play/last-play updates before an inadmitted presentation
returns. The gate precedes toolbar, merge/filter/collapse, row-layout cache and
widget work. The no-argument content wrapper keeps ordinary callers admitted.
The independent host Console poll continues draining logs without changing
retention, filtering or Clear-on-Play timing; it never takes ownership of that
play transition. Admission does not retire or recreate the Console session.

## Bounded live trace view

The existing trace panel owns at most 1024 observed thread rows, 2048 counter
names and 8192 active span IDs, alongside its existing task/completed limits.
Existing keys keep updating at capacity. New-key refusals are counted as events,
not distinct identities; there is no refused-ID set or invented overflow thread.
The toolbar marks the local view partial and separates these refusals from Core
cursor loss. Thread registration stays unknown/registered/unregistration observed;
these historical rows are not an OS roster. The open-work column counts only
locally paired cached work begins. An unmatched or wrong-kind end cannot consume
another tracked begin; updating an existing begin transfers its local accounting.
Clear releases every view cache and resets local refusal counts without resetting
the producer. Prefix loss keeps refusal history while invalidating registrations
and clearing unpaired begins as before. Public cursor/stats expose no generation
token: a quiescent external producer reset followed by explicit Clear is supported;
automatic detection of every external reset is not claimed. Counters retain signed
int64 values and the public uint32 cursor endpoint remains unchanged. Core trace,
canonical capture and export are independent of these local retention limits.

## Covered Threads and Tasks presentation

The existing trace content entry always polls its passive Core cursor before
checking same-host maximized Game presentation. Covered content skips the
summary stats read, GPU Scene query and all widgets/row formatting. The local
bounded history, live-off setting and cursor-loss qualification retain their
owners; Core recording/export and application async work remain independent.
Detached profiler windows and normal presentation consume the current history.
This gate adds no producer, cache, timer, public API or performance guarantee.

The WaterNearshore Inspector picks terrain entities through the scene SDK.
The selector is undoable and the scene registry owns component persistence.
