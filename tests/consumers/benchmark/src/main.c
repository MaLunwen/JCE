/* SDK-only benchmark host. Scenario and capture policy belong to this project. */
#include <jce/api_app.h>
#include <jce/api_core.h>
#include <jce/api_scene.h>
#include <jce/api_graphics.h>
#include <jce/api_render.h>
#include <jce/api_resource.h>
#include "probes.h"
#include "metrics.h"
#include "mutations.h"
#include "nodraw_witness.h"
#include "physics_pressure_witness.h"
#include "text_witness.h"
#include "asset_witness.h"
#include "culling_witness.h"
#include "occlusion_witness.h"
#include "gpu_survivors.h"
#include "hiz_probe.h"
#include "rt_witness.h"
#include "lod_witness.h"
#include "light_transition.h"
#include "light_timing_witness.h"
#include "shadow_witness.h"
#include "particle_witness.h"
#include "stream_route.h"
#include "load_witness.h"
#include "async_load_witness.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_WIDTH 1280u
#define BENCH_HEIGHT 720u
#define BENCH_VIEW JCE_VIEW_RUNTIME_GAME
#define BENCH_READBACK_VIEW 249u
#define BENCH_COMPOSITE_VIEW 181u /* After post/present 160..180. */
#define BENCH_UI_VIEW 182u
#define LOG_TAG "benchmark_player"

typedef struct Benchmark {
    JceScene *scene;
    JceRuntime *runtime;
    JceCamera *camera;
    JceSceneRenderer *renderer;
    JceOffscreenTarget *target;
    JceUICanvas *ui;
    JcePakArchive *base_pak;
    JcePakArchive *asset_pak;
    JceSceneCameraPose pose;
    BenchCulling culling;
    BenchOcclusion occlusion;
    BenchGpuSurvivors survivors;
    BenchHizProbe hiz_probe;
    BenchLod lod;
    BenchAssetReadiness asset_readiness;
    BenchStreamRoute stream_route;
    BenchLoadWitness load_witness;
    BenchAsyncLoad async_load;
    BenchParticleReadback particle_readback;
    BenchPhysicsPressure physics_pressure;
    unsigned frame;
    uint32_t shot_frame;
    uint32_t play_frame;
    float dt;
    bool shot_submitted;
    bool probe_only;
    uint64_t next_metrics_ms;
    double simulation_seconds;
} Benchmark;

static Benchmark state;

static bool probe_number(const char *key, uint32_t fallback, uint32_t *out)
{
    const char *text = getenv(key);
    if (!text || !text[0]) { *out = fallback; return true; }
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || !value || value > UINT32_MAX) return false;
    *out = (uint32_t)value;
    return true;
}

static bool workload_desc(JceSceneStressDesc *desc, bool *requested)
{
    const char *text = getenv("JCE_BENCH_AUTOSPAWN");
    char *end;
    *requested = text && text[0];
    if (!*requested) return true;
    errno = 0;
    const unsigned long kind = strtoul(text, &end, 10);
    if (errno || end == text || *end != ',' || kind > 4) return false;
    text = end + 1;
    const unsigned long count = strtoul(text, &end, 10);
    if (errno || end == text || *end || count == 0 || count > 2000000) return false;
    desc->kind = (JceSceneStressKind)kind;
    desc->count = (uint32_t)count;
    text = getenv("JCE_BENCH_SPREAD");
    if (text && text[0]) {
        errno = 0;
        desc->spread = strtof(text, &end);
        if (errno || end == text || *end || !isfinite(desc->spread) || desc->spread < 0)
            return false;
    }
    return true;
}

static void benchmark_exit(void *user)
{
    Benchmark *s = user;
    jce_renderer_readback_capture_flush();
    if (s->scene) bench_metrics_sample(s->scene, s->renderer, s->frame,
                                        s->simulation_seconds, "exit");
    bench_particle_readback_destroy(&s->particle_readback);
    bench_async_load_destroy(&s->async_load);
    bench_load_destroy(&s->load_witness);
    bench_stream_route_destroy(&s->stream_route);
    bench_physics_pressure_destroy(&s->physics_pressure);
    if (s->runtime) jce_runtime_destroy(s->runtime);
    if (s->ui) jce_ui_canvas_destroy(s->ui);
    bench_occlusion_destroy(&s->occlusion);
    bench_gpu_survivors_destroy(&s->survivors);
    bench_hiz_probe_destroy(&s->hiz_probe);
    if (s->renderer) jce_scene_renderer_destroy(s->renderer);
    bench_lod_destroy(&s->lod);
    if (s->target) jce_offscreen_target_destroy(s->target);
    if (s->camera) jce_camera_destroy(s->camera);
    if (s->scene) jce_scene_destroy(s->scene);
    if (s->asset_pak) {
        jce_pak_overlay_remove(s->base_pak, s->asset_pak);
        jce_pak_close(s->asset_pak);
    }
    memset(s, 0, sizeof(*s));
}

