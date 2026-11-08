/* Controlled shader recovery, using public SDK owners only. */
#include "fixture.h"
#include <jce/api_core.h>
#include <jce/api_scene.h>
#include <jce/api_graphics.h>
#include <jce/api_render.h>
#include <jce/api_resource.h>
#include <jce/os/core/jce_trace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define LAB_WIDTH 960u
#define LAB_HEIGHT 540u
#define LAB_VIEW JCE_VIEW_RUNTIME_GAME
#define LAB_COPY_VIEW 181u
#define LAB_READ_VIEW 249u
#define LAB_PHASE_COUNT 15u
#define LAB_SETTLE_FRAMES 12u
#define LAB_PHASE_TIMEOUT_MS 15000u
#define LAB_TAG "render_liveness_lab"

typedef struct LabPhase {
    const char *name;
    unsigned archive;
    bool reset, visible, casts;
    JceSceneViewModeKind view;
    bool expect_colour;
    uint32_t expected_program_reason;
} LabPhase;

/* Failed colour starts before any valid grass program exists. A colour-only
 * archive then restores colour while depth remains absent. Valid programs are
 * never artificially destroyed to fabricate a later missing-resource case. */
static const LabPhase phases[LAB_PHASE_COUNT] = {
    {"colour_missing", 0, false, true, false, JCE_SCENE_VIEW_SHADED, false, 1024},
    {"colour_restored_without_reset", 1, false, true, false, JCE_SCENE_VIEW_SHADED, false, 1024},
    {"colour_recovered", 1, true, true, false, JCE_SCENE_VIEW_SHADED, true, 0},
    {"depth_missing", 1, false, true, true, JCE_SCENE_VIEW_SHADED, true, 256},
    {"depth_restored_without_reset", 2, false, true, true, JCE_SCENE_VIEW_SHADED, true, 256},
    {"depth_recovered", 2, true, true, true, JCE_SCENE_VIEW_SHADED, true, 0},
    {"visible_off", 2, false, false, true, JCE_SCENE_VIEW_SHADED, false, 0},
    {"visible_on", 2, false, true, true, JCE_SCENE_VIEW_SHADED, true, 0},
    {"cast_off", 2, false, true, false, JCE_SCENE_VIEW_SHADED, true, 0},
    {"cast_on_repeat", 2, false, true, true, JCE_SCENE_VIEW_SHADED, true, 0},
    {"scene_depth_on", 2, false, true, true, JCE_SCENE_VIEW_SCENE_DEPTH, true, 0},
    {"scene_depth_off", 2, false, false, true, JCE_SCENE_VIEW_SCENE_DEPTH, false, 0},
    {"shadow_mask_on", 2, false, true, true, JCE_SCENE_VIEW_SHADOW_MASK, true, 0},
    {"shadow_mask_off", 2, false, true, false, JCE_SCENE_VIEW_SHADOW_MASK, true, 0},
    {"shaded_repeat", 2, false, true, true, JCE_SCENE_VIEW_SHADED, true, 0}
};

typedef struct Lab {
    JceScene *scene;
    JceCamera *camera;
    JceSceneRenderer *renderer;
    JceOffscreenTarget *target;
    JcePakArchive *anchor;
    JcePakArchive *archives[3];
    JcePakArchive *active;
    JceEntity grass;
    JceTraceCursor cursor;
    uint64_t phase_id, previous_task, phase_started;
    uint64_t colour_accepted, colour_authored, colour_owner, generation;
    uint16_t colour_program, depth_program;
    unsigned shadow_admissions, shadow_scopes, colour_failures, depth_failures;
    unsigned phase, frame, total_frames;
    bool phase_active, capture_pending, failed, finished;
    char output[1024];
} Lab;
static Lab lab;
static void lab_release(void);

static bool lab_record(const char *name, const char *text)
{
    char path[1400];
    int length = snprintf(path, sizeof path, "%s/%s", lab.output, name);
    if (length <= 0 || length >= (int)sizeof path) {
        LOG_ERROR(LAB_TAG, "LAB_RECORD_FAILED name=%s reason=record_path_overflow", name);
        return false;
    }
    if (!jce_fs_host_write_new(path, text, (uint64_t)strlen(text))) {
        LOG_ERROR(LAB_TAG, "LAB_RECORD_FAILED name=%s reason=host_write_new_failed path=%s",
            name, path);
        return false;
    }
    return true;
}

