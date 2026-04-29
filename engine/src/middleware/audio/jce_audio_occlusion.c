/*
 * jce_audio_occlusion.c -- generic audio occlusion / obstruction.
 */

#include <jce/middleware/audio/jce_audio_occlusion.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "audio_occ"

JceAudioOcclusionParams jce_audio_occlusion_default_params(void)
{
    JceAudioOcclusionParams p;
    p.min_lowpass_hz    = 400.0f;
    p.max_lowpass_hz    = 22050.0f;
    p.min_direct_volume = 0.15f;
    p.smoothing         = 0.85f;
    p.max_raycast_dist  = 200.0f;
    return p;
}

static void apply_curves(const JceAudioOcclusionParams *p,
                         float occ,
                         float *out_lowpass, float *out_atten)
{
    if (occ < 0.0f) occ = 0.0f;
    if (occ > 1.0f) occ = 1.0f;
    /* Direct volume drops linearly from 1.0 to min_direct_volume. */
    *out_atten = 1.0f - (1.0f - p->min_direct_volume) * occ;
    /* Low-pass cutoff sweeps logarithmically from max -> min. */
    float lo = p->min_lowpass_hz   <= 1.0f ? 1.0f : p->min_lowpass_hz;
    float hi = p->max_lowpass_hz   <= lo  ? lo + 1.0f : p->max_lowpass_hz;
    float k  = expf(logf(lo / hi) * occ);
    *out_lowpass = hi * k;
}

void jce_audio_occlusion_solve(const JceAudioOcclusionParams *params,
                               jce_vec3 listener,
                               JceAudioOcclusionQuery *q, uint32_t n,
                               JceAudioOcclusionRaycastFn raycast,
                               void *ud)
{
    if (!params || !q || n == 0 || !raycast) return;
    JCE_PROFILE_ZONE_N("Audio::Occlusion::solve");
    JCE_PROFILE_PLOT_I("audio.occ.queries", (int64_t)n);
    for (uint32_t i = 0; i < n; ++i) {
        jce_vec3 dv = jce_v3_sub(q[i].source_position, listener);
        float dist = sqrtf(jce_v3_dot(dv, dv));
        if (dist <= 1e-4f || dist > params->max_raycast_dist) {
            q[i].occlusion   = 0.0f;
            q[i].lowpass_hz  = params->max_lowpass_hz;
            q[i].attenuation = 1.0f;
            continue;
        }
        jce_vec3 dir = { dv.x / dist, dv.y / dist, dv.z / dist };
        float absorption = 0.5f;
        float hit = raycast(ud, listener, dir, dist, &absorption);
        if (hit < 0.0f) hit = 0.0f;
        if (hit > 1.0f) hit = 1.0f;
        if (absorption < 0.0f) absorption = 0.0f;
        if (absorption > 1.0f) absorption = 1.0f;
        float occ = (1.0f - hit) * absorption;
        q[i].occlusion = occ;
        apply_curves(params, occ, &q[i].lowpass_hz, &q[i].attenuation);
    }
    JCE_PROFILE_ZONE_END;
}

/* ---------------------- stateful tracker ---------------------- */

typedef struct {
    uint64_t voice_id;       /* 0 = empty slot */
    uint32_t generation;     /* GC tag */
    float    smoothed_occ;
} TrackerEntry;

struct JceAudioOcclusionTracker {
    JceAudioOcclusionParams params;
    TrackerEntry           *table;
    uint32_t                cap;        /* power of two */
    uint32_t                size;
    uint32_t                gc_gen;
};

