# engine/src/middleware/scene — Scene / ECS (L4)

> The ECS heart: components, prefabs, terrain, virtual cameras, sequencer, LOD, space partition.

## Identity

- **Layer**: L4. C99. Backed by **flecs** (private link).
- **Public umbrella**: `<jce/api_scene.h>` → `<jce/middleware/scene/jce_*.h>`

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_scene.h` | `jce_scene.c` | Scene container, world, root ECS components (Transform, Tag, etc.) |
| `jce_scene_components_json.h` | `jce_scene_components_json.c` | JSON (de)serializer for every public component |
| `jce_scene_async.h` | `jce_scene_async.c` | **P3-A.3** Async prefab/scene instantiate. Worker thread (jce_thread) does I/O + JSON parse; main-thread `dispatch_main` (wired to `JCE_PHASE_EARLY_UPDATE`) commits ≤ 1 parsed payload per frame via the same `jce_scene_load_json` helper used by the sync path. Cooperative cancel via atomic flag. |
| `jce_prefab.h` | `jce_prefab.c` | Prefab instantiate / override (sync) |
| `jce_lod.h` | `jce_lod.c` | LOD selection based on screen-space metrics |
| `jce_space_partition.h` | `jce_space_partition.c` | Spatial accel (grid/octree) used for culling + queries |
| `jce_terrain.h` | `jce_terrain.c` | Heightfield terrain + chunk LOD |
| `jce_sequencer.h` | `jce_sequencer.c` | Cinematic / cutscene track sequencer |
| `jce_virtual_camera.h` / `jce_vcam_system.h` | `jce_virtual_camera.c` / `jce_vcam_system.c` | Cinemachine-style virtual cameras + blender |

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

## Don't

- Don't bake gameplay rules into terrain/sequencer/vcam — keep them generic.
- Don't add a second ECS or scene graph.
- Don't expose flecs handles to consumers.
