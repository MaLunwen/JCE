/*
 * jce_ui_canvas.c  ECS UI (Canvas/RectTransform/Image/Text/Button) renderer.
 *
 * Three passes per frame, all driven from jce_ui_canvas_render():
 *
 *   1. LAYOUT   — for every Screen-Space-Overlay Canvas, walk its UI subtree
 *                 and resolve each element's screen rect from its parent rect
 *                 + RectTransform (anchors / pivot / sizeDelta / anchoredPos).
 *                 A LayoutGroup on a parent overrides its direct children's
 *                 positions with horizontal / vertical / grid arrangement.
 *
 *   2. RAYCAST  — top-most-first hit test of the pointer against raycast-
 *                 target rects; drives UIButton hover / press / click and the
 *                 colour-tint state machine (normal/highlight/pressed fade).
 *
 *   3. RENDER   — depth-first draw: UIImage (simple quad / 9-slice) then
 *                 UIText (FreeType+HarfBuzz glyph atlas) as transient quads in
 *                 screen space, modulated by the inherited CanvasGroup alpha.
 *
 * This is the ECS-UI path (Unity-UGUI shaped).  It is independent of and does
 * NOT touch RmlUI (the shipping HTML/CSS game-UI path).
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/middleware/ui/jce_localization.h>
#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_str.h>
#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_texture.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "ui_canvas"

#define UC_MAX_FONTS    32
#define UC_MAX_TEXTURES 64
#define UC_MAX_BUTTONS  256
#define UC_MAX_DEPTH    32   /* RectTransform nesting depth guard */
#define UC_DEFAULT_FONT "fonts/JCE.ttf"

/* A resolved screen rectangle (top-left origin, logical pixels). */
typedef struct { float x, y, w, h; } UCRect;

/* Cached font, keyed by (path, integer px size). */
typedef struct {
    char     path[256];
    int      px;
    JceFont *font;
} UCFontSlot;

/* Cached sprite texture, keyed by path.  tex.idx == UINT16_MAX ⇒ load failed
 * (cached negative so we don't re-try every frame). */
typedef struct {
    char       path[256];
    JceTexture tex;
} UCTexSlot;

/* Per-button interaction state, persisted across frames. */
typedef struct {
    uint64_t entity;       /* 0 = free slot */
    float    fade;         /* 0..1 toward target tint */
    int      state;        /* 0 normal, 1 hover, 2 pressed */
    bool     press_inside; /* press began over this button */
} UCButtonState;

struct JceUICanvas {
    JceRenderer         *renderer;
    const JcePakArchive *pak;

    UCFontSlot    fonts[UC_MAX_FONTS];
    int           font_count;

    UCTexSlot     textures[UC_MAX_TEXTURES];
    int           texture_count;

    UCButtonState buttons[UC_MAX_BUTTONS];

    uint64_t      last_clicked;
};

/* ── Resource caches ───────────────────────────────────────────────── */

static JceFont *uc_get_font(JceUICanvas *uc, const char *path, int px)
{
    if (px < 4)   px = 4;
    if (px > 256) px = 256;
    const char *p = (path && path[0]) ? path : UC_DEFAULT_FONT;

    for (int i = 0; i < uc->font_count; i++) {
        if (uc->fonts[i].px == px && strcmp(uc->fonts[i].path, p) == 0)
            return uc->fonts[i].font;   /* may be NULL = cached failure */
    }
    if (uc->font_count >= UC_MAX_FONTS || !uc->pak)
        return NULL;

    /* Pre-render the engine's curated CJK/Latin-extended codepoints so UI
     * text matches what the editor / HUD already supports. */
    JceFont *f = NULL;
    {
        uint32_t cps[256];
        int n = jce_i18n_collect_codepoints(cps, 256);
        f = jce_font_open_ex(uc->pak, p, (float)px, cps, n);
    }
    if (!f && strcmp(p, UC_DEFAULT_FONT) != 0) {
        /* Fall back to the default engine font. */
        uint32_t cps[256];
        int n = jce_i18n_collect_codepoints(cps, 256);
        f = jce_font_open_ex(uc->pak, UC_DEFAULT_FONT, (float)px, cps, n);
    }

    UCFontSlot *slot = &uc->fonts[uc->font_count++];
    jce_strlcpy(slot->path, p, sizeof slot->path);
    slot->px   = px;
    slot->font = f;
    if (!f) LOG_WARN(LOG_TAG, "font load failed: %s @%dpx", p, px);
    return f;
}

