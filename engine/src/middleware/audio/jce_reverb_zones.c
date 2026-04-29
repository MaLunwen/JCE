/*
 * jce_reverb_zones.c -- generic 3D reverb-zone blender.
 */

#include <jce/middleware/audio/jce_reverb_zones.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "reverb"

typedef struct Slot {
    bool                 alive;
    uint32_t             generation;
    JceReverbZoneDesc    desc;
} Slot;

struct JceReverbZones {
    Slot           *slots;
    uint32_t        capacity;
    uint32_t        count;
    JceReverbPreset default_preset;
};

static JceReverbZoneId pack_id(uint32_t idx, uint32_t gen)
{
    /* gen in upper 16 bits, idx + 1 in lower 16 (so 0 stays sentinel). */
    return ((JceReverbZoneId)(gen & 0xFFFFu) << 16) | ((idx + 1u) & 0xFFFFu);
}
static bool unpack_id(JceReverbZoneId id, uint32_t cap, uint32_t *out_idx, uint32_t *out_gen)
{
    if (id == JCE_REVERB_ZONE_INVALID) return false;
    uint32_t lo = id & 0xFFFFu;
    if (lo == 0 || lo - 1u >= cap) return false;
    *out_idx = lo - 1u;
    *out_gen = (id >> 16) & 0xFFFFu;
    return true;
}

JceReverbZones *jce_reverb_zones_create(uint32_t cap)
{
    if (cap < 8) cap = 8;
    JceReverbZones *r = (JceReverbZones *)JCE_CALLOC(1, sizeof(*r));
    if (!r) return NULL;
    r->slots = (Slot *)JCE_CALLOC(cap, sizeof(Slot));
    if (!r->slots) { JCE_FREE(r); return NULL; }
    r->capacity = cap;
    r->default_preset = jce_reverb_preset_outdoor();
    return r;
}

void jce_reverb_zones_destroy(JceReverbZones *r)
{
    if (!r) return;
    JCE_FREE(r->slots);
    JCE_FREE(r);
}

static bool grow(JceReverbZones *r)
{
    uint32_t new_cap = r->capacity * 2u;
    Slot *ns = (Slot *)JCE_REALLOC(r->slots, new_cap * sizeof(Slot));
    if (!ns) return false;
    memset(ns + r->capacity, 0, (new_cap - r->capacity) * sizeof(Slot));
    r->slots = ns;
    r->capacity = new_cap;
    return true;
}

/* OBB rotation row vectors are expected to form an orthonormal basis.
 * Helper used by add/update to log a one-shot warning if not.  Public
 * so unit tests / asset cookers can pre-validate. */
static bool obb_rotation_is_orthonormal(const jce_mat4 *m)
{
    jce_vec3 ax = { m->raw[0][0], m->raw[0][1], m->raw[0][2] };
    jce_vec3 ay = { m->raw[1][0], m->raw[1][1], m->raw[1][2] };
    jce_vec3 az = { m->raw[2][0], m->raw[2][1], m->raw[2][2] };
    const float eps_len = 1e-3f;
    const float eps_dot = 1e-3f;
    if (fabsf(jce_v3_dot(ax, ax) - 1.0f) > eps_len) return false;
    if (fabsf(jce_v3_dot(ay, ay) - 1.0f) > eps_len) return false;
    if (fabsf(jce_v3_dot(az, az) - 1.0f) > eps_len) return false;
    if (fabsf(jce_v3_dot(ax, ay)) > eps_dot) return false;
    if (fabsf(jce_v3_dot(ax, az)) > eps_dot) return false;
    if (fabsf(jce_v3_dot(ay, az)) > eps_dot) return false;
    return true;
}

static void warn_if_bad_obb(const JceReverbZoneDesc *d, const char *what)
{
    if (d->shape != JCE_REVERB_SHAPE_OBB) return;
    if (!obb_rotation_is_orthonormal(&d->rotation)) {
        LOG_WARN(LOG_TAG, "%s: OBB rotation matrix is not orthonormal — "
                          "containment tests will be inaccurate", what);
    }
}

