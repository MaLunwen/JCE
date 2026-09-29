/*
 * jce_weapon.h -- runtime weapon system.
 *
 * Models hitscan and projectile weapons with:
 *   - Per-weapon archetype (damage, RPM, mag, reload time, accuracy,
 *     recoil profile, range, projectile/hitscan flag).
 *   - Per-instance state (current clip, reserve, fire timer, reload
 *     timer, recoil accumulator).
 *   - Fire/reload state machine.
 *
 * Output: caller polls jce_weapon_consume_shot() each frame to retrieve
 * pending shots (origin, direction, damage), then the caller does the
 * actual physics raycast / projectile spawn / damage application.
 *
 * Not included (caller responsibility):
 *   - Physics raycast (use jce_physics)
 *   - Spawning bullet decals / muzzle particles (jce_decals, jce_gpu_particles)
 *   - Audio (jce_audio)
 *
 * This module is engine-agnostic of those systems; it just produces
 * shot events.  A caller wires them itself: poll the shot events each
 * frame, raycast with jce_physics against the ray they carry, then spawn
 * whatever decal / particle / sound that hit deserves.
 *
 * (This used to say "see ck_weapons.c" -- a file that ships with one game
 * and with no SDK consumer, so the pointer answered nothing for the reader
 * who most needed it.)
 *
 * Layer: middleware/world (Layer 4) — public.
 */
#ifndef JCE_WEAPON_H
#define JCE_WEAPON_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_WEAPON_KIND_HITSCAN    = 0,
    JCE_WEAPON_KIND_PROJECTILE = 1
} JceWeaponKind;

typedef struct {
    const char     *name;
    JceWeaponKind   kind;
    float           damage;             /* per shot */
    float           range;              /* m */
    float           rpm;                /* rounds per minute */
    int32_t         clip_size;
    int32_t         reserve_max;
    float           reload_seconds;
    float           spread_deg;         /* base cone half-angle */
    float           recoil_per_shot;    /* deg/shot, accumulates */
    float           recoil_recovery;    /* deg/sec recovery */
    int32_t         pellets;            /* shotgun: > 1 */
    float           projectile_speed;   /* projectile only */
    bool            full_auto;
} JceWeaponArchetype;

typedef enum {
    JCE_WEAPON_STATE_IDLE   = 0,
    JCE_WEAPON_STATE_FIRING = 1,
    JCE_WEAPON_STATE_RELOAD = 2
} JceWeaponState;

typedef struct {
    JceWeaponState state;
    int32_t        clip;
    int32_t        reserve;
    float          fire_cooldown;       /* seconds */
    float          reload_timer;
    float          recoil;              /* current accumulated deg */
    bool           trigger_held;
    uint32_t       shots_fired_total;
} JceWeaponInstance;

typedef struct {
    jce_vec3   origin;
    jce_vec3   direction;
    float      damage;
    JceWeaponKind kind;
    float      projectile_speed;        /* valid if kind==PROJECTILE */
    uint64_t   shooter_cookie;          /* opaque caller id */
    uint32_t   archetype_id;
} JceWeaponShot;

JCE_API void jce_weapon_archetype_default(JceWeaponArchetype *out, JceWeaponKind kind);

JCE_API void jce_weapon_instance_init(JceWeaponInstance *inst,
                                       const JceWeaponArchetype *arch);

/* Fire control. */
JCE_API void jce_weapon_pull_trigger(JceWeaponInstance *inst);
JCE_API void jce_weapon_release_trigger(JceWeaponInstance *inst);
JCE_API bool jce_weapon_request_reload(JceWeaponInstance *inst,
                                        const JceWeaponArchetype *arch);
JCE_API void jce_weapon_add_reserve(JceWeaponInstance *inst,
                                     const JceWeaponArchetype *arch,
                                     int32_t amount);

/* Update & shot generation.
 *
 * Each call:
 *   - Drains fire_cooldown, reload_timer, recoil recovery.
 *   - If trigger held + can_fire, generates 1 shot per pellet into
 *     out_shots (capped by max_shots).  Returns how many were written.
 *   - Decrements clip and applies recoil for each shot.
 *
 * aim_origin / aim_direction define the shooter's eye/muzzle ray.
 * Applies spread (random cone within spread + recoil).
 */
JCE_API uint32_t jce_weapon_update(JceWeaponInstance *inst,
                                    const JceWeaponArchetype *arch,
                                    jce_vec3 aim_origin,
                                    jce_vec3 aim_direction,
                                    uint64_t shooter_cookie,
                                    uint32_t archetype_id,
                                    float dt,
                                    JceWeaponShot *out_shots,
                                    uint32_t max_shots,
                                    uint64_t *rng_state);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WEAPON_H */
