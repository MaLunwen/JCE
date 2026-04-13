/*
 * jce_ui.c  Game UI system implementation.
 *
 * Provides a retained-mode UI layer for in-game HUD, menus, etc.
 * All real work is delegated to the RmlUi C++ backend via
 * the jce_ui_backend.h bridge.
 */

#include <jce/ui/jce_ui.h>
#include <jce/core/jce_log.h>
#include "jce_ui_backend.h"

#include <string.h>
#include <stdlib.h>

#define LOG_TAG "ui"

/* ── Context ───────────────────────────────────────────────────────── */

struct JceUIContext {
    jce_allocator_t  alloc;
    JceRmlBackend   *backend;
    uint32_t         width;
    uint32_t         height;
    float            dpi_scale;
};

/* ── Create / Destroy ──────────────────────────────────────────────── */

JceUIContext *jce_ui_create(const JceUIContextDesc *desc, jce_allocator_t alloc)
{
    if (!desc) return NULL;

    JceUIContext *ctx = (JceUIContext *)alloc.alloc(sizeof(JceUIContext), alloc.ctx);
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc     = alloc;
    ctx->width     = desc->width;
    ctx->height    = desc->height;
    ctx->dpi_scale = 1.0f;

    ctx->backend = jce_rml_create(desc->width, desc->height);
    if (!ctx->backend) {
        LOG_ERROR(LOG_TAG, "failed to create RmlUi backend");
        alloc.free(ctx, alloc.ctx);
        return NULL;
    }

    LOG_SUCCESS(LOG_TAG, "UI context created (%ux%u)", desc->width, desc->height);
    return ctx;
}

void jce_ui_destroy(JceUIContext *ctx)
{
    if (!ctx) return;

    jce_rml_destroy(ctx->backend);
    ctx->backend = NULL;

    jce_allocator_t a = ctx->alloc;
    a.free(ctx, a.ctx);
}

/* ── Documents ─────────────────────────────────────────────────────── */

JceUIDocHandle jce_ui_doc_load(JceUIContext *ctx, const char *name,
                               const char *markup, uint32_t markup_len)
{
    if (!ctx || !name) return JCE_UI_DOC_INVALID;

    uint32_t idx = jce_rml_doc_load(ctx->backend, name, markup, markup_len);
    if (idx == UINT32_MAX) return JCE_UI_DOC_INVALID;

    return (JceUIDocHandle){ idx };
}

JceUIDocHandle jce_ui_doc_load_file(JceUIContext *ctx, const char *path)
{
    if (!ctx || !path) return JCE_UI_DOC_INVALID;

    uint32_t idx = jce_rml_doc_load_file(ctx->backend, path);
    if (idx == UINT32_MAX) return JCE_UI_DOC_INVALID;

    return (JceUIDocHandle){ idx };
}

void jce_ui_doc_show(JceUIContext *ctx, JceUIDocHandle doc)
{
    if (!ctx || !jce_ui_doc_valid(doc)) return;
    jce_rml_doc_show(ctx->backend, doc.idx);
}

void jce_ui_doc_hide(JceUIContext *ctx, JceUIDocHandle doc)
{
    if (!ctx || !jce_ui_doc_valid(doc)) return;
    jce_rml_doc_hide(ctx->backend, doc.idx);
}

void jce_ui_doc_close(JceUIContext *ctx, JceUIDocHandle doc)
{
    if (!ctx || !jce_ui_doc_valid(doc)) return;
    jce_rml_doc_close(ctx->backend, doc.idx);
}

/* ── Elements ──────────────────────────────────────────────────────── */

JceUIElementHandle jce_ui_find_element(JceUIContext *ctx, JceUIDocHandle doc,
                                      const char *element_id)
{
    if (!ctx || !element_id || !jce_ui_doc_valid(doc))
        return JCE_UI_ELEM_INVALID;

    uint32_t idx = jce_rml_find_element(ctx->backend, doc.idx, element_id);
    if (idx == UINT32_MAX) return JCE_UI_ELEM_INVALID;

    return (JceUIElementHandle){ idx };
}

