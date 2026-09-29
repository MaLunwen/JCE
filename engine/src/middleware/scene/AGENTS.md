# engine/src/middleware/scene — Scene / ECS (L4)

> The ECS heart: components, prefabs, terrain, virtual cameras, sequencer, LOD, space partition.

## Identity

- **Layer**: L4. C99. Backed by **flecs** (private link).
- **Public umbrella**: `<jce/api_scene.h>` → `<jce/middleware/scene/jce_*.h>`

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_scene.h` | `jce_scene.c` | Scene container, world, root ECS components (Transform, Tag, etc.) |
| `jce_scene_camera.h` | `jce_scene_camera.c` | Strict single-primary camera resolution and world-pose/projection binding to the generic render camera. |
| `jce_scene_components_json.h` | `jce_scene_components_json.c` | JSON (de)serializer for every public component |
| `jce_material_override.h` | `jce_material_override.c` | Per-renderer material overrides.  `jce_mesh_renderer_apply_material_pbr()` is the ONLY place the "skip what this renderer claims" rule lives -- runtime loader, editor path-repair and the inspector's material load/reload all call it.  Do not re-inline it: the two loaders each had their own copy and both overwrote all six factors unconditionally, so a per-instance tint on a shared material was discarded on the next open. |
| `jce_scene_fullscreen_effect.h` | `jce_scene_fullscreen_effect.c` + render serializer | Generic full-screen shader component. It is appended to the dense component registry with no legacy flag; renderer consumption stays generic and project equations remain in consumer shaders/scripts. |
| `jce_scene_async.h` | `jce_scene_async.c` | **P3-A.3** Async prefab/scene instantiate. Worker thread (jce_thread) does I/O + JSON parse; main-thread `dispatch_main` (wired to `JCE_PHASE_EARLY_UPDATE`) commits ≤ 1 parsed payload per frame via the same `jce_scene_load_json` helper used by the sync path. Cooperative cancel via atomic flag. |
| `jce_scene_recipe.h` | `jce_scene_recipe.c` | Strict semantic recipe/catalog validation and JSON ingestion. Schema v2 validates stable parent role/instance references and rejects missing, nonguaranteed, self, or cyclic hierarchy before compilation. |
| `jce_scene_compiler.h` | `jce_scene_compiler.c` | Pure deterministic compiler: canonical role ordering, independently derived per-role PCG32 streams, integer placement/quantization, stable entity/parent IDs, graph validation/topology, canonical hash, and FrozenPlan v2 wire format. |
| `jce_scene_transaction.h` | `jce_scene_transaction.c` | Explicit staging ownership and bounded build slices; creates all entities before attaching the existing flecs hierarchy, reads relations back against the plan, validates/prewarms before commit, retains the previous scene through a health frame, and rolls back without mutating prior ownership. |
| `jce_prefab.h` | `jce_prefab.c` | Prefab instantiate / override (sync) |
| `jce_lod.h` | `jce_lod.c` | LOD selection based on screen-space metrics |
| `jce_space_partition.h` | `jce_space_partition.c` | Spatial accel (grid/octree) used for culling + queries |
| `jce_scene_renderer_view_order.h` | `jce_scene_renderer_view_order.c` | Pure helper that orders bgfx shadow producer views before the scene color consumer view, preventing one-frame-late shadow sampling during camera motion |
| `jce_sr_depth_prepass.h` | `jce_sr_depth_prepass.c` | Pure policy: who wants the camera depth pre-pass this frame.  Four scene reasons (SSAO / SSR / TAA velocity / water) plus the active pipeline's `depth_prepass` flag, which nothing read until this existed.  Needs no `JceSceneRenderer` and no bgfx, which is what makes it testable; the LOW-tier floor still clears the pipeline flag. |
| `jce_scene_probe_capture.h` | `jce_scene_probe_capture.c` | Renders a reflection probe's six faces from the ACTUAL SCENE -- the bake filled them with a procedural sky gradient and said so under "v1 limitation". One 90-degree face per frame through the ordinary scene renderer + offscreen bridge, read back with the impostor bake's blit + `bgfx_read_texture` pattern. **MODAL**: it renders from the Scene View's base id and BOTH editor viewports yield while `jce_scene_probe_capture_in_flight()` (a full scene render claims 117 view ids; 256 views hold no third span). The Scene View DRIVES it (`jce_editor_scene_render.cpp`) because that is the one place per frame holding both the renderer and the scene. `jce_probe_face_basis` is the cube convention and is asserted against `jce_ibl_cube_direction`, the single authority. Fails to the old gradient rather than to nothing.  The component's `hdr` decides the STAGING FORMAT, and it must match the offscreen target's or the blit is undefined -- which is the whole of that feature: the scene was already being rendered into an RGBA16F target and the values above 1.0 were thrown away by an RGBA8 staging texture, one call before they would have been stored.  An adapter with no RGBA16F render target downgrades `g_cap.bpp` and the bake is told the format actually produced, never the one asked for. |
| `jce_scene_component_normalise.h` | `jce_scene_component_normalise.c` | The invariants a CALLER's component must meet before it enters the scene. Today: JceMeshRenderer's seven interned strings are never NULL. Every consumer assumed that and nothing enforced it, so `JceMeshRenderer mr = {0}` through the public setter segfaulted `jce_scene_serial_save` -- twice over (`strlen` on the pointer, and `mr->albedo_tex[0]` guards that dereference before they can decide). Established at the SETTER, not at the serialiser: the same NULL was reachable from a draw. |
| *(in `jce_scene.h`)* | `jce_scene_particles.c` | **P2-particle-vfx-runtime** ParticleEmitter component runtime: lazily owns one `JceParticleSystem` on the scene, loads each component's `*.particles.json` (`asset_path`) into a real emitter (legacy quick-tune fields drive a default when no asset), syncs emitter origin to entity world pos, steps the sim, mark-and-sweeps orphaned emitters. Driven from `jce_runtime.c` after `jce_scene_video_update`. Rendering is owned by `jce_scene_renderer.c` (`sr_draw_particles`, debug-line billboards) so editor + shipping share one render path. `jce_scene_internal.h` exposes the system slot accessor (private). |
| `jce_terrain.h` | `jce_terrain.c` + `jce_scene_terrain_cache.c` | Heightfield terrain algorithms plus the scene-owned loose-file cache bridge; cook tools link only the algorithm TU. |
| `jce_sequencer.h` | `jce_sequencer.c` | Cinematic / cutscene track sequencer |
| `jce_virtual_camera.h` / `jce_vcam_system.h` | `jce_virtual_camera.c` / `jce_vcam_system.c` | Virtual-camera value types compatibility TU + ECS-driven Cinemachine-style resolver |
| `jce_ui_canvas.h` | `jce_ui_canvas.c` | **P1-ecs-ui-canvas** ECS-UI (Unity-UGUI) renderer: layout (RectTransform anchors/pivot/sizeDelta + LayoutGroup H/V/grid) + render (UIImage simple/9-slice + UIText via FreeType/HarfBuzz atlas) + graphic raycaster (UIButton hover/press/click) for Screen-Space Overlay canvases. Draws transient quads into a caller-given bgfx view/FBO. NOT RmlUI (separate shipping HTML/CSS path). RectTransform is embedded in UIImage/UIText (no free component-flag bit). |

## Renderer modules (`jce_scene_renderer.c` split, 2026-06-23)

The scene renderer was decomposed from an 11.6k-line god file into cohesive
modules that all share `jce_sr_internal.h` (the `JceSceneRenderer` struct +
`Sr*` helper types + a `Shared internal renderer helpers (cross-module)`
prototype section). Pure move + `static`→external linkage — no behavior change.

| TU | Role |
|----|------|
| `jce_scene_renderer.c` | Core: model/texture/shadergraph caches, mesh resolution, light-dir helpers, material registry, public API, `jce_scene_renderer_render` frame orchestration |
| `jce_sr_internal.h` | Shared renderer state (struct + `Sr*` types + cross-module helper prototypes). **Internal, never installed.** |
| `jce_sr_batch_tracker.h` | Pure, unit-tested single draw-group detector used to bypass redundant batch sorting |
| `jce_sr_gpu_policy.h` | Pure, unit-tested workload crossover policy for adaptive GPU Scene / MDI dispatch |
| `jce_sr_prim_material_memo.h` | Pure signature contract for the frame-local primitive-instancing material memo |
| `jce_sr_draw.c` | Colour/shadow instancing batches, GPU-driven draw, per-entity model/material draw, entity-render pass |
| `jce_sr_anim.c` | Sprite animator, avatar mask, frame events, IK (two-bone/foot/full-body), ragdoll override, retargeting, skinned-anim eval, and the per-instance morph-VB lifecycle. Owns the animator-SELECTION memo (`sr_asel_n`/`sr_asel_i`), which is why the blendshape resolve LOOP stayed here when the rest of that pass left |
| `jce_sr_morph.c` | The blendshape pass: sample the clip's weight track, deform each morph-bearing primitive, hand the draw path the deformed VB (`sr_morph_vb_cb`). TWO evaluators, one formula -- a compute dispatch where the backend has compute (`middleware/animation/jce_morph_gpu.h`), `jce_morph_apply` on this thread where it does not. The choice is made PER PRIMITIVE and BEFORE the output buffer exists, because a COMPUTE_WRITE buffer cannot be written from the CPU -- see `morph_vb_gpu_mask` |
| `jce_sr_shadow.c` | CSM helpers, directional + local shadow passes, caster cull, shadow spatial grid |
| `jce_sr_environment.c` | Sky submission, async IBL bake, water, vegetation scatter, tilemap, cloth, ToD/weather/decals |
| `jce_sr_cloud_atlas.c` | Cancellable low-priority cloud-density CPU bake and render-thread GPU publication; keeps the prior resident atlas until replacement succeeds. |
| `jce_sr_foliage_cooked.{c,h}` | Validated cooked foliage placement loading from loose assets or PAK, with deterministic scatter fallback. |
| `jce_sr_cull.c` | TAA prev-xform table, SSAO/velocity depth prepass, light selection, uniform-grid frustum cull, baked-GI consume |
| `jce_sr_terrain.c` | Terrain LOD cache + chunk draw |
| `jce_sr_particles.c` | CPU particle vis + GPU particles |
| `jce_sr_ribbon.{c,h}` | LineRenderer / TrailRenderer camera-facing triangle ribbon, and the `material_path` -> base-colour image resolution it samples. The header holds the two DECIDING halves (material -> image, arc-length U) with no bgfx include, so they are asserted headlessly; the submit half is in the `.c`. |

New renderer code goes in the relevant module; cross-module statics are promoted
into the shared-helpers section of `jce_sr_internal.h`.

## Rules

1. **flecs is private.** Never include `<flecs.h>` from a public `<jce/...>` header. Components in public headers are POD structs; registration is internal.
2. **Add a component checklist**:
   - Define POD struct in `<jce/middleware/scene/jce_scene.h>` (or a focused sub-header).
   - Register in `jce_scene.c`.
   - Add JSON (de)serializer in `jce_scene_components_json.c`.
   - Add editor inspector drawer in `editor/src/panels/jce_panel_inspector.cpp` + reflection in `editor/src/core/jce_reflect_builtin.cpp`.
3. **No game logic.** Components are *data*. Behaviors that act on them live in other middleware (physics, animation, ai) or in the consumer game.
4. **Querying**: provide convenience iterators in public headers if needed; do NOT expose `ecs_query_t*`.
5. **Math** = `jce_math` (column-major mat4 in Transform).
6. **Allocations** via `jce_alloc`.
7. **vcam blending** uses `jce_camera` from renderer for the final transform.
8. **Baked GI consumption (P1-baked-gi-consume)** lives in `jce_scene_renderer.c`. Each frame `sr_gather_baked_gi()` picks the camera-nearest baked `ReflectionProbe` (`baked_cubemap_path` loaded once via `jce__ktx_load_cubemap`, cached on `sr`) and `LightProbeGroup` (`sh9`). `sr_bind_baked_gi()` overrides the IBL sampler stages 6/7 with the probe cube (+ `.irr.ktx` sidecar for diffuse, scaled by `u_giParams.y` intensity) and uploads `u_sh9` (9 vec4) + `u_giParams` to `fs_pbr.sc`. SH9 supplies diffuse ambient; the probe supplies specular. No new sampler stage — it reuses the runtime-IBL bind path. Shader treats unset `u_giParams.y` (bgfx zero-clear) as 1.0 so non-GI draws are unaffected.
9. **Generated-scene determinism** is local and versioned. Never use request
   arrival order, wall-clock time, pointer values, host byte order, or a shared
   cross-role RNG stream as layout entropy.
10. **Generated hierarchy is two-pass and verified.** Create every candidate
    entity first, resolve parents by stable ID second, use only the existing
    scene parent API, and reject any readback mismatch before activation.

## Don't

- Don't bake gameplay rules into terrain/sequencer/vcam — keep them generic.
- Don't add a second ECS or scene graph.
- Don't expose flecs handles to consumers.

## jce_ui_canvas.c: the RectTransform input space is Unity UGUI (2026-08-29)

`uc_resolve_rect` takes UGUI in and produces draw space out, and they are not the
same space:

* IN  — anchors are fractions of the parent from its BOTTOM-left, anchoredPosition
  is pixels with **+Y UP**, pivot names the point of the element's own box that
  anchoredPosition addresses.
* OUT — pixels, origin TOP-left, **+Y DOWN** (the `JCE_VIEW_UI` ortho).

The flip between them was missing: anchor.y and anchoredPosition.y went straight
into the top-left space, so `anchorMin/Max.y = 1` (Unity's TOP) resolved to the
BOTTOM of the screen. All 844 UI rects authored in this tree use a UGUI idiom, so
every authored HUD landed on the wrong edge — in the editor AND in the shipped
runtime. Nothing was authored in the old reading, which is why the repair needed
no content migration.

The stretch branch was wrong for the same reason: Unity's relation is
`size = anchorSpan*parentSize + sizeDelta` with
`rect.min = anchorMin*parentSize + anchoredPosition - sizeDelta*pivot`, so a
negative sizeDelta INSETS. The old code subtracted sizeDelta and ignored the
pivot, growing what Unity shrinks.

Do NOT flip anything downstream. `uc_apply_layout_group`, the ScrollView offset
and `uc_draw_text` all operate on the already-resolved screen rect and are
correct in +Y-down. There is exactly ONE flip and it is the last line of the Y
solve in `uc_resolve_rect`.

`tests/middleware/scene/test_jce_ui_rect_transform.c` pins all of this through
the headless raycaster. Before it, every UI test pinned anchor/pivot to {0,0}
with a full-height rect — the one configuration where both readings agree — so
the suite endorsed the bug instead of catching it.

Sprite lookup (`uc_get_texture`) uses the same three-tier ladder as `uc_get_font`:
project asset root → active VFS → canvas pak. Pak-only meant a project sprite was
looked up in `editor_assets.pak`, missed silently, and drew a flat quad — or, on a
path collision, drew the EDITOR's picture.

## uc_is_ui_element decides who EXISTS, not who draws (2026-08-29)

`uc_layout_draw` only walks INTO children that pass `uc_is_ui_element`.  An
entity that fails it is not merely undrawn: it is not laid out, not raycast,
and NOT RECURSED INTO — its whole subtree of real UI disappears with it.

The predicate listed the eight graphic/widget components and nothing else, so
three shapes were silently deleted:

* an empty node carrying only a **CanvasGroup** — Unity's fade/gate container;
* an empty node carrying only a **LayoutGroup** — Unity's "empty GameObject
  with a Vertical Layout Group holding a column of buttons"; the group never
  ran AND every child vanished;
* an entity carrying only a **UIButton**, which was also the one widget with no
  RectTransform of its own.

All three are offered unrestricted by Add Component and drawn in full by the
Inspector, so they were authorable and inert.  A container with no graphic
resolves a NULL rect and `uc_resolve_rect` substitutes a zeroed one = full
stretch of the parent, which is the container semantic; UIButton now has its
own rect, resolved LAST in `uc_entity_rect` so a sibling UIImage still wins and
no already-authored scene moves.

`uc_collect_canvas_cb` took EVERY Canvas and rendered each against the whole
framebuffer.  A Canvas nested under a UI element therefore escaped its parent's
rect, and if it also carried a graphic it was drawn and raycast TWICE.  It now
skips a Canvas with a Canvas ancestor, which is Unity's rule.

`tests/middleware/scene/test_jce_ui_rect_transform.c` pins both.  Negative
control run: reverting the predicate makes the container test read
`Expected 458 Was 0` — the button under the container is wholly unreachable.

## The reservation table cannot yet catch a MOVED band (2026-08-29)

`JCE_VIEW_SR_OFFSET_RESERVED()` is assembled from the SAME live constants the
order builder uses, so the two cannot disagree about a band's WIDTH — a
negative control that changed `JCE_VIEW_SR_DYN_CSM_COUNT` changed both and the
test stayed green.  `test_jce_view_reservation_table.c` therefore catches a
band added to the builder as a LITERAL (verified: injecting `base+70u` makes it
fail by name) and nothing else.

The fix, unanimous across three independent design reviews, is a FROZEN
append-only reservation table whose constants are independent of the builder's.
Not done.

## The raycast and the draw must derive from the same rect (2026-08-29)

Three defects in `uc_layout_draw` / `uc_update_*` were all the same mistake:
the DRAW side honoured something the RAYCAST side did not.

* A ScrollView clipped its descendants with a view-level bgfx scissor, but the
  hit record was the child's own rect with no intersection against any ancestor
  viewport — a widget scrolled out of view stayed clickable where it is not
  drawn.  The clip is now threaded through the recursion and a fully-clipped
  widget records NO hit.
* An expanded dropdown's popup rows were drawn on top and handled by
  `uc_update_widgets`, but were never in the hit list — and `uc_update_buttons`
  resolves from that list and runs FIRST, so picking an option also pressed
  whatever was underneath.  The popup is now published as a frame-level modal
  rect BEFORE the renderer gate (a headless canvas must block too).
* `UIButton`'s state colours were applied by re-drawing the sibling UIImage, so
  a button whose entity has no image ran the whole click machine and showed no
  feedback.  It tints its own resolved rect when there is no image.

`sv` was also fetched twice with two different enable-gates, so a DISABLED
ScrollView stopped drawing and kept offsetting and clipping.

METHOD NOTE, worth more than any of the fixes: the first version of the popup
test used a click point that was ABOVE the button entirely, and it PASSED with
the modal gate disabled — it asserted nothing.  Only running the negative
control caught it.  The point is now derived from the rects in a comment.  Any
test written against this file should state where its coordinates come from.

## CORRECTION: the ScrollView draw was never clipped (2026-08-29)

The previous entry said the DRAW side "has always clipped" and only the raycast
did not.  That was wrong, and an adversarial sweep of the vendored bgfx proved
it: `bgfx_set_view_scissor` writes a single per-VIEW value that bgfx copies once
per frame at `Context::swap`; the renderer reads one scissor per view and it is
NOT recorded per submit.  This file set it before a ScrollView's subtree and
RESET it to (0,0,0,0) after, in the same frame — so the reset is what reached
the renderer, every frame, and nothing was ever clipped.

`bgfx_set_scissor` is the per-DRAW primitive: consumed by the next submit,
discarded with the rest of the draw state, so it must be re-armed before EVERY
submit.  There is now a file-static current clip and two wrapper macros around
the only two renderer calls the canvas's quads go through, so a new draw site
cannot forget it.  11 call sites route through them.

TEXT IS STILL NOT CLIPPED.  Glyph runs go through `jce_text_draw_*`, which
submits per glyph inside the renderer where this file cannot re-arm the
scissor.  A text row scrolled out of a ScrollView still draws.  The RAYCAST is
clipped either way, so it is not clickable — the half that silently does the
wrong thing is correct, and the half that is merely ugly is written down.

## CORRECTION: base+57 was a new two-owner id (2026-08-29)

Moving the GPU-cull counter reset off base+3 (SSAO's blur) put it on base+57,
and 3 + 57 is 60 — `JCE_VIEW_EDITOR_PREVIEW`.  The commit that removed one
two-owner view created another, against an ABSOLUTE id, which is the one axis
no per-base static_assert reaches.  Now 63 (free at every live base), with 57
kept in the frozen set: append-only earning its keep the first time a band
actually moved.

`tools/audit/check_view_reservations.py` gained the rule that catches it, and
two rules it had been missing — it parsed only two of the contract's four
sections and tested three of the seven fixed ids it records, so half the
contract was decorative.  Negative control: putting the reset back on 57 makes
it fail by name.

## `jce_scene_environment.c` — THE environment driver (2026-09-01)

`JceEnvironmentState` was already scene-owned; what was not was the thing that
makes it tick. `jce_environment_advance` had **exactly one caller in the
repository** and it sat in the scene RENDERER (`sr_advance_environment_state`).
A headless build creates no renderer, so on a dedicated server the clock never
moved, `global_wetness` / `snow_amount` — integrators — integrated nothing, and
nothing placed the sun, so `jce_environment_is_daytime()` answered **true
forever**.

Time of day additionally had **two** clocks: the renderer's private
`tod_clock_hour`, and the editor advancing the AUTHORED `tod_hour` in place from
ImGui's frame delta (which also marked the scene dirty every frame, so Ctrl+S
baked the current hour into the saved scene).

Now: this file syncs the authored settings into the environment, unifies the day
clock onto the `world_time_seconds` / `day_fraction` / `seconds_per_day` the
state already carried, places the sun, and advances. `jce_scene_update()` calls
it for the runtime and headless; the editor calls it in edit mode. The renderer
only reads. Guarded by `tools/lint/check_environment_authority.py`.

## 2026-09-01 — reparent 到一个已占用同名的作用域，观察到进程死亡（机制未明）

`jce_scene_create_entity` 只在**创建时的作用域**里去重（`jce_scene.c:1176` 自己的注释写明
「同作用域重名会 ABORT 进程」）。`jce_scene_reparent` 把实体挪进**另一个**作用域时，
不再检查名字。

**已确立的观察**：`tests/application/test_jce_runtime_vehicle_bridge.c` 里四个都叫 "wheel"
的实体挂到同一个底盘上，进程在**第二次** `jce_scene_set_parent` 时死亡（exit 127，无输出）；
改成 wheel0..wheel3 即通过。两个方向各复现两次。

**机制未确立**：`tests/middleware/scene/test_jce_scene_reparent_name_clash.c` 依次加了
①两个同名子 ②四个 ③reparent 后各带 WheelCollider ④父带 BoxCollider + 启用的
VehicleComponent ⑤先建静态 ground，并改为链接完整 `JCE`——**五种都通过**。
所以那个文件不是这个崩溃的回归测试，它是一道**行为守卫**（同名兄弟必须能共享父实体、
两者都仍可寻址、授权名必须保住）。否定结果列在它的头注释里，不要重跑。

下一步应当是拿一个 debug 构建接住那次 abort 读栈，或在 `scene_set_parent_unchecked`
的 `ecs_add_pair` 前加一次 `ecs_lookup_child` 探测把条件缩小。

## 2026-09-01 — 下拉框与输入框已移出 `jce_ui_canvas.c`

该文件曾 3102 行（超 3000 上限、被体量门冻结，以致当天一处七行的注释更正必须压回五行）。
移出 242 行后是 **2864 行——低于上限**。

新增 `jce_ui_canvas_widgets.c` / `.h`。**边界刻意做窄**：3 个绘制原语进、7 个函数出、
1 个矩形类型，外加两个访问器 `uc_renderer()` / `uc_caret()`——
`struct JceUICanvas` 仍然私有，为了两个字段把四十个字段公开出去，
是把边界拓宽而不是收窄。

**没有选那个更大的块**：交互状态机（按钮/滑块/开关）有 555 行、更诱人，
但它需要 `UCFrame`——画布的整个逐帧状态——边界会切在文件内部而不是接缝上。

## 2026-09-06 — 给一个镜头起名字：`jce_vcam_system_set_active_by_name()`

`JceVirtualCameraComponent.vcam_name` 授权、序列化、显示在 VCam Manager 里，而
**`engine/src` 下没有任何东西读它**——选择按 priority，所以名字只是面板上的一个标签。
「切到叫 BossIntro 的那台相机」——过场、触发器、以及**经 SDK 授权场景的 AI CLI**
最需要说的那一句——根本无法表达。

**覆盖住在系统里，不写回组件。** 显然的实现（把选中相机的 `priority` 抬高）会把玩法状态
写进授权场景：编辑器标脏、Ctrl+S 把过场的镜头选择烤进关卡、结束过场还得从别处记起旧数字。
所以它在 `VcamState` **里面**（不是旁边——那会让本文件的文件级可变计数 +1，dedup 棘齿会红）。

三条不变量，改这里之前先读：
1. **被点名的相机不会被复活**：inactive 或组件被禁用的相机不会因为被点名就赢——
   作者取消勾选 Active 是一句话。选择回落到 priority。
2. **记录是无条件的，返回值才是「此刻是否解析得到」**——一个还没流式加载进来的分块里的
   相机，不能因为「现在找不到」就静默变成「按 priority 来」。
3. **`jce_vcam_system_reset()` 必须清掉它**（正因为第 2 条允许点名一台还不存在的相机，
   它绝不能跨场景存活，否则上一关请求的切换会劫持下一关同名的第一台相机）。

## 2026-09-20 — 脚本公开参数：**每一行都把三个值全写出来**，哪怕 kind 只认其中一个

`JceScriptComponent` 现在带 `params[JCE_SCRIPT_PARAM_MAX]`（Unity 的
`[SerializeField]` / Godot 的 `@export`）。一行参数 = 名字 + kind
（number / bool / text / entity）+ **每种 kind 各自的值槽**。

**`ser_script` 无条件写 `number`、`entity`、`text` 三者。**
只写 kind 点名的那一个会通过一次朴素的往返测试（每种 kind 各自留住自己的值），
但会在**唯一要紧的那次编辑**上丢数据：作者在 Inspector 里把一行从 Number 改成 Text
再改回来，原来打的数字就没了。结构体把三个槽分开正是为了这件事不会发生，
序列化器不许把它合回去。

**空数组整个不写。** 没有参数的脚本序列化出的字节与这个特性存在之前**逐字节相同**——
否则全仓库既有场景会集体显示为已修改。`parse_script` 缺 `"params"` 键就是 `param_count = 0`，
而 memset 已经给了这个值，所以「没有」不花任何代价也不表示别的意思。

**ENTITY kind 的那一行是跨实体引用**，走 `jce_scene_components_json.c` 里
loader 的 `src_id -> new_id` 映射，和这里每一条引用一样；解析不到就变 0
（把炮塔指向一块石头，比指向虚无更糟）。它**没有**走 `refs64[]`：那个数组是按
「一组固定的单引用」定尺寸的，而这里的条数是**逐实体数据**。

**两端都夹 `param_count`。** 组件可经 Automation API、经脚本、经手改场景文件到达，
所以序列化端的夹取是必须的——没有它就是从一个八元素数组外面读。
测试断言的是**存盘文本里的行数**（数 `"number"` 键，它是这五个键里唯一没有别的组件在用的），
因为回读端也夹，会诚实地报回一个看上去合法的 8。

**测 `src_id -> new_id` 映射时，不要用 load 路径。** `jce_scene_serial_load` 是**替换**语义
（先销毁调用方的全部实体），所以目标场景在加载开始时是空的、按序列化顺序发 id，
**必然重现源场景的 id**。⇒ 在这条路径上「引用回来仍指向 Hero」这句断言，
**在有没有 remap 两种情况下都成立**，它不判别任何东西——源场景里先造再销毁一批实体去「错开 id」
也没用（实测：Hero 回来还是原 id）。

判别它的是同一份 map 的**另一半**：`jce_prefab_instantiate` 是 **append** 路径，
目标场景里已经有实体，id 从构造上无法对齐。把一个 prefab 实例化**两次**、
断言每个实例的引用指向**它自己那份**拷贝——「map 建一次反复用」与「按源 id 解析」
这两种错法都会让两个实例共用第一份，而这在 load 路径上永远看不见。
（`tests/middleware/scene/test_jce_script_params_roundtrip.c` 两种都有；
这条是变异对照照出来的，不是设计时想到的。）


## 2026-09-21 — 缺的从来不是机器，是**词汇**；以及**链接器报的是设计问题**

`JceSeqPropId` 枚举了十五个属性，**没有一个能指向 UI 元素**，于是
`jce_seq_prop_from_name` 把每一条 UI 轨道都解析成 `JCE_SEQ_PROP_NONE`，
一个菜单淡入只能靠脚本逐帧驱动。而 sequencer 一直在跑、缓动库一直接着、
编辑器的属性选择器**是遍历枚举自己长出来的**（`jce_panel_sequencer.cpp:905`）——
所以补这十一个属性，编辑器**一行都不用改**。

**这一类缺陷不留任何痕迹**：没有红测试、没有红门、没有错的数字。
它只是「这件事说不出来」，于是全树的 UI 动画都是脚本，而没人写下为什么。

**每一个属性的读者都是在加它之前查的，不是之后。**
`cg->alpha` 在 `jce_ui_canvas.c:1489` 被乘进子树继承 alpha；
`uc_resolve_rect` 读 `anchored_position`；`uc_elem_xform` 读 `rotation_deg`/`scale`；
`uc_color` 读两个 tint。**没有读者的属性 = 一条在动一个数字的轨道。**

**`uc_entity_rect` 搬进了 `jce_ui_rect_lookup.c`，是链接器逼的，而它逼得对。**
从 sequencer 调它，把整个 `jce_ui_canvas.c.obj` 拖进了每个链 `jce_scene` 的目标，
于是 `uc_draw_text` 里的 `jce_loc_t` 无法解析：

```
jce_scene.lib(jce_ui_canvas.c.obj) : error LNK2019: unresolved external
    symbol jce_loc_t referenced in function uc_draw_text
