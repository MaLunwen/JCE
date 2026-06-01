/*
 * api_physics.h  Physics simulation.
 *
 * 3D physics via Bullet3 C bridge, 2D physics via Box2D.
 * Common types shared between both.
 */

#ifndef JCE_API_PHYSICS_H
#define JCE_API_PHYSICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics2d.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/middleware/physics/jce_collider_asset.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/middleware/physics/jce_physics_joint_query.h>
#include <jce/middleware/physics/jce_physics_layers.h>
#include <jce/middleware/physics/jce_physics_material.h>
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/middleware/physics/jce_cloth.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_PHYSICS_H */