void jce_ui_elem_set_text(JceUIContext *ctx, JceUIElementHandle elem,
                          const char *text)
{
    if (!ctx || !jce_ui_elem_valid(elem)) return;
    jce_rml_elem_set_text(ctx->backend, elem.idx, text);
}

const char *jce_ui_elem_get_text(JceUIContext *ctx, JceUIElementHandle elem)
{
    if (!ctx || !jce_ui_elem_valid(elem)) return "";
    return jce_rml_elem_get_text(ctx->backend, elem.idx);
}

void jce_ui_elem_set_property(JceUIContext *ctx, JceUIElementHandle elem,
                              const char *property, const char *value)
{
    if (!ctx || !jce_ui_elem_valid(elem)) return;
    jce_rml_elem_set_property(ctx->backend, elem.idx, property, value);
}

/* ── Event callbacks ───────────────────────────────────────────────── */

/*
 * Adapter: the public jce_ui_event_fn receives a JceUIElementHandle,
 * while the backend bridge uses a raw uint32_t index.  We store the
 * original callback+userdata and translate.
 */

typedef struct {
    jce_ui_event_fn fn;
    void           *userdata;
} UIEventCBWrapper;

static void ui_event_bridge(uint32_t elem_idx, const char *event_type,
                            void *ud)
{
    UIEventCBWrapper *w = (UIEventCBWrapper *)ud;
    if (w && w->fn) {
        JceUIElementHandle h = { elem_idx };
        w->fn(h, event_type, w->userdata);
    }
}

void jce_ui_elem_on(JceUIContext *ctx, JceUIElementHandle elem,
                    const char *event_type, jce_ui_event_fn fn, void *userdata)
{
    if (!ctx || !jce_ui_elem_valid(elem) || !fn) return;

    /*
     * Allocate a small wrapper that lives for the lifetime of the
     * listener.  In practice these are few and long-lived, so the
     * leak-on-close is acceptable until a proper destroy path exists.
     */
    UIEventCBWrapper *w =
        (UIEventCBWrapper *)ctx->alloc.alloc(sizeof(UIEventCBWrapper),
                                             ctx->alloc.ctx);
    if (!w) {
        LOG_ERROR(LOG_TAG, "failed to allocate event wrapper");
        return;
    }

    w->fn       = fn;
    w->userdata = userdata;

    jce_rml_elem_on(ctx->backend, elem.idx, event_type, ui_event_bridge, w);
}

/* ── Per-frame ─────────────────────────────────────────────────────── */

void jce_ui_process_input(JceUIContext *ctx, const JceInput *input)
{
    if (!ctx) return;
    jce_rml_process_input(ctx->backend, input);
}

void jce_ui_update(JceUIContext *ctx, float dt)
{
    if (!ctx) return;
    jce_rml_update(ctx->backend, dt);
}

void jce_ui_render(JceUIContext *ctx)
{
    if (!ctx) return;
    jce_rml_render(ctx->backend);
}

void jce_ui_resize(JceUIContext *ctx, uint32_t width, uint32_t height)
{
    if (!ctx) return;
    ctx->width  = width;
    ctx->height = height;
    jce_rml_resize(ctx->backend, width, height);
}

void jce_ui_set_dpi_scale(JceUIContext *ctx, float dpi_scale)
{
    if (!ctx || dpi_scale <= 0.0f) return;
    ctx->dpi_scale = dpi_scale;

    /* Forward the DPI scale to the RmlUi backend which applies it
       as dp-ratio on the rendering context. */
    jce_rml_set_dp_ratio(ctx->backend, dpi_scale);
    LOG_DEBUG(LOG_TAG, "DPI scale set to %.2f", (double)dpi_scale);
}

float jce_ui_get_dpi_scale(const JceUIContext *ctx)
{
    return ctx ? ctx->dpi_scale : 1.0f;
}
