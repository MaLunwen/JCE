/*
 * jce_ui_canvas_widgets.c — the dropdown and the input field.
 *
 * Split out of jce_ui_canvas.c, which was 3,102 lines: past the 3,000-line
 * cap and frozen at the size gate's baseline, so every addition had to be
 * paid for by a removal.  These two widgets were 242 CONTIGUOUS lines with
 * the smallest interface in the file -- three helpers in, seven functions
 * out, one struct -- which is why they were chosen over the 555-line
 * interaction state machines, whose boundary would have had to cut through
 * UCFrame, the canvas's whole per-frame state.
 *
 * The shared boundary is jce_ui_canvas_widgets.h and nothing else in either
 * file became visible.
 */

#include "jce_ui_canvas_widgets.h"

#include <jce/os/core/jce_str.h>   /* jce_strlcpy: was an implicit declaration */
#include <jce/renderer/jce_primitives.h>

#include <stdio.h>
#include <string.h>

/* ── Dropdown geometry + draw (PASS 3) ──────────────────────────────── */

/* Number of valid options, clamped to the fixed POD capacity. */
int uc_dd_option_count(const JceUIDropdownComponent *d)
{
    int oc = d->option_count;
    if (oc < 0) oc = 0;
    if (oc > JCE_UI_DROPDOWN_MAX_OPTIONS) oc = JCE_UI_DROPDOWN_MAX_OPTIONS;
    return oc;
}

/* Rect of expanded option row `i` (0-based): one main-rect height tall.
 *
 * The list prefers to hang BELOW the control, but a dropdown near the bottom of
 * the screen would put its options past the bottom edge, where they draw
 * off-screen and are unclickable -- while the popup still claims them as its
 * modal rect, so the rows nobody can see still block whatever is underneath.
 * Unity's Dropdown constrains the list to the canvas for exactly this reason.
 * So: below if it fits, above if that fits, otherwise clamped to the screen.
 *
 * Shared by the draw, the headless raycast and the modal-rect publication, so
 * the three cannot disagree about where a row is.  `screen_h <= 0` (an
 * unmeasured frame) keeps the old unconditional below-placement. */
UCRect uc_dd_row_rect(const UCRect *main, int i, int count, float screen_h)
{
    UCRect r;
    r.x = main->x;
    r.w = main->w;
    r.h = main->h;

    const float below = main->y + main->h;
    if (count <= 0 || screen_h <= 0.0f) {
        r.y = below + main->h * (float)i;
        return r;
    }
    const float block = main->h * (float)count;
    float top;
    if (below + block <= screen_h)        top = below;              /* fits below */
    else if (main->y - block >= 0.0f)     top = main->y - block;    /* flip above */
    else if (block <= screen_h)           top = screen_h - block;   /* clamp up   */
    else                                  top = 0.0f;               /* taller than the screen */
    r.y = top + main->h * (float)i;
    return r;
}

/* Draw a single centred-left, vertically-centred text line inside `r`. */
void uc_dd_draw_label(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                             JceFont *font, const char *str, const float col[4],
                             float alpha_mul, float ui_scale)
{
    if (!font || !str || !str[0]) return;
    const float pad = 4.0f * ui_scale;
    float lh = (float)jce_font_line_height(font);
    float ty = r->y + (r->h - lh) * 0.5f;
    if (ty < r->y) ty = r->y;
    jce_text_draw_scaled_view(uc_renderer(uc), font, view_id, r->x + pad, ty, 1.0f,
                              str, uc_color(col, alpha_mul));
}

/* Draw the collapsed dropdown (bg + selected label + arrow).  The expanded
 * popup is drawn separately (uc_draw_dropdown_popup) AFTER the dropdown's own
 * row so it sits on top in the "drawn last" z-order.  Renderer-gated. */
