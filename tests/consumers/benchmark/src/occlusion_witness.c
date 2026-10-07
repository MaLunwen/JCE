/* Known hidden/visible groups and reveal route; all rendering is SDK-owned. */
#include "occlusion_witness.h"
#include "mutations.h"
#include <jce/api_core.h>
#include <jce/api_graphics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_occlusion"

static void position_entity(JceScene *scene, JceEntity entity, void *user)
{
    BenchOcclusion *state = user;
    if (jce_scene_get_parent(scene, entity) != state->root ||
        !jce_scene_get_mesh_renderer(scene, entity)) return;
    uint32_t index = state->positioned++;
    uint32_t side = (uint32_t)ceil(sqrt((state->count + 1u) / 2u));
    uint32_t slot = index / 2u;
    float x = side > 1 ? -0.9f + 1.8f * (slot % side) / (side - 1u) : 0;
    float y = side > 1 ? -1.8f + 3.6f * (slot / side) / (side - 1u) : 0;
    float scale = fminf(0.2f, 1.2f / side);
    JceTransform transform = {0};
    transform.position = jce_v3(x + (index % 2u ? 2.5f : -2.5f), y, -5.0f);
    transform.rotation = jce_q_identity();
    transform.scale = jce_v3(scale, scale, scale);
    jce_scene_set_transform(scene, entity, &transform);
    if (state->model_path) {
        JceMeshRenderer mesh = *jce_scene_get_mesh_renderer(scene, entity);
        mesh.mesh_path = jce_scene_intern(scene, state->model_path);
        mesh.uv_tiling[0] = mesh.uv_tiling[1] = 1.0f;
        jce_scene_set_mesh_renderer(scene, entity, &mesh);
    }
}

bool bench_occlusion_initialize(JceScene *scene, const JceSceneStressDesc *desc,
    const JceSceneStressResult *result, const JceServices *services, BenchOcclusion *state)
{
    const char *mode = getenv("JCE_BENCH_OCCLUSION");
    if (!mode) return true;
    if (!strcmp(mode, "on")) state->enabled = true;
    else if (strcmp(mode, "off")) return false;
    if (!scene || !desc || !result || !services || desc->kind != JCE_SCENE_STRESS_ENTITY ||
        desc->count < 2 || result->spawned != desc->count || getenv("JCE_BENCH_CULLING")) return false;
    state->requested = true;
    state->reveal = getenv("JCE_BENCH_OCCLUSION_REVEAL") != NULL;
    state->root = result->root;
    state->count = result->spawned;
    state->model_path = getenv("JCE_BENCH_OCCLUSION_MODEL");
    if (state->model_path && !state->model_path[0]) return false;
    jce_scene_stress_motion(scene, result->root, false);
    jce_scene_each_entity(scene, position_entity, state);
    if (state->positioned != state->count) return false;
    state->wall = jce_scene_create_entity(scene, "OcclusionWall");
    if (state->wall == JCE_ENTITY_INVALID) return false;
    JceTransform transform = {0};
    transform.position = jce_v3(-1.67f, 0, 0);
    transform.rotation = jce_q_identity();
    transform.scale = jce_v3(2.8f, 3.2f, 0.3f);
    jce_scene_set_transform(scene, state->wall, &transform);
    JceMeshRenderer mesh;
    jce_mesh_renderer_init(&mesh);
    mesh.visible = true;
    mesh.base_color[0] = 0.15f;
    mesh.base_color[1] = 0.3f;
    mesh.base_color[2] = 0.6f;
    mesh.base_color[3] = mesh.normal_scale = mesh.ao_strength = 1;
    mesh.roughness = 0.6f;
    jce_scene_set_mesh_renderer(scene, state->wall, &mesh);
    if (state->enabled) {
        JceOcclusionConfig config = jce_occlusion_config_default();
        config.max_entities = state->count + 1u;
        JceShaderSet shaders = {0};
        shaders.color.idx = jce_renderer_get_program_color(services->renderer).idx;
        if (shaders.color.idx == UINT16_MAX) return false;
        state->culler = jce_occlusion_culler_create(&config, &shaders);
        if (!state->culler || !jce_occlusion_culler_is_active(state->culler)) {
            LOG_WARN(LOG_TAG, "benchmark-occlusion: UNAVAILABLE hardware queries");
            return false;
        }
    }
    LOG_INFO(LOG_TAG, "benchmark-occlusion: initialized count=%u hidden=%u visible=%u enabled=%u reveal=%u",
        state->count, (state->count + 1u) / 2u, state->count / 2u, state->enabled, state->reveal);
    return true;
}

bool bench_occlusion_update(JceScene *scene, BenchOcclusion *state, unsigned frame)
{
    if (!state->requested) return true;
    const char *motion = getenv("JCE_BENCH_OCCLUSION_MOTION");
    if (motion) {
        if (strcmp(motion, "oscillate") || state->reveal) return false;
        JceTransform *current = jce_scene_get_transform(scene, state->wall);
        if (!current) return false;
        float offset = 1.8f * bench_motion_wave(frame);
        if (frame >= 51 && frame <= 81) {
            JceTransform transform = *current;
            transform.position.x = -1.67f + offset;
            jce_scene_set_transform(scene, state->wall, &transform);
        }
        if ((frame >= 50 && frame <= 82) || frame == 20 || frame == 90) {
            current = jce_scene_get_transform(scene, state->wall);
            LOG_INFO(LOG_TAG,
                "benchmark-occluder: frame=%u offset=%.7f pos=%.7f,%.7f,%.7f",
                frame, offset, current->position.x, current->position.y, current->position.z);
        }
        return true;
    }
    if (!state->reveal || frame != 80) return true;
    JceTransform *current = jce_scene_get_transform(scene, state->wall);
    if (!current) return false;
    JceTransform transform = *current;
    transform.position.x = 1000;
    jce_scene_set_transform(scene, state->wall, &transform);
    LOG_INFO(LOG_TAG, "benchmark-occlusion: reveal frame=%u", frame);
    return true;
}

void bench_occlusion_configure(const BenchOcclusion *state, JceSceneRenderConfig *config)
{
    if (!state->requested) return;
    config->occlusion_culler = state->culler;
    config->draw_shadows = false;
}

bool bench_occlusion_observe(const BenchOcclusion *state, unsigned frame)
{
    if (!state->requested || (frame != 20 && frame != 50 &&
        !(state->reveal && frame >= 79 && frame <= 90))) return true;
    JceOcclusionStats stats = jce_occlusion_culler_get_stats(state->culler);
    LOG_INFO(LOG_TAG, "benchmark-occlusion: frame=%u enabled=%u total=%u visible=%u occluded=%u warm=%u pending=%u unqueried=%u",
        frame, state->enabled, stats.total_entities, stats.visible, stats.occluded,
        stats.warm_up, stats.no_result, stats.unqueried);
    if (!state->enabled) return stats.total_entities == 0;
    /* The camera, light and root conservatively participate as non-rendering
     * records, matching the public frustum fixture's three extra candidates. */
    uint32_t expected = state->count + 3u + (state->reveal && frame >= 80 ? 0u : 1u);
    return stats.total_entities == expected && stats.total_entities == stats.visible +
        stats.occluded + stats.warm_up + stats.no_result + stats.unqueried;
}

void bench_occlusion_destroy(BenchOcclusion *state)
{
    jce_occlusion_culler_destroy(state->culler);
    memset(state, 0, sizeof(*state));
}
