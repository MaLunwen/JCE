/*
 * jce_camera_shake.c -- Trauma-based camera shake (see jce_camera_shake.h).
 *
 * shake = trauma²; per-axis offset = shake * max * noise(axis, time).  The
 * noise is a seeded two-sine blend whose amplitude sums to 1, so |noise| <= 1
 * and therefore |offset| <= max — a property the unit tests lock in.
 */

#include <jce/os/core/jce_camera_shake.h>

#include <math.h>

#define SHAKE_TWO_PI 6.28318530717958647692f

static float shake_clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Seeded smooth noise in [-1,1] for a given axis + time. */
static float shake_noise(uint32_t seed, int axis, float t, float freq)
{
    uint32_t a = seed ^ ((uint32_t)axis * 2654435761u);
    uint32_t b = (seed * 40503u) ^ ((uint32_t)axis * 0x9E3779B9u);
    float p1 = (float)(a & 0xFFFFu) / 65535.0f * SHAKE_TWO_PI;
    float p2 = (float)(b & 0xFFFFu) / 65535.0f * SHAKE_TWO_PI;
    float s1 = sinf(t * freq * SHAKE_TWO_PI + p1);
    float s2 = sinf(t * freq * 2.17f * SHAKE_TWO_PI + p2);
    return 0.6f * s1 + 0.4f * s2;   /* |.| <= 1.0 */
}

void jce_camera_shake_init(JceCameraShake *s, float decay_per_sec,
                           float frequency, uint32_t seed)
{
    if (!s) return;
    s->trauma        = 0.0f;
    s->decay_per_sec = (decay_per_sec > 0.0f) ? decay_per_sec : 1.0f;
    s->frequency     = (frequency > 0.0f) ? frequency : 10.0f;
    s->seed          = seed ? seed : 0x1234567u;
    s->time          = 0.0f;
    s->max_pos[0] = s->max_pos[1] = s->max_pos[2] = 0.3f;
    s->max_rot_deg[0] = s->max_rot_deg[1] = s->max_rot_deg[2] = 5.0f;
}

void jce_camera_shake_set_amplitude(JceCameraShake *s, const float max_pos[3],
                                    const float max_rot_deg[3])
{
    if (!s) return;
    if (max_pos) { s->max_pos[0] = max_pos[0]; s->max_pos[1] = max_pos[1]; s->max_pos[2] = max_pos[2]; }
    if (max_rot_deg) { s->max_rot_deg[0] = max_rot_deg[0]; s->max_rot_deg[1] = max_rot_deg[1]; s->max_rot_deg[2] = max_rot_deg[2]; }
}

void jce_camera_shake_add_trauma(JceCameraShake *s, float amount)
{
    if (!s) return;
    /* Reject a non-finite amount (NaN / Inf): clamping does NOT tame NaN
     * (every comparison is false, so it slips through and permanently poisons
     * trauma — and via jce.shake_camera() a script could trigger this). */
    if (!(amount == amount) || amount > 1e30f || amount < -1e30f) return;
    s->trauma = shake_clamp01(s->trauma + amount);
}

void jce_camera_shake_update(JceCameraShake *s, float dt)
{
    if (!s || dt <= 0.0f) return;
    s->time += dt;
    s->trauma = shake_clamp01(s->trauma - s->decay_per_sec * dt);
}

float jce_camera_shake_amount(const JceCameraShake *s)
{
    if (!s) return 0.0f;
    return s->trauma * s->trauma;
}

bool jce_camera_shake_active(const JceCameraShake *s)
{
    return s && s->trauma > 0.0f;
}

void jce_camera_shake_offset(const JceCameraShake *s,
                             float out_pos[3], float out_euler_deg[3])
{
    float fx = s ? (s->trauma * s->trauma) : 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (out_pos)
            out_pos[i] = s ? fx * s->max_pos[i] * shake_noise(s->seed, i, s->time, s->frequency) : 0.0f;
        if (out_euler_deg)
            out_euler_deg[i] = s ? fx * s->max_rot_deg[i] * shake_noise(s->seed, i + 3, s->time, s->frequency) : 0.0f;
    }
}
