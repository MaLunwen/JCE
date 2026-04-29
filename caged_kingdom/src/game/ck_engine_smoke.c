/*
 * ck_engine_smoke.c — Stage 17-26 integration smoke tests.
 *
 * Each STest_* function:
 *   - returns true on pass, false on fail (logs why).
 *   - is self-contained: creates → exercises → destroys all state.
 *   - has no external prerequisites (no PAK, no bgfx, no flecs world).
 *
 * Renderer post-effects (SSR / VolFog) cannot be fully constructed
 * without a live bgfx + PAK; we exercise the parameter-clamp path only.
 * The full create/render path is exercised by the editor target, which
 * also acts as integration coverage.
 *
 * ECS adapters are not exercised here (they require a flecs world and
 * are already covered transitively by the editor build linking them).
 */

#include "ck_engine_smoke.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_alloc.h>

#include <jce/middleware/audio/jce_audio_occlusion.h>
#include <jce/middleware/audio/jce_reverb_zones.h>
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/ai/jce_graph_astar.h>
#include <jce/middleware/ai/jce_steering.h>
#include <jce/middleware/world/jce_trigger_volume.h>
#include <jce/middleware/save/jce_snapshot.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_shadow_filter.h>
#include <jce/renderer/jce_ssr.h>
#include <jce/renderer/jce_volumetric_fog.h>

/* ECS adapter headers (Pass C deliverables). */
#include <jce/middleware/audio/jce_audio_ecs.h>
#include <jce/middleware/ai/jce_ai_ecs.h>
#include <jce/middleware/world/jce_world_ecs.h>
#include <jce/renderer/jce_renderer_ecs.h>

#include <math.h>
#include <string.h>

#define LOG_TAG "ck_smoke"

#define EXPECT(cond, msg) do {                                              \
    if (!(cond)) { LOG_ERROR(LOG_TAG, "  FAIL: " msg); return false; }      \
} while (0)

/* ---------- Stage 17 — audio_occlusion ---------- */
static float fake_raycast(void *ud, jce_vec3 o, jce_vec3 d, float maxd, float *abs_out)
{
    (void)ud; (void)o; (void)d; (void)maxd;
    *abs_out = 0.5f;
    return 0.3f;  /* hit value */
}

static bool s_audio_occlusion(void)
{
    JceAudioOcclusionParams p = jce_audio_occlusion_default_params();
    EXPECT(p.min_lowpass_hz < p.max_lowpass_hz, "default params: min < max lowpass");
    EXPECT(p.smoothing >= 0.0f && p.smoothing < 1.0f, "smoothing in [0,1)");

    JceAudioOcclusionQuery q[2] = {
        { .source_position = { 1, 0, 0 } },
        { .source_position = { 10, 0, 0 } },
    };
    jce_vec3 listener = { 0, 0, 0 };
    jce_audio_occlusion_solve(&p, listener, q, 2, fake_raycast, NULL);
    EXPECT(q[0].occlusion >= 0.0f && q[0].occlusion <= 1.0f, "occlusion in [0,1]");
    EXPECT(q[0].lowpass_hz >= p.min_lowpass_hz, "lowpass >= floor");
    EXPECT(q[0].lowpass_hz <= p.max_lowpass_hz + 1.0f, "lowpass <= ceiling");

    JceAudioOcclusionTracker *t = jce_audio_occlusion_tracker_create(8);
    EXPECT(t != NULL, "tracker create");
    jce_audio_occlusion_tracker_set_params(t, &p);
    jce_audio_occlusion_tracker_destroy(t);
    return true;
}

/* ---------- Stage 18 — reverb_zones ---------- */
static bool s_reverb_zones(void)
{
    JceReverbZones *r = jce_reverb_zones_create(4);
    EXPECT(r != NULL, "create");
    EXPECT(jce_reverb_zones_count(r) == 0, "empty count");

    JceReverbZoneDesc d = { 0 };
    d.shape = JCE_REVERB_SHAPE_SPHERE;
    d.center = (jce_vec3){ 0, 0, 0 };
    d.extents = (jce_vec3){ 5, 0, 0 };
    d.priority = 1;
    d.preset = jce_reverb_preset_room();

    JceReverbZoneId id = jce_reverb_zones_add(r, &d);
    EXPECT(id != JCE_REVERB_ZONE_INVALID, "add returns valid id");
    EXPECT(jce_reverb_zones_count(r) == 1, "count==1 after add");
    EXPECT(jce_reverb_zones_remove(r, id), "remove valid id");
    EXPECT(jce_reverb_zones_count(r) == 0, "count==0 after remove");

    jce_reverb_zones_destroy(r);
    return true;
}

