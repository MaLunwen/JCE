/*
 * jce_wind_zone.c  Wind zone registry + sampler.
 *
 * The turbulence noise is a sin/cos lattice — cheap, periodic, good
 * enough for foliage/cloth.  Not a substitute for Perlin/curl when
 * artistic quality matters; callers can layer custom forces on top.
 */

#include <jce/middleware/world/jce_wind_zone.h>

#include <math.h>
#include <string.h>

static JceWindZone s_zones[JCE_WIND_ZONE_MAX];

void jce_wind_clear(void)
{
    memset(s_zones, 0, sizeof(s_zones));
}

uint16_t jce_wind_register(const JceWindZone *zone)
{
    if (!zone) return JCE_WIND_ZONE_MAX;
    for (uint16_t i = 0; i < JCE_WIND_ZONE_MAX; ++i) {
        if (!s_zones[i].active) {
            s_zones[i] = *zone;
            s_zones[i].active = true;
            return i;
        }
    }
    return JCE_WIND_ZONE_MAX;
}

bool jce_wind_remove(uint16_t id)
{
    if (id >= JCE_WIND_ZONE_MAX) return false;
    if (!s_zones[id].active) return false;
    s_zones[id].active = false;
    return true;
}

JceWindZone *jce_wind_get(uint16_t id)
{
    if (id >= JCE_WIND_ZONE_MAX) return NULL;
    return s_zones[id].active ? &s_zones[id] : NULL;
}

uint16_t jce_wind_active_count(void)
{
    uint16_t n = 0;
    for (uint16_t i = 0; i < JCE_WIND_ZONE_MAX; ++i)
        if (s_zones[i].active) n++;
    return n;
}

static float noise01(float x, float y, float z, float t)
{
    /* Cheap lattice noise — sum of two phase-shifted cosines. */
    float n = 0.5f + 0.5f * cosf(x * 0.13f + y * 0.07f + z * 0.11f + t);
    n      += 0.5f + 0.5f * cosf(x * 0.27f - y * 0.19f + z * 0.23f + t * 1.7f);
    return n * 0.5f;
}

void jce_wind_sample(const float *pos, float time_s, float *out)
{
    if (!out) return;
    out[0] = out[1] = out[2] = 0.0f;
    if (!pos) return;
    for (uint16_t i = 0; i < JCE_WIND_ZONE_MAX; ++i) {
        const JceWindZone *z = &s_zones[i];
        if (!z->active) continue;
        float scale = 1.0f;
        float dir[3] = { z->direction[0], z->direction[1], z->direction[2] };
        if (z->mode == JCE_WIND_ZONE_SPHERICAL) {
            float dx = pos[0] - z->center[0];
            float dy = pos[1] - z->center[1];
            float dz = pos[2] - z->center[2];
            float d2 = dx*dx + dy*dy + dz*dz;
            float r2 = z->radius * z->radius;
            if (r2 <= 0.0f || d2 >= r2) continue;
            float fall = 1.0f - sqrtf(d2 / r2);
            scale *= fall * fall;
            float L = sqrtf(d2);
            if (L > 1e-5f) {
                dir[0] = dx / L; dir[1] = dy / L; dir[2] = dz / L;
            }
        }
        /* Pulsing scale via low-freq cosine. */
        if (z->pulse_frequency > 0.0f) {
            float p = cosf(time_s * z->pulse_frequency * 6.2831853f);
            scale *= 1.0f + z->pulse_magnitude * p;
        }
        float main_f = z->main_strength * scale;
        out[0] += dir[0] * main_f;
        out[1] += dir[1] * main_f;
        out[2] += dir[2] * main_f;
        /* Turbulence — independently per axis. */
        if (z->turbulence_strength > 0.0f) {
            float tx = noise01(pos[0], pos[1], pos[2],     time_s);
            float ty = noise01(pos[0], pos[1], pos[2], 3 + time_s);
            float tz = noise01(pos[0], pos[1], pos[2], 7 + time_s);
            float a  = z->turbulence_strength * scale;
            out[0] += (tx - 0.5f) * 2.0f * a;
            out[1] += (ty - 0.5f) * 2.0f * a;
            out[2] += (tz - 0.5f) * 2.0f * a;
        }
    }
}
