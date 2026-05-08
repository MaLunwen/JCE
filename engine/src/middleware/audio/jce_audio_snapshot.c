/*
 * jce_audio_snapshot.c  Mixer snapshot capture & fade implementation.
 *
 * The mixer hands out 1-based bus IDs in a fixed [0, MAX_BUSES] range,
 * so we record a flat array indexed by bus id.  Captured slots that
 * point to dead buses at apply-time are silently skipped.
 */

#include <jce/middleware/audio/jce_audio_snapshot.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG    "snapshot"
#define MAX_BUSES  128 /* must match jce_audio_mixer.c */

typedef struct {
    bool  alive;
    float volume;
    bool  muted;
    bool  solo;
} BusSlot;

struct JceAudioSnapshot {
    BusSlot slots[MAX_BUSES];
};

/* In-progress transition state lives outside the snapshot so the user
 * can capture/destroy snapshots freely.  Single global since fades are
 * inherently per-mixer and we expect one mixer per game.  Could move
 * into JceAudioMixer if multi-mixer support is ever needed. */
typedef struct {
    bool                   active;
    const JceAudioMixer   *mixer;     /* sanity check on tick */
    float                  total_seconds;
    float                  elapsed_seconds;
    JceAudioSnapshot       from;
    JceAudioSnapshot       to;
} FadeState;

static FadeState s_fade = {0};

JceAudioSnapshot *jce_audio_snapshot_capture(const JceAudioMixer *m)
{
    if (!m) return NULL;
    JceAudioSnapshot *s = (JceAudioSnapshot *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;

    JceAudioBusId ids[MAX_BUSES];
    uint32_t n = jce_audio_mixer_list_buses(m, ids, MAX_BUSES);
    for (uint32_t i = 0; i < n; ++i) {
        JceAudioBusId b = ids[i];
        if (b == 0 || b >= MAX_BUSES) continue;
        s->slots[b].alive  = true;
        s->slots[b].volume = jce_audio_mixer_get_volume(m, b);
        s->slots[b].muted  = jce_audio_mixer_is_muted(m, b);
        s->slots[b].solo   = jce_audio_mixer_is_solo(m, b);
    }
    return s;
}

void jce_audio_snapshot_destroy(JceAudioSnapshot *s)
{
    if (!s) return;
    JCE_FREE(s);
}

void jce_audio_snapshot_apply(JceAudioMixer *m, const JceAudioSnapshot *s)
{
    if (!m || !s) return;
    for (uint16_t b = 1; b < MAX_BUSES; ++b) {
        if (!s->slots[b].alive) continue;
        jce_audio_mixer_set_volume(m, b, s->slots[b].volume);
        jce_audio_mixer_set_muted (m, b, s->slots[b].muted);
        jce_audio_mixer_set_solo  (m, b, s->slots[b].solo);
    }
}

void jce_audio_snapshot_transition_to(JceAudioMixer *m,
                                      const JceAudioSnapshot *s,
                                      float seconds)
{
    if (!m || !s) return;

    if (seconds <= 0.0f) {
        s_fade.active = false;
        jce_audio_snapshot_apply(m, s);
        return;
    }

    /* Re-snapshot current state as the "from" so an interrupted fade
     * picks up smoothly instead of jumping. */
    JceAudioSnapshot *cur = jce_audio_snapshot_capture(m);
    if (cur) {
        s_fade.from = *cur;
        jce_audio_snapshot_destroy(cur);
    }
    s_fade.to              = *s;
    s_fade.mixer           = m;
    s_fade.total_seconds   = seconds;
    s_fade.elapsed_seconds = 0.0f;
    s_fade.active          = true;
}

/* Convert linear gain (0..1) to dB.  -∞ at 0, 0 at 1.  We clamp to a
 * floor of -120 dB to avoid log(0). */
static float linear_to_db(float lin)
{
    if (lin <= 0.000001f) return -120.0f;
    return 20.0f * log10f(lin);
}

static float db_to_linear(float db)
{
    return powf(10.0f, db * 0.05f);
}

void jce_audio_snapshot_tick(JceAudioMixer *m, float dt)
{
    if (!m || !s_fade.active) return;
    if (s_fade.mixer != m) return; /* fade was for a different mixer */

    s_fade.elapsed_seconds += dt;
    float t = s_fade.elapsed_seconds / s_fade.total_seconds;
    bool finished = false;
    if (t >= 1.0f) { t = 1.0f; finished = true; }

    for (uint16_t b = 1; b < MAX_BUSES; ++b) {
        if (!s_fade.from.slots[b].alive && !s_fade.to.slots[b].alive)
            continue;
        /* Volume: lerp in dB-space for perceptually-linear fades. */
        float v_from = s_fade.from.slots[b].alive ? s_fade.from.slots[b].volume
                                                   : s_fade.to.slots[b].volume;
        float v_to   = s_fade.to.slots[b].alive   ? s_fade.to.slots[b].volume
                                                   : s_fade.from.slots[b].volume;
        float db = linear_to_db(v_from) * (1.0f - t) + linear_to_db(v_to) * t;
        jce_audio_mixer_set_volume(m, b, db_to_linear(db));

        /* Booleans snap at midpoint so changes feel "decisive". */
        if (t >= 0.5f) {
            jce_audio_mixer_set_muted(m, b, s_fade.to.slots[b].muted);
            jce_audio_mixer_set_solo (m, b, s_fade.to.slots[b].solo);
        }
    }

    if (finished) s_fade.active = false;
}
