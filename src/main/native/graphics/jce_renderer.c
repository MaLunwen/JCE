/*
 * jce_renderer.c  bgfx renderer implementation.
 */

#include "jce_renderer.h"
#include "jce_camera.h"
#include "jce_views.h"
#include "platform/jce_window.h"
#include "resource/pak_loader.h"
#include "foundation/jce_log.h"
#include "foundation/jce_math.h"
#include "jce_shaders.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#define LOG_TAG "jce_renderer"

struct JceRenderer {
    bool                   is_fallback;
    SDL_Renderer          *sdl_renderer;

    bgfx_program_handle_t  program;          /* color (pos+color) */
    bgfx_vertex_layout_t   layout;           /* color vertex layout */
    bgfx_program_handle_t  program_textured; /* textured (pos+color+uv) */
    bgfx_vertex_layout_t   layout_textured;  /* textured vertex layout */
    bgfx_uniform_handle_t  u_tex_color;      /* sampler uniform for textures */
    bgfx_program_handle_t  program_mesh;     /* mesh (pos+normal+uv) */
    bgfx_uniform_handle_t  u_light_dir;      /* vec4: xyz = light direction */
    bgfx_uniform_handle_t  u_light_color;    /* vec4: xyz = color, w = ambient */
    uint32_t               reset_flags;
    char                   gpu_name[128];
};

/* Map backend enum to bgfx renderer type. */
static bgfx_renderer_type_t map_backend(int backend)
{
    switch (backend) {
    case 1:  return BGFX_RENDERER_TYPE_DIRECT3D11;
    case 2:  return BGFX_RENDERER_TYPE_DIRECT3D12;
    case 3:  return BGFX_RENDERER_TYPE_VULKAN;
    case 4:  return BGFX_RENDERER_TYPE_OPENGL;
    case 5:  return BGFX_RENDERER_TYPE_OPENGLES;
    case 6:  return BGFX_RENDERER_TYPE_METAL;
    default: return BGFX_RENDERER_TYPE_COUNT; /* auto */
    }
}

/* Platform-specific preferred backend order (best first).
 * Sentinel: BGFX_RENDERER_TYPE_COUNT marks end of list. */
static const bgfx_renderer_type_t *get_platform_fallback_chain(void)
{
#if defined(_WIN32)
    static const bgfx_renderer_type_t chain[] = {
        BGFX_RENDERER_TYPE_DIRECT3D12,
        BGFX_RENDERER_TYPE_DIRECT3D11,
        BGFX_RENDERER_TYPE_VULKAN,
        BGFX_RENDERER_TYPE_OPENGL,
        BGFX_RENDERER_TYPE_COUNT
    };
#elif defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IOS || TARGET_OS_TV
    static const bgfx_renderer_type_t chain[] = {
        BGFX_RENDERER_TYPE_METAL,
        BGFX_RENDERER_TYPE_OPENGLES,
        BGFX_RENDERER_TYPE_COUNT
    };
  #else /* macOS */
    static const bgfx_renderer_type_t chain[] = {
        BGFX_RENDERER_TYPE_METAL,
        BGFX_RENDERER_TYPE_OPENGL,
        BGFX_RENDERER_TYPE_COUNT
    };
  #endif
#elif defined(__ANDROID__)
    static const bgfx_renderer_type_t chain[] = {
        BGFX_RENDERER_TYPE_OPENGLES,
        BGFX_RENDERER_TYPE_VULKAN,
        BGFX_RENDERER_TYPE_COUNT
    };
#elif defined(__EMSCRIPTEN__)
    static const bgfx_renderer_type_t chain[] = {
        BGFX_RENDERER_TYPE_OPENGLES,
        BGFX_RENDERER_TYPE_COUNT
    };
#else /* Linux and other Unix */
    static const bgfx_renderer_type_t chain[] = {
        BGFX_RENDERER_TYPE_VULKAN,
        BGFX_RENDERER_TYPE_OPENGL,
        BGFX_RENDERER_TYPE_COUNT
    };
#endif
    return chain;
}

/* -- Lifecycle ------------------------------------------------------ */

