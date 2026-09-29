/*
 * jce_ui_canvas_fitter.c -- see jce_ui_canvas_fitter.h.
 */
#include "jce_ui_canvas_fitter.h"

#include <jce/middleware/scene/jce_component_registry.h>  /* jce_component_find */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/ui/jce_localization.h>            /* jce_loc_t */
#include <jce/renderer/jce_text.h>

#include "os/core/jce_memory.h"   /* the child list grows past the stack fast path */

#include <stdio.h>
#include <string.h>

/* ── ContentSizeFitter ─────────────────────────────────────────────
 *
 * The size an element WANTS, in device px, or false when nothing about the
 * element can answer.  "Content" is decided by what the element IS: a label
 * measures its own shaped text through the FONT AND LINE SPACING IT DRAWS
 * WITH -- not an approximation of them, because a fitter that sizes a panel
 * to a different measurement than the draw uses puts the overflow back one
 * pixel at a time.  A node with a LayoutGroup measures the packed extent of
 * its children instead.  Anything else keeps its authored size: there is
 * nothing else to ask it. */
/* Depth guard.  The walk that CALLS this already has one (UC_MAX_DEPTH), but
 * this recursion is its own: a group measuring a group measuring a group is a
 * different stack, and a cycle in the hierarchy would ride it down. */
#define UC_MEASURE_MAX_DEPTH 16

static bool uc_preferred_size_depth(JceScene *s, JceUICanvas *uc,
                                    float ui_scale, JceEntity node,
                                    float *out_w, float *out_h, int depth);

/* What one CHILD contributes to its parent's measurement, in priority order:
 *
 *   1. its LayoutElement's preferred size, if it states one.  That is the
 *      author speaking directly and nothing should override it.
 *   2. its own CONTENT -- its text, or its packing if it is a group.  This is
 *      the half that was missing: a child was measured by its authored rect
 *      even when it could say what it actually needed, so a column of labels
 *      sized itself to their rects rather than to their text.
 *   3. its authored rect.  Nothing else to ask.
 *
 * A child that ignores the layout contributes NOTHING, for the same reason it
 * takes no cell and no share: it is not in the arrangement. */
static void uc_child_measure(JceScene *s, JceUICanvas *uc, float ui_scale,
                             JceEntity kid, int depth,
                             float *out_w, float *out_h)
{
    *out_w = 0.0f; *out_h = 0.0f;

    const JceLayoutElementComponent *le = jce_scene_get_layout_element(s, kid);
    if (le) {
        static int cid = -2;
        if (cid == -2) cid = jce_component_find("LayoutElement");
        if (cid >= 0 && !jce_scene_comp_enabled(s, kid, cid)) le = NULL;
    }
    if (le && le->ignore_layout) return;

    const JceRectTransform *krt = uc_entity_rect(s, kid);
    float w = krt ? krt->size_delta[0] * ui_scale : 0.0f;
    float h = krt ? krt->size_delta[1] * ui_scale : 0.0f;

    float cw = 0.0f, ch = 0.0f;
    if (depth < UC_MEASURE_MAX_DEPTH &&
        uc_preferred_size_depth(s, uc, ui_scale, kid, &cw, &ch, depth + 1)) {
        if (cw > 0.0f) w = cw;
        if (ch > 0.0f) h = ch;
    }

    /* The author's explicit number wins over both. */
    if (le) {
        if (le->preferred_width  >= 0.0f) w = le->preferred_width  * ui_scale;
        if (le->preferred_height >= 0.0f) h = le->preferred_height * ui_scale;
        const float mnw = le->min_width  * ui_scale;
        const float mnh = le->min_height * ui_scale;
        if (mnw > 0.0f && w < mnw) w = mnw;
        if (mnh > 0.0f && h < mnh) h = mnh;
    }
    *out_w = w; *out_h = h;
}

bool uc_preferred_size(JceScene *s, JceUICanvas *uc, float ui_scale,
                       JceEntity node, float *out_w, float *out_h)
{
    return uc_preferred_size_depth(s, uc, ui_scale, node, out_w, out_h, 0);
}