/* ---------- Stage 19 — audio_mixer ---------- */
static bool s_audio_mixer(void)
{
    JceAudioMixer *m = jce_audio_mixer_create();
    EXPECT(m != NULL, "create");

    JceAudioBusId master = jce_audio_mixer_add_bus(m, JCE_AUDIO_BUS_INVALID, "master", 1.0f);
    JceAudioBusId music  = jce_audio_mixer_add_bus(m, master, "music", 1.0f);
    JceAudioBusId sfx    = jce_audio_mixer_add_bus(m, master, "sfx",   1.0f);
    EXPECT(master && music && sfx, "add_bus returns non-zero");

    EXPECT(jce_audio_mixer_find_bus(m, "music") == music, "find_bus");
    EXPECT(jce_audio_mixer_get_parent(m, music) == master, "parent==master");

    jce_audio_mixer_set_volume(m, music, 0.5f);
    EXPECT(jce_audio_mixer_get_volume(m, music) == 0.5f, "set/get volume");

    /* Solo idempotency: second set with same value must not double-count. */
    jce_audio_mixer_set_solo(m, sfx, true);
    jce_audio_mixer_set_solo(m, sfx, true);
    jce_audio_mixer_set_solo(m, sfx, false);
    EXPECT(!jce_audio_mixer_is_solo(m, sfx), "solo cleared after one false");
    EXPECT(jce_audio_mixer_resolve_volume(m, music) > 0.0f,
           "no soloed bus → music audible");

    jce_audio_mixer_destroy(m);
    return true;
}

/* ---------- Stage 20 — graph_astar ---------- */
/* 4-node line graph: 0—1—2—3, all edges cost 1. */
static uint32_t line_neighbors(uint32_t n, uint32_t *out, float *cost,
                               uint32_t cap, void *ud)
{
    (void)ud;
    uint32_t k = 0;
    if (n > 0 && k < cap) { out[k] = n - 1; cost[k] = 1.0f; k++; }
    if (n < 3 && k < cap) { out[k] = n + 1; cost[k] = 1.0f; k++; }
    return k;
}
static float line_h(uint32_t n, uint32_t goal, void *ud)
{ (void)ud; return (float)((int32_t)goal - (int32_t)n < 0 ? -((int32_t)goal - (int32_t)n) : (int32_t)goal - (int32_t)n); }

static bool s_graph_astar(void)
{
    JceGraphAstar *a = jce_graph_astar_create();
    EXPECT(a != NULL, "create");
    jce_graph_astar_set_callbacks(a, line_neighbors, line_h, NULL);

    uint32_t path[16]; uint32_t count = 0;
    bool ok = jce_graph_astar_search(a, 0, 3, path, 16, &count);
    EXPECT(ok, "0->3 reachable");
    EXPECT(count == 4, "path length 4 (0,1,2,3)");
    EXPECT(path[0] == 0 && path[3] == 3, "endpoints correct");
    EXPECT(jce_graph_astar_last_cost(a) == 3.0f, "cost == 3");

    jce_graph_astar_destroy(a);
    return true;
}

/* ---------- Stage 21 — steering ---------- */
static bool s_steering(void)
{
    jce_vec3 pos = { 0, 0, 0 };
    jce_vec3 vel = { 0, 0, 0 };
    jce_vec3 tgt = { 10, 0, 0 };
    jce_vec3 f = jce_steer_seek(pos, vel, tgt, 5.0f);
    EXPECT(f.x > 0.0f, "seek pulls toward +X");

    f = jce_steer_flee(pos, vel, tgt, 5.0f);
    EXPECT(f.x < 0.0f, "flee pushes away from +X target");

    jce_vec3 long_v = { 100, 0, 0 };
    jce_vec3 clamped = jce_steer_truncate(long_v, 5.0f);
    float len_sq = clamped.x * clamped.x + clamped.y * clamped.y + clamped.z * clamped.z;
    EXPECT(fabsf(sqrtf(len_sq) - 5.0f) < 1e-3f, "truncate clamps to max_len");

    return true;
}

