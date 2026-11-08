#include "jce_ui_canvas_text.h"
#include "jce_ui_text_fit.h"
#include <jce/middleware/ui/jce_localization.h>
#include <stdio.h>
#include <string.h>

/* ── Text draw ─────────────────────────────────────────────────────── */

/* Measure the widest line and total block height of a (possibly multi-line)
 * line list at `font`, with `lsp` line-spacing multiplier. */
void uc_text_block_extent(JceFont *font, const char *const *lines, int nlines,
                                 float lsp, float *out_w, float *out_h)
{
    float maxw = 0.0f;
    for (int i = 0; i < nlines; ++i) {
        float w = 0.0f, h = 0.0f;
        jce_text_measure(font, lines[i], &w, &h);
        if (w > maxw) maxw = w;
    }
    *out_w = maxw;
    *out_h = (float)jce_font_line_height(font) * lsp * (float)(nlines > 0 ? nlines : 1);
}

/* ── Rich text (L7): <color=...> spans; other tags recognised + stripped ── */

static int uc_hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a color spec (the text after "color=", `len` chars): #RRGGBB[AA] or a
 * small set of names.  Writes rgba and returns true on success. */
static bool uc_parse_color(const char *spec, int len, float out[4])
{
    out[3] = 1.0f;
    if (spec[0] == '#') {
        int comps;
        if      (len == 7) comps = 3;       /* #RRGGBB   */
        else if (len == 9) comps = 4;       /* #RRGGBBAA */
        else return false;                  /* reject odd/short hex specs */
        unsigned v[4] = { 0, 0, 0, 255 };
        for (int c = 0; c < comps; ++c) {
            int hi = uc_hexv(spec[1 + c * 2]), lo = uc_hexv(spec[2 + c * 2]);
            if (hi < 0 || lo < 0) return false;
            v[c] = (unsigned)(hi * 16 + lo);
        }
        out[0] = v[0] / 255.0f; out[1] = v[1] / 255.0f;
        out[2] = v[2] / 255.0f; out[3] = v[3] / 255.0f;
        return true;
    }
    static const struct { const char *n; float r, g, b; } names[] = {
        { "red", 1, 0, 0 }, { "green", 0, 1, 0 }, { "blue", 0, 0, 1 },
        { "white", 1, 1, 1 }, { "black", 0, 0, 0 }, { "yellow", 1, 1, 0 },
        { "cyan", 0, 1, 1 }, { "magenta", 1, 0, 1 },
        { "gray", 0.5f, 0.5f, 0.5f }, { "grey", 0.5f, 0.5f, 0.5f },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if ((int)strlen(names[i].n) == len && strncmp(spec, names[i].n, (size_t)len) == 0) {
            out[0] = names[i].r; out[1] = names[i].g; out[2] = names[i].b; out[3] = 1.0f;
            return true;
        }
    }
    return false;
}