static JceTexture uc_get_texture(JceUICanvas *uc, const char *path)
{
    if (!path || !path[0]) return JCE_TEXTURE_INVALID;
    for (int i = 0; i < uc->texture_count; i++) {
        if (strcmp(uc->textures[i].path, path) == 0)
            return uc->textures[i].tex;
    }
    if (uc->texture_count >= UC_MAX_TEXTURES)
        return JCE_TEXTURE_INVALID;

    JceTexture t = uc->pak ? jce_texture_load(uc->pak, path)
                           : JCE_TEXTURE_INVALID;
    UCTexSlot *slot = &uc->textures[uc->texture_count++];
    jce_strlcpy(slot->path, path, sizeof slot->path);
    slot->tex = t;
    return t;
}

static UCButtonState *uc_button_state(JceUICanvas *uc, uint64_t e)
{
    UCButtonState *free_slot = NULL;
    for (int i = 0; i < UC_MAX_BUTTONS; i++) {
        if (uc->buttons[i].entity == e) return &uc->buttons[i];
        if (!free_slot && uc->buttons[i].entity == 0) free_slot = &uc->buttons[i];
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->entity = e;
        free_slot->fade   = 1.0f;
        return free_slot;
    }
    return NULL;
}

/* ── Colour helpers ────────────────────────────────────────────────── */

static uint32_t uc_color(const float rgba[4], float alpha_mul)
{
    float a = rgba[3] * alpha_mul;
    float r = rgba[0], g = rgba[1], b = rgba[2];
    if (r < 0) r = 0; if (r > 1) r = 1;
    if (g < 0) g = 0; if (g > 1) g = 1;
    if (b < 0) b = 0; if (b > 1) b = 1;
    if (a < 0) a = 0; if (a > 1) a = 1;
    return jce_rgba((uint8_t)(r * 255.0f), (uint8_t)(g * 255.0f),
                    (uint8_t)(b * 255.0f), (uint8_t)(a * 255.0f));
}