/* ---------- Stage 22 — trigger_volume ---------- */
static int s_trig_event_count = 0;
static void s_trig_evt(const JceTriggerEvent *e, void *ud)
{ (void)e; (void)ud; s_trig_event_count++; }

static bool s_trigger_volume(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    EXPECT(w != NULL, "create");
    jce_trigger_world_set_event_fn(w, s_trig_evt, NULL);

    JceTriggerDesc d = {
        .shape = JCE_TRIGGER_SPHERE,
        .center = { 0, 0, 0 },
        .half_extents = { 5.0f, 0, 0 },
    };
    JceTriggerHandle th = jce_trigger_add(w, &d, (uint64_t)0xCAFE);
    EXPECT(jce_trigger_valid(th), "trigger handle valid");

    JceObserverHandle oh = jce_observer_add(w, (jce_vec3){ 0, 0, 0 }, (uint64_t)0xBEEF);
    EXPECT(jce_observer_valid(oh), "observer handle valid");

    s_trig_event_count = 0;
    jce_trigger_world_update(w);  /* ENTER */
    EXPECT(s_trig_event_count >= 1, "ENTER event fired");

    /* Verify stay_event_period throttle: setting N=10 should suppress STAY
     * for the next ~9 frames. */
    jce_trigger_world_set_stay_event_period(w, 10);
    int before = s_trig_event_count;
    jce_trigger_world_update(w); jce_trigger_world_update(w);
    EXPECT(s_trig_event_count == before, "STAY throttled by period=10");

    /* Move out → EXIT. */
    jce_observer_set_position(w, oh, (jce_vec3){ 100, 0, 0 });
    int prev = s_trig_event_count;
    jce_trigger_world_update(w);
    EXPECT(s_trig_event_count > prev, "EXIT event fired");

    jce_trigger_world_destroy(w);
    return true;
}

/* ---------- Stage 23 — snapshot ---------- */
static bool s_snap_write(JceSnapshotStream *s, void *ud)
{ (void)ud; return jce_snap_write_u32(s, 0xDEADBEEFu); }
static uint32_t s_snap_read_value;
static bool s_snap_read(JceSnapshotStream *s, uint32_t v, void *ud)
{ (void)v; (void)ud; return jce_snap_read_u32(s, &s_snap_read_value); }

static bool s_snapshot(void)
{
    JceSnapshotRegistry *r = jce_snapshot_registry_create();
    EXPECT(r != NULL, "registry create");
    jce_snapshot_register(r, "test_sec", 1, s_snap_write, s_snap_read, NULL);

    void *buf = NULL; size_t sz = 0;
    EXPECT(jce_snapshot_save_to_buffer(r, &buf, &sz), "save_to_buffer");
    EXPECT(buf && sz > 24, "buffer non-empty");

    s_snap_read_value = 0;
    EXPECT(jce_snapshot_load_from_buffer(r, buf, sz), "load_from_buffer");
    EXPECT(s_snap_read_value == 0xDEADBEEFu, "round-trip value matches");

    /* Truncated buffer must fail gracefully (CRC). */
    EXPECT(!jce_snapshot_load_from_buffer(r, buf, sz - 1),
           "truncated load rejected");

    jce_free(buf);
    jce_snapshot_registry_destroy(r);
    return true;
}

/* ---------- Stage 24 — shadow_filter ---------- */
static bool s_shadow_filter(void)
{
    float pts[32];  /* 16 (x,y) pairs */
    jce_shadow_filter_poisson_disk(16, 0xC0FFEEu, pts);
    for (int i = 0; i < 16; ++i) {
        float x = pts[i * 2 + 0], y = pts[i * 2 + 1];
        float r2 = x * x + y * y;
        EXPECT(r2 <= 1.0001f, "poisson point inside unit disk");
    }

    /* PSSM writes cascade_count+1 entries; 4 cascades → 5 distances. */
    float splits[5];
    jce_shadow_filter_pssm_splits(0.1f, 100.0f, 4, 0.5f, splits);
    EXPECT(splits[0] >= 0.1f - 1e-3f, "PSSM split[0] == near");
    EXPECT(splits[4] <= 100.0f + 1e-3f, "PSSM split[N] == far");
    EXPECT(splits[0] < splits[1] && splits[1] < splits[2]
           && splits[2] < splits[3] && splits[3] < splits[4],
           "PSSM splits ascending");

    float kern[5];
    jce_shadow_filter_gaussian_1d(2, 1.0f, kern);
    float sum = kern[0] + kern[1] + kern[2] + kern[3] + kern[4];
    EXPECT(fabsf(sum - 1.0f) < 1e-3f, "gaussian kernel sums to 1");

    return true;
}

