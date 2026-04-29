/*
 * jce_weapon.c -- weapon state-machine + shot generator.
 */

#include <jce/middleware/world/jce_weapon.h>
#include <jce/os/core/jce_log.h>

#include <math.h>
#include <string.h>

#define LOG_TAG "weapon"

void jce_weapon_archetype_default(JceWeaponArchetype *o, JceWeaponKind kind)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->kind             = kind;
    o->name             = (kind == JCE_WEAPON_KIND_HITSCAN) ? "Pistol" : "Rocket";
    o->damage           = (kind == JCE_WEAPON_KIND_HITSCAN) ? 25.0f : 80.0f;
    o->range            = (kind == JCE_WEAPON_KIND_HITSCAN) ? 80.0f : 200.0f;
    o->rpm              = (kind == JCE_WEAPON_KIND_HITSCAN) ? 360.0f : 60.0f;
    o->clip_size        = (kind == JCE_WEAPON_KIND_HITSCAN) ? 12 : 1;
    o->reserve_max      = (kind == JCE_WEAPON_KIND_HITSCAN) ? 60 : 6;
    o->reload_seconds   = (kind == JCE_WEAPON_KIND_HITSCAN) ? 1.4f : 2.5f;
    o->spread_deg       = (kind == JCE_WEAPON_KIND_HITSCAN) ? 0.6f : 0.0f;
    o->recoil_per_shot  = (kind == JCE_WEAPON_KIND_HITSCAN) ? 0.8f : 4.0f;
    o->recoil_recovery  = 6.0f;
    o->pellets          = 1;
    o->projectile_speed = (kind == JCE_WEAPON_KIND_PROJECTILE) ? 60.0f : 0.0f;
    o->full_auto        = false;
}

void jce_weapon_instance_init(JceWeaponInstance *inst, const JceWeaponArchetype *arch)
{
    if (!inst) return;
    memset(inst, 0, sizeof(*inst));
    inst->state   = JCE_WEAPON_STATE_IDLE;
    if (arch) {
        inst->clip    = arch->clip_size;
        inst->reserve = arch->reserve_max;
    }
}

void jce_weapon_pull_trigger(JceWeaponInstance *i)    { if (i) i->trigger_held = true;  }
void jce_weapon_release_trigger(JceWeaponInstance *i) { if (i) i->trigger_held = false; }

bool jce_weapon_request_reload(JceWeaponInstance *i, const JceWeaponArchetype *a)
{
    if (!i || !a) return false;
    if (i->state == JCE_WEAPON_STATE_RELOAD) return false;
    if (i->reserve <= 0) return false;
    if (i->clip >= a->clip_size) return false;
    i->state        = JCE_WEAPON_STATE_RELOAD;
    i->reload_timer = a->reload_seconds;
    return true;
}

void jce_weapon_add_reserve(JceWeaponInstance *i, const JceWeaponArchetype *a, int32_t amount)
{
    if (!i || !a || amount <= 0) return;
    int32_t cap = a->reserve_max;
    int32_t v = i->reserve + amount;
    if (v > cap) v = cap;
    i->reserve = v;
}

/* xorshift64* */
static uint32_t rng_next(uint64_t *s)
{
    uint64_t x = *s ? *s : 0xD1B54A32D192ED03ULL;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return (uint32_t)((x * 2685821657736338717ULL) >> 32);
}
static float rng_float(uint64_t *s)
{
    return (float)rng_next(s) / 4294967295.0f;
}

static jce_vec3 v3_normalize_or(jce_vec3 v, jce_vec3 fb)
{
    float l = sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    if (l < 1e-5f) return fb;
    return jce_v3(v.x/l, v.y/l, v.z/l);
}

/* Apply a small angular deviation (degrees) to `dir` using two
 * perpendicular axes derived from world up. */
