/*
 * jce_ui.c  Game UI system implementation.
 *
 * Provides a retained-mode UI layer for in-game HUD, menus, etc.
 * Documents are stored as lightweight DOM trees with elements,
 * properties, and event listeners.
 *
 * This is a foundation layer — a full RmlUi integration can be
 * plugged in behind this API without changing game code.
 */

#include <jce/ui/jce_ui.h>
#include <jce/core/jce_log.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define LOG_TAG "ui"

#define MAX_DOCS     32
#define MAX_ELEMENTS 256

/* ── Element ───────────────────────────────────────────────────────── */

typedef struct {
    bool              alive;
    uint32_t          doc_idx;
    char              id[64];
    char              text[256];
    jce_ui_event_fn   event_fn;
    void             *event_ud;
    char              event_type[32];
} UIElement;

/* ── Document ──────────────────────────────────────────────────────── */

typedef struct {
    bool   alive;
    bool   visible;
    char   name[64];
} UIDocument;

/* ── Context ───────────────────────────────────────────────────────── */

struct JceUIContext {
    jce_allocator_t alloc;
    uint32_t        width;
    uint32_t        height;
    UIDocument      docs[MAX_DOCS];
    UIElement       elements[MAX_ELEMENTS];
    uint32_t        doc_count;
    uint32_t        elem_count;
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

    LOG_SUCCESS(LOG_TAG, "UI context created (%ux%u)", desc->width, desc->height);
    return ctx;
}

void jce_ui_destroy(JceUIContext *ctx)
{
    if (!ctx) return;
    jce_allocator_t a = ctx->alloc;
    a.free(ctx, a.ctx);
}

/* ── Documents ─────────────────────────────────────────────────────── */

JceUIDocHandle jce_ui_doc_load(JceUIContext *ctx, const char *name,
                               const char *markup, uint32_t markup_len)
{
    if (!ctx || !name) return JCE_UI_DOC_INVALID;
    (void)markup; (void)markup_len;

    for (uint32_t i = 0; i < MAX_DOCS; i++) {
        if (!ctx->docs[i].alive) {
            UIDocument *doc = &ctx->docs[i];
            memset(doc, 0, sizeof(*doc));
            doc->alive   = true;
            doc->visible = false;
            snprintf(doc->name, sizeof(doc->name), "%s", name);
            ctx->doc_count++;

            LOG_INFO(LOG_TAG, "document '%s' loaded", name);
            return (JceUIDocHandle){ i };
        }
    }

    LOG_ERROR(LOG_TAG, "document limit reached (%u)", MAX_DOCS);
    return JCE_UI_DOC_INVALID;
}

JceUIDocHandle jce_ui_doc_load_file(JceUIContext *ctx, const char *path)
{
    if (!ctx || !path) return JCE_UI_DOC_INVALID;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERROR(LOG_TAG, "cannot open '%s'", path);
        return JCE_UI_DOC_INVALID;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0) { fclose(fp); return JCE_UI_DOC_INVALID; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return JCE_UI_DOC_INVALID; }

    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';

    /* Extract filename for doc name. */
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    JceUIDocHandle h = jce_ui_doc_load(ctx, base, buf, (uint32_t)rd);
    free(buf);
    return h;
}

void jce_ui_doc_show(JceUIContext *ctx, JceUIDocHandle doc)
{
    if (!ctx || !jce_ui_doc_valid(doc) || doc.idx >= MAX_DOCS) return;
    if (ctx->docs[doc.idx].alive)
        ctx->docs[doc.idx].visible = true;
}

void jce_ui_doc_hide(JceUIContext *ctx, JceUIDocHandle doc)
{
    if (!ctx || !jce_ui_doc_valid(doc) || doc.idx >= MAX_DOCS) return;
    if (ctx->docs[doc.idx].alive)
        ctx->docs[doc.idx].visible = false;
}