/* ---------- Stage 25 — ssr (params validation only) ---------- */
static bool s_ssr_params(void)
{
    JceSsrParams p = jce_ssr_default_params();
    EXPECT(p.max_distance > 0.0f, "default max_distance > 0");
    EXPECT(p.near_plane > 0.0f && p.far_plane > p.near_plane,
           "default near < far");
    EXPECT(p.step_count >= 1.0f && p.step_count <= 256.0f, "step_count sane");
    /* NULL desc → NULL handle (no crash). */
    EXPECT(jce_ssr_create(NULL) == NULL, "NULL desc rejected");
    return true;
}

/* ---------- Stage 26 — volumetric_fog (params validation only) ---------- */
static bool s_vfog_params(void)
{
    JceVolumetricFogParams p = jce_volumetric_fog_default_params();
    EXPECT(p.density >= 0.0f, "density >= 0");
    EXPECT(p.scattering >= 0.0f, "scattering >= 0");
    EXPECT(p.near_plane > 0.0f && p.far_plane > p.near_plane,
           "near < far");
    EXPECT(jce_volumetric_fog_create(NULL) == NULL, "NULL desc rejected");
    return true;
}

/* ---------- Driver ---------- */

typedef bool (*SmokeFn)(void);
typedef struct { const char *name; SmokeFn fn; } SmokeCase;

int ck_engine_smoke_run(void)
{
    static const SmokeCase cases[] = {
        { "Stage 17 audio_occlusion", s_audio_occlusion },
        { "Stage 18 reverb_zones",    s_reverb_zones    },
        { "Stage 19 audio_mixer",     s_audio_mixer     },
        { "Stage 20 graph_astar",     s_graph_astar     },
        { "Stage 21 steering",        s_steering        },
        { "Stage 22 trigger_volume",  s_trigger_volume  },
        { "Stage 23 snapshot",        s_snapshot        },
        { "Stage 24 shadow_filter",   s_shadow_filter   },
        { "Stage 25 ssr (params)",    s_ssr_params      },
        { "Stage 26 vfog (params)",   s_vfog_params     },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));

    LOG_INFO(LOG_TAG, "==================================================");
    LOG_INFO(LOG_TAG, "JCE engine smoke tests — %d cases", n);
    LOG_INFO(LOG_TAG, "==================================================");

    int passed = 0, failed = 0;
    for (int i = 0; i < n; ++i) {
        if (cases[i].fn()) {
            LOG_INFO(LOG_TAG, "  PASS  %s", cases[i].name);
            passed++;
        } else {
            LOG_ERROR(LOG_TAG, "  FAIL  %s", cases[i].name);
            failed++;
        }
    }

    LOG_INFO(LOG_TAG, "--------------------------------------------------");
    LOG_INFO(LOG_TAG, "Result: %d/%d passed, %d failed", passed, n, failed);
    LOG_INFO(LOG_TAG, "==================================================");

    /* Run ECS adapter smoke against a throwaway scene so it doesn't
     * depend on the rest of ck_app_create succeeding. */
    JceScene *tmp = jce_scene_create();
    int ecs_failed = ck_engine_smoke_run_ecs(jce_scene_get_world(tmp));
    jce_scene_destroy(tmp);

    return failed + ecs_failed;
}

/* ====================================================================
 * ECS adapter smoke (Pass C deliverables).
 *
 * These tests run AFTER a JceScene exists (we need its flecs world).
 * For each adapter we verify:
 *   1. _create() on a valid world returns non-NULL,
 *   2. _create(NULL world) is rejected (returns NULL),
 *   3. tick on an empty world is a graceful no-op (no crash),
 *   4. _destroy() doesn't crash and is idempotent for NULL.
 *
 * Component population (which would require flecs.h in ck) is covered
 * by the editor build and per-adapter unit verification.
 * ================================================================== */

