/*
 * jce_editor_game_render.cpp  Implementation for the embedded "Game View"
 * renderer (free-fly FPS camera, dedicated off-screen target, shares the
 * engine scene renderer instance with the main scene viewport).
 */

#include "scene/jce_editor_game_render.h"
#include "scene/jce_editor_scene_render.h"
#include "core/jce_editor_state.h"

extern "C" {
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_views.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/runtime/jce_game_module.h>
extern void jce_game_module_set_active_scene(JceScene *scene);

/* Lighting panel accessors — defined in jce_panel_lighting.cpp */
bool jce_editor_lighting_get_fog_enabled(void);
void jce_editor_lighting_get_fog_params(JceVolumetricFogParams *out);
void jce_editor_lighting_get_ambient(float out_color_rgb[3], float *out_intensity);
}

#include <cstring>
#include <cmath>

#define LOG_TAG "game_render"

namespace {

struct GameRenderState {
    bool                   initialized       = false;
    bool                   homogeneous_depth = false;
    bool                   mouse_captured    = false;
    JceRenderer           *renderer          = nullptr;
    JceWindow             *window            = nullptr;
    JceOffscreenTarget    *bridge            = nullptr;
    JceCamera             *camera            = nullptr;
    JcePostFXPipeline     *postfx            = nullptr;  /* game-view own pipeline */
    uint16_t               postfx_output_tex = UINT16_MAX;

    /* Active game module + lifecycle bookkeeping. */
    const JceGameModule   *module            = nullptr;
    bool                   module_inited     = false;
    int                    last_play_state   = 0;  /* JcePlayState */
    JceServices            svc               = {};
};

GameRenderState g;

/* Use a view ID well above the editor scene + shadow + post FX range.
 * Engine scene renderer expects shadow views at base+10..+13 and post
 * stages around base+20, so picking 80 keeps us out of every other slot
 * the editor uses. */
constexpr uint16_t GAME_VIEW_BASE = 80;

void game_reset_camera_internal(void)
{
    if (!g.camera) return;
    jce_camera_set_position(g.camera, jce_v3(0.0f, 1.7f, 6.0f));
    jce_camera_set_target  (g.camera, jce_v3(0.0f, 1.5f, 0.0f));
    jce_camera_set_fov     (g.camera, 60.0f);
    jce_camera_set_near_far(g.camera, 0.05f, 1000.0f);
}

} /* anon namespace */

bool jce_editor_game_render_init(JceRenderer *renderer, JceWindow *window,
                                 const JcePakArchive *pak)
{
    if (g.initialized) return true;
    if (!renderer) return false;

    g.renderer = renderer;
    g.window   = window;
    JceGfxCaps caps = jce_gfx_caps();
    g.homogeneous_depth = caps.homogeneous_depth;

    JceCameraDesc desc = {};
    desc.mode       = JCE_CAMERA_PERSPECTIVE;
    desc.position   = jce_v3(0.0f, 1.7f, 6.0f);
    desc.target     = jce_v3(0.0f, 1.5f, 0.0f);
    desc.up         = jce_v3(0.0f, 1.0f, 0.0f);
    desc.fov_deg    = 60.0f;
    desc.near_plane = 0.05f;
    desc.far_plane  = 1000.0f;
    g.camera = jce_camera_create(&desc);
    if (!g.camera) {
        LOG_WARN(LOG_TAG, "[init] failed to create game-view camera");
        return false;
    }

    g.bridge = jce_offscreen_target_create(renderer, GAME_VIEW_BASE);
    if (!g.bridge) {
        LOG_WARN(LOG_TAG, "[init] failed to create game-view offscreen target");
        jce_camera_destroy(g.camera);
        g.camera = nullptr;
        return false;
    }

    g.initialized = true;
    LOG_INFO(LOG_TAG, "[init] game view renderer ready (view id=%u)",
             (unsigned)GAME_VIEW_BASE);

    /* Create a dedicated PostFX pipeline for the game view so it can
     * apply the same effects as the scene view without conflicting on
     * bgfx view IDs.  Uses view IDs starting at GAME_VIEW_BASE +
     * JCE_VIEW_POST_BASE (= 100) — well clear of the scene view's range
     * (20-29) and of the editor overlay (50) and ImGui (250). */
    g.postfx = jce_postfx_create(jce_allocator_default(), 1, 1);
    if (g.postfx) {
        jce_postfx_set_view_base(g.postfx,
                                 (uint16_t)(GAME_VIEW_BASE + JCE_VIEW_POST_BASE));
        if (pak) {
            jce_postfx_load_shaders(g.postfx, pak);
        }
    } else {
        LOG_WARN(LOG_TAG, "[init] failed to create game view PostFX pipeline");
    }

    return true;
}

