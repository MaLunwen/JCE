# engine/include/jce/middleware/scene — Scene public headers (L4)

Public API surface for the ECS / scene subsystem.  Consumed via
`<jce/api_scene.h>`.

## File map

| Header | Role |
|--------|------|
| `jce_scene.h`                   | Components, entity helpers, hierarchy, world iteration. |
| `jce_scene_identity.h`          | Standalone opaque scene/entity handles and scene-owned cache generations, dirty observations, and invalidation signals. Re-exported by `jce_scene.h`; depends only on core definitions and C standard types. |
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

`jce_scene_stress.h` exposes diagnostic subtree generation and scene-local
bounded motion to editor and SDK Player consumers. It has no renderer, host,
project or third-party type in its descriptor. Requests report actual counts
and fail explicitly at resource/primitive limits.

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

WaterSpectrum is an optional versioned component in the existing ECS registry,
not an expansion of the unversioned Water ABI. Its append-only registry row
preserves old dense IDs. Water retains wind/seed/fetch/swell ownership; PM/TMA
and directionality are configured before FFT evolution. Shared scene acquisition
uses the extension only for enabled FFT bodies; absent, disabled and non-FFT
bodies keep legacy selection. WaterField rebuilds commit only complete resources
and preserve borrowed field addresses and the single scene clock. CPU regression
coverage is not proof of current GPU pixels or low-end visual acceptance.

WaterFoam's additive versioned component is declared in jce_scene_water_foam.h
and included by api_scene.h. Rates are in inverse seconds and detail scale in
inverse metres. It requires an enabled versioned FFT WaterSpectrum; absence or
disable keeps old water rendering. Its history never changes the ambient
absolute-time sampling contract. It is transient simulation state, not scene
JSON state; JSON retains only authoring parameters.

WaterCrest is an optional versioned optical component in jce_scene_water_crest.h. It does not change the Water layout, spectrum sampling or the simulation clock.

WaterCaustics opts a physical FFT body into RGB caustic projection. It is additive and uses the existing Water clarity and caustic strength.

WaterNearshore is an optional versioned FFT-water component with a scene-local
terrain reference and a world-space square simulation region. Terrain zero
leaves the extension inert. The published composite surface query includes wet
coverage and leaves its outputs intact when no current sample is available.

WaterMotion is an optional versioned FFT authoring component in
jce_scene_water_motion.h. A zero cascade fraction selects one FFT. The phase
rate, horizontal strength and metre cap default to 1, 1 and 0 respectively.
The public Water and WaterSpectrum component layouts stay unchanged.

WaterOptics adds versioned strengths for screen-space reflection and scene-color
refraction. Both default to zero. Its registry row is append-only and the
unversioned Water layout remains stable.

WaterCausticDetail is an optional versioned extension of the projected caustic
effect. Zero secondary weight and tile size keep single-scale, single-sample
projection. The WaterCaustics component retains its existing layout.
The fine layer uses a fraction of the primary period, with zero period ratio
and weight by default. Its fade depth defaults to 1.25 m. Double-scale JSON
without these three fields retains the same behavior.

The physical sky accepts optional scene-level celestial controls through
jce_scene_sky_celestial.h. These settings retain the existing rendering and
environment ABI and use the existing scene time-of-day directions.

WaterNormalDetail is an optional versioned FFT-water micro-normal component in
jce_scene_water_normal_detail.h. Its two independent inverse-metre noise scales
retain their authored order in [0.1,64], defaulting to 6 and 16; slope strength
is [0,1], default 0.14. Absence/disable retains legacy detail; strength zero
explicitly suppresses this extension's micro-normal. Existing Water/WaterMotion
layouts are unchanged. Set/get/remove follow scene-owned main-thread storage.
