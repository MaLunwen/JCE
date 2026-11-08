/* Test policy around the stock SDK lifecycle; no private host state access. */
#include <jce/application/jce_default_main.inc.h>
#include "default_rt.h"
#include "metrics.h"
#include <jce/api_graphics.h>
#include <string.h>

static JcePakArchive *postfx_asset_pak;
static JcePakArchive *postfx_base_pak;
static unsigned postfx_frame;
static unsigned postfx_raw_frame;
static uint64_t postfx_wait_started;
static bool postfx_ready;
/* Value packet, with engine-owned members borrowed only until postfx_exit.
 * Never retain the callback's services-packet address across callbacks. */
static JceServices postfx_metrics_services;
static bool postfx_metrics_live;
static uint64_t postfx_next_metrics_ms;
static const char *postfx_after;
static unsigned postfx_switch_frame;
static bool postfx_apply(const char *effect);

typedef struct {
    unsigned entities, meshes, cameras, lights;
    bool valid;
} PostfxWorldWitness;

static void postfx_world_entity(JceScene *scene, JceEntity entity, void *user)
{
    PostfxWorldWitness *w = user;
    const char *name = jce_scene_entity_name(scene, entity);
    JceTransform *transform = jce_scene_get_transform(scene, entity);
    JceMeshRenderer *mesh = jce_scene_get_mesh_renderer(scene, entity);
    JceCameraComponent *camera = jce_scene_get_camera(scene, entity);
    ++w->entities;
    if (!name || !transform) { w->valid = false; return; }
    if (mesh) {
        ++w->meshes;
        if (strcmp(name, "StaticModel") || !mesh->mesh_path ||
            strcmp(mesh->mesh_path, getenv("JCE_BENCH_POST_MODEL"))) w->valid = false;
        LOG_INFO("postfx_benchmark", "stock-mesh: scale=%.6f,%.6f,%.6f path=%s",
                 (double)transform->scale.x, (double)transform->scale.y,
                 (double)transform->scale.z, mesh->mesh_path ? mesh->mesh_path : "");
    } else if (camera) {
        ++w->cameras;
        if (strcmp(name, "Camera") || !camera->is_primary || camera->ortho) w->valid = false;
        LOG_INFO("postfx_benchmark", "stock-authored-camera: position=%.6f,%.6f,%.6f fov=%.6f near=%.6f far=%.6f",
                 (double)transform->position.x, (double)transform->position.y,
                 (double)transform->position.z, (double)camera->fov_deg,
                 (double)camera->near_plane, (double)camera->far_plane);
    } else if (!strcmp(name, "Sun") && jce_scene_get_dir_light(scene, entity)) {
        ++w->lights;
    } else w->valid = false;
}

static bool postfx_world_witness(void)
{
    JceScene *scene = jce_default_main_get_scene();
    const JceCamera *camera = jce_default_main_get_camera();
    PostfxWorldWitness w = {0, 0, 0, 0, true};
    if (!scene || !camera) return false;
    jce_scene_each_entity(scene, postfx_world_entity, &w);
    jce_vec3 position = jce_camera_get_position(camera);
    jce_vec3 forward = jce_camera_get_forward(camera);
    LOG_INFO("postfx_benchmark", "stock-world: entities=%u meshes=%u cameras=%u lights=%u valid=%u",
             w.entities, w.meshes, w.cameras, w.lights, (unsigned)w.valid);
    LOG_INFO("postfx_benchmark", "stock-camera: position=%.6f,%.6f,%.6f forward=%.6f,%.6f,%.6f fov_deg=%.6f near=%.6f far=%.6f",
             (double)position.x, (double)position.y, (double)position.z,
             (double)forward.x, (double)forward.y, (double)forward.z,
             (double)jce_camera_get_fov(camera), (double)jce_camera_get_near(camera),
             (double)jce_camera_get_far(camera));
    return w.valid && w.entities == 3 && w.meshes == 1 && w.cameras == 1 && w.lights == 1;
}

