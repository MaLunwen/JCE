/*
 * jce_editor_game_render.cpp  Implementation for the embedded "Game View"
 * renderer (free-fly FPS camera, dedicated off-screen target, shares the
 * engine scene renderer instance with the main scene viewport).
 */

#include "scene/jce_editor_game_render.h"
#include "scene/jce_editor_scene_render.h"
#include "scene/jce_editor_scene_asset_cache.h"  /* finalize() — GPU-upload async loads */
#include "scene/jce_editor_viewport_common.h"    /* plumbing shared with Scene View */
#include "gizmo/jce_gizmo_compound_collider.h"   /* draw fitted compound colliders */
#include "core/jce_editor_state.h"

#include "jce_scene_content_context.h"   /* project content roots (C++ linkage) */
#include "core/jce_editor_project.h"

extern "C" {
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_console.h>  /* r.taa cvar query for game-view TAA */
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_taa.h>
#include <jce/renderer/jce_views.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/scene/jce_vcam_system.h>
#include <jce/os/core/jce_filesystem.h>          /* active FS policy */
#include <jce/os/platform/jce_input.h>
#include <jce/runtime/jce_game_module.h>
#include <jce/application/jce_runtime.h>  /* UI widget → script dispatch in Play */
extern void jce_game_module_set_active_scene(JceScene *scene);
}

#include <cmath>
#include <cstdlib>   /* getenv — JCE_KPI_OCCLUSION_LOG forensic toggle */

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
    uint16_t               render_width      = 0;
    uint16_t               render_height     = 0;

    /* GPU-query occlusion culler (Play / game-view).  The game view shares the
     * ENGINE scene renderer with the scene view but renders in its own pass with
     * its own camera, so it needs its OWN culler instance (per-entity visibility
     * history must not be shared with the scene-view camera).  Uses a DISTINCT
     * proxy view id (253) from the scene-view culler (which keeps the config
     * default, 252) so the two never clobber each other's depth-only proxy pass
     * when both panels render in the same bgfx frame.  NULL when
     * JCE_DISABLE_OCCLUSION is set or hardware queries are unsupported (silent
     * always-visible fallback). */
    JceOcclusionCuller    *occlusion_culler  = nullptr;

    /* TAA (game view): own jitter/history state so the first-person view also
     * benefits from temporal AA with per-object motion-vector de-ghosting.
     * Mirrors the scene-view TAA driver but against g.postfx. */
    JceTaaState            taa_state         = {};
    jce_mat4               taa_prev_view     = {};
    jce_mat4               taa_prev_proj     = {};
    bool                   taa_prev_valid    = false;

    /* Active game module + lifecycle bookkeeping. */
    const JceGameModule   *module            = nullptr;
    bool                   module_inited     = false;
    int                    last_play_state   = 0;  /* JcePlayState */
    JceServices            svc               = {};

    /* ECS-UI (Canvas) renderer — draws Canvas/UIImage/UIText/UIButton on
     * top of the game view and hit-tests the pointer. */
    JceUICanvas           *ui_canvas         = nullptr;
    JceUIPointer           ui_pointer        = {};
    /* OS text-input enable mirror: true while an InputField is focused so we
     * only toggle jce_window_start/stop_text_input on a focus-state change. */
    bool                   ui_text_input_on  = false;
    /* The live Play-mode runtime (set by jce_editor_game_render_set_play_runtime
     * while Play is active, NULL in edit mode).  When set, drained canvas UI
     * events are dispatched to its gameplay script VM — exactly like the shipped
     * default-main app loop — so in-editor Play exercises UI on_click /
     * on_value_changed / on_submit handlers, not just visual feedback. */
    JceRuntime            *play_runtime      = nullptr;
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

    /* ECS-UI Canvas renderer (Screen-Space Overlay over the game view). */
    g.ui_canvas = jce_ui_canvas_create(renderer, pak);
    if (!g.ui_canvas)
        LOG_WARN(LOG_TAG, "[init] failed to create UI canvas renderer");

    /* Create a dedicated PostFX pipeline for the game view so it can
     * apply the same effects as the scene view without conflicting on
     * bgfx view IDs.  Uses view IDs starting at GAME_VIEW_BASE +
     * JCE_VIEW_POST_BASE (= 100) — well clear of the scene view's range
     * (20-29) and of the editor overlay (50) and ImGui (250). */
    g.postfx = jce_postfx_create(jce_allocator_default(), 1, 1);
    if (g.postfx) {
        jce_postfx_set_view_base(g.postfx,
                                 (uint16_t)(GAME_VIEW_BASE +
                                     JCE_EDITOR_VP_POSTFX_BASE_OFFSET));
        if (pak) {
            jce_postfx_load_shaders(g.postfx, pak);
        }
    } else {
        LOG_WARN(LOG_TAG, "[init] failed to create game view PostFX pipeline");
    }

    /* Occlusion culler (Play / game-view): GPU-query two-pass coherence culling,
     * mirroring the scene-view (jce_editor_scene_render.cpp).  Activating it here
     * is what makes Play / shipped-equivalent rendering skip fully-hidden
     * geometry in a dense world (previously occlusion ran ONLY in the editor
     * scene-view).  Opt-OUT via JCE_DISABLE_OCCLUSION=1 for A/B measurement and
     * as a safety hatch (mirrors JCE_DISABLE_WCACHE / JCE_STREAM_SYNC).  Degrades
     * silently to always-visible when hardware queries are unsupported. */
    if (jce_editor_viewport_occlusion_disabled()) {
        LOG_INFO(LOG_TAG,
                 "[init] JCE_DISABLE_OCCLUSION set — game-view occlusion culling OFF");
    } else {
        /* Proxy view 253 — distinct from the scene-view culler's so both can
         * run in the same bgfx frame without clobbering each other. */
        g.occlusion_culler =
            jce_editor_viewport_create_occlusion_culler(renderer, 253);
        if (!g.occlusion_culler)
            LOG_WARN(LOG_TAG,
                     "[init] occlusion culler creation failed (game-view culling disabled)");
    }

    return true;
}

