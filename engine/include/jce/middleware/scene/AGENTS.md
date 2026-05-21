# engine/include/jce/middleware/scene — Scene public headers (L4)

Public API surface for the ECS / scene subsystem.  Consumed via
`<jce/api_scene.h>`.

## File map

| Header | Role |
|--------|------|
| `jce_scene.h`                   | Components, entity helpers, hierarchy, world iteration. |
| `jce_scene_components_json.h`   | JSON (de)serializer entry points for every public component. |
| `jce_scene_async.h`             | **P3-A.3** — async prefab/scene instantiate (`InstantiateAsync` / additive `LoadSceneAsync` parity).  Worker thread parses; main thread commits one payload per `JCE_PHASE_EARLY_UPDATE` tick. |
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

## P3-E.5 — Light Cookies + IES Profiles

Spot lights ship end-to-end (cookie texture + IES LM-63 photometric LUT) via new `JceSpotLight` fields (`cookie_texture` / `ies_lut_texture` / `cookie_strength` / `cookie_path` / `ies_path`) and matching `JceSpotLightDesc` fields. Directional cookie is data-side scaffolded (component, serializer, inspector, uniform upload) with the shader projection guarded `#if 0` until a CSM-aligned VP is wired. Sampler slots **13 = `s_cookie`** and **14 = `s_iesLut`**; v1 binds the FIRST spot light that has a cookie / IES (multi-cookie atlas deferred to P3-E.5b). New public header: `<jce/renderer/jce_ies_profile.h>` (`jce_ies_parse`, `jce_ies_bake_lut_from_file`, `jce_ies_bake_lut_from_memory`). **Rebuild shaders**: `fs_pbr.sc` adds samplers + `u_cookieParams` / `u_cookieSpotVP` uniforms — run shaderc against `engine/shaders/pbr/fs_pbr.sc` to refresh `_cooked/*/shaders/*/fs_pbr.bin`.
