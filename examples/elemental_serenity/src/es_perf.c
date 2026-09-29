/*
 * es_perf.c — on-screen performance panel (win-x64 vs wasm comparison).
 *
 * A small bottom-left ECS-UI cluster (own canvas, sort above the HUD)
 * refreshed 4x/sec from jce_renderer_get_gpu_stats() — the engine's public
 * stats surface — plus an app-side frame-time window (avg + worst).  The
 * backend name in the title row is the platform tag (D3D12 / D3D11 /
 * Vulkan / OpenGL / OpenGL ES a.k.a. WebGL2), so screenshots from the two
 * builds are directly comparable.  P toggles visibility.
 *
 * GPU time reads "n/a" where the backend cannot report it (WebGL2 has no
 * timer queries through bgfx) — expected, not a bug.
 */
#include "es_perf.h"

#include <jce/api.h>

#include <stdio.h>
#include <string.h>

#define ES_PERF_LINES     5
#define ES_PERF_INTERVAL  0.25f   /* text refresh cadence (seconds) */

typedef struct {
    JceEntity bg;
    JceEntity line[ES_PERF_LINES];
    bool      built;
    bool      visible;
    /* frame-time window (reset each refresh) */
    float     acc_t;
    int       acc_frames;
    float     acc_worst;
} EsPerf;

static EsPerf g_perf;

/* Bottom-left anchoring: uc_resolve_rect's parent origin is TOP-LEFT with +Y
 * DOWN, so anchor/pivot y = 1.0 pins to the BOTTOM edge and a NEGATIVE
 * anchored y offsets upward.  x uses anchor/pivot 0 (left edge). */
static JceEntity es_perf_text_line(JceScene *sc, JceEntity canvas,
                                   const char *name, float y)
{
    JceEntity e = jce_scene_create_entity(sc, name);
    JceUITextComponent t;
    jce_scene_set_parent(sc, e, canvas);
    memset(&t, 0, sizeof t);
    t.text[0] = '\0';
    t.font_size = 13.0f;
    t.alignment = 0;                   /* LEFT */
    t.color[0] = t.color[1] = t.color[2] = 1.0f; t.color[3] = 0.95f;
    t.line_spacing = 1.0f;
    t.rect.anchor_min[0] = 0.0f; t.rect.anchor_min[1] = 1.0f;
    t.rect.anchor_max[0] = 0.0f; t.rect.anchor_max[1] = 1.0f;
    t.rect.pivot[0]      = 0.0f; t.rect.pivot[1]      = 1.0f;
    t.rect.anchored_position[0] = 16.0f;
    t.rect.anchored_position[1] = y;
    t.rect.size_delta[0] = 300.0f;
    t.rect.size_delta[1] = 20.0f;
    jce_scene_set_ui_text(sc, e, &t);
    return e;
}

void es_perf_init(JceScene *scene)
{
    JceEntity canvas;
    JceCanvasComponent cv;
    JceUIImageComponent bi;
    int i;

    memset(&g_perf, 0, sizeof g_perf);
    if (!scene) return;

    canvas = jce_scene_create_entity(scene, "PerfCanvas");
    memset(&cv, 0, sizeof cv);
    cv.render_mode = 0;   /* OVERLAY */
    cv.sort_order  = 110; /* above the season HUD and the AI panel (100) */
    cv.reference_resolution[0] = 1280.0f;
    cv.reference_resolution[1] = 720.0f;
    cv.scale_factor = 1.0f;
    jce_scene_set_canvas(scene, canvas, &cv);

    /* Dark backdrop (created first = drawn behind the text siblings). */
    g_perf.bg = jce_scene_create_entity(scene, "PerfBackdrop");
    jce_scene_set_parent(scene, g_perf.bg, canvas);
    memset(&bi, 0, sizeof bi);
    bi.image_type = 0;
    bi.color[0] = bi.color[1] = bi.color[2] = 0.04f; bi.color[3] = 0.60f;
    bi.raycast_target = false;   /* never eat scene clicks */
    bi.rect.anchor_min[0] = 0.0f; bi.rect.anchor_min[1] = 1.0f;
    bi.rect.anchor_max[0] = 0.0f; bi.rect.anchor_max[1] = 1.0f;
    bi.rect.pivot[0]      = 0.0f; bi.rect.pivot[1]      = 1.0f;
    bi.rect.anchored_position[0] = 8.0f;
    bi.rect.anchored_position[1] = -6.0f;
    bi.rect.size_delta[0] = 320.0f;
    bi.rect.size_delta[1] = 96.0f;
    jce_scene_set_ui_image(scene, g_perf.bg, &bi);

    for (i = 0; i < ES_PERF_LINES; ++i) {
        char nm[32];
        /* line 0 at the top of the backdrop, line 4 at the bottom. */
        float y = -(6.0f + 96.0f) + 6.0f + 17.0f * (float)i + 20.0f;
        snprintf(nm, sizeof nm, "PerfLine%d", i);
        g_perf.line[i] = es_perf_text_line(scene, canvas, nm, y);
    }

    g_perf.built   = true;
    g_perf.visible = true;
}