void jce_editor_game_render_shutdown(void)
{
    if (g.window) {
        jce_window_set_mouse_rect(g.window, 0, 0, 0, 0);
    }
    if (g.mouse_captured && g.window) {
        jce_window_set_relative_mouse_mode(g.window, false);
        jce_window_set_mouse_grab(g.window, false);
        g.mouse_captured = false;
    }
    if (g.bridge) { jce_offscreen_target_destroy(g.bridge); g.bridge = nullptr; }
    if (g.camera) { jce_camera_destroy(g.camera); g.camera = nullptr; }
    if (g.postfx) { jce_postfx_destroy(g.postfx); g.postfx = nullptr; }
    g.postfx_output_tex = UINT16_MAX;
    g.renderer = nullptr;
    g.window   = nullptr;
    g.initialized = false;
}

bool jce_editor_game_render_set_mouse_capture(bool capture)
{
    if (!g.window) { g.mouse_captured = false; return false; }
    if (capture == g.mouse_captured) return g.mouse_captured;
    jce_window_set_relative_mouse_mode(g.window, capture);
    /* Hard-grab so the cursor cannot escape the editor window even if the
     * relative-mouse mode is loosened by the OS during focus transitions. */
    jce_window_set_mouse_grab(g.window, capture);
    g.mouse_captured = capture;
    return g.mouse_captured;
}

bool jce_editor_game_render_is_mouse_captured(void)
{
    return g.mouse_captured;
}

void jce_editor_game_render_warp_cursor(int x, int y)
{
    if (!g.window) return;
    jce_window_warp_mouse(g.window, x, y);
}

void jce_editor_game_render_set_cursor_rect(int x, int y, int w, int h)
{
    if (!g.window) return;
    jce_window_set_mouse_rect(g.window, x, y, w, h);
}

/* Accumulated relative mouse delta (pixels) coming from SDL_EVENT_MOUSE_MOTION
 * via the editor's main event handler.  Reads/writes happen on the same
 * thread (main loop), so a plain pair of floats is enough. */
static float s_mouse_dx_accum = 0.0f;
static float s_mouse_dy_accum = 0.0f;

void jce_editor_game_render_push_mouse_delta(float xrel, float yrel)
{
    if (!g.mouse_captured) return;  /* Only accumulate while in FPS capture. */
    s_mouse_dx_accum += xrel;
    s_mouse_dy_accum += yrel;
}

void jce_editor_game_render_consume_mouse_delta(float *dx, float *dy)
{
    if (dx) *dx = s_mouse_dx_accum;
    if (dy) *dy = s_mouse_dy_accum;
    s_mouse_dx_accum = 0.0f;
    s_mouse_dy_accum = 0.0f;
}

JceCamera *jce_editor_game_render_get_camera(void)
{
    return g.camera;
}

void jce_editor_game_render_reset_camera(void)
{
    game_reset_camera_internal();
}

uint16_t jce_editor_game_render_get_texture(void)
{
    if (!g.bridge) return UINT16_MAX;
    /* Return PostFX output when available (same as scene viewport). */
    if (g.postfx_output_tex != UINT16_MAX)
        return g.postfx_output_tex;
    return jce_offscreen_target_get_color_texture(g.bridge);
}