static bool uc_preferred_size_depth(JceScene *s, JceUICanvas *uc,
                                    float ui_scale, JceEntity node,
                                    float *out_w, float *out_h, int depth)
{
    JceUITextComponent *tx = jce_scene_get_ui_text(s, node);
    if (tx && !jce_scene_component_enabled(s, node, JCE_COMP_FLAG_UI_TEXT))
        tx = NULL;
    if (tx && uc) {
        const char *str = tx->text;
        if (tx->locale_key[0]) {
            const char *loc = jce_loc_t(tx->locale_key);
            str = (loc == tx->locale_key && tx->text[0]) ? tx->text : loc;
        }
        if (!str || !str[0]) { *out_w = 0.0f; *out_h = 0.0f; return true; }

        int px = (int)(tx->font_size * ui_scale + 0.5f);
        JceFont *f = uc_get_font(uc, tx->font_path, px, tx->sdf);
        if (!f) return false;

        /* Same split the draw does: newlines only, up to 64 lines. */
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", str);
        const char *lines[64];
        int nlines = 0;
        lines[nlines++] = buf;
        for (char *p = buf; *p && nlines < 64; ++p)
            if (*p == '\n') { *p = '\0'; lines[nlines++] = p + 1; }

        float lsp = (tx->line_spacing > 0.0f) ? tx->line_spacing : 1.0f;
        uc_text_block_extent(f, lines, nlines, lsp, out_w, out_h);
        return true;
    }

    /* A LayoutGroup node wants exactly the box its packing needs.  Children
     * are measured by their AUTHORED size here: without a per-child
     * LayoutElement there is nothing else to consult, and Unity behaves the
     * same way when a child declares no preferred size. */
    JceLayoutGroupComponent *lg = jce_scene_get_layout_group(s, node);
    if (lg && !jce_scene_component_enabled(s, node, JCE_COMP_FLAG_LAYOUT_GROUP))
        lg = NULL;
    if (lg) {
        /* Stack array as a fast path, not a limit -- the same shape and the
         * same reason as the canvas walk's: jce_scene_get_children truncates
         * SILENTLY, so a 200-row list measured to the size of its first 128
         * rows and the container came out short with nothing said. */
        enum { KIDS_LOCAL = 128 };
        JceEntity kids_local[KIDS_LOCAL];
        JceEntity *kids = kids_local;
        JceEntity *kids_heap = NULL;
        int cap = KIDS_LOCAL;
        const int child_count = jce_scene_get_child_count(s, node);
        if (child_count > KIDS_LOCAL) {
            kids_heap = (JceEntity *)JCE_MALLOC((size_t)child_count *
                                                sizeof(JceEntity));
            if (kids_heap) { kids = kids_heap; cap = child_count; }
        }
        int n = jce_scene_get_children(s, node, kids, cap);
        float main_sum = 0.0f, cross_max = 0.0f;
        int counted = 0;
        for (int i = 0; i < n; i++) {
            if (!uc_is_ui_element(s, kids[i])) continue;
            /* THE CHANGE: ask the child what it needs instead of reading its
             * authored rect.  A label reports its shaped text, a nested group
             * reports its own packing, and either can be overridden by a
             * LayoutElement -- which is what lets a column size its rows to
             * their content. */
            float kw = 0.0f, kh = 0.0f;
            uc_child_measure(s, uc, ui_scale, kids[i], depth, &kw, &kh);
            if (kw <= 0.0f && kh <= 0.0f) continue;   /* ignore_layout */
            const bool horiz = (lg->layout_kind == 0);
            main_sum  += horiz ? kw : kh;
            const float cross = horiz ? kh : kw;
            if (cross > cross_max) cross_max = cross;
            counted++;
        }
        if (kids_heap) JCE_FREE(kids_heap);
        if (counted == 0) return false;
        const bool horiz = (lg->layout_kind == 0);
        const float gap = (horiz ? lg->spacing[0] : lg->spacing[1])
                        * ui_scale * (float)(counted - 1);
        const float pad_w = (lg->padding[0] + lg->padding[1]) * ui_scale;
        const float pad_h = (lg->padding[2] + lg->padding[3]) * ui_scale;
        *out_w = (horiz ? main_sum + gap : cross_max) + pad_w;
        *out_h = (horiz ? cross_max : main_sum + gap) + pad_h;
        return true;
    }

    return false;
}

/* Resize `r` in place to the element's content, per axis.  Returns true when
 * anything changed.
 *
 * The rect GROWS ABOUT ITS PIVOT, so a centred label grows both ways and a
 * left-anchored one grows to the right -- which is what an author who set
 * that pivot already asked for, and the only choice that leaves the element
 * where they put it. */
bool uc_fit_rect(JceScene *s, JceUICanvas *uc, float ui_scale,
                 JceEntity node, UCRect *r)
{
    JceContentSizeFitterComponent *ft = jce_scene_get_content_size_fitter(s, node);
    if (!ft) return false;
    { static int cid = -2; if (cid == -2) cid = jce_component_find("ContentSizeFitter");
      if (cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) return false; }
    if (ft->horizontal_fit == JCE_UI_FIT_UNCONSTRAINED &&
        ft->vertical_fit   == JCE_UI_FIT_UNCONSTRAINED) return false;

    float pw = 0.0f, ph = 0.0f;
    if (!uc_preferred_size(s, uc, ui_scale, node, &pw, &ph)) return false;

    const JceRectTransform *rt = uc_entity_rect(s, node);
    float pvx = rt ? rt->pivot[0] : 0.5f;
    float pvy = rt ? rt->pivot[1] : 0.5f;
    if (pvx == 0.0f && pvy == 0.0f) { pvx = 0.5f; pvy = 0.5f; }

    bool changed = false;
    if (ft->horizontal_fit == JCE_UI_FIT_PREFERRED && pw > 0.0f) {
        r->x += (r->w - pw) * pvx;
        r->w  = pw;
        changed = true;
    }
    if (ft->vertical_fit == JCE_UI_FIT_PREFERRED && ph > 0.0f) {
        /* Draw space is y-DOWN while the pivot is authored y-UP -- the same
         * flip uc_resolve_rect makes for the rect itself. */
        r->y += (r->h - ph) * (1.0f - pvy);
        r->h  = ph;
        changed = true;
    }
    return changed;
}