JceRenderer *jce_renderer_create(JceWindow *win, const PakArchive *pak,
                                  const JceRendererConfig *cfg)
{
    if (!win || !pak || !cfg) return NULL;

    /* Retrieve native window handle.
     * On iOS the native handle may become available slightly after window
     * creation, so retry briefly before giving up. */
    JceNativeWindow nw;
    memset(&nw, 0, sizeof(nw));
    for (int i = 0; i < 120; i++) {
        jce_window_get_native(win, &nw);
        if (nw.nwh) break;
        SDL_PumpEvents();
        SDL_Delay(16);
    }

    if (!nw.nwh) {
        LOG_ERROR(LOG_TAG, "native window handle is NULL");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "JCE",
            "Native window not ready", NULL);
        return NULL;
    }

    bgfx_platform_data_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.nwh = nw.nwh;
    pd.ndt = nw.ndt;
    bgfx_set_platform_data(&pd);

    /* Initialise bgfx with graceful fallback. */
    uint32_t w, h;
    jce_window_get_size(win, &w, &h);

    uint32_t reset_flags = cfg->vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE;

    bgfx_renderer_type_t requested_type = map_backend(cfg->backend);
    LOG_INFO(LOG_TAG, "init request: backend=%s nwh=%p ndt=%p size=%ux%u",
             requested_type == BGFX_RENDERER_TYPE_COUNT ? "auto" : bgfx_get_renderer_name(requested_type),
             pd.nwh, pd.ndt, w, h);

    /* On macOS (not iOS), bgfx's default multi-threaded mode causes a deadlock:
     * the render thread needs to call back to the main thread (via GCD)
     * to set up CAMetalLayer, but the main thread is blocked in bgfx_init()
     * waiting for the render thread.  Calling bgfx_render_frame(-1) before
     * bgfx_init() switches bgfx to single-threaded mode, avoiding the deadlock.
     *
     * iOS does NOT have this problem — UIKit's run-loop allows bgfx's render
     * thread to initialise Metal without deadlocking, so we leave bgfx in its
     * default multi-threaded mode on iOS. */
#if defined(__APPLE__) && TARGET_OS_OSX
    bgfx_render_frame(-1);
