/* Private, allocation-free preparation of the optional Flecs OS adapter.
 * Call before the first ecs_init/other Flecs OS use. Installation is one-shot;
 * concurrent preparation fails closed and does not make world creation itself
 * thread-safe. Installed callbacks persist across all world lifetimes. */
#ifndef JCE_SCENE_FLECS_OS_H
#define JCE_SCENE_FLECS_OS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool jce_scene_flecs_os_prepare(void);

#ifdef __cplusplus
}
#endif

#endif
