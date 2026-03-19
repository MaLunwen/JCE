/*
 * jce_renderer.c  bgfx renderer implementation.
 */

#include "jce_renderer.h"
#include "jce_camera.h"
#include "jce_views.h"
#include "platform/jce_window.h"
#include "resource/pak_loader.h"
#include "core/jce_log.h"
#include "core/jce_math.h"
#include "jce_shaders.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>

#define LOG_TAG "jce_renderer"

struct JceRenderer {
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

JceRenderer *jce_renderer_create(JceWindow *win, PakArchive *pak,
                                  const JceRendererConfig *cfg)
{
    if (!win || !pak || !cfg) return NULL;

    /* Retrieve native window handle. */
    JceNativeWindow nw;
    jce_window_get_native(win, &nw);

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
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "JCE",
            "GPU initialization failed.\n"
            "No compatible graphics backend found.", NULL);
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
    r->program = shader_load_program(pak, "color");
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
    r->program_textured = shader_load_program(pak, "textured");
    if (r->program_textured.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "shader_load_program('textured') failed  textures unavailable");
        r->program_textured.idx = UINT16_MAX;
    }

    /* Texture sampler uniform. */
    r->u_tex_color = bgfx_create_uniform("s_texColor", BGFX_UNIFORM_TYPE_SAMPLER, 1);

    /* Load mesh shader program (pos+normal+uv + lighting). */
    r->program_mesh = shader_load_program(pak, "mesh");
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

void jce_renderer_destroy(JceRenderer *r)
{
    if (!r) return;
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

void jce_renderer_begin_frame(JceRenderer *r, JceWindow *win)
{
    if (!r || !win) return;

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

void jce_renderer_begin_frame_3d(JceRenderer *r, JceWindow *win,
                                  JceCamera *cam, uint16_t view_id)
{
    if (!r || !win) return;

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

void jce_renderer_end_frame(JceRenderer *r)
{
    if (!r) return;
    bgfx_frame(false);
}

/* -- Events --------------------------------------------------------- */

void jce_renderer_resize(JceRenderer *r, uint32_t w, uint32_t h)
{
    if (!r) return;
    bgfx_reset(w, h, r->reset_flags, BGFX_TEXTURE_FORMAT_COUNT);
}

void jce_renderer_rebind_platform(JceRenderer *r, JceWindow *win)
{
    if (!r || !win) return;

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

bgfx_uniform_handle_t jce_renderer_get_tex_uniform(const JceRenderer *r)
{
    bgfx_uniform_handle_t invalid = { UINT16_MAX };
    return r ? r->u_tex_color : invalid;
}

bgfx_program_handle_t jce_renderer_get_program_mesh(const JceRenderer *r)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    return r ? r->program_mesh : invalid;
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