/* Optional fixed-environment readiness witness, before timed samples. */
static bool postfx_model_witness(void)
{
    const char *path = getenv("JCE_BENCH_POST_MODEL");
    const JceSceneRenderer *renderer = jce_default_main_get_scene_renderer();
    JceModel *model;
    uint64_t vertices = 0, primitives = 0;
    if (!path || !path[0]) return true;
    model = jce_scene_renderer_get_model((JceSceneRenderer *)renderer, path);
    if (!model) return false;
    for (uint32_t node = 0; node < jce_model_node_count(model); ++node) {
        uint32_t count = jce_model_node_prim_count(model, node);
        primitives += count;
        for (uint32_t prim = 0; prim < count; ++prim)
            vertices += jce_model_prim_vertex_count(model, node, prim);
    }
    LOG_INFO("postfx_benchmark",
             "stock-model: frame=120 vertices=%llu primitives=%llu",
             (unsigned long long)vertices, (unsigned long long)primitives);
    return vertices != 0 && primitives != 0 && postfx_world_witness();
}

static void postfx_draw(const JceServices *svc, void *user)
{
    JceGpuStats stats;
    if (postfx_after && postfx_frame == postfx_switch_frame &&
        !postfx_apply(postfx_after)) {
        LOG_ERROR("postfx_benchmark", "%s", "effect transition failed");
        jce_engine_request_quit();
        return;
    }
    if (postfx_after && postfx_frame == postfx_switch_frame)
        LOG_INFO("postfx_benchmark", "stock-transition: frame=%u effect=%s",
                 postfx_frame, postfx_after);
    app_draw(svc, user);
    ++postfx_raw_frame;
    if(postfx_metrics_live) {
        uint64_t now=jce_time_ticks_ms();
        if(now>=postfx_next_metrics_ms) {
            bench_metrics_sample_draw(svc,jce_default_main_get_scene(),
                (JceSceneRenderer *)jce_default_main_get_scene_renderer(),postfx_raw_frame,"default-periodic");
            postfx_next_metrics_ms=now+5000u;
        }
    }
    const char *model_path = getenv("JCE_BENCH_POST_MODEL");
    if (model_path && model_path[0] && !postfx_ready) {
        JceSceneRenderer *renderer = (JceSceneRenderer *)jce_default_main_get_scene_renderer();
        if (!jce_scene_renderer_get_model(renderer, model_path)) {
            if (jce_time_ticks_ms() - postfx_wait_started >= 60000u) {
                LOG_ERROR("postfx_benchmark", "%s", "fixed environment readiness timed out");
                jce_engine_request_quit();
            }
            return;
        }
        postfx_ready = true;
        LOG_INFO("postfx_benchmark", "stock-ready: raw_frame=%u elapsed_ms=%llu",
                 postfx_raw_frame, (unsigned long long)(jce_time_ticks_ms() - postfx_wait_started));
    }
    ++postfx_frame;
    if (model_path && model_path[0] && postfx_frame >= 721u)
        jce_engine_request_quit();
    bench_default_rt_tick(jce_default_main_get_scene_renderer(), postfx_frame);
    if (postfx_frame == 120 && !postfx_model_witness()) {
        LOG_ERROR("postfx_benchmark", "%s", "fixed environment was not ready before timing");
        jce_engine_request_quit();
        return;
    }
    if ((postfx_frame == 60 || postfx_frame == 120) &&
        jce_renderer_get_gpu_stats(&stats)) {
        LOG_INFO("postfx_benchmark", "stock-stats: frame=%u draws=%u views=%u",
                 postfx_frame, stats.num_draw, (unsigned)stats.num_views);
        for (uint16_t i = 0; i < stats.num_views; ++i)
            LOG_INFO("postfx_benchmark", "stock-view: frame=%u id=%u name=%s",
                     postfx_frame, (unsigned)stats.views[i].view_id,
                     stats.views[i].name);
    }
}