static bool benchmark_init(const JceServices *svc, void *user)
{
    Benchmark *s = user;
    bool preparing_load = false;
    if (!bench_load_prepare(&preparing_load)) goto failed;
    if (preparing_load) {
        s->probe_only = true;
        jce_engine_request_quit();
        return true;
    }
    const char *probe = getenv("JCE_BENCH_PROBE");
    if (getenv("JCE_BENCH_HIZ_NPOT")) {
        if (!bench_gpu_survivors_create(&s->survivors, svc->renderer) ||
            !s->survivors.queue || !bench_hiz_probe_create(&s->hiz_probe, svc->pak)) goto failed;
        LOG_INFO(LOG_TAG, "benchmark-hiz-npot: initialized 5x5 depth witness");
        return true;
    }
    if (probe && probe[0]) {
        BenchProbeConfig config = {0};
        if (!jce_renderer_is_headless(svc->renderer) ||
            !probe_number("JCE_BENCH_COUNT", 1000, &config.count) ||
            !probe_number("JCE_BENCH_ROUNDS", 10, &config.rounds) ||
            !probe_number("JCE_BENCH_CONCURRENCY", 1, &config.concurrency) ||
            !probe_number("JCE_BENCH_DEPTH", 1, &config.depth) ||
            !bench_probe_run(probe, &config)) goto failed;
        s->probe_only = true;
        LOG_INFO(LOG_TAG, "benchmark-probe: completed name=%s", probe);
        return true;
    }
    const char *path = getenv("JCE_SCENE");
    if (!probe_number("JCE_BENCH_SHOT_FRAME", 20, &s->shot_frame) ||
        !probe_number("JCE_BENCH_PLAY_FRAME", 10, &s->play_frame)) goto failed;
    const char *asset_pak = getenv("JCE_BENCH_ASSET_PAK");
    if (asset_pak && asset_pak[0]) {
        s->base_pak = svc->pak;
        s->asset_pak = jce_pak_open_file(asset_pak);
        if (!s->base_pak || !s->asset_pak) goto failed;
        jce_pak_overlay_push(s->base_pak, s->asset_pak);
    }
    uint64_t size = 0;
    void *bytes = path ? jce_fs_host_read_all(path, &size) : NULL;
    JceJson *json = bytes && size <= SIZE_MAX ? jce_json_parse(bytes, (size_t)size) : NULL;
    s->scene = jce_scene_create();
    const bool loaded = json && s->scene && jce_scene_load_json(s->scene, json) > 0;
    if (json) jce_json_free(json);
    if (bytes) jce_fs_buffer_free(bytes);
    JceSceneStressDesc desc = {0};
    JceSceneStressResult result = {0};
    bool requested = false;
    if (!loaded || !workload_desc(&desc, &requested) || !bench_text_witness_spawn(s->scene)) goto failed;
    if (requested) {
        if (!jce_scene_stress_spawn(s->scene, &desc, &result)) goto failed;
        jce_scene_stress_motion(s->scene, result.root,
                               desc.kind == JCE_SCENE_STRESS_ENTITY);
        LOG_INFO(LOG_TAG, "benchmark-spawn: kind=%u requested=%u spawned=%u",
                 (unsigned)desc.kind, desc.count, result.spawned);
    }
    if (!bench_culling_initialize(s->scene, requested ? &desc : NULL,
                                   requested ? &result : NULL, &s->culling)) goto failed;
    if (!bench_occlusion_initialize(s->scene, requested ? &desc : NULL,
            requested ? &result : NULL, svc, &s->occlusion)) goto failed;
    JceRuntimeDesc rd = {0};
    rd.scene = s->scene;
    rd.pak = svc->pak;
    rd.audio = svc->audio;
    rd.enable_physics = (requested && desc.kind == JCE_SCENE_STRESS_PHYSICS) || bench_physics_pressure_enabled();
    if (bench_physics_pressure_requested()) {
        rd.fixed_timestep = 1.0f / 60.0f;
        rd.solver_iterations = 10;
    }
    s->runtime = jce_runtime_create(&rd);
    if (!s->runtime ||
        !bench_physics_pressure_initialize(&s->physics_pressure, s->scene, s->runtime))
        goto failed;
    JceCameraDesc cd = {0};
    cd.up = jce_v3(0, 1, 0);
    cd.target = jce_v3(0, 0, -1);
    cd.fov_deg = 60;
    cd.near_plane = 0.1f;
    cd.far_plane = 1000;
    s->camera = jce_camera_create(&cd);
    if (!s->camera || jce_scene_camera_apply_primary(s->scene, s->camera, &s->pose)
                      != JCE_SCENE_CAMERA_RESOLVE_OK) goto failed;
    if (!jce_renderer_is_headless(svc->renderer)) {
        JceSceneRendererCallbacks callbacks = {0};
        if (!bench_lod_create(&s->lod, &callbacks)) goto failed;
        s->renderer = jce_scene_renderer_create(svc->renderer, svc->pak,
                                                s->lod.requested ? &callbacks : NULL);
        s->target = jce_offscreen_target_create(svc->renderer, BENCH_VIEW);
        s->ui = jce_ui_canvas_create(svc->renderer, svc->pak);
        if (!s->renderer || !s->target || !s->ui) goto failed;
        bench_lod_bind(&s->lod, s->renderer);
        if (!bench_asset_capture_configure()) goto failed;
        if (!bench_rt_configure()) goto failed;
        jce_postfx_set_view_base(jce_scene_renderer_get_postfx(s->renderer), 160);
    }
    if (!bench_particle_readback_create(&s->particle_readback, svc->renderer)) goto failed;
    if (!bench_stream_route_init(&s->stream_route, s->scene, s->renderer, svc->renderer)) goto failed;
    if (!bench_gpu_survivors_create(&s->survivors, svc->renderer)) goto failed;
    if (!bench_asset_readiness_initialize(&s->asset_readiness)) goto failed;
    LOG_INFO(LOG_TAG, "benchmark-player: initialized viewport=%ux%u", BENCH_WIDTH, BENCH_HEIGHT);
    bench_metrics_sample(s->scene, s->renderer, 0, 0, "initialized");
    s->next_metrics_ms = jce_time_ticks_ms() + 5000u;
    return true;
failed:
    LOG_ERROR(LOG_TAG, "benchmark-player: initialization failed");
    benchmark_exit(s);
    return false;
}

