/*
 * jce_ui_canvas_layout.c -- LayoutGroup arrangement.  See the header for why
 * this is not in jce_ui_canvas.c.
 */

#include "jce_ui_canvas_layout.h"
#include "jce_ui_canvas_fitter.h"   /* uc_preferred_size: the measure pass */

#include <jce/middleware/scene/jce_component_registry.h>

#include "os/core/jce_memory.h"

/* Per-child scratch inside one LayoutGroup.  A fast path, not a limit: past
 * this the arrangement grows on the heap, for the same reason the canvas
 * walk's own child arrays do. */
#define UC_LAYOUT_STACK_CHILDREN 128

/* This child's LayoutElement, or NULL.  Goes through the registry's enable
 * gate rather than the raw fetch, so unticking the component in the Inspector
 * takes its sizing back out -- the same gate every other widget applies, and
 * the one a second fetch in this file was once found not to apply. */
static const JceLayoutElementComponent *uc_layout_element(JceScene *s,
                                                          JceEntity e)
{
    const JceLayoutElementComponent *le = jce_scene_get_layout_element(s, e);
    if (!le) return NULL;
    static int s_cid = -2;
    if (s_cid == -2) s_cid = jce_component_find("LayoutElement");
    if (s_cid >= 0 && !jce_scene_comp_enabled(s, e, s_cid)) return NULL;
    return le;
}

/* Does this child take part in the arrangement at all?  An ignored child keeps
 * its own resolved rect and consumes neither a grid cell nor a share of the
 * main axis -- and, just as importantly, no spacing gap either. */
static bool uc_layout_participates(JceScene *s, JceEntity e)
{
    const JceLayoutElementComponent *le = uc_layout_element(s, e);
    return !(le && le->ignore_layout);
}

/* Apply a LayoutGroup on `parent` to its UI children, overriding the
 * children's resolved rects with packed positions. */
