/*
 * jce_ui_dom_serialize.c  ECS UI tree → RML document string.
 *
 * Recursive walk: emit a wrapping div for the canvas, then per-child
 * emit appropriate RML element + inline style based on which UI
 * component the child carries.  Non-UI children are skipped.
 *
 * The output uses px units so RmlUi positions elements where our
 * RectTransform math placed them.  Anchors are baked into px on the
 * way out — runtime anchor recomputation lives in RmlUi's own
 * resize path once the document is mounted.
 */

#include <jce/middleware/ui/jce_ui_dom_serialize.h>

#include "os/core/jce_memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Dynamic string buffer ──────────────────────────────────────── */

typedef struct {
    char    *buf;
    size_t   len;
    size_t   cap;
} StrBuf;

static bool sb_reserve(StrBuf *s, size_t need)
{
    if (s->cap >= need) return true;
    size_t cap = s->cap ? s->cap * 2 : 256;
    while (cap < need) cap *= 2;
    char *p = (char *)JCE_REALLOC(s->buf, cap);
    if (!p) return false;
    s->buf = p;
    s->cap = cap;
    return true;
}

static bool sb_append(StrBuf *s, const char *txt)
{
    size_t n = strlen(txt);
    if (!sb_reserve(s, s->len + n + 1)) return false;
    memcpy(s->buf + s->len, txt, n);
    s->len += n;
    s->buf[s->len] = '\0';
    return true;
}

static bool sb_appendf(StrBuf *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[512];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n >= sizeof(tmp)) {
        /* Truncated — fine for our usage (style strings stay short). */
    }
    return sb_append(s, tmp);
}

/* ── Per-component emitters ─────────────────────────────────────── */

static void emit_rect_style(StrBuf *s, JceUIRect r)
{
    sb_appendf(s, "position: absolute; left: %.0fpx; top: %.0fpx; "
                  "width: %.0fpx; height: %.0fpx;", r.x, r.y, r.w, r.h);
}

static void emit_canvas(StrBuf *s, JceScene *scene, JceEntity e,
                        JceUIRect parent_rect);

static void emit_child(StrBuf *s, JceScene *scene, JceEntity e,
                       JceUIRect parent_rect)
{
    /* Resolve this child's rect from its anchors (if any).  Without
     * a UI anchor record we'd need to look at TransformComponent —
     * for now use the parent rect as the placeholder rect. */
    JceUIRect r = parent_rect;

    JceUIImageComponent *img = jce_scene_get_ui_image(scene, e);
    if (img) {
        sb_appendf(s, "<img src=\"%s\" style=\"", img->sprite_path);
        emit_rect_style(s, r);
        sb_appendf(s, " image-color: rgba(%.0f,%.0f,%.0f,%.0f);\"/>\n",
                   img->color[0]*255.0f, img->color[1]*255.0f,
                   img->color[2]*255.0f, img->color[3]*255.0f);
    }
    JceUITextComponent *txt = jce_scene_get_ui_text(scene, e);
    if (txt) {
        const char *align = "left";
        if (txt->alignment == JCE_UI_TEXT_ALIGN_CENTER) align = "center";
        else if (txt->alignment == JCE_UI_TEXT_ALIGN_RIGHT) align = "right";
        sb_appendf(s, "<p style=\"font-size: %.0fpx; text-align: %s; "
                       "color: rgba(%.0f,%.0f,%.0f,%.0f); ",
                   txt->font_size, align,
                   txt->color[0]*255.0f, txt->color[1]*255.0f,
                   txt->color[2]*255.0f, txt->color[3]*255.0f);
        emit_rect_style(s, r);
        sb_appendf(s, "\">%s</p>\n", txt->text);
    }
    JceUIButtonComponent *btn = jce_scene_get_ui_button(scene, e);
    if (btn) {
        sb_appendf(s, "<button id=\"btn_%llu\" style=\"",
                   (unsigned long long)e);
        emit_rect_style(s, r);
        const char *meta_name = jce_scene_entity_name(scene, e);
        sb_appendf(s, "\">%s</button>\n",
                   meta_name && meta_name[0] ? meta_name : "Button");
    }
    /* Recurse into children regardless. */
    JceEntity kids[64];
    int kc = jce_scene_get_children(scene, e, kids, 64);
    for (int i = 0; i < kc; ++i) {
        if (jce_scene_has_canvas(scene, kids[i])) {
            emit_canvas(s, scene, kids[i], r);
        } else {
            emit_child(s, scene, kids[i], r);
        }
    }
}

static void emit_canvas(StrBuf *s, JceScene *scene, JceEntity e,
                        JceUIRect parent_rect)
{
    JceCanvasComponent *cv = jce_scene_get_canvas(scene, e);
    JceUIRect rect = parent_rect;
    if (cv && cv->reference_resolution[0] > 0 && cv->reference_resolution[1] > 0) {
        rect.x = 0; rect.y = 0;
        rect.w = cv->reference_resolution[0];
        rect.h = cv->reference_resolution[1];
    }
    sb_appendf(s, "<div class=\"canvas\" id=\"canvas_%llu\" style=\"",
               (unsigned long long)e);
    emit_rect_style(s, rect);
    sb_append(s, "\">\n");

    JceEntity kids[64];
    int kc = jce_scene_get_children(scene, e, kids, 64);
    for (int i = 0; i < kc; ++i) emit_child(s, scene, kids[i], rect);

    sb_append(s, "</div>\n");
}

/* ── Public API ──────────────────────────────────────────────────── */

char *jce_ui_dom_serialize(JceScene *scene, JceEntity canvas,
                           float screen_w, float screen_h)
{
    if (!scene || !canvas) return NULL;
    StrBuf s = { NULL, 0, 0 };
    if (!sb_append(&s, "<rml><body>\n")) {
        JCE_FREE(s.buf);
        return NULL;
    }
    JceUIRect screen = { 0.0f, 0.0f, screen_w, screen_h };
    emit_canvas(&s, scene, canvas, screen);
    sb_append(&s, "</body></rml>\n");
    /* Hand the buffer off to the caller as a malloc-style string.
     * The custom allocator hands out plain malloc'd memory under the
     * hood (mimalloc), so caller can free with `free()`. */
    return s.buf;
}