static jce_vec3 apply_cone_spread(jce_vec3 dir, float spread_deg, uint64_t *rng)
{
    if (spread_deg <= 0.0f) return dir;
    /* Build orthonormal basis. */
    jce_vec3 up   = (fabsf(dir.y) > 0.95f) ? jce_v3(1, 0, 0) : jce_v3(0, 1, 0);
    jce_vec3 side = jce_v3_normalize(jce_v3_cross(up, dir));
    jce_vec3 vup  = jce_v3_cross(dir, side);

    float ang = rng_float(rng) * 6.28318530718f;
    float r   = sqrtf(rng_float(rng)) * spread_deg * 0.0174533f; /* deg → rad */
    float kx  = sinf(r) * cosf(ang);
    float ky  = sinf(r) * sinf(ang);
    float kz  = cosf(r);
    jce_vec3 d = jce_v3(
        side.x * kx + vup.x * ky + dir.x * kz,
        side.y * kx + vup.y * ky + dir.y * kz,
        side.z * kx + vup.z * ky + dir.z * kz);
    return v3_normalize_or(d, dir);
}

uint32_t jce_weapon_update(JceWeaponInstance *inst, const JceWeaponArchetype *arch,
                            jce_vec3 aim_origin, jce_vec3 aim_dir,
                            uint64_t shooter_cookie, uint32_t archetype_id,
                            float dt, JceWeaponShot *out, uint32_t max_shots,
                            uint64_t *rng)
{
    if (!inst || !arch) return 0;
    if (dt < 0.0f) dt = 0.0f;

    /* Drain timers. */
    if (inst->fire_cooldown > 0.0f) {
        inst->fire_cooldown -= dt;
        if (inst->fire_cooldown < 0.0f) inst->fire_cooldown = 0.0f;
    }
    if (inst->recoil > 0.0f) {
        inst->recoil -= arch->recoil_recovery * dt;
        if (inst->recoil < 0.0f) inst->recoil = 0.0f;
    }
    if (inst->state == JCE_WEAPON_STATE_RELOAD) {
        inst->reload_timer -= dt;
        if (inst->reload_timer <= 0.0f) {
            int32_t need = arch->clip_size - inst->clip;
            int32_t take = need < inst->reserve ? need : inst->reserve;
            inst->clip    += take;
            inst->reserve -= take;
            inst->state          = JCE_WEAPON_STATE_IDLE;
            inst->reload_timer   = 0.0f;
        }
        return 0;
    }

    if (!inst->trigger_held)            { inst->state = JCE_WEAPON_STATE_IDLE; return 0; }
    if (inst->fire_cooldown > 0.0f)     { return 0; }
    if (inst->clip <= 0) {
        /* Auto-reload if we have reserve. */
        jce_weapon_request_reload(inst, arch);
        return 0;
    }
    if (!arch->full_auto && inst->state == JCE_WEAPON_STATE_FIRING) {
        /* semi-auto: must release trigger between shots */
        return 0;
    }

    aim_dir = v3_normalize_or(aim_dir, jce_v3(0, 0, 1));

    /* One trigger pull = one shot (or burst pellets). */
    uint32_t pellets = arch->pellets > 0 ? (uint32_t)arch->pellets : 1u;
    uint32_t emitted = 0;
    float total_spread = arch->spread_deg + inst->recoil;
    uint64_t local_rng = rng ? *rng : 0xC0FFEE12345ULL;

    for (uint32_t p = 0; p < pellets && emitted < max_shots; p++) {
        jce_vec3 d = apply_cone_spread(aim_dir, total_spread, &local_rng);
        if (out) {
            out[emitted].origin           = aim_origin;
            out[emitted].direction        = d;
            out[emitted].damage           = arch->damage;
            out[emitted].kind             = arch->kind;
            out[emitted].projectile_speed = arch->projectile_speed;
            out[emitted].shooter_cookie   = shooter_cookie;
            out[emitted].archetype_id     = archetype_id;
        }
        emitted++;
    }
    if (rng) *rng = local_rng;

    inst->clip               -= 1;
    inst->shots_fired_total  += 1;
    inst->recoil             += arch->recoil_per_shot;
    if (arch->rpm > 0.0f) inst->fire_cooldown = 60.0f / arch->rpm;
    inst->state = JCE_WEAPON_STATE_FIRING;
    return emitted;
}