static void uc_lerp4(const float a[4], const float b[4], float t, float out[4])
{
    for (int i = 0; i < 4; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

/* ── RectTransform resolution ──────────────────────────────────────── */

/* Resolve a child's screen rect from its parent rect + RectTransform.
 * A zero-initialised RectTransform (legacy authoring) is treated as a
 * full-stretch rect filling the parent. */
static UCRect uc_resolve_rect(const UCRect *parent, const JceRectTransform *rt,
                              float scale, bool pixel_perfect)
{
    JceRectTransform z;
    if (!rt) { memset(&z, 0, sizeof z); rt = &z; }

    float a_min_x = rt->anchor_min[0], a_min_y = rt->anchor_min[1];
    float a_max_x = rt->anchor_max[0], a_max_y = rt->anchor_max[1];
    float piv_x   = rt->pivot[0],      piv_y   = rt->pivot[1];
    /* px dimensions are scaled by the CanvasScaler factor. */
    float sd_w    = rt->size_delta[0] * scale, sd_h = rt->size_delta[1] * scale;
    float ap_x    = rt->anchored_position[0] * scale;
    float ap_y    = rt->anchored_position[1] * scale;

    /* All-zero ⇒ default full-stretch with centred pivot. */
    bool all_zero = (a_min_x == 0 && a_min_y == 0 && a_max_x == 0 &&
                     a_max_y == 0 && piv_x == 0 && piv_y == 0 &&
                     rt->anchored_position[0] == 0 &&
                     rt->anchored_position[1] == 0 && sd_w == 0 && sd_h == 0);
    if (all_zero) {
        a_max_x = 1.0f; a_max_y = 1.0f;
        piv_x = 0.5f;   piv_y = 0.5f;
    }

    /* Anchor reference points in parent space (top-left origin). */
    float anc_min_x = parent->x + parent->w * a_min_x;
    float anc_max_x = parent->x + parent->w * a_max_x;
    float anc_min_y = parent->y + parent->h * a_min_y;
    float anc_max_y = parent->y + parent->h * a_max_y;

    UCRect r;
    /* X axis. */
    if (a_min_x == a_max_x) {
        /* Fixed width: size_delta.x is the width; anchored from pivot. */
        r.w = sd_w;
        r.x = anc_min_x + ap_x - piv_x * r.w;
    } else {
        /* Stretch: size_delta.x is symmetric inset from the anchored span. */
        r.x = anc_min_x + sd_w * 0.5f + ap_x;
        r.w = (anc_max_x - anc_min_x) - sd_w;
    }
    /* Y axis. */
    if (a_min_y == a_max_y) {
        r.h = sd_h;
        r.y = anc_min_y + ap_y - piv_y * r.h;
    } else {
        r.y = anc_min_y + sd_h * 0.5f + ap_y;
        r.h = (anc_max_y - anc_min_y) - sd_h;
    }
    /* pixel_perfect: snap to whole pixels to avoid sub-pixel blur. */
    if (pixel_perfect) {
        r.x = floorf(r.x + 0.5f); r.y = floorf(r.y + 0.5f);
        r.w = floorf(r.w + 0.5f); r.h = floorf(r.h + 0.5f);
    }
    return r;
}

/* Read whichever UI graphic RectTransform an entity carries. */
static const JceRectTransform *uc_entity_rect(JceScene *s, JceEntity e)
{
    JceUIImageComponent *im = jce_scene_get_ui_image(s, e);
    if (im) return &im->rect;
    JceUITextComponent *tx = jce_scene_get_ui_text(s, e);
    if (tx) return &tx->rect;
    return NULL;
}

static bool uc_is_ui_element(JceScene *s, JceEntity e)
{
    return jce_scene_has_ui_image(s, e) || jce_scene_has_ui_text(s, e);
}

/* ── Raycast accumulation ──────────────────────────────────────────── */

typedef struct {
    JceEntity entity;
    UCRect    rect;
    float     alpha;        /* inherited canvas-group alpha */
    bool      interactable; /* inherited canvas-group interactable (AND chain) */
} UCHit;

typedef struct {
    JceUICanvas *uc;
    JceScene    *scene;
    uint16_t     view_id;
    /* CanvasScaler (overlay "scale with screen size"): multiplies every px
     * dimension (size_delta / anchored_position / font size / 9-slice borders)
     * so UI authored at the canvas reference resolution scales uniformly. */
    float        ui_scale;
    bool         pixel_perfect;
    /* draw order accumulation for raycast top-most resolution */
    UCHit        hits[UC_MAX_BUTTONS];
    int          hit_count;
} UCFrame;

/* ── 9-slice / image draw ──────────────────────────────────────────── */

static void uc_draw_image(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                          const JceUIImageComponent *im, float alpha_mul,
                          float ui_scale)
{
    uint32_t tint = uc_color(im->color, alpha_mul);
    JceTexture tex = uc_get_texture(uc, im->sprite_path);

    if (!jce_texture_valid(tex)) {
        /* No sprite: flat coloured quad (still a valid UI panel). */
        jce_draw_filled_rect_view(uc->renderer, view_id, r->x, r->y, r->w, r->h, tint);
        return;
    }

    bool sliced = (im->image_type == JCE_UI_IMAGE_SLICED) &&
                  (im->slice_border[0] + im->slice_border[1] +
                   im->slice_border[2] + im->slice_border[3] > 0.0f);
    if (!sliced) {
        uint32_t tw = 0, th = 0;
        jce_texture_get_size(tex, &tw, &th);

        float dx = r->x, dy = r->y, dw = r->w, dh = r->h;

        /* preserve_aspect: fit the texture's aspect ratio inside the rect,
         * centred (letterbox/pillarbox). */
        if (im->preserve_aspect && tw > 0 && th > 0 && dw > 0 && dh > 0) {
            float ar = (float)tw / (float)th;
            if (ar > dw / dh) { float nh = dw / ar; dy += (dh - nh) * 0.5f; dh = nh; }
            else              { float nw = dh * ar; dx += (dw - nw) * 0.5f; dw = nw; }
        }

        if (im->image_type == JCE_UI_IMAGE_TILED && tw > 0 && th > 0) {
            /* Repeat the sprite at native pixel size across the rect.  Needs a
             * wrapping sampler; degrades to clamp otherwise. */
            float uv[4] = { 0.0f, 0.0f, dw / (float)tw, dh / (float)th };
            jce_draw_textured_rect_view(uc->renderer, view_id, dx, dy, dw, dh, tex, tint, uv);
            return;
        }

        if (im->image_type == JCE_UI_IMAGE_FILLED) {
            /* Horizontal left-to-right fill: show the leftmost fill_amount of
             * both the rect and the sprite (cropped, not stretched). */
            float amt = im->fill_amount < 0.0f ? 0.0f : (im->fill_amount > 1.0f ? 1.0f : im->fill_amount);
            if (amt <= 0.0f) return;
            float uv[4] = { 0.0f, 0.0f, amt, 1.0f };
            jce_draw_textured_rect_view(uc->renderer, view_id, dx, dy, dw * amt, dh, tex, tint, uv);
            return;
        }

        /* SIMPLE (and any other type): single quad, full sprite. */
        jce_draw_textured_rect_view(uc->renderer, view_id, dx, dy, dw, dh, tex, tint, NULL);
        return;
    }

    /* 9-slice: split into a 3x3 grid.  Borders are in source pixels. */
    uint32_t tw = 0, th = 0;
    jce_texture_get_size(tex, &tw, &th);
    if (tw == 0 || th == 0) {
        jce_draw_textured_rect_view(uc->renderer, view_id, r->x, r->y,
                                    r->w, r->h, tex, tint, NULL);
        return;
    }
    float bl = im->slice_border[0], br = im->slice_border[1];
    float bt = im->slice_border[2], bb = im->slice_border[3];

    /* Border widths in DST space scale with the CanvasScaler factor; the UV
     * (source-texture) borders below stay unscaled. */
    float dbl0 = bl * ui_scale, dbr0 = br * ui_scale;
    float dbt0 = bt * ui_scale, dbb0 = bb * ui_scale;
    /* Clamp borders so opposite borders never exceed the dst rect. */
    float dst_lr = dbl0 + dbr0, dst_tb = dbt0 + dbb0;
    float sx = (dst_lr > r->w && dst_lr > 0) ? r->w / dst_lr : 1.0f;
    float sy = (dst_tb > r->h && dst_tb > 0) ? r->h / dst_tb : 1.0f;
    float dbl = dbl0 * sx, dbr = dbr0 * sx, dbt = dbt0 * sy, dbb = dbb0 * sy;

    float u_l = bl / (float)tw, u_r = 1.0f - br / (float)tw;
    float v_t = bt / (float)th, v_b = 1.0f - bb / (float)th;

    float cx[4] = { r->x, r->x + dbl, r->x + r->w - dbr, r->x + r->w };
    float cy[4] = { r->y, r->y + dbt, r->y + r->h - dbb, r->y + r->h };
    float cu[4] = { 0.0f, u_l, u_r, 1.0f };
    float cv[4] = { 0.0f, v_t, v_b, 1.0f };

    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            float qx = cx[col], qy = cy[row];
            float qw = cx[col + 1] - cx[col];
            float qh = cy[row + 1] - cy[row];
            if (qw <= 0.0f || qh <= 0.0f) continue;
            float uv[4] = { cu[col], cv[row], cu[col + 1], cv[row + 1] };
            jce_draw_textured_rect_view(uc->renderer, view_id, qx, qy, qw, qh,
                                        tex, tint, uv);
        }
    }
}

