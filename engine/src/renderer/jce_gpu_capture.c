/*
 * jce_gpu_capture.c  RenderDoc in-application GPU-frame-capture trigger.
 *
 * See jce/renderer/jce_gpu_capture.h. Engine-owned so every consumer (editor,
 * games, headless QA) can capture a frame; placed beside the renderer because
 * the capture wraps the backbuffer swap in jce_renderer_end_frame(). The
 * RenderDoc module is located through the jce_library shared-object abstraction
 * (SDL3-backed), so no <windows.h>/<dlfcn.h> is pulled into the renderer layer.
 *
 * Built only when JCE_RENDERDOC_ENABLED=1 (forced 0 for dist, mirroring Tracy);
 * otherwise this is a single empty no-op function with zero footprint.
 */

#include <jce/renderer/jce_gpu_capture.h>

#if defined(JCE_RENDERDOC_ENABLED) && (JCE_RENDERDOC_ENABLED + 0 == 1)

#include <jce/os/platform/jce_library.h>
#include <jce/os/core/jce_log.h>

#include "third_party/renderdoc_app.h"

#include <stdint.h>
#include <stdlib.h>   /* getenv, atoi */

#define LOG_TAG "gpu_capture"

static RENDERDOC_API_1_4_0 *s_api    = NULL;
static int                  s_target = -2;  /* -2 unparsed, -1 disabled, >=0 frame */
static uint32_t             s_tick   = 0;
static bool                 s_done   = false;

/* Parse JCE_RDOC_FRAME once and, if armed, grab the injected RenderDoc API.
 * The module is resident only when launched under a RenderDoc capture; probe
 * the platform-native names and take the first that resolves the entry point.
 * The handle is kept resident on purpose (closing it could unload RenderDoc out
 * from under itself) — a one-shot, dev-only intentional leak. */
static void gpu_capture_lazy_init(void)
{
    const char *f = getenv("JCE_RDOC_FRAME");
    s_target = (f && f[0]) ? atoi(f) : -1;
    if (s_target < 0)
        return;

    static const char *const k_names[] = {
        "renderdoc.dll", "librenderdoc.so", "librenderdoc.dylib"
    };
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); ++i) {
        JceLibrary lib = jce_library_open(k_names[i]);
        if (!lib)
            continue;
        pRENDERDOC_GetAPI get_api =
            (pRENDERDOC_GetAPI)jce_library_symbol(lib, "RENDERDOC_GetAPI");
        if (get_api)
            get_api(eRENDERDOC_API_Version_1_4_0, (void **)&s_api);
        if (s_api)
            break;
        jce_library_close(lib);
    }
    LOG_INFO(LOG_TAG, "JCE_RDOC_FRAME=%d renderdoc_api=%p",
             s_target, (void *)s_api);
}

void JCE_CALL jce_gpu_capture_tick(void)
{
    if (s_target == -2)
        gpu_capture_lazy_init();
    if (s_target < 0 || s_done || !s_api)
        return;
    if (++s_tick >= (uint32_t)s_target) {
        s_api->TriggerCapture();
        s_done = true;
        LOG_INFO(LOG_TAG, "TriggerCapture at frame %u", s_tick);
    }
}

#else /* RenderDoc disabled — no-op, zero footprint in shipped builds */

void JCE_CALL jce_gpu_capture_tick(void) { }

#endif