static void es_perf_set_line(JceScene *sc, int i, const char *txt)
{
    JceUITextComponent *t;
    if (!g_perf.built) return;
    t = jce_scene_get_ui_text(sc, g_perf.line[i]);
    if (!t) return;
    snprintf(t->text, sizeof t->text, "%s", txt);
}

static void es_perf_apply_visibility(JceScene *sc)
{
    JceUIImageComponent *im = jce_scene_get_ui_image(sc, g_perf.bg);
    if (im) im->color[3] = g_perf.visible ? 0.60f : 0.0f;
    if (!g_perf.visible) {
        int i;
        for (i = 0; i < ES_PERF_LINES; ++i)
            es_perf_set_line(sc, i, "");
    }
}

void es_perf_update(JceScene *scene, float dt, const JceInput *in)
{
    if (!g_perf.built || !scene) return;

    if (in && jce_input_key_pressed(in, JCE_KEY_P)) {
        g_perf.visible = !g_perf.visible;
        es_perf_apply_visibility(scene);
    }

    /* frame-time window */
    g_perf.acc_t      += dt;
    g_perf.acc_frames += 1;
    if (dt > g_perf.acc_worst) g_perf.acc_worst = dt;

    if (!g_perf.visible) return;
    if (g_perf.acc_t < ES_PERF_INTERVAL || g_perf.acc_frames <= 0) return;

    {
        float fps     = (float)g_perf.acc_frames / g_perf.acc_t;
        float avg_ms  = 1000.0f * g_perf.acc_t / (float)g_perf.acc_frames;
        float worst_ms = 1000.0f * g_perf.acc_worst;
        JceGpuStats st;
        char buf[192];
        bool have = jce_renderer_get_gpu_stats(&st);

        snprintf(buf, sizeof buf, "PERF  %s  %ux%u",
                 jce_renderer_backend_name(jce_renderer_get_active_backend()),
                 have ? st.backbuffer_width  : 0u,
                 have ? st.backbuffer_height : 0u);
        es_perf_set_line(scene, 0, buf);

        snprintf(buf, sizeof buf, "fps %5.1f   frame %5.2f ms (max %5.2f)",
                 fps, avg_ms, worst_ms);
        es_perf_set_line(scene, 1, buf);

        if (have && st.valid && st.gpu_ms > 0.0)
            snprintf(buf, sizeof buf, "cpu %5.2f ms   gpu %5.2f ms",
                     st.cpu_frame_ms, st.gpu_ms);
        else if (have)
            snprintf(buf, sizeof buf, "cpu %5.2f ms   gpu n/a",
                     st.cpu_frame_ms);
        else
            snprintf(buf, sizeof buf, "cpu n/a   gpu n/a");
        es_perf_set_line(scene, 2, buf);

        snprintf(buf, sizeof buf, "draw %u   prog %u   tex %u   fb %u",
                 have ? st.num_draw : 0u, have ? st.num_programs : 0u,
                 have ? st.num_textures : 0u, have ? st.num_frame_buffers : 0u);
        es_perf_set_line(scene, 3, buf);

        if (have && st.texture_memory_used >= 0)
            snprintf(buf, sizeof buf, "texmem %.1f MB   rt %.1f MB   [P] hide",
                     (double)st.texture_memory_used / (1024.0 * 1024.0),
                     st.rt_memory_used >= 0
                         ? (double)st.rt_memory_used / (1024.0 * 1024.0) : 0.0);
        else
            snprintf(buf, sizeof buf, "texmem n/a   [P] hide");
        es_perf_set_line(scene, 4, buf);

        g_perf.acc_t = 0.0f; g_perf.acc_frames = 0; g_perf.acc_worst = 0.0f;
    }
}