/* ── Text draw ─────────────────────────────────────────────────────── */

/* Measure the widest line and total block height of a (possibly multi-line)
 * line list at `font`, with `lsp` line-spacing multiplier. */
static void uc_text_block_extent(JceFont *font, const char *const *lines, int nlines,
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
                              const float base_rgba[4], float alpha_mul)
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
                jce_text_draw_scaled_view(uc->renderer, font, view_id, cx, y, 1.0f,
                                          run, uc_color(cur, alpha_mul));
                cx += w;
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
        jce_text_draw_scaled_view(uc->renderer, font, view_id, cx, y, 1.0f,
                                  run, uc_color(cur, alpha_mul));
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

static void uc_draw_text(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                         const JceUITextComponent *tx, float alpha_mul,
                         float ui_scale)
{
    /* L5: a non-empty locale_key overrides `text` at display time via the
     * localization table; `text` is the fallback when no key is set. */
    const char *str = (tx->locale_key[0]) ? jce_loc_t(tx->locale_key) : tx->text;
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

    /* L7 best_fit: shrink px within the (scaled) [min_size,max_size] range
     * until the whole block fits the rect (or we hit min_size). */
    if (tx->best_fit) {
        int lo = tx->min_size > 0 ? (int)(tx->min_size * ui_scale) : 1;
        int hi = tx->max_size > 0 ? (int)(tx->max_size * ui_scale) : px;
        if (lo < 1) lo = 1;
        if (hi < lo) hi = lo;
        /* Binary-search the largest px in [lo,hi] whose block fits the rect:
         * O(log range) font instantiations rather than a per-px scan that could
         * fill the small (UC_MAX_FONTS) per-canvas font cache. */
        int best = lo;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            JceFont *f = uc_get_font(uc, tx->font_path, mid);
            if (!f) { hi = mid - 1; continue; }   /* treat as "doesn't fit" */
            float bw = 0.0f, bh = 0.0f;
            uc_text_block_extent(f, vis, nlines, lsp, &bw, &bh);
            if (bw <= r->w && bh <= r->h) { best = mid; lo = mid + 1; }
            else                          { hi = mid - 1; }
        }
        px = best;
    }

    JceFont *font = uc_get_font(uc, tx->font_path, px);
    if (!font) return;

    const float lh = (float)jce_font_line_height(font) * lsp;
    float block_h = lh * (float)nlines;
    float oy = r->y + (r->h - block_h) * 0.5f;   /* vertically centre the block */
    if (oy < r->y) oy = r->y;

    uint32_t col = uc_color(tx->color, alpha_mul);
    for (int i = 0; i < nlines; ++i, oy += lh) {
        if (!vis[i][0]) continue;                /* visibly-blank line: advance */
        float lw = 0.0f, lhh = 0.0f;
        if (tx->rich_text) lw = uc_rich_line_width(font, lines[i]); /* match run-split render */
        else jce_text_measure(font, vis[i], &lw, &lhh);
        float ox = r->x;                         /* horizontal alignment per line */
        if (tx->alignment == JCE_UI_TEXT_ALIGN_CENTER) ox = r->x + (r->w - lw) * 0.5f;
        else if (tx->alignment == JCE_UI_TEXT_ALIGN_RIGHT) ox = r->x + (r->w - lw);
        if (tx->rich_text)
            uc_draw_rich_line(uc, font, view_id, ox, oy, lines[i], tx->color, alpha_mul);
        else
            jce_text_draw_scaled_view(uc->renderer, font, view_id, ox, oy, 1.0f, vis[i], col);
    }
}