void jce_editor_game_render_reset_occlusion(void)
{
    /* See jce_editor_scene_render_reset_occlusion: the game-view culler is
     * keyed by entity id and must drop its slots whenever the ECS world is
     * recreated (Play-stop snapshot restore), or recreated entities inherit
     * dead cull verdicts and the query pool leaks across Play sessions. */
    if (g.occlusion_culler)
        jce_occlusion_culler_reset(g.occlusion_culler);
    /* The shared engine scene renderer's entity-keyed environment caches
     * (foliage/grass/water/canopies) go stale on the same trigger — see
     * jce_editor_scene_render_reset_occlusion.  Idempotent when the
     * scene-view reset already ran. */
    jce_scene_renderer_reset_entity_caches(jce_editor_get_scene_renderer());
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
    if (g.occlusion_culler) {
        jce_occlusion_culler_destroy(g.occlusion_culler);
        g.occlusion_culler = nullptr;
    }
    if (g.ui_canvas) { jce_ui_canvas_destroy(g.ui_canvas); g.ui_canvas = nullptr; }
    if (g.bridge) { jce_offscreen_target_destroy(g.bridge); g.bridge = nullptr; }
    if (g.camera) { jce_camera_destroy(g.camera); g.camera = nullptr; }
    if (g.postfx) { jce_postfx_destroy(g.postfx); g.postfx = nullptr; }
    g.postfx_output_tex = UINT16_MAX;
    g.renderer = nullptr;
    g.window   = nullptr;
    g.initialized = false;
}

/* Captured mouse-button bitmask (bit per button: 0=left,1=right,2=mid,...).
 * Tracked while the Game View holds FPS capture, because the editor's ImGui
 * event pump intentionally STOPS forwarding mouse buttons to ImGui during
 * capture ("clicks belong to the game", jce_editor.cpp).  Reading ImGui's
 * button state during Play would therefore stick: the button-up that ends the
 * capture-acquiring click arrives AFTER capture engaged and is swallowed, so
 * ImGui's MouseLeft stays down forever.  The game-view player-input gather
 * reads THIS (fed the up/down even during capture) instead of ImGui. */
static uint32_t s_captured_mouse_buttons = 0;

