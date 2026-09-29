# editor/src/panels/ — ImGui Dock Panels

## Identity
- **Language**: C++17 + ImGui (via `jce_tools_imgui`)
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
- `jce_panel_memory_profiler.cpp`      — tag-categorised memory profiler (P3-A.5). Reads `jce_mem_profile_*` + `mi_process_info`; sortable per-tag table, 512 MB baseline bar, Reset Peaks / Snapshot (JSON) buttons, 0.1–5 s refresh combo.
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

## Don't
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
