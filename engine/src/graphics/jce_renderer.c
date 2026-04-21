/*
 * jce_renderer.c  bgfx renderer implementation.
 */

#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_camera.h>
#include <jce/graphics/jce_views.h>
#include <jce/graphics/jce_shaders.h>
#include <jce/platform/jce_window.h>
#include "platform/jce_window_internal.h"
#include <jce/core/jce_log.h>
#include <jce/core/jce_math.h>

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include "core/jce_memory.h"
#include <jce/core/jce_profiler.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

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
    /* PBR programs */
    bgfx_program_handle_t  program_pbr;
    bgfx_program_handle_t  program_pbr_skinned;
    bgfx_program_handle_t  program_shadow;
    bgfx_program_handle_t  program_shadow_skinned;
    bgfx_uniform_handle_t  u_light_dir;      /* vec4: xyz = light direction */
    bgfx_uniform_handle_t  u_light_color;    /* vec4: xyz = color, w = ambient */
    uint32_t               reset_flags;
    uint32_t               debug_flags;
    char                   gpu_name[128];
};

static bool s_dbg_text_enabled = false;

static void jce_bgfx_fatal(bgfx_callback_interface_t* _this,
                           const char* _filePath,
                           uint16_t _line,
                           bgfx_fatal_t _code,
                           const char* _str)
{
    (void)_this;
    LOG_ERROR(LOG_TAG,
              "bgfx fatal: code=%d file=%s line=%u msg=%s",
              (int)_code,
              _filePath ? _filePath : "<null>",
              (unsigned)_line,
              _str ? _str : "<null>");
}

static void jce_bgfx_trace_vargs(bgfx_callback_interface_t* _this,
                                 const char* _filePath,
                                 uint16_t _line,
                                 const char* _format,
                                 va_list _argList)
{
    (void)_this;
    (void)_filePath;
    (void)_line;
    (void)_format;
    (void)_argList;
}

static void jce_bgfx_profiler_begin(bgfx_callback_interface_t* _this,
                                    const char* _name,
                                    uint32_t _abgr,
                                    const char* _filePath,
                                    uint16_t _line)
{
    (void)_this; (void)_name; (void)_abgr; (void)_filePath; (void)_line;
}

static void jce_bgfx_profiler_begin_literal(bgfx_callback_interface_t* _this,
                                            const char* _name,
                                            uint32_t _abgr,
                                            const char* _filePath,
                                            uint16_t _line)
{
    (void)_this; (void)_name; (void)_abgr; (void)_filePath; (void)_line;
}

static void jce_bgfx_profiler_end(bgfx_callback_interface_t* _this)
{
    (void)_this;
}

static uint32_t jce_bgfx_cache_read_size(bgfx_callback_interface_t* _this,
                                         uint64_t _id)
{
    (void)_this; (void)_id;
    return 0;
}

static bool jce_bgfx_cache_read(bgfx_callback_interface_t* _this,
                                uint64_t _id,
                                void* _data,
                                uint32_t _size)
{
    (void)_this; (void)_id; (void)_data; (void)_size;
    return false;
}

static void jce_bgfx_cache_write(bgfx_callback_interface_t* _this,
                                 uint64_t _id,
                                 const void* _data,
                                 uint32_t _size)
{
    (void)_this; (void)_id; (void)_data; (void)_size;
}

static void jce_bgfx_screen_shot(bgfx_callback_interface_t* _this,
                                 const char* _filePath,
                                 uint32_t _width,
                                 uint32_t _height,
                                 uint32_t _pitch,
                                 const void* _data,
                                 uint32_t _size,
                                 bool _yflip)
{
    (void)_this; (void)_filePath; (void)_width; (void)_height;
    (void)_pitch; (void)_data; (void)_size; (void)_yflip;
}

static void jce_bgfx_capture_begin(bgfx_callback_interface_t* _this,
                                   uint32_t _width,
                                   uint32_t _height,
                                   uint32_t _pitch,
                                   bgfx_texture_format_t _format,
                                   bool _yflip)
{
    (void)_this; (void)_width; (void)_height; (void)_pitch;
    (void)_format; (void)_yflip;
}

static void jce_bgfx_capture_end(bgfx_callback_interface_t* _this)
{
    (void)_this;
}