/* Copy `in` to `out` stripping every <...> tag; returns the visible length. */
static size_t uc_strip_tags(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < outsz; ++p) {
        if (*p == '<') {
            while (*p && *p != '>') ++p;
            if (!*p) break;
            continue;       /* skip the whole tag */
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return o;
}

/* Draw one rich-text line: flush visible runs in the current colour, pushing/
 * popping a colour stack on <color=..>/</color>; unknown tags are stripped. */
static void uc_draw_rich_line(JceUICanvas *uc, JceFont *font, uint16_t view_id,
                              float x, float y, const char *line,
                              const float base_rgba[4], float alpha_mul,
                              const JceRectXform *xf, float scale)
{
    float stack[8][4]; int sp = 0;
    float cur[4]; memcpy(cur, base_rgba, sizeof cur);
    char run[512]; int rn = 0;
    float cx = x;
    for (const char *p = line; *p;) {
        if (*p == '<') {
            if (rn > 0) {                    /* flush the run before the tag */
                run[rn] = '\0'; rn = 0;
                float w = 0, h = 0; jce_text_measure(font, run, &w, &h);
                jce_text_draw_scaled_view_xf(uc_renderer(uc), font, view_id,
                                             cx, y, scale, run,
                                             uc_color(cur, alpha_mul), xf);
                cx += w * scale;
            }
            const char *t = p + 1, *e = t;
            while (*e && *e != '>') ++e;
            int tlen = (int)(e - t);
            if (tlen >= 6 && strncmp(t, "color=", 6) == 0) {
                float c4[4];
                if (uc_parse_color(t + 6, tlen - 6, c4)) {
                    if (sp < 8) memcpy(stack[sp++], cur, sizeof cur);
                    memcpy(cur, c4, sizeof cur);
                }
            } else if (tlen == 6 && strncmp(t, "/color", 6) == 0) {
                if (sp > 0) memcpy(cur, stack[--sp], sizeof cur);
            }
            /* other tags (b/i/size/...) are recognised by being skipped */
            p = (*e == '>') ? e + 1 : e;
            continue;
        }
        if (rn + 1 < (int)sizeof(run)) run[rn++] = *p;
        ++p;
    }
    if (rn > 0) {
        run[rn] = '\0';
        jce_text_draw_scaled_view_xf(uc_renderer(uc), font, view_id, cx, y, scale,
                                     run, uc_color(cur, alpha_mul), xf);
    }
}

/* Width of a rich line measured the SAME way uc_draw_rich_line renders it —
 * per-run (split at every tag), summed — so centre/right alignment matches the
 * drawn glyphs (run-split shaping differs from shaping the whole line at once). */
static float uc_rich_line_width(JceFont *font, const char *line)
{
    float total = 0.0f;
    char run[512]; int rn = 0;
    for (const char *p = line; *p;) {
        if (*p == '<') {
            if (rn > 0) {
                run[rn] = '\0'; rn = 0;
                float w = 0, h = 0; jce_text_measure(font, run, &w, &h);
                total += w;
            }
            const char *e = p + 1;
            while (*e && *e != '>') ++e;
            p = (*e == '>') ? e + 1 : e;
            continue;
        }
        if (rn + 1 < (int)sizeof(run)) run[rn++] = *p;
        ++p;
    }
    if (rn > 0) {
        run[rn] = '\0';
        float w = 0, h = 0; jce_text_measure(font, run, &w, &h);
        total += w;
    }
    return total;
}

/* Width follows the actual draw mode: rich text shapes each colour run,
 * while mathematical markup has its own layout. Math height comes from that
 * same layout and extends the line box when a fraction is taller than a face. */
static void uc_text_mode_extent(JceFont *font, const char *const *lines,
                                const char *const *visible, int count,
                                const JceUITextComponent *tx, float spacing,
                                float *out_width, float *out_height,
                                float *out_line_height)
{
    float width = 0.0f;
    float line_height = (float)jce_font_line_height(font);
    for (int i = 0; i < count; ++i) {
        float w = 0.0f, h = 0.0f;
        if (tx->math_text) {
            jce_text_measure_math(font, visible[i], &w, &h);
            if (h > line_height) line_height = h;
        } else if (tx->rich_text) {
            w = uc_rich_line_width(font, lines[i]);
        } else {
            jce_text_measure(font, visible[i], &w, &h);
        }
        if (w > width) width = w;
    }
    *out_width = width;
    *out_line_height = line_height * spacing;
    *out_height = *out_line_height * (float)(count > 0 ? count : 1);
}

void uc_draw_text(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                   const JceUITextComponent *tx, float alpha_mul,
                   float ui_scale, const JceRectXform *txf)
{
    /* Inherited opacity gates every run and style; base/span colours retain
     * their independent semantics. NaN also fails closed before resources. */
    if (!(alpha_mul > 0.0f)) return;
    /* L5: a non-empty locale_key overrides `text` at display time via the
     * localization table; `text` is the fallback when no key is set.
     * jce_loc_t returns the key POINTER itself on a miss (documented
     * signal) — fall back to the authored text then, so raw keys never
     * flash before the locale tables are loaded. */
    const char *str = tx->text;
    if (tx->locale_key[0]) {
        const char *loc = jce_loc_t(tx->locale_key);
        str = (loc == tx->locale_key && tx->text[0]) ? tx->text : loc;
    }
    if (!str || !str[0]) return;

    /* Split into lines for multi-line layout (L7). Tokenize a local copy so we
     * never mutate the component / localization string. */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", str);
    const char *lines[64];
    int nlines = 0;
    lines[nlines++] = buf;
    for (char *p = buf; *p && nlines < 64; ++p) {
        if (*p == '\n') { *p = '\0'; lines[nlines++] = p + 1; }
    }

    /* L7 rich_text: measurement / alignment / best_fit use the tag-stripped
     * visible text; rendering (below) walks the original markup for per-span
     * colour.  Without rich_text, vis[] simply aliases lines[]. */
    const char *vis[64];
    char vbuf[512];
    if (tx->rich_text) {
        char  *vp   = vbuf;
        size_t vrem = sizeof(vbuf);
        for (int i = 0; i < nlines; ++i) {
            if (vrem <= 1) { vis[i] = ""; continue; }
            vis[i] = vp;
            size_t n = uc_strip_tags(lines[i], vp, vrem);
            vp += n + 1; vrem -= (n + 1);
        }
    } else {
        for (int i = 0; i < nlines; ++i) vis[i] = lines[i];
    }

    const float lsp = (tx->line_spacing > 0.0f) ? tx->line_spacing : 1.0f;
    int px = (int)((tx->font_size > 0 ? tx->font_size : 14.0f) * ui_scale);
    if (px < 1) px = 1;
    /* MAGNIFICATION, when the label is bigger than an atlas may be.  1.0 for
     * every px at or below the ceiling, which is every label authored before
     * this existed -- so nothing already on screen moves. */
    float dscale = (float)px / (float)uc_font_px_clamp(px);
    if (dscale < 1.0f) dscale = 1.0f;

    JceFont *font = NULL;
    /* Best Fit measures the complete multiline block once at the upper
     * authored size. Every candidate and the final draw scale that SAME
     * atlas, so changing the available rectangle does not retain one atlas
     * per binary-search candidate or per fitted size. Explicit-newline and
     * line-spacing metrics remain shared with the ordinary draw below. */
    if (tx->best_fit) {
        int lo = tx->min_size > 0 ? (int)(tx->min_size * ui_scale) : 1;
        int hi = tx->max_size > 0 ? (int)(tx->max_size * ui_scale) : px;
        if (lo < 1) lo = 1;
        if (hi < lo) hi = lo;
        int atlas_px = uc_font_px_clamp(hi);
        font = uc_get_font(uc, tx->font_path, atlas_px, tx->sdf);
        if (!font) return;
        float width = 0.0f, height = 0.0f;
        float line_height = 0.0f;
        uc_text_mode_extent(font, lines, vis, nlines, tx, lsp,
                             &width, &height, &line_height);
        px = uc_text_fit_size(lo, hi, atlas_px, width, height, r->w, r->h);
        dscale = (float)px / (float)atlas_px;
    } else {
        font = uc_get_font(uc, tx->font_path, px, tx->sdf);
    }
    if (!font) return;

    /* ── Word wrap (JCE_UI_TEXT_OVERFLOW_WRAP) ──────────────────────
     *
     * Greedy, on token boundaries: a run of non-space bytes plus the spaces
     * that follow it.  A "token" with no space in it -- CJK, or one very long
     * word -- falls back to breaking at UTF-8 codepoint boundaries, so a line
     * of Chinese wraps too rather than running off the rect.
     *
     * The whole line is measured ONCE first and skipped entirely when it
     * already fits, so the common case costs one measure per line rather than
     * one per token.
     *
     * NOT applied to rich_text or math_text: a break inside a <color> span or
     * a math run would have to re-open the markup on the next line, and the
     * draw path walks the ORIGINAL markup string per line.  Those two keep
     * clipping, which is what they did before this existed. */
    char wbuf[1024];
    if (tx->overflow == JCE_UI_TEXT_OVERFLOW_WRAP &&
        !tx->rich_text && !tx->math_text && r->w > 1.0f) {
        const float maxw = r->w;
        char *wp = wbuf;
        size_t wrem = sizeof wbuf;
        const char *src[64];
        int nsrc = nlines;
        for (int i = 0; i < nsrc && i < 64; ++i) src[i] = vis[i];
        int out_n = 0;

        for (int i = 0; i < nsrc && out_n < 64 && wrem > 1; ++i) {
            const char *ln = src[i];
            float lw = 0.0f, lhh = 0.0f;
            jce_text_measure(font, ln, &lw, &lhh);
            lw *= dscale;
            if (lw <= maxw) {                       /* fits: copy verbatim */
                size_t n = strlen(ln);
                if (n + 1 > wrem) n = wrem - 1;
                memcpy(wp, ln, n); wp[n] = '\0';
                lines[out_n] = wp; vis[out_n] = wp; out_n++;
                wp += n + 1; wrem -= n + 1;
                continue;
            }
            /* Does not fit: accumulate tokens until one would overflow. */
            char seg[512];
            size_t seglen = 0;
            seg[0] = '\0';
            size_t p = 0, n = strlen(ln);
            while (p < n && out_n < 64 && wrem > 1) {
                /* one token: non-spaces, then the spaces after them */
                size_t t0 = p;
                while (p < n && ln[p] != ' ') {
                    p++;                              /* to a UTF-8 boundary */
                    while (p < n && ((unsigned char)ln[p] & 0xC0u) == 0x80u) p++;
                    if (t0 == 0 && p >= n) break;
                }
                size_t tend = p;
                while (p < n && ln[p] == ' ') p++;
                size_t tlen = p - t0;
                if (tlen == 0) break;

                char cand[512];
                size_t clen = seglen + tlen;
                if (clen >= sizeof cand) clen = sizeof cand - 1;
                memcpy(cand, seg, seglen);
                memcpy(cand + seglen, ln + t0,
                       (clen > seglen) ? (clen - seglen) : 0);
                cand[clen] = '\0';

                float cw = 0.0f, chh = 0.0f;
                jce_text_measure(font, cand, &cw, &chh);
                cw *= dscale;
                if (cw > maxw && seglen > 0) {
                    /* Emit what we had; this token starts the next line.
                     * Trailing spaces are dropped so alignment is not skewed
                     * by an invisible run at the end of the line. */
                    while (seglen > 0 && seg[seglen - 1] == ' ') seglen--;
                    seg[seglen] = '\0';
                    size_t need = seglen + 1;
                    if (need > wrem) need = wrem;
                    memcpy(wp, seg, need - 1); wp[need - 1] = '\0';
                    lines[out_n] = wp; vis[out_n] = wp; out_n++;
                    wp += need; wrem -= need;
                    seglen = 0; seg[0] = '\0';
                    p = t0;                    /* re-consume this token */
                    continue;
                }
                memcpy(seg, cand, clen); seglen = clen; seg[seglen] = '\0';
                (void)tend;
            }
            if (seglen > 0 && out_n < 64 && wrem > 1) {
                while (seglen > 0 && seg[seglen - 1] == ' ') seglen--;
                seg[seglen] = '\0';
                size_t need = seglen + 1;
                if (need > wrem) need = wrem;
                memcpy(wp, seg, need - 1); wp[need - 1] = '\0';
                lines[out_n] = wp; vis[out_n] = wp; out_n++;
                wp += need; wrem -= need;
            }
        }
        if (out_n > 0) nlines = out_n;
    }

    float measured_width = 0.0f, block_h = 0.0f, lh = 0.0f;
    uc_text_mode_extent(font, lines, vis, nlines, tx, lsp,
                         &measured_width, &block_h, &lh);
    lh *= dscale;
    block_h *= dscale;
    /* VERTICAL alignment.  MIDDLE is 0 because it is the only behaviour that
     * existed before the field, so an old scene and a memset-zero component
     * both keep centring (see JCE_UI_TEXT_VALIGN_* in jce_scene.h). */
    float oy;
    switch (tx->vertical_alignment) {
    case JCE_UI_TEXT_VALIGN_TOP:    oy = r->y;                        break;
    case JCE_UI_TEXT_VALIGN_BOTTOM: oy = r->y + (r->h - block_h);     break;
    default:                        oy = r->y + (r->h - block_h) * 0.5f; break;
    }
    if (oy < r->y) oy = r->y;

    uint32_t col = uc_color(tx->color, alpha_mul);

    /* OUTLINE / SHADOW for this label, passed to the draw rather than set as a
     * mode -- so there is nothing to leave set and nothing for the next label
     * to inherit.
     *
     * Only for a distance-field label: a coverage atlas has no distance to
     * threshold a second time, so setting it for a bitmap font would be
     * uploading uniforms the shader that runs cannot read. */
    JceTextStyle st;
    const JceTextStyle *stp = NULL;
    if (tx->sdf && (tx->outline_width > 0.0f || tx->shadow_color[3] > 0.0f)) {
        memset(&st, 0, sizeof st);
        st.outline_width = tx->outline_width * ui_scale;
        st.shadow_offset[0] = tx->shadow_offset[0] * ui_scale;
        st.shadow_offset[1] = tx->shadow_offset[1] * ui_scale;
        for (int k = 0; k < 4; k++) {
            st.outline_color[k] = tx->outline_color[k];
            st.shadow_color[k]  = tx->shadow_color[k];
        }
        /* The label's own alpha multiplies both, so a faded CanvasGroup fades
         * the outline and the shadow with the face instead of leaving them
         * behind at full strength. */
        st.outline_color[3] *= alpha_mul;
        st.shadow_color[3]  *= alpha_mul;
        stp = &st;
    }

    for (int i = 0; i < nlines; ++i, oy += lh) {
        if (!vis[i][0]) continue;                /* visibly-blank line: advance */
        float lw = 0.0f, lhh = 0.0f;
        if (tx->math_text)     jce_text_measure_math(font, vis[i], &lw, &lhh);
        else if (tx->rich_text) lw = uc_rich_line_width(font, lines[i]); /* match run-split render */
        else jce_text_measure(font, vis[i], &lw, &lhh);
        lw *= dscale;
        float ox = r->x;                         /* horizontal alignment per line */
        if (tx->alignment == JCE_UI_TEXT_ALIGN_CENTER) ox = r->x + (r->w - lw) * 0.5f;
        else if (tx->alignment == JCE_UI_TEXT_ALIGN_RIGHT) ox = r->x + (r->w - lw);
        if (tx->math_text)
            /* Math typesetting keeps its own glyph batching and has no
             * transform entry point yet; a rotated math label stays
             * upright, which is visible rather than silent. */
            jce_text_draw_math_view(uc_renderer(uc), font, view_id, ox, oy, dscale, vis[i], col);
        else if (tx->rich_text)
            uc_draw_rich_line(uc, font, view_id, ox, oy, lines[i],
                              tx->color, alpha_mul, txf, dscale);
        else
            jce_text_draw_styled_view_xf(uc_renderer(uc), font, view_id, ox, oy,
                                         dscale, vis[i], col, txf, stp);
    }
}