static void lab_failure(const char *reason)
{
    if (!lab.failed)
        LOG_ERROR(LAB_TAG, "LAB_FAILED phase=%u name=%s reason=%s",
            lab.phase, lab.phase < LAB_PHASE_COUNT ? phases[lab.phase].name : "end", reason);
    if (!lab.failed && lab.output[0]) {
        char evidence[512];
        snprintf(evidence, sizeof evidence,
            "{\"phase\":%u,\"reason\":\"%s\",\"accepted\":%llu,\"colour_failures\":%u,\"depth_failures\":%u}\n",
            lab.phase, reason, (unsigned long long)lab.colour_accepted,
            lab.colour_failures, lab.depth_failures);
        lab_record("failure.json", evidence);
    }
    lab.failed = true;
    jce_engine_request_quit();
}

static bool lab_setup_check(bool accepted, const char *stage,
                            const char *reason, const char *detail)
{
    if (accepted) return true;
    LOG_ERROR(LAB_TAG, "LAB_ADMISSION_FAILED stage=%s reason=%s detail=%s",
        stage, reason, detail);
    if (!lab.failed && lab.output[0]) {
        char evidence[512];
        int written = snprintf(evidence, sizeof evidence,
            "{\"stage\":\"%s\",\"reason\":\"%s\",\"detail\":\"%s\"}\n",
            stage, reason, detail);
        if (written > 0 && written < (int)sizeof evidence)
            lab_record("admission.json", evidence);
        else
            LOG_ERROR(LAB_TAG, "%s", "LAB_RECORD_FAILED name=admission.json reason=record_overflow");
    }
    lab_failure(reason);
    return false;
}

static JceEntity lab_entity(const char *name, jce_vec3 position, jce_vec3 scale)
{
    JceEntity entity = jce_scene_create_entity(lab.scene, name);
    JceTransform transform = {position, jce_q_identity(), scale};
    if (entity) jce_scene_set_transform(lab.scene, entity, &transform);
    return entity;
}