void jce_editor_game_render_frame(uint32_t width, uint32_t height)
{
    if (!g.initialized || !g.renderer) return;
    if (width == 0 || height == 0) return;

    JceSceneRenderer *engine_sr = jce_editor_get_scene_renderer();
    JceScene         *scene     = jce_state_get_scene();
    if (!engine_sr || !scene || !g.camera || !g.bridge) return;

    /* ── Game module lifecycle (drive on Play state edges) ──────── */
    int play_state = (int)jce_state_get_play_state();
    const JceGameModule *mod = g.module ? g.module : jce_game_module_default();

    bool play_active = (play_state == 1 /*PLAYING*/ ||
                        play_state == 2 /*PAUSED */);
    bool was_active  = (g.last_play_state == 1 || g.last_play_state == 2);

    if (play_active && !was_active) {
        /* Rising edge — init module. */
        g.svc.window   = g.window;
        g.svc.renderer = g.renderer;
        jce_game_module_set_active_scene(scene);
        if (mod && mod->init) {
            if (!mod->init(&g.svc, mod->user_data)) {
                LOG_WARN(LOG_TAG, "[play] module '%s' init failed",
                         mod->name ? mod->name : "?");
            } else {
                g.module_inited = true;
                LOG_INFO(LOG_TAG, "[play] module '%s' started",
                         mod->name ? mod->name : "?");
            }
        } else {
            g.module_inited = true;
        }
    } else if (!play_active && was_active) {
        /* Falling edge — exit module. */
        if (mod && mod->exit && g.module_inited)
            mod->exit(mod->user_data);
        g.module_inited = false;
        jce_game_module_set_active_scene(NULL);
    }
    g.last_play_state = play_state;

    /* Compute real frame dt (jce_time_perf_counter) so the engine scene
     * renderer can advance skeletal animation, particles, and other
     * time-based components in PLAY mode (Game View). */
    static uint64_t s_last_tp = jce_time_perf_counter();
    uint64_t now_tp  = jce_time_perf_counter();
    float    frame_dt = (float)jce_time_perf_to_seconds(s_last_tp, now_tp);
    s_last_tp = now_tp;
    if (frame_dt < 0.0f || frame_dt > 0.25f) frame_dt = 1.0f / 60.0f;

    /* Per-frame tick (only when actually PLAYING, not PAUSED). */
    if (play_state == 1 /*PLAYING*/ && g.module_inited && mod) {
        if (mod->update) mod->update(frame_dt, mod->user_data);
        if (mod->draw)   mod->draw(&g.svc, mod->user_data);
    }

    float aspect = (float)width / (float)height;
    jce_mat4 view = jce_camera_view(g.camera);
    jce_mat4 proj = jce_camera_proj(g.camera, aspect, g.homogeneous_depth);

    if (!jce_offscreen_target_prepare(
            g.bridge, width, height,
            view.raw[0], proj.raw[0],
            0x202028FFu,
            "EditorGame")) {
        return;
    }

    /* Game view always uses the shipped "shaded" pipeline — no debug
     * wireframe overrides, all engine features (shadows, IBL, postfx)
     * enabled by default. */
    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    cfg.view_mode = JCE_SCENE_VIEW_SHADED;

    /* The editor's Game View is meant to preview "what the player
     * would see", so always draw the skybox / sprites and never inject
     * the editor grid overlay. */
    cfg.on_after_sky    = nullptr;
    cfg.on_after_sky_ud = nullptr;

    cfg.frustum_culling = true;

    /* ── Lighting settings from the editor Lighting panel ──────────
     * Apply the same ambient override and volumetric fog that the scene
     * viewport uses so both viewports reflect lighting panel changes. */
    {
        float amb_color[3];
        float amb_intensity = 0.15f;
        jce_editor_lighting_get_ambient(amb_color, &amb_intensity);
        jce_scene_renderer_set_ambient_override(engine_sr, amb_color, amb_intensity);
    }

    cfg.fog_enabled = jce_editor_lighting_get_fog_enabled();
    if (cfg.fog_enabled) {
        jce_editor_lighting_get_fog_params(&cfg.fog);
        cfg.fog_depth_tex_handle =
            jce_offscreen_target_get_depth_texture(g.bridge);
        cfg.fog_rt_width  = (int)width;
        cfg.fog_rt_height = (int)height;
    } else {
        cfg.fog_depth_tex_handle = UINT16_MAX;
        cfg.fog_rt_width  = 0;
        cfg.fog_rt_height = 0;
    }

    uint16_t base = jce_offscreen_target_get_view_id(g.bridge);
    /* Pass real dt only while actually PLAYING — paused/stopped states
     * should freeze animation, matching Unity's Game View semantics. */
    float render_dt = (play_state == 1) ? frame_dt : 0.0f;
    jce_scene_renderer_render(engine_sr, scene, g.camera, base,
                              render_dt, &cfg);

    /* Composite volumetric fog (mirrors scene view path). */
    if (cfg.fog_enabled) {
        uint16_t fog_composite_view = (uint16_t)(base + 16);
        uint16_t dst_fb = jce_offscreen_target_get_frame_buffer(g.bridge);
        jce_scene_renderer_composite_fog(engine_sr, fog_composite_view, dst_fb);
    }

    /* ── PostFX ─────────────────────────────────────────────────────
     * Sync enabled flags and params from the shared scene renderer
     * pipeline so PostFX panel changes are reflected in the game view.
     * Uses the game view's OWN pipeline (different view IDs) to avoid
     * conflicts with the scene view applying PostFX in the same frame. */
    g.postfx_output_tex = UINT16_MAX;
    if (g.postfx) {
        JcePostFXPipeline *shared_pfx =
            jce_scene_renderer_get_postfx(engine_sr);
        if (shared_pfx) {
            /* Mirror enabled state. */
            bool any_effect = false;
            for (int i = 0; i < JCE_POSTFX_COUNT; i++) {
                bool en = jce_postfx_is_enabled(shared_pfx, (JcePostFXType)i);
                jce_postfx_enable(g.postfx, (JcePostFXType)i, en);
                if (en) any_effect = true;
            }
            /* Mirror params. */
            JcePostFXParams pfx_params;
            jce_postfx_get_params(shared_pfx, &pfx_params);
            jce_postfx_set_params(g.postfx, &pfx_params);

            if (any_effect) {
                jce_postfx_resize(g.postfx, width, height);
                JceTextureHandle game_color = { UINT16_MAX };
                JceTextureHandle prev_pass  = { UINT16_MAX };
                game_color.idx = jce_offscreen_target_get_color_texture(g.bridge);
                if (jce_gfx_texture_valid(game_color)) {
                    jce_postfx_apply(g.postfx, game_color, prev_pass);
                    JceTextureHandle out = jce_postfx_get_output(g.postfx);
                    if (jce_gfx_texture_valid(out))
                        g.postfx_output_tex = out.idx;
                }
            }
        }
    }

    /* Physics debug wireframes (toggled via scene-view View menu). */
    if (jce_state_get_show_physics_debug() && scene) {
        const uint32_t col_box     = 0xFF00FF00;
        const uint32_t col_sphere  = 0xFF00FFFF;
        const uint32_t col_capsule = 0xFFFFFF00;
        int count = jce_state_get_entity_count();
        for (int i = 0; i < count; i++) {
            uint32_t id = jce_state_get_entity_id_by_index(i);
            if (id == 0 || !jce_state_entity_exists(id)) continue;
            if (!jce_state_entity_enabled(id)) continue;
            JceEntity e = (JceEntity)id;
            JceTransform *t = jce_scene_get_transform(scene, e);
            if (!t) continue;
            jce_vec3 center = t->position;
            jce_quat q = t->rotation;
            if (jce_scene_has_box_collider(scene, e)) {
                float sx = (t->scale.x > 0) ? t->scale.x : 1.0f;
                float sy = (t->scale.y > 0) ? t->scale.y : 1.0f;
                float sz = (t->scale.z > 0) ? t->scale.z : 1.0f;
                jce_vec3 half = jce_v3(0.5f * sx, 0.5f * sy, 0.5f * sz);
                jce_debug_draw_box(center, half, q, col_box);
            }
            if (jce_scene_has_sphere_collider(scene, e)) {
                float r = (t->scale.x > 0) ? t->scale.x * 0.5f : 0.5f;
                jce_debug_draw_sphere(center, r, col_sphere);
            }
            if (jce_scene_has_character_controller(scene, e)) {
                jce_debug_draw_capsule(center, 0.35f, 0.55f, q, col_capsule);
            }
        }
        jce_debug_draw_flush(base, g.renderer);
    }
}

void jce_editor_game_render_set_module(const JceGameModule *mod)
{
    if (g.module == mod) return;
    /* If a module is currently inited, tear it down first. */
    if (g.module_inited && g.module && g.module->exit)
        g.module->exit(g.module->user_data);
    g.module_inited = false;
    g.module = mod;
}

const JceGameModule *jce_editor_game_render_get_module(void)
{
    return g.module ? g.module : jce_game_module_default();
}