bool jce_editor_game_render_set_mouse_capture(bool capture)
{
    if (!g.window) { g.mouse_captured = false; return false; }
    if (capture == g.mouse_captured) return g.mouse_captured;
    jce_window_set_relative_mouse_mode(g.window, capture);
    /* Hard-grab so the cursor cannot escape the editor window even if the
     * relative-mouse mode is loosened by the OS during focus transitions. */
    jce_window_set_mouse_grab(g.window, capture);
    g.mouse_captured = capture;
    if (!capture) s_captured_mouse_buttons = 0;  /* never strand a held button across toggles */
    return g.mouse_captured;
}

bool jce_editor_game_render_is_mouse_captured(void)
{
    return g.mouse_captured;
}

void jce_editor_game_render_set_ui_pointer(float x, float y,
                                           bool down, bool valid)
{
    g.ui_pointer.x     = x;
    g.ui_pointer.y     = y;
    g.ui_pointer.down  = down;
    g.ui_pointer.valid = valid;
}

/* Hand the game renderer the live Play-mode runtime (or NULL to detach on
 * stop).  While set, canvas UI events drained after the overlay render are
 * dispatched to its gameplay script VM (UIButton on_click / widget
 * on_value_changed / InputField on_submit).  Called by jce_editor_play.cpp. */
void jce_editor_game_render_set_play_runtime(JceRuntime *rt)
{
    g.play_runtime = rt;
}

/* Forward a UTF-8 text-input chunk into the focused ECS-UI InputField.  The
 * Game View panel sources these from ImGui's per-frame character queue (its
 * natural event source, exactly like the pointer above) only while the panel
 * is hovered/focused in Play.  Fire-and-forget: a no-op when no field is
 * focused, and it never consumes events the rest of the editor needs. */
void jce_editor_game_render_text_input(const char *utf8)
{
    if (g.ui_canvas) jce_ui_canvas_text_input(g.ui_canvas, utf8);
}

/* Forward an editing key (JCE_KEY_*) into the focused InputField. */
void jce_editor_game_render_key_edit(int scancode, unsigned short mod)
{
    if (g.ui_canvas) jce_ui_canvas_key_edit(g.ui_canvas, scancode, (uint16_t)mod);
}

/* Forward a mouse-wheel delta into the ECS-UI ScrollView under the pointer.
 * Fire-and-forget (no-op when no scroll view is hovered). */