static bool lab_scene_create(void)
{
    lab.scene = jce_scene_create();
    if (!lab_setup_check(lab.scene != NULL, "scene_create",
            "scene_owner_create_failed", "scene")) return false;
    JceEntity floor = lab_entity("Receiver", jce_v3(0, -.2f, 0), jce_v3(14, .4f, 14));
    JceEntity sun = lab_entity("Sun", jce_v3(0, 0, 0), jce_v3(1, 1, 1));
    lab.grass = lab_entity("Grass", jce_v3(-1, .02f, 0), jce_v3(1, 1, 1));
    if (!lab_setup_check(floor != 0, "receiver_create",
            "receiver_entity_create_failed", "Receiver")) return false;
    if (!lab_setup_check(sun != 0, "sun_create",
            "sun_entity_create_failed", "Sun")) return false;
    if (!lab_setup_check(lab.grass != 0, "grass_create",
            "grass_entity_create_failed", "Grass")) return false;
    JceMeshRenderer mesh;
    jce_mesh_renderer_init(&mesh);
    mesh.visible = true;
    mesh.mesh_shape = 0;
    mesh.base_color[0] = .55f;
    mesh.base_color[1] = .55f;
    mesh.base_color[2] = .55f;
    mesh.base_color[3] = 1;
    mesh.roughness = 1;
    mesh.normal_scale = mesh.ao_strength = 1;
    mesh.shadow_cast_off = true;
    mesh.uv_tiling[0] = mesh.uv_tiling[1] = 1;
    jce_scene_set_mesh_renderer(lab.scene, floor, &mesh);
    if (!lab_setup_check(jce_scene_get_mesh_renderer(lab.scene, floor) != NULL,
            "receiver_component", "receiver_component_missing", "MeshRenderer")) return false;
    JceDirectionalLight light = {0};
    light.direction = jce_v3(-.7f, -1, -.2f);
    light.color = jce_v3(1, 1, 1);
    light.intensity = 2;
    light.casts_shadow = true;
    jce_scene_set_dir_light(lab.scene, sun, &light);
    if (!lab_setup_check(jce_scene_get_dir_light(lab.scene, sun) != NULL,
            "sun_component", "sun_component_missing", "DirectionalLight")) return false;
    JceGrassFieldComponent field = {0};
    field.density = 8;
    field.seed = 417;
    field.area_x = field.area_z = 3;
    field.max_slope_deg = 90;
    field.scale_min = field.scale_max = 1;
    field.blade_height = 1.8f;
    field.blade_width = .22f;
    field.cards = 3;
    field.root_color[0] = .02f;
    field.root_color[1] = .18f;
    field.root_color[2] = .01f;
    field.tip_color[0] = .15f;
    field.tip_color[1] = .8f;
    field.tip_color[2] = .04f;
    field.wind_dir[0] = 1;
    field.wind_speed = field.wind_amplitude = field.hue_jitter = 0;
    field.fade_start = 80;
    field.fade_end = 100;
    field.visible = true;
    jce_scene_set_grass_field(lab.scene, lab.grass, &field);
    if (!lab_setup_check(jce_scene_get_grass_field(lab.scene, lab.grass) != NULL,
            "grass_component", "grass_component_missing", "GrassField")) return false;
    JceSceneRenderingSettings settings = jce_scene_rendering_settings_default();
    settings.ambient_color[0] = settings.ambient_color[1] = settings.ambient_color[2] = .15f;
    settings.ambient_intensity = .3f;
    settings.fog_enabled = settings.tod_enabled = false;
    settings.shadow_distance = 40;
    settings.shadow_resolution = 1024;
    settings.cascade_count = 2;
    settings.soft_shadow_mode = JCE_SCENE_SOFT_SHADOW_OFF;
    memset(settings.postfx_enabled, 0, sizeof settings.postfx_enabled);
    jce_scene_set_rendering_settings(lab.scene, &settings);
    JceCameraDesc camera = {0};
    camera.position = jce_v3(7, 7, 10);
    camera.target = jce_v3(0, .5f, 0);
    camera.up = jce_v3(0, 1, 0);
    camera.fov_deg = 45;
    camera.near_plane = .1f;
    camera.far_plane = 60;
    lab.camera = jce_camera_create(&camera);
    return lab_setup_check(lab.camera != NULL, "camera_create",
        "camera_owner_create_failed", "camera");
}

/* Read production counters through the public bounded trace stream. These are
 * actual CPU admission/submission witnesses, not GPU execution completion. */
static void lab_trace_read(bool retain)
{
    JceTraceEvent events[256];
    uint32_t count;
    do {
        count = jce_trace_read(&lab.cursor, events, 256);
        for (uint32_t i = 0; retain && i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            if (event->type == JCE_TRACE_EVENT_COUNTER) {
                if (!strcmp(event->name, "grass.colour.accepted") &&
                    event->value_u64 > lab.colour_accepted)
                    lab.colour_accepted = event->value_u64;
                if (!strcmp(event->name, "grass.colour.authored") &&
                    event->value_u64 > lab.colour_authored)
                    lab.colour_authored = event->value_u64;
                if (!strcmp(event->name, "grass.colour.owner")) lab.colour_owner = event->value_u64;
                if (!strcmp(event->name, "grass.program.generation")) lab.generation = event->value_u64;
                if (!strcmp(event->name, "grass.program.colour")) lab.colour_program = (uint16_t)event->value_u64;
                if (!strcmp(event->name, "grass.program.depth")) lab.depth_program = (uint16_t)event->value_u64;
                if (!strcmp(event->name, "grass.program.need_shadow") && event->value_u64 == 1)
                    ++lab.shadow_admissions;
                if (!strcmp(event->name, "grass.program.reason")) {
                    if (event->value_u64 == 1024) ++lab.colour_failures;
                    if (event->value_u64 == 256) ++lab.depth_failures;
                }
            }
            if (event->type == JCE_TRACE_EVENT_TASK_BEGIN &&
                !strcmp(event->name, "sr.grass.shadow.submit")) ++lab.shadow_scopes;
        }
    } while (count == 256);
    if (lab.cursor.lost_events) lab_failure("production_trace_events_lost");
}

