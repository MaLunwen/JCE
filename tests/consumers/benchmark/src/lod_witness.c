/* Analytically equivalent planes isolate distance selection and submissions. */
#include "lod_witness.h"
#include <jce/api_core.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

static JceMesh *load_mesh(const char *path, void *user)
{
    BenchLod *state = user;
    return path && !strcmp(path, "benchmark_lod_plane") ? state->meshes[0] : NULL;
}

bool bench_lod_create(BenchLod *state, JceSceneRendererCallbacks *callbacks)
{
    const char *mode = getenv("JCE_BENCH_LOD");
    if (!mode) return true;
    if (!strcmp(mode, "on")) state->enabled = true;
    else if (strcmp(mode, "off")) return false;
    state->requested = true;
    const uint32_t subdivisions[3] = {32, 8, 1};
    for (unsigned i = 0; i < 3; ++i) {
        state->meshes[i] = jce_mesh_create_plane(3.5f, 3.5f, subdivisions[i]);
        if (!state->meshes[i] || jce_mesh_index_count(state->meshes[i]) !=
                6u * subdivisions[i] * subdivisions[i]) return false;
    }
    jce_lod_setup(&state->group, state->meshes[0], 45.0f,
                  state->meshes[1], 75.0f, state->meshes[2], FLT_MAX);
    state->group.hysteresis = 0.05f;
    jce_lod_set_global_bias(1.0f);
    callbacks->load_mesh = load_mesh;
    callbacks->userdata = state;
    LOG_INFO("benchmark_lod", "benchmark-lod: initialized enabled=%u triangles=2048,128,2",
             (unsigned)state->enabled);
    return true;
}

void bench_lod_bind(BenchLod *state, JceSceneRenderer *renderer)
{
    if (state->requested)
        jce_scene_renderer_set_global_lod(renderer, state->enabled ? &state->group : NULL);
}

void bench_lod_destroy(BenchLod *state)
{
    for (unsigned i = 0; i < 3; ++i) {
        if (state->meshes[i]) jce_mesh_destroy(state->meshes[i]);
    }
    memset(state, 0, sizeof(*state));
}

bool bench_lod_update(const BenchLod *state, JceCamera *camera, unsigned frame)
{
    if (!state->requested) return true;
    float z = 20.0f;
    if (frame > 20 && frame <= 80) z += (frame - 20u) * (100.0f / 60.0f);
    else if (frame > 80 && frame <= 100) z = 120.0f;
    else if (frame > 100 && frame <= 160) z = 120.0f - (frame - 100u) * (100.0f / 60.0f);
    jce_camera_set_position(camera, jce_v3(0, 20, z));
    jce_camera_look_at(camera, jce_v3(0, 0, 0));
    return true;
}

void bench_lod_configure(const BenchLod *state, JceSceneRenderConfig *config)
{
    if (!state->requested) return;
    config->draw_shadows = false;
    config->draw_skybox = false;
    config->frustum_culling = false;
    config->occlusion_culler = NULL;
}

bool bench_lod_observe(const BenchLod *state, JceSceneRenderer *renderer,
                       JceCamera *camera, unsigned frame)
{
    if (!state->requested) return true;
    JceSceneLodStats stats;
    jce_scene_renderer_get_lod_stats(renderer, &stats);
    jce_vec3 position = jce_camera_get_position(camera);
    LOG_INFO("benchmark_lod",
        "benchmark-lod: frame=%u enabled=%u levels=%u picks=%u,%u,%u culled=%u pos=%.6f,%.6f,%.6f",
        frame, (unsigned)stats.enabled, stats.level_count,
        stats.picks[0], stats.picks[1], stats.picks[2], stats.culled,
        position.x, position.y, position.z);
    return stats.enabled == state->enabled && stats.culled == 0 &&
        (!state->enabled || (stats.level_count == 3 &&
         stats.picks[0] + stats.picks[1] + stats.picks[2] == 64));
}
