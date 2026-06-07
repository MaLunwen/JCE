/*
 * jce_virtual_camera.c  Compatibility TU for virtual-camera value types.
 *
 * The standalone handle-based vcam manager was removed in v0.9.4.
 * Runtime behavior lives in jce_vcam_system.c, which resolves ECS
 * VirtualCamera components.  Keep this TU so configured build trees that
 * still list jce_virtual_camera.c remain valid without reintroducing the
 * removed manager API.
 */

#include <jce/middleware/scene/jce_virtual_camera.h>