static bool postfx_apply(const char *effect)
{
    JceRenderPipelineDesc pipeline;
    JceCvar *taa;
    if (!effect || (strcmp(effect, "base") && strcmp(effect, "taa") &&
                    strcmp(effect, "ssao") && strcmp(effect, "ssr") &&
                    strcmp(effect, "bloom") && strcmp(effect, "fog"))) {
        LOG_ERROR("postfx_benchmark", "%s", "invalid explicit effect input");
        return false;
    }
    jce_render_pipeline_preset_high(&pipeline);
    pipeline.enable_taa = !strcmp(effect, "taa");
    pipeline.enable_ssao = !strcmp(effect, "ssao");
    pipeline.enable_ssr = !strcmp(effect, "ssr");
    pipeline.enable_bloom = !strcmp(effect, "bloom");
    pipeline.enable_volumetric_fog = !strcmp(effect, "fog");
    pipeline.enable_motion_blur = false;
    pipeline.enable_gpu_particles = false;
    pipeline.enable_cloth = false;
    pipeline.enable_stylized_sky = false;
    pipeline.enable_ssgi = false;
    pipeline.msaa_samples = 1;
    pipeline.render_scale = 1.0f;
    for (int i = 0; i < JCE_RP_PERF_COUNT; ++i) pipeline.perf[i] = 0;
    jce_render_pipeline_apply(&pipeline);
    taa = jce_cvar_find("r.taa");
    if (!taa) {
        LOG_ERROR("postfx_benchmark", "%s", "TAA authority was not registered");
        return false;
    }
    jce_cvar_set_bool(taa, pipeline.enable_taa);
    LOG_INFO("postfx_benchmark",
             "stock-host: effect=%s taa=%u ssao=%u ssr=%u bloom=%u fog=%u",
             effect, (unsigned)jce_cvar_get_bool(taa),
             (unsigned)jce_render_pipeline_is_feature_enabled("ssao"),
             (unsigned)jce_render_pipeline_is_feature_enabled("ssr"),
             (unsigned)jce_render_pipeline_is_feature_enabled("bloom"),
             (unsigned)jce_render_pipeline_is_feature_enabled("volumetric_fog"));
    return true;
}

static bool postfx_init(const JceServices *svc, void *user)
{
    postfx_metrics_live=false;
    memset(&postfx_metrics_services,0,sizeof(postfx_metrics_services));
    if(!svc)return false;
    postfx_wait_started = jce_time_ticks_ms();
    const char *switch_at = getenv("JCE_BENCH_POST_SWITCH_FRAME");
    postfx_after = getenv("JCE_BENCH_POST_EFFECT_AFTER");
    if (postfx_after || switch_at) {
        char *end = NULL;
        unsigned long frame = switch_at ? strtoul(switch_at, &end, 10) : 0;
        if (!postfx_after || !postfx_after[0] || !switch_at ||
            !end || *end || frame == 0 || frame > 10000) return false;
        postfx_switch_frame = (unsigned)frame;
    }
    const char *asset_pak = getenv("JCE_BENCH_ASSET_PAK");
    if (asset_pak && asset_pak[0]) {
        postfx_base_pak = svc->pak;
        postfx_asset_pak = jce_pak_open_file(asset_pak);
        if (!postfx_base_pak || !postfx_asset_pak) {
            if (postfx_asset_pak) jce_pak_close(postfx_asset_pak);
            postfx_asset_pak = NULL;
            postfx_base_pak = NULL;
            LOG_ERROR("postfx_benchmark", "%s", "fixed environment PAK unavailable");
            return false;
        }
        jce_pak_overlay_push(postfx_base_pak, postfx_asset_pak);
    }
    const bool ready=app_init(svc,user) && postfx_apply(getenv("JCE_BENCH_POST_EFFECT")) &&
        bench_default_rt_init(svc);
    if(ready) {
        postfx_metrics_services=*svc;postfx_metrics_live=true;
        postfx_next_metrics_ms=jce_time_ticks_ms()+5000u;
        bench_metrics_sample_draw(svc,jce_default_main_get_scene(),
            (JceSceneRenderer *)jce_default_main_get_scene_renderer(),0,"default-initialized");
    }
    return ready;
}

static void postfx_exit(void *user)
{
    bench_default_rt_exit();
    if(postfx_metrics_live) {
        /* Final read is before the stock host retires scene/renderer/window. */
        bench_metrics_sample_draw(&postfx_metrics_services,jce_default_main_get_scene(),
            (JceSceneRenderer *)jce_default_main_get_scene_renderer(),postfx_raw_frame,"default-exit");
    }
    postfx_metrics_live=false;
    memset(&postfx_metrics_services,0,sizeof(postfx_metrics_services));
    app_exit(user);
    if (postfx_asset_pak) {
        jce_pak_overlay_remove(postfx_base_pak, postfx_asset_pak);
        jce_pak_close(postfx_asset_pak);
        postfx_asset_pak = NULL;
        postfx_base_pak = NULL;
    }
}

static JceAppDesc postfx_desc(void)
{
    const char *config = getenv("JCE_BENCH_CONFIG_FILE");
    JceAppDesc desc = {0};
    if (config && config[0]) jce_engine_set_config_path(config);
    desc.name = "JCE SDK Default Host Benchmark";
    desc.init = postfx_init;
    desc.update = app_update;
    desc.draw = postfx_draw;
    desc.exit = postfx_exit;
    desc.window_width = 1280;
    desc.window_height = 720;
    return desc;
}

JCE_MAIN(postfx_desc)
