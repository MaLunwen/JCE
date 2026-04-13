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
};

/* ── Create / Destroy ──────────────────────────────────────────────── */

JceUIContext *jce_ui_create(const JceUIContextDesc *desc, jce_allocator_t alloc)
{
    if (!desc) return NULL;

    JceUIContext *ctx = (JceUIContext *)alloc.alloc(sizeof(JceUIContext), alloc.ctx);
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc  = alloc;
    ctx->width  = desc->width;
    ctx->height = desc->height;

    ctx->backend = jce_rml_create(desc->width, desc->height,
                                  desc->renderer, desc->pak);
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

/* ── Font loading ──────────────────────────────────────────────────── */

bool jce_ui_load_font(JceUIContext *ctx, const char *pak_path)
{
    if (!ctx || !pak_path) return false;
    return jce_rml_load_font(ctx->backend, pak_path);
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

const char *jce_ui_elem_get_value(JceUIContext *ctx, JceUIElementHandle elem)
{
    if (!ctx || !jce_ui_elem_valid(elem)) return "";
    return jce_rml_elem_get_value(ctx->backend, elem.idx);
}

void jce_ui_elem_set_value(JceUIContext *ctx, JceUIElementHandle elem,
                           const char *value)
{
    if (!ctx || !jce_ui_elem_valid(elem) || !value) return;
    jce_rml_elem_set_value(ctx->backend, elem.idx, value);
}

const char *jce_ui_elem_get_attribute(JceUIContext *ctx, JceUIElementHandle elem,
                                      const char *attribute)
{
    if (!ctx || !jce_ui_elem_valid(elem) || !attribute) return "";
    return jce_rml_elem_get_attribute(ctx->backend, elem.idx, attribute);
}

void jce_ui_elem_set_attribute(JceUIContext *ctx, JceUIElementHandle elem,
                               const char *attribute, const char *value)
{
    if (!ctx || !jce_ui_elem_valid(elem) || !attribute) return;
    jce_rml_elem_set_attribute(ctx->backend, elem.idx, attribute, value);
}

void jce_ui_elem_remove_attribute(JceUIContext *ctx, JceUIElementHandle elem,
                                  const char *attribute)
{
    if (!ctx || !jce_ui_elem_valid(elem) || !attribute) return;
    jce_rml_elem_remove_attribute(ctx->backend, elem.idx, attribute);
}

void jce_ui_elem_set_inner_rml(JceUIContext *ctx, JceUIElementHandle elem,
                               const char *rml)
{
    if (!ctx || !jce_ui_elem_valid(elem) || !rml) return;
    jce_rml_elem_set_inner_rml(ctx->backend, elem.idx, rml);
}

bool jce_ui_elem_get_bounds(JceUIContext *ctx, JceUIElementHandle elem,
                            JceUIRect *out_rect)
{
    if (!ctx || !jce_ui_elem_valid(elem) || !out_rect) return false;
    return jce_rml_elem_get_bounds(ctx->backend, elem.idx,
                                   &out_rect->x, &out_rect->y,
                                   &out_rect->w, &out_rect->h);
}

JceUIElementHandle jce_ui_doc_get_body(JceUIContext *ctx, JceUIDocHandle doc)
{
    if (!ctx || !jce_ui_doc_valid(doc)) return JCE_UI_ELEM_INVALID;
    uint32_t idx = jce_rml_doc_get_body(ctx->backend, doc.idx);
    if (idx == UINT32_MAX) return JCE_UI_ELEM_INVALID;
    return (JceUIElementHandle){ idx };
}

/* ── Event callbacks ───────────────────────────────────────────────── */

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

void jce_ui_process_pointer_input(JceUIContext *ctx, const JceInput *input)
{
    if (!ctx) return;
    jce_rml_process_pointer_input(ctx->backend, input);
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
