/*
 * api_world.h  Aggregate header for the world / gameplay middleware layer
 * (engine/src/middleware/world, lib jce_world).
 *
 * Pulls in the public surface of the Layer-4 "world" subsystems: weapons,
 * the Gameplay Ability System (GAS), spawn management, weather, time-of-day,
 * trigger volumes, road networks and the flecs trigger adapter.  Use when a
 * client wants the whole gameplay-world API without listing each header.
 *
 * Every header below is self-contained and depends only on os/core; none of
 * them pulls a heavy third-party (flecs/bgfx) header into this aggregate.
 */

#ifndef JCE_API_WORLD_H
#define JCE_API_WORLD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/world/jce_atmosphere.h>
#include <jce/middleware/world/jce_gas.h>
#include <jce/middleware/world/jce_road_network.h>
#include <jce/middleware/world/jce_spawn_manager.h>
#include <jce/middleware/world/jce_time_of_day.h>
#include <jce/middleware/world/jce_trigger_volume.h>
#include <jce/middleware/world/jce_weapon.h>
#include <jce/middleware/world/jce_weather.h>
#include <jce/middleware/world/jce_world_ecs.h>

/* Completed 2026-08-31.  These headers export JCE_API symbols and were
 * reachable from NO umbrella, so §4's promise -- `#include <jce/api.h>`
 * gives you the whole engine -- did not hold for them.  Several are named
 * in §4's own table by capability.
 */
#include <jce/middleware/world/jce_gas_replication.h>
#include <jce/middleware/world/jce_sky.h>

#ifdef __cplusplus
}
#endif
#endif /* JCE_API_WORLD_H */
