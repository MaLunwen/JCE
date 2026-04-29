/*
 * jce_weather.c -- screen-space weather overlay implementation.
 *
 * Rendering: a single full-screen quad in normalized-device coords (no
 * matrices) drawn alpha-blended with depth disabled.  The fragment
 * shader procedurally generates rain streaks or snow flakes layered
 * across screen space.  Wind tangential speed is the dot of wind_dir
 * with the camera-right vector, currently approximated as wind.x
 * (sufficient for top-down or roughly-yawed cameras).
 */

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_weather.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "weather"

struct JceWeatherSystem {
    JceWeatherState         state;
    float                   time_accum;

    bgfx_vertex_layout_t    layout;
    bgfx_program_handle_t   program;
    bgfx_uniform_handle_t   u_params;
    bgfx_uniform_handle_t   u_color;
    bgfx_vertex_buffer_handle_t vbh;
    bgfx_index_buffer_handle_t  ibh;
};

typedef struct { float pos[2]; float uv[2]; } QuadV;

static const char *weather_backend_suffix(void)
{
    switch (bgfx_get_renderer_type()) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12: return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:     return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:     return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:   return "essl";
    case BGFX_RENDERER_TYPE_METAL:      return "mtl";
    default:                            return NULL;
    }
}

static bgfx_shader_handle_t weather_load_shader(const JcePakArchive *pak,
                                                 const char *name,
                                                 const char *sfx)
{
    bgfx_shader_handle_t invalid = { UINT16_MAX };
    char path[256];
    snprintf(path, sizeof(path), "shaders/%s_%s.bin", name, sfx);
    const JcePakAsset *asset = jce_pak_find(pak, path);
    if (!asset) { LOG_ERROR(LOG_TAG, "shader not found: %s", path); return invalid; }
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return invalid;
    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) { JCE_FREE(buf); return invalid; }
    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);
    return bgfx_create_shader(mem);
}

JceWeatherSystem *jce_weather_create(const JceWeatherDesc *desc)
{
    if (!desc || !desc->pak) return NULL;
    const char *sfx = weather_backend_suffix();
    if (!sfx) return NULL;

    JceWeatherSystem *w = (JceWeatherSystem *)JCE_CALLOC(1, sizeof(*w));
    if (!w) return NULL;
    w->state = jce_weather_default(JCE_WEATHER_CLEAR, 0.0f);

    bgfx_vertex_layout_begin(&w->layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&w->layout, BGFX_ATTRIB_POSITION,  2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&w->layout, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&w->layout);

    static const QuadV verts[4] = {
        { { -1.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  1.0f, -1.0f }, { 1.0f, 1.0f } },
        { {  1.0f,  1.0f }, { 1.0f, 0.0f } },
        { { -1.0f,  1.0f }, { 0.0f, 0.0f } },
    };
    static const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    w->vbh = bgfx_create_vertex_buffer(bgfx_copy(verts, sizeof(verts)),
                                       &w->layout, BGFX_BUFFER_NONE);
    w->ibh = bgfx_create_index_buffer(bgfx_copy(idx, sizeof(idx)),
                                      BGFX_BUFFER_NONE);

    bgfx_shader_handle_t vsh = weather_load_shader(desc->pak, "vs_weather", sfx);
    bgfx_shader_handle_t fsh = weather_load_shader(desc->pak, "fs_weather", sfx);
    if (vsh.idx == UINT16_MAX || fsh.idx == UINT16_MAX) {
        if (vsh.idx != UINT16_MAX) bgfx_destroy_shader(vsh);
        if (fsh.idx != UINT16_MAX) bgfx_destroy_shader(fsh);
        bgfx_destroy_vertex_buffer(w->vbh);
        bgfx_destroy_index_buffer(w->ibh);
        JCE_FREE(w);
        return NULL;
    }
    w->program = bgfx_create_program(vsh, fsh, true);
    w->u_params = bgfx_create_uniform("u_weather_params", BGFX_UNIFORM_TYPE_VEC4, 1);
    w->u_color  = bgfx_create_uniform("u_weather_color",  BGFX_UNIFORM_TYPE_VEC4, 1);
    return w;
}

void jce_weather_destroy(JceWeatherSystem *w)
{
    if (!w) return;
    if (w->program.idx != UINT16_MAX) bgfx_destroy_program(w->program);
    if (w->u_params.idx != UINT16_MAX) bgfx_destroy_uniform(w->u_params);
    if (w->u_color.idx  != UINT16_MAX) bgfx_destroy_uniform(w->u_color);
    if (w->vbh.idx != UINT16_MAX) bgfx_destroy_vertex_buffer(w->vbh);
    if (w->ibh.idx != UINT16_MAX) bgfx_destroy_index_buffer(w->ibh);
    JCE_FREE(w);
}

void jce_weather_set_state(JceWeatherSystem *w, const JceWeatherState *s)
{
    if (!w || !s) return;
    w->state = *s;
}

JceWeatherState jce_weather_get_state(const JceWeatherSystem *w)
{
    if (!w) return jce_weather_default(JCE_WEATHER_CLEAR, 0.0f);
    return w->state;
}

void jce_weather_update(JceWeatherSystem *w, float dt)
{
    if (!w || dt <= 0.0f) return;
    JCE_PROFILE_ZONE_N("Weather::Update");
    w->time_accum += dt;
    if (w->time_accum > 1.0e6f) w->time_accum -= 1.0e6f;
    JCE_PROFILE_ZONE_END;
}

void jce_weather_render(JceWeatherSystem *w, uint16_t view_id)
{
    if (!w) return;
    if (w->state.type == JCE_WEATHER_CLEAR) return;
    if (w->state.intensity <= 0.0f) return;
    if (w->program.idx == UINT16_MAX) return;

    JCE_PROFILE_ZONE_N("Weather::Render");
    float mode = (w->state.type == JCE_WEATHER_SNOW) ? 1.0f : 0.0f;
    float wind = w->state.wind_dir.x * w->state.wind_strength * 0.05f;
    float params[4] = { mode, w->state.intensity, w->time_accum, wind };
    float color[4]  = { w->state.tint.x, w->state.tint.y, w->state.tint.z,
                        w->state.alpha };
    bgfx_set_uniform(w->u_params, params, 1);
    bgfx_set_uniform(w->u_color,  color,  1);

    bgfx_set_vertex_buffer(0, w->vbh, 0, 4);
    bgfx_set_index_buffer(w->ibh, 0, 6);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                   | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                           BGFX_STATE_BLEND_INV_SRC_ALPHA),
                   0);
    bgfx_submit(view_id, w->program, 0, BGFX_DISCARD_ALL);
    JCE_PROFILE_ZONE_END;
}

JceWeatherState jce_weather_default(JceWeatherType type, float intensity)
{
    JceWeatherState s;
    s.type          = type;
    s.intensity     = intensity;
    s.wetness       = (type == JCE_WEATHER_RAIN) ? intensity * 0.7f : 0.0f;
    s.wind_dir      = jce_v3(1.0f, 0.0f, 0.0f);
    s.wind_strength = 3.0f * intensity;
    s.alpha         = 0.55f;
    switch (type) {
    case JCE_WEATHER_RAIN:
        s.tint = jce_v3(0.65f, 0.72f, 0.85f);
        break;
    case JCE_WEATHER_SNOW:
        s.tint = jce_v3(0.95f, 0.96f, 1.00f);
        s.wind_strength = 1.5f * intensity;
        break;
    default:
        s.tint = jce_v3(1.0f, 1.0f, 1.0f);
        s.intensity = 0.0f;
        break;
    }
    return s;
}
