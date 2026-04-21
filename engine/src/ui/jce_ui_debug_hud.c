/*
 * jce_ui_debug_hud.c  Built-in debug HUD overlay implementation.
 *
 * Loads the engine/ui RML document from the PAK archive and
 * exposes a simple update() interface for the game to push data.
 */

#include <jce/ui/jce_ui_debug_hud.h>
#include <jce/graphics/jce_primitives.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/platform/jce_window.h>
#include <jce/core/jce_log.h>
#include "core/jce_memory.h"

#include <bgfx/c99/bgfx.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "ui.hud"

struct JceDebugHud {
    JceUIContext        *ui;
    JceRenderer         *renderer;
    JceWindow           *window;
    JceUIDocHandle       doc;
    bool                 visible;

    /* Element handles. */
    JceUIElementHandle   el_gpu_value;
    JceUIElementHandle   el_cpu_value;
    JceUIElementHandle   el_vram_value;
    JceUIElementHandle   el_ram_value;
    JceUIElementHandle   el_api_value;
    JceUIElementHandle   el_api_detail;
    JceUIElementHandle   el_fps;
    JceUIElementHandle   el_frametime;
    JceUIElementHandle   el_frametime_ms;
    JceUIElementHandle   el_graph;
    JceUIElementHandle   el_extra_status;
    JceUIElementHandle   el_shortcut_hints;

    const float         *frametime_history;
    int                  frametime_history_count;
    int                  frametime_history_head;
    int                  frametime_graph_columns;
};

enum {
    JCE_HUD_MAX_GRAPH_POINTS = 256
};

