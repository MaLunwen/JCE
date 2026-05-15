/*
 * jce_ui_layout.c  Auto-layout computation.
 *
 * Horizontal / vertical pack children along the main axis with
 * spacing; cross-axis position determined by alignment.  Grid wraps
 * children into rows using a fixed cell size.
 */

#include <jce/middleware/ui/jce_ui_layout.h>

#include <string.h>

static float total_flex(const JceUiChildSize *c, uint32_t count)
{
    float t = 0;
    for (uint32_t i = 0; i < count; ++i) t += c[i].flex;
    return t;
}

static void compute_axis(float content, float padding_start, float padding_end,
                          float spacing, JceUiAlign align,
                          const float *prefs, const float *flexes,
                          uint32_t count, float *out_pos, float *out_size)
{
    float used = padding_start + padding_end;
    for (uint32_t i = 0; i < count; ++i) used += prefs[i];
    if (count > 1) used += spacing * (count - 1);
    float leftover = content - used;
    float t_flex = 0;
    for (uint32_t i = 0; i < count; ++i) t_flex += flexes[i];

    float cursor = padding_start;
    if (t_flex <= 0.0f) {
        if (align == JCE_UI_ALIGN_CENTER) cursor += leftover * 0.5f;
        else if (align == JCE_UI_ALIGN_END) cursor += leftover;
    }
    for (uint32_t i = 0; i < count; ++i) {
        float s = prefs[i];
        if (t_flex > 0.0f && leftover > 0.0f)
            s += leftover * (flexes[i] / t_flex);
        out_pos [i] = cursor;
        out_size[i] = s;
        cursor += s + spacing;
    }
}

bool jce_ui_layout_compute(const JceUiRect *parent,
                            const JceUiLayoutGroup *g,
                            const JceUiChildSize *kids,
                            JceUiRect *out, uint32_t count)
{
    if (!parent || !g || !kids || !out || count == 0) return false;

    /* Per-axis temp buffers up to 64 children. */
    enum { TMP = 64 };
    if (count > TMP) count = TMP;
    float prefs[TMP], flexes[TMP];
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = g->reverse ? (count - 1 - i) : i;
        prefs[i]  = (g->kind == JCE_UI_LAYOUT_VERTICAL)   ? kids[idx].pref_h
                  : (g->kind == JCE_UI_LAYOUT_HORIZONTAL) ? kids[idx].pref_w
                  :                                          g->cell_w;
        flexes[i] = kids[idx].flex;
    }

    if (g->kind == JCE_UI_LAYOUT_HORIZONTAL ||
        g->kind == JCE_UI_LAYOUT_VERTICAL) {
        float pos[TMP], sz[TMP];
        bool h = g->kind == JCE_UI_LAYOUT_HORIZONTAL;
        compute_axis(h ? parent->w : parent->h,
                     h ? g->padding[0] : g->padding[1],
                     h ? g->padding[2] : g->padding[3],
                     g->spacing, JCE_UI_ALIGN_START,
                     prefs, flexes, count, pos, sz);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t idx = g->reverse ? (count - 1 - i) : i;
            JceUiRect *r = &out[idx];
            if (h) {
                r->x = parent->x + pos[i];
                r->w = sz[i];
                /* Cross-axis = vertical. */
                float ch = kids[idx].pref_h;
                float free_h = parent->h - g->padding[1] - g->padding[3];
                if (g->child_align == JCE_UI_ALIGN_STRETCH) {
                    r->y = parent->y + g->padding[1];
                    r->h = free_h;
                } else {
                    r->h = ch;
                    if (g->child_align == JCE_UI_ALIGN_CENTER)
                        r->y = parent->y + g->padding[1] + (free_h - ch) * 0.5f;
                    else if (g->child_align == JCE_UI_ALIGN_END)
                        r->y = parent->y + parent->h - g->padding[3] - ch;
                    else
                        r->y = parent->y + g->padding[1];
                }
            } else {
                r->y = parent->y + pos[i];
                r->h = sz[i];
                float cw = kids[idx].pref_w;
                float free_w = parent->w - g->padding[0] - g->padding[2];
                if (g->child_align == JCE_UI_ALIGN_STRETCH) {
                    r->x = parent->x + g->padding[0];
                    r->w = free_w;
                } else {
                    r->w = cw;
                    if (g->child_align == JCE_UI_ALIGN_CENTER)
                        r->x = parent->x + g->padding[0] + (free_w - cw) * 0.5f;
                    else if (g->child_align == JCE_UI_ALIGN_END)
                        r->x = parent->x + parent->w - g->padding[2] - cw;
                    else
                        r->x = parent->x + g->padding[0];
                }
            }
        }
        return true;
    }

    /* Grid layout. */
    float content_w = parent->w - g->padding[0] - g->padding[2];
    float cell_w = g->cell_w > 0 ? g->cell_w : 64.0f;
    float cell_h = g->cell_h > 0 ? g->cell_h : 64.0f;
    int cols = (int)((content_w + g->spacing) / (cell_w + g->spacing));
    if (cols < 1) cols = 1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = g->reverse ? (count - 1 - i) : i;
        int row = (int)i / cols;
        int col = (int)i % cols;
        JceUiRect *r = &out[idx];
        r->x = parent->x + g->padding[0] + col * (cell_w + g->spacing);
        r->y = parent->y + g->padding[1] + row * (cell_h + g->spacing);
        r->w = cell_w;
        r->h = cell_h;
        (void)0; (void)prefs; (void)flexes;
    }
    return true;
}