JceReverbZoneId jce_reverb_zones_add(JceReverbZones *r, const JceReverbZoneDesc *d)
{
    if (!r || !d) return JCE_REVERB_ZONE_INVALID;
    warn_if_bad_obb(d, "reverb_zones_add");
    for (uint32_t i = 0; i < r->capacity; ++i) {
        if (!r->slots[i].alive) {
            r->slots[i].alive = true;
            r->slots[i].desc  = *d;
            r->count++;
            return pack_id(i, r->slots[i].generation);
        }
    }
    uint32_t old_cap = r->capacity;
    if (!grow(r)) return JCE_REVERB_ZONE_INVALID;
    r->slots[old_cap].alive = true;
    r->slots[old_cap].desc  = *d;
    r->count++;
    return pack_id(old_cap, r->slots[old_cap].generation);
}

bool jce_reverb_zones_update(JceReverbZones *r, JceReverbZoneId id, const JceReverbZoneDesc *d)
{
    if (!r || !d) return false;
    warn_if_bad_obb(d, "reverb_zones_update");
    uint32_t idx, gen;
    if (!unpack_id(id, r->capacity, &idx, &gen)) return false;
    if (!r->slots[idx].alive || (r->slots[idx].generation & 0xFFFFu) != gen) return false;
    r->slots[idx].desc = *d;
    return true;
}

bool jce_reverb_zones_remove(JceReverbZones *r, JceReverbZoneId id)
{
    if (!r) return false;
    uint32_t idx, gen;
    if (!unpack_id(id, r->capacity, &idx, &gen)) return false;
    if (!r->slots[idx].alive || (r->slots[idx].generation & 0xFFFFu) != gen) return false;
    r->slots[idx].alive = false;
    r->slots[idx].generation++;
    r->count--;
    return true;
}

uint32_t jce_reverb_zones_count(const JceReverbZones *r) { return r ? r->count : 0u; }

void jce_reverb_zones_set_default(JceReverbZones *r, const JceReverbPreset *p)
{ if (r && p) r->default_preset = *p; }

/* ------------------ shape distance helpers ------------------ */