static uint32_t hash_u64(uint64_t x)
{
    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static TrackerEntry *table_find_or_insert(JceAudioOcclusionTracker *t, uint64_t id)
{
    /* Linear-probe open-addressed table; resize when 70% full. */
    if (t->size * 10u >= t->cap * 7u) {
        uint32_t new_cap = t->cap * 2u;
        TrackerEntry *new_tab = (TrackerEntry *)JCE_CALLOC(new_cap, sizeof(TrackerEntry));
        if (!new_tab) return NULL;
        for (uint32_t i = 0; i < t->cap; ++i) {
            if (t->table[i].voice_id == 0) continue;
            uint32_t h = hash_u64(t->table[i].voice_id) & (new_cap - 1u);
            while (new_tab[h].voice_id != 0) h = (h + 1u) & (new_cap - 1u);
            new_tab[h] = t->table[i];
        }
        JCE_FREE(t->table);
        t->table = new_tab;
        t->cap   = new_cap;
    }
    uint32_t h = hash_u64(id) & (t->cap - 1u);
    while (t->table[h].voice_id != 0 && t->table[h].voice_id != id)
        h = (h + 1u) & (t->cap - 1u);
    if (t->table[h].voice_id == 0) {
        t->table[h].voice_id     = id;
        t->table[h].smoothed_occ = 0.0f;
        t->size++;
    }
    t->table[h].generation = t->gc_gen;
    return &t->table[h];
}

JceAudioOcclusionTracker *jce_audio_occlusion_tracker_create(uint32_t cap)
{
    if (cap < 16) cap = 16;
    /* Round up to power of two. */
    uint32_t p = 1; while (p < cap) p <<= 1;
    JceAudioOcclusionTracker *t = (JceAudioOcclusionTracker *)JCE_CALLOC(1, sizeof(*t));
    if (!t) return NULL;
    t->table = (TrackerEntry *)JCE_CALLOC(p, sizeof(TrackerEntry));
    if (!t->table) { JCE_FREE(t); return NULL; }
    t->cap    = p;
    t->params = jce_audio_occlusion_default_params();
    return t;
}

void jce_audio_occlusion_tracker_destroy(JceAudioOcclusionTracker *t)
{
    if (!t) return;
    JCE_FREE(t->table);
    JCE_FREE(t);
}

void jce_audio_occlusion_tracker_set_params(JceAudioOcclusionTracker *t,
                                            const JceAudioOcclusionParams *p)
{ if (t && p) t->params = *p; }

void jce_audio_occlusion_tracker_solve(JceAudioOcclusionTracker *t,
                                       jce_vec3 listener,
                                       const uint64_t *ids,
                                       JceAudioOcclusionQuery *q, uint32_t n,
                                       JceAudioOcclusionRaycastFn raycast, void *ud)
{
    if (!t || !ids || !q || n == 0 || !raycast) return;
    JCE_PROFILE_ZONE_N("Audio::Occlusion::tracker_solve");
    JCE_PROFILE_PLOT_I("audio.occ.tracker_queries", (int64_t)n);
    float s = t->params.smoothing;
    if (s < 0.0f) s = 0.0f;
    if (s > 0.999f) s = 0.999f;

    for (uint32_t i = 0; i < n; ++i) {
        TrackerEntry *e = table_find_or_insert(t, ids[i]);
        if (!e) {
            q[i].occlusion = 0.0f;
            q[i].lowpass_hz = t->params.max_lowpass_hz;
            q[i].attenuation = 1.0f;
            continue;
        }
        jce_vec3 dv = jce_v3_sub(q[i].source_position, listener);
        float dist = sqrtf(jce_v3_dot(dv, dv));
        float raw_occ = 0.0f;
        if (dist > 1e-4f && dist <= t->params.max_raycast_dist) {
            jce_vec3 dir = { dv.x / dist, dv.y / dist, dv.z / dist };
            float absorption = 0.5f;
            float hit = raycast(ud, listener, dir, dist, &absorption);
            if (hit < 0.0f) hit = 0.0f;
            if (hit > 1.0f) hit = 1.0f;
            if (absorption < 0.0f) absorption = 0.0f;
            if (absorption > 1.0f) absorption = 1.0f;
            raw_occ = (1.0f - hit) * absorption;
        }
        e->smoothed_occ = s * e->smoothed_occ + (1.0f - s) * raw_occ;
        q[i].occlusion = e->smoothed_occ;
        apply_curves(&t->params, e->smoothed_occ, &q[i].lowpass_hz, &q[i].attenuation);
    }
    JCE_PROFILE_ZONE_END;
}

void jce_audio_occlusion_tracker_gc(JceAudioOcclusionTracker *t,
                                    const uint64_t *active, uint32_t active_count)
{
    if (!t) return;
    t->gc_gen++;
    /* Mark active. */
    for (uint32_t i = 0; i < active_count; ++i) {
        if (active[i] == 0) continue;
        uint32_t h = hash_u64(active[i]) & (t->cap - 1u);
        while (t->table[h].voice_id != 0 && t->table[h].voice_id != active[i])
            h = (h + 1u) & (t->cap - 1u);
        if (t->table[h].voice_id == active[i])
            t->table[h].generation = t->gc_gen;
    }
    /* Sweep + repack with backward-shift. */
    for (uint32_t i = 0; i < t->cap; ++i) {
        if (t->table[i].voice_id != 0 && t->table[i].generation != t->gc_gen) {
            t->table[i].voice_id = 0;
            t->size--;
            uint32_t j = (i + 1u) & (t->cap - 1u);
            while (t->table[j].voice_id != 0) {
                uint32_t home = hash_u64(t->table[j].voice_id) & (t->cap - 1u);
                if (((j - home) & (t->cap - 1u)) > 0) {
                    t->table[i] = t->table[j];
                    t->table[j].voice_id = 0;
                    i = j;
                }
                j = (j + 1u) & (t->cap - 1u);
            }
        }
    }
}