static bool lab_phase_begin(void)
{
    const LabPhase *phase = &phases[lab.phase];
    lab_trace_read(false);
    if (lab.active != lab.archives[phase->archive]) {
        if (lab.active) jce_pak_overlay_remove(lab.anchor, lab.active);
        lab.active = lab.archives[phase->archive];
        jce_pak_overlay_push(lab.anchor, lab.active);
    }
    if (phase->reset) {
#if !defined(LAB_DISABLE_RESET)
        jce_scene_renderer_reset_entity_caches(lab.renderer);
#endif
    }
    JceGrassFieldComponent *field = jce_scene_get_grass_field(lab.scene, lab.grass);
    if (!field) {
        lab_failure("phase_grass_component_missing");
        return false;
    }
    JceGrassFieldComponent edited = *field;
    edited.visible = phase->visible;
    edited.cast_shadow = phase->casts;
    jce_scene_set_grass_field(lab.scene, lab.grass, &edited);
    lab.colour_accepted = lab.colour_authored = lab.colour_owner = lab.generation = 0;
    lab.shadow_admissions = lab.shadow_scopes = lab.colour_failures = lab.depth_failures = 0;
    lab.colour_program = lab.depth_program = UINT16_MAX;
    lab.frame = 0;
    lab.phase_started = jce_time_ticks_ms();
    lab.previous_task = jce_trace_task_current_id();
    lab.phase_id = jce_trace_next_id();
    jce_trace_task_submit(lab.phase_id, lab.previous_task, JCE_TRACE_TASK_FRAME_JOB, phase->name);
    jce_trace_task_begin(lab.phase_id, lab.phase_id, phase->name, 0);
    LOG_INFO(LAB_TAG, "LAB_PHASE index=%u name=%s owner=%llu entity=%llu archive=%u reset_requested=%u visible=%u casts=%u view=%u",
        lab.phase, phase->name, (unsigned long long)(uintptr_t)lab.renderer,
        (unsigned long long)lab.grass, phase->archive, (unsigned)phase->reset,
        (unsigned)phase->visible, (unsigned)phase->casts, (unsigned)phase->view);
    lab.phase_active = true;
    return true;
}

static bool lab_phase_complete(void)
{
    const LabPhase *phase = &phases[lab.phase];
    LOG_INFO(LAB_TAG, "LAB_WITNESS index=%u name=%s owner=%llu generation=%llu accepted=%llu authored=%llu shadow_admissions=%u shadow_scopes=%u colour_failures=%u depth_failures=%u lost=%llu",
        lab.phase, phase->name, (unsigned long long)(uintptr_t)lab.renderer,
        (unsigned long long)lab.generation, (unsigned long long)lab.colour_accepted,
        (unsigned long long)lab.colour_authored, lab.shadow_admissions, lab.shadow_scopes,
        lab.colour_failures, lab.depth_failures, (unsigned long long)lab.cursor.lost_events);
    if (phase->expect_colour != (lab.colour_accepted > 0))
        return false;
    if (phase->expect_colour && (!lab.colour_authored ||
        lab.colour_owner != (uint64_t)(uintptr_t)lab.renderer)) return false;
    if (phase->expected_program_reason == 1024 && !lab.colour_failures) return false;
    if (phase->expected_program_reason == 256 && !lab.depth_failures) return false;
    if (phase->expected_program_reason == 0 && (lab.colour_failures || lab.depth_failures)) return false;
    if (phase->visible && phase->casts && phase->expected_program_reason == 0 &&
        (!lab.shadow_admissions || !lab.shadow_scopes)) return false;

    char evidence[1400], record_name[128];
    snprintf(record_name, sizeof record_name, "%02u-%s.json", lab.phase, phase->name);
    int written = snprintf(evidence, sizeof evidence,
        "{\"index\":%u,\"name\":\"%s\",\"owner\":%llu,\"entity\":%llu,"
        "\"generation\":%llu,\"accepted\":%llu,\"authored\":%llu,"
        "\"colour_program\":%u,\"depth_program\":%u,"
        "\"shadow_admissions\":%u,\"shadow_scopes\":%u,"
        "\"colour_failures\":%u,\"depth_failures\":%u,\"lost\":%llu,"
        "\"visible\":%s,\"casts\":%s,\"view\":%u,\"capture_frames\":%u}\n",
        lab.phase, phase->name, (unsigned long long)(uintptr_t)lab.renderer,
        (unsigned long long)lab.grass, (unsigned long long)lab.generation,
        (unsigned long long)lab.colour_accepted, (unsigned long long)lab.colour_authored,
        (unsigned)lab.colour_program, (unsigned)lab.depth_program,
        lab.shadow_admissions, lab.shadow_scopes, lab.colour_failures, lab.depth_failures,
        (unsigned long long)lab.cursor.lost_events,
        phase->visible ? "true" : "false", phase->casts ? "true" : "false",
        (unsigned)phase->view, lab.frame);
    if (written <= 0 || written >= (int)sizeof evidence || !lab_record(record_name, evidence))
        return false;
    jce_trace_task_end(lab.phase_id, lab.phase_id, phase->name, JCE_TRACE_TASK_SUCCEEDED, 0);
    lab.phase_active = false;
    ++lab.phase;
    if (lab.phase == LAB_PHASE_COUNT) {
        if (!lab_record("complete.json",
                "{\"state\":\"CAPTURES_COMPLETE\",\"phases\":15}\n")) return false;
        lab.finished = true;
        LOG_INFO(LAB_TAG, "LAB_COMPLETE phases=%u owner=%llu frames=%u lost=0",
            LAB_PHASE_COUNT, (unsigned long long)(uintptr_t)lab.renderer, lab.total_frames);
        jce_engine_request_quit();
    }
    return true;
}