#endif

    /* Try to initialise bgfx.  When the user picked a specific backend we
     * attempt that first; on failure (or AUTO) we walk the platform-specific
     * preferred list until one succeeds. */
    bgfx_init_t init;
    bool ok = false;

    if (requested_type != BGFX_RENDERER_TYPE_COUNT) {
        bgfx_init_ctor(&init);
        init.type              = requested_type;
        init.resolution.width  = w;
        init.resolution.height = h;
        init.resolution.reset  = reset_flags;
        init.platformData      = pd;
        ok = bgfx_init(&init);
        if (!ok)
            LOG_WARN(LOG_TAG, "requested backend %s failed",
                     bgfx_get_renderer_name(requested_type));
    }

    if (!ok) {
        const bgfx_renderer_type_t *chain = get_platform_fallback_chain();
        for (int i = 0; chain[i] != BGFX_RENDERER_TYPE_COUNT; i++) {
            if (chain[i] == requested_type) continue; /* already tried */
            LOG_INFO(LOG_TAG, "trying backend: %s",
                     bgfx_get_renderer_name(chain[i]));
            bgfx_init_ctor(&init);
            init.type              = chain[i];
            init.resolution.width  = w;
            init.resolution.height = h;
            init.resolution.reset  = reset_flags;
            init.platformData      = pd;
            if (bgfx_init(&init)) { ok = true; break; }
        }
    }

    if (!ok) {
        LOG_ERROR(LOG_TAG, "bgfx_init failed - all backends exhausted "
                  "(nwh=%p, w=%u, h=%u)", pd.nwh, w, h);
        /* 
         * We do NOT show a messagebox here, as the engine will handle
         * falling back to the SDL renderer or show an error screen later.
         */
        return NULL;
    }

    LOG_INFO(LOG_TAG, "renderer: %s",
             bgfx_get_renderer_name(bgfx_get_renderer_type()));

    if (cfg->debug_text)
        bgfx_set_debug(BGFX_DEBUG_TEXT);

    /* View 0 (3D): clear color + depth. */
    bgfx_set_view_clear(JCE_VIEW_MAIN_3D,
        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
        cfg->clear_color, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_MAIN_3D, 0, 0, (uint16_t)w, (uint16_t)h);

    /* View 1 (UI): no clear  draws on top of 3D. */
    bgfx_set_view_clear(JCE_VIEW_UI, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_UI, 0, 0, (uint16_t)w, (uint16_t)h);

    /* View 2 (debug): no clear  debug text overlay. */
    bgfx_set_view_clear(JCE_VIEW_DEBUG, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_DEBUG, 0, 0, (uint16_t)w, (uint16_t)h);

    /* Vertex layout: Position (float3) + Color0 (UINT8x4, normalized). */
    JceRenderer *r = (JceRenderer *)SDL_calloc(1, sizeof(*r));
    if (!r) {
        bgfx_shutdown();
        return NULL;
    }
    r->reset_flags = reset_flags;

    bgfx_vertex_layout_begin(&r->layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&r->layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout, BGFX_ATTRIB_COLOR0, 4,
                           BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&r->layout);

    /* Load color shader program. */
    r->program = (bgfx_program_handle_t){ shader_load_program(pak, "color").idx };
    if (r->program.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader_load_program('color') failed");
        bgfx_shutdown();
        SDL_free(r);
        return NULL;
    }

    /* Textured vertex layout: Position (float3) + Color0 (UINT8x4) + TexCoord0 (float2). */
    bgfx_vertex_layout_begin(&r->layout_textured, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&r->layout_textured, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout_textured, BGFX_ATTRIB_COLOR0, 4,
                           BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_add(&r->layout_textured, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&r->layout_textured);

    /* Load textured shader program. */
    r->program_textured = (bgfx_program_handle_t){ shader_load_program(pak, "textured").idx };
    if (r->program_textured.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "shader_load_program('textured') failed  textures unavailable");
        r->program_textured.idx = UINT16_MAX;
    }

    /* Texture sampler uniform. */
    r->u_tex_color = bgfx_create_uniform("s_texColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Load mesh shader program (pos+normal+uv + lighting). */
    r->program_mesh = (bgfx_program_handle_t){ shader_load_program(pak, "mesh").idx };
    if (r->program_mesh.idx == UINT16_MAX)
        LOG_WARN(LOG_TAG, "shader_load_program('mesh') failed  3D meshes unavailable");

    /* Lighting uniforms. */
    r->u_light_dir   = bgfx_create_uniform("u_lightDir",   BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_light_color = bgfx_create_uniform("u_lightColor", BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Build GPU name from vendor ID + renderer name. */
    {
        const bgfx_caps_t *caps = bgfx_get_caps();
        const char *vendor;
        switch (caps->vendorId) {
        case 0x1002: vendor = "AMD";     break;
        case 0x10DE: vendor = "NVIDIA";  break;
        case 0x8086: vendor = "Intel";   break;
        case 0x13B5: vendor = "ARM";     break;
        case 0x106B: vendor = "Apple";   break;
        default:     vendor = "Unknown"; break;
        }
        snprintf(r->gpu_name, sizeof(r->gpu_name), "%s / %s",
                 vendor, bgfx_get_renderer_name(bgfx_get_renderer_type()));
    }

    LOG_SUCCESS(LOG_TAG, "initialized (%s)", r->gpu_name);

    return r;
}

JceRenderer *jce_renderer_create_fallback(JceWindow *win)
{
    if (!win) return NULL;
    JceRenderer *r = (JceRenderer *)SDL_calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->is_fallback = true;
    
    SDL_Window *sdl_win = jce_window_sdl(win);
    r->sdl_renderer = SDL_CreateRenderer(sdl_win, NULL);
    if (!r->sdl_renderer) {
        LOG_ERROR(LOG_TAG, "SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_free(r);
        return NULL;
    }

    snprintf(r->gpu_name, sizeof(r->gpu_name), "%s", SDL_GetRendererName(r->sdl_renderer));
    LOG_SUCCESS(LOG_TAG, "fallback initialized (%s)", r->gpu_name);
    return r;
}

bool jce_renderer_is_fallback(const JceRenderer *r)
{
    return r ? r->is_fallback : false;
}

void jce_renderer_render_fallback_frame(const JceRenderer *r)
{
    if (!r || !r->is_fallback || !r->sdl_renderer) return;

    uint64_t ticks = SDL_GetTicks();
    if ((ticks / 500) % 2 == 0) {
        SDL_SetRenderDrawColor(r->sdl_renderer, 20, 180, 255, 255); /* Orange */
    } else {
        SDL_SetRenderDrawColor(r->sdl_renderer, 255, 140, 40, 255);   /* Blue */
    }
    
    SDL_RenderClear(r->sdl_renderer);
    
    // We could draw text here if we had an SDL backend font, but for now
    // a blue screen of safe fallback is better than crashing out.
    // At least the user sees a blue screen instead of a crash and the
    // window event loop still runs.

    SDL_RenderPresent(r->sdl_renderer);
}

void jce_renderer_destroy(JceRenderer *r)
{
    if (!r) return;
    if (r->is_fallback) {
        if (r->sdl_renderer) {
            SDL_DestroyRenderer(r->sdl_renderer);
        }
        SDL_free(r);
        return;
    }
    bgfx_destroy_program(r->program);
    if (r->program_textured.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_textured);
    if (r->u_tex_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_tex_color);
    if (r->program_mesh.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_mesh);
    if (r->u_light_dir.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_dir);
    if (r->u_light_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_color);
    bgfx_shutdown();
    SDL_free(r);
}

/* -- Per-frame ------------------------------------------------------ */

void jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win)
{
    if (!r || !win) return;
    if (r->is_fallback) return;

    /* Full-backbuffer viewport for all views. */
    uint16_t vp_x, vp_y, vp_w, vp_h;
    jce_window_calc_viewport(win, &vp_x, &vp_y, &vp_w, &vp_h);

    const bgfx_caps_t *caps = bgfx_get_caps();

    /* View 0 (3D): identity view, ortho proj  overridden by begin_frame_3d. */
    {
        jce_mat4 view = jce_m4_identity();
        int lw, lh;
        jce_window_get_logical(win, &lw, &lh);
        jce_mat4 proj = jce_m4_ortho(0, (float)lw, (float)lh, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(JCE_VIEW_MAIN_3D, view.m, proj.m);
        bgfx_set_view_rect(JCE_VIEW_MAIN_3D, vp_x, vp_y, vp_w, vp_h);
    }

    /* View 1 (UI): 2D orthographic in logical coordinates. */
    {
        jce_mat4 view = jce_m4_identity();
        int lw, lh;
        jce_window_get_logical(win, &lw, &lh);
        jce_mat4 proj = jce_m4_ortho(0, (float)lw, (float)lh, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(JCE_VIEW_UI, view.m, proj.m);
        bgfx_set_view_rect(JCE_VIEW_UI, vp_x, vp_y, vp_w, vp_h);
    }

    /* View 2 (debug): same as UI for debug text. */
    bgfx_set_view_rect(JCE_VIEW_DEBUG, vp_x, vp_y, vp_w, vp_h);

    bgfx_dbg_text_clear(0, false);
    bgfx_touch(JCE_VIEW_MAIN_3D);
    bgfx_touch(JCE_VIEW_UI);
    bgfx_touch(JCE_VIEW_DEBUG);
}

void jce_renderer_begin_frame_3d(const JceRenderer *r, JceWindow *win,
                                  const JceCamera *cam, uint16_t view_id)
{
    if (!r || !win) return;
    if (r->is_fallback) return;

    uint16_t vp_x, vp_y, vp_w, vp_h;
    jce_window_calc_viewport(win, &vp_x, &vp_y, &vp_w, &vp_h);

    if (cam) {
        const bgfx_caps_t *caps = bgfx_get_caps();
        float aspect = (vp_h > 0) ? (float)vp_w / (float)vp_h : 1.0f;

        jce_mat4 view = jce_camera_view(cam);
        jce_mat4 proj = jce_camera_proj(cam, aspect, caps->homogeneousDepth);

        bgfx_set_view_transform(view_id, view.m, proj.m);
    } else {
        /* Fallback: 2D ortho. */
        jce_mat4 view = jce_m4_identity();
        int lw, lh;
        jce_window_get_logical(win, &lw, &lh);
        const bgfx_caps_t *caps = bgfx_get_caps();
        jce_mat4 proj = jce_m4_ortho(0, (float)lw, (float)lh, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(view_id, view.m, proj.m);
    }

    bgfx_set_view_rect(view_id, vp_x, vp_y, vp_w, vp_h);
    bgfx_touch(view_id);
}

void jce_renderer_end_frame(const JceRenderer *r)
{
    if (!r) return;
    if (r->is_fallback) return;
    bgfx_frame(false);
}

/* -- Events --------------------------------------------------------- */

void jce_renderer_resize(const JceRenderer *r, uint32_t w, uint32_t h)
{
    if (!r) return;
    if (r->is_fallback) return;
    bgfx_reset(w, h, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

void jce_renderer_rebind_platform(JceRenderer *r, JceWindow *win)
{
    if (!r || !win) return;
    if (r->is_fallback) return;

    JceNativeWindow nw;
    jce_window_get_native(win, &nw);
    if (!nw.nwh) {
        LOG_WARN(LOG_TAG, "rebind_platform: nwh is NULL, skipping");
        return;
    }

    bgfx_platform_data_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.nwh = nw.nwh;
    pd.ndt = nw.ndt;
    bgfx_set_platform_data(&pd);

    uint32_t w, h;
    jce_window_get_size(win, &w, &h);
    bgfx_reset(w, h, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

/* -- Debug text ----------------------------------------------------- */

void jce_renderer_dbg_text(uint16_t x, uint16_t y,
                           uint8_t attr, const char *fmt, ...)
{
    // A bit tricky because we don't have 'r' here, but bgfx handles global state
    // We shouldn't call bgfx if in fallback. We just check bgfx context.
    // Wait, bgfx_dbg_text_printf won't crash if not initialized, but it's better to avoid maybe.
    // To be perfectly safe, let's keep it as is, but our calls in jce_engine skip if fallback.
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    bgfx_dbg_text_printf(x, y, attr, "%s", buf);
}

/* -- Accessors for primitives module -------------------------------- */

const bgfx_vertex_layout_t *jce_renderer_get_layout(const JceRenderer *r)
{
    return r ? &r->layout : NULL;
}

bgfx_program_handle_t jce_renderer_get_program(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program : invalid;
}

const bgfx_vertex_layout_t *jce_renderer_get_layout_textured(const JceRenderer *r)
{
    return r ? &r->layout_textured : NULL;
}

bgfx_program_handle_t jce_renderer_get_program_textured(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_textured : invalid;
}

JceUniformHandle jce_renderer_get_tex_uniform(const JceRenderer *r)
{
    JceUniformHandle invalid = JCE_INVALID_UNIFORM;
    if (!r) return invalid;
    return (JceUniformHandle){ r->u_tex_color.idx };
}

JceShaderHandle jce_renderer_get_program_mesh(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_mesh.idx };
}

bgfx_uniform_handle_t jce_renderer_get_light_dir_uniform(const JceRenderer *r)
{
    bgfx_uniform_handle_t invalid = { UINT16_MAX };
    return r ? r->u_light_dir : invalid;
}

bgfx_uniform_handle_t jce_renderer_get_light_color_uniform(const JceRenderer *r)
{
    bgfx_uniform_handle_t invalid = { UINT16_MAX };
    return r ? r->u_light_color : invalid;
}

/* -- Queries (for debug HUD) --------------------------------------- */

const char *jce_renderer_get_backend_name(const JceRenderer *r)
{
    (void)r;
    return bgfx_get_renderer_name(bgfx_get_renderer_type());
}

const char *jce_renderer_get_gpu_name(const JceRenderer *r)
{
    return r ? r->gpu_name : "N/A";
}

bool jce_renderer_get_vsync(const JceRenderer *r)
{
    return r ? (r->reset_flags & BGFX_RESET_VSYNC) != 0 : false;
}
