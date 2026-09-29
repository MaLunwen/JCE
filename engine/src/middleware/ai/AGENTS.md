# engine/src/middleware/ai — AI (L4)

> Navmesh + behaviour trees + steering + A* + ECS agents.

## Identity

- **Layer**: L4. C99 + C++ bridge TUs.
- **Public umbrella**: `<jce/api_ai.h>` → `<jce/middleware/ai/jce_*.h>`
- **Deps**: Recast/Detour (navmesh), behaviortree_cpp (BT), flecs (agent ECS).

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_navmesh.h` | `jce_navmesh.c` + `jce_navmesh_recast.cpp` | Navmesh query/path; bridge to Recast/Detour |
| `jce_nav_agent.h` | `jce_nav_agent.c` | Per-agent navigation state + steering integration |
| `jce_bt.h` | `jce_bt.c` + `jce_bt_impl.cpp` (+ `jce_bt_impl.h`) | Behaviour tree façade over behaviortree_cpp |
| `jce_graph_astar.h` | `jce_graph_astar.c` | Generic graph A* (non-navmesh) |
| `jce_steering.h` | `jce_steering.c` | Boid/Reynolds steering primitives |
| `jce_ai_ecs.h` | `jce_ai_ecs.c` | flecs components: NavAgent, BTRunner, SteeringTarget |

## Rules

1. **C++ libs are wrapped.** All Recast/BT types stay inside `.cpp` bridges; public headers are pure C.
2. **Path query is cooperative async** — long queries via `jce_jobs` (don't block the main thread on a 10k-poly path).
3. **Navmesh build is editor/cooker-side**; runtime only loads + queries. Add new build code under `tools/` or `resource/`.
4. **No game-specific behaviors** — BT nodes shipped here are primitives (Selector/Sequence/Decorator/condition + action interfaces). Game-specific nodes live in `examples/caged_kingdom/`.
5. **Math** = `jce_math`. Convert at the Recast boundary.

## Don't

- Don't expose Detour query types in public headers.
- Don't write game-specific BT actions in this dir.
- Don't add a second pathfinder (A* + Detour covers our cases).