bool lab_init(const JceServices *services, void *user)
{
    (void)user;
    memset(&lab, 0, sizeof lab);
    const char *output = getenv("JCE_LAB_OUTPUT");
    const char *anchor = getenv("JCE_LAB_ANCHOR");
    const char *names[3] = {"JCE_LAB_MISSING_COLOUR", "JCE_LAB_MISSING_DEPTH", "JCE_LAB_FULL"};
    if (!lab_setup_check(output && output[0] && strlen(output) < sizeof lab.output,
            "output_input", "explicit_output_required", "JCE_LAB_OUTPUT")) return false;
    strcpy(lab.output, output);
    if (!lab_setup_check(anchor && anchor[0], "anchor_input",
            "explicit_anchor_required", "JCE_LAB_ANCHOR")) return false;
    if (!lab_setup_check(services != NULL, "services", "services_unavailable", "services"))
        return false;
    if (!lab_setup_check(services->renderer != NULL, "renderer_service",
            "renderer_service_unavailable", "renderer")) return false;
    if (!lab_setup_check(!jce_renderer_is_headless(services->renderer), "gpu_backend",
            "headless_backend_rejected", "renderer")) return false;
    if (!lab_setup_check(!jce_renderer_is_fallback(services->renderer), "gpu_backend",
            "fallback_backend_rejected", "renderer")) return false;
    if (!lab_setup_check(jce_trace_enabled(), "trace_admission",
            "production_trace_required", "trace")) return false;
    if (!lab_setup_check(jce_shaders_embedded_engine_pak() == NULL, "sdk_admission",
            "embedded_engine_shader_pak_rejected", "matching_embed_off_sdk")) return false;
    if (!lab_setup_check((jce_renderer_get_caps() & JCE_CAP_INSTANCING) != 0,
            "instancing_admission", "gpu_instancing_required", "renderer")) return false;
    const char *persistent = getenv("JCE_PERSIST_GRASS");
    if (!lab_setup_check(persistent && !strcmp(persistent, "1"), "grass_policy",
            "JCE_PERSIST_GRASS_must_equal_1", "JCE_PERSIST_GRASS")) return false;
    lab.anchor = jce_pak_open_file(anchor);
    if (!lab_setup_check(lab.anchor != NULL, "anchor_open",
            "anchor_archive_open_failed", "JCE_LAB_ANCHOR")) goto failed;
    if (!lab_setup_check(jce_pak_count(lab.anchor) == 1, "anchor_count",
            "anchor_archive_count_mismatch", "anchor")) goto failed;
    for (unsigned i = 0; i < 3; ++i) {
        const char *path = getenv(names[i]);
        if (!lab_setup_check(path && path[0], "controlled_archive_input",
                "explicit_controlled_archive_required", names[i])) goto failed;
        lab.archives[i] = jce_pak_open_file(path);
        if (!lab_setup_check(lab.archives[i] != NULL, "controlled_archive_open",
                "controlled_archive_open_failed", names[i])) goto failed;
        LOG_INFO(LAB_TAG, "LAB_SETUP stage=controlled_archive_open input=%s entries=%u",
            names[i], jce_pak_count(lab.archives[i]));
    }
    const char *suffix = jce_shaders_backend_suffix();
    char keys[3][128];
    if (!lab_setup_check(suffix && suffix[0], "shader_backend",
            "shader_backend_suffix_unavailable", "backend")) goto failed;
    if (!lab_setup_check(services->pak != NULL, "boot_archive",
            "complete_boot_archive_unavailable", "services.pak")) goto failed;
    snprintf(keys[0], sizeof keys[0], "shaders/vs_grass_%s.bin", suffix);
    snprintf(keys[1], sizeof keys[1], "shaders/fs_grass_%s.bin", suffix);
    snprintf(keys[2], sizeof keys[2], "shaders/fs_grass_shadow_%s.bin", suffix);
    for (unsigned key = 0; key < 3; ++key) {
        if (!lab_setup_check(jce_pak_find(lab.anchor, keys[key]) == NULL, "anchor_stage_absence",
                "anchor_contains_grass_stage", keys[key])) goto failed;
        if (!lab_setup_check(jce_pak_find(lab.archives[2], keys[key]) != NULL, "full_stage_presence",
                "full_archive_grass_stage_missing", keys[key])) goto failed;
        if (!lab_setup_check(jce_pak_find(services->pak, keys[key]) != NULL, "boot_stage_presence",
                "boot_archive_grass_stage_missing", keys[key])) goto failed;
    }
    if (!lab_setup_check(jce_pak_find(lab.archives[0], keys[0]) != NULL, "missing_colour_vertex",
            "missing_colour_vertex_stage_missing", keys[0])) goto failed;
    if (!lab_setup_check(jce_pak_find(lab.archives[0], keys[1]) == NULL, "missing_colour_fragment",
            "missing_colour_fragment_stage_present", keys[1])) goto failed;
    if (!lab_setup_check(jce_pak_find(lab.archives[0], keys[2]) == NULL, "missing_colour_depth",
            "missing_colour_depth_stage_present", keys[2])) goto failed;
    if (!lab_setup_check(jce_pak_find(lab.archives[1], keys[0]) != NULL, "missing_depth_vertex",
            "missing_depth_vertex_stage_missing", keys[0])) goto failed;
    if (!lab_setup_check(jce_pak_find(lab.archives[1], keys[1]) != NULL, "missing_depth_colour",
            "missing_depth_colour_stage_missing", keys[1])) goto failed;
    if (!lab_setup_check(jce_pak_find(lab.archives[1], keys[2]) == NULL, "missing_depth_fragment",
            "missing_depth_fragment_stage_present", keys[2])) goto failed;
    LOG_INFO(LAB_TAG, "LAB_SETUP stage=controlled_stage_admission backend_suffix=%s", suffix);
    /* Non-grass programs are eagerly created by the shared renderer owner. */
    lab.active = lab.archives[0];
    jce_pak_overlay_push(lab.anchor, lab.active);
    if (!lab_setup_check(jce_pak_find(lab.anchor, keys[0]) != NULL, "anchor_fallback_vertex",
            "anchor_fallback_vertex_stage_missing", keys[0])) goto failed;
    if (!lab_setup_check(jce_pak_find(lab.anchor, keys[1]) == NULL, "anchor_fallback_colour",
            "anchor_fallback_colour_stage_present", keys[1])) goto failed;
    if (!lab_setup_check(jce_pak_find(lab.anchor, keys[2]) == NULL, "anchor_fallback_depth",
            "anchor_fallback_depth_stage_present", keys[2])) goto failed;
    JceRenderPipelineDesc pipeline;
    jce_render_pipeline_preset_high(&pipeline);
    pipeline.enable_taa = pipeline.enable_ssao = pipeline.enable_ssr = false;
    pipeline.enable_bloom = pipeline.enable_motion_blur = pipeline.enable_volumetric_fog = false;
    pipeline.enable_gpu_particles = pipeline.enable_cloth = pipeline.enable_stylized_sky = false;
    pipeline.enable_ssgi = pipeline.depth_prepass = false;
    pipeline.shadow_resolution = 1024;
    pipeline.csm_cascade_count = 2;
    pipeline.shadow_filter_quality = 0;
    pipeline.msaa_samples = 1;
    pipeline.render_scale = 1;
    for (unsigned i = 0; i < JCE_RP_PERF_COUNT; ++i) pipeline.perf[i] = 0;
    jce_render_pipeline_apply(&pipeline);
    if (!lab_scene_create()) goto failed;
    lab.renderer = jce_scene_renderer_create(services->renderer, lab.anchor, NULL);
    if (!lab_setup_check(lab.renderer != NULL, "scene_renderer_create",
            "scene_renderer_owner_create_failed", "scene_renderer")) goto failed;
    LOG_INFO(LAB_TAG, "LAB_SETUP stage=scene_renderer_create owner=%llu",
        (unsigned long long)(uintptr_t)lab.renderer);
    /* The scene renderer registers this public cvar during creation. */
    JceCvar *taa = jce_cvar_find("r.taa");
    if (!lab_setup_check(taa != NULL, "taa_cvar_lookup",
            "taa_cvar_unavailable_after_renderer_create", "r.taa")) goto failed;
    jce_cvar_set_bool(taa, false);
    if (!lab_setup_check(!jce_cvar_get_bool(taa), "taa_cvar_control",
            "taa_cvar_disable_failed", "r.taa")) goto failed;
    lab.target = jce_offscreen_target_create(services->renderer, LAB_VIEW);
    if (!lab_setup_check(lab.target != NULL, "offscreen_target_create",
            "offscreen_target_owner_create_failed", "offscreen_target")) goto failed;
    jce_scene_renderer_set_grass_enabled(lab.renderer, true);
    JcePostFXPipeline *post = jce_scene_renderer_get_postfx(lab.renderer);
    if (!lab_setup_check(post != NULL, "postfx_owner",
            "scene_renderer_postfx_unavailable", "postfx")) goto failed;
    jce_postfx_set_view_base(post, 160);
    jce_trace_cursor_init(&lab.cursor);
    lab_trace_read(false);
    if (lab.failed) goto failed;
    LOG_INFO(LAB_TAG, "LAB_READY backend=%s owner=%llu grass=%llu caster_components=1 receiver_casts=0 fixed_render_dt=0 wind_amplitude=0 embedded_fallback=0",
        jce_renderer_get_backend_name(services->renderer),
        (unsigned long long)(uintptr_t)lab.renderer, (unsigned long long)lab.grass);
    return true;
failed:
    lab_release();
    return false;
}

