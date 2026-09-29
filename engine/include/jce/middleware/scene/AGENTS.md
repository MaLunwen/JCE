# engine/include/jce/middleware/scene — Scene public headers (L4)

Public API surface for the ECS / scene subsystem.  Consumed via
`<jce/api_scene.h>`.

## File map

| Header | Role |
|--------|------|
| `jce_scene.h`                   | Components, entity helpers, hierarchy, world iteration. |
| `jce_scene_camera.h`            | Strict single-primary scene-camera world-pose resolution and render-camera binding. |
| `jce_scene_components_json.h`   | JSON (de)serializer entry points for every public component. |
| `jce_material_override.h`       | Per-renderer overrides of a shared material (Unity's MaterialPropertyBlock): the `JCE_MR_OVERRIDE_*` bits of `JceMeshRenderer.material_override_mask`, and `jce_mesh_renderer_apply_material_pbr()` -- the single authority for "copy the material's factors, except the ones this renderer claims".  Every load path must go through it; three hand-written copies existed and two of them had no guard at all. |
| `jce_scene_fullscreen_effect.h` | Generic, presence-gated scene descriptor for project full-screen shaders. Fixed ABI: bounded shader/texture paths, four typed sampler slots, sixteen `vec4` parameters, insertion/blend/output intent, and no project-specific semantics. New components use the dense registry (`legacy_flag = 0`), not another bit flag. |
| `jce_scene_async.h`             | **P3-A.3** — async prefab/scene instantiate (`InstantiateAsync` / additive `LoadSceneAsync` parity).  Worker thread parses; main thread commits one payload per `JCE_PHASE_EARLY_UPDATE` tick. |
| `jce_scene_recipe.h`            | Strict, bounded semantic recipe and catalog contracts. Schema v2 adds stable parent-role/instance references and rejects paths, scripts, unknown fields, missing parents, nonguaranteed parent instances, cycles, and version drift. |
| `jce_scene_compiler.h`          | Deterministic recipe-to-`JceSceneFrozenPlan` compiler. FrozenPlan v2 carries stable entity/parent IDs, validates acyclic topology, emits deterministic parent-first order, and uses a canonical little-endian wire format and fixed-point hash. |
| `jce_scene_transaction.h`       | Isolated two-pass entity/hierarchy build, plan-to-live relation readback, validation/prewarm gates, generation-guarded frame-boundary commit, one-frame health gate, post-commit cancellation lockout, and rollback ownership. |
| `jce_prefab.h`                  | Synchronous prefab save / instantiate (kept; the async path is purely additive). |
| `jce_lod.h`                     | LOD selection thresholds. |
| `jce_space_partition.h`         | Spatial accel (grid / octree). |
| `jce_terrain.h`                 | Heightfield terrain. |
| `jce_sequencer.h`               | Cinematic timeline. |
| `jce_virtual_camera.h` · `jce_vcam_system.h` | Cinemachine-style vcams + blender. |
| `jce_scene_systems.h`           | **P3-B.5** — read-only introspection of the active scene's flecs systems (name, group, `time_spent` ms, matched entity count, enabled flag) plus `jce_scene_set_system_enabled`. Editor's Systems panel consumes this without ever touching `<flecs.h>`. |

## Rules

1. **flecs stays private.**  POD components only — no `ecs_world_t *`,
   `ecs_entity_t`, or `<flecs.h>` includes here.
2. **Sync + async live side-by-side.**  `jce_scene_async.h` did NOT
   replace `jce_prefab.h` / `jce_scene_serial.h`; both paths reuse
   `jce_scene_load_json()` as the single parse helper.
3. **Async API is scene-implicit.**  `jce_scene_instantiate_async()`
   takes no `JceScene*` — the target is set once via
   `jce_scene_async_init(target, fs)`.  Callbacks fire on the main
   thread inside the player-loop tick.
4. **No new ECS / scene graph.**  Extend the existing model.
5. **AI stays semantic.** Provider output may select bounded capability IDs and
   placement intent only. Asset paths, scripts, ECS commands, and raw component
   payloads never cross the recipe boundary.
6. **Frozen plans are the replay unit.** Network/cache/replay paths consume the
   canonical frozen plan and must not repeat inference.
7. **Hierarchy is the existing scene relation.** Generated plans identify
   parents by stable ID, but materialization must call `jce_scene_set_parent`
   and verify with `jce_scene_get_parent`; never add a parallel node graph.

## P3-E.5 — Light Cookies + IES Profiles

Spot lights ship end-to-end (cookie texture + IES LM-63 photometric LUT) via new `JceSpotLight` fields (`cookie_texture` / `ies_lut_texture` / `cookie_strength` / `cookie_path` / `ies_path`) and matching `JceSpotLightDesc` fields. Directional cookie is data-side scaffolded (component, serializer, inspector, uniform upload) with the shader projection guarded `#if 0` until a CSM-aligned VP is wired. Sampler slots **13 = `s_cookie`** and **14 = `s_iesLut`**; v1 binds the FIRST spot light that has a cookie / IES (multi-cookie atlas deferred to P3-E.5b). New public header: `<jce/renderer/jce_ies_profile.h>` (`jce_ies_parse`, `jce_ies_bake_lut_from_file`, `jce_ies_bake_lut_from_memory`). **Rebuild shaders**: `fs_pbr.sc` adds samplers + `u_cookieParams` / `u_cookieSpotVP` uniforms — run shaderc against `engine/shaders/pbr/fs_pbr.sc` to refresh `_cooked/*/shaders/*/fs_pbr.bin`.

## 2026-09-20 — `JceScriptComponent` 现在带公开参数（类型必须同住这里）

`JceScriptParamKind` / `JceScriptParam` / `JCE_SCRIPT_PARAM_MAX` 与 `params[]`
成员一起声明在本目录的 `jce_scene.h` 里，**不能拆到单独的头**——另一个头无法
声明本头所定义结构体的成员。这也是 `check_file_size.py` 那条 baseline 抬升
（4406 → 4449）记录的理由。

**一行参数带四个值槽（kind + number + entity + text），而不是一个 union。**
联合体会让「在 Inspector 里把一行从 Number 改成 Text 再改回来」丢掉作者打的数字。
序列化端无条件写全三个值，正是靠这个分开的布局。

**kind 的数值是 ABI 的一部分。** `jce_rt_script.c` 里有一条 C99 负长度数组把
`NUMBER/BOOL/TEXT/ENTITY == 0/1/2/3` 钉死：脚本头与场景头不能互相 include，
那个 TU 是唯一同时看见两者的地方，而一次静默的重新编号会把 TEXT 当 ENTITY 交给每个脚本。
