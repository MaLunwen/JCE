#ifndef JCE_PHYSICS_DEBUG_QUERY_HPP
#define JCE_PHYSICS_DEBUG_QUERY_HPP
#include <jce/middleware/physics/jce_physics_debug.h>
class btCollisionWorld;
class btCollisionObject;
bool jce_bullet_debug_query_native(btCollisionWorld *world,
    const JcePhysicsDebugQuery *query, jce_debug_line_fn sink, void *userdata,
    JcePhysicsDebugQueryStats *stats, const btCollisionObject *keep_character = nullptr);
#endif