static void jce_bgfx_capture_frame(bgfx_callback_interface_t* _this,
                                   const void* _data,
                                   uint32_t _size)
{
    (void)_this; (void)_data; (void)_size;
}

static const bgfx_callback_vtbl_t s_bgfx_callback_vtbl = {
    jce_bgfx_fatal,
    jce_bgfx_trace_vargs,
    jce_bgfx_profiler_begin,
    jce_bgfx_profiler_begin_literal,
    jce_bgfx_profiler_end,
    jce_bgfx_cache_read_size,
    jce_bgfx_cache_read,
    jce_bgfx_cache_write,
    jce_bgfx_screen_shot,
    jce_bgfx_capture_begin,
    jce_bgfx_capture_end,
    jce_bgfx_capture_frame,
};

static bgfx_callback_interface_t s_bgfx_callback = {
    &s_bgfx_callback_vtbl,
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

JceRenderer *jce_renderer_create(JceWindow *win,
                                  const JceRendererConfig *cfg)
{
    if (!win || !cfg) return NULL;

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
    const char *backend_name =
        requested_type == BGFX_RENDERER_TYPE_COUNT
            ? "auto"
            : bgfx_get_renderer_name(requested_type);
    LOG_INFO(LOG_TAG,
        "init request: backend=%s nwh=%p ndt=%p size=%ux%u",
        backend_name, pd.nwh, pd.ndt, w, h);

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
        init.callback          = &s_bgfx_callback;
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
            init.callback          = &s_bgfx_callback;
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

    bool enable_debug_text = cfg->debug_text;
    if (bgfx_get_renderer_type() == BGFX_RENDERER_TYPE_OPENGL) {
        if (enable_debug_text) {
            LOG_INFO(LOG_TAG, "disabling bgfx debug text on OpenGL backend");
        }
        enable_debug_text = false;
    }

    if (enable_debug_text)
        bgfx_set_debug(BGFX_DEBUG_TEXT);

    /* View 0 (3D): clear color + depth. */
    bgfx_set_view_clear(JCE_VIEW_MAIN_3D,
        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
        cfg->clear_color, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_MAIN_3D, 0, 0, (uint16_t)w, (uint16_t)h);

    /* View 1 (UI): no clear  draws on top of 3D.
       Sequential mode = painter's algorithm (submission order).
       RmlUi already submits back-to-front; post-render primitives
       (polyline graph, etc.) appear on top of the UI panels. */
    bgfx_set_view_clear(JCE_VIEW_UI, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_UI, 0, 0, (uint16_t)w, (uint16_t)h);
    bgfx_set_view_mode(JCE_VIEW_UI, BGFX_VIEW_MODE_SEQUENTIAL);

    /* View 2 (debug): no clear  debug text overlay. */
    bgfx_set_view_clear(JCE_VIEW_DEBUG, BGFX_CLEAR_NONE, 0, 1.0f, 0);
    bgfx_set_view_rect(JCE_VIEW_DEBUG, 0, 0, (uint16_t)w, (uint16_t)h);

    /* Vertex layout: Position (float3) + Color0 (UINT8x4, normalized). */
    JceRenderer *r = (JceRenderer *)JCE_CALLOC(1, sizeof(*r));
    if (!r) {
        bgfx_shutdown();
        return NULL;
    }
    r->reset_flags = reset_flags;
    r->debug_flags = enable_debug_text ? BGFX_DEBUG_TEXT : 0;
    s_dbg_text_enabled = enable_debug_text;

    /* Color vertex layout: pos(float3) + color(uint8x4). */
    bgfx_vertex_layout_begin(&r->layout,
        bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&r->layout,
        BGFX_ATTRIB_POSITION, 3,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout,
        BGFX_ATTRIB_COLOR0, 4,
        BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&r->layout);

    /* Textured vertex layout: pos + color + uv. */
    bgfx_vertex_layout_begin(&r->layout_textured,
        bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&r->layout_textured,
        BGFX_ATTRIB_POSITION, 3,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&r->layout_textured,
        BGFX_ATTRIB_COLOR0, 4,
        BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_add(&r->layout_textured,
        BGFX_ATTRIB_TEXCOORD0, 2,
        BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&r->layout_textured);

    /* Uniforms (created here; shaders attached later). */
    r->u_tex_color = bgfx_create_uniform(
        "s_texColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    r->u_light_dir = bgfx_create_uniform(
        "u_lightDir", BGFX_UNIFORM_TYPE_VEC4, 1);
    r->u_light_color = bgfx_create_uniform(
        "u_lightColor", BGFX_UNIFORM_TYPE_VEC4, 1);

    /* Shader programs default to invalid; call
       jce_renderer_set_shaders() after creation. */
    r->program.idx              = UINT16_MAX;
    r->program_textured.idx     = UINT16_MAX;
    r->program_mesh.idx         = UINT16_MAX;
    r->program_pbr.idx          = UINT16_MAX;
    r->program_pbr_skinned.idx  = UINT16_MAX;
    r->program_shadow.idx       = UINT16_MAX;
    r->program_shadow_skinned.idx = UINT16_MAX;

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

void jce_renderer_set_shaders(JceRenderer *r,
                              const JceShaderSet *shaders)
{
    if (!r || !shaders || r->is_fallback) return;

    r->program = (bgfx_program_handle_t){
        shaders->color.idx };
    r->program_textured = (bgfx_program_handle_t){
        shaders->textured.idx };
    r->program_mesh = (bgfx_program_handle_t){
        shaders->mesh.idx };
    r->program_pbr = (bgfx_program_handle_t){ shaders->pbr.idx };
    r->program_pbr_skinned = (bgfx_program_handle_t){ shaders->pbr_skinned.idx };
    r->program_shadow = (bgfx_program_handle_t){ shaders->shadow.idx };
    r->program_shadow_skinned = (bgfx_program_handle_t){ shaders->shadow_skinned.idx };

    if (r->program.idx == UINT16_MAX)
        LOG_ERROR(LOG_TAG, "color shader not provided");
}

JceRenderer *jce_renderer_create_fallback(JceWindow *win)
{
    if (!win) return NULL;
    JceRenderer *r = (JceRenderer *)JCE_CALLOC(1, sizeof(*r));
    if (!r) return NULL;

    r->is_fallback = true;

    SDL_Window *sdl_win = jce_window_sdl(win);

    /* 1) Let SDL pick the best available GPU-backed renderer. */
    r->sdl_renderer = SDL_CreateRenderer(sdl_win, NULL);
    if (r->sdl_renderer) {
        snprintf(r->gpu_name, sizeof(r->gpu_name), "Fallback: %s",
                 SDL_GetRendererName(r->sdl_renderer));
        LOG_SUCCESS(LOG_TAG, "fallback initialized (%s)", r->gpu_name);
        return r;
    }
    LOG_WARN(LOG_TAG, "SDL auto renderer failed: %s — trying software", SDL_GetError());

    /* 2) Force pure CPU software renderer (no GPU needed at all). */
    r->sdl_renderer = SDL_CreateRenderer(sdl_win, SDL_SOFTWARE_RENDERER);
    if (r->sdl_renderer) {
        snprintf(r->gpu_name, sizeof(r->gpu_name), "Fallback: software (CPU)");
        LOG_SUCCESS(LOG_TAG, "fallback initialized (software CPU renderer)");
        return r;
    }
    LOG_ERROR(LOG_TAG, "SDL software renderer failed: %s", SDL_GetError());

    JCE_FREE(r);
    return NULL;
}

bool jce_renderer_is_fallback(const JceRenderer *r)
{
    return r ? r->is_fallback : false;
}

/* -- Fallback frame: real 2D rendering via SDL_Renderer ------------- */

/* Draw a filled rounded-corner rectangle (approximated with rects). */
static void fb_draw_panel(SDL_Renderer *rd, float x, float y, float w, float h,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(rd, r, g, b, a);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderFillRect(rd, &rect);
}

/* Draw a 1px border rectangle. */
static void fb_draw_border(SDL_Renderer *rd, float x, float y, float w, float h,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    SDL_SetRenderDrawColor(rd, r, g, b, a);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderRect(rd, &rect);
}

void jce_renderer_render_fallback_frame(const JceRenderer *r)
{
    if (!r || !r->is_fallback || !r->sdl_renderer) return;

    SDL_Renderer *rd = r->sdl_renderer;
    int ww = 0, wh = 0;
    SDL_GetRenderOutputSize(rd, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    float fw = (float)ww, fh = (float)wh;
    float scale = fw / 800.0f; /* base design at 800px wide */
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 2.5f) scale = 2.5f;

    /* -- Background gradient (approximated with horizontal bands) -- */
    for (int i = 0; i < wh; i++) {
        float t = (float)i / fh;
        uint8_t cr = (uint8_t)(20  + t * 15);
        uint8_t cg = (uint8_t)(22  + t * 18);
        uint8_t cb = (uint8_t)(35  + t * 25);
        SDL_SetRenderDrawColor(rd, cr, cg, cb, 255);
        SDL_FRect line = { 0, (float)i, fw, 1.0f };
        SDL_RenderFillRect(rd, &line);
    }

    /* -- Center panel ------------------------------------------------ */
    float panel_w = 460 * scale;
    float panel_h = 280 * scale;
    float px = (fw - panel_w) / 2.0f;
    float py = (fh - panel_h) / 2.0f;

    fb_draw_panel(rd, px, py, panel_w, panel_h, 30, 32, 45, 230);
    fb_draw_border(rd, px, py, panel_w, panel_h, 80, 180, 255, 200);

    /* -- Title bar --------------------------------------------------- */
    float bar_h = 36 * scale;
    fb_draw_panel(rd, px, py, panel_w, bar_h, 50, 130, 220, 255);

    /* -- Text via SDL_RenderDebugText (8x8 monospace, built-in) ------ */
    float text_scale = scale * 1.5f;
    SDL_SetRenderScale(rd, text_scale, text_scale);

    float tx = (px + 12 * scale) / text_scale;
    float ty = (py + 10 * scale) / text_scale;

    /* Title. */
    SDL_SetRenderDrawColor(rd, 255, 255, 255, 255);
    SDL_RenderDebugText(rd, tx, ty, "JCE - Software Renderer");

    /* Info lines below title bar. */
    float line_y = (py + bar_h + 16 * scale) / text_scale;
    float line_x = (px + 20 * scale) / text_scale;
    float line_h = 14.0f;

    SDL_SetRenderDrawColor(rd, 200, 200, 210, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "GPU acceleration unavailable.");
    line_y += line_h;
    SDL_RenderDebugText(rd, line_x, line_y, "Running in CPU software mode.");
    line_y += line_h * 1.8f;

    SDL_SetRenderDrawColor(rd, 140, 180, 220, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "Renderer:");
    SDL_SetRenderDrawColor(rd, 255, 220, 100, 255);
    SDL_RenderDebugText(rd, line_x + 80, line_y, r->gpu_name);
    line_y += line_h;

    SDL_SetRenderDrawColor(rd, 140, 180, 220, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "Platform:");
    SDL_SetRenderDrawColor(rd, 255, 220, 100, 255);
    SDL_RenderDebugText(rd, line_x + 80, line_y, SDL_GetPlatform());
    line_y += line_h * 1.8f;

    /* Uptime. */
    uint64_t ticks = SDL_GetTicks();
    unsigned secs = (unsigned)(ticks / 1000);
    unsigned mins = secs / 60;
    secs %= 60;
    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%u:%02u", mins, secs);

    SDL_SetRenderDrawColor(rd, 140, 180, 220, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "Uptime:");
    SDL_SetRenderDrawColor(rd, 180, 255, 180, 255);
    SDL_RenderDebugText(rd, line_x + 80, line_y, time_buf);
    line_y += line_h * 1.8f;

    /* Hint message. */
    SDL_SetRenderDrawColor(rd, 120, 120, 140, 255);
    SDL_RenderDebugText(rd, line_x, line_y, "For full rendering, use a system");
    line_y += line_h;
    SDL_RenderDebugText(rd, line_x, line_y, "with GPU hardware acceleration.");

    /* Restore scale. */
    SDL_SetRenderScale(rd, 1.0f, 1.0f);

    /* -- Animated activity indicator (bottom of panel) --------------- */
    {
        float bar_x = px + 20 * scale;
        float bar_y = py + panel_h - 28 * scale;
        float bar_w = panel_w - 40 * scale;
        float bar_ht = 6 * scale;
        /* Ping-pong animation. */
        float t = (float)(ticks % 3000) / 3000.0f;
        float pos = t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
        fb_draw_panel(rd, bar_x, bar_y, bar_w, bar_ht, 40, 40, 50, 255);
        float dot_w = bar_w * 0.25f;
        fb_draw_panel(rd, bar_x + pos * (bar_w - dot_w), bar_y,
                      dot_w, bar_ht, 80, 180, 255, 255);
    }

    SDL_RenderPresent(rd);
}

void jce_renderer_destroy(JceRenderer *r)
{
    if (!r) return;
    if (r->is_fallback) {
        if (r->sdl_renderer) {
            SDL_DestroyRenderer(r->sdl_renderer);
        }
        JCE_FREE(r);
        return;
    }
    s_dbg_text_enabled = false;
    if (r->program.idx != UINT16_MAX)
        bgfx_destroy_program(r->program);
    if (r->program_textured.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_textured);
    if (r->u_tex_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_tex_color);
    if (r->program_mesh.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_mesh);
    if (r->program_pbr.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr);
    if (r->program_pbr_skinned.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_pbr_skinned);
    if (r->program_shadow.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_shadow);
    if (r->program_shadow_skinned.idx != UINT16_MAX)
        bgfx_destroy_program(r->program_shadow_skinned);
    if (r->u_light_dir.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_dir);
    if (r->u_light_color.idx != UINT16_MAX)
        bgfx_destroy_uniform(r->u_light_color);
    bgfx_shutdown();
    JCE_FREE(r);
}

/* -- Per-frame ------------------------------------------------------ */

void jce_renderer_begin_frame(const JceRenderer *r, JceWindow *win)
{
    JCE_PROFILE_ZONE_N("Renderer::BeginFrame");
    if (!r || !win) { JCE_PROFILE_ZONE_END; return; }
    if (r->is_fallback) { JCE_PROFILE_ZONE_END; return; }

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
        bgfx_set_view_transform(JCE_VIEW_MAIN_3D, view.raw[0], proj.raw[0]);
        bgfx_set_view_rect(JCE_VIEW_MAIN_3D, vp_x, vp_y, vp_w, vp_h);
    }

    /* View 1 (UI): 2D orthographic in logical coordinates. */
    {
        jce_mat4 view = jce_m4_identity();
        uint32_t pw, ph;
        jce_window_get_size(win, &pw, &ph);
        jce_mat4 proj = jce_m4_ortho(0, (float)pw, (float)ph, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(JCE_VIEW_UI, view.raw[0], proj.raw[0]);
        bgfx_set_view_rect(JCE_VIEW_UI, vp_x, vp_y, vp_w, vp_h);
    }

    /* View 2 (debug): same as UI for debug text. */
    bgfx_set_view_rect(JCE_VIEW_DEBUG, vp_x, vp_y, vp_w, vp_h);

    if (s_dbg_text_enabled)
        bgfx_dbg_text_clear(0, false);
    bgfx_touch(JCE_VIEW_MAIN_3D);
    bgfx_touch(JCE_VIEW_UI);
    bgfx_touch(JCE_VIEW_DEBUG);
    JCE_PROFILE_ZONE_END;
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

        bgfx_set_view_transform(view_id, view.raw[0], proj.raw[0]);
    } else {
        /* Fallback: 2D ortho. */
        jce_mat4 view = jce_m4_identity();
        int lw, lh;
        jce_window_get_logical(win, &lw, &lh);
        const bgfx_caps_t *caps = bgfx_get_caps();
        jce_mat4 proj = jce_m4_ortho(0, (float)lw, (float)lh, 0,
                                      0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(view_id, view.raw[0], proj.raw[0]);
    }

    bgfx_set_view_rect(view_id, vp_x, vp_y, vp_w, vp_h);
    bgfx_touch(view_id);
}

void jce_renderer_end_frame(const JceRenderer *r)
{
    JCE_PROFILE_ZONE_N("Renderer::EndFrame");
    if (!r || r->is_fallback) { JCE_PROFILE_ZONE_END; return; }
    bgfx_frame(false);
    JCE_PROFILE_ZONE_END;
}

/* -- Events --------------------------------------------------------- */

void jce_renderer_resize(const JceRenderer *r, uint32_t w, uint32_t h)
{
    if (!r) return;
    if (r->is_fallback) return;

    /* Sanitize dimensions before handing them to bgfx:
     *  - bgfx_reset(0, 0) leaves the backbuffer in an invalid state which
     *    typically manifests as a permanent black screen after restoring
     *    from a minimized window.
     *  - Some D3D11/D3D12 drivers reject backbuffer widths that are not a
     *    multiple of 4, falling back silently and leaving the previous
     *    swap chain — the user sees a frozen / flickering image while the
     *    window is dragged across DPI boundaries that produce odd pixel
     *    widths. Round the width up so reset always succeeds. */
    if (w < 1u) w = 1u;
    if (h < 1u) h = 1u;
    w = (w + 3u) & ~3u;

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
    /* bgfx handles global state so we don't need 'r' here.
     * Our callers in jce_engine already skip this when in fallback mode. */
    if (!s_dbg_text_enabled)
        return;

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

JceShaderHandle jce_renderer_get_program_color(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program.idx };
}

JceShaderHandle jce_renderer_get_program_mesh(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_mesh.idx };
}

JceShaderHandle jce_renderer_get_program_pbr(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_pbr.idx };
}

JceShaderHandle jce_renderer_get_program_pbr_skinned(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_pbr_skinned.idx };
}