static void lab_draw_body(const JceServices *services, void *user)
{
    (void)services; (void)user;
    if (lab.failed || lab.finished) return;
    if (!lab.phase_active && !lab_phase_begin()) {
        lab_failure("phase_setup_failed"); return;
    }
    jce_trace_task_restore_id(lab.phase_id);
    const LabPhase *phase = &phases[lab.phase];
    JceSceneRenderConfig config = jce_scene_render_config_default();
    config.viewport_width = LAB_WIDTH;
    config.viewport_height = LAB_HEIGHT;
    config.frustum_culling = false;
    config.draw_skybox = config.apply_postfx = false;
    config.draw_shadows = true;
    config.view_mode = phase->view;
    config.shadow_map_size = 1024;
    config.csm_cascades = 2;
    config.shadow_distance = 40;
    jce_mat4 view = jce_camera_view(lab.camera);
    jce_mat4 projection = jce_camera_proj(lab.camera, (float)LAB_WIDTH / LAB_HEIGHT,
                                         jce_gfx_caps().homogeneous_depth);
    if (!jce_offscreen_target_prepare(lab.target, LAB_WIDTH, LAB_HEIGHT,
            JCE_M4_PTR(view), JCE_M4_PTR(projection), 0x202020ff, "Render Liveness Lab") ||
        !jce_offscreen_target_is_hdr(lab.target)) {
        lab_failure("requires_prepared_HDR_readback_target"); return;
    }
    config.scene_frame_buffer = jce_offscreen_target_get_frame_buffer(lab.target);
    config.scene_depth_tex_handle = jce_offscreen_target_get_depth_texture(lab.target);
    /* The fixture intentionally freezes renderer time, with no runtime/scripts. */
    jce_scene_renderer_render(lab.renderer, lab.scene, lab.camera, LAB_VIEW, 0, &config);
    lab_trace_read(true);
    JcePostFXPipeline *post = jce_scene_renderer_get_postfx(lab.renderer);
    jce_postfx_resize(post, LAB_WIDTH, LAB_HEIGHT);
    jce_postfx_enable(post, JCE_POSTFX_TONEMAP, true);
    JcePostFXParams params = jce_postfx_default_params();
    params.exposure = 1;
    params.gamma = 2.2f;
    jce_postfx_set_params(post, &params);
    JceTextureHandle color = {jce_offscreen_target_get_color_texture(lab.target)};
    JceTextureHandle depth = {jce_offscreen_target_get_depth_texture(lab.target)};
    jce_postfx_apply(post, color, depth);
    jce_postfx_present(post, LAB_WIDTH, LAB_HEIGHT);
    JceTextureHandle output = jce_postfx_get_output(post);
    if (output.idx != color.idx)
        jce_offscreen_target_composite_texture(lab.target, LAB_COPY_VIEW, output.idx,
            LAB_WIDTH, LAB_HEIGHT, jce_renderer_origin_bottom_left());
    ++lab.frame; ++lab.total_frames;
    if (!lab.capture_pending && lab.frame == LAB_SETTLE_FRAMES) {
        char path[1400];
        int written = snprintf(path, sizeof path, "%s/%02u-%s.png",
            lab.output, lab.phase, phase->name);
        if (written < 0 || written >= (int)sizeof path ||
            !jce_renderer_readback_capture_submit(color.idx, LAB_READ_VIEW, LAB_WIDTH,
                LAB_HEIGHT, path, jce_renderer_origin_bottom_left())) {
            lab_failure("actual_GPU_readback_submission_failed"); return;
        }
        lab.capture_pending = true;
    }
    if (lab.capture_pending) {
        const int result = jce_renderer_readback_capture_poll();
        if (result == 2) { lab_failure("actual_PNG_write_failed"); return; }
        if (result == 1) {
            lab.capture_pending = false;
            if (!lab_phase_complete()) lab_failure("production_phase_witness_failed");
        }
    }
    if (lab.phase_active && jce_time_ticks_ms() - lab.phase_started > LAB_PHASE_TIMEOUT_MS)
        lab_failure("actual_capture_or_phase_timeout");
}