void uc_draw_dropdown(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                             const JceUIDropdownComponent *d, float alpha_mul,
                             float ui_scale)
{
    /* Collapsed background. */
    uc_draw_quad(uc, view_id, r->x, r->y, r->w, r->h, NULL, d->bg_color, alpha_mul);

    int px = (int)((d->font_size > 0.0f ? d->font_size : 16.0f) * ui_scale);
    if (px < 1) px = 1;
    JceFont *font = uc_get_font(uc, d->font_path, px, false);

    /* Selected label. */
    int oc = uc_dd_option_count(d);
    int sel = d->selected_index;
    if (sel < 0) sel = 0;
    if (oc > 0 && sel < oc)
        uc_dd_draw_label(uc, view_id, r, font, d->options[sel], d->text_color,
                         alpha_mul, ui_scale);

    /* Arrow glyph: a small square at the right edge (a real triangle glyph is a
     * followup; the quad reads as the expand affordance for v1). */
    float a = r->h * 0.3f;
    if (a > 0.0f) {
        float ax = r->x + r->w - a - 4.0f * ui_scale;
        float ay = r->y + (r->h - a) * 0.5f;
        uc_draw_quad(uc, view_id, ax, ay, a, a, NULL, d->text_color, alpha_mul);
    }
}

/* Draw the expanded option-list popup below the main rect (popup bg per row +
 * each option's label, highlighting the hovered/selected row).  Drawn AFTER
 * the dropdown's own row so it overlays sibling UI in the same canvas (a true
 * global overlay above ALL UI is a followup — inline-last for v1).  Renderer-
 * gated by the caller. */
void uc_draw_dropdown_popup(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                                   const JceUIDropdownComponent *d, float alpha_mul,
                                   float ui_scale, float ptr_x, float ptr_y,
                                   bool ptr_valid, float screen_h)
{
    int oc = uc_dd_option_count(d);
    if (oc <= 0) return;

    int px = (int)((d->font_size > 0.0f ? d->font_size : 16.0f) * ui_scale);
    if (px < 1) px = 1;
    JceFont *font = uc_get_font(uc, d->font_path, px, false);

    for (int i = 0; i < oc; i++) {
        UCRect row = uc_dd_row_rect(r, i, oc, screen_h);
        /* Row background: highlight the selected row or the row the pointer is
         * over; otherwise the popup background colour. */
        bool over = ptr_valid &&
                    ptr_x >= row.x && ptr_x < row.x + row.w &&
                    ptr_y >= row.y && ptr_y < row.y + row.h;
        /* Hovered and selected used the SAME highlight colour, so on an open
         * list the user could not tell which row was the current value and
         * which was merely under the cursor -- and on the frame the cursor sat
         * over the selected row, the two were indistinguishable from a single
         * highlighted row.  Unity separates them: the highlight is the hover
         * state, and the current value carries a checkmark.  Same split here,
         * with a marker bar instead of a glyph so it needs no font coverage. */
        const bool sel = (i == d->selected_index);
        const float *bg = over ? d->highlight_color : d->popup_color;
        uc_draw_quad(uc, view_id, row.x, row.y, row.w, row.h, NULL, bg, alpha_mul);
        if (sel) {
            const float mw = 3.0f * ui_scale;
            uc_draw_quad(uc, view_id, row.x, row.y, mw, row.h, NULL,
                         d->highlight_color, alpha_mul);
        }
        uc_dd_draw_label(uc, view_id, &row, font, d->options[i], d->text_color,
                         alpha_mul, ui_scale);
    }
}

/* ── InputField content-type filter + caret edit helpers ────────────── */

/* Does codepoint-ish byte `ch` pass the InputField's content_type filter?
 * Operates byte-wise; multibyte UTF-8 lead/continuation bytes (>= 0x80) are
 * accepted only by content_type 0 (any) so filtered types stay ASCII-clean. */
