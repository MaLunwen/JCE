/*
 * jce_physics_debug.h  Bullet debug-draw bridge + contact-event listener
 *                      API (P3-C.5).
 *
 * Two features in one header:
 *
 *   1. Debug draw — installs a btIDebugDraw subclass against the
 *      Bullet world; each frame the user calls jce_physics_debug_flush()
 *      to push wireframes / AABBs / contacts through a user-supplied
 *      line-sink callback (typically jce_debug_draw_line from the
 *      renderer).  Gated behind a flag set so disabled-state cost is
 *      a single branch.
 *
 *   2. Trigger / contact events — per-frame manifold diffing turns
 *      Bullet's stateless contact stream into proper
 *      BEGIN / STAY / END events delivered to registered listeners.
 *      The legacy jce_physics_set_contact_begin / _end callbacks keep
 *      firing unchanged; this layer is purely additive.
 *
 * Layer: L4 (middleware/physics).  No renderer / scene dependency —
 * the editor or game wires the line sink and (optionally) maps body
 * handles to ECS entities via jce_physics_body_set_entity().
 */

#ifndef JCE_PHYSICS_DEBUG_H
#define JCE_PHYSICS_DEBUG_H


#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JcePhysicsWorld JcePhysicsWorld;

/* ================================================================== */
/* Debug-draw flags                                                    */
/* ================================================================== */

/* Bit flags — combinable.  Map 1:1 to btIDebugDraw debug-mode bits. */
typedef enum JcePhysicsDebugFlag {
    JCE_PHYS_DBG_NONE         = 0,
    JCE_PHYS_DBG_WIREFRAME    = 1u << 0,
    JCE_PHYS_DBG_AABB         = 1u << 1,
    JCE_PHYS_DBG_CONTACTS     = 1u << 2,
    JCE_PHYS_DBG_CONSTRAINTS  = 1u << 3,
    JCE_PHYS_DBG_NORMALS      = 1u << 4,
    JCE_PHYS_DBG_ALL          = 0xFFFFFFFFu
} JcePhysicsDebugFlag;

/* Line-sink callback.  abgr is 0xAABBGGRR (matches jce_debug_draw). */
typedef void (*jce_debug_line_fn)(jce_vec3 from, jce_vec3 to,
                                  uint32_t abgr, void *ud);

/* Install the line sink used by every world.  Pass fn=NULL to detach
 * (jce_physics_debug_flush becomes a no-op). */
JCE_API void JCE_CALL jce_physics_debug_set_line_sink(jce_debug_line_fn fn,
                                                      void *userdata);

/* Set / read the global debug flag set.  Default = NONE. */
JCE_API void     JCE_CALL jce_physics_debug_set_flags(uint32_t flags);
JCE_API uint32_t JCE_CALL jce_physics_debug_get_flags(void);

/* Walk Bullet's debug-draw buffer for `world` and forward every line
 * to the installed sink.  Cheap when flags==NONE or sink==NULL.
 * Call once per render frame (typically PRE_RENDER).  No state across
 * calls — safe to invoke from multiple worlds in turn. */
JCE_API void JCE_CALL jce_physics_debug_flush(JcePhysicsWorld *world);

/* ================================================================== */
/* Contact-event listeners (BEGIN / STAY / END)                        */
/* ================================================================== */

/* Lifecycle phase of a contact pair, recovered by diffing each
 * fixed-step's manifold set against the previous one. */
typedef enum JceContactEventType {
    JCE_CONTACT_BEGIN = 0,  /* first frame of overlap                */
    JCE_CONTACT_STAY,       /* overlap present last frame too        */
    JCE_CONTACT_END         /* present last frame, gone this frame   */
} JceContactEventType;

/* New listener callback — receives the enriched JceContactEvent
 * (now carrying event type + opaque entity tags). */
typedef void (*jce_contact_listener_fn)(const JceContactEvent *ev,
                                        void *userdata);

/* Tag a body with an opaque 64-bit identifier (typically a flecs
 * ecs_entity_t cast to uint64_t).  Echoed back via entity_a /
 * entity_b on every JceContactEvent.  0 = no tag (default).
 *
 * NOTE: This is metadata only — the physics layer never dereferences
 * it.  Mapping body→entity is the caller's responsibility, which is
 * what keeps physics free of any flecs include. */
JCE_API void JCE_CALL jce_physics_body_set_entity(JcePhysicsWorld *world,
                                                  JceBodyHandle body,
                                                  uint64_t entity_id);
JCE_API uint64_t JCE_CALL jce_physics_body_get_entity(const JcePhysicsWorld *world,
                                                      JceBodyHandle body);

/* Register / unregister a listener.  Up to JCE_PHYSICS_MAX_LISTENERS
 * per world; returns false on overflow or NULL args.  Listeners fire
 * after every physics step in registration order. */
#define JCE_PHYSICS_MAX_LISTENERS 16

JCE_API bool JCE_CALL jce_physics_add_contact_listener(JcePhysicsWorld *world,
                                                       jce_contact_listener_fn fn,
                                                       void *userdata);
JCE_API void JCE_CALL jce_physics_remove_contact_listener(JcePhysicsWorld *world,
                                                          jce_contact_listener_fn fn,
                                                          void *userdata);

/*
 * ECS integration recipe (flecs):
 *
 *   // 1. Tag bodies when you create them.
 *   JceBodyHandle h = jce_physics_body_create(world, &desc);
 *   jce_physics_body_set_entity(world, h, (uint64_t)my_entity);
 *
 *   // 2. Subscribe.  The listener is called with BEGIN / STAY / END.
 *   //    From there you can ecs_emit() a flecs event, set a tag,
 *   //    write a component — whatever the game needs.  Keeping the
 *   //    flecs glue in scene code avoids dragging flecs into the
 *   //    physics layer.
 *   static void on_contact(const JceContactEvent *ev, void *ud) {
 *       ecs_world_t *w = (ecs_world_t *)ud;
 *       if (ev->type == JCE_CONTACT_BEGIN && ev->is_trigger) {
 *           ecs_emit(w, &(ecs_event_desc_t){
 *               .event  = ecs_id(JceContactEvent),
 *               .ids    = &(ecs_type_t){...},
 *               .entity = (ecs_entity_t)ev->entity_a,
 *               .param  = (void *)ev,
 *           });
 *       }
 *   }
 *   jce_physics_add_contact_listener(world, on_contact, ecs);
 */

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_DEBUG_H */
