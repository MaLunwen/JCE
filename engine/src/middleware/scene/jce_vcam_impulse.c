/*
 * jce_vcam_impulse.c  Process-global impulse pool + listener integrator.
 *
 * Phase envelope: 0..attack rises 0→1, attack..attack+sustain stays
 * 1, then attack+sustain..end falls 1→0 via the chosen decay curve.
 * Attenuation is quadratic with distance to source.
 */

#include <jce/middleware/scene/jce_vcam_impulse.h>

#include <math.h>
#include <string.h>

static JceVcamImpulse s_pool[JCE_VCAM_IMPULSE_MAX];

void jce_vcam_impulse_clear(void)
{
    memset(s_pool, 0, sizeof(s_pool));
}

bool jce_vcam_impulse_spawn(const JceVcamImpulse *imp)
{
    if (!imp) return false;
    /* Evict the oldest if pool is full. */
    int slot = -1;
    float worst_age = -1.0f;
    for (int i = 0; i < JCE_VCAM_IMPULSE_MAX; ++i) {
        if (!s_pool[i].active) { slot = i; break; }
        if (s_pool[i].age > worst_age) { worst_age = s_pool[i].age; slot = i; }
    }
    if (slot < 0) return false;
    s_pool[slot] = *imp;
    s_pool[slot].age = 0.0f;
    s_pool[slot].active = true;
    return true;
}

static float envelope(float age, float attack, float sustain, float release,
                       JceVcamImpulseDecay decay)
{
    if (age <= 0.0f) return 0.0f;
    if (age < attack) {
        return (attack > 0.0001f) ? (age / attack) : 1.0f;
    }
    float t_rel = age - attack - sustain;
    if (t_rel <= 0.0f) return 1.0f;
    if (release <= 0.0001f) return 0.0f;
    float u = t_rel / release;
    if (u >= 1.0f) return 0.0f;
    switch (decay) {
    case JCE_VCAM_IMPULSE_LINEAR: return 1.0f - u;
    case JCE_VCAM_IMPULSE_EXPO:   return expf(-3.5f * u);
    case JCE_VCAM_IMPULSE_BUMP: {
        float s = sinf((1.0f - u) * 1.5707963f);
        return s * s;
    }
    }
    return 1.0f - u;
}

void jce_vcam_impulse_integrate(const float listener_pos[3], float dt,
                                  JceVcamImpulseFrame *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!listener_pos) return;

    for (int i = 0; i < JCE_VCAM_IMPULSE_MAX; ++i) {
        JceVcamImpulse *imp = &s_pool[i];
        if (!imp->active) continue;
        imp->age += dt;
        float total = imp->attack_time + imp->sustain_time + imp->release_time;
        if (imp->age > total) { imp->active = false; continue; }

        float env = envelope(imp->age, imp->attack_time,
                              imp->sustain_time, imp->release_time,
                              imp->decay);

        /* Distance attenuation. */
        float dx = listener_pos[0] - imp->source_pos[0];
        float dy = listener_pos[1] - imp->source_pos[1];
        float dz = listener_pos[2] - imp->source_pos[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        float r2 = imp->attenuation_radius * imp->attenuation_radius;
        float atten = 1.0f;
        if (r2 > 0.0f) {
            if (d2 >= r2) continue;
            float t = sqrtf(d2 / r2);
            atten = (1.0f - t) * (1.0f - t);
        }
        float k = env * atten;

        out->position_offset[0] += imp->impulse_force[0] * k;
        out->position_offset[1] += imp->impulse_force[1] * k;
        out->position_offset[2] += imp->impulse_force[2] * k;
        out->rotation_angle_rad += imp->rotation_magnitude * k;
        /* Average axis (last-wins for simplicity; production would
         * compose quaternions). */
        out->rotation_axis[0] = imp->rotation_axis[0];
        out->rotation_axis[1] = imp->rotation_axis[1];
        out->rotation_axis[2] = imp->rotation_axis[2];
    }
}

uint32_t jce_vcam_impulse_active_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_VCAM_IMPULSE_MAX; ++i)
        if (s_pool[i].active) n++;
    return n;
}