static bool s_ecs_audio(void *world)
{
    JceAudioEcs *a = jce_audio_ecs_create((ecs_world_t *)world, NULL, NULL, NULL);
    EXPECT(a != NULL, "create with all-NULL modules ok");

    /* All ticks must be no-ops with NULL modules and empty world. */
    jce_audio_ecs_sync_zones(a);
    JceReverbPreset out = { 0 };
    jce_audio_ecs_sample_reverb(a, (jce_vec3){ 0, 0, 0 }, &out);
    jce_audio_ecs_solve_occlusion(a, (jce_vec3){ 0, 0, 0 }, NULL, NULL);
    jce_audio_ecs_apply_mixer(a);

    jce_audio_ecs_destroy(a);
    jce_audio_ecs_destroy(NULL);  /* NULL safe */
    return true;
}

static bool s_ecs_ai(void *world)
{
    /* Without an astar context the path tick must still no-op. */
    JceAiEcs *a = jce_ai_ecs_create((ecs_world_t *)world, NULL);
    EXPECT(a != NULL, "create without astar ok");
    jce_ai_ecs_tick_steering(a, 0.016f);
    jce_ai_ecs_tick_paths(a, 8);
    jce_ai_ecs_destroy(a);

    /* With a real astar the lifetime still works. */
    JceGraphAstar *gs = jce_graph_astar_create();
    JceAiEcs *b = jce_ai_ecs_create((ecs_world_t *)world, gs);
    EXPECT(b != NULL, "create with astar ok");
    jce_ai_ecs_tick_paths(b, 4);
    jce_ai_ecs_destroy(b);
    jce_graph_astar_destroy(gs);
    return true;
}

static bool s_ecs_world(void *world)
{
    JceTriggerWorld *tw = jce_trigger_world_create();
    EXPECT(tw != NULL, "trigger world create");
    JceWorldEcs *we = jce_world_ecs_create((ecs_world_t *)world, tw);
    EXPECT(we != NULL, "world ecs create");

    jce_world_ecs_tick_triggers(we);
    jce_world_ecs_tick_triggers(we);  /* idempotent */

    jce_world_ecs_destroy(we);
    jce_trigger_world_destroy(tw);
    return true;
}

static bool s_ecs_renderer(void *world)
{
    /* SSR/VFog modules need bgfx — pass NULL to test the adapter alone. */
    JceRendererEcs *re = jce_renderer_ecs_create((ecs_world_t *)world, NULL, NULL);
    EXPECT(re != NULL, "create with NULL effects ok");

    /* Tick with an empty world: no entity carries the component, so tick
     * walks zero rows and returns. Pass dummy view/proj. */
    jce_mat4 ident = { 0 };
    ident.raw[0][0] = ident.raw[1][1] = ident.raw[2][2] = ident.raw[3][3] = 1.0f;
    jce_renderer_ecs_tick_ssr(re, 0xFFFF, 0xFFFF, 0xFFFF, &ident, &ident, 0);
    jce_renderer_ecs_tick_fog(re, 0xFFFF, &ident, &ident, 0);

    jce_renderer_ecs_destroy(re);
    return true;
}

typedef bool (*EcsFn)(void *);
typedef struct { const char *name; EcsFn fn; } EcsCase;

int ck_engine_smoke_run_ecs(void *ecs_world)
{
    if (!ecs_world) {
        LOG_WARN(LOG_TAG, "ECS smoke skipped: scene world is NULL");
        return 0;
    }

    static const EcsCase cases[] = {
        { "Pass C 1/4 audio_ecs",    s_ecs_audio    },
        { "Pass C 2/4 ai_ecs",       s_ecs_ai       },
        { "Pass C 3/4 world_ecs",    s_ecs_world    },
        { "Pass C 4/4 renderer_ecs", s_ecs_renderer },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));

    LOG_INFO(LOG_TAG, "==================================================");
    LOG_INFO(LOG_TAG, "JCE ECS adapter smoke tests — %d cases", n);
    LOG_INFO(LOG_TAG, "==================================================");

    int passed = 0, failed = 0;
    for (int i = 0; i < n; ++i) {
        if (cases[i].fn(ecs_world)) {
            LOG_INFO(LOG_TAG, "  PASS  %s", cases[i].name);
            passed++;
        } else {
            LOG_ERROR(LOG_TAG, "  FAIL  %s", cases[i].name);
            failed++;
        }
    }

    LOG_INFO(LOG_TAG, "--------------------------------------------------");
    LOG_INFO(LOG_TAG, "ECS Result: %d/%d passed, %d failed", passed, n, failed);
    LOG_INFO(LOG_TAG, "==================================================");
    return failed;
}
