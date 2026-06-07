/*
 * jce_perception.h  Agent perception + blackboard (P2-perception-bt-binding).
 *
 * Two cooperating pieces an AI agent needs before a behavior tree can act
 * on the world:
 *
 *   - JceBlackboard — a small string-keyed key/value store (the working
 *     memory a BT reads/writes through its action callbacks).  Holds float,
 *     vec3, bool, int and entity-id values.  A BT action looks up
 *     "target.visible" / "target.position" etc. that perception wrote.
 *
 *   - JcePerception — a stimulus producer.  Each tick it tests a list of
 *     candidate targets against an agent's sight cone (range + half-angle)
 *     with a line-of-sight raycast through the physics world, plus an
 *     optional hearing radius, and writes the resulting stimuli
 *     ("can-see-target", last-known position, distance, heard a sound)
 *     into the agent's blackboard.
 *
 * The raycast is supplied by the caller (the runtime passes a thin adapter
 * over jce_physics_raycast_filtered) so this module stays free of a hard
 * dependency on the physics layer — it remains a pure-AI primitive.
 *
 * Thread-safety: NOT thread-safe.  Main-thread / per-agent use.
 *
 * Layer: AI (Layer 4).
 */

#ifndef JCE_PERCEPTION_H
#define JCE_PERCEPTION_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Blackboard                                                          */
/* ================================================================== */

typedef struct JceBlackboard JceBlackboard;

/* Value kinds a blackboard slot can hold (tagged union internally). */
typedef enum {
    JCE_BB_NONE   = 0,
    JCE_BB_BOOL   = 1,
    JCE_BB_INT    = 2,
    JCE_BB_FLOAT  = 3,
    JCE_BB_VEC3   = 4,
    JCE_BB_ENTITY = 5
} JceBlackboardKind;

JCE_API JceBlackboard *JCE_CALL jce_blackboard_create(void);
JCE_API void           JCE_CALL jce_blackboard_destroy(JceBlackboard *bb);

/* Drop every key (keeps the allocation for reuse). */
JCE_API void JCE_CALL jce_blackboard_clear(JceBlackboard *bb);

/* Setters — create the key if absent, overwrite (and retype) if present. */
JCE_API void JCE_CALL jce_blackboard_set_bool  (JceBlackboard *bb, const char *key, bool v);
JCE_API void JCE_CALL jce_blackboard_set_int   (JceBlackboard *bb, const char *key, int v);
JCE_API void JCE_CALL jce_blackboard_set_float (JceBlackboard *bb, const char *key, float v);
JCE_API void JCE_CALL jce_blackboard_set_vec3  (JceBlackboard *bb, const char *key, jce_vec3 v);
JCE_API void JCE_CALL jce_blackboard_set_entity(JceBlackboard *bb, const char *key, uint64_t v);

/* Getters — return `def` (or false / NONE) when the key is absent or holds a
 * different kind.  jce_blackboard_kind reports the stored kind (NONE if no key). */
JCE_API bool     JCE_CALL jce_blackboard_get_bool  (const JceBlackboard *bb, const char *key, bool def);
JCE_API int      JCE_CALL jce_blackboard_get_int   (const JceBlackboard *bb, const char *key, int def);
JCE_API float    JCE_CALL jce_blackboard_get_float (const JceBlackboard *bb, const char *key, float def);
JCE_API jce_vec3 JCE_CALL jce_blackboard_get_vec3  (const JceBlackboard *bb, const char *key, jce_vec3 def);
JCE_API uint64_t JCE_CALL jce_blackboard_get_entity(const JceBlackboard *bb, const char *key, uint64_t def);

JCE_API bool              JCE_CALL jce_blackboard_has(const JceBlackboard *bb, const char *key);
JCE_API JceBlackboardKind JCE_CALL jce_blackboard_kind(const JceBlackboard *bb, const char *key);

/* Number of keys currently set (debug / introspection). */
JCE_API uint32_t JCE_CALL jce_blackboard_count(const JceBlackboard *bb);

/* ================================================================== */
/* Perception                                                          */
/* ================================================================== */

/* Caller-supplied line-of-sight test.  Return true when the segment from
 * `from` to `to` is BLOCKED by world geometry (i.e. NO line of sight).
 * `userdata` is the pointer passed to jce_perception_update.  When NULL is
 * passed for the function, perception assumes clear LOS (range/cone only). */
typedef bool (*jce_perception_los_fn)(jce_vec3 from, jce_vec3 to, void *userdata);

/* One sensing candidate the agent may perceive this tick. */
typedef struct {
    uint64_t entity;     /* candidate entity id (written to the blackboard) */
    jce_vec3 position;   /* candidate world position (eye/center) */
    float    loudness;   /* sound emitted this tick in metres of audible
                          * radius; 0 = silent.  Used by the hearing test. */
} JcePerceptionTarget;

/* Agent sensing parameters. */
typedef struct {
    jce_vec3 eye_position;     /* agent eye/sensor world position */
    jce_vec3 forward;          /* agent facing (need not be normalized) */
    float    sight_range;      /* max sight distance (m); <=0 disables sight */
    float    sight_half_angle; /* half-angle of the sight cone (radians) */
    float    hearing_range;    /* max hearing distance (m); <=0 disables hearing */
} JcePerceptionAgent;

/* Result of a perception update for one agent. */
typedef struct {
    bool     can_see;        /* at least one target visible this tick */
    uint64_t seen_entity;    /* nearest visible target (0 if none) */
    jce_vec3 seen_position;  /* its world position */
    float    seen_distance;  /* distance to it (m) */
    bool     can_hear;       /* heard at least one sound this tick */
    uint64_t heard_entity;   /* loudest audible target (0 if none) */
    jce_vec3 heard_position; /* its world position */
} JcePerceptionResult;

/*
 * Run one perception pass.  For every candidate the agent's sight cone is
 * tested (within sight_range AND within sight_half_angle of `forward`),
 * then a LOS raycast confirms nothing blocks the view; the NEAREST visible
 * candidate becomes the "seen" stimulus.  Independently, any candidate
 * within hearing_range whose loudness reaches the agent becomes the
 * "heard" stimulus (loudest wins).
 *
 * When `bb` is non-NULL the stimuli are also written into it under the
 * conventional keys the bundled BT actions read:
 *   target.visible   (bool)    target.entity   (entity)
 *   target.position  (vec3)    target.distance (float)
 *   sound.heard      (bool)    sound.entity    (entity)
 *   sound.position   (vec3)
 * When a target is seen, target.last_known_position (vec3) is refreshed;
 * it is intentionally NOT cleared on loss so a BT can search the last spot.
 *
 * `out` may be NULL if the caller only wants the blackboard side effects.
 * Returns true if anything was sensed (seen or heard).
 */
JCE_API bool JCE_CALL jce_perception_update(JceBlackboard *bb,
                                            const JcePerceptionAgent *agent,
                                            const JcePerceptionTarget *targets,
                                            uint32_t target_count,
                                            jce_perception_los_fn los_fn,
                                            void *los_userdata,
                                            JcePerceptionResult *out);

JCE_EXTERN_C_END

#endif /* JCE_PERCEPTION_H */