static float dist_to_aabb(jce_vec3 p, jce_vec3 c, jce_vec3 e)
{
    float dx = fmaxf(0.0f, fabsf(p.x - c.x) - e.x);
    float dy = fmaxf(0.0f, fabsf(p.y - c.y) - e.y);
    float dz = fmaxf(0.0f, fabsf(p.z - c.z) - e.z);
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float dist_to_sphere(jce_vec3 p, jce_vec3 c, float r)
{
    jce_vec3 d = jce_v3_sub(p, c);
    float dist = sqrtf(jce_v3_dot(d, d));
    return fmaxf(0.0f, dist - r);
}

static float dist_to_obb(jce_vec3 p, const JceReverbZoneDesc *z)
{
    /* Transform p into OBB local space using rotation^T (assume orthonormal).
     * rotation columns 0..2 are local axes. */
    jce_vec3 d = jce_v3_sub(p, z->center);
    jce_vec3 axis_x = { z->rotation.raw[0][0], z->rotation.raw[0][1], z->rotation.raw[0][2] };
    jce_vec3 axis_y = { z->rotation.raw[1][0], z->rotation.raw[1][1], z->rotation.raw[1][2] };
    jce_vec3 axis_z = { z->rotation.raw[2][0], z->rotation.raw[2][1], z->rotation.raw[2][2] };
    jce_vec3 local;
    local.x = jce_v3_dot(d, axis_x);
    local.y = jce_v3_dot(d, axis_y);
    local.z = jce_v3_dot(d, axis_z);
    jce_vec3 zero = { 0.0f, 0.0f, 0.0f };
    return dist_to_aabb(local, zero, z->extents);
}

static float zone_distance(const JceReverbZoneDesc *z, jce_vec3 p)
{
    switch (z->shape) {
    case JCE_REVERB_SHAPE_SPHERE: return dist_to_sphere(p, z->center, z->extents.x);
    case JCE_REVERB_SHAPE_AABB:   return dist_to_aabb(p, z->center, z->extents);
    case JCE_REVERB_SHAPE_OBB:    return dist_to_obb(p, z);
    }
    return INFINITY;
}

static float smoothstep01(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static void blend_into(JceReverbPreset *acc, const JceReverbPreset *p, float w)
{
    acc->wet_mix       += p->wet_mix       * w;
    acc->dry_mix       += p->dry_mix       * w;
    acc->decay_seconds += p->decay_seconds * w;
    acc->room_size     += p->room_size     * w;
    acc->damping       += p->damping       * w;
    acc->diffusion     += p->diffusion     * w;
    acc->density       += p->density       * w;
    acc->pre_delay_ms  += p->pre_delay_ms  * w;
    acc->lowpass_hz    += p->lowpass_hz    * w;
}

void jce_reverb_zones_sample(const JceReverbZones *r, jce_vec3 pos, JceReverbPreset *out)
{
    if (!out) return;
    if (!r) {
        memset(out, 0, sizeof(*out));
        return;
    }
    JCE_PROFILE_ZONE_N("Audio::ReverbZones::sample");

    JceReverbPreset acc; memset(&acc, 0, sizeof(acc));
    float total_w = 0.0f;
    int   max_pri = -2147483647 - 1;

    /* Two-pass for priority weighting. */
    for (uint32_t i = 0; i < r->capacity; ++i) {
        if (!r->slots[i].alive) continue;
        if (r->slots[i].desc.priority > max_pri) max_pri = r->slots[i].desc.priority;
    }

    for (uint32_t i = 0; i < r->capacity; ++i) {
        if (!r->slots[i].alive) continue;
        const JceReverbZoneDesc *z = &r->slots[i].desc;
        float d = zone_distance(z, pos);
        float w;
        if (d <= 0.0f) {
            w = 1.0f;
        } else if (z->falloff_radius > 0.0f && d < z->falloff_radius) {
            w = smoothstep01(1.0f - d / z->falloff_radius);
        } else {
            continue;
        }
        int pri_diff = z->priority - max_pri;
        float pri_scale = powf(4.0f, (float)pri_diff);
        w *= pri_scale;
        blend_into(&acc, &z->preset, w);
        total_w += w;
    }

    if (total_w < 1.0f) {
        blend_into(&acc, &r->default_preset, 1.0f - total_w);
        total_w = 1.0f;
    }
    float inv = 1.0f / total_w;
    out->wet_mix       = acc.wet_mix       * inv;
    out->dry_mix       = acc.dry_mix       * inv;
    out->decay_seconds = acc.decay_seconds * inv;
    out->room_size     = acc.room_size     * inv;
    out->damping       = acc.damping       * inv;
    out->diffusion     = acc.diffusion     * inv;
    out->density       = acc.density       * inv;
    out->pre_delay_ms  = acc.pre_delay_ms  * inv;
    out->lowpass_hz    = acc.lowpass_hz    * inv;
    JCE_PROFILE_ZONE_END;
}

/* ------------------ generic presets ------------------ */

JceReverbPreset jce_reverb_preset_outdoor(void)
{
    JceReverbPreset p;
    p.wet_mix = 0.05f; p.dry_mix = 1.0f; p.decay_seconds = 0.3f;
    p.room_size = 1000.0f; p.damping = 0.5f; p.diffusion = 0.3f;
    p.density = 0.3f; p.pre_delay_ms = 5.0f; p.lowpass_hz = 22050.0f;
    return p;
}
JceReverbPreset jce_reverb_preset_room(void)
{
    JceReverbPreset p;
    p.wet_mix = 0.3f; p.dry_mix = 0.85f; p.decay_seconds = 0.6f;
    p.room_size = 8.0f; p.damping = 0.6f; p.diffusion = 0.6f;
    p.density = 0.7f; p.pre_delay_ms = 8.0f; p.lowpass_hz = 12000.0f;
    return p;
}
JceReverbPreset jce_reverb_preset_hall(void)
{
    JceReverbPreset p;
    p.wet_mix = 0.55f; p.dry_mix = 0.7f; p.decay_seconds = 2.5f;
    p.room_size = 60.0f; p.damping = 0.4f; p.diffusion = 0.85f;
    p.density = 0.85f; p.pre_delay_ms = 25.0f; p.lowpass_hz = 8000.0f;
    return p;
}
JceReverbPreset jce_reverb_preset_cave(void)
{
    JceReverbPreset p;
    p.wet_mix = 0.7f; p.dry_mix = 0.5f; p.decay_seconds = 4.0f;
    p.room_size = 40.0f; p.damping = 0.2f; p.diffusion = 0.95f;
    p.density = 0.9f; p.pre_delay_ms = 35.0f; p.lowpass_hz = 5000.0f;
    return p;
}
JceReverbPreset jce_reverb_preset_underwater(void)
{
    JceReverbPreset p;
    p.wet_mix = 0.2f; p.dry_mix = 0.6f; p.decay_seconds = 1.0f;
    p.room_size = 30.0f; p.damping = 0.95f; p.diffusion = 0.5f;
    p.density = 0.4f; p.pre_delay_ms = 15.0f; p.lowpass_hz = 800.0f;
    return p;
}
