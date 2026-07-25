/*
 * jce_audio_ecs.c -- flecs ECS adapter for the audio middleware.
 *
 * Implementation strategy:
 *   - Each tick function builds a flecs query over the relevant
 *     component, then iterates and calls the underlying audio API.
 *   - Queries are cached on the JceAudioEcs handle to avoid per-frame
 *     query construction (zero hot-path alloc).
 */

#include <jce/middleware/audio/jce_audio_ecs.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>

#include "os/core/jce_memory.h"

#include <flecs.h>
#include <string.h>

#define LOG_TAG "audio_ecs"

/* ── Component IDs (one set per shared library) ────────────────────── */

ECS_COMPONENT_DECLARE(JceReverbZoneEcs);
ECS_COMPONENT_DECLARE(JceOcclusionSourceEcs);
ECS_COMPONENT_DECLARE(JceMixerBusEcs);

/* ── Handle ────────────────────────────────────────────────────────── */

struct JceAudioEcs {
    ecs_world_t              *world;
    JceReverbZones           *rz;
    JceAudioOcclusionTracker *occ;
    JceAudioMixer            *mx;

    ecs_query_t *q_zones;
    ecs_query_t *q_occ;
    ecs_query_t *q_mixer;

    /* Scratch buffers for batch APIs (grow-only). */
    uint64_t                *occ_ids;
    JceAudioOcclusionQuery  *occ_queries;
    uint32_t                 occ_cap;
};

/* ── Lifetime ──────────────────────────────────────────────────────── */

JceAudioEcs *jce_audio_ecs_create(void                     *world_opaque,
                                  JceReverbZones           *rz,
                                  JceAudioOcclusionTracker *occ,
                                  JceAudioMixer            *mx)
{
    ecs_world_t *world = (ecs_world_t *)world_opaque;
    if (!world) return NULL;
    JceAudioEcs *a = (JceAudioEcs *)JCE_CALLOC(1, sizeof(*a));
    if (!a) return NULL;

    a->world = world;
    a->rz    = rz;
    a->occ   = occ;
    a->mx    = mx;

    ECS_COMPONENT_DEFINE(world, JceReverbZoneEcs);
    ECS_COMPONENT_DEFINE(world, JceOcclusionSourceEcs);
    ECS_COMPONENT_DEFINE(world, JceMixerBusEcs);

    a->q_zones = ecs_query(world, {
        .terms = {{ ecs_id(JceReverbZoneEcs) }}
    });
    a->q_occ = ecs_query(world, {
        .terms = {{ ecs_id(JceOcclusionSourceEcs) }}
    });
    a->q_mixer = ecs_query(world, {
        .terms = {{ ecs_id(JceMixerBusEcs) }}
    });

    LOG_SUCCESS(LOG_TAG, "audio ECS adapter created (rz=%p occ=%p mx=%p)",
                (void*)rz, (void*)occ, (void*)mx);
    return a;
}

void jce_audio_ecs_destroy(JceAudioEcs *a)
{
    if (!a) return;
    if (a->q_zones) ecs_query_fini(a->q_zones);
    if (a->q_occ)   ecs_query_fini(a->q_occ);
    if (a->q_mixer) ecs_query_fini(a->q_mixer);
    JCE_FREE(a->occ_ids);
    JCE_FREE(a->occ_queries);
    JCE_FREE(a);
}

/* ── Reverb zone sync ──────────────────────────────────────────────── */

void jce_audio_ecs_sync_zones(JceAudioEcs *a)
{
    if (!a || !a->rz || !a->q_zones) return;
    JCE_PROFILE_ZONE_N("AudioECS::sync_zones");

    ecs_iter_t it = ecs_query_iter(a->world, a->q_zones);
    while (ecs_query_next(&it)) {
        JceReverbZoneEcs *zs = ecs_field(&it, JceReverbZoneEcs, 0);
        if (!zs) continue;
        for (int i = 0; i < it.count; ++i) {
            JceReverbZoneEcs *z = &zs[i];
            if (z->zone_id == 0) {
                z->zone_id = jce_reverb_zones_add(a->rz, &z->desc);
                z->dirty = false;
            } else if (z->dirty) {
                jce_reverb_zones_update(a->rz, z->zone_id, &z->desc);
                z->dirty = false;
            }
        }
    }
    JCE_PROFILE_ZONE_END;
}