static void benchmark_update(float dt, void *user)
{
    Benchmark *s = user;
    if (s->probe_only) return;
    if (s->hiz_probe.active) { ++s->frame; return; }
    if (!bench_asset_readiness_update(&s->asset_readiness, s->renderer, s->frame)) return;
    if (s->physics_pressure.requested) dt = 1.0f / 60.0f;
    s->dt = dt;
    ++s->frame;
    if (!bench_occlusion_update(s->scene, &s->occlusion, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: occlusion route failed");
    if (!bench_nodraw_update(s->scene, s->frame)) {
        LOG_ERROR(LOG_TAG, "benchmark-player: no-draw transition witness failed");
        jce_engine_request_quit();
        return;
    }
    if (!bench_mutation_update(s->scene, s->frame)) {
        LOG_ERROR(LOG_TAG, "benchmark-player: mutation witness failed");
        return;
    }
    if (!bench_camera_motion_update(s->camera, &s->pose, s->frame)) {
        LOG_ERROR(LOG_TAG, "benchmark-player: camera motion witness failed");
        return;
    }
    if (!bench_lod_update(&s->lod, s->camera, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: LOD camera route failed");
    if (!bench_asset_start(s->scene, s->renderer, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: asset was not ready at the fixed start frame");
    if (s->frame == s->play_frame) LOG_INFO(LOG_TAG, "benchmark-player: entered Play");
    if (s->frame >= s->play_frame) {
        bench_physics_pressure_before(&s->physics_pressure, s->frame);
        jce_runtime_step(s->runtime, dt);
        if (!bench_physics_pressure_after(&s->physics_pressure, s->frame)) {
            LOG_ERROR(LOG_TAG, "benchmark-player: physics pressure observation failed");
            jce_engine_request_quit();
            return;
        }
        s->simulation_seconds += dt;
    }
    if (s->shot_submitted) jce_renderer_readback_capture_poll();
    uint64_t now = jce_time_ticks_ms();
    if (now >= s->next_metrics_ms) {
        bench_metrics_sample(s->scene, s->renderer, s->frame, s->simulation_seconds, "periodic");
        s->next_metrics_ms = now + 5000u;
    }
}

static void benchmark_draw(const JceServices *svc, void *user)
{
    (void)svc;
    Benchmark *s = user;
    if (s->hiz_probe.active) {
        if (!bench_hiz_probe_draw(&s->hiz_probe, &s->survivors, s->frame))
            LOG_ERROR(LOG_TAG, "benchmark-hiz-npot: GPU dispatch/readback failed");
        return;
    }
    if (!s->renderer) return;
    if (!bench_light_transition(s->frame)) return;
    if (!bench_async_load_tick(&s->async_load, svc, s->frame) ||
        !bench_load_witness(&s->load_witness, svc, s->frame) ||
        !bench_stream_route_update(&s->stream_route, s->camera, s->frame)) {
        LOG_ERROR(LOG_TAG, "benchmark-player: asset/streaming input failed");
        return;
    }
    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    cfg.viewport_width = BENCH_WIDTH;
    cfg.viewport_height = BENCH_HEIGHT;
    cfg.camera_culling_mask = s->pose.culling_mask;
    cfg.camera_clear_mode = s->pose.clear_mode;
    cfg.draw_skybox = jce_scene_camera_clear_draws_skybox(s->pose.clear_mode);
    bench_lod_configure(&s->lod, &cfg);
    bench_culling_configure(&s->culling, &cfg);
    bench_occlusion_configure(&s->occlusion, &cfg);
    if (!bench_shadow_configure(&cfg, s->renderer, s->frame)) return;
    jce_mat4 view = jce_camera_view(s->camera);
    jce_mat4 proj = jce_camera_proj(s->camera, (float)BENCH_WIDTH / BENCH_HEIGHT,
                                   jce_gfx_caps().homogeneous_depth);
    jce_mat4 jittered;
    jce_scene_renderer_begin_velocity_frame(s->renderer);
    jce_scene_renderer_taa_begin_frame(s->renderer, BENCH_WIDTH, BENCH_HEIGHT,
                                       &view, &proj, &jittered);
    if (!jce_offscreen_target_prepare(s->target, BENCH_WIDTH, BENCH_HEIGHT,
                                       JCE_M4_PTR(view), JCE_M4_PTR(jittered),
                                       0x222222ff, "Benchmark Player")) {
        LOG_ERROR(LOG_TAG, "benchmark-player: render target preparation failed");
        return;
    }
    cfg.scene_frame_buffer = jce_offscreen_target_get_frame_buffer(s->target);
    if (bench_rt_enabled())
        cfg.ssr_color_tex_handle = jce_offscreen_target_get_color_texture(s->target);
    jce_scene_renderer_render(s->renderer, s->scene, s->camera, BENCH_VIEW, s->dt, &cfg);
    if (bench_rt_enabled())
        jce_scene_renderer_composite_ssr(s->renderer,
            BENCH_VIEW + JCE_VIEW_SR_REFLECTION_COMPOSITE_OFFSET, cfg.scene_frame_buffer);
    if (!bench_lod_observe(&s->lod, s->renderer, s->camera, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: LOD population mismatch");
    if (!bench_light_timing_observe(s->scene, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: light timing population mismatch");
    bench_particle_readback_tick(&s->particle_readback, s->renderer, s->frame);
    bench_gpu_survivors_tick(&s->survivors, s->renderer, s->frame);
    if (!bench_occlusion_observe(&s->occlusion, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: occlusion population accounting failed");
    if (!bench_culling_observe(&s->culling, s->renderer, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: culling population differs from the controlled fixture");
    JcePostFXPipeline *post = jce_scene_renderer_get_postfx(s->renderer);
    jce_postfx_resize(post, BENCH_WIDTH, BENCH_HEIGHT);
    jce_postfx_enable(post, JCE_POSTFX_TONEMAP, jce_offscreen_target_is_hdr(s->target));
    JceTextureHandle color = {jce_offscreen_target_get_color_texture(s->target)};
    JceTextureHandle depth = {jce_offscreen_target_get_depth_texture(s->target)};
    jce_postfx_apply(post, color, depth);
    jce_postfx_present(post, BENCH_WIDTH, BENCH_HEIGHT);
    /* The opt-in text witness uses the editor's public post -> bridge -> UI
     * composition order. Capture the plain bridge with its own row origin,
     * never apply the post texture's readback convention to Canvas glyphs. */
    const bool text_capture = getenv("JCE_BENCH_TEXT_CAPTURE") != NULL;
    if (text_capture) {
        JceTextureHandle output = jce_postfx_get_output(post);
        if (output.idx != color.idx)
            jce_offscreen_target_composite_texture(s->target, BENCH_COMPOSITE_VIEW,
                output.idx, BENCH_WIDTH, BENCH_HEIGHT, jce_renderer_origin_bottom_left());
        jce_ui_canvas_render(s->ui, s->scene, BENCH_UI_VIEW,
            jce_offscreen_target_get_frame_buffer(s->target),
            BENCH_WIDTH, BENCH_HEIGHT, NULL, s->dt);
    }
    jce_scene_renderer_taa_end_frame(s->renderer, &view, &proj);
    bench_particle_observe(s->scene, s->frame);
    if (!bench_stream_route_observe(&s->stream_route, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: streaming observation failed");
    if (!bench_asset_witness(s->scene, s->renderer, s->frame))
        LOG_ERROR(LOG_TAG, "benchmark-player: imported asset witness failed");
    const char *shot = getenv("JCE_BENCH_PLAYER_SHOT");
    if (s->frame == s->shot_frame && shot && shot[0]) {
        LOG_INFO(LOG_TAG, "benchmark-light: post_render_forwardplus=%u program_available=%u pixel_budget=%d",
            (unsigned)jce_renderer_get_forwardplus_program_active(svc->renderer),
            (unsigned)jce_shader_valid(jce_renderer_get_program_pbr_fwdplus(svc->renderer)),
            jce_lighting_get_pixel_light_count());
        JceTextureHandle output = jce_postfx_get_output(post);
        if (text_capture) output.idx = jce_offscreen_target_get_color_texture(s->target);
        if (bench_rt_enabled()) {
            JceTextureHandle intermediate = bench_rt_texture(s->renderer, output);
            if (intermediate.idx == UINT16_MAX) {
                LOG_ERROR(LOG_TAG, "benchmark-player: intermediate capture unavailable");
                return;
            }
            jce_offscreen_target_composite_texture(s->target, BENCH_COMPOSITE_VIEW,
                intermediate.idx, BENCH_WIDTH, BENCH_HEIGHT, jce_renderer_origin_bottom_left());
            output.idx = jce_offscreen_target_get_color_texture(s->target);
        }
        if (!text_capture && !bench_rt_enabled() && output.idx != UINT16_MAX) {
            /* Reuse the public post -> plain bridge composition path. The
             * staging source then has the bridge's documented row origin,
             * rather than assuming every post chain reads back bottom-up. */
            if (output.idx != color.idx)
                jce_offscreen_target_composite_texture(s->target, BENCH_COMPOSITE_VIEW,
                    output.idx, BENCH_WIDTH, BENCH_HEIGHT, jce_renderer_origin_bottom_left());
            output.idx = color.idx;
        }
        if (output.idx == UINT16_MAX || !jce_renderer_readback_capture_submit(
                output.idx, BENCH_READBACK_VIEW, BENCH_WIDTH, BENCH_HEIGHT, shot,
                jce_renderer_origin_bottom_left()))
            LOG_ERROR(LOG_TAG, "benchmark-player: capture submission failed");
        else s->shot_submitted = true;
    }
}

JceAppDesc benchmark_desc(void)
{
    const char *config_path = getenv("JCE_BENCH_CONFIG_FILE");
    if (config_path && config_path[0]) jce_engine_set_config_path(config_path);
    JceAppDesc desc = {0};
    desc.name = "JCE Benchmark Player";
    desc.init = benchmark_init;
    desc.exit = benchmark_exit;
    desc.update = benchmark_update;
    desc.draw = benchmark_draw;
    desc.user_data = &state;
    desc.window_width = BENCH_WIDTH;
    desc.window_height = BENCH_HEIGHT;
    return desc;
}

JCE_MAIN(benchmark_desc)
