/*
 * jce_particle_force_fields.c  Sum-of-fields particle force sampler.
 *
 * Mirrors the jce_wind_zone pattern: registered fields each
 * contribute to the per-particle force, summed at sample time.
 * Shape gates the per-field contribution to its volume.
 */

#include <jce/middleware/scene/jce_particle_force_fields.h>

#include <math.h>
#include <string.h>

static JceParticleForceField s_fields[JCE_PARTICLE_FORCE_FIELD_MAX];

void jce_particle_force_clear(void)
{
    memset(s_fields, 0, sizeof(s_fields));
}

bool jce_particle_force_register(const JceParticleForceField *ff)
{
    if (!ff) return false;
    for (int i = 0; i < JCE_PARTICLE_FORCE_FIELD_MAX; ++i) {
        if (!s_fields[i].active) {
            s_fields[i] = *ff;
            s_fields[i].active = true;
            return true;
        }
    }
    return false;
}

uint32_t jce_particle_force_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_PARTICLE_FORCE_FIELD_MAX; ++i)
        if (s_fields[i].active) n++;
    return n;
}

static bool in_shape(const JceParticleForceField *ff, const float p[3])
{
    switch (ff->shape) {
    case JCE_PARTICLE_FORCE_SHAPE_INFINITE: return true;
    case JCE_PARTICLE_FORCE_SHAPE_SPHERE: {
        float dx = p[0] - ff->center[0];
        float dy = p[1] - ff->center[1];
        float dz = p[2] - ff->center[2];
        return (dx*dx + dy*dy + dz*dz) <= ff->radius * ff->radius;
    }
    case JCE_PARTICLE_FORCE_SHAPE_BOX: {
        for (int i = 0; i < 3; ++i) {
            float v = p[i] - ff->center[i];
            if (v < -ff->half_extents[i] || v > ff->half_extents[i])
                return false;
        }
        return true;
    }
    }
    return false;
}

static float turb_noise(float x, float y, float z, float t)
{
    float a = 0.5f + 0.5f * cosf(x * 0.17f + y * 0.13f + z * 0.11f + t);
    float b = 0.5f + 0.5f * cosf(x * 0.31f - y * 0.27f + z * 0.19f + t * 1.7f);
    return (a + b) * 0.5f - 0.5f;
}

void jce_particle_sample_force(const float pos[3], const float vel[3],
                                 float t, float out[3])
{
    if (!out) return;
    out[0] = out[1] = out[2] = 0.0f;
    if (!pos) return;
    static float zero_vel[3] = { 0, 0, 0 };
    if (!vel) vel = zero_vel;

    for (int i = 0; i < JCE_PARTICLE_FORCE_FIELD_MAX; ++i) {
        const JceParticleForceField *ff = &s_fields[i];
        if (!ff->active || !in_shape(ff, pos)) continue;

        switch (ff->kind) {
        case JCE_PARTICLE_FORCE_EXTERNAL: {
            out[0] += ff->direction[0] * ff->strength;
            out[1] += ff->direction[1] * ff->strength;
            out[2] += ff->direction[2] * ff->strength;
            break;
        }
        case JCE_PARTICLE_FORCE_TURBULENCE: {
            float fx = ff->frequency > 0 ? ff->frequency : 1.0f;
            float nx = turb_noise(pos[0]*fx, pos[1]*fx, pos[2]*fx, t);
            float ny = turb_noise(pos[1]*fx + 13, pos[2]*fx, pos[0]*fx, t);
            float nz = turb_noise(pos[2]*fx + 7,  pos[0]*fx, pos[1]*fx, t);
            out[0] += nx * ff->turbulence_amp[0] * ff->strength;
            out[1] += ny * ff->turbulence_amp[1] * ff->strength;
            out[2] += nz * ff->turbulence_amp[2] * ff->strength;
            break;
        }
        case JCE_PARTICLE_FORCE_DRAG: {
            out[0] -= vel[0] * ff->drag_coefficient;
            out[1] -= vel[1] * ff->drag_coefficient;
            out[2] -= vel[2] * ff->drag_coefficient;
            break;
        }
        case JCE_PARTICLE_FORCE_VORTEX: {
            /* Tangent = cross(axis, r). */
            float r[3] = {
                pos[0] - ff->center[0],
                pos[1] - ff->center[1],
                pos[2] - ff->center[2],
            };
            float ax = ff->direction[0], ay = ff->direction[1], az = ff->direction[2];
            float L = sqrtf(ax*ax + ay*ay + az*az);
            if (L > 1e-6f) { ax/=L; ay/=L; az/=L; }
            float tx = ay*r[2] - az*r[1];
            float ty = az*r[0] - ax*r[2];
            float tz = ax*r[1] - ay*r[0];
            out[0] += tx * ff->strength;
            out[1] += ty * ff->strength;
            out[2] += tz * ff->strength;
            break;
        }
        }
    }
}
