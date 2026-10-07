/* Consumer route through the same public world streamer and scene renderer. */
#include "stream_route.h"
#include <jce/api_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned route[] = {0, 1, 2, 1, 0};

static void spawned(const uint64_t *ids, uint32_t count, void *user)
{
    (void)ids;
    ((BenchStreamRoute *)user)->spawned += count;
}
static void despawned(const uint64_t *ids, uint32_t count, void *user)
{
    (void)ids;
    ((BenchStreamRoute *)user)->despawned += count;
}
static uint64_t residency(const uint64_t *ids, uint32_t count, void *user)
{
    BenchStreamRoute *s = user;
    return jce_scene_renderer_entities_vram_bytes(s->renderer, s->scene, ids, count);
}
void bench_stream_route_destroy(BenchStreamRoute *s)
{
    if (s->world) jce_world_streamer_destroy(s->world);
    if (s->fs) jce_fs_destroy(s->fs);
    memset(s, 0, sizeof(*s));
}
static bool topology_env(const char *name, unsigned fallback, unsigned maximum, unsigned *out)
{
    const char *value = getenv(name);
    if (!value) { *out = fallback; return true; }
    if (!*value) return false;
    unsigned parsed = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9' || parsed > maximum / 10u) return false;
        parsed = parsed * 10u + (unsigned)(*p - '0');
        if (parsed > maximum) return false;
    }
    if (!parsed) return false;
    *out = parsed;
    return true;
}
/* Read actual cumulative CPU payload submissions, never resident estimates. */
static bool observe_payload(BenchStreamRoute *s, unsigned frame, const char *phase)
{
    JceSubmittedPayloadStats stats;
    if (!jce_renderer_get_submitted_payload_stats(s->device, &stats)) return false;
    LOG_INFO("benchmark_stream", "benchmark-stream-payload: frame=%u step=%u phase=%s valid=%u generation=%llu renderer_frame=%u vertex=%llu index=%llu texture_create=%llu texture_update=%llu saturated=%u",
        frame, s->step, phase, stats.valid ? 1u : 0u,
        (unsigned long long)stats.context_generation, stats.frame_index,
        (unsigned long long)stats.static_vertex_bytes,
        (unsigned long long)stats.static_index_bytes,
        (unsigned long long)stats.texture_create_bytes,
        (unsigned long long)stats.texture_update_bytes,
        stats.saturated ? 1u : 0u);
    return !stats.saturated;
}
bool bench_stream_route_init(BenchStreamRoute *s, JceScene *scene, JceSceneRenderer *renderer, JceRenderer *device)
{
    const char *root = getenv("JCE_BENCH_STREAM_ROOT");
    if (!root) return true;
    const char *limit = getenv("JCE_BENCH_STREAM_STEPS");
    if (!root[0] || !limit || strlen(limit) != 1 || limit[0] < '1' || limit[0] > '5') return false;
    if (!topology_env("JCE_BENCH_STREAM_VERTICES", 3, 263169, &s->expected_vertices) ||
        !topology_env("JCE_BENCH_STREAM_PRIMITIVES", 1, 1, &s->expected_primitives)) return false;
    s->active = true;
    s->steps = (unsigned)(limit[0] - '0');
    s->scene = scene;
    s->renderer = renderer;
    s->device = device;
    s->step = 0;
    if (!observe_payload(s, 0, "begin")) return false;
    s->started_ms = jce_time_ticks_ms();
    s->fs = jce_fs_create();
    if (!s->fs || !jce_fs_mount_dir(s->fs, "", root)) return false;
    JceWorldStreamConfig cfg = jce_world_stream_config_default();
    cfg.load_radius = 20;
    cfg.unload_radius = 30;
    cfg.max_pending = 1;
    cfg.single_thread = true;
    cfg.budget_mb = 0; /* Model budget is measured separately, not estimated entities. */
    cfg.frame_budget_ms = 2;
    s->world = jce_world_streamer_create(&cfg, scene, s->fs, NULL);
    if (!s->world) return false;
    for (unsigned i = 0; i < 3; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "cell_%u.scene.json", i);
        jce_world_streamer_register_chunk(s->world, i, jce_v3(100.f * i, 0, 0), 1, path);
    }
    jce_world_streamer_set_entity_callbacks(s->world, spawned, despawned, s);
    jce_world_streamer_set_residency_query(s->world, residency, s);
    LOG_INFO("benchmark_stream", "benchmark-stream-route: init steps=%u load=20 unload=30 cooperative=1 budget=measured-first-model scope=tracked-model-bytes", s->steps);
    return true;
}
bool bench_stream_route_update(BenchStreamRoute *s, JceCamera *camera, unsigned frame)
{
    if (!s->active) return true;
    if (jce_time_ticks_ms() - s->started_ms > 60000) return false;
    unsigned step = frame ? (frame - 1) / 200 : 0;
    if (step >= s->steps) step = s->steps - 1;
    s->step = step;
    if (step > 0 && frame == step * 200 + 1 && !observe_payload(s, frame, "begin")) return false;
    float x = 100.f * route[step];
    jce_camera_set_position(camera, jce_v3(x, 0, 12));
    jce_world_streamer_update(s->world, jce_v3(x, 0, 0));
    return true;
}
bool bench_stream_route_observe(BenchStreamRoute *s, unsigned frame)
{
    if (!s->active) return true;
    char path[64];
    snprintf(path, sizeof(path), "set_%u.glb", route[s->step]);
    uint64_t bytes = jce_scene_renderer_model_path_vram_bytes(s->renderer, path);
    if (!s->budget && bytes) {
        s->budget = bytes;
        jce_scene_renderer_set_model_vram_budget(s->renderer, bytes);
        LOG_INFO("benchmark_stream", "benchmark-stream-route: budget=%llu source=public-resolved-model", (unsigned long long)bytes);
    }
    if (frame == s->step * 200 + 199)
        return observe_payload(s, frame, "frozen");
    if (frame != s->step * 200 + 190) return true;
    if (!observe_payload(s, frame, "loaded")) return false;
    JceModel *model = jce_scene_renderer_get_model(s->renderer, path);
    uint32_t vertices = 0, primitives = 0;
    if (model) {
        for (uint32_t n = 0; n < jce_model_node_count(model); ++n) {
            for (uint32_t p = 0; p < jce_model_node_prim_count(model, n); ++p) {
                vertices += jce_model_prim_vertex_count(model, n, p);
                ++primitives;
            }
        }
    }
    LOG_INFO("benchmark_stream", "benchmark-stream-route-ready: frame=%u step=%u vertices=%u primitives=%u", frame, s->step, vertices, primitives);
    uint64_t total = jce_scene_renderer_model_vram_bytes(s->renderer);
    uint32_t evicted = jce_scene_renderer_model_evicted_count(s->renderer);
    uint32_t pending = jce_world_streamer_pending_count(s->world);
    uint32_t loaded = jce_world_streamer_loaded_count(s->world);
    uint32_t entities = jce_world_streamer_entity_count(s->world);
    LOG_INFO("benchmark_stream", "benchmark-stream-route-point: frame=%u step=%u set=%u spawned=%llu despawned=%llu loaded=%u pending=%u entities=%u path_bytes=%llu total_bytes=%llu budget=%llu model_evicted=%u chunk_evicted=%u world_accounted=%llu",
        frame, s->step, route[s->step], (unsigned long long)s->spawned,
        (unsigned long long)s->despawned, loaded, pending, entities,
        (unsigned long long)bytes, (unsigned long long)total, (unsigned long long)s->budget,
        evicted, jce_world_streamer_evicted_count(s->world),
        (unsigned long long)jce_world_streamer_memory_used(s->world));
    return model && vertices == s->expected_vertices && primitives == s->expected_primitives && bytes && s->budget && total == bytes && total <= s->budget && loaded == 1 && !pending &&
        entities == 1 && s->spawned == s->step + 1 && s->despawned == s->step &&
        evicted >= s->step;
}