```

**静态库按 object 链接**，所以一个五行的查询让调用方付了一整个子系统的钱。
「这个实体带的是哪个 RectTransform」是**关于场景**的问题，却只能从渲染文字的
模块里问出来——修法不是多链一点，是**把问题放到它答案来的地方**。
顺带：那份**组件顺序只有一份**（UIButton 排最后是有代价换来的知识），
抄第二份不会报错，只会**让轨道去动一个没人画的 rect**。

**`transform.scale.uniform` 的不对称在这里重演**：读 x、写两个轴。
写一半不会失败，会把一次等比 pop 播成横向拉伸——看起来像作者键错了轨道。

## 2026-09-21 — `JceRectTransform.scale` 的 0 = **未缩放**，sequencer 不为它开特例

每一个在该字段存在之前序列化的 rect 都把它读成 0，字面 0 会画不出东西——
那看起来像元素消失而不像默认值。所以 **0 表示「未缩放」**。
代价落在作者身上：**键 0 → 1 的 pop 在 t=0 播的是原尺寸**，要从 0.01 起键。

**不在 applier 里翻译它**：一个字段按写它的人不同而有两种含义，
正是这棵树反复付账的那类缺陷。`get_float` 也返回**原始存储值**——
它的职责是编辑器预览的快照/还原，而一个把 0 写回成 1 的还原**屏幕上一模一样、
却悄悄改了场景文件**。**会改变字节的往返不是往返。**


## 2026-09-21 — 检视器：**它要找的缺陷和它自己最坏的缺陷是同一幅画**

场景渲染器手里有十几张中间纹理（深度预通道、normal/albedo/velocity G-buffer、
阴影图与级联图集、天空 LUT），**没有任何东西能看其中任何一张**——
于是一张错的 G-buffer 只能表现为一张错的最终画面，而那是最难查的位置：
下游每一个 pass 都是嫌疑人。

**用「拉」不用「注册表」。** 渲染器自己拥有每一个句柄，所以它直接回答；
推式注册表要每帧付费（不管有没有人在看），而且它是**同一批事实的第二份清单**——
这棵树里任何第二份清单最终都会和第一份不一致。

**有效位就是全部设计。** 每张纹理旁边都已经有一个「这一帧里它是不是真的有像素」的
标志，全部被遵守。**一个过期的目标不会显示空白，它会显示上一帧的内容**，
或者一块充满看起来很合理的噪声的未初始化分配。
⇒ 因此这张表**逐帧改变长度**，这是设计：某一项出现 = 那个特性打开了。

**`ssao_valid` 是陷阱本身。** 它只说渲染目标**已分配**，而该目标是**跨帧保留**的，
所以 SSAO / SSR / TAA 全关之后那个句柄永远有效——`jce_sr_internal.h` 自己的注释
记着只测句柄的代价：**水体对着早已移动过的几何做吸收，而在切换的那一刻毫无症状**。
真正的判据是 `depth_prepass_frame`。

**我自己的测试第一次跑就抓到我自己的缺陷。** velocity 那一行当时没有要求
`depth_prepass_frame`，而是依赖 `jce_sr_cull.c` 里 :656 先于 :710 的顺序——
**那是调用图的性质，不是标志的性质**，在这两行之间加一个 early return
就会让 velocity 宣称一个从未完成的帧。现在那一行**说出它的意思**，而不是依赖别处。

**表可以在无 GPU 下测**，因为 `jce_sr_debug_targets_build` 收的是一个普通 state 结构体
而不是 `JceSceneRenderer`（后者持有 bgfx 资源、单测里造不出来）。
那个 state 是**参数对象，不是第二份目标清单**：表只存在一份。
唯一可能漏的是 `state_from_renderer` 里的逐字段拷贝——所以测试钉了一份
**手写的全量名字清单**：加了一行却没把句柄接出来的目标会停在 `UINT16_MAX`
被静默丢掉，**在编辑器里永远不出现而没有任何东西报错**，金名单就是发现它的那个。

**每条断言都是关于「不该出现」的。** 只测 happy path 的测试，在一张**无视每一个
标志**的表上照样全绿。

## 2026-09-21 — 文件 clip 缓存：**两个生命周期点都要释放，两个解析器都要回退**

`SrAnimInstance` 新增 `file_clips[SR_FILE_CLIP_MAX]`（`<骨架目录>/<名字>.animclip.json`
解析出来的 clip）。这是**唯一真正的风险点**：文件 clip **没有别的 owner**
（模型 clip 归模型所有），所以它必须在**两处**实例生命周期点释放——
`:184` 模型切换 与 `:238` 槽位回收，**就是 retarget map 与 aoc 已经在释放的那两处**。
**往这个结构体里加资源，两处都得加**，否则会在作者想不到的那条路径上泄漏。

**两个解析器都要回退，不能只改一个**：`sr_sm_state_model_clip` 与
`sr_find_model_clip` 都要在模型里找不到名字时落到文件 clip。
只改一个 = 作者在两处填同一个名字、只有一处生效，比完全没有更糟。

**读取必须 PAK 优先、再走 host 路径**，与 `sr_anim_load_sm` 同序。
理由就写在同一个文件里、相隔三百行：只走 `resolve_path->host` 的读法
在单 exe 构建里**什么都读不到**（没有散装 cooked 树）。
第一版就是 host-only，那会做出一个「编辑器里对、出货游戏里静默返回 NULL」的特性。

**miss 也要缓存**：两边都没有的名字否则每帧每实体 stat 一次文件系统。
