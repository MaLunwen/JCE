/*
 * jce_sensor.c  AI perception runtime.
 */

#include <jce/middleware/ai/jce_sensor.h>

#include <math.h>
#include <string.h>

static JceSensorStimulus s_sound_queue[JCE_SENSOR_SOUND_QUEUE];
static uint32_t           s_sound_count;

void jce_sensor_init(JceSensor *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->forward[2] = 1.0f;
    s->up[1] = 1.0f;
    s->vision_fov_deg     = 90.0f;
    s->vision_range       = 20.0f;
    s->hearing_range      = 15.0f;
    s->memory_half_life   = 5.0f;
}

void jce_sensor_update_memory(JceSensor *s, float dt)
{
    if (!s || dt <= 0.0f) return;
    float factor = 1.0f;
    if (s->memory_half_life > 0.0f)
        factor = powf(0.5f, dt / s->memory_half_life);
    for (uint32_t i = 0; i < s->memory_count;) {
        s->memory[i].confidence *= factor;
        if (s->memory[i].confidence < 0.05f) {
            s->memory[i] = s->memory[s->memory_count - 1];
            s->memory_count--;
            continue;
        }
        i++;
    }
}

static void remember(JceSensor *s, JceSensorStimulus stim)
{
    /* Replace existing entry with same source_entity. */
    for (uint32_t i = 0; i < s->memory_count; ++i) {
        if (s->memory[i].source_entity == stim.source_entity) {
            s->memory[i] = stim;
            return;
        }
    }
    if (s->memory_count >= JCE_SENSOR_MEMORY_MAX) {
        /* Evict lowest-confidence. */
        uint32_t worst = 0;
        for (uint32_t i = 1; i < s->memory_count; ++i)
            if (s->memory[i].confidence < s->memory[worst].confidence)
                worst = i;
        s->memory[worst] = stim;
    } else {
        s->memory[s->memory_count++] = stim;
    }
}

uint32_t jce_sensor_query_visual(JceSensor *s,
                                   const uint64_t *cands,
                                   const float *pos_triples,
                                   uint32_t count,
                                   float t_now,
                                   JceSensorLosFn los_fn,
                                   void *los_user)
{
    if (!s || !cands || !pos_triples) return 0;
    float cos_half_fov = cosf((s->vision_fov_deg * 0.5f) * 0.01745329f);
    float range2 = s->vision_range * s->vision_range;
    uint32_t added = 0;
    for (uint32_t i = 0; i < count; ++i) {
        float dx = pos_triples[i*3+0] - s->position[0];
        float dy = pos_triples[i*3+1] - s->position[1];
        float dz = pos_triples[i*3+2] - s->position[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > range2) continue;
        float L = sqrtf(d2);
        if (L < 1e-6f) continue;
        float dot = (dx * s->forward[0] +
                     dy * s->forward[1] +
                     dz * s->forward[2]) / L;
        if (dot < cos_half_fov) continue;
        if (los_fn && !los_fn(s->position, &pos_triples[i*3], los_user)) continue;

        JceSensorStimulus stim;
        memset(&stim, 0, sizeof(stim));
        stim.kind          = JCE_SENSOR_STIMULUS_VISUAL;
        stim.source_entity = cands[i];
        stim.position[0]   = pos_triples[i*3+0];
        stim.position[1]   = pos_triples[i*3+1];
        stim.position[2]   = pos_triples[i*3+2];
        stim.strength      = 1.0f - sqrtf(d2) / s->vision_range;
        stim.time_seconds  = t_now;
        stim.confidence    = 1.0f;
        remember(s, stim);
        added++;
    }
    return added;
}

bool jce_sensor_report_sound(uint64_t emitter, const float pos[3],
                              float loud, float t_now)
{
    if (s_sound_count >= JCE_SENSOR_SOUND_QUEUE) return false;
    JceSensorStimulus *s = &s_sound_queue[s_sound_count++];
    memset(s, 0, sizeof(*s));
    s->kind          = JCE_SENSOR_STIMULUS_SOUND;
    s->source_entity = emitter;
    s->position[0]   = pos[0];
    s->position[1]   = pos[1];
    s->position[2]   = pos[2];
    s->strength      = loud;
    s->time_seconds  = t_now;
    s->confidence    = 1.0f;
    return true;
}

uint32_t jce_sensor_pull_sounds(JceSensor *s)
{
    if (!s) return 0;
    uint32_t added = 0;
    float r2 = s->hearing_range * s->hearing_range;
    for (uint32_t i = 0; i < s_sound_count; ++i) {
        JceSensorStimulus *snd = &s_sound_queue[i];
        float dx = snd->position[0] - s->position[0];
        float dy = snd->position[1] - s->position[1];
        float dz = snd->position[2] - s->position[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > r2) continue;
        JceSensorStimulus stim = *snd;
        stim.confidence = snd->strength *
                           (1.0f - sqrtf(d2) / s->hearing_range);
        remember(s, stim);
        added++;
    }
    return added;
}

void jce_sensor_clear_sounds(void) { s_sound_count = 0; }