/* ── LayoutGroup arrangement ───────────────────────────────────────── */

/* Apply a LayoutGroup on `parent` to its UI children, overriding the
 * children's resolved rects with packed positions. */
static void uc_apply_layout_group(JceScene *s, const UCRect *parent_rect,
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
        int cols = (int)((inner_w + sx) / (cw + sx));
        if (cols < 1) cols = 1;
        for (int i = 0; i < child_count; i++) {
            int idx = lg->reverse_arrangement ? (child_count - 1 - i) : i;
            int row = i / cols, col = i % cols;
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

    /* Equal-share size when controlling child size on the main axis;
     * otherwise sum the children's preferred sizes to support alignment. */
    float ctrl_main = 0.0f, total = 0.0f;
    if (ctrl_main_axis) {
        float box = horizontal ? inner_w : inner_h;
        float gaps = spacing * (float)(child_count > 0 ? child_count - 1 : 0);
        ctrl_main = (child_count > 0) ? (box - gaps) / (float)child_count : 0.0f;
        total = box;
    } else {
        for (int i = 0; i < child_count; i++)
            total += horizontal ? child_rects[i].w : child_rects[i].h;
        if (child_count > 1) total += spacing * (float)(child_count - 1);
    }

    /* child_alignment: Unity TextAnchor 0..8 (rows: upper/middle/lower,
     * cols: left/center/right).  Use it to offset the packed run within the
     * inner box on the main axis. */
    float start = horizontal ? inner_x : inner_y;
    {
        float box = horizontal ? inner_w : inner_h;
        float slack = box - total;
        if (slack < 0) slack = 0;
        int along = horizontal ? (lg->child_alignment % 3)        /* L/C/R */
                               : (lg->child_alignment / 3);        /* U/M/L */
        if (along == 1) start += slack * 0.5f;
        else if (along == 2) start += slack;
    }

    float cursor = start;
    for (int i = 0; i < child_count; i++) {
        int idx = lg->reverse_arrangement ? (child_count - 1 - i) : i;
        UCRect *cr = &child_rects[idx];
        if (horizontal) {
            float w = (lg->control_child_size_w) ? ctrl_main : cr->w;
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
            float h = (lg->control_child_size_h) ? ctrl_main : cr->h;
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
}

/* ── Recursive layout + draw ───────────────────────────────────────── */

static float uc_node_alpha(JceScene *s, JceEntity e, float inherited)
{
    JceCanvasGroupComponent *cg = jce_scene_get_canvas_group(s, e);
    if (cg) {
        if (cg->ignore_parent_groups) return cg->alpha;
        return inherited * cg->alpha;
    }
    return inherited;
}

/* Resolve a node's effective CanvasGroup raycast state from the inherited
 * chain.  `blocks_raycasts == false` makes the node (and its subtree) invisible
 * to the pointer (events pass through); `interactable == false` keeps the node
 * blocking but disables its button.  `ignore_parent_groups` resets the chain to
 * this group's own values, mirroring uc_node_alpha. */
static void uc_node_raycast(JceScene *s, JceEntity e,
                            bool inherited_blocks, bool inherited_inter,
                            bool *out_blocks, bool *out_inter)
{
    JceCanvasGroupComponent *cg = jce_scene_get_canvas_group(s, e);
    if (cg) {
        if (cg->ignore_parent_groups) {
            *out_blocks = cg->blocks_raycasts;
            *out_inter  = cg->interactable;
        } else {
            *out_blocks = inherited_blocks && cg->blocks_raycasts;
            *out_inter  = inherited_inter  && cg->interactable;
        }
        return;
    }
    *out_blocks = inherited_blocks;
    *out_inter  = inherited_inter;
}

static void uc_layout_draw(UCFrame *fr, JceEntity node, const UCRect *node_rect,
                           float inherited_alpha, bool inherited_blocks,
                           bool inherited_inter, int depth)
{
    JceScene *s = fr->scene;
    JceUICanvas *uc = fr->uc;
    if (depth > UC_MAX_DEPTH) return;

    float alpha = uc_node_alpha(s, node, inherited_alpha);
    bool  blocks = inherited_blocks, inter = inherited_inter;
    uc_node_raycast(s, node, inherited_blocks, inherited_inter, &blocks, &inter);

    /* Draw this node's own graphics (image first, then text on top).  This
     * also covers a Canvas entity that itself carries a UIImage (full-screen
     * background) — its node_rect is the whole canvas. */
    {
        JceUIImageComponent *im = jce_scene_get_ui_image(s, node);
        if (im) uc_draw_image(uc, fr->view_id, node_rect, im, alpha, fr->ui_scale);

        /* Record raycast hit for interactive elements — but only while the
         * inherited CanvasGroup chain blocks raycasts (else the pointer passes
         * through).  The inherited `interactable` rides along so a button under
         * a non-interactable group is driven to its disabled state. */
        JceUIButtonComponent *bt = jce_scene_get_ui_button(s, node);
        bool ray = (im && im->raycast_target) || (bt != NULL);
        if (ray && blocks && fr->hit_count < UC_MAX_BUTTONS) {
            fr->hits[fr->hit_count].entity       = node;
            fr->hits[fr->hit_count].rect         = *node_rect;
            fr->hits[fr->hit_count].alpha        = alpha;
            fr->hits[fr->hit_count].interactable = inter;
            fr->hit_count++;
        }

        JceUITextComponent *tx = jce_scene_get_ui_text(s, node);
        if (tx) uc_draw_text(uc, fr->view_id, node_rect, tx, alpha, fr->ui_scale);
    }

    /* Children. */
    int child_count = jce_scene_get_child_count(s, node);
    if (child_count <= 0) return;

    /* Gather UI children. */
    enum { MAX_LOCAL_CHILDREN = 128 };
    JceEntity kids[MAX_LOCAL_CHILDREN];
    int n = jce_scene_get_children(s, node, kids, MAX_LOCAL_CHILDREN);
    if (n <= 0) return;

    /* Filter to UI elements only and resolve each child's rect. */
    JceEntity ui_kids[MAX_LOCAL_CHILDREN];
    UCRect    kid_rects[MAX_LOCAL_CHILDREN];
    int ui_n = 0;
    for (int i = 0; i < n && ui_n < MAX_LOCAL_CHILDREN; i++) {
        if (!uc_is_ui_element(s, kids[i])) continue;
        const JceRectTransform *rt = uc_entity_rect(s, kids[i]);
        ui_kids[ui_n]  = kids[i];
        kid_rects[ui_n] = uc_resolve_rect(node_rect, rt, fr->ui_scale, fr->pixel_perfect);
        ui_n++;
    }

    /* LayoutGroup on this node overrides child positions. */
    JceLayoutGroupComponent *lg = jce_scene_get_layout_group(s, node);
    if (lg && ui_n > 0)
        uc_apply_layout_group(s, node_rect, lg, ui_kids, ui_n, kid_rects, fr->ui_scale);

    for (int i = 0; i < ui_n; i++)
        uc_layout_draw(fr, ui_kids[i], &kid_rects[i], alpha, blocks, inter, depth + 1);
}

/* ── Button raycast + state machine ────────────────────────────────── */

static bool uc_point_in(const UCRect *r, float px, float py)
{
    return px >= r->x && px < r->x + r->w &&
           py >= r->y && py < r->y + r->h;
}

static void uc_update_buttons(UCFrame *fr, const JceUIPointer *ptr, float dt)
{
    JceUICanvas *uc = fr->uc;
    JceScene *s = fr->scene;
    uc->last_clicked = 0;

    /* Top-most hit = last recorded (drawn last → on top). */
    JceEntity hovered = 0;
    if (ptr && ptr->valid) {
        for (int i = fr->hit_count - 1; i >= 0; i--) {
            if (fr->hits[i].alpha <= 0.001f) continue;
            if (uc_point_in(&fr->hits[i].rect, ptr->x, ptr->y)) {
                hovered = fr->hits[i].entity;
                break;
            }
        }
    }

    /* Reap stale button states (entities no longer present). */
    for (int i = 0; i < UC_MAX_BUTTONS; i++) {
        if (uc->buttons[i].entity &&
            !jce_scene_has_ui_button(s, (JceEntity)uc->buttons[i].entity))
            uc->buttons[i].entity = 0;
    }

    /* Drive each visible button's state. */
    for (int i = 0; i < fr->hit_count; i++) {
        JceEntity e = fr->hits[i].entity;
        JceUIButtonComponent *bt = jce_scene_get_ui_button(s, e);
        if (!bt) continue;

        UCButtonState *st = uc_button_state(uc, (uint64_t)e);
        if (!st) continue;

        bool over = (hovered == e);
        int target = 0; /* normal */
        if (!ptr || !ptr->valid) {
            /* Pointer gone (cursor left the viewport): drop any in-flight
             * press so it cannot resume as a stale click next frame. */
            st->press_inside = false;
        }
        /* Disabled when the button itself OR its inherited CanvasGroup chain is
         * non-interactable. */
        if (!bt->interactable || !fr->hits[i].interactable) {
            target = 3; /* disabled */
            st->press_inside = false;
        } else {
            if (ptr && ptr->down && over && !st->press_inside) {
                /* press began this frame over the button */
                st->press_inside = true;
            }
            if (st->press_inside && ptr && !ptr->down) {
                /* release: click only if released over the button */
                if (over) uc->last_clicked = (uint64_t)e;
                st->press_inside = false;
            }
            if (st->press_inside && over)      target = 2; /* pressed */
            else if (over)                     target = 1; /* hover   */
            else                               target = 0; /* normal  */
        }

        if (target != st->state) { st->state = target; st->fade = 0.0f; }
        float fade_dur = bt->fade_duration > 0 ? bt->fade_duration : 0.0001f;
        st->fade += dt / fade_dur;
        if (st->fade > 1.0f) st->fade = 1.0f;

        /* Overdraw the button's UIImage rect with the current state colour.
         * The base image was already drawn this frame with its authored
         * colour; blending authored→state by `fade` makes the transition
         * visible (Unity ColorTint transition). */
        JceUIImageComponent *im = jce_scene_get_ui_image(s, e);
        if (im) {
            const float *col = bt->normal_color;
            if (st->state == 1) col = bt->highlighted_color;
            else if (st->state == 2) col = bt->pressed_color;
            else if (st->state == 3) col = bt->disabled_color;
            float blended[4];
            uc_lerp4(im->color, col, st->fade, blended);
            uint32_t tint = uc_color(blended, fr->hits[i].alpha);
            JceTexture tex = uc_get_texture(uc, im->sprite_path);
            if (jce_texture_valid(tex))
                jce_draw_textured_rect_view(uc->renderer, fr->view_id,
                                            fr->hits[i].rect.x, fr->hits[i].rect.y,
                                            fr->hits[i].rect.w, fr->hits[i].rect.h,
                                            tex, tint, NULL);
            else
                jce_draw_filled_rect_view(uc->renderer, fr->view_id,
                                          fr->hits[i].rect.x, fr->hits[i].rect.y,
                                          fr->hits[i].rect.w, fr->hits[i].rect.h,
                                          tint);
        }
    }
}

/* ── Canvas enumeration ────────────────────────────────────────────── */

typedef struct { JceEntity list[64]; int count; } UCCanvasList;

static void uc_collect_canvas_cb(JceScene *s, JceEntity e, void *ud)
{
    UCCanvasList *cl = (UCCanvasList *)ud;
    if (cl->count >= 64) return;
    if (jce_scene_has_canvas(s, e))
        cl->list[cl->count++] = e;
}

/* Sort canvases by Canvas.sort_order ascending (lowest drawn first). */
static void uc_sort_canvases(JceScene *s, UCCanvasList *cl)
{
    for (int i = 1; i < cl->count; i++) {
        JceEntity key = cl->list[i];
        JceCanvasComponent *ck = jce_scene_get_canvas(s, key);
        int kso = ck ? ck->sort_order : 0;
        int j = i - 1;
        while (j >= 0) {
            JceCanvasComponent *cj = jce_scene_get_canvas(s, cl->list[j]);
            if ((cj ? cj->sort_order : 0) <= kso) break;
            cl->list[j + 1] = cl->list[j];
            j--;
        }
        cl->list[j + 1] = key;
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

JceUICanvas *jce_ui_canvas_create(JceRenderer *renderer, const JcePakArchive *pak)
{
    if (!renderer) return NULL;
    JceUICanvas *uc = (JceUICanvas *)JCE_CALLOC(1, sizeof(*uc));
    if (!uc) return NULL;
    uc->renderer = renderer;
    uc->pak      = pak;
    return uc;
}

void jce_ui_canvas_destroy(JceUICanvas *uc)
{
    if (!uc) return;
    for (int i = 0; i < uc->font_count; i++)
        if (uc->fonts[i].font) jce_font_close(uc->fonts[i].font);
    for (int i = 0; i < uc->texture_count; i++)
        if (jce_texture_valid(uc->textures[i].tex))
            jce_texture_destroy(uc->textures[i].tex);
    JCE_FREE(uc);
}

void jce_ui_canvas_render(JceUICanvas *uc, JceScene *scene, uint16_t view_id,
                          uint16_t fb_idx, float screen_w, float screen_h,
                          const JceUIPointer *pointer, float dt_sec)
{
    if (!uc || !scene || screen_w <= 0 || screen_h <= 0) return;
    JCE_PROFILE_ZONE_N("UICanvas::Render");

    /* Configure the UI view: bind framebuffer (panel FBO or backbuffer),
     * set the rect, and a top-left-origin orthographic projection in logical
     * pixels — matching JCE_VIEW_UI so UI coords are screen pixels.  The view
     * does NOT clear (it overlays the rendered scene). */
    {
        const bgfx_caps_t *caps = bgfx_get_caps();
        bgfx_frame_buffer_handle_t fb = { fb_idx };
        bgfx_set_view_frame_buffer(view_id, fb);
        bgfx_set_view_rect(view_id, 0, 0, (uint16_t)screen_w, (uint16_t)screen_h);
        bgfx_set_view_clear(view_id, BGFX_CLEAR_NONE, 0, 1.0f, 0);
        jce_mat4 view = jce_m4_identity();
        jce_mat4 proj = jce_m4_ortho(0, screen_w, screen_h, 0,
                                     0, 100.0f, caps->homogeneousDepth);
        bgfx_set_view_transform(view_id, view.raw[0], proj.raw[0]);
        bgfx_set_view_mode(view_id, BGFX_VIEW_MODE_SEQUENTIAL);
        bgfx_touch(view_id);
    }

    UCCanvasList canvases; canvases.count = 0;
    jce_scene_each_entity(scene, uc_collect_canvas_cb, &canvases);
    if (canvases.count == 0) { JCE_PROFILE_ZONE_END; return; }
    uc_sort_canvases(scene, &canvases);

    UCFrame fr;
    fr.uc = uc; fr.scene = scene; fr.view_id = view_id;
    fr.hit_count = 0;
    uc->last_clicked = 0;

    for (int i = 0; i < canvases.count; i++) {
        JceEntity canvas = canvases.list[i];
        JceCanvasComponent *cv = jce_scene_get_canvas(scene, canvas);
        if (!cv) continue;
        /* Screen-Space Overlay is the supported render mode.  World/Camera
         * canvases are laid out as overlays for now (documented partial).
         * CanvasScaler ("scale with screen size"): RectTransform anchors give
         * positional resolution-independence; the scale factor below makes the
         * px-sized parts (size_delta / font / borders) scale uniformly too. */
        fr.ui_scale      = 1.0f;
        fr.pixel_perfect = cv->pixel_perfect;
        if (cv->render_mode == JCE_CANVAS_OVERLAY &&
            cv->reference_resolution[0] > 0.0f && cv->reference_resolution[1] > 0.0f) {
            float rsx = screen_w / cv->reference_resolution[0];
            float rsy = screen_h / cv->reference_resolution[1];
            if (rsx > 0.0f && rsy > 0.0f)
                fr.ui_scale = sqrtf(rsx * rsy);   /* match=0.5 (geometric mean) */
        }
        UCRect root = { 0, 0, screen_w, screen_h };
        uc_layout_draw(&fr, canvas, &root, 1.0f, /*blocks*/true, /*interactable*/true, 0);
    }

    uc_update_buttons(&fr, pointer, dt_sec > 0 ? dt_sec : 0.0f);

    JCE_PROFILE_ZONE_END;
}

uint64_t jce_ui_canvas_last_clicked(const JceUICanvas *uc)
{
    return uc ? uc->last_clicked : 0;
}