bool uc_if_accepts(int content_type, char ch, const char *text, int caret)
{
    unsigned char u = (unsigned char)ch;
    switch (content_type) {
        case 1: /* integer: digits + a single leading '-' */
            if (ch >= '0' && ch <= '9') return true;
            if (ch == '-') return caret == 0 && text[0] != '-';
            return false;
        case 2: /* decimal: digits + a single leading '-' + a single '.' */
            if (ch >= '0' && ch <= '9') return true;
            if (ch == '-') return caret == 0 && text[0] != '-';
            if (ch == '.') return strchr(text, '.') == NULL;
            return false;
        case 3: /* alphanumeric: [A-Za-z0-9] */
            return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                   (ch >= '0' && ch <= '9');
        case 0: /* any printable (and any UTF-8 byte) */
        default:
            return u >= 0x20 || u >= 0x80;   /* reject ASCII control bytes */
    }
}

/* Effective char (byte) cap: char_limit when >0, else the buffer cap. */
int uc_if_cap(const JceUIInputFieldComponent *f)
{
    int buf_cap = (int)sizeof(f->text) - 1;   /* 255 */
    if (f->char_limit > 0 && f->char_limit < buf_cap) return f->char_limit;
    return buf_cap;
}

/* Draw the InputField: bg quad + (text | placeholder) + caret when focused. */
void uc_draw_input_field(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                                const JceUIInputFieldComponent *f, float alpha_mul,
                                float ui_scale, bool focused, float blink)
{
    /* Background box. */
    uc_draw_quad(uc, view_id, r->x, r->y, r->w, r->h, NULL, f->bg_color, alpha_mul);

    int px = (int)((f->font_size > 0.0f ? f->font_size : 16.0f) * ui_scale);
    if (px < 1) px = 1;
    JceFont *font = uc_get_font(uc, f->font_path, px, false);

    bool empty = (f->text[0] == '\0');
    /* Build the display string: password → '*' run; else the value (or the
     * placeholder when empty & not focused). */
    char disp[256];
    const float *col;
    if (empty && !focused) {
        jce_strlcpy(disp, f->placeholder, sizeof disp);
        col = f->placeholder_color;
    } else if (f->is_password) {
        int n = 0;
        for (const char *p = f->text; *p && n < (int)sizeof(disp) - 1; ++p) disp[n++] = '*';
        disp[n] = '\0';
        col = f->text_color;
    } else {
        jce_strlcpy(disp, f->text, sizeof disp);
        col = f->text_color;
    }

    const float pad = 4.0f * ui_scale;     /* left text inset */
    float ty = r->y;
    if (font) {
        float lh = (float)jce_font_line_height(font);
        ty = r->y + (r->h - lh) * 0.5f;    /* vertically centre the single line */
        if (ty < r->y) ty = r->y;
    }
    if (font && disp[0])
        jce_text_draw_scaled_view(uc_renderer(uc), font, view_id, r->x + pad, ty, 1.0f,
                                  disp, uc_color(col, alpha_mul));

    /* Caret: measure the text up to the byte caret to find its x, blink on/off. */
    if (focused && blink >= 0.5f) {
        float cx = r->x + pad;
        if (font) {
            char pre[256];
            int caret = uc_caret(uc);
            int textlen = (int)strlen(f->text);
            if (caret > textlen) caret = textlen;
            if (caret < 0) caret = 0;
            if (f->is_password) {
                /* caret advances over '*' glyphs, one per stored byte. */
                int n = caret < (int)sizeof(pre) - 1 ? caret : (int)sizeof(pre) - 1;
                for (int i = 0; i < n; ++i) pre[i] = '*';
                pre[n] = '\0';
            } else {
                int n = caret < (int)sizeof(pre) - 1 ? caret : (int)sizeof(pre) - 1;
                memcpy(pre, f->text, (size_t)n);
                pre[n] = '\0';
            }
            float w = 0.0f, h = 0.0f;
            jce_text_measure(font, pre, &w, &h);
            cx += w;
        }
        float cw = 1.0f * ui_scale; if (cw < 1.0f) cw = 1.0f;
        float ch = (font ? (float)jce_font_line_height(font) : r->h * 0.7f);
        float cy = r->y + (r->h - ch) * 0.5f;
        if (cy < r->y) cy = r->y;
        uc_draw_quad(uc, view_id, cx, cy, cw, ch, NULL, f->caret_color, alpha_mul);
    }
}
