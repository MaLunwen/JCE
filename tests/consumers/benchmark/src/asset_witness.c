/* Consumer-owned imported-model witness; all state remains engine-owned. */
#include "asset_witness.h"
#include <jce/api_app.h>
#include <jce/api_core.h>
#include <jce/api_graphics.h>
#include <jce/api_animation.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOG_TAG "benchmark_asset"

typedef struct {
    const char *path;
    uint32_t meshes;
    uint32_t animators;
    uint32_t advanced;
    uint32_t layers;
    uint32_t layer_sum;
    float clock_min;
    float clock_max;
    bool invalid_clock;
} Population;

bool bench_asset_readiness_initialize(BenchAssetReadiness *state)
{
    const char *mode = getenv("JCE_BENCH_WAIT_ASSET");
    if (!mode) return true;
    state->path = getenv("JCE_BENCH_ASSET_PATH");
    if (strcmp(mode, "1") || !state->path || !state->path[0]) return false;
    state->started_ms = jce_time_ticks_ms();
    return true;
}

bool bench_asset_readiness_update(BenchAssetReadiness *state,
                                 JceSceneRenderer *renderer, unsigned frame)
{
    if (!state->path) return true;
    if (!state->ready) {
        uint64_t elapsed_ms = jce_time_ticks_ms() - state->started_ms;
        if (renderer && jce_scene_renderer_get_model(renderer, state->path)) {
            state->ready = true;
            LOG_INFO(LOG_TAG, "benchmark-asset: ready elapsed_ms=%llu frame=%u",
                     (unsigned long long)elapsed_ms, frame);
        } else {
            if (elapsed_ms >= 60000u) {
                LOG_ERROR(LOG_TAG, "benchmark-asset: readiness timed out");
                jce_engine_request_quit();
            }
            return false;
        }
    }
    /* Only this opt-in diagnostic holds the fixture clock during import.
     * Its ordinary async renderer keeps drawing/polling; no loader callback,
     * private cache reset or blocking decode bypass is introduced. */
    if (frame >= 180u) {
        jce_engine_request_quit();
        return false;
    }
    return true;
}

bool bench_asset_capture_configure(void)
{
    if (!getenv("JCE_BENCH_ASSET_STILL")) return true;
    JceRenderPipelineDesc pipeline;
    jce_render_pipeline_get(&pipeline);
    pipeline.enable_taa = false;
    pipeline.enable_ssao = false;
    pipeline.enable_ssr = false;
    pipeline.enable_motion_blur = false;
    jce_render_pipeline_apply(&pipeline);
    /* TAA's startup default is independently registered when the renderer is
     * created. Configure that public console authority before the first frame. */
    JceCvar *taa = jce_cvar_find("r.taa");
    if (!taa) return false;
    jce_cvar_set_bool(taa, false);
    if (jce_cvar_get_bool(taa)) return false;
    const char *indirect = getenv("JCE_BENCH_GPU_INDIRECT");
    if (indirect) {
        if (strcmp(indirect, "0") && strcmp(indirect, "1")) return false;
        JceCvar *mode = jce_cvar_find("r.gpu_driven.indirect");
        if (!mode) return false;
        jce_cvar_set_bool(mode, indirect[0] == '1');
        LOG_INFO(LOG_TAG, "benchmark-asset: indirect policy=%u", jce_cvar_get_bool(mode));
    }
    jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                  "benchmark-asset: still controls taa=0 ssao=0 ssr=0 motion_blur=0");
    return true;
}

static void count_entity(JceScene *scene, JceEntity entity, void *user)
{
    Population *p = user;
    const JceMeshRenderer *mesh = jce_scene_get_mesh_renderer(scene, entity);
    const JceSkeletalAnimatorComponent *anim = jce_scene_get_skeletal_animator(scene, entity);
    if (mesh && !strcmp(mesh->mesh_path, p->path)) ++p->meshes;
    if (anim && !strcmp(anim->skeleton_path, p->path)) {
        ++p->animators;
        uint32_t layer = jce_scene_get_entity_layer(scene, entity);
        p->layers |= 1u << layer;
        p->layer_sum += layer;
        float time = anim->norm_time;
        if (!isfinite(time) || time < 0 || time > 1) p->invalid_clock = true;
        if (time > 0.000001f) ++p->advanced;
        if (time < p->clock_min) p->clock_min = time;
        if (time > p->clock_max) p->clock_max = time;
    }
}

static void start_entity(JceScene *scene, JceEntity entity, void *user)
{
    Population *p = user;
    const JceSkeletalAnimatorComponent *source = jce_scene_get_skeletal_animator(scene, entity);
    if (!source || strcmp(source->skeleton_path, p->path)) return;
    JceSkeletalAnimatorComponent value = *source;
    value.playing = true;
    jce_scene_set_skeletal_animator(scene, entity, &value);
    ++p->animators;
}

bool bench_asset_start(JceScene *scene, JceSceneRenderer *renderer, unsigned frame)
{
    const char *path = getenv("JCE_BENCH_ASSET_PATH");
    if (!path || !path[0] || frame != 10 || !getenv("JCE_BENCH_ASSET_PLAY")) return true;
    /* Fail instead of letting decode completion shift the animation clock
     * between captures. The authored scene starts paused during import. */
    if (!renderer || !jce_scene_renderer_get_model(renderer, path)) return false;
    Population p = {path, 0, 0};
    jce_scene_each_entity(scene, start_entity, &p);
    LOG_INFO(LOG_TAG, "benchmark-asset: start frame=10 animators=%u", p.animators);
    return p.animators > 0;
}

bool bench_asset_witness(JceScene *scene, JceSceneRenderer *renderer, unsigned frame)
{
    const char *path = getenv("JCE_BENCH_ASSET_PATH");
    if (!path || !path[0] || (frame != 20 && frame != 50)) return true;
    JceModel *model = jce_scene_renderer_get_model(renderer, path);
    if (!model) return false;
    Population population = {0};
    population.path = path;
    population.clock_min = 1;
    jce_scene_each_entity(scene, count_entity, &population);
    uint32_t vertices = 0, primitives = 0;
    for (uint32_t n = 0; n < jce_model_node_count(model); ++n) {
        for (uint32_t p = 0; p < jce_model_node_prim_count(model, n); ++p) {
            vertices += jce_model_prim_vertex_count(model, n, p);
            ++primitives;
        }
    }
    JceSkeleton *skeleton = jce_model_get_skeleton(model);
    JceAnimPlayer *player = jce_scene_renderer_get_anim_player(renderer, path);
    LOG_INFO(LOG_TAG,
        "benchmark-asset: frame=%u meshes=%u animators=%u vertices=%u primitives=%u joints=%u clips=%u player=%u time=%.6f gpu_bytes=%llu",
        frame, population.meshes, population.animators, vertices, primitives,
        skeleton ? jce_skeleton_joint_count(skeleton) : 0,
        jce_model_anim_count(model), player != NULL,
        player ? jce_anim_player_get_time(player) : 0.0f,
        (unsigned long long)jce_model_gpu_bytes(model));
    LOG_INFO(LOG_TAG,
        "benchmark-asset-population: frame=%u advanced=%u min=%.6f max=%.6f layers=%u layer_sum=%u",
        frame, population.advanced, population.clock_min, population.clock_max,
        population.layers, population.layer_sum);
    return population.meshes > 0 && vertices > 0 && primitives > 0 && !population.invalid_clock;
}
