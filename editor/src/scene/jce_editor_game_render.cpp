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
#include "core/jce_editor_viewport_trace.h"
#include "core/jce_editor_frame_observation.h"

#include "jce_scene_content_context.h"   /* project content roots (C++ linkage) */
#include "core/jce_editor_project.h"

extern "C" {
#include <jce/os/core/jce_allocator.h>
#include <jce/middleware/scene/jce_scene_probe_capture.h>
#include <jce/middleware/scene/jce_scene_camera.h>
#include <jce/os/core/jce_console.h>  /* r.taa cvar query for game-view TAA */
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_render_pipeline.h>  /* scene_extent: one answer, shared with the shipped runtime */
#include <jce/os/core/jce_timer.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_occlusion_culler.h>
#include <jce/renderer/jce_impostor.h>   /* modal bake owns the frame */
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_taa.h>
#include <jce/renderer/jce_views.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/application/jce_rml_scene_overlay.h>
#include <jce/middleware/scene/jce_vcam_system.h>
#include <jce/os/core/jce_filesystem.h>          /* active FS policy */
#include <jce/os/platform/jce_input.h>
#include <jce/runtime/jce_game_module.h>
#include <jce/application/jce_runtime.h>  /* UI widget → script dispatch in Play */
}

#include <cmath>
#include <cstdlib>   /* getenv — JCE_KPI_OCCLUSION_LOG forensic toggle */
#include <string>

#define LOG_TAG "game_render"

namespace {

struct GameRenderState {
    bool                   initialized       = false;
    bool                   homogeneous_depth = false;
    bool                   mouse_captured    = false;
    JceRenderer           *renderer          = nullptr;
    JceWindow             *window            = nullptr;
    JceOffscreenTarget    *bridge            = nullptr;
    JceCamera             *camera            = nullptr;  /* stopped preview */
    JceCamera             *play_camera       = nullptr;  /* Play-owned; never aliases preview */
    JcePostFXPipeline     *postfx            = nullptr;  /* game-view own pipeline */
    uint16_t               postfx_output_tex = UINT16_MAX;
    uint16_t               render_width      = 0;
    uint16_t               render_height     = 0;
    /* The renderer frame in which render_width/height were last written, and
     * whether they have EVER been written.  A SIZE ALONE IS NOT ENOUGH.
     *
     * The panel only calls jce_editor_game_render_frame() when ImGui::Begin
     * returns true, which it does not for a background tab -- and nothing
     * clears the extent when the panel stops drawing.  So a Game View that
     * was in front once and is now behind another tab leaves a perfectly
     * plausible 1380x898 sitting here forever, describing a frame nobody is
     * drawing.  A size guard cannot tell that apart from a live panel; only
     * "was this written for the frame we are capturing" can. */
    uint32_t               extent_frame      = 0u;
    bool                   extent_written    = false;
    bool                   stats_frame_completed = false; /* CPU-only HUD freshness. */

    /* GPU-query occlusion culler (Play / game-view).  The game view shares the
     * ENGINE scene renderer with the scene view but renders in its own pass with
     * its own camera, so it needs its OWN culler instance (per-entity visibility
     * history must not be shared with the scene-view camera).  Uses a DISTINCT
     * proxy view id (253) from the scene-view culler (which keeps the config
     * default, 252) so the two never clobber each other's depth-only proxy pass
     * when both panels render in the same bgfx frame.  NULL when
     * occlusion is disabled or hardware queries are unsupported (silent
     * always-visible fallback). */
    JceOcclusionCuller    *occlusion_culler  = nullptr;

    /* TAA (game view): own jitter/history state so the first-person view also
     * benefits from temporal AA with per-object motion-vector de-ghosting.
     * Mirrors the scene-view TAA driver but against g.postfx. */
    JceTaaState            taa_state         = {};
    jce_mat4               taa_prev_view     = {};
    jce_mat4               taa_prev_proj     = {};
    bool                   taa_prev_valid    = false;
    bool                   camera_cut_pending = true;