JceShaderHandle jce_renderer_get_program_shadow(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_shadow.idx };
}

JceShaderHandle jce_renderer_get_program_shadow_skinned(const JceRenderer *r)
{
    JceShaderHandle invalid = JCE_INVALID_SHADER;
    if (!r) return invalid;
    return (JceShaderHandle){ r->program_shadow_skinned.idx };
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_pbr : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_pbr_skinned(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_pbr_skinned : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_shadow : invalid;
}

bgfx_program_handle_t jce_renderer_get_bgfx_program_shadow_skinned(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_shadow_skinned : invalid;
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

void jce_renderer_set_vsync_for_size(JceRenderer *r, bool enabled,
                                     uint32_t width, uint32_t height)
{
    if (!r || r->is_fallback) return;
    bool current = (r->reset_flags & BGFX_RESET_VSYNC) != 0;
    if (current == enabled) return;

    if (enabled)
        r->reset_flags |= BGFX_RESET_VSYNC;
    else
        r->reset_flags &= ~BGFX_RESET_VSYNC;

    if (width == 0 || height == 0) {
        const bgfx_stats_t *stats = bgfx_get_stats();
        width = stats->width;
        height = stats->height;
    }

    bgfx_reset(width, height, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

void jce_renderer_set_vsync(JceRenderer *r, bool enabled)
{
    const bgfx_stats_t *stats = bgfx_get_stats();
    jce_renderer_set_vsync_for_size(r, enabled, stats->width, stats->height);
}

/* -- Transform / texture binding (game-layer wrappers) ------------- */

void jce_renderer_set_transform(const float *mtx)
{
    bgfx_set_transform(mtx, 1);
}

void jce_renderer_bind_texture(const JceRenderer *r,
                               uint8_t stage,
                               JceTexture tex)
{
    if (!r || tex.idx == UINT16_MAX) return;
    bgfx_texture_handle_t th = { tex.idx };
    bgfx_set_texture(stage,
        (bgfx_uniform_handle_t){ r->u_tex_color.idx },
        th, UINT32_MAX);
}

void jce_renderer_dbg_text_attr(uint16_t x, uint16_t y,
                                uint8_t attr,
                                const char *str)
{
    if (!s_dbg_text_enabled)
        return;
    bgfx_dbg_text_printf(x, y, attr, "%s", str);
}

/* -- Wireframe debug mode ------------------------------------------ */

void jce_renderer_set_wireframe(JceRenderer *r, bool enabled)
{
    if (!r || r->is_fallback) return;
    if (enabled)
        r->debug_flags |= BGFX_DEBUG_WIREFRAME;
    else
        r->debug_flags &= ~(uint32_t)BGFX_DEBUG_WIREFRAME;
    bgfx_set_debug(r->debug_flags);
}

bool jce_renderer_get_wireframe(const JceRenderer *r)
{
    return r ? (r->debug_flags & BGFX_DEBUG_WIREFRAME) != 0 : false;
}

bool jce_renderer_origin_bottom_left(void)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    return caps ? caps->originBottomLeft : false;
}