void lab_draw(const JceServices *services, void *user)
{
    uint64_t previous = jce_trace_task_current_id();
    lab_draw_body(services, user);
    jce_trace_task_restore_id(previous);
}

static void lab_release(void)
{
    jce_renderer_readback_capture_flush();
    if (lab.phase_active) {
        jce_trace_task_end(lab.phase_id, lab.phase_id, phases[lab.phase].name,
            JCE_TRACE_TASK_FAILED, 0);
    }
    lab.phase_active = lab.capture_pending = false;
    if (lab.renderer) jce_scene_renderer_destroy(lab.renderer);
    lab.renderer = NULL;
    if (lab.target) jce_offscreen_target_destroy(lab.target);
    lab.target = NULL;
    if (lab.camera) jce_camera_destroy(lab.camera);
    lab.camera = NULL;
    if (lab.scene) jce_scene_destroy(lab.scene);
    lab.scene = NULL;
    if (lab.active) jce_pak_overlay_remove(lab.anchor, lab.active);
    lab.active = NULL;
    for (unsigned i = 0; i < 3; ++i) {
        if (lab.archives[i]) jce_pak_close(lab.archives[i]);
        lab.archives[i] = NULL;
    }
    if (lab.anchor) jce_pak_close(lab.anchor);
    lab.anchor = NULL;
    /* The host still calls exit after failed init. Keep the first failure and
     * output context while making repeated public-owner release harmless. */
}

void lab_exit(void *user)
{
    (void)user;
    if (!lab.finished && !lab.failed)
        lab_failure("host_exit_before_all_actual_captures");
    lab_release();
}