void jce_audio_ecs_sample_reverb(JceAudioEcs *a,
                                 jce_vec3 listener_pos,
                                 JceReverbPreset *out)
{
    if (!out) return;
    if (!a || !a->rz) {
        memset(out, 0, sizeof(*out));
        return;
    }
    jce_reverb_zones_sample(a->rz, listener_pos, out);
}

/* ── Occlusion solve ───────────────────────────────────────────────── */

static bool occ_grow(JceAudioEcs *a, uint32_t need)
{
    if (need <= a->occ_cap) return true;
    uint32_t cap = a->occ_cap ? a->occ_cap : 16u;
    while (cap < need) cap *= 2u;
    uint64_t *ids = (uint64_t *)JCE_REALLOC(a->occ_ids,
                                            (size_t)cap * sizeof(uint64_t));
    JceAudioOcclusionQuery *q = (JceAudioOcclusionQuery *)JCE_REALLOC(
        a->occ_queries, (size_t)cap * sizeof(JceAudioOcclusionQuery));
    if (!ids || !q) return false;
    a->occ_ids     = ids;
    a->occ_queries = q;
    a->occ_cap     = cap;
    return true;
}

void jce_audio_ecs_solve_occlusion(JceAudioEcs *a,
                                   jce_vec3 listener_pos,
                                   JceAudioOcclusionRaycastFn raycast,
                                   void *ud)
{
    if (!a || !a->occ || !a->q_occ || !raycast) return;
    JCE_PROFILE_ZONE_N("AudioECS::solve_occlusion");

    /* First pass: count + grow scratch. */
    uint32_t total = 0;
    {
        ecs_iter_t it = ecs_query_iter(a->world, a->q_occ);
        while (ecs_query_next(&it)) total += (uint32_t)it.count;
    }
    if (total == 0) { JCE_PROFILE_ZONE_END; return; }
    if (!occ_grow(a, total)) {
        LOG_ERROR(LOG_TAG, "OOM growing occlusion scratch to %u", total);
        JCE_PROFILE_ZONE_END;
        return;
    }

    /* Second pass: pack inputs. */
    uint32_t off = 0;
    {
        ecs_iter_t it = ecs_query_iter(a->world, a->q_occ);
        while (ecs_query_next(&it)) {
            JceOcclusionSourceEcs *ss =
                ecs_field(&it, JceOcclusionSourceEcs, 0);
            if (!ss) continue;
            for (int i = 0; i < it.count; ++i, ++off) {
                a->occ_ids[off]                   = ss[i].voice_id;
                a->occ_queries[off].source_position = ss[i].source_position;
            }
        }
    }

    jce_audio_occlusion_tracker_solve(a->occ, listener_pos,
                                      a->occ_ids, a->occ_queries, total,
                                      raycast, ud);

    /* Third pass: scatter outputs back. */
    off = 0;
    {
        ecs_iter_t it = ecs_query_iter(a->world, a->q_occ);
        while (ecs_query_next(&it)) {
            JceOcclusionSourceEcs *ss =
                ecs_field(&it, JceOcclusionSourceEcs, 0);
            if (!ss) continue;
            for (int i = 0; i < it.count; ++i, ++off) {
                ss[i].occlusion   = a->occ_queries[off].occlusion;
                ss[i].lowpass_hz  = a->occ_queries[off].lowpass_hz;
                ss[i].attenuation = a->occ_queries[off].attenuation;
            }
        }
    }
    JCE_PROFILE_ZONE_END;
}

/* ── Mixer bus apply ───────────────────────────────────────────────── */

void jce_audio_ecs_apply_mixer(JceAudioEcs *a)
{
    if (!a || !a->mx || !a->q_mixer) return;
    JCE_PROFILE_ZONE_N("AudioECS::apply_mixer");

    ecs_iter_t it = ecs_query_iter(a->world, a->q_mixer);
    while (ecs_query_next(&it)) {
        JceMixerBusEcs *bs = ecs_field(&it, JceMixerBusEcs, 0);
        if (!bs) continue;
        for (int i = 0; i < it.count; ++i) {
            JceMixerBusEcs *b = &bs[i];
            if (b->voice_id == 0) continue;
            if (b->bus != b->last_applied_bus) {
                if (b->bus == JCE_AUDIO_BUS_INVALID) {
                    jce_audio_mixer_unassign_voice(a->mx, b->voice_id);
                } else {
                    jce_audio_mixer_assign_voice(a->mx, b->voice_id, b->bus);
                }
                b->last_applied_bus = b->bus;
            }
        }
    }
    JCE_PROFILE_ZONE_END;
}