    /* ECS-UI (Canvas) renderer — draws Canvas/UIImage/UIText/UIButton on
     * top of the game view and hit-tests the pointer. */
    JceUICanvas           *ui_canvas         = nullptr;
    const JcePakArchive    *rml_pak           = nullptr;
    JceRmlSceneOverlay     rml_overlay        = {};
    std::string            rml_assets_root;
    bool                   rml_checked        = false;
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
JceEditorFrameObservation s_game_frame_observation;

/* Use a view ID well above the editor scene + shadow + post FX range.
 * Engine scene renderer expects shadow views at base+10..+13 and post
 * stages around base+20, so picking 80 keeps us out of every other slot
 * the editor uses. */
/* The base itself now lives in jce_editor_viewport_common.h, beside the
 * Scene View's, so the view-id spacing assertions can see both. */
constexpr uint16_t GAME_VIEW_BASE = JCE_EDITOR_GAME_VIEW_BASE;

JceCameraDesc game_default_camera_desc_internal(void)
{
    JceCameraDesc desc = {};
    desc.mode       = JCE_CAMERA_PERSPECTIVE;
    desc.position   = jce_v3(0.0f, 1.7f, 6.0f);
    desc.target     = jce_v3(0.0f, 1.5f, 0.0f);
    desc.up         = jce_v3(0.0f, 1.0f, 0.0f);
    desc.fov_deg    = 60.0f;
    desc.near_plane = 0.05f;
    desc.far_plane  = 1000.0f;
    return desc;
}

JceCamera *game_display_camera_internal(void)
{
    return g.play_camera ? g.play_camera : g.camera;
}

void game_reset_camera_internal(void)
{
    JceCamera *camera = game_display_camera_internal();
    if (!camera) return;
    jce_camera_set_position(camera, jce_v3(0.0f, 1.7f, 6.0f));
    jce_camera_set_target  (camera, jce_v3(0.0f, 1.5f, 0.0f));
    jce_camera_set_fov     (camera, 60.0f);
    jce_camera_set_near_far(camera, 0.05f, 1000.0f);
}

} /* anon namespace */

/* A NAMED FUNCTION WITH THE EXPLICIT CONVENTION, not a lambda.
 *
 * JceAutoCaptureFn carries JCE_CALL, which is `__cdecl` on Windows.  A
 * capture-less lambda converts to a function pointer with the compiler's
 * DEFAULT convention, and on x64 that happens to be the same -- so a lambda
 * would work here and silently not on x86, where __cdecl and the default
 * differ and the mismatch is a corrupted stack rather than a compile error.
 *
 * AND C LANGUAGE LINKAGE, which is the second half of the same point.
 * JceAutoCaptureFn is declared inside jce_renderer.h's JCE_EXTERN_C_BEGIN,
 * so it is a pointer to a C-LINKAGE function.  Assigning a C++-linkage one
 * to it is formally ill-formed -- every compiler in this tree accepts it,
 * which is exactly why it would go unnoticed until one did not.
 *
 * Declining is normal rather than a failure: no Game View panel means no
 * offscreen target, and the renderer then falls back to the backbuffer,
 * which is exactly what this editor did before the hook existed. */
extern "C" {
static bool JCE_CALL game_view_auto_capture(void *user, const char *path)
{
    (void)user;
    return jce_editor_game_render_screenshot(path, nullptr);
}
}

bool jce_editor_game_render_init(JceRenderer *renderer, JceWindow *window,
                                 const JcePakArchive *pak)
{
    if (g.initialized) return true;
    g.stats_frame_completed = false;
    if (!renderer) return false;

    g.renderer = renderer;
    g.window   = window;
    g.rml_pak  = pak;
    JceGfxCaps caps = jce_gfx_caps();
    g.homogeneous_depth = caps.homogeneous_depth;

    const JceCameraDesc desc = game_default_camera_desc_internal();
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

    /* THE AUTOMATED CAPTURE (JCE_CAPTURE_FRAME) NOW PHOTOGRAPHS THE GAME
     * VIEW, not the backbuffer.
     *
     * The backbuffer in this process carries the ImGui layer, so an
     * automated capture of it contains the editor rather than the project --
     * and the profiler panel draws a CLOCK into it: measured, 53,089 of
     * 3,911,680 pixels differ between two runs of one input digest, every one
     * inside rows 1080..1526, with the rendered scene above byte-identical.
     * That band is why an editor-sourced run directory cannot be
     * byte-reproducible, and app.run says so in its result today.
     *
     * jce_editor_game_render_screenshot reads the texture the Game View
     * DISPLAYS -- postfx output when there is one, else the bridge's colour
     * attachment, with the y-flip that belongs to whichever it is.  That is
     * the distinction a framebuffer-index API could not express, and is why
     * this is a hook. */
    jce_renderer_set_auto_capture_hook(game_view_auto_capture, nullptr);

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
     * scene-view).  Opt-IN via JCE_ENABLE_OCCLUSION=1 for A/B measurement and
     * as a safety hatch (mirrors JCE_DISABLE_WCACHE / JCE_STREAM_SYNC).  Degrades
     * silently to always-visible when hardware queries are unsupported. */
    if (!jce_editor_viewport_occlusion_enabled()) {
        LOG_INFO(LOG_TAG,
                 "[init] %s occlusion culling OFF (%s)", "game-view",
                 jce_editor_viewport_occlusion_forced_by_env()
                     ? "JCE_ENABLE_OCCLUSION=0"
                     : "off by default; enable with Preferences > Viewport > occlusion culling, or JCE_ENABLE_OCCLUSION=1");
    } else {
        /* Proxy view 253 — distinct from the scene-view culler's so both can
         * run in the same bgfx frame without clobbering each other. */
        g.occlusion_culler =
            jce_editor_viewport_create_occlusion_culler(
                renderer, JCE_VIEW_EDITOR_GAME_OCCLUSION);
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
    g.stats_frame_completed = false;
    if (g.window) {
        jce_window_set_mouse_rect(g.window, 0, 0, 0, 0);
    }
    /* CLEAR THE HOOK FIRST, before anything it reads is destroyed.
     *
     * The renderer holds a pointer to game_view_auto_capture, which reads
     * g.bridge and g.postfx_output_tex.  A capture requested after this
     * function frees them would run the hook against a destroyed target --
     * and a hook that outlives its state does not fail loudly, it reads
     * whatever is at that address now. */
    jce_renderer_set_auto_capture_hook(nullptr, nullptr);

    if (g.mouse_captured && g.window) {
        jce_window_set_relative_mouse_mode(g.window, false);
        jce_window_set_mouse_grab(g.window, false);
        g.mouse_captured = false;
    }
    if (g.occlusion_culler) {
        jce_occlusion_culler_destroy(g.occlusion_culler);
        g.occlusion_culler = nullptr;
    }
    jce_rml_scene_overlay_close(&g.rml_overlay);
    g.rml_assets_root.clear();
    g.rml_checked = false;
    g.rml_pak = nullptr;
    if (g.ui_canvas) { jce_ui_canvas_destroy(g.ui_canvas); g.ui_canvas = nullptr; }
    if (g.bridge) { jce_offscreen_target_destroy(g.bridge); g.bridge = nullptr; }
    jce_editor_game_render_end_play_camera();
    g.play_runtime = nullptr;
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
    if (!capture) s_captured_mouse_buttons = 0; /* idempotent Stop release */
    if (!g.window) { g.mouse_captured = false; return false; }
    if (capture) {
        uint32_t flags = 0;
        /* Dock focus does not imply host input focus. Recheck even when the
         * previous frame captured, so a hidden or inactive host releases it. */
        capture = jce_window_get_flags(g.window, &flags) &&
                  (flags & JCE_WINDOW_INPUT_FOCUS) != 0 &&
                  (flags & (JCE_WINDOW_HIDDEN | JCE_WINDOW_MINIMIZED |
                            JCE_WINDOW_NOT_FOCUSABLE)) == 0;
    }
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

/* Camera allocation participates in Play admission, independently of whether
 * the Game panel draws. Keeping the preview object untouched preserves its
 * complete projection, orthographic extent and rolled pose without a partial
 * snapshot built from getters. All calls run on the Editor owner thread. */
bool jce_editor_game_render_begin_play_camera(void)
{
    if (g.play_camera) return true;
    const JceCameraDesc desc = game_default_camera_desc_internal();
    JceCamera *camera = g.camera ? jce_camera_clone(g.camera)
                                 : jce_camera_create(&desc);
    if (!camera) {
        LOG_ERROR(LOG_TAG, "failed to create Play camera");
        return false;
    }
    g.play_camera = camera;
    g.taa_prev_valid = false;
    g.camera_cut_pending = true;
    return true;
}

void jce_editor_game_render_end_play_camera(void)
{
    JceCamera *camera = g.play_camera;
    g.play_camera = nullptr;
    if (camera) jce_camera_destroy(camera);
    g.taa_prev_valid = false;
    g.camera_cut_pending = true;
}

/* Hand the game renderer the live Play-mode runtime (or NULL to detach on
 * stop).  While set, canvas UI events drained after the overlay render are
 * dispatched to its gameplay script VM (UIButton on_click / widget
 * on_value_changed / InputField on_submit).  Called by jce_editor_play.cpp. */
void jce_editor_game_render_set_play_runtime(JceRuntime *rt)
{
    g.play_runtime = rt;
}

bool jce_editor_game_render_apply_cursor(void)
{
    return g.play_runtime && g.ui_pointer.valid &&
        jce_runtime_apply_cursor(g.play_runtime);
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
    return game_display_camera_internal();
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

/* Below this in EITHER dimension the Game View panel is collapsed, not a view,
 * and its target is not a picture of anything.
 *
 * MEASURED, and it is why this constant exists rather than a bare `!= 0`.
 * The first version of the automated-capture hook tested only for zero, and
 * an `app.run` through the editor -- a hidden window, so the panel gets
 * whatever ImGui's minimum content region is -- captured a 16x16, 328-byte
 * PNG.  The run before the hook shipped captured 2560x1528 at 970 KB.  So the
 * hook replaced a full frame that had a clock in the bottom band with an image
 * containing nothing at all, which is strictly worse: the clock made the frame
 * non-reproducible, the 16x16 makes it non-evidence.
 *
 * NOTHING CAUGHT IT.  The capture was non-blank, written to the right path,
 * reported as a success, and app.run's own content guard returned
 * `has_content: null` with "guard could not run: sampled no pixels -- the crop
 * window is outside the image (16x16)" -- a guard that cannot run reports
 * neither pass nor fail, so the gate stayed green over a capture of nothing.
 * 64 is not a tuned number: it is an order of magnitude below any real panel
 * and an order of magnitude above the degenerate case that was observed. */
#define JCE_EDITOR_GAME_VIEW_MIN_CAPTURE_PX 64

/* How many renderer frames old the Game View's extent may be and still
 * describe the frame being captured.
 *
 * MEASURED AT 0, AND MY FIRST EXPLANATION FOR IT WAS WRONG.  I reasoned
 * that the capture site (jce_renderer.c:1954, inside jce_renderer_end_frame)
 * runs AFTER bgfx_frame() advances s_bgfx_frame_index at :1878, so the age
 * would always be at least 1 and a strict equality would decline every time.
 * Then I forced this constant to 0 and rebuilt: the capture still fired.
 * :1878 is the tail of jce_renderer_begin_frame -- the 2D/fallback path the
 * editor does not take.  The editor goes through begin_frame_3d, so the only
 * advance is at :2037, AFTER the capture.  Age is 0.
 *
 * 1, not 0, only so the 2D begin_frame path -- where the index does advance
 * once before end_frame -- is also covered.  Both numbers come from
 * measurement, not from taste.  A panel that has stopped drawing exceeds
 * this on the very next frame and then diverges without bound, so the window
 * separates one frame of pipeline from a dead panel by orders of magnitude. */
#define JCE_EDITOR_GAME_VIEW_EXTENT_MAX_AGE 1

bool jce_editor_game_render_screenshot(
    const char *path, JceEditorGameScreenshotFrame *frame)
{
    if (!g.initialized || !g.bridge || !path || !path[0] ||
        g.render_width == 0 || g.render_height == 0) {
        return false;
    }
    if (g.render_width  < JCE_EDITOR_GAME_VIEW_MIN_CAPTURE_PX ||
        g.render_height < JCE_EDITOR_GAME_VIEW_MIN_CAPTURE_PX) {
        /* DECLINE, which is a normal answer: the caller falls back to the
         * backbuffer, and a backbuffer frame carrying the editor is at least
         * a photograph of something.  Logged because "the automated capture
         * quietly became the other path" is exactly the kind of change that
         * is invisible until someone compares two run directories. */
        LOG_INFO(LOG_TAG, "[capture] Game View is %ux%u -- below %d px, "
                          "declining so the capture falls back to the "
                          "backbuffer rather than photographing a collapsed "
                          "panel",
                 (unsigned)g.render_width, (unsigned)g.render_height,
                 JCE_EDITOR_GAME_VIEW_MIN_CAPTURE_PX);
        return false;
    }
    /* IS THE EXTENT ABOUT THE FRAME WE ARE CAPTURING?
     *
     * The size guard above catches a panel that has NEVER drawn at a usable
     * size.  It cannot catch the other half: a Game View that WAS in front,
     * wrote 1380x898 here, and is now the background tab.  ImGui::Begin
     * returns false for a background tab, so the panel body is skipped and
     * jce_editor_game_render_frame() stops being called -- and nothing clears
     * the extent.  The stale value is large, plausible, and describes a frame
     * nobody is drawing, so the capture would photograph a frozen target and
     * report success.  A stale-but-plausible reading and a live one are
     * indistinguishable by size; only the frame they belong to separates
     * them.
     *
     * THE WINDOW IS MEASURED, NOT GUESSED -- see
     * JCE_EDITOR_GAME_VIEW_EXTENT_MAX_AGE, whose comment records that the
     * observed age on the editor's path is 0 and that my first reasoning for
     * why it could not be 0 was wrong. */
    const uint32_t now = g.renderer ? jce_renderer_get_frame_index(g.renderer)
                                    : 0u;
    /* Unsigned: a wrapped or reordered counter must not read as "fresh". */
    const uint32_t age = (now >= g.extent_frame) ? (now - g.extent_frame)
                                                 : UINT32_MAX;
    if (!g.extent_written || age > JCE_EDITOR_GAME_VIEW_EXTENT_MAX_AGE) {
        LOG_INFO(LOG_TAG, "[capture] Game View extent is from frame %u, this "
                          "is frame %u (age %u > %d, written=%d) -- the panel "
                          "is not drawing, declining so the capture falls "
                          "back to the backbuffer rather than photographing a "
                          "frozen target",
                 (unsigned)g.extent_frame, (unsigned)now, (unsigned)age,
                 JCE_EDITOR_GAME_VIEW_EXTENT_MAX_AGE, (int)g.extent_written);
        return false;
    }

    if (!jce_editor_viewport_screenshot_submit(
            g.bridge, jce_offscreen_target_get_view_id(g.bridge),
            g.postfx_output_tex, g.render_width, g.render_height, path,
            JCE_EDITOR_SCREENSHOT_GAME))
        return false;
    if (frame) {
        frame->source_frame = g.extent_frame;
        frame->submit_frame = now;
    }
    return true;
}

int jce_editor_game_render_capture_poll(void)
{
    return jce_editor_viewport_capture_poll(JCE_EDITOR_SCREENSHOT_GAME);
}

/* Dimensions are resolved by the shared pipeline extent owner before this
 * function. The panel maps ImGui points into these Canvas/FBO pixels and
 * publishes the same extent to Play before input or first script startup.
 * Never apply render_scale a second time here. */
void jce_editor_game_render_observation_begin(bool enabled)
{
    s_game_frame_observation.begin(enabled);
}

void jce_editor_game_render_observation_policy(uint32_t reason)
{
    s_game_frame_observation.policy(static_cast<JceEditorViewportPolicyReason>(reason));
}

void jce_editor_game_render_observation_request(uint32_t width, uint32_t height)
{
    s_game_frame_observation.request(width, height);
}

void jce_editor_game_render_observation_image(uint16_t texture)
{
    if (!s_game_frame_observation.enabled() || texture == UINT16_MAX) return;
    uint32_t allocated_width = 0, allocated_height = 0;
    const bool bridge_extent = jce_offscreen_target_get_extent(
        g.bridge, &allocated_width, &allocated_height);
    uint32_t display_width = 0, display_height = 0;
    if (texture == jce_offscreen_target_get_color_texture(g.bridge)) {
        if (bridge_extent) {
            display_width = allocated_width;
            display_height = allocated_height;
        }
    } else if (g.postfx && texture == g.postfx_output_tex) {
        JcePostFXOutputTarget output{};
        output.struct_size = sizeof(output);
        output.version = JCE_POSTFX_OUTPUT_TARGET_VERSION;
        if (jce_postfx_get_output_target(g.postfx, &output) &&
            output.kind == JCE_POSTFX_OUTPUT_OWNED && output.texture.idx == texture) {
            display_width = output.width;
            display_height = output.height;
        }
    }
    s_game_frame_observation.image(texture, allocated_width, allocated_height,
                                   display_width, display_height);
}

bool jce_editor_game_render_observation_copy(JceFrameViewportObservation *out)
{
    return s_game_frame_observation.copy(out);
}

bool jce_editor_game_render_get_committed_extent(uint32_t *width, uint32_t *height)
{
    if (width) *width = 0;
    if (height) *height = 0;
    if (!width || !height || !g.initialized || !g.renderer || !g.bridge ||
        !g.stats_frame_completed || !g.extent_written ||
        g.extent_frame != jce_renderer_get_frame_index(g.renderer))
        return false;
    return jce_offscreen_target_get_extent(g.bridge, width, height);
}

void jce_editor_game_render_resolve_extent(uint32_t width, uint32_t height,
                                          uint32_t *out_w, uint32_t *out_h)
{
    jce_render_pipeline_scene_extent(width, height, out_w, out_h);
}

void jce_editor_game_render_frame(uint32_t width, uint32_t height)
{
    g.stats_frame_completed = false;
    s_game_frame_observation.render_begin(width, height);
    JceEditorViewportRenderTrace render_trace("editor.game.render", "editor.game.render.reason",
        "editor.game.render.admitted");
    if (!g.initialized || !g.renderer) {
        s_game_frame_observation.refuse(JceEditorViewportRenderReason::Unavailable);
        render_trace.reason(JceEditorViewportRenderReason::Unavailable); return;
    }
    if (width == 0 || height == 0) {
        s_game_frame_observation.refuse(JceEditorViewportRenderReason::EmptyExtent);
        render_trace.reason(JceEditorViewportRenderReason::EmptyExtent); return;
    }

    /* THE size of the offscreen chain, asked once.  See the note above the
     * function: the panel size is for the ImGui image and nothing else. */
    const uint32_t render_w = width, render_h = height;

    /* A modal impostor bake owns the frame's view ids (127..227), which is
     * inside this viewport's range under any base that fits in 256 views.
     * Yield rather than share: the panel shows its last frame for the two or
     * three frames a bake takes.  That is what an Unreal HLOD build and a
     * Unity lightmap bake do with the live viewport, and the alternative is
     * the failure this whole band of work exists to remove -- two owners on
     * one view id and one of them silently producing nothing.
     *
     * The bake is driven from the Inspector / the QA harness, not from here,
     * so skipping this render does not stall it. */
    if (jce_impostor_bake_in_flight()) {
        s_game_frame_observation.refuse(JceEditorViewportRenderReason::ImpostorBusy);
        render_trace.reason(JceEditorViewportRenderReason::ImpostorBusy); return;
    }
    /* ...and to a probe capture, for the same reason: it renders the scene
     * from a viewport base id while it runs. The admitted host pump drives it;
     * this viewport also yields on the pump's completion/failure frame. */
    if (jce_editor_scene_render_probe_busy()) {
        s_game_frame_observation.refuse(JceEditorViewportRenderReason::ProbeBusy);
        render_trace.reason(JceEditorViewportRenderReason::ProbeBusy); return;
    }

    JceSceneRenderer *engine_sr = jce_editor_get_scene_renderer();
    JceScene         *scene     = jce_state_get_scene();
    const int play_state = (int)jce_state_get_play_state();
    const bool play_active = play_state == 1 || play_state == 2;
    JceCamera *camera = game_display_camera_internal();
    /* A failed/missing Play admission must never fall back to mutating preview. */
    if (!engine_sr || !scene || !camera || !g.bridge ||
        (play_active && !g.play_camera)) {
        s_game_frame_observation.refuse(JceEditorViewportRenderReason::MissingSceneOwner);
        render_trace.reason(JceEditorViewportRenderReason::MissingSceneOwner); return;
    }
    render_trace.admitted();
    const float frame_dt = jce_editor_play_render_delta();
    /* Module drawing is view work; init/update/exit belong to Play, which
     * continues when this viewport is hidden or clipped. */
    jce_editor_play_draw_module();

    /* WYSIWYG with the standalone runtime: during Play, if the scene has a
     * playable character (CharacterController → runtime player position), drive
     * the Game View camera with the SAME third-person follow as jce_default_main
     * (jce_camera_third_person_follow) instead of the free-fly debug camera, so
     * editor-Play and the shipped runtime frame the scene identically.  Falls
     * back to free-fly when not playing or the scene has no character. */
    if (play_active && g.play_runtime) {
        jce_vec3 player_pos;
        if (jce_runtime_get_player_position(g.play_runtime, &player_pos))
            jce_camera_third_person_follow(camera, player_pos);
    }

    /* Resolve the authored scene camera after the Play runtime has stepped and
     * before matrices are built.  The panel used to evaluate it after this
     * render call, which made editor Play exactly one visual frame late. */
    if (play_active) {
        /* THE AUTHORED CAMERA FIRST, which nothing here used to apply.
         *
         * This block only ever asked the VCam system, so a scene whose view
         * is an ordinary authored Camera component -- no VirtualCamera
         * anywhere -- was rendered by Play from game_reset_camera_internal's
         * default: eye level at (0, 1.7, 6) looking at the origin, 60-degree
         * perspective.  The shipped game meanwhile called
         * jce_scene_camera_apply_primary() and used the camera the scene
         * authored.
         *
         * So the two halves rendered the same scene from two different
         * cameras, and the logic was IDENTICAL while doing it -- measured on
         * a top-down board game: the per-tick trace matched the shipped exe
         * tick for tick, and 99.989% of the pixels differed, because Play was
         * looking at the board edge-on from ground level.
         *
         * The VCam override below still wins, exactly as it does in the
         * runtime: authored pose first, virtual camera on top of it. */
        JceSceneCameraPose cpose;
        const JceSceneCameraResolveResult cres =
            jce_scene_camera_apply_primary(scene, camera, &cpose);
        /* Say it ONCE when the scene cannot supply a camera.  Silently
         * keeping the default is how this whole defect stayed invisible:
         * Play renders something plausible from a camera nobody authored,
         * and "my scene looks wrong in Play" has no line anywhere to
         * attach itself to.  Each distinct outcome is reported once so a
         * scene switch reports again without spamming a frame loop. */
        {
            static JceSceneCameraResolveResult s_said =
                JCE_SCENE_CAMERA_RESOLVE_OK;
            if (cres != JCE_SCENE_CAMERA_RESOLVE_OK && cres != s_said) {
                s_said = cres;
                const char *why =
                    cres == JCE_SCENE_CAMERA_RESOLVE_NOT_FOUND
                        ? "no entity has a primary Camera"
                    : cres == JCE_SCENE_CAMERA_RESOLVE_AMBIGUOUS
                        ? "more than one primary Camera"
                    : cres == JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE
                        ? "the primary Camera's transform is not a valid pose"
                        : "invalid argument";
                LOG_WARN(LOG_TAG,
                         "Play is using the default camera: %s. The shipped "
                         "game resolves the same way, so it will look the "
                         "same there -- this is the scene, not the editor.",
                         why);
            }
        }

        JceVcamOutput output;
        bool has_vcam = false;
        jce_vcam_system_evaluate(scene, frame_dt, &output, &has_vcam);
        if (has_vcam) {
            jce_camera_set_position(
                camera, jce_v3(output.position[0], output.position[1],
                                 output.position[2]));
            jce_camera_look_at(
                camera, jce_v3(output.target[0], output.target[1],
                                 output.target[2]));
            jce_camera_set_fov(camera, output.fov_deg);
        }
    } else {
        jce_vcam_system_reset();
    }

    float aspect = (float)width / (float)height;
    jce_mat4 view = jce_camera_view(camera);
    jce_mat4 proj = jce_camera_proj(camera, aspect, g.homogeneous_depth);

    /* Camera.clearFlags, resolved ONCE here because both halves of the answer
     * are needed and they are needed at different moments: what the target
     * clears is decided by the prepare below, and whether the sky draws is
     * decided by the render config further down.  Resolving twice would let a
     * scene edit between the two land in one answer and not the other. */
    JceSceneCameraPose base_pose;
    memset(&base_pose, 0, sizeof base_pose);
    bool have_base_pose = false;
    {
        JceScene *cscene = jce_state_get_scene();
        if (cscene && jce_scene_camera_resolve_primary(cscene, &base_pose) ==
                          JCE_SCENE_CAMERA_RESOLVE_OK)
            have_base_pose = true;
    }
    bool base_keep_color = false, base_keep_depth = false;
    if (have_base_pose)
        jce_scene_camera_clear_keeps(base_pose.clear_mode,
                                     &base_keep_color, &base_keep_depth);

    /* ── TAA (game view) ──────────────────────────────────────────────
     * When r.taa is on, sub-pixel-jitter THIS view's colour-pass projection and
     * arm g.postfx's TAA resolve against per-object motion vectors produced by
     * the shared scene renderer's velocity pre-pass.  Jitter only the colour
     * pass; the motion vectors use the CLEAN (un-jittered) matrices. */
    /* TAA, on the SAME condition the shipped runtime uses.
     *
     * Between 2026-09-22 and this commit this was pinned off, because the
     * runtime could not resolve TAA at all: jce_scene_renderer_taa_begin_frame()
     * had exactly one caller in the tree and it was the editor's Scene view, so
     * this panel -- whose entire job is to show what the player will see -- was
     * showing a temporally anti-aliased image no build produced.  Measured then:
     * forcing it off on both sides took a static frame from 12.50% of pixels
     * differing to 2.98%.
     *
     * jce_default_main.inc.h now drives TAA the way jce_scene_renderer.h
     * documents, so r.taa is once more the right condition on both sides and
     * the pin is gone.  examples/snake_seven/parity.py asserts the two hosts
     * agree about it, so a future divergence is a named FAIL and not a pixel
     * count. */
    const bool game_taa_on =
        jce_scene_renderer_taa_requested(engine_sr) &&
        jce_postfx_taa_supported();
    jce_mat4 color_proj = proj;
    if (game_taa_on && g.postfx) {
        jce_taa_advance(&g.taa_state, render_w, render_h);
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
        /* A camera cut must not blend the previous camera's colour history.
         * Keep this pending across hidden/skipped views until an actual apply. */
        jce_postfx_set_taa(g.postfx, true,
            g.camera_cut_pending ? 0.0f : taa_fb, taa_lc, taa_mc);
        /* Request the shared renderer write a per-object/per-bone velocity
         * buffer this frame (the game view is the sole renderer when the Scene
         * tab is hidden; when both are visible the scene path's prev-state is
         * shared, a documented v1 limitation). */
        jce_scene_renderer_set_taa_velocity_enabled(engine_sr, true);
    } else if (g.postfx) {
        jce_postfx_set_taa(g.postfx, false, 0.9f, 1.0f, 1.0f);
    }

    /* The Game view renders at the SHIPPED resolution, not at the panel's:
     * jce_render_pipeline_scene_extent composes the authored render_scale with
     * the low-tier pixel budget exactly as the standalone runtime does, so a
     * designer dragging that slider sees what the player will see and the two
     * cannot disagree.  The Scene view deliberately does NOT do this -- its
     * picking and gizmos are calibrated to the panel, and Unity draws the same
     * line: URP's Render Scale applies to the Game view only.
     *
     * The texture is presented stretched to the panel either way, and the only
     * consumer of the recorded extent is the screenshot path, so a scaled
     * target changes no input mapping. */
    if (!jce_offscreen_target_prepare_keep(
            g.bridge, static_cast<int>(render_w), static_cast<int>(render_h),
            view.raw[0], color_proj.raw[0],   /* jittered when game TAA on */
            /* THE SAME CLEAR COLOUR THE RUNTIME USES, not an editor tone.
             *
             * This was 0x202028FF -- a dark editor slate.  The shipped game
             * clears a scene camera's view to 0x000000FF
             * (jce_sr_camera_stack.c), so the Game View, whose entire job is
             * to show what the player will see, showed it on a different
             * background from the player's.
             *
             * Measured on a board that does not fill the frame: 23% of all
             * differing pixels between a Game View capture and the shipped
             * game's frame were this one colour, everywhere the scene does
             * not draw. */
            0x000000FFu,
            base_keep_color, base_keep_depth,
            "EditorGame")) {
        s_game_frame_observation.refuse(JceEditorViewportRenderReason::TargetRefused);
        render_trace.reason(JceEditorViewportRenderReason::TargetRefused);
        return;
    }
    if (s_game_frame_observation.enabled()) {
        uint32_t allocated_w = 0, allocated_h = 0;
        (void)jce_offscreen_target_get_extent(g.bridge, &allocated_w, &allocated_h);
        s_game_frame_observation.prepared(allocated_w, allocated_h);
    }
    g.render_width = static_cast<uint16_t>(render_w);
    g.render_height = static_cast<uint16_t>(render_h);
    /* Stamp the frame this extent belongs to.  Read by the screenshot path to
     * refuse an extent left behind by a panel that has stopped drawing. */
    g.extent_frame = g.renderer ? jce_renderer_get_frame_index(g.renderer) : 0u;
    g.extent_written = true;

    /* GPU-upload any completed async mesh/texture loads before the engine
     * renderer queries them this frame. Previously only the Scene View did
     * this (jce_editor_scene_render_frame), so a fresh launch with only the
     * Game View visible showed missing meshes/textures until you clicked the
     * Scene tab once. */
    jce_editor_scene_asset_cache_finalize();
    if (!jce_scene_renderer_prepare_resources(engine_sr, scene)) {
        s_game_frame_observation.refuse(JceEditorViewportRenderReason::ResourcesRefused);
        render_trace.reason(JceEditorViewportRenderReason::ResourcesRefused); return;
    }

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
                proj ? proj->cooked_assets : nullptr,
                jce_editor_scene_content_root());
        const char *root = paths.particle_asset_root.empty()
                               ? nullptr
                               : paths.particle_asset_root.c_str();
        jce_scene_particles_set_asset_root(root);
        jce_ui_canvas_set_asset_root(root);
        /* The project's fallback face for any UIText with an empty fontPath.
         * It used to be reachable only from a project's own main(), so the
         * editor rendered such text in the engine's built-in font while the
         * shipped exe rendered it in the project's -- different outlines,
         * different advance widths, therefore different line breaks. */
        jce_ui_canvas_set_default_font(proj ? proj->ui_default_font : nullptr);
        jce_ui_canvas_set_font_fallbacks(proj ? proj->ui_font_fallbacks
                                            : nullptr);

        /* Optional DOCUMENT UI: projects opt in with assets/ui/hud.rml and
         * hud.bindings. Native RmlUi renders on the Game View FBO, sharing the
         * already-authoritative scene state (UIText/UIProgressBar values).
         * This remains editor-owned; no CK-specific code enters engine. */
        const std::string next_root = isolated ? std::string() : paths.particle_asset_root;
        if (g.rml_assets_root != next_root) {
            jce_rml_scene_overlay_close(&g.rml_overlay);
            g.rml_assets_root = next_root;
            g.rml_checked = false;
        }
        if (!g.rml_checked && !g.rml_assets_root.empty()) {
            g.rml_checked = true;
            const std::string doc_path = g.rml_assets_root + "/ui/hud.rml";
            const std::string map_path = g.rml_assets_root + "/ui/hud.bindings";
            if (jce_fs_host_exists_file(doc_path.c_str()) &&
                jce_fs_host_exists_file(map_path.c_str())) {
                uint64_t ndoc = 0, nmap = 0;
                void *rml = jce_fs_host_read_all(doc_path.c_str(), &ndoc);
                void *map = jce_fs_host_read_all(map_path.c_str(), &nmap);
                if (rml && map && ndoc <= 1024u * 1024u && nmap <= 8192u &&
                    jce_rml_scene_overlay_open(&g.rml_overlay,g.renderer,
                        (JcePakArchive *)g.rml_pak,(const char *)rml,(size_t)ndoc,
                        (const char *)map,(size_t)nmap,render_w,render_h)) {
                    LOG_INFO(LOG_TAG,"[rml] scene document UI loaded from project assets");
                } else {
                    LOG_WARN(LOG_TAG,"[rml] UI document/bindings refused; check assets/ui");
                }
                jce_fs_buffer_free(rml);
                jce_fs_buffer_free(map);
            }
        }
    }

    /* Game view always uses the shipped "shaded" pipeline — no debug
     * wireframe overrides, all engine features (shadows, IBL, postfx)
     * enabled by default. */
    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    cfg.view_mode = JCE_SCENE_VIEW_SHADED;

    /* Camera culling mask (Unity's Camera.cullingMask), from the scene's
     * authored primary camera -- the same source the shipped drop-in main
     * reads, so what Play shows is what the build renders.
     *
     * GAME VIEW ONLY.  It is deliberately NOT in
     * jce_editor_viewport_apply_shared_config, which the Scene View also
     * calls: hiding masked objects from the Scene View would make them
     * unselectable and un-editable, and Unity keeps them visible there.  That
     * is why this sits at the call site instead of the shared helper whose
     * comment otherwise demands both viewports agree.
     *
     * Re-resolved per frame rather than cached because the Inspector can
     * change the mask between frames and the designer expects to see it; the
     * scan is skipped entirely when no scene is open. */
    static JceSceneCameraPose s_overlay_pose[JCE_VIEW_SR_CAMERA_OVERLAY_MAX];
    static uint8_t             s_overlay_count = 0;
    {
        JceScene *gscene = jce_state_get_scene();
        cfg.camera_culling_mask = 0u;
        /* CAMERA STACKING.  Resolved BEFORE the base render because the count
         * decides the view ORDER: the overlay views have to be named ahead of
         * everything that reads the scene colour they draw into.  Re-resolved
         * per frame for the same reason the mask is -- the Inspector can add
         * an overlay between frames. */
        s_overlay_count = 0;
        if (gscene &&
            jce_scene_camera_resolve_stack(gscene, s_overlay_pose,
                                           JCE_VIEW_SR_CAMERA_OVERLAY_MAX,
                                           &s_overlay_count) !=
            JCE_SCENE_CAMERA_RESOLVE_OK)
            s_overlay_count = 0;   /* ambiguous / degenerate: draw the base alone */
        cfg.camera_overlay_count = s_overlay_count;
        if (have_base_pose) {
            {
                const JceSceneCameraPose &gpose = base_pose;
                cfg.camera_culling_mask = gpose.culling_mask;
                /* Camera.clearFlags, from the SAME resolve.  The Game View is
                 * the runtime-parity surface, so it answers this the way the
                 * shipped exe does; the Scene View keeps the Show > Skybox
                 * flag (jce_editor_scene_render.cpp), which is a viewing aid
                 * and not something the scene authored. */
                cfg.draw_skybox =
                    jce_scene_camera_clear_draws_skybox(gpose.clear_mode);
            }
        }
    }

    /* Bridge FBO, panel resolution, volumetric fog, SSR and GI — the plumbing
     * that must be identical on both editor render paths (see
     * jce_editor_viewport_common.h). */
    jce_editor_viewport_apply_shared_config(&cfg, g.bridge, render_w, render_h,
                                            jce_state_get_scene());

    cfg.viewport_id = 0;   /* Game viewport slot (Scene = 1): own TAA prev camera */

    /* The editor's Game View is meant to preview "what the player
     * would see", so always draw the skybox / sprites and never inject
     * the editor grid overlay. */
    cfg.on_after_sky    = nullptr;
    cfg.on_after_sky_ud = nullptr;

    /* Two-pass GPU-query occlusion culling (Play / game-view).  Mirrors the
     * scene-view: the renderer drives begin_frame / entity_visible / submit_query
     * internally from this pointer.  NULL when occlusion is disabled or
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
        jce_vec3 cam_pos = jce_camera_get_position(camera);
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
    /* Before the main render -- see the note in the Scene View. */
    jce_scene_renderer_render_planar_reflection(engine_sr, scene, camera,
                                                 render_dt);
    /* THE OVERLAY CAMERAS, SUBMITTED FIRST.  They are DRAWN last -- bgfx runs
     * views in the order the view-order pass set, and base+64..66 sit after the
     * base camera's whole run -- but they are SUBMITTED first, because each one
     * is a second scene render and whichever render runs last is the one whose
     * per-frame renderer state survives into the composites below.  Submitting
     * them after the base render left jce_editor_viewport_composite_fog_ssr and
     * the fullscreen-effect pass reading an overlay's state instead of the base
     * camera's: an overlay masked to an empty layer, clearing nothing and
     * drawing nothing, still moved 47% of the pixels around the silhouettes.
     * Same rule, same reason, as the planar reflection's "FIRST" note in the
     * shipped runtime.
     *
     * One reusable JceCamera: an overlay's pose is applied to it and consumed
     * inside the call, so there is no per-overlay lifetime. */
    if (s_overlay_count > 0) {
        static JceCamera *s_overlay_cam = nullptr;
        if (!s_overlay_cam) {
            JceCameraDesc d;
            memset(&d, 0, sizeof d);
            d.position = jce_v3(0.0f, 0.0f, 0.0f);
            d.target   = jce_v3(0.0f, 0.0f, -1.0f);
            d.up       = jce_v3(0.0f, 1.0f, 0.0f);
            d.fov_deg  = 60.0f;
            d.near_plane = 0.1f;
            d.far_plane  = 1000.0f;
            s_overlay_cam = jce_camera_create(&d);
        }
        for (uint8_t oi = 0; oi < s_overlay_count && s_overlay_cam; ++oi) {
            if (!jce_scene_camera_apply_pose(s_overlay_cam, &s_overlay_pose[oi]))
                continue;
            JceSceneRenderConfig ocfg = cfg;
            ocfg.camera_culling_mask = s_overlay_pose[oi].culling_mask;
            ocfg.camera_clear_mode   = s_overlay_pose[oi].clear_mode;
            jce_scene_renderer_render_camera_overlay(
                engine_sr, scene, s_overlay_cam, base, oi, render_dt, &ocfg);
        }
    }

    jce_scene_renderer_render(engine_sr, scene, camera, base,
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
        engine_sr, scene, camera, g.bridge, base, 0,
        render_w, render_h, render_dt);

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
                jce_postfx_resize(g.postfx, render_w, render_h);
                JceTextureHandle game_color = { UINT16_MAX };
                JceTextureHandle game_depth = { UINT16_MAX };
                game_color.idx = jce_offscreen_target_get_color_texture(g.bridge);
                game_depth.idx = jce_offscreen_target_get_depth_texture(g.bridge);
                if (jce_gfx_texture_valid(game_color)) {
                    jce_postfx_apply(g.postfx, game_color, game_depth);
                    g.camera_cut_pending = false;
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
    /* Same declaration as the Scene View -- see the comment there. */
    jce_editor_viewport_claim_view_band("editor-game-view", GAME_VIEW_BASE);

    uint16_t postfx_ui_fb = UINT16_MAX;
    const bool postfx_overlay = g.ui_canvas && jce_editor_game_postfx_overlay_target(
        g.bridge, g.postfx, g.postfx_output_tex, render_w, render_h,
        g.renderer, scene, jce_state_get_show_physics_debug(), &postfx_ui_fb);
    const bool postfx_composited = !postfx_overlay && jce_editor_viewport_composite_postfx(
        g.bridge, GAME_VIEW_BASE, g.postfx_output_tex, render_w, render_h,
        g.renderer, scene, g.ui_canvas != nullptr, jce_state_get_show_physics_debug());
    if (postfx_composited)
        g.postfx_output_tex = UINT16_MAX; /* bridge is now the final frame */

    /* ── ECS-UI (Canvas) overlay ────────────────────────────────────
     * Draw Canvas/UIImage/UIText/UIButton on top of the rendered game
     * view, into the admitted current PostFX FBO or the bridge. With PostFX active
     * the overlay draws after the composite pass above (crisp, un-tonemapped
     * UI, matching the runtime); without PostFX it draws right after the
     * scene color (base) and fog composite (base+16). */
    if (g.ui_canvas && scene) {
        uint16_t ui_view = jce_editor_viewport_ui_overlay_view(GAME_VIEW_BASE,
                                                               postfx_composited);
        uint16_t ui_fb = postfx_overlay ? postfx_ui_fb
            : jce_offscreen_target_get_frame_buffer(g.bridge);
        const JceUIPointer *ptr = g.ui_pointer.valid ? &g.ui_pointer : nullptr;
        /* render_w/render_h, not the panel size: the canvas is rasterised
         * INTO the admitted full-size target, so its logical extent must match or the
         * whole UI lands in a corner of a scaled target.
         *
         * This is the one place the Game view cannot match the shipped
         * runtime, and it is worth naming rather than discovering: the
         * standalone host draws the ECS-UI straight to the BACKBUFFER at
         * native resolution, precisely so text stays crisp while the scene
         * scales.  The editor presents through a single texture, so here the
         * UI shares the scene's resolution -- at render_scale below 1.0 the
         * Game view's text is softer than the player's will be. */
        jce_ui_canvas_render(g.ui_canvas, scene, ui_view, ui_fb,
                             (float)render_w, (float)render_h, ptr, render_dt);
        if (g.rml_overlay.ui) {
            jce_rml_scene_overlay_frame(&g.rml_overlay,scene,ui_view,
                render_w,render_h,render_dt,g.ui_pointer.valid,
                g.ui_pointer.x,g.ui_pointer.y,g.ui_pointer.down,nullptr);
        }

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
         * VM.  This is now LITERALLY the same code the shipped default-main app
         * loop runs (jce_runtime_dispatch_ui_events), not a second transcription
         * of the same four-channel list -- the two copies had already drifted
         * apart on which channels were gated on having a pointer.
         * Clean no-op in edit mode (play_runtime is nullptr); the canvas's
         * clear-on-read channels are still drained so nothing latches. */
        jce_runtime_dispatch_ui_events(g.play_runtime, g.ui_canvas, scene);
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
            JceEntity e = jce_state_to_ecs_entity(id);
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
    s_game_frame_observation.completed();
    g.stats_frame_completed = true;
}

void jce_editor_game_render_set_module(const JceGameModule *mod)
{
    jce_editor_play_set_module(mod);
}

const JceGameModule *jce_editor_game_render_get_module(void)
{
    return jce_editor_play_get_module();
}
