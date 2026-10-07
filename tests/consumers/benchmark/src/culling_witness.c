/* Controlled front/offscreen/behind fixture policy; no private renderer access. */
#include "culling_witness.h"
#include <jce/api_core.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_culling"

static void position_entity(JceScene *scene, JceEntity entity, void *user)
{
    BenchCulling *state = user;
    if (jce_scene_get_parent(scene, entity) != state->root ||
        !jce_scene_get_mesh_renderer(scene, entity)) return;
    uint32_t index = state->groups[0] + state->groups[1] + state->groups[2];
    uint32_t group = index % 3u;
    uint32_t side = (uint32_t)ceil(sqrt((state->count + 2u) / 3u));
    uint32_t slot = index / 3u;
    float x = side > 1 ? -4.0f + 8.0f * (slot % side) / (side - 1u) : 0;
    float y = side > 1 ? -2.0f + 4.0f * (slot / side) / (side - 1u) : 0;
    float scale = fminf(0.1f, 4.0f / side);
    JceTransform transform = {0};
    transform.position = jce_v3(group == 1 ? x + 1000.0f : x, y,
                                group == 2 ? 1000.0f : -5.0f);
    transform.rotation = jce_q_identity();
    transform.scale = jce_v3(scale, scale, scale);
    jce_scene_set_transform(scene, entity, &transform);
    if (state->model_path) {
        JceMeshRenderer mesh = *jce_scene_get_mesh_renderer(scene, entity);
        mesh.mesh_path = jce_scene_intern(scene, state->model_path);
        /* The primitive fixture has no UVs. Imported geometry requires neutral
         * tiling; zero tiling is a real per-renderer material divergence. */
        mesh.uv_tiling[0] = mesh.uv_tiling[1] = 1.0f;
        jce_scene_set_mesh_renderer(scene, entity, &mesh);
    }
    ++state->groups[group];
}

bool bench_culling_initialize(JceScene *scene, const JceSceneStressDesc *desc,
                              const JceSceneStressResult *result, BenchCulling *state)
{
    const char *mode = getenv("JCE_BENCH_CULLING");
    if (!mode) return true;
    if (!strcmp(mode, "on")) state->enabled = true;
    else if (!strcmp(mode, "off")) state->enabled = false;
    else return false;
    if (!scene || !desc || !result || desc->kind != JCE_SCENE_STRESS_ENTITY ||
        desc->count < 3 || result->spawned != desc->count) return false;
    state->requested = true;
    state->root = result->root;
    state->count = result->spawned;
    state->model_path = getenv("JCE_BENCH_CULLING_MODEL");
    if (state->model_path && !state->model_path[0]) return false;
    jce_scene_stress_motion(scene, result->root, false);
    jce_scene_each_entity(scene, position_entity, state);
    if (state->groups[0] + state->groups[1] + state->groups[2] != state->count) return false;
    LOG_INFO(LOG_TAG, "benchmark-culling: initialized count=%u front=%u offscreen=%u behind=%u enabled=%u",
        state->count, state->groups[0], state->groups[1], state->groups[2], state->enabled);
    return true;
}

void bench_culling_configure(const BenchCulling *state, JceSceneRenderConfig *config)
{
    if (!state->requested) return;
    config->frustum_culling = state->enabled;
    config->draw_shadows = false;
}

bool bench_culling_observe(const BenchCulling *state, JceSceneRenderer *renderer,
                           unsigned frame)
{
    if (!state->requested || (frame != 20 && frame != 50)) return true;
    JceSceneCullStats stats;
    jce_scene_renderer_get_cull_stats(renderer, &stats);
    uint32_t expected_visible = (state->enabled ? state->groups[0] : state->count) + 3u;
    LOG_INFO(LOG_TAG, "benchmark-culling: frame=%u enabled=%u candidates=%u visible=%u culled=%u expected_visible=%u",
        frame, stats.enabled, stats.total, stats.visible, stats.culled, expected_visible);
    /* Camera, directional light and the stress root are non-rendering records.
     * All three remain conservative visible candidates in this exact fixture. */
    return stats.enabled == state->enabled && stats.total == state->count + 3u &&
        stats.visible == expected_visible && stats.culled == stats.total - expected_visible;
}
