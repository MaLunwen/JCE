# engine/src/middleware/world — World primitives (L4)

> Roads, spawners, weapons, triggers, weather, time-of-day, world ECS aggregation.

## Identity

- **Layer**: L4. C99. Backed by flecs (private).
- **Public headers** under `<jce/middleware/world/jce_*.h>`; consumed via `<jce/api_middleware.h>`.

## File map

| Header | TU | Role |
|--------|----|------|
| `jce_world_ecs.h` | `jce_world_ecs.c` | Aggregator that wires world-level ECS systems |
| `jce_road_network.h` | `jce_road_network.c` | Spline-based road graph for procedural worlds |
| `jce_spawn_manager.h` | `jce_spawn_manager.c` | Spawn rules + density / radius pop-in |
| `jce_trigger_volume.h` | `jce_trigger_volume.c` | OBB/sphere trigger volumes with enter/exit events |
| `jce_weapon.h` | `jce_weapon.c` | Weapon primitive (fire rate, projectile spawn, ammo) — *primitive only*, not game balance |
| `jce_weather.h` | `jce_weather.c` | Weather state (rain, fog density, wind) feeding renderer + audio |
| `jce_time_of_day.h` | `jce_time_of_day.c` | Sun direction / sky tint / clock |

## Rules

1. **Primitives, not gameplay.** A weapon here exposes "spawn projectile / cooldown / ammo"; balance numbers and player loadouts belong to the game (`examples/caged_kingdom/`).
2. **Triggers fire events** through `jce_event` — consumers subscribe; no direct callbacks across modules.
3. **Weather + time-of-day** publish state; renderer (sky/fog), audio (reverb damping), and gameplay subscribe.
4. **No spawning of game-specific entity types** here — provide a callback / prefab handle.
5. **flecs is private.** Components in public headers are POD.

## Don't

- Don't put game logic (player progression, AI personalities) here.
- Don't render directly — write state, let renderer read it.
- Don't add a second weather/time system.