void uc_apply_layout_group(JceScene *s, JceUICanvas *uc,
                           const UCRect *parent_rect,
                           const JceLayoutGroupComponent *lg,
                           JceEntity *children, int child_count,
                           UCRect *child_rects, float scale)
{
    /* padding / spacing / cell_size are px metrics — scale them with the
     * CanvasScaler factor so a LayoutGroup tracks the anchored UI. */
    float pad_l = lg->padding[0] * scale, pad_r = lg->padding[1] * scale;
    float pad_t = lg->padding[2] * scale, pad_b = lg->padding[3] * scale;
    float inner_x = parent_rect->x + pad_l;
    float inner_y = parent_rect->y + pad_t;
    float inner_w = parent_rect->w - pad_l - pad_r;
    float inner_h = parent_rect->h - pad_t - pad_b;
    if (inner_w < 0) inner_w = 0;
    if (inner_h < 0) inner_h = 0;

    if (lg->layout_kind == JCE_LAYOUT_GRID) {
        float cw = (lg->cell_size[0] > 0 ? lg->cell_size[0] : 100.0f) * scale;
        float ch = (lg->cell_size[1] > 0 ? lg->cell_size[1] : 100.0f) * scale;
        float sx = lg->spacing[0] * scale, sy = lg->spacing[1] * scale;

        /* COLUMN COUNT.  Flexible -- as many as the width fits -- is what a
         * grid did before the constraint field and is what a zero-initialised
         * component still does.  It is also why a 3-column grid was not
         * authorable: you resized the parent until three happened to fit, and
         * it silently became four on a wider screen. */
        const int want = (lg->grid_constraint_count > 0)
                       ? (int)lg->grid_constraint_count : 1;
        int cols;
        if (lg->grid_constraint == (uint8_t)JCE_GRID_FIXED_COLUMNS) {
            cols = want;
        } else if (lg->grid_constraint == (uint8_t)JCE_GRID_FIXED_ROWS) {
            /* Fixed ROWS is expressed as the columns needed to hold the
             * children in that many rows: the row count is the promise, the
             * column count is what keeps it. */
            cols = (child_count + want - 1) / want;
        } else {
            cols = (int)((inner_w + sx) / (cw + sx));
        }
        if (cols < 1) cols = 1;
        /* child_alignment applies to the GRID too.  It was read only by the
         * linear path below, which this branch returns before ever reaching,
         * so the Inspector's 0..8 control did nothing whatsoever in grid mode
         * -- the same slack math, just never run here. */
        {
            int rows = (child_count + cols - 1) / cols;
            if (rows < 1) rows = 1;
            float block_w = (float)cols * cw + (float)(cols - 1) * sx;
            float block_h = (float)rows * ch + (float)(rows - 1) * sy;
            float slack_x = inner_w - block_w;
            float slack_y = inner_h - block_h;
            if (slack_x < 0) slack_x = 0;
            if (slack_y < 0) slack_y = 0;
            int along_x = lg->child_alignment % 3;   /* L / C / R */
            int along_y = lg->child_alignment / 3;   /* U / M / L */
            if (along_x == 1) inner_x += slack_x * 0.5f;
            else if (along_x == 2) inner_x += slack_x;
            if (along_y == 1) inner_y += slack_y * 0.5f;
            else if (along_y == 2) inner_y += slack_y;
        }
        /* Cells are counted over the PARTICIPATING children, so an ignored
         * child does not leave a hole in the grid. */
        int cells = 0;
        for (int c = 0; c < child_count; c++)
            if (uc_layout_participates(s, children[c])) cells++;
        int rows_span = (cells + cols - 1) / cols;
        if (rows_span < 1) rows_span = 1;

        int cell = 0;
        for (int c = 0; c < child_count; c++) {
            int idx = lg->reverse_arrangement ? (child_count - 1 - c) : c;
            if (!uc_layout_participates(s, children[idx])) continue;
            const int i = cell++;

            /* START AXIS: horizontal fills a row and then moves down (reading
             * order, and what a grid did before this); vertical fills a column
             * and then moves across.  A column-major grid is not the same
             * picture with the cells renamed -- it is where item 2 sits. */
            int row, col;
            if (lg->grid_start_axis == (uint8_t)JCE_GRID_AXIS_VERTICAL) {
                row = i % rows_span;
                col = i / rows_span;
                if (col >= cols) col = cols - 1;   /* clamp a ragged tail */
            } else {
                row = i / cols;
                col = i % cols;
            }

            /* START CORNER mirrors the INDICES, not the geometry, so the cells
             * stay on the same lattice and only their order changes.  Mirroring
             * the positions instead would shift the whole block by the unused
             * slack of the last row. */
            if (lg->grid_start_corner == (uint8_t)JCE_GRID_CORNER_UPPER_RIGHT ||
                lg->grid_start_corner == (uint8_t)JCE_GRID_CORNER_LOWER_RIGHT)
                col = cols - 1 - col;
            if (lg->grid_start_corner == (uint8_t)JCE_GRID_CORNER_LOWER_LEFT ||
                lg->grid_start_corner == (uint8_t)JCE_GRID_CORNER_LOWER_RIGHT)
                row = rows_span - 1 - row;

            child_rects[idx].x = inner_x + (cw + sx) * (float)col;
            child_rects[idx].y = inner_y + (ch + sy) * (float)row;
            child_rects[idx].w = cw;
            child_rects[idx].h = ch;
        }
        return;
    }

    bool horizontal = (lg->layout_kind == JCE_LAYOUT_HORIZONTAL);
    float spacing = (horizontal ? lg->spacing[0] : lg->spacing[1]) * scale;
    bool ctrl_main_axis = horizontal ? lg->control_child_size_w
                                     : lg->control_child_size_h;

    /* MAIN-AXIS SIZE, PER CHILD.
     *
     * This used to be ONE number for everyone: control_child_size gave
     * strictly equal shares, so "this child takes twice the space" could not
     * be said, and with it off a child's own rect was its size, so "at least
     * this big" could not be said either.  A panel is a header that stays put
     * and a body that takes the rest, which is why Unity, Godot and Slate all
     * ship a per-child weight.
     *
     * The pass is: preferred size, then hand the LEFTOVER out by flexible
     * weight, then raise anything under its minimum.  A child with no
     * LayoutElement gets exactly what it got before -- which is what makes
     * this safe under every scene already authored. */
    float  size_local[UC_LAYOUT_STACK_CHILDREN];
    float *main_size = size_local;
    float *size_heap = NULL;
    if (child_count > UC_LAYOUT_STACK_CHILDREN) {
        size_heap = (float *)JCE_MALLOC((size_t)child_count * sizeof(float));
        if (!size_heap) return;   /* leave every rect exactly as resolved */
        main_size = size_heap;
    }

    const float box = horizontal ? inner_w : inner_h;
    /* Everything below counts PARTICIPANTS, not children: an ignored child
     * takes no share and leaves no gap, so a run of three with one ignored
     * packs exactly like a run of two. */
    int parts = 0;
    for (int i = 0; i < child_count; i++)
        if (uc_layout_participates(s, children[i])) parts++;
    const float gaps = spacing * (float)(parts > 1 ? parts - 1 : 0);
    const float equal_share = (parts > 0) ? (box - gaps) / (float)parts : 0.0f;
    float total = 0.0f;
    {
        float sum_pref = 0.0f, sum_flex = 0.0f;
        for (int i = 0; i < child_count; i++) {
            const JceLayoutElementComponent *le =
                uc_layout_element(s, children[i]);
            if (le && le->ignore_layout) {
                /* NAN marks "not in the run".  A sentinel of -1 would be
                 * erased by the clamp in the minimums pass below, which is
                 * exactly the kind of quiet collision that makes a skipped
                 * child reappear at width zero. */
                main_size[i] = -1.0f;
                continue;
            }
            float pref;
            if (le && (horizontal ? le->preferred_width
                                  : le->preferred_height) >= 0.0f) {
                pref = (horizontal ? le->preferred_width
                                   : le->preferred_height) * scale;
            } else if (ctrl_main_axis) {
                pref = equal_share;      /* the behaviour before this */
            } else {
                /* THE BOTTOM-UP HALF.  Ask the child what it NEEDS before
                 * falling back to the rect it was authored with: a label
                 * answers with its shaped text, a nested group with its own
                 * packing.  Without this a column of labels is sized by their
                 * rects and the text clips -- and the rect is what an author
                 * has least reason to have set correctly, because the whole
                 * point of putting a label in a group is not having to. */
                pref = horizontal ? child_rects[i].w : child_rects[i].h;
                if (uc) {
                    float mw = 0.0f, mh = 0.0f;
                    if (uc_preferred_size(s, uc, scale, children[i], &mw, &mh)) {
                        const float m = horizontal ? mw : mh;
                        if (m > 0.0f) pref = m;
                    }
                }
            }
            main_size[i] = pref;
            sum_pref += pref;
            if (le) sum_flex += horizontal ? le->flexible_width
                                           : le->flexible_height;
        }

        /* LEFTOVER, by weight.  Only distributed when someone asked for it, so
         * a group with no flexible children keeps its packed run and the slack
         * that child_alignment exists to place. */
        if (sum_flex > 0.0f) {
            const float left = box - gaps - sum_pref;
            if (left > 0.0f) {
                for (int i = 0; i < child_count; i++) {
                    const JceLayoutElementComponent *le =
                        uc_layout_element(s, children[i]);
                    if (!le || le->ignore_layout) continue;
                    const float w = horizontal ? le->flexible_width
                                               : le->flexible_height;
                    if (w > 0.0f) main_size[i] += left * (w / sum_flex);
                }
            }
        }

        /* MINIMUMS LAST.  A group too small for its children shrinks them and
         * this is how a child says how far.  After the share, so a minimum can
         * only raise a size and never take another child's. */
        for (int i = 0; i < child_count; i++) {
            const JceLayoutElementComponent *le =
                uc_layout_element(s, children[i]);
            if (le && le->ignore_layout) continue;   /* keeps its -1 marker */
            if (le) {
                const float mn =
                    (horizontal ? le->min_width : le->min_height) * scale;
                if (mn > 0.0f && main_size[i] < mn) main_size[i] = mn;
            }
            if (main_size[i] < 0.0f) main_size[i] = 0.0f;
            total += main_size[i];
        }
        total += gaps;
    }

    /* child_alignment: Unity TextAnchor 0..8 (rows: upper/middle/lower,
     * cols: left/center/right).  Use it to offset the packed run within the
     * inner box on the main axis. */
    float start = horizontal ? inner_x : inner_y;
    {
        float slack = box - total;   /* box is the run's axis, computed above */
        if (slack < 0) slack = 0;
        int along = horizontal ? (lg->child_alignment % 3)        /* L/C/R */
                               : (lg->child_alignment / 3);        /* U/M/L */
        if (along == 1) start += slack * 0.5f;
        else if (along == 2) start += slack;
    }

    float cursor = start;
    for (int i = 0; i < child_count; i++) {
        int idx = lg->reverse_arrangement ? (child_count - 1 - i) : i;
        /* An ignored child is LEFT EXACTLY AS RESOLVED -- not moved, not
         * resized, and the cursor does not advance past it. */
        if (main_size[idx] < 0.0f) continue;
        UCRect *cr = &child_rects[idx];
        if (horizontal) {
            float w = main_size[idx];
            cr->x = cursor;
            cr->w = w;
            if (lg->control_child_size_h) { cr->y = inner_y; cr->h = inner_h; }
            else {
                /* cross-axis align (U/M/L) */
                int cross = lg->child_alignment / 3;
                float slack = inner_h - cr->h;
                cr->y = inner_y + (cross == 1 ? slack * 0.5f : cross == 2 ? slack : 0.0f);
            }
            cursor += w + spacing;
        } else {
            float h = main_size[idx];
            cr->y = cursor;
            cr->h = h;
            if (lg->control_child_size_w) { cr->x = inner_x; cr->w = inner_w; }
            else {
                int cross = lg->child_alignment % 3;
                float slack = inner_w - cr->w;
                cr->x = inner_x + (cross == 1 ? slack * 0.5f : cross == 2 ? slack : 0.0f);
            }
            cursor += h + spacing;
        }
    }

    if (size_heap) JCE_FREE(size_heap);
}