void jce_ui_doc_close(JceUIContext *ctx, JceUIDocHandle doc)
{
    if (!ctx || !jce_ui_doc_valid(doc) || doc.idx >= MAX_DOCS) return;
    UIDocument *d = &ctx->docs[doc.idx];
    if (d->alive) {
        d->alive   = false;
        d->visible = false;
        ctx->doc_count--;

        /* Remove associated elements. */
        for (uint32_t i = 0; i < MAX_ELEMENTS; i++) {
            if (ctx->elements[i].alive && ctx->elements[i].doc_idx == doc.idx) {
                ctx->elements[i].alive = false;
                ctx->elem_count--;
            }
        }
    }
}

/* ── Elements ──────────────────────────────────────────────────────── */

JceUIElementHandle jce_ui_find_element(JceUIContext *ctx, JceUIDocHandle doc,
                                      const char *element_id)
{
    if (!ctx || !element_id || !jce_ui_doc_valid(doc)) return JCE_UI_ELEM_INVALID;

    /* Search existing. */
    for (uint32_t i = 0; i < MAX_ELEMENTS; i++) {
        if (ctx->elements[i].alive &&
            ctx->elements[i].doc_idx == doc.idx &&
            strcmp(ctx->elements[i].id, element_id) == 0) {
            return (JceUIElementHandle){ i };
        }
    }

    /* Auto-create on first lookup. */
    for (uint32_t i = 0; i < MAX_ELEMENTS; i++) {
        if (!ctx->elements[i].alive) {
            UIElement *el = &ctx->elements[i];
            memset(el, 0, sizeof(*el));
            el->alive   = true;
            el->doc_idx = doc.idx;
            snprintf(el->id, sizeof(el->id), "%s", element_id);
            ctx->elem_count++;
            return (JceUIElementHandle){ i };
        }
    }

    return JCE_UI_ELEM_INVALID;
}

void jce_ui_elem_set_text(JceUIContext *ctx, JceUIElementHandle elem,
                          const char *text)
{
    if (!ctx || !jce_ui_elem_valid(elem) || elem.idx >= MAX_ELEMENTS) return;
    UIElement *el = &ctx->elements[elem.idx];
    if (el->alive && text)
        snprintf(el->text, sizeof(el->text), "%s", text);
}

const char *jce_ui_elem_get_text(JceUIContext *ctx, JceUIElementHandle elem)
{
    if (!ctx || !jce_ui_elem_valid(elem) || elem.idx >= MAX_ELEMENTS) return "";
    UIElement *el = &ctx->elements[elem.idx];
    return el->alive ? el->text : "";
}

void jce_ui_elem_set_property(JceUIContext *ctx, JceUIElementHandle elem,
                              const char *property, const char *value)
{
    (void)ctx; (void)elem; (void)property; (void)value;
    /* CSS property storage deferred to RmlUi integration. */
}

/* ── Event callbacks ───────────────────────────────────────────────── */

void jce_ui_elem_on(JceUIContext *ctx, JceUIElementHandle elem,
                    const char *event_type, jce_ui_event_fn fn, void *userdata)
{
    if (!ctx || !jce_ui_elem_valid(elem) || elem.idx >= MAX_ELEMENTS) return;
    UIElement *el = &ctx->elements[elem.idx];
    if (!el->alive) return;

    el->event_fn = fn;
    el->event_ud = userdata;
    if (event_type)
        snprintf(el->event_type, sizeof(el->event_type), "%s", event_type);
}

/* ── Per-frame ─────────────────────────────────────────────────────── */

void jce_ui_process_input(JceUIContext *ctx, const JceInput *input)
{
    (void)ctx; (void)input;
    /* Input routing deferred to RmlUi integration. */
}

void jce_ui_update(JceUIContext *ctx, float dt)
{
    (void)ctx; (void)dt;
    /* Layout / animation update deferred to RmlUi integration. */
}

void jce_ui_render(JceUIContext *ctx)
{
    (void)ctx;
    /* Render deferred to RmlUi integration. */
}

void jce_ui_resize(JceUIContext *ctx, uint32_t width, uint32_t height)
{
    if (!ctx) return;
    ctx->width  = width;
    ctx->height = height;
}