static float clampf(float value, float min_value, float max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static void format_gb_value(char *buf, size_t buf_size, int64_t mb)
{
    if (!buf || buf_size == 0) return;
    if (mb <= 0) {
        snprintf(buf, buf_size, "N/A");
        return;
    }

    if (mb >= 1024)
        snprintf(buf, buf_size, "%.2f GB", (double)mb / 1024.0);
    else
        snprintf(buf, buf_size, "%lld MB", (long long)mb);
}

static void format_gpu_value(char *buf, size_t buf_size, const char *gpu_name)
{
    if (!buf || buf_size == 0) return;
    if (!gpu_name || !gpu_name[0]) {
        snprintf(buf, buf_size, "N/A");
        return;
    }

    const size_t name_len = strlen(gpu_name);
    if (name_len < buf_size) {
        snprintf(buf, buf_size, "%s", gpu_name);
        return;
    }

    if (buf_size > 4) {
        memcpy(buf, gpu_name, buf_size - 4);
        memcpy(buf + buf_size - 4, "...", 4);
    }
}

static int debug_hud_visible_sample_count(const JceDebugHud *hud)
{
    int visible_count;

    if (!hud || !hud->frametime_history || hud->frametime_history_count < 2)
        return 0;

    visible_count = hud->frametime_graph_columns;
    if (visible_count <= 1 || visible_count > hud->frametime_history_count)
        visible_count = hud->frametime_history_count;
    if (visible_count > JCE_HUD_MAX_GRAPH_POINTS)
        visible_count = JCE_HUD_MAX_GRAPH_POINTS;
    return visible_count;
}

static float debug_hud_graph_y(float graph_y, float graph_h,
                               float sample_ms, float max_ms)
{
    const float normalized = clampf(sample_ms / max_ms, 0.0f, 1.0f);
    return graph_y + graph_h - 1.0f - (normalized * (graph_h - 2.0f));
}

JceDebugHud *jce_debug_hud_create(const JceDebugHudDesc *desc)
{
    if (!desc || !desc->ui || !desc->renderer || !desc->window)
        return NULL;

    JceDebugHud *hud = (JceDebugHud *)JCE_CALLOC(1, sizeof(*hud));
    if (!hud) return NULL;

    hud->ui       = desc->ui;
    hud->renderer = desc->renderer;
    hud->window   = desc->window;

    /* Load file-based document from engine/ui. */
    hud->doc = jce_ui_doc_load_file(hud->ui, "engine_debug_hud.rml");
    if (!jce_ui_doc_valid(hud->doc)) {
        LOG_ERROR(LOG_TAG, "failed to load debug HUD document: engine_debug_hud.rml");
        JCE_FREE(hud);
        return NULL;
    }

    /* Cache element handles. */
    hud->el_gpu_value    = jce_ui_find_element(hud->ui, hud->doc, "gpu-value");
    hud->el_cpu_value    = jce_ui_find_element(hud->ui, hud->doc, "cpu-value");
    hud->el_vram_value   = jce_ui_find_element(hud->ui, hud->doc, "vram-value");
    hud->el_ram_value    = jce_ui_find_element(hud->ui, hud->doc, "ram-value");
    hud->el_api_value    = jce_ui_find_element(hud->ui, hud->doc, "api-value");
    hud->el_api_detail   = jce_ui_find_element(hud->ui, hud->doc, "api-detail");
    hud->el_fps          = jce_ui_find_element(hud->ui, hud->doc, "fps");
    hud->el_frametime    = jce_ui_find_element(hud->ui, hud->doc, "frametime");
    hud->el_frametime_ms = jce_ui_find_element(hud->ui, hud->doc, "frametime-ms");
    hud->el_graph        = jce_ui_find_element(hud->ui, hud->doc, "frametime-graph");
    hud->el_extra_status = jce_ui_find_element(hud->ui, hud->doc, "extra-status");
    hud->el_shortcut_hints = jce_ui_find_element(hud->ui, hud->doc, "shortcut-hints");

    /* Apply font family if specified. */
    if (desc->font_family)
        jce_debug_hud_set_font_family(hud, desc->font_family);

    LOG_INFO(LOG_TAG, "debug HUD created");
    return hud;
}

void jce_debug_hud_destroy(JceDebugHud *hud)
{
    if (!hud) return;
    if (hud->ui && jce_ui_doc_valid(hud->doc))
        jce_ui_doc_close(hud->ui, hud->doc);
    JCE_FREE(hud);
}

void jce_debug_hud_show(JceDebugHud *hud)
{
    if (!hud || !hud->ui) return;
    hud->visible = true;
    jce_ui_doc_show(hud->ui, hud->doc);
}

void jce_debug_hud_hide(JceDebugHud *hud)
{
    if (!hud || !hud->ui) return;
    hud->visible = false;
    jce_ui_doc_hide(hud->ui, hud->doc);
}

bool jce_debug_hud_is_visible(const JceDebugHud *hud)
{
    return hud ? hud->visible : false;
}

void jce_debug_hud_update(JceDebugHud *hud, const JceDebugHudData *data)
{
    if (!hud || !hud->ui || !data) return;

    char buf[256];
    char detail[128];
    const bgfx_stats_t *stats = bgfx_get_stats();
    int64_t gpu_used_mb = 0;
    int64_t gpu_max_mb = 0;

    if (stats) {
        gpu_used_mb = stats->gpuMemoryUsed / (1024 * 1024);
        gpu_max_mb  = stats->gpuMemoryMax / (1024 * 1024);
        if (gpu_used_mb <= 0)
            gpu_used_mb = (stats->textureMemoryUsed + stats->rtMemoryUsed)
                        / (1024 * 1024);
    }

    /* GPU adapter. */
    if (jce_ui_elem_valid(hud->el_gpu_value)) {
        format_gpu_value(buf, sizeof(buf), jce_renderer_get_gpu_name(hud->renderer));
        jce_ui_elem_set_text(hud->ui, hud->el_gpu_value, buf);
    }

    /* CPU usage. */
    if (jce_ui_elem_valid(hud->el_cpu_value)) {
        snprintf(buf, sizeof(buf), "%.0f%%  %dC", data->cpu_usage,
                 data->cpu_cores);
        jce_ui_elem_set_text(hud->ui, hud->el_cpu_value, buf);
    }

    /* VRAM usage. */
    if (jce_ui_elem_valid(hud->el_vram_value)) {
        format_gb_value(buf, sizeof(buf), gpu_used_mb);
        jce_ui_elem_set_text(hud->ui, hud->el_vram_value, buf);
    }

    /* RAM usage. */
    if (jce_ui_elem_valid(hud->el_ram_value)) {
        format_gb_value(buf, sizeof(buf), data->ram_used_mb);
        jce_ui_elem_set_text(hud->ui, hud->el_ram_value, buf);
    }

    /* Renderer backend + detail line. */
    if (jce_ui_elem_valid(hud->el_api_value))
        jce_ui_elem_set_text(hud->ui, hud->el_api_value,
                             jce_renderer_get_backend_name(hud->renderer));
    if (jce_ui_elem_valid(hud->el_api_detail)) {
        uint32_t w, h;
        jce_window_get_size(hud->window, &w, &h);
        bool vsync = jce_renderer_get_vsync(hud->renderer);
        if (gpu_max_mb > 0) {
            snprintf(detail, sizeof(detail),
                     "%ux%u  VSYNC %s  VRAM %.2f/%.2f GB",
                     w, h, vsync ? "ON" : "OFF",
                     (double)gpu_used_mb / 1024.0,
                     (double)gpu_max_mb / 1024.0);
        } else {
            snprintf(detail, sizeof(detail), "%ux%u  VSYNC %s",
                     w, h, vsync ? "ON" : "OFF");
        }
        jce_ui_elem_set_text(hud->ui, hud->el_api_detail, detail);
    }

    /* FPS with dynamic color. */
    if (jce_ui_elem_valid(hud->el_fps)) {
        snprintf(buf, sizeof(buf), "%.0f FPS", data->fps);
        jce_ui_elem_set_text(hud->ui, hud->el_fps, buf);
        const char *color;
        if (data->fps >= 55.0f)       color = "#00cc00";
        else if (data->fps >= 30.0f)  color = "#cccc00";
        else                           color = "#cc0000";
        jce_ui_elem_set_property(hud->ui, hud->el_fps, "color", color);
    }

    /* Frametime. */
    if (jce_ui_elem_valid(hud->el_frametime)) {
        snprintf(buf, sizeof(buf), "%.1f ms", data->frametime_ms);
        jce_ui_elem_set_text(hud->ui, hud->el_frametime, buf);
    }
    if (jce_ui_elem_valid(hud->el_frametime_ms)) {
        snprintf(buf, sizeof(buf), "%.1f ms", data->frametime_ms);
        jce_ui_elem_set_text(hud->ui, hud->el_frametime_ms, buf);
    }

    hud->frametime_history = data->frametime_history;
    hud->frametime_history_count = data->frametime_history_count;
    hud->frametime_history_head = data->frametime_history_head;
    hud->frametime_graph_columns = data->frametime_graph_columns;

    /* Extra status line (wireframe indicator, language, etc.). */
    if (jce_ui_elem_valid(hud->el_extra_status)) {
        if (data->extra_status && data->extra_status[0]) {
            jce_ui_elem_set_text(hud->ui, hud->el_extra_status,
                                 data->extra_status);
            jce_ui_elem_set_property(hud->ui, hud->el_extra_status,
                                     "display", "block");
        } else {
            jce_ui_elem_set_text(hud->ui, hud->el_extra_status, "");
            jce_ui_elem_set_property(hud->ui, hud->el_extra_status,
                                     "display", "none");
        }
    }

    if (jce_ui_elem_valid(hud->el_shortcut_hints)) {
        if (data->shortcut_hints && data->shortcut_hints[0]) {
            jce_ui_elem_set_text(hud->ui, hud->el_shortcut_hints,
                                 data->shortcut_hints);
            jce_ui_elem_set_property(hud->ui, hud->el_shortcut_hints,
                                     "display", "block");
        } else {
            jce_ui_elem_set_text(hud->ui, hud->el_shortcut_hints, "");
            jce_ui_elem_set_property(hud->ui, hud->el_shortcut_hints,
                                     "display", "none");
        }
    }
}

void jce_debug_hud_draw(JceDebugHud *hud)
{
    static const float graph_max_ms = 50.0f;
    static const float ref_60fps_ms = 16.67f;
    static const float ref_30fps_ms = 33.3f;

    JceUIRect graph_bounds;
    int visible_count;
    float x;
    float y;
    float w;
    float h;

    if (!hud || !hud->visible || !hud->renderer || !hud->ui)
        return;
    if (!jce_ui_elem_valid(hud->el_graph))
        return;
    if (!jce_ui_elem_get_bounds(hud->ui, hud->el_graph, &graph_bounds))
        return;

    visible_count = debug_hud_visible_sample_count(hud);
    if (visible_count < 1)
        return;

    x = graph_bounds.x + 4.0f;
    y = graph_bounds.y + 4.0f;
    w = graph_bounds.w - 8.0f;
    h = graph_bounds.h - 8.0f;
    if (w < 8.0f || h < 8.0f)
        return;

    /* Reference lines (60 fps yellow, 30 fps red). */
    {
        const float line_60fps = debug_hud_graph_y(y, h, ref_60fps_ms, graph_max_ms);
        const float line_30fps = debug_hud_graph_y(y, h, ref_30fps_ms, graph_max_ms);
        jce_draw_filled_rect(hud->renderer, x, line_60fps, w, 1.0f,
                             jce_rgba(240, 197, 66, 110));
        jce_draw_filled_rect(hud->renderer, x, line_30fps, w, 1.0f,
                             jce_rgba(255, 106, 94, 140));
    }

    /* MangoHud-style polyline (thin colored line, no markers, no fill). */
    {
        const int first = (hud->frametime_history_head - visible_count
                         + hud->frametime_history_count) % hud->frametime_history_count;
        const float step_x = (visible_count > 1)
                           ? w / (float)(visible_count - 1)
                           : 0.0f;

        for (int i = 0; i < visible_count - 1; ++i) {
            const int idx0 = (first + i)     % hud->frametime_history_count;
            const int idx1 = (first + i + 1) % hud->frametime_history_count;
            float ms0 = hud->frametime_history[idx0];
            float ms1 = hud->frametime_history[idx1];
            if (ms0 < 0.0f) ms0 = 0.0f;
            if (ms1 < 0.0f) ms1 = 0.0f;

            float seg[4];
            seg[0] = x + step_x * (float)i;
            seg[1] = debug_hud_graph_y(y, h, ms0, graph_max_ms);
            seg[2] = x + step_x * (float)(i + 1);
            seg[3] = debug_hud_graph_y(y, h, ms1, graph_max_ms);

            const float worst = ms0 > ms1 ? ms0 : ms1;
            uint32_t color;
            if (worst >= ref_30fps_ms)
                color = jce_rgba(255, 106, 94, 240);     /* red    < 30 fps */
            else if (worst >= ref_60fps_ms)
                color = jce_rgba(240, 197, 66, 235);     /* yellow < 60 fps */
            else
                color = jce_rgba(60, 226, 85, 230);      /* green  >= 60 fps */

            jce_draw_polyline(hud->renderer, seg, 2, color);
        }
    }
}

void jce_debug_hud_set_font_family(JceDebugHud *hud, const char *family)
{
    if (!hud || !hud->ui || !family) return;
    JceUIElementHandle body = jce_ui_doc_get_body(hud->ui, hud->doc);
    if (jce_ui_elem_valid(body))
        jce_ui_elem_set_property(hud->ui, body, "font-family", family);
}

JceUIDocHandle jce_debug_hud_get_doc(const JceDebugHud *hud)
{
    return hud ? hud->doc : JCE_UI_DOC_INVALID;
}