void jce_editor_game_render_scroll(float dx, float dy)
{
    if (g.ui_canvas) jce_ui_canvas_scroll(g.ui_canvas, dx, dy);
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

/* Captured mouse-button state (set from the editor event pump while the Game
 * View holds FPS capture; see s_captured_mouse_buttons).  btn: 0=left, 1=right,
 * 2=middle, 3=x1, 4=x2 (mirrors the ImGui button indices). */
void jce_editor_game_render_push_mouse_button(int btn, bool down)
{
    if (btn < 0 || btn > 31) return;
    if (down) s_captured_mouse_buttons |=  (1u << btn);
    else      s_captured_mouse_buttons &= ~(1u << btn);
}

bool jce_editor_game_render_mouse_button(int btn)
{
    if (btn < 0 || btn > 31) return false;
    return (s_captured_mouse_buttons & (1u << btn)) != 0;
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

bool jce_editor_game_render_screenshot(const char *path)
{
    if (!g.initialized || !g.bridge || !path || !path[0] ||
        g.render_width == 0 || g.render_height == 0) {
        return false;
    }

    return jce_editor_viewport_screenshot_submit(
        g.bridge, jce_offscreen_target_get_view_id(g.bridge),
        g.postfx_output_tex, g.render_width, g.render_height, path);
}

int jce_editor_game_render_capture_poll(void)
{
    return jce_renderer_readback_capture_poll();
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
        /* Re-point the module at the CURRENT scene every frame.  The active
         * scene was pinned at module-init (rising edge above); if the editor
         * scene has since been swapped or recreated (scene load / project
         * switch mid-Play), that pinned pointer now dangles and mod->draw ->
         * jce_scene_update would dereference freed memory (ACCESS_VIOLATION at
         * jce_scene.c scene_drop_world_cache_frame).  `scene` is this frame's
         * jce_state_get_scene(), already null-checked above, so it is valid. */
        jce_game_module_set_active_scene(scene);
        if (mod->update) mod->update(frame_dt, mod->user_data);
        if (mod->draw)   mod->draw(&g.svc, mod->user_data);
    }

    /* WYSIWYG with the standalone runtime: during Play, if the scene has a
     * playable character (CharacterController → runtime player position), drive
     * the Game View camera with the SAME third-person follow as jce_default_main
     * (jce_camera_third_person_follow) instead of the free-fly debug camera, so
     * editor-Play and the shipped runtime frame the scene identically.  Falls
     * back to free-fly when not playing or the scene has no character. */
    if (play_active && g.play_runtime) {
        jce_vec3 player_pos;
        if (jce_runtime_get_player_position(g.play_runtime, &player_pos))
            jce_camera_third_person_follow(g.camera, player_pos);
    }

    /* Resolve the authored scene camera after the Play runtime has stepped and
     * before matrices are built.  The panel used to evaluate it after this
     * render call, which made editor Play exactly one visual frame late. */
    if (play_active) {
        JceVcamOutput output;
        bool has_vcam = false;
        jce_vcam_system_evaluate(scene, frame_dt, &output, &has_vcam);
        if (has_vcam) {
            jce_camera_set_position(
                g.camera, jce_v3(output.position[0], output.position[1],
                                 output.position[2]));
            jce_camera_look_at(
                g.camera, jce_v3(output.target[0], output.target[1],
                                 output.target[2]));
            jce_camera_set_fov(g.camera, output.fov_deg);
        }
    } else {
        jce_vcam_system_reset();
    }

    float aspect = (float)width / (float)height;
    jce_mat4 view = jce_camera_view(g.camera);
    jce_mat4 proj = jce_camera_proj(g.camera, aspect, g.homogeneous_depth);

    /* ── TAA (game view) ──────────────────────────────────────────────
     * When r.taa is on, sub-pixel-jitter THIS view's colour-pass projection and
     * arm g.postfx's TAA resolve against per-object motion vectors produced by
     * the shared scene renderer's velocity pre-pass.  Jitter only the colour
     * pass; the motion vectors use the CLEAN (un-jittered) matrices. */
    static const JceCvar *s_cv_taa = jce_cvar_find("r.taa");
    bool game_taa_on = s_cv_taa ? jce_cvar_get_bool(s_cv_taa) : false;
    jce_mat4 color_proj = proj;
    if (game_taa_on && g.postfx) {
        jce_taa_advance(&g.taa_state, width, height);
        jce_taa_apply_jitter(&color_proj, g.taa_state.current_jitter);

        jce_mat4 view_proj = jce_m4_multiply(&proj, &view);
        jce_mat4 inv_vp    = jce_m4_inverse(&view_proj);
        jce_mat4 prev_vp   = g.taa_prev_valid
            ? jce_m4_multiply(&g.taa_prev_proj, &g.taa_prev_view)
            : view_proj;
        jce_postfx_set_taa_matrices(g.postfx, &inv_vp, &prev_vp);
        /* Authorable TAA tuning (0 = engine defaults: feedback 0.9, clamps 1.0). */
        float taa_fb = 0.9f, taa_lc = 1.0f, taa_mc = 1.0f;
        if (scene) {
            const JceSceneRenderingSettings *rs = jce_scene_get_rendering_settings(scene);
            if (rs) {
                if (rs->taa_feedback     > 0.0f) taa_fb = rs->taa_feedback;
                if (rs->taa_luma_clamp   > 0.0f) taa_lc = rs->taa_luma_clamp;
                if (rs->taa_motion_clamp > 0.0f) taa_mc = rs->taa_motion_clamp;
            }
        }
        jce_postfx_set_taa(g.postfx, true, taa_fb, taa_lc, taa_mc);
        /* Request the shared renderer write a per-object/per-bone velocity
         * buffer this frame (the game view is the sole renderer when the Scene
         * tab is hidden; when both are visible the scene path's prev-state is
         * shared, a documented v1 limitation). */
        jce_scene_renderer_set_taa_velocity_enabled(engine_sr, true);
    } else if (g.postfx) {
        jce_postfx_set_taa(g.postfx, false, 0.9f, 1.0f, 1.0f);
    }

    if (!jce_offscreen_target_prepare(
            g.bridge, width, height,
            view.raw[0], color_proj.raw[0],   /* jittered when game TAA on */
            0x202028FFu,
            "EditorGame")) {
        return;
    }
    g.render_width = static_cast<uint16_t>(width);
    g.render_height = static_cast<uint16_t>(height);

    /* GPU-upload any completed async mesh/texture loads before the engine
     * renderer queries them this frame. Previously only the Scene View did
     * this (jce_editor_scene_render_frame), so a fresh launch with only the
     * Game View visible showed missing meshes/textures until you clicked the
     * Scene tab once. */
    jce_editor_scene_asset_cache_finalize();

    /* Same story, one step further along: the particle-descriptor and canvas
     * font roots are process-wide statics that only the Scene View was
     * setting. With just the Game View shown, *.particles.json never resolved
     * and every emitter came up empty -- an engine plume that burned in the
     * standalone runtime and not in the editor. Set them here too, from the
     * same project paths, so the Game View is self-sufficient. */
    {
        const bool isolated =
            jce_fs_get_active_policy() == JCE_FS_ACTIVE_ISOLATED;
        const JceProject *proj = jce_editor_project_get();
        const JceEditorSceneContentPaths paths =
            jce_editor_scene_content_paths(
                isolated,
                proj ? proj->project_root : nullptr,
                proj ? proj->source_assets : nullptr,
                proj ? proj->cooked_assets : nullptr);
        const char *root = paths.particle_asset_root.empty()
                               ? nullptr
                               : paths.particle_asset_root.c_str();
        jce_scene_particles_set_asset_root(root);
        jce_ui_canvas_set_asset_root(root);
    }

    /* Game view always uses the shipped "shaded" pipeline — no debug
     * wireframe overrides, all engine features (shadows, IBL, postfx)
     * enabled by default. */
    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    cfg.view_mode = JCE_SCENE_VIEW_SHADED;

    /* Bridge FBO, panel resolution, volumetric fog, SSR and GI — the plumbing
     * that must be identical on both editor render paths (see
     * jce_editor_viewport_common.h). */
    jce_editor_viewport_apply_shared_config(&cfg, g.bridge, width, height,
                                            jce_state_get_scene());

    cfg.viewport_id = 0;   /* Game viewport slot (Scene = 1): own TAA prev camera */

    /* The editor's Game View is meant to preview "what the player
     * would see", so always draw the skybox / sprites and never inject
     * the editor grid overlay. */
    cfg.on_after_sky    = nullptr;
    cfg.on_after_sky_ud = nullptr;

    /* Two-pass GPU-query occlusion culling (Play / game-view).  Mirrors the
     * scene-view: the renderer drives begin_frame / entity_visible / submit_query
     * internally from this pointer.  NULL when JCE_DISABLE_OCCLUSION is set or
     * hardware queries are unsupported (always-visible fallback). */
    cfg.occlusion_culler = g.occlusion_culler;

    /* Ambient override from the editor Lighting panel — applied on both
     * viewports so panel changes reach whichever one renders. */
    jce_editor_viewport_apply_ambient_override(engine_sr);

    /* Focus-bounded entity collection ("draw distance"): only entities within
     * cull_radius (horizontal) of the play camera are collected, so every
     * downstream renderer pass becomes O(near) instead of O(all entities) —
     * what keeps a full-loaded big world playable in the Game View / Play.
     * Focus = the play (free-fly/FPS) camera position. */
    {
        jce_vec3 cam_pos = jce_camera_get_position(g.camera);
        cfg.cull_focus_enabled = true;
        cfg.cull_focus_x = cam_pos.x;
        cfg.cull_focus_y = cam_pos.y;
        cfg.cull_focus_z = cam_pos.z;
        cfg.cull_radius  = 900.0f;
    }

    uint16_t base = jce_offscreen_target_get_view_id(g.bridge);
    /* Pass real dt only while actually PLAYING — paused/stopped states
     * should freeze animation, matching Unity's Game View semantics. */
    float render_dt = (play_state == 1) ? frame_dt : 0.0f;
    jce_scene_renderer_render(engine_sr, scene, g.camera, base,
                              render_dt, &cfg);

    /* Forensic occlusion KPI (JCE_KPI_OCCLUSION_LOG=1) for the GAME-view
     * offscreen path — parity with the scene-view log so a headless run can
     * confirm occlusion is live (occluded>0) in the game-view bridge FBO too. */
    {
        static int s_occ_log = -1;
        if (s_occ_log < 0)
            s_occ_log = (getenv("JCE_KPI_OCCLUSION_LOG") != nullptr) ? 1 : 0;
        if (s_occ_log) {
            static unsigned s_gocc_frame = 0;
            if ((s_gocc_frame++ % 60u) == 0u) {
                JceSceneOcclusionStats ocs = {};
                jce_scene_renderer_get_occlusion_stats(engine_sr, &ocs);
                LOG_INFO(LOG_TAG,
                    "[occ-kpi game] mode=%s tested=%u visible=%u occluded=%u "
                    "warm_up=%u no_result=%u", ocs.enabled ? "ON" : "OFF",
                    ocs.total, ocs.visible, ocs.occluded, ocs.warm_up,
                    ocs.no_result);
            }
        }
    }

    /* Composite volumetric fog (base+16) and SSR (base+19) back over the bridge
     * — same passes, same offsets as the scene view. */
    jce_editor_viewport_composite_fog_ssr(engine_sr, g.bridge, base,
                                          cfg.fog_enabled);
    jce_editor_viewport_apply_fullscreen_effects(
        engine_sr, scene, g.camera, g.bridge, base, 0,
        width, height, render_dt);

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
            /* Replay the shared pipeline's authorable look onto the game view's
             * own pipeline.  Single conversion — see
             * jce_editor_viewport_postfx_mirror for what it does and does NOT
             * copy (view base / chain size / TAA stay per-viewport). */
            jce_editor_viewport_postfx_mirror(g.postfx, shared_pfx);

                /* The "HDR target => force tone mapping" guard used to sit here.  It is
                 * gone on purpose, and NOT because it was in the wrong place.
                 *
                 * 2e5174e3 moved it after jce_scene_renderer_render so the
                 * renderer's per-frame rewrite of scene_rendering->postfx_enabled[]
                 * could no longer stomp it.  That made it live -- and live, it
                 * overrides what a scene explicitly authored.  Its own rationale
                 * was that an untonemapped HDR target reads washed out; bisected
                 * against real content, forcing it ON is what washes out:
                 *
                 *   caged_kingdom/graveyard, same camera, PNG size as a proxy
                 *     ec933eb8 (guard dead)  1,126,840 -> correct
                 *     2e5174e3 (guard live)  1,494,787 -> washed out
                 *
                 * and the split across projects is exactly postfx.tonemap:
                 * elemental_serenity and space author it TRUE (forcing it is a
                 * no-op there, and they always looked right), while caged_kingdom
                 * and street_demo author it FALSE and had their exposure and
                 * lights tuned that way.  A new scene defaults to false, which is
                 * why every newly created scene came up white too.
                 *
                 * Silently overriding an explicit authored value to protect
                 * against a hypothetical is the wrong trade: honour the scene.  A
                 * scene that wants tone mapping says so. */

            bool any_effect = jce_editor_viewport_postfx_any_effect(shared_pfx);

            /* TAA (game view): bind the shared renderer's per-object velocity
             * buffer as the motion source so animated/skinned geometry stops
             * ghosting, then ensure the chain runs even if TAA is the only
             * effect (TAA is not a JCE_POSTFX_COUNT effect, so any_effect may be
             * false). */
            if (game_taa_on) {
                JceTextureHandle vt = { UINT16_MAX };
                vt.idx = jce_scene_renderer_get_velocity_texture(engine_sr);
                jce_postfx_set_taa_motion_tex(g.postfx, vt);
            }

            if (any_effect || game_taa_on) {
                jce_postfx_resize(g.postfx, width, height);
                JceTextureHandle game_color = { UINT16_MAX };
                JceTextureHandle game_depth = { UINT16_MAX };
                game_color.idx = jce_offscreen_target_get_color_texture(g.bridge);
                game_depth.idx = jce_offscreen_target_get_depth_texture(g.bridge);
                if (jce_gfx_texture_valid(game_color)) {
                    jce_postfx_apply(g.postfx, game_color, game_depth);
                    JceTextureHandle out = jce_postfx_get_output(g.postfx);
                    if (jce_gfx_texture_valid(out))
                        g.postfx_output_tex = out.idx;
                }
            }
        }
    }

    /* Fold the tone-mapped PostFX output back into the bridge so the canvas UI
     * (drawn next) lands AFTER post-fx, matching the standalone game
     * pixel-for-pixel (modulo content). */
    const bool postfx_composited = jce_editor_viewport_composite_postfx(
        g.bridge, GAME_VIEW_BASE, g.postfx_output_tex, width, height);
    if (postfx_composited)
        g.postfx_output_tex = UINT16_MAX; /* bridge is now the final frame */

    /* ── ECS-UI (Canvas) overlay ────────────────────────────────────
     * Draw Canvas/UIImage/UIText/UIButton on top of the rendered game
     * view, into the offscreen target's framebuffer.  With PostFX active
     * the overlay draws after the composite pass above (crisp, un-tonemapped
     * UI, matching the runtime); without PostFX it draws right after the
     * scene color (base) and fog composite (base+16). */
    if (g.ui_canvas && scene) {
        uint16_t ui_view = jce_editor_viewport_ui_overlay_view(GAME_VIEW_BASE,
                                                               postfx_composited);
        uint16_t ui_fb   = jce_offscreen_target_get_frame_buffer(g.bridge);
        const JceUIPointer *ptr = g.ui_pointer.valid ? &g.ui_pointer : nullptr;
        jce_ui_canvas_render(g.ui_canvas, scene, ui_view, ui_fb,
                             (float)width, (float)height, ptr, render_dt);

        /* OS text-input follows InputField focus: start SDL text input only
         * when a field becomes focused, stop it when focus clears.  Toggle on
         * the edge so we don't spam the platform layer every frame. */
        if (g.window) {
            bool want = jce_ui_canvas_focused_input(g.ui_canvas) != 0;
            if (want != g.ui_text_input_on) {
                if (want) jce_window_start_text_input(g.window);
                else      jce_window_stop_text_input(g.window);
                g.ui_text_input_on = want;
            }
        }

        /* In-editor Play: drain the canvas UI events recorded this frame and
         * fire each widget's authored handler through the Play runtime's script
         * VM — the same drain the shipped default-main app loop performs, so
         * Play-testing exercises real UI gameplay (button clicks, slider/toggle/
         * dropdown value changes, input-field edits + submit), not just visuals.
         * No-op in edit mode (play_runtime NULL). */
        if (g.play_runtime) {
            uint64_t clicked = jce_ui_canvas_last_clicked(g.ui_canvas);
            if (clicked) {
                JceUIButtonComponent *bt =
                    jce_scene_get_ui_button(scene, (JceEntity)clicked);
                if (bt)
                    jce_runtime_dispatch_ui_click(g.play_runtime, clicked,
                                                  bt->on_click_handler);
            }
            uint64_t vc = jce_ui_canvas_last_value_changed(g.ui_canvas);
            if (vc) jce_runtime_dispatch_ui_value_changed(g.play_runtime, vc);
            uint64_t tc = jce_ui_canvas_last_text_changed(g.ui_canvas);
            if (tc) jce_runtime_dispatch_ui_text_changed(g.play_runtime, tc);
            uint64_t sub = jce_ui_canvas_last_submitted(g.ui_canvas);
            if (sub) jce_runtime_dispatch_ui_submit(g.play_runtime, sub);
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
            jce_quat q = t->rotation;
            /* Honor each collider's size/center/radius (matches the physics +
             * the scene-view overlay) — was a fixed 0.5*scale cube before. */
            jce_vec3 sca = jce_v3_abs_safe_scale(t->scale);
            float sx = sca.x, sy = sca.y, sz = sca.z;
            if (jce_scene_has_box_collider(scene, e)) {
                JceBoxColliderComponent *bc = jce_scene_get_box_collider(scene, e);
                jce_vec3 ofs = bc ? jce_v3(bc->center[0], bc->center[1], bc->center[2])
                                  : jce_v3(0, 0, 0);
                float bx = bc ? bc->size[0] : 1.0f, by = bc ? bc->size[1] : 1.0f,
                      bz = bc ? bc->size[2] : 1.0f;
                jce_vec3 half = jce_v3(0.5f * bx * sx, 0.5f * by * sy, 0.5f * bz * sz);
                jce_vec3 c = jce_v3_add(t->position, jce_q_rotate(q,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
                jce_debug_draw_box(c, half, q, col_box);
            }
            if (jce_scene_has_sphere_collider(scene, e)) {
                JceSphereColliderComponent *sc = jce_scene_get_sphere_collider(scene, e);
                jce_vec3 ofs = sc ? jce_v3(sc->center[0], sc->center[1], sc->center[2])
                                  : jce_v3(0, 0, 0);
                float r = ((sc && sc->radius > 0.0f) ? sc->radius : 0.5f)
                          * fmaxf(sx, fmaxf(sy, sz));
                jce_vec3 c = jce_v3_add(t->position, jce_q_rotate(q,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
                jce_debug_draw_sphere(c, r, col_sphere);
            }
            if (jce_scene_has_capsule_collider(scene, e)) {
                JceCapsuleColliderComponent *cc = jce_scene_get_capsule_collider(scene, e);
                jce_vec3 ofs = cc ? jce_v3(cc->center[0], cc->center[1], cc->center[2])
                                  : jce_v3(0, 0, 0);
                float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * fmaxf(sx, sz);
                float h = ((cc && cc->height > 0.0f) ? cc->height : 1.0f) * sy;
                float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
                jce_vec3 c = jce_v3_add(t->position, jce_q_rotate(q,
                                  jce_v3(ofs.x * sx, ofs.y * sy, ofs.z * sz)));
                jce_debug_draw_capsule(c, r, hh, q, col_capsule);
            }
            if (jce_scene_has_character_controller(scene, e)) {
                JceCharacterControllerComponent *cc =
                    jce_scene_get_character_controller(scene, e);
                float r = ((cc && cc->radius > 0.0f) ? cc->radius : 0.3f) * fmaxf(sx, sz);
                float h = ((cc && cc->height > 0.0f) ? cc->height : 1.6f) * sy;
                float hh = 0.5f * fmaxf(0.0f, h - 2.0f * r);
                jce_vec3 cc_c = t->position; cc_c.y += hh + r;  /* feet -> capsule center */
                jce_debug_draw_capsule(cc_c, r, hh, q, col_capsule);
            }
            if (jce_scene_has_compound_collider(scene, e)) {
                JceCompoundColliderComponent *cpc =
                    jce_scene_get_compound_collider(scene, e);
                if (cpc) jce_gizmo_compound_collider_draw_from_component(
                             scene, e, cpc, 0xFF00FF00u, 0);  /* overlay: bounds, green */
            }
            if (jce_scene_has_mesh_collider(scene, e)) {
                JceMeshColliderComponent *msc =
                    jce_scene_get_mesh_collider(scene, e);
                if (msc) jce_gizmo_mesh_collider_draw_from_component(
                             scene, e, msc, 0xFF00FF00u, 0);  /* overlay: bounds, green */
            }
        }
        jce_debug_draw_flush(base, g.renderer);
    }

    /* TAA end-of-frame (game view): stash the UN-JITTERED camera for next
     * frame's reproject and disable TAA on g.postfx so it never leaks into a
     * later apply.  Mirrors jce_scene_renderer_taa_end_frame. */
    if (game_taa_on) {
        g.taa_prev_view  = view;
        g.taa_prev_proj  = proj;
        g.taa_prev_valid = true;
    } else {
        g.taa_prev_valid = false;
    }
    if (g.postfx)
        jce_postfx_set_taa(g.postfx, false, 0.9f, 1.0f, 1.0f);
    /* Clear the shared renderer's velocity request so it isn't reused by a
     * later scene/pick render this frame. */
    jce_scene_renderer_set_taa_velocity_enabled(engine_sr, false);
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
