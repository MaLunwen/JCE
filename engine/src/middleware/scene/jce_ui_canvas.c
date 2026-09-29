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
#include <jce/middleware/scene/jce_component_registry.h>  /* per-component disable gate */
#include <jce/middleware/scene/jce_ui_canvas.h>
#include "jce_ui_canvas_resources.h"
#include "jce_ui_canvas_widgets.h"
#include "jce_ui_canvas_fitter.h"
#include "jce_ui_canvas_layout.h"
#include <jce/middleware/ui/jce_localization.h>
#include <jce/os/core/jce_filesystem.h>   /* active-VFS font fallback (bundle Play) */
#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/platform/jce_keys.h>
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

/* How many clip changes one render records.  A bound and not a growth: the
 * log is a diagnostic, and a canvas deep enough to overflow it has a bigger
 * problem than an incomplete log. */
#define UC_MAX_CLIP_LOG 64
#define UC_MAX_DEPTH    32   /* RectTransform nesting depth guard */
#define UC_DEFAULT_FONT "fonts/JCE.ttf"


/* One entry in the draw-side clip log.  `active` distinguishes "clipped to
 * this rect" from "no clip", which a rect alone cannot: (0,0,0,0) is a real
 * and different answer from "unclipped", and those are the two states a
 * clipping bug lands in. */
typedef struct { UCRect rect; bool active; } UCClip;

/* One raycast-eligible node recorded during the layout walk.  Defined up here
 * because JceUICanvas keeps the last frame's list for the editor queries. */
typedef struct {
    JceEntity entity;
    UCRect    rect;         /* ALREADY intersected with `clip` -- see below */
    float     alpha;        /* inherited canvas-group alpha */
    bool      interactable; /* inherited canvas-group interactable (AND chain) */
} UCHit;


/* ── The active scissor ─────────────────────────────────────────────────
 *
 * The STATE now lives in jce_primitives.c, which owns the submits, and this
 * file only says what to clip to.  It used to keep the state here and arm it
 * through two macros around the two rect helpers -- correct for quads and
 * silently wrong for TEXT, because jce_text_draw_scaled_view submits from
 * inside jce_text.c (one draw per GLYPH until runs batched, 2026-09-01),
 * where a macro in this file cannot reach.  Scrolled text ran outside its
 * viewport; no care at these call sites could fix it, so the duty moved down.
 *
 * (The layer below that, bgfx_set_view_scissor, is a single per-VIEW value
 * bgfx copies once per frame -- this file once set it before a ScrollView's
 * subtree and reset it after in the same frame, so the RESET is what reached
 * the renderer and nothing was ever clipped.  That is why the per-DRAW
 * primitive is the one being used.) */
static void uc_clip_set(const UCRect *r)
{
    /* NOT gated on uc->renderer.  It once was, because the body called bgfx
     * directly; jce_draw_set_scissor is a plain state setter, so gating it
     * only made the canvas's clip DECISION unobservable to a headless test
     * while changing nothing about what a headless render draws (nothing). */
    if (r) jce_draw_set_scissor((int)r->x, (int)r->y, (int)r->w, (int)r->h);
    else   jce_draw_clear_scissor();
}

/* Resolve an element's RectTransform rotation/scale into the ABSOLUTE-pivot
 * form the draw layer takes, or return NULL when the element does not turn.
 *
 * NULL matters: it is what routes the draw back through the untransformed,
 * BATCHED path.  A label is hundreds of glyph quads, and "identity applied
 * to each" is a different code path from "not applied", not a no-op.
 *
 * The pivot is the element's own RectTransform pivot resolved against the
 * rect this element actually got, so the label and the panel turn about the
 * same point -- which is the whole reason JceRectXform's pivot is absolute. */
static const JceRectXform *uc_elem_xform(const JceRectTransform *rt,
                                         const UCRect *r, JceRectXform *out)
{
    if (!rt || !r || !out) return NULL;
    float sx = rt->scale[0], sy = rt->scale[1];
    if (sx == 0.0f) sx = 1.0f;          /* unset in every older scene */
    if (sy == 0.0f) sy = 1.0f;
    if (rt->rotation_deg == 0.0f && sx == 1.0f && sy == 1.0f) return NULL;

    /* pivot (0,0) is the RectTransform default for a legacy zero struct, and
     * uc_resolve_rect already treats that as a centred full-stretch; centre
     * is also what a pop/spin means, so an unset pivot spins about the middle
     * rather than about the top-left corner. */
    float pvx = rt->pivot[0], pvy = rt->pivot[1];
    if (pvx == 0.0f && pvy == 0.0f) { pvx = 0.5f; pvy = 0.5f; }
    out->pivot[0]  = r->x + r->w * pvx;
    /* Draw space is y-DOWN while the pivot is authored y-UP, the same flip
     * uc_resolve_rect makes for the rect itself. */
    out->pivot[1]  = r->y + r->h * (1.0f - pvy);
    out->angle_deg = rt->rotation_deg;
    out->scale[0]  = sx;
    out->scale[1]  = sy;
    return out;
}

/* The two renderer entry points every canvas quad goes through.  Kept as
 * macros after the state moved, because they are the single place a future
 * reader looks for "how does a canvas quad get drawn" -- they simply no
 * longer arm anything, since the renderer does. */
#define uc_rect_view(r, v, x, y, w, h, c)     jce_draw_filled_rect_view((r), (v), (x), (y), (w), (h), (c))
#define uc_tex_view(r, v, x, y, w, h, t, c, uv)     jce_draw_textured_rect_view((r), (v), (x), (y), (w), (h), (t), (c), (uv))

/* Cached font, keyed by (path, integer px size). */
typedef struct {
    char     path[256];
    int      px;
    bool     sdf;      /* part of the KEY: a different atlas, not a flag */
    JceFont *font;
} UCFontSlot;

/* Cached sprite texture, keyed by path.  tex.idx == UINT16_MAX ⇒ load failed
 * (cached negative so we don't re-try every frame). */
typedef struct {
    char       path[256];
    JceTexture tex;
} UCTexSlot;

/* Per-widget interaction state, persisted across frames.  Shared by the
 * UIButton click state machine and the UISlider/UIToggle machines:
 *   - button : fade/state/press_inside (Unity ColorTint transition)
 *   - slider : drag_active (a drag that began over the handle keeps tracking
 *              even if the pointer slips slightly off the rect, mirroring the
 *              button press_inside latch)
 *   - toggle : press_inside (same press-inside → release-over click latch as
 *              the button) */
typedef struct {
    uint64_t entity;       /* 0 = free slot */
    float    fade;         /* 0..1 toward target tint (button) */
    int      state;        /* 0 normal, 1 hover, 2 pressed (button) */
    bool     press_inside; /* press began over this widget (button/toggle) */
    bool     drag_active;  /* slider drag in flight */
    /* Dropdown press latch: the target the press began over —
     *   -1 = none, -2 = the collapsed main rect, >=0 = an expanded option row.
     * A click commits only when the release lands over the SAME target
     * (press-inside → release-over, mirroring the button/toggle latch). */
    int      dd_press;
} UCButtonState;

struct JceUICanvas {
    JceRenderer         *renderer;
    const JcePakArchive *pak;

    /* The display's safe area in drawable PIXELS, pushed in by the host (see
     * jce_ui_canvas_set_safe_area).  Zero width or height means "never told",
     * which is the same as "the whole drawable" -- the root rect below only
     * consults these when both are positive AND the canvas opted in. */
    int           safe_x, safe_y, safe_w, safe_h;

    UCFontSlot    fonts[UC_MAX_FONTS];
    int           font_count;
    uint32_t      font_root_gen;   /* asset-root generation the cache was
                                    * built against (0 = never validated) */

    UCTexSlot     textures[UC_MAX_TEXTURES];
    int           texture_count;
    uint32_t      tex_root_gen;    /* same generation gate as font_root_gen:
                                    * a project switch must not keep serving
                                    * the previous project's sprite */

    UCButtonState buttons[UC_MAX_BUTTONS];

    uint64_t      last_clicked;
    uint64_t      last_value_changed;  /* slider/toggle/dropdown value changed this render */
    uint64_t      last_text_changed;   /* inputfield text edited (clear-on-read)           */

    /* InputField focus + edit state (single focused field at a time). */
    uint64_t      focused_input;   /* focused InputField entity, 0 = none  */
    int           caret;           /* byte caret index into its `text`     */
    uint64_t      last_submitted;  /* set on RETURN, cleared each render    */
    float         caret_blink;     /* 0..1 blink phase (driven by dt)       */
    JceScene     *edit_scene;      /* scene last rendered (text/key target) */

    /* ScrollView wheel channel: the scroll view under the pointer during the
     * most recent render (raycast pass), plus its resolved viewport rect so
     * jce_ui_canvas_scroll can clamp without re-laying-out.  0 = none. */
    /* ScrollView DRAG: thumb drag, track jump, and content drag.  The
     * scrollbars used to be drawn and never read -- a control that looks
     * grabbable and is not -- and the wheel was the only way to move a list at
     * all, so a trackpad or touch user got nothing. */
    uint64_t      scroll_drag;         /* entity being dragged, 0 = none        */
    int           scroll_drag_mode;    /* 1 = v-thumb, 2 = h-thumb, 3 = content */
    float         scroll_drag_grab[2]; /* pointer at the grab, device px        */
    float         scroll_drag_base[2]; /* scroll_position at the grab, ref units*/
    UCRect        scroll_drag_rect;    /* viewport rect at the grab, device px  */
    float         scroll_drag_scale;   /* ui_scale at the grab                  */
    bool          scroll_drag_moved;   /* passed the threshold this gesture     */
    uint64_t      scroll_press;        /* content-drag candidate under a press  */
    float         scroll_press_xy[2];  /* where that press began, device px     */

    /* The last frame's hit list, kept so the editor can ask what is where
     * without re-running a layout -- and so it asks the SAME question the
     * pointer does, by the same rules.  UCFrame itself is a stack local. */
    UCHit         last_hits[UC_MAX_BUTTONS];
    int           last_hit_count;

    /* THE DRAW-SIDE CLIP LOG, beside last_hits for the same reason last_hits
     * exists: a record of what the last render actually decided, for anyone
     * who has to answer "why was this clipped like that".
     *
     * It exists because the draw clip is otherwise UNOBSERVABLE.  last_hits
     * records the RAYCAST clip, and the two used to disagree -- the scissor
     * was set to a ScrollView's own rect while the raycast used the
     * intersection with its parent, so input and pixels clipped a nested
     * viewport differently.  A test could see one and not the other, which is
     * how that survived.  Bounded and overwritten each render; a deeper tree
     * simply stops recording rather than growing. */
    UCClip        clip_log[UC_MAX_CLIP_LOG];
    int           clip_log_count;
    UCRect        last_modal_rect;
    JceEntity     last_modal_owner;
    bool          last_modal_active;
    float         last_screen_w, last_screen_h;

    uint64_t      hovered_scroll;      /* hovered UIScrollView entity, 0 = none */
    UCRect        hovered_scroll_rect; /* its resolved viewport rect (device px) */
    float         hovered_scroll_scale;/* the ui_scale that resolved that rect   */
};

/* ── Resource caches ───────────────────────────────────────────────── */

/* Process-global content root for canvas assets (mirrors the particle
 * system's asset-root anchor).  When set — the editor points it at the open
 * project's source-assets dir — fonts referenced by UIText resolve from the
 * project tree FIRST, then fall back to the canvas pak.  Runtime PAK-only
 * boots (shipped exe, web) never set it and keep the pak path.  A generation
 * counter invalidates per-canvas font caches when the root changes (project
 * switch), so a stale font never outlives its project. */

/* App-scoped override for the fallback font used by any UIText that leaves its
 * font_path empty.  Empty (the default) keeps UC_DEFAULT_FONT so the editor and
 * every other app are unchanged.  Bumps the same generation counter as the
 * asset root so cached fonts rebuild when it changes. */




JceRenderer *uc_renderer(const JceUICanvas *uc) { return uc ? uc->renderer : NULL; }
int          uc_caret(const JceUICanvas *uc)    { return uc ? uc->caret : 0; }

int uc_font_px_clamp(int px)
{
    if (px < 4)   px = 4;
    /* 256 is the ceiling on ATLAS size, not on label size.  A CJK set at
     * 400px would be an enormous texture; a 400px label is ordinary. */
    if (px > 256) px = 256;
    return px;
}



JceFont *uc_get_font(JceUICanvas *uc, const char *path, int px, bool sdf)
{
    px = uc_font_px_clamp(px);
    /* An EMPTY font_path means, in order: the app's override, then this
     * machine's UI font, then UC_DEFAULT_FONT.  The middle step is the
     * point: UC_DEFAULT_FONT is fonts/JCE.ttf, a hand-drawn glyph set that
     * ships with the sample content -- artwork, not a UI face -- and it used
     * to be first, so "I did not choose a font" rendered as a decorative
     * one.  It was also the only worked example, so every scene copied from
     * a sample named it explicitly too; a default that is also the only
     * example becomes the answer everybody gives.  It remains the last
     * resort on a machine with no readable system font. */
    const char *dflt = uc_resource_default_font();
    char sysfont[1024];
    const char *fallback;
    static bool s_said = false;
    if (dflt[0])
        fallback = dflt;
    else if (uc_resource_system_ui_font(sysfont, sizeof sysfont))
        fallback = sysfont;
    else
        fallback = UC_DEFAULT_FONT;
    /* Say which font an unspecified UIText actually got, once.  Silence is
     * not an answer here: "found the system font" and "fell back to the
     * bundled one" produce identical logs otherwise, and the difference is
     * the entire point of the lookup. */
    if (!s_said) {
        s_said = true;
        LOG_INFO(LOG_TAG, "default UI font: %s", fallback);
    }
    const char *p = (path && path[0]) ? path : fallback;

    if (uc->font_root_gen != uc_resource_generation()) {
        for (int i = 0; i < uc->font_count; i++)
            if (uc->fonts[i].font) jce_font_close(uc->fonts[i].font);
        uc->font_count = 0;
        uc->font_root_gen = uc_resource_generation();
    }

    for (int i = 0; i < uc->font_count; i++) {
        if (uc->fonts[i].px == px && uc->fonts[i].sdf == sdf &&
            strcmp(uc->fonts[i].path, p) == 0)
            return uc->fonts[i].font;   /* may be NULL = cached failure */
    }
    if (uc->font_count >= UC_MAX_FONTS)
        return NULL;

    JceFont *f = uc_open_font_path(uc->pak, p, px, sdf);
    if (!f && strcmp(p, UC_DEFAULT_FONT) != 0 && uc->pak) {
        /* Fall back to the default engine font. */
        uint32_t cps[256];
        int n = jce_i18n_collect_codepoints(cps, 256);
        f = jce_font_open_ex(uc->pak, UC_DEFAULT_FONT, (float)px, cps, n);
    }
    UCFontSlot *slot = &uc->fonts[uc->font_count++];
    jce_strlcpy(slot->path, p, sizeof slot->path);
    slot->px   = px;
    slot->sdf  = sdf;
    slot->font = f;
    if (!f) LOG_WARN(LOG_TAG, "font load failed: %s @%dpx", p, px);

    /* AFTER the slot is recorded, and that ordering is load-bearing: attaching
     * opens each fallback through uc_get_font, which re-enters here.  With the
     * slot already in the table a fallback that names the primary's own path
     * finds it and returns instead of opening a second copy -- and a list that
     * names the font itself terminates rather than recursing. */
    uc_attach_fallbacks(uc, f, px, sdf);
    return f;
}

/* Resolve a UI sprite through the SAME three-tier ladder uc_get_font uses:
 * project asset root on the host FS, then the active VFS (bundle boots), then
 * the canvas pak.
 *
 * It used to be pak-only, and the pak a canvas is created with is fixed at
 * creation.  In the editor that pak is editor_assets.pak, so a project-authored
 * UIImage.spritePath was looked up in the EDITOR's own assets, missed, and fell
 * through to a flat coloured quad -- while the same component in the shipped
 * exe (whose pak is the game's) found the texture and drew the sprite.  Worse
 * than a miss: a project path that happened to collide with an editor asset
 * resolved to the editor's picture instead.  Neither case logged anything.
 *
 * The cache is keyed on the asset-root generation for the same reason the font
 * cache is: without it, switching projects keeps serving the previous
 * project's sprite. */
static JceTexture uc_get_texture(JceUICanvas *uc, const char *path)
{
    if (!path || !path[0]) return JCE_TEXTURE_INVALID;

    if (uc->tex_root_gen != uc_resource_generation()) {
        for (int i = 0; i < uc->texture_count; i++)
            if (jce_texture_valid(uc->textures[i].tex))
                jce_texture_destroy(uc->textures[i].tex);
        uc->texture_count = 0;
        uc->tex_root_gen  = uc_resource_generation();
    }

    for (int i = 0; i < uc->texture_count; i++) {
        if (strcmp(uc->textures[i].path, path) == 0)
            return uc->textures[i].tex;   /* may be INVALID = cached failure */
    }
    if (uc->texture_count >= UC_MAX_TEXTURES)
        return JCE_TEXTURE_INVALID;

    JceTexture t = JCE_TEXTURE_INVALID;

    /* 1. Project source-assets root -- the editor points this at the open
     *    project, so authored sprites resolve without being baked into the
     *    editor's own pak. */
    const char *root = uc_resource_asset_root();
    if (root[0]) {
        char full[768];
        snprintf(full, sizeof full, "%s/%s", root, path);
        uint64_t sz = 0;
        void *buf = jce_fs_host_read_all(full, &sz);
        if (buf) {
            if (sz > 0)
                t = jce_texture_upload_cpu(
                        jce_texture_decode_cpu_mem(buf, (size_t)sz,
                                                   JCE_TEX_CLAMP));
            jce_fs_buffer_free(buf);
        }
    }

    /* 2. Active VFS: in bundle Play (isolated preview) and shipped bundle
     *    boots the sprite lives ONLY in the mounted bundle. */
    if (!jce_texture_valid(t)) {
        JceFileSystem *afs = jce_fs_get_active();
        if (afs) {
            uint64_t vsz = 0;
            void *vbuf = jce_fs_read_all(afs, path, &vsz);
            if (vbuf) {
                if (vsz > 0)
                    t = jce_texture_upload_cpu(
                            jce_texture_decode_cpu_mem(vbuf, (size_t)vsz,
                                                       JCE_TEX_CLAMP));
                jce_fs_buffer_free(vbuf);
            }
        }
    }

    /* 3. The canvas pak -- in a shipped exe / web boot the only tier there is. */
    if (!jce_texture_valid(t) && uc->pak)
        t = jce_texture_load(uc->pak, path);

    UCTexSlot *slot = &uc->textures[uc->texture_count++];
    jce_strlcpy(slot->path, path, sizeof slot->path);
    slot->tex = t;
    /* Warn like the font path does: uc_draw_image's silent flat-quad fallback
     * is exactly what made a missing sprite invisible as a defect. */
    if (!jce_texture_valid(t))
        LOG_WARN(LOG_TAG, "UI sprite load failed: %s", path);
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
        free_slot->entity   = e;
        free_slot->fade     = 1.0f;
        free_slot->dd_press = -1;  /* no dropdown press latched */
        return free_slot;
    }
    return NULL;
}

/* ── Colour helpers ────────────────────────────────────────────────── */

uint32_t uc_color(const float rgba[4], float alpha_mul)
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
/* Resolve one RectTransform against its parent's already-resolved rect.
 *
 * TWO SPACES MEET HERE, and conflating them is what this function got wrong.
 *
 *   INPUT  is Unity UGUI: anchors are fractions of the parent measured from its
 *          BOTTOM-left corner, anchoredPosition is in pixels with +Y UP, and
 *          pivot names the point of the element's own box that
 *          anchoredPosition addresses (pivot.y = 1 is the element's TOP edge).
 *   OUTPUT is this renderer's draw space: pixels, origin at the TOP-left,
 *          +Y DOWN -- the JCE_VIEW_UI ortho convention.
 *
 * They differ by a Y flip, and the flip was missing: anchor.y and
 * anchoredPosition.y were fed straight into the top-left space, so anchorMin/
 * Max.y = 1 -- Unity's TOP edge -- resolved to the BOTTOM of the screen and a
 * negative anchoredY moved the element further up instead of down.  Every one
 * of the 844 UI rects authored in this tree uses one of the two Unity idioms
 * (anchor 1 / pivot 1 / negative anchoredY for a top bar, anchor 0 / pivot 0 /
 * positive anchoredY for a bottom corner), so every one of them landed on the
 * opposite edge -- in the editor AND in the shipped runtime.  Nothing was
 * authored in the old top-left reading, which is why this is a fix and not a
 * migration.
 *
 * The size_delta sign in the STRETCH branch was inverted for the same reason.
 * Unity's relation is  size = anchorSpan*parentSize + sizeDelta  and
 * rect.min = anchorMin*parentSize + anchoredPosition - sizeDelta*pivot, so a
 * negative sizeDelta insets a stretched element.  The old code subtracted
 * sizeDelta from the span and offset by +sizeDelta*0.5 regardless of pivot,
 * which grew the element where Unity shrinks it.  No content in this tree
 * stretches (all 844 rects have anchorMin == anchorMax on both axes), so this
 * half is parity work, not a repair.
 *
 * X needs no flip: both spaces run left-to-right from the same edge. */
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

    /* Anchor reference points.  X is absolute in the output space; Y is a
     * distance UP from the parent's bottom edge, i.e. still UGUI space -- it is
     * flipped once, at the end, after the whole Y solve. */
    float anc_min_x = parent->x + parent->w * a_min_x;
    float anc_max_x = parent->x + parent->w * a_max_x;
    float anc_min_y = parent->h * a_min_y;
    float anc_max_y = parent->h * a_max_y;

    UCRect r;
    /* X axis. */
    if (a_min_x == a_max_x) {
        /* Fixed width: size_delta.x is the width; anchored from pivot. */
        r.w = sd_w;
        r.x = anc_min_x + ap_x - piv_x * r.w;
    } else {
        /* Stretch: size = anchor span + size_delta, positioned from the pivot. */
        r.w = (anc_max_x - anc_min_x) + sd_w;
        r.x = anc_min_x + ap_x - piv_x * sd_w;
    }
    /* Y axis, solved in UGUI space: y_up is the element's BOTTOM edge measured
     * up from the parent's bottom. */
    float y_up;
    if (a_min_y == a_max_y) {
        r.h  = sd_h;
        y_up = anc_min_y + ap_y - piv_y * r.h;
    } else {
        r.h  = (anc_max_y - anc_min_y) + sd_h;
        y_up = anc_min_y + ap_y - piv_y * sd_h;
    }
    /* The single flip into top-left-origin, +Y-down draw space. */
    r.y = parent->y + parent->h - y_up - r.h;
    /* pixel_perfect: snap to whole pixels to avoid sub-pixel blur. */
    if (pixel_perfect) {
        r.x = floorf(r.x + 0.5f); r.y = floorf(r.y + 0.5f);
        r.w = floorf(r.w + 0.5f); r.h = floorf(r.h + 0.5f);
    }
    return r;
}

/* uc_entity_rect / uc_entity_rect_mut MOVED to jce_ui_rect_lookup.c.
 *
 * Not for size: the scene sequencer asks the same question now, and a static
 * library links per object, so calling it from here would have pulled this
 * whole module -- uc_draw_text and its jce_loc_t reference included -- into
 * every target that links jce_scene.  See that file's header. */

/* Which children uc_layout_draw walks INTO.
 *
 * This is not "does the entity draw something": an entity that fails this test
 * is dropped from the child list, so it is not laid out, not raycast, and NOT
 * RECURSED INTO -- its entire subtree of real UI disappears with it.
 *
 * It used to list only the eight graphic/widget components, which silently
 * deleted the two canonical container idioms: an empty node carrying a
 * CanvasGroup (Unity's fade/gate container) and an empty node carrying a
 * LayoutGroup (Unity's "empty GameObject with a Vertical Layout Group holding
 * a column of buttons").  Both are offered unrestricted by Add Component and
 * drawn in full by the Inspector, so they were authorable and inert -- and
 * worse than inert, because everything under them vanished too.
 *
 * UIButton was missing for a different reason: it is the only widget with no
 * RectTransform of its own, so a button-only entity had no rect either.  It
 * has one now (appended to JceUIButtonComponent, resolved LAST in
 * uc_entity_rect so a sibling UIImage still wins for every scene authored
 * before it existed).
 *
 * A container with no graphic resolves a NULL rect, and uc_resolve_rect
 * substitutes a zeroed one, which is full-stretch of the parent -- exactly the
 * semantic a container wants. */
bool uc_is_ui_element(JceScene *s, JceEntity e)
{
    return jce_scene_has_ui_image(s, e) || jce_scene_has_ui_text(s, e) ||
           jce_scene_has_ui_slider(s, e) || jce_scene_has_ui_toggle(s, e) ||
           jce_scene_has_ui_input_field(s, e) ||
           jce_scene_has_ui_scroll_view(s, e) ||
           jce_scene_has_ui_progress_bar(s, e) ||
           jce_scene_has_ui_dropdown(s, e) ||
           jce_scene_has_ui_button(s, e) ||
           jce_scene_has_canvas_group(s, e) ||
           jce_scene_has_layout_group(s, e);
}

/* ── Raycast accumulation ──────────────────────────────────────────── */

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
    /* Pointer snapshot for the render pass (dropdown popup hover highlight).
     * Interaction proper still runs in uc_update_*; this is draw-only. */
    float        ptr_x, ptr_y;
    bool         ptr_valid;
    /* MODAL rect: an expanded dropdown's popup.  Its rows are drawn and
     * click-handled by uc_update_widgets, but they were never recorded as
     * hits, and uc_update_buttons runs FIRST against the unmodified hit list --
     * so choosing an option also pressed whatever sat under the popup.  Any
     * hit whose centre falls inside this rect and does not belong to
     * `modal_owner` is rejected. */
    UCRect       modal_rect;
    JceEntity    modal_owner;
    bool         modal_active;
    /* Screen (framebuffer) height in device px.  The dropdown popup needs it
     * to decide whether the option list fits below the control. */
    float        screen_h;
} UCFrame;

/* ── 9-slice / image draw ──────────────────────────────────────────── */

static void uc_draw_image(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                          const JceUIImageComponent *im, float alpha_mul,
                          float ui_scale)
{
    uint32_t tint = uc_color(im->color, alpha_mul);
    JceTexture tex = uc_get_texture(uc, im->sprite_path);
    /* NULL unless this element actually turns -- see uc_elem_xform. */
    JceRectXform xf_store;
    const JceRectXform *xf = uc_elem_xform(&im->rect, r, &xf_store);

    if (!jce_texture_valid(tex)) {
        /* No sprite: flat coloured quad (still a valid UI panel). */
        jce_draw_filled_rect_view_xf(uc->renderer, view_id,
                                     r->x, r->y, r->w, r->h, tint, xf);
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
            jce_draw_textured_rect_view_xf(uc->renderer, view_id, dx, dy, dw, dh,
                                           tex, tint, uv, xf);
            return;
        }

        if (im->image_type == JCE_UI_IMAGE_FILLED) {
            /* Horizontal left-to-right fill: show the leftmost fill_amount of
             * both the rect and the sprite (cropped, not stretched). */
            float amt = im->fill_amount < 0.0f ? 0.0f : (im->fill_amount > 1.0f ? 1.0f : im->fill_amount);
            if (amt <= 0.0f) return;
            float uv[4] = { 0.0f, 0.0f, amt, 1.0f };
            jce_draw_textured_rect_view_xf(uc->renderer, view_id, dx, dy, dw * amt, dh,
                                           tex, tint, uv, xf);
            return;
        }

        /* SIMPLE (and any other type): single quad, full sprite. */
        jce_draw_textured_rect_view_xf(uc->renderer, view_id, dx, dy, dw, dh,
                                       tex, tint, NULL, xf);
        return;
    }

    /* 9-slice: split into a 3x3 grid.  Borders are in source pixels. */
    uint32_t tw = 0, th = 0;
    jce_texture_get_size(tex, &tw, &th);
    if (tw == 0 || th == 0) {
        jce_draw_textured_rect_view_xf(uc->renderer, view_id, r->x, r->y,
                                       r->w, r->h, tex, tint, NULL, xf);
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
            jce_draw_textured_rect_view_xf(uc->renderer, view_id, qx, qy, qw, qh,
                                           tex, tint, uv, xf);
        }
    }
}

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
                              const JceRectXform *xf)
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
                jce_text_draw_scaled_view_xf(uc->renderer, font, view_id,
                                             cx, y, 1.0f, run,
                                             uc_color(cur, alpha_mul), xf);
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
        jce_text_draw_scaled_view_xf(uc->renderer, font, view_id, cx, y, 1.0f,
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

static void uc_draw_text(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                         const JceUITextComponent *tx, float alpha_mul,
                         float ui_scale)
{
    /* The label turns with its element, about the ELEMENT's pivot -- see
     * uc_elem_xform.  NULL unless it actually turns, which keeps every
     * ordinary line of text on the batched path it has always used. */
    JceRectXform txf_store;
    const JceRectXform *txf = uc_elem_xform(tx ? &tx->rect : NULL, r,
                                            &txf_store);
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
            JceFont *f = uc_get_font(uc, tx->font_path, mid, tx->sdf);
            if (!f) { hi = mid - 1; continue; }   /* treat as "doesn't fit" */
            float bw = 0.0f, bh = 0.0f;
            uc_text_block_extent(f, vis, nlines, lsp, &bw, &bh);
            /* The search asks "does mid fit", and above the ceiling the font
             * it measured is SMALLER than mid -- so without this the search
             * says yes to sizes that do not fit and best_fit overflows its
             * rect exactly where the text is biggest. */
            const float ms = (float)mid / (float)uc_font_px_clamp(mid);
            if (ms > 1.0f) { bw *= ms; bh *= ms; }
            if (bw <= r->w && bh <= r->h) { best = mid; lo = mid + 1; }
            else                          { hi = mid - 1; }
        }
        px = best;
        dscale = (float)px / (float)uc_font_px_clamp(px);
        if (dscale < 1.0f) dscale = 1.0f;
    }

    JceFont *font = uc_get_font(uc, tx->font_path, px, tx->sdf);
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

    const float lh = (float)jce_font_line_height(font) * lsp * dscale;
    float block_h = lh * (float)nlines;
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
            jce_text_draw_math_view(uc->renderer, font, view_id, ox, oy, 1.0f, vis[i], col);
        else if (tx->rich_text)
            uc_draw_rich_line(uc, font, view_id, ox, oy, lines[i],
                              tx->color, alpha_mul, txf);
        else
            jce_text_draw_styled_view_xf(uc->renderer, font, view_id, ox, oy,
                                         dscale, vis[i], col, txf, stp);
    }
}

/* ── Slider / Toggle value mapping ──────────────────────────────────── */

/* Normalized value position (0..1) of the slider along its main axis. */
static float uc_slider_norm(const JceUISliderComponent *sl)
{
    float span = sl->max_value - sl->min_value;
    if (span <= 0.0f) return 0.0f;
    float t = (sl->value - sl->min_value) / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/* Map a pointer position to a normalized t (0..1) along the slider's main
 * axis, honouring `direction`.  Shared by the headless interaction path. */
static float uc_slider_pointer_t(const JceUISliderComponent *sl, const UCRect *r,
                                 float px, float py)
{
    float t;
    switch (sl->direction) {
        case 1: /* R→L */
            t = (r->w > 0.0f) ? (px - r->x) / r->w : 0.0f;
            t = 1.0f - t;
            break;
        case 2: /* B→T (screen y grows downward, so bottom = larger y) */
            t = (r->h > 0.0f) ? (py - r->y) / r->h : 0.0f;
            t = 1.0f - t;
            break;
        case 3: /* T→B */
            t = (r->h > 0.0f) ? (py - r->y) / r->h : 0.0f;
            break;
        case 0: /* L→R */
        default:
            t = (r->w > 0.0f) ? (px - r->x) / r->w : 0.0f;
            break;
    }
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/* ── Slider / Toggle draw (PASS 3) ──────────────────────────────────── */

void uc_draw_quad(JceUICanvas *uc, uint16_t view_id, float x, float y,
                         float w, float h, const char *sprite,
                         const float rgba[4], float alpha_mul)
{
    if (w <= 0.0f || h <= 0.0f) return;
    uint32_t tint = uc_color(rgba, alpha_mul);
    JceTexture tex = uc_get_texture(uc, sprite);
    if (jce_texture_valid(tex))
        uc_tex_view(uc->renderer, view_id, x, y, w, h, tex, tint, NULL);
    else
        uc_rect_view(uc->renderer, view_id, x, y, w, h, tint);
}

static void uc_draw_slider(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                           const JceUISliderComponent *sl, float alpha_mul,
                           float ui_scale)
{
    float t = uc_slider_norm(sl);

    /* Background track. */
    uc_draw_quad(uc, view_id, r->x, r->y, r->w, r->h, NULL, sl->bg_color, alpha_mul);

    /* Fill: a sub-rect from the "start" edge to the value position. */
    float fx = r->x, fy = r->y, fw = r->w, fh = r->h;
    switch (sl->direction) {
        case 1: /* R→L: fill the right portion */
            fw = r->w * t; fx = r->x + r->w - fw; break;
        case 2: /* B→T: fill the bottom portion */
            fh = r->h * t; fy = r->y + r->h - fh; break;
        case 3: /* T→B: fill the top portion */
            fh = r->h * t; break;
        case 0: /* L→R: fill the left portion */
        default:
            fw = r->w * t; break;
    }
    uc_draw_quad(uc, view_id, fx, fy, fw, fh, sl->fill_sprite, sl->fill_color, alpha_mul);

    /* Handle: a square centred at the value position along the main axis. */
    float hs = (sl->handle_size > 0.0f ? sl->handle_size : 20.0f) * ui_scale;
    float hx, hy, hw, hh;
    bool vertical = (sl->direction == 2 || sl->direction == 3);
    if (vertical) {
        hw = r->w; hh = hs;
        hx = r->x;
        /* t runs along the value axis; convert to a screen-y centre. */
        float cy = (sl->direction == 3) ? (r->y + r->h * t)
                                        : (r->y + r->h * (1.0f - t));
        hy = cy - hh * 0.5f;
    } else {
        hw = hs; hh = r->h;
        hy = r->y;
        float cx = (sl->direction == 1) ? (r->x + r->w * (1.0f - t))
                                        : (r->x + r->w * t);
        hx = cx - hw * 0.5f;
    }
    uc_draw_quad(uc, view_id, hx, hy, hw, hh, sl->handle_sprite, sl->handle_color, alpha_mul);
}

static void uc_draw_toggle(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                           const JceUIToggleComponent *tg, float alpha_mul)
{
    /* Background box. */
    uc_draw_quad(uc, view_id, r->x, r->y, r->w, r->h, tg->bg_sprite, tg->bg_color, alpha_mul);
    /* Checkmark: drawn only when on, inset slightly inside the box. */
    if (tg->is_on) {
        float inset_x = r->w * 0.2f, inset_y = r->h * 0.2f;
        uc_draw_quad(uc, view_id, r->x + inset_x, r->y + inset_y,
                     r->w - inset_x * 2.0f, r->h - inset_y * 2.0f,
                     tg->checkmark_sprite, tg->checkmark_color, alpha_mul);
    }
}

/* ── ProgressBar draw (PASS 3) ──────────────────────────────────────── */

/* Normalized fill position (0..1) of the progress bar along its main axis. */
static float uc_progress_norm(const JceUIProgressBarComponent *p)
{
    float span = p->max_value - p->min_value;
    if (span <= 0.0f) return 0.0f;
    float t = (p->value - p->min_value) / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

/* Read-only fill bar: bg quad + a fill sub-rect from the "start" edge to the
 * value position along `direction` (reusing the slider fill-quad math; no
 * handle, no raycast).  Renderer-gated by the caller. */
static void uc_draw_progress_bar(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                                 const JceUIProgressBarComponent *p, float alpha_mul)
{
    float t = uc_progress_norm(p);

    /* Background track. */
    uc_draw_quad(uc, view_id, r->x, r->y, r->w, r->h, NULL, p->bg_color, alpha_mul);

    /* Fill: a sub-rect from the start edge to the value position. */
    float fx = r->x, fy = r->y, fw = r->w, fh = r->h;
    switch (p->direction) {
        case 1: /* R→L: fill the right portion */
            fw = r->w * t; fx = r->x + r->w - fw; break;
        case 2: /* B→T: fill the bottom portion */
            fh = r->h * t; fy = r->y + r->h - fh; break;
        case 3: /* T→B: fill the top portion */
            fh = r->h * t; break;
        case 0: /* L→R: fill the left portion */
        default:
            fw = r->w * t; break;
    }
    uc_draw_quad(uc, view_id, fx, fy, fw, fh, p->fill_sprite, p->fill_color, alpha_mul);
}

/* Dropdown + InputField moved to jce_ui_canvas_widgets.c; the shared
 * rect type and the three drawing primitives are in its header. */
/* ── ScrollView: clamp math + draw (PASS 3) ─────────────────────────── */

/* Maximum scroll offset on each axis for a scroll view whose viewport rect is
 * `r`: max(0, content_size - viewport_size), but only on enabled axes (a
 * disabled axis has no scroll range).  Runs headless (pure math). */
static void uc_scroll_max(const JceUIScrollViewComponent *sv, const UCRect *r,
                          float ui_scale, float *out_max_x, float *out_max_y)
{
    /* UNITS.  `r` is a RESOLVED rect: uc_resolve_rect has already multiplied it
     * by the CanvasScaler factor, so it is in device px.  content_size,
     * scroll_position and scroll_sensitivity are AUTHORED numbers in
     * reference-resolution units, exactly like scrollbar_thickness (which the
     * draw already scales) and like every length a LayoutGroup carries.
     * Subtracting one from the other without converting made the scroll range
     * wrong by the scale factor on any canvas that actually scales -- too
     * little range on a scaled-up canvas, too much on a scaled-down one.
     *
     * Everything here is reference units; the viewport is divided down into
     * them, and the caller multiplies back up when it offsets device rects. */
    float vs = (ui_scale > 0.0001f) ? ui_scale : 1.0f;
    float vw = r->w / vs, vh = r->h / vs;
    float cw = sv->content_size[0], chh = sv->content_size[1];
    /* content_size 0 ⇒ treat as viewport (no scroll on that axis). */
    if (cw <= 0.0f) cw = vw;
    if (chh <= 0.0f) chh = vh;
    float mx = sv->horizontal ? (cw - vw) : 0.0f;
    float my = sv->vertical   ? (chh - vh) : 0.0f;
    if (mx < 0.0f) mx = 0.0f;
    if (my < 0.0f) my = 0.0f;
    *out_max_x = mx;
    *out_max_y = my;
}

/* Clamp the live scroll_position in place to [0, scroll_max] on each axis.
 * Always run (even when wheel does nothing) so an authored/restored offset that
 * exceeds the current viewport is corrected.  Pure math → headless-safe. */
static void uc_scroll_clamp(JceUIScrollViewComponent *sv, const UCRect *r,
                            float ui_scale)
{
    float mx, my;
    uc_scroll_max(sv, r, ui_scale, &mx, &my);
    if (sv->scroll_position[0] < 0.0f) sv->scroll_position[0] = 0.0f;
    if (sv->scroll_position[0] > mx)   sv->scroll_position[0] = mx;
    if (sv->scroll_position[1] < 0.0f) sv->scroll_position[1] = 0.0f;
    if (sv->scroll_position[1] > my)   sv->scroll_position[1] = my;
}

/* Draw the scroll view's viewport background + (optional) scrollbars.  The
 * scrollbars sit ON TOP of the (already-clipped) content: a track along the
 * right edge (vertical) / bottom edge (horizontal) plus a proportional thumb.
 * Renderer-gated by the caller (only invoked when uc->renderer != NULL). */
static void uc_draw_scroll_view(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                                const JceUIScrollViewComponent *sv, float alpha_mul)
{
    /* Viewport background. */
    uc_draw_quad(uc, view_id, r->x, r->y, r->w, r->h, NULL, sv->bg_color, alpha_mul);
}

/* ── Scrollbar geometry, single-sourced ────────────────────────────────
 *
 * The draw and the drag must agree on where the track and the thumb are, to
 * the pixel: a thumb you can see but cannot grab, or grab somewhere other than
 * where it is drawn, is the same class of defect as a hit rect that disagrees
 * with a draw rect.  So one function answers both.
 *
 * `axis` 0 = horizontal (bottom edge), 1 = vertical (right edge).  Returns
 * false when that axis has no scrollbar this frame (disabled, hidden, or the
 * content fits).  All rects are DEVICE px; `*out_max` is the scroll maximum in
 * reference units, matching scroll_position (see uc_scroll_max). */
static bool uc_sb_geom(const JceUIScrollViewComponent *sv, const UCRect *r,
                       float ui_scale, int axis,
                       UCRect *out_track, UCRect *out_thumb, float *out_max)
{
    if (!sv->show_scrollbar) return false;

    float mx, my;
    uc_scroll_max(sv, r, ui_scale, &mx, &my);
    const float m = axis ? my : mx;
    if (m <= 0.0f) return false;
    if (axis ? !sv->vertical : !sv->horizontal) return false;

    const float thick =
        (sv->scrollbar_thickness > 0.0f ? sv->scrollbar_thickness : 8.0f) * ui_scale;
    const float vs = (ui_scale > 0.0001f) ? ui_scale : 1.0f;
    const float vw = r->w / vs, vh = r->h / vs;
    float cw  = sv->content_size[0] > 0.0f ? sv->content_size[0] : vw;
    float chh = sv->content_size[1] > 0.0f ? sv->content_size[1] : vh;

    /* Does the OTHER axis also show a bar?  If so the two tracks are each
     * shortened by one thickness so they do not overlap in the corner -- they
     * used to draw over each other there, and the overlap square belonged to
     * whichever was drawn second.  Unity leaves that corner empty. */
    const bool other = axis
        ? (sv->horizontal && mx > 0.0f)
        : (sv->vertical   && my > 0.0f);
    const float corner = other ? thick : 0.0f;

    UCRect track, thumb;
    if (axis) {                                   /* vertical, right edge */
        track.x = r->x + r->w - thick;
        track.y = r->y;
        track.w = thick;
        track.h = r->h - corner;
        float th = track.h * (vh / (chh > 0.0f ? chh : vh));
        if (th < 8.0f * ui_scale) th = 8.0f * ui_scale;
        if (th > track.h)         th = track.h;
        float frac = sv->scroll_position[1] / m;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        thumb.x = track.x;
        thumb.w = thick;
        thumb.h = th;
        thumb.y = track.y + frac * (track.h - th);
    } else {                                      /* horizontal, bottom edge */
        track.x = r->x;
        track.y = r->y + r->h - thick;
        track.w = r->w - corner;
        track.h = thick;
        float tw = track.w * (vw / (cw > 0.0f ? cw : vw));
        if (tw < 8.0f * ui_scale) tw = 8.0f * ui_scale;
        if (tw > track.w)         tw = track.w;
        float frac = sv->scroll_position[0] / m;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        thumb.y = track.y;
        thumb.h = thick;
        thumb.w = tw;
        thumb.x = track.x + frac * (track.w - tw);
    }
    if (out_track) *out_track = track;
    if (out_thumb) *out_thumb = thumb;
    if (out_max)   *out_max   = m;
    return true;
}

/* Draw the scrollbar track + thumb for one scroll view (after its descendants
 * + scissor reset, so the bars are never clipped).  Renderer-gated by caller. */
static void uc_draw_scrollbars(JceUICanvas *uc, uint16_t view_id, const UCRect *r,
                               const JceUIScrollViewComponent *sv, float alpha_mul,
                               float ui_scale)
{
    for (int axis = 1; axis >= 0; --axis) {
        UCRect track, thumb;
        if (!uc_sb_geom(sv, r, ui_scale, axis, &track, &thumb, NULL)) continue;
        uc_draw_quad(uc, view_id, track.x, track.y, track.w, track.h, NULL,
                     sv->scrollbar_bg_color, alpha_mul);
        uc_draw_quad(uc, view_id, thumb.x, thumb.y, thumb.w, thumb.h, NULL,
                     sv->scrollbar_color, alpha_mul);
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

/* Intersect `r` with `clip`; returns false when nothing is left. */
static bool uc_rect_clip(const UCRect *r, const UCRect *clip, UCRect *out)
{
    float x0 = r->x > clip->x ? r->x : clip->x;
    float y0 = r->y > clip->y ? r->y : clip->y;
    float x1 = (r->x + r->w < clip->x + clip->w) ? r->x + r->w
                                                 : clip->x + clip->w;
    float y1 = (r->y + r->h < clip->y + clip->h) ? r->y + r->h
                                                 : clip->y + clip->h;
    if (x1 <= x0 || y1 <= y0) return false;
    out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0;
    return true;
}

/* Record + apply.  ONE function, so a clip that reaches the GPU and a clip
 * the log knows about cannot be two different things. */
static void uc_clip_set_logged(JceUICanvas *uc, const UCRect *r)
{
    if (uc && uc->clip_log_count < UC_MAX_CLIP_LOG) {
        UCClip *e = &uc->clip_log[uc->clip_log_count++];
        e->active = (r != NULL);
        if (r) e->rect = *r;
        else   { e->rect.x = e->rect.y = e->rect.w = e->rect.h = 0.0f; }
    }
    uc_clip_set(r);
}

/* `clip` is the active viewport a ScrollView ancestor imposes, or NULL.  It is
 * threaded through the recursion because the DRAW side already honours it (a
 * view-level bgfx scissor) while the raycast side did not: a widget scrolled
 * out of view stayed clickable at coordinates where it was not drawn.  Both
 * sides now derive from the same rect. */

static void uc_layout_draw(UCFrame *fr, JceEntity node, const UCRect *node_rect,
                           float inherited_alpha, bool inherited_blocks,
                           bool inherited_inter, int depth,
                           const UCRect *clip)
{
    JceScene *s = fr->scene;
    JceUICanvas *uc = fr->uc;
    if (depth > UC_MAX_DEPTH) return;

    /* ContentSizeFitter, BEFORE anything reads the rect: this node's own
     * graphics, its hit test and every child rect are all resolved from it,
     * so fitting it afterwards would size the panel and leave its contents
     * laid out against the old box. */
    UCRect fitted;
    if (node_rect) {
        fitted = *node_rect;
        if (uc_fit_rect(s, uc, fr->ui_scale, node, &fitted)) node_rect = &fitted;
    }

    float alpha = uc_node_alpha(s, node, inherited_alpha);
    bool  blocks = inherited_blocks, inter = inherited_inter;
    uc_node_raycast(s, node, inherited_blocks, inherited_inter, &blocks, &inter);

    /* Draw this node's own graphics (image first, then text on top).  This
     * also covers a Canvas entity that itself carries a UIImage (full-screen
     * background) — its node_rect is the whole canvas. */
    {
        /* Per-component disable: nullify any disabled UI component at fetch so
         * it is skipped for DRAW *and* excluded from the raycast hit list below
         * (which is what uc_update_buttons / uc_update_widgets iterate) — one
         * gate covers visuals + interaction.  Flag widgets use the flag shim;
         * presence-gated ones resolve a cached comp_id. */
        JceUIImageComponent *im = jce_scene_get_ui_image(s, node);
        if (im && !jce_scene_component_enabled(s, node, JCE_COMP_FLAG_UI_IMAGE)) im = NULL;
        if (im && uc->renderer)
            uc_draw_image(uc, fr->view_id, node_rect, im, alpha, fr->ui_scale);

        /* Slider / Toggle: draw their own quads (PASS 3).  Each is a
         * graphic in its own right and embeds its own RectTransform, so it
         * is laid out + drawn here next to UIImage. */
        JceUISliderComponent *sl = jce_scene_get_ui_slider(s, node);
        { static int cid = -2; if (cid == -2) cid = jce_component_find("UISlider");
          if (sl && cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) sl = NULL; }
        if (sl && uc->renderer)
            uc_draw_slider(uc, fr->view_id, node_rect, sl, alpha, fr->ui_scale);
        JceUIToggleComponent *tg = jce_scene_get_ui_toggle(s, node);
        { static int cid = -2; if (cid == -2) cid = jce_component_find("UIToggle");
          if (tg && cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) tg = NULL; }
        if (tg && uc->renderer)
            uc_draw_toggle(uc, fr->view_id, node_rect, tg, alpha);

        /* InputField: bg + (text|placeholder) + caret.  Each embeds its own
         * RectTransform so it is laid out + drawn here next to UIImage. */
        JceUIInputFieldComponent *inf = jce_scene_get_ui_input_field(s, node);
        { static int cid = -2; if (cid == -2) cid = jce_component_find("UIInputField");
          if (inf && cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) inf = NULL; }
        if (inf && uc->renderer)
            uc_draw_input_field(uc, fr->view_id, node_rect, inf, alpha,
                                fr->ui_scale, uc->focused_input == (uint64_t)node,
                                uc->caret_blink);

        /* ScrollView: a CONTAINER.  Clamp its live offset to the current
         * viewport (headless-safe), draw its viewport background here; its
         * descendants are offset + clipped in the children block below, and the
         * scrollbars are drawn there after the scissor reset.  The clamp runs
         * even on a headless canvas so a restored offset that exceeds the
         * viewport is corrected and so jce_ui_canvas_scroll sees a valid base. */
        JceUIScrollViewComponent *sv = jce_scene_get_ui_scroll_view(s, node);
        { static int cid = -2; if (cid == -2) cid = jce_component_find("UIScrollView");
          if (sv && cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) sv = NULL; }
        if (sv) {
            uc_scroll_clamp(sv, node_rect, fr->ui_scale);
            if (uc->renderer)
                uc_draw_scroll_view(uc, fr->view_id, node_rect, sv, alpha);
        }

        /* ProgressBar: read-only fill bar.  Drawn here next to UIImage; NOT a
         * raycast target (purely visual — gameplay drives `value`). */
        JceUIProgressBarComponent *pb = jce_scene_get_ui_progress_bar(s, node);
        { static int cid = -2; if (cid == -2) cid = jce_component_find("UIProgressBar");
          if (pb && cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) pb = NULL; }
        if (pb && uc->renderer)
            uc_draw_progress_bar(uc, fr->view_id, node_rect, pb, alpha);

        /* Dropdown: collapsed row here; the expanded popup is drawn AFTER it
         * (still this node, before children) so it overlays sibling UI in the
         * "drawn last → on top" z-order.  Each embeds its own RectTransform. */
        JceUIDropdownComponent *dd = jce_scene_get_ui_dropdown(s, node);
        { static int cid = -2; if (cid == -2) cid = jce_component_find("UIDropdown");
          if (dd && cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) dd = NULL; }
        /* An expanded popup that the pointer cannot REACH must not stay open.
         *
         * The interaction pass below walks fr->hits, so a dropdown whose
         * inherited CanvasGroup stopped blocking raycasts -- or whose main rect
         * is fully clipped away by an ancestor ScrollView -- is never visited
         * and can never be told to collapse.  It would keep drawing, and keep
         * publishing the modal rect below, so it becomes a permanent input
         * dead-zone over whatever is underneath: unreachable itself, and
         * blocking everything else.  The non-interactable branch of that pass
         * already states this invariant ("can't get stuck open"); it was just
         * enforced in the one place that is still reachable.
         *
         * Collapsing is also what Unity converges on: the popup there is a real
         * child object with a full-screen Blocker, and a popup whose blocker
         * cannot receive the click is not a state Unity's own Dropdown can
         * produce.  Reachability is decided by exactly the condition the hit
         * append below uses, so the two cannot drift. */
        if (dd && dd->expanded) {
            UCRect dd_clipped = *node_rect;
            const bool dd_reachable =
                blocks && (!clip || uc_rect_clip(node_rect, clip, &dd_clipped));
            if (!dd_reachable) dd->expanded = false;
        }

        /* Publish the popup as the frame's modal rect BEFORE any draw gate:
         * a headless canvas has no renderer and still runs the raycast, and
         * the popup must block the pointer there too. */
        if (dd && dd->expanded) {
            const int nopt = uc_dd_option_count(dd);
            if (nopt > 0) {
                /* The popup may sit above the control now (uc_dd_row_rect), so
                 * the modal rect is the union of the main rect and the option
                 * block rather than "everything below the main rect". */
                UCRect first = uc_dd_row_rect(node_rect, 0, nopt, fr->screen_h);
                UCRect last  = uc_dd_row_rect(node_rect, nopt - 1, nopt,
                                              fr->screen_h);
                float top = node_rect->y < first.y ? node_rect->y : first.y;
                float bot = node_rect->y + node_rect->h;
                if (last.y + last.h > bot) bot = last.y + last.h;
                fr->modal_rect.x = node_rect->x;
                fr->modal_rect.y = top;
                fr->modal_rect.w = node_rect->w;
                fr->modal_rect.h = bot - top;
                fr->modal_owner  = node;
                fr->modal_active = true;
            }
        }
        if (dd && uc->renderer) {
            uc_draw_dropdown(uc, fr->view_id, node_rect, dd, alpha, fr->ui_scale);
            if (dd->expanded)
                uc_draw_dropdown_popup(uc, fr->view_id, node_rect, dd, alpha,
                                       fr->ui_scale, fr->ptr_x, fr->ptr_y,
                                       fr->ptr_valid, fr->screen_h);
        }

        /* Record raycast hit for interactive elements — but only while the
         * inherited CanvasGroup chain blocks raycasts (else the pointer passes
         * through).  The inherited `interactable` rides along so a button under
         * a non-interactable group is driven to its disabled state.  Sliders,
         * toggles, input fields, scroll views and dropdowns are raycast-
         * interactive by nature (no raycast_target opt-in) so they join the
         * same top-most hit list.  ProgressBar is purely visual (not recorded). */
        JceUIButtonComponent *bt = jce_scene_get_ui_button(s, node);
        if (bt && !jce_scene_component_enabled(s, node, JCE_COMP_FLAG_UI_BUTTON)) bt = NULL;
        bool ray = (im && im->raycast_target) || (bt != NULL) ||
                   (sl != NULL) || (tg != NULL) || (inf != NULL) || (sv != NULL) ||
                   (dd != NULL);
        if (ray && blocks && fr->hit_count < UC_MAX_BUTTONS) {
            /* Clipped to the ancestor viewport, exactly like the draw.  A
             * fully-clipped widget records NO hit at all rather than a hit at
             * coordinates it is not drawn at. */
            UCRect hit_rect = *node_rect;
            if (!clip || uc_rect_clip(node_rect, clip, &hit_rect)) {
                fr->hits[fr->hit_count].entity       = node;
                fr->hits[fr->hit_count].rect         = hit_rect;
                fr->hits[fr->hit_count].alpha        = alpha;
                fr->hits[fr->hit_count].interactable = inter;
                fr->hit_count++;
            }
        }

        JceUITextComponent *tx = jce_scene_get_ui_text(s, node);
        if (tx && !jce_scene_component_enabled(s, node, JCE_COMP_FLAG_UI_TEXT)) tx = NULL;
        if (tx && uc->renderer)
            uc_draw_text(uc, fr->view_id, node_rect, tx, alpha, fr->ui_scale);
    }

    /* Children. */
    int child_count = jce_scene_get_child_count(s, node);
    if (child_count <= 0) return;

    /* Gather UI children.
     *
     * THE STACK ARRAYS ARE A FAST PATH, NOT A LIMIT.  They used to be the
     * limit: the walk asked for the true child_count above and then gathered
     * at most 128, and jce_scene_get_children truncates SILENTLY (it drains
     * the iterator but stops writing).  Everything downstream reads the
     * gathered array -- the draw, the LayoutGroup packing, the scissor stack
     * and the raycast hit list -- so a node's 129th UI child was not merely
     * unarranged, it did not exist: never drawn, never clickable.  Nothing
     * said so, and a list that long is exactly the case (an inventory, a
     * server browser, a chat log) where a UI toolkit is expected to hold.
     *
     * Growing on the heap only when a node really has more keeps the common
     * case allocation-free, which matters because this runs per node per
     * frame and this function's recursion is the stack. */
    enum { MAX_LOCAL_CHILDREN = 128 };
    JceEntity  kids_local[MAX_LOCAL_CHILDREN];
    JceEntity  ui_local[MAX_LOCAL_CHILDREN];
    UCRect     rect_local[MAX_LOCAL_CHILDREN];
    JceEntity *kids      = kids_local;
    JceEntity *ui_kids   = ui_local;
    UCRect    *kid_rects = rect_local;
    void      *heap      = NULL;
    int cap = MAX_LOCAL_CHILDREN;

    if (child_count > MAX_LOCAL_CHILDREN) {
        /* One block, three arrays: a single allocation and a single free, so
         * there is one early-return hazard instead of three. */
        const size_t bytes = (size_t)child_count * (2u * sizeof(JceEntity) +
                                                    sizeof(UCRect));
        heap = JCE_MALLOC(bytes);
        if (heap) {
            kids      = (JceEntity *)heap;
            ui_kids   = kids + child_count;
            kid_rects = (UCRect *)(void *)(ui_kids + child_count);
            cap       = child_count;
        } else {
            /* Out of memory: fall back to the stack arrays and SAY SO once.
             * Silently drawing a prefix is what this change exists to end. */
            static bool warned_oom = false;
            if (!warned_oom) {
                warned_oom = true;
                LOG_WARN(LOG_TAG,
                         "ui: could not allocate for %d children of one node; "
                         "showing the first %d", child_count,
                         MAX_LOCAL_CHILDREN);
            }
        }
    }

    int n = jce_scene_get_children(s, node, kids, cap);
    if (n <= 0) { if (heap) JCE_FREE(heap); return; }

    /* Filter to UI elements only and resolve each child's rect. */
    int ui_n = 0;
    for (int i = 0; i < n && ui_n < cap; i++) {
        if (!uc_is_ui_element(s, kids[i])) continue;
        const JceRectTransform *rt = uc_entity_rect(s, kids[i]);
        ui_kids[ui_n]  = kids[i];
        kid_rects[ui_n] = uc_resolve_rect(node_rect, rt, fr->ui_scale, fr->pixel_perfect);
        ui_n++;
    }

    /* LayoutGroup on this node overrides child positions. */
    JceLayoutGroupComponent *lg = jce_scene_get_layout_group(s, node);
    if (lg && !jce_scene_component_enabled(s, node, JCE_COMP_FLAG_LAYOUT_GROUP)) lg = NULL;
    if (lg && ui_n > 0)
        uc_apply_layout_group(s, uc, node_rect, lg, ui_kids, ui_n, kid_rects,
                              fr->ui_scale);

    /* ScrollView container: OFFSET every descendant by -scroll_position (so the
     * content scrolls under a fixed viewport) and CLIP them to the viewport.
     * The offset is applied AFTER any LayoutGroup packing (the list is packed in
     * viewport space, then the whole packed block scrolls).  The clip uses a
     * view-level bgfx scissor (applies to every primitive submitted to this view
     * regardless of the per-draw BGFX_DISCARD_ALL) which is RESET after the
     * subtree so it never clips later unrelated UI draws.  All offset math runs
     * headless; only the scissor + scrollbar draws are renderer-gated. */
    /* SAME GATE as the draw fetch above.  This second fetch used to skip the
     * jce_scene_comp_enabled test that every sibling widget applies, so a
     * DISABLED UIScrollView stopped drawing but kept offsetting and clipping
     * its children -- one component, two gates, and only one of them honoured
     * the per-component disable the editor offers.  (The draw fetch's pointer
     * is scoped to the node block above, so the test is repeated rather than
     * the pointer reused; they must not diverge again.) */
    JceUIScrollViewComponent *sv2 = jce_scene_get_ui_scroll_view(s, node);
    { static int cid = -2; if (cid == -2) cid = jce_component_find("UIScrollView");
      if (sv2 && cid >= 0 && !jce_scene_comp_enabled(s, node, cid)) sv2 = NULL; }
    if (sv2) {
        /* scroll_position is in reference units (see uc_scroll_max); the child
         * rects it shifts are device px, so scale on the way out. */
        float ox = sv2->scroll_position[0] * fr->ui_scale;
        float oy = sv2->scroll_position[1] * fr->ui_scale;
        if (ox != 0.0f || oy != 0.0f) {
            for (int i = 0; i < ui_n; i++) {
                kid_rects[i].x -= ox;
                kid_rects[i].y -= oy;
            }
        }
    }

    /* A ScrollView's own rect becomes the clip for everything beneath it,
     * INTERSECTED with whatever clip was already active. */
    UCRect child_clip;
    const UCRect *child_clip_p = clip;
    if (sv2) {
        if (!clip) { child_clip = *node_rect; child_clip_p = &child_clip; }
        else if (uc_rect_clip(node_rect, clip, &child_clip)) child_clip_p = &child_clip;
        else { child_clip = *node_rect; child_clip.w = 0.0f; child_clip.h = 0.0f;
               child_clip_p = &child_clip; }
    }

    /* The GPU scissor follows child_clip_p, NOT node_rect, and is RESTORED to
     * the incoming clip rather than cleared.  Both were wrong before and both
     * only show up with a ScrollView inside a ScrollView:
     *
     *   - scissoring to node_rect ignores the outer clip, so an inner
     *     ScrollView drew outside the outer viewport;
     *   - clearing on the way out drops the outer clip for every SIBLING that
     *     follows, so content after a nested ScrollView was not clipped at all.
     *
     * Restoring works because this function's recursion IS the stack: each
     * level holds its own `clip` and puts it back. */
    if (sv2) uc_clip_set_logged(uc, child_clip_p);

    for (int i = 0; i < ui_n; i++)
        uc_layout_draw(fr, ui_kids[i], &kid_rects[i], alpha, blocks, inter,
                       depth + 1, child_clip_p);

    if (sv2) {
        /* Scrollbars sit ON TOP of the clipped content and are part of the
         * ScrollView's own chrome, so they are drawn against the clip this
         * level was entered with -- not against the content clip, which would
         * scissor away a scrollbar that overhangs the viewport edge. */
        uc_clip_set_logged(uc, clip);
        uc_draw_scrollbars(uc, fr->view_id, node_rect, sv2, alpha, fr->ui_scale);
    }

    /* AFTER the recursion and the scrollbars: every read of kid_rects is done.
     * Freeing at the last statement rather than at each early return is why
     * the three arrays share one block. */
    if (heap) JCE_FREE(heap);
}

/* ── Button raycast + state machine ────────────────────────────────── */

static bool uc_point_in(const UCRect *r, float px, float py)
{
    return px >= r->x && px < r->x + r->w &&
           py >= r->y && py < r->y + r->h;
}

/* True when the pointer is over an OPEN dropdown popup that `e` does not own.
 *
 * The popup is drawn on top and handled by uc_update_widgets, but it was never
 * in the hit list, and uc_update_buttons resolves `hovered` from that list and
 * runs FIRST -- so choosing an option also pressed whatever sat under the
 * popup.  Both hover resolutions consult this now, so a modal popup blocks the
 * pointer the way it visually appears to. */
static bool uc_blocked_by_modal(const UCFrame *fr, JceEntity e,
                                float px, float py)
{
    if (!fr->modal_active || e == fr->modal_owner) return false;
    return uc_point_in(&fr->modal_rect, px, py);
}


/* -- ScrollView drag ---------------------------------------------------
 *
 * Three gestures, all of them missing before: dragging the scrollbar THUMB,
 * clicking the scrollbar TRACK to jump, and dragging the CONTENT itself.
 *
 * Runs BEFORE uc_update_buttons because a content drag has to be able to
 * cancel a press a button under the finger has already latched -- Unity does
 * the same thing through the event system's drag threshold, and without it
 * every drag that starts on a list item also clicks it.
 *
 * The threshold is what separates a click from a drag; below it nothing moves
 * and the click stands.  Distances are device px and offsets are reference
 * units (see uc_scroll_max), so the pointer delta is divided by ui_scale
 * exactly once, here.  Fully headless -- no draw, no renderer.
 *
 * Returns true when a drag owns the gesture, i.e. clicks are suppressed. */
static bool uc_update_scroll_drag(UCFrame *fr, const JceUIPointer *ptr)
{
    JceUICanvas *uc = fr->uc;
    JceScene    *s  = fr->scene;

    const float THRESH = 6.0f;   /* device px before a press becomes a drag */

    if (!ptr || !ptr->valid || !ptr->down) {
        const bool consumed = uc->scroll_drag_moved;
        uc->scroll_drag       = 0;
        uc->scroll_drag_mode  = 0;
        uc->scroll_drag_moved = false;
        uc->scroll_press      = 0;
        return consumed;   /* the release that ENDS a drag must not click */
    }

    /* The scroll view under the pointer: top-most, interactable, not behind an
     * open popup.  Same rule the wheel channel uses. */
    JceEntity target = 0;
    UCRect    target_rect = { 0, 0, 0, 0 };
    for (int i = fr->hit_count - 1; i >= 0; i--) {
        if (fr->hits[i].alpha <= 0.001f) continue;
        if (!fr->hits[i].interactable)   continue;
        if (!jce_scene_has_ui_scroll_view(s, fr->hits[i].entity)) continue;
        if (uc_blocked_by_modal(fr, fr->hits[i].entity, ptr->x, ptr->y)) continue;
        if (uc_point_in(&fr->hits[i].rect, ptr->x, ptr->y)) {
            target = fr->hits[i].entity;
            target_rect = fr->hits[i].rect;
            break;
        }
    }

    /* -- press edge: thumb / track / content ------------------------ */
    if (!uc->scroll_drag && !uc->scroll_press && target) {
        JceUIScrollViewComponent *sv = jce_scene_get_ui_scroll_view(s, target);
        static int cid = -2; if (cid == -2) cid = jce_component_find("UIScrollView");
        if (sv && cid >= 0 && !jce_scene_comp_enabled(s, target, cid)) sv = NULL;
        if (sv && sv->interactable) {
            int mode = 0;
            for (int axis = 1; axis >= 0 && !mode; --axis) {
                UCRect track, thumb; float m;
                if (!uc_sb_geom(sv, &target_rect, fr->ui_scale, axis,
                                &track, &thumb, &m)) continue;
                if (!uc_point_in(&track, ptr->x, ptr->y)) continue;
                mode = axis ? 1 : 2;
                if (!uc_point_in(&thumb, ptr->x, ptr->y)) {
                    /* Track click: centre the thumb on the pointer, then drag
                     * on from there so a click-and-hold keeps tracking. */
                    const float len = axis ? (track.h - thumb.h)
                                           : (track.w - thumb.w);
                    const float pos = axis ? (ptr->y - track.y - thumb.h * 0.5f)
                                           : (ptr->x - track.x - thumb.w * 0.5f);
                    float frac = (len > 0.0f) ? (pos / len) : 0.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    sv->scroll_position[axis] = frac * m;
                    uc_scroll_clamp(sv, &target_rect, fr->ui_scale);
                }
                uc->scroll_drag         = (uint64_t)target;
                uc->scroll_drag_mode    = mode;
                uc->scroll_drag_moved   = true;  /* a bar grab is never a click */
                uc->scroll_drag_grab[0] = ptr->x;
                uc->scroll_drag_grab[1] = ptr->y;
                uc->scroll_drag_base[0] = sv->scroll_position[0];
                uc->scroll_drag_base[1] = sv->scroll_position[1];
                uc->scroll_drag_rect    = target_rect;
                uc->scroll_drag_scale   = fr->ui_scale;
            }
            if (!mode) {
                /* Content-drag CANDIDATE.  Not a drag yet: it becomes one only
                 * past the threshold, so a plain click on a list item still
                 * clicks. */
                uc->scroll_press        = (uint64_t)target;
                uc->scroll_press_xy[0]  = ptr->x;
                uc->scroll_press_xy[1]  = ptr->y;
                uc->scroll_drag_rect    = target_rect;
                uc->scroll_drag_scale   = fr->ui_scale;
                uc->scroll_drag_base[0] = sv->scroll_position[0];
                uc->scroll_drag_base[1] = sv->scroll_position[1];
            }
        }
    }

    /* -- the candidate crosses the threshold ------------------------ */
    if (!uc->scroll_drag && uc->scroll_press) {
        const float dx = ptr->x - uc->scroll_press_xy[0];
        const float dy = ptr->y - uc->scroll_press_xy[1];
        if (dx * dx + dy * dy >= THRESH * THRESH) {
            uc->scroll_drag         = uc->scroll_press;
            uc->scroll_drag_mode    = 3;
            uc->scroll_drag_moved   = true;
            uc->scroll_drag_grab[0] = uc->scroll_press_xy[0];
            uc->scroll_drag_grab[1] = uc->scroll_press_xy[1];
            uc->scroll_press        = 0;
        }
    }

    if (!uc->scroll_drag) return false;

    JceUIScrollViewComponent *sv =
        jce_scene_get_ui_scroll_view(s, (JceEntity)uc->scroll_drag);
    if (!sv) { uc->scroll_drag = 0; uc->scroll_drag_mode = 0; return false; }

    const float vs = (uc->scroll_drag_scale > 0.0001f) ? uc->scroll_drag_scale
                                                       : 1.0f;
    const UCRect *vr = &uc->scroll_drag_rect;

    if (uc->scroll_drag_mode == 3) {
        /* Content drag: the content follows the finger, so the OFFSET moves
         * against it -- drag up, reveal what is below, offset increases. */
        if (sv->horizontal)
            sv->scroll_position[0] = uc->scroll_drag_base[0]
                                   - (ptr->x - uc->scroll_drag_grab[0]) / vs;
        if (sv->vertical)
            sv->scroll_position[1] = uc->scroll_drag_base[1]
                                   - (ptr->y - uc->scroll_drag_grab[1]) / vs;
    } else {
        /* Thumb drag: travel along the track maps to the whole range. */
        const int axis = (uc->scroll_drag_mode == 1) ? 1 : 0;
        UCRect track, thumb; float m;
        if (uc_sb_geom(sv, vr, vs, axis, &track, &thumb, &m)) {
            const float len = axis ? (track.h - thumb.h) : (track.w - thumb.w);
            const float d   = axis ? (ptr->y - uc->scroll_drag_grab[1])
                                   : (ptr->x - uc->scroll_drag_grab[0]);
            if (len > 0.0f)
                sv->scroll_position[axis] = uc->scroll_drag_base[axis]
                                          + (d / len) * m;
        }
    }
    uc_scroll_clamp(sv, vr, vs);
    return true;
}

static void uc_update_buttons(UCFrame *fr, const JceUIPointer *ptr, float dt)
{
    JceUICanvas *uc = fr->uc;
    JceScene *s = fr->scene;
    uc->last_clicked = 0;

    /* A drag in flight owns the gesture.  Every latched press is dropped, so
     * the release that ends the drag cannot also fire on_click -- the failure
     * anyone who has scrolled a list on a phone would notice immediately. */
    const bool dragging = uc_update_scroll_drag(fr, ptr);
    if (dragging) {
        for (int i = 0; i < UC_MAX_BUTTONS; i++)
            uc->buttons[i].press_inside = false;
    }

    /* Top-most hit = last recorded (drawn last → on top). */
    JceEntity hovered = 0;
    if (ptr && ptr->valid) {
        for (int i = fr->hit_count - 1; i >= 0; i--) {
            if (fr->hits[i].alpha <= 0.001f) continue;
            if (uc_blocked_by_modal(fr, fr->hits[i].entity,
                                    ptr->x, ptr->y)) continue;
            if (uc_point_in(&fr->hits[i].rect, ptr->x, ptr->y)) {
                hovered = fr->hits[i].entity;
                break;
            }
        }
    }

    /* Reap stale widget states (entities that are no longer any interactive
     * widget).  The shared state cache is keyed by entity and used by buttons,
     * sliders AND toggles (uc_update_widgets runs next), so only reap a slot
     * once its entity carries none of them. */
    for (int i = 0; i < UC_MAX_BUTTONS; i++) {
        uint64_t e = uc->buttons[i].entity;
        if (e &&
            !jce_scene_has_ui_button(s, (JceEntity)e) &&
            !jce_scene_has_ui_slider(s, (JceEntity)e) &&
            !jce_scene_has_ui_toggle(s, (JceEntity)e) &&
            !jce_scene_has_ui_input_field(s, (JceEntity)e) &&
            !jce_scene_has_ui_dropdown(s, (JceEntity)e))
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
                if (over && !dragging) uc->last_clicked = (uint64_t)e;
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
         * visible (Unity ColorTint transition).  Skipped on a headless canvas
         * — the click state machine above still ran. */
        /* A button with no UIImage on its own entity used to show NO state
         * feedback at all: the click state machine ran and on_click fired, but
         * every one of the four state colours and fade_duration was dead,
         * because the tint was applied by re-drawing the sibling image.  Draw
         * the tint on the button's own resolved rect instead when there is no
         * image to re-draw -- the rect is already in the hit record. */
        JceUIImageComponent *im = uc->renderer ? jce_scene_get_ui_image(s, e) : NULL;
        if (!im && uc->renderer && st->fade > 0.0f) {
            const float *bcol = bt->normal_color;
            if (st->state == 1) bcol = bt->highlighted_color;
            else if (st->state == 2) bcol = bt->pressed_color;
            else if (st->state == 3) bcol = bt->disabled_color;
            uc_rect_view(uc->renderer, fr->view_id,
                                      fr->hits[i].rect.x, fr->hits[i].rect.y,
                                      fr->hits[i].rect.w, fr->hits[i].rect.h,
                                      uc_color(bcol, fr->hits[i].alpha));
        }
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
                uc_tex_view(uc->renderer, fr->view_id,
                                            fr->hits[i].rect.x, fr->hits[i].rect.y,
                                            fr->hits[i].rect.w, fr->hits[i].rect.h,
                                            tex, tint, NULL);
            else
                uc_rect_view(uc->renderer, fr->view_id,
                                          fr->hits[i].rect.x, fr->hits[i].rect.y,
                                          fr->hits[i].rect.w, fr->hits[i].rect.h,
                                          tint);
        }
    }
}

/* ── Slider / Toggle interaction state machine ──────────────────────── */

/* Drives UISlider drag and UIToggle click, mirroring uc_update_buttons:
 *   - top-most hit under the pointer is the hovered widget;
 *   - a TOGGLE flips its is_on on a press-inside → release-over (same latch
 *     the button uses), recorded in the shared press_inside flag;
 *   - a SLIDER tracks a drag_active latch: a press that begins over it (or a
 *     hold that is over it) sets drag_active and maps the pointer to a value,
 *     writing it back into the scene component (Unity source-of-truth model);
 *     releasing the pointer clears drag_active.
 * The value/state mutation runs on a HEADLESS canvas too (renderer NULL) — only
 * the draw calls above are gated on uc->renderer, exactly like buttons. */
static void uc_update_widgets(UCFrame *fr, const JceUIPointer *ptr, float dt)
{
    (void)dt;
    JceUICanvas *uc = fr->uc;
    JceScene *s = fr->scene;

    /* Top-most hit = last recorded (drawn last → on top). */
    JceEntity hovered = 0;
    if (ptr && ptr->valid) {
        for (int i = fr->hit_count - 1; i >= 0; i--) {
            if (fr->hits[i].alpha <= 0.001f) continue;
            if (uc_blocked_by_modal(fr, fr->hits[i].entity,
                                    ptr->x, ptr->y)) continue;
            if (uc_point_in(&fr->hits[i].rect, ptr->x, ptr->y)) {
                hovered = fr->hits[i].entity;
                break;
            }
        }
    }

    /* Hovered ScrollView for the wheel channel: the TOP-MOST scroll view whose
     * viewport contains the pointer.  This is resolved SEPARATELY from `hovered`
     * (which is the absolute top-most interactive widget) because a child widget
     * inside the list — e.g. a button — is drawn on top of the scroll view, yet
     * the wheel should still scroll the enclosing view.  Stored with its
     * resolved viewport rect so jce_ui_canvas_scroll can clamp without a
     * re-layout. */
    uc->hovered_scroll = 0;
    if (ptr && ptr->valid) {
        for (int i = fr->hit_count - 1; i >= 0; i--) {
            if (fr->hits[i].alpha <= 0.001f) continue;
            /* A ScrollRect under a non-interactable CanvasGroup does not scroll
             * in Unity, and the wheel is the one channel here that was reading
             * the hit list without consulting the inherited chain. */
            if (!fr->hits[i].interactable) continue;
            if (!jce_scene_has_ui_scroll_view(s, fr->hits[i].entity)) continue;
            if (uc_point_in(&fr->hits[i].rect, ptr->x, ptr->y)) {
                uc->hovered_scroll       = (uint64_t)fr->hits[i].entity;
                uc->hovered_scroll_rect  = fr->hits[i].rect;
                uc->hovered_scroll_scale = fr->ui_scale;
                break;
            }
        }
    }

    /* Reap stale states (entities that are no longer slider/toggle/button/
     * input-field/dropdown). */
    for (int i = 0; i < UC_MAX_BUTTONS; i++) {
        uint64_t e = uc->buttons[i].entity;
        if (e &&
            !jce_scene_has_ui_button(s, (JceEntity)e) &&
            !jce_scene_has_ui_slider(s, (JceEntity)e) &&
            !jce_scene_has_ui_toggle(s, (JceEntity)e) &&
            !jce_scene_has_ui_input_field(s, (JceEntity)e) &&
            !jce_scene_has_ui_dropdown(s, (JceEntity)e))
            uc->buttons[i].entity = 0;
    }

    /* ── InputField focus management ──────────────────────────────────
     * A pointer press that begins this frame: if it lands on an interactable
     * InputField (the top-most hit), focus it on release-over (press-inside →
     * release-over latch, mirroring the button click).  A press that lands on
     * NO input field clears focus (clicking empty space / another widget).
     * Runs headless (renderer NULL) — only the draws above are gated. */
    if (uc->focused_input &&
        !jce_scene_has_ui_input_field(s, (JceEntity)uc->focused_input)) {
        uc->focused_input = 0;   /* focused field was deleted */
        uc->caret = 0;
    }
    if (ptr && ptr->valid) {
        bool press_began = ptr->down;
        /* Track press/release on the focus latch via the shared state of the
         * hovered input field (reuse press_inside).  We detect a click on a
         * field, or a click that hit no field at all. */
        bool hovered_is_field = hovered &&
            jce_scene_has_ui_input_field(s, (JceEntity)hovered);
        if (!press_began) {
            /* Release frame: resolve any pending focus press. */
            for (int i = 0; i < fr->hit_count; i++) {
                JceEntity e = fr->hits[i].entity;
                if (!jce_scene_has_ui_input_field(s, e)) continue;
                UCButtonState *st = uc_button_state(uc, (uint64_t)e);
                if (!st) continue;
                JceUIInputFieldComponent *inf = jce_scene_get_ui_input_field(s, e);
                if (st->press_inside) {
                    if (hovered == e && inf && inf->interactable &&
                        fr->hits[i].interactable) {
                        if (uc->focused_input != (uint64_t)e) {
                            uc->focused_input = (uint64_t)e;
                            uc->caret = (int)strlen(inf->text); /* caret to end */
                            uc->caret_blink = 1.0f;             /* show caret */
                        }
                    }
                    st->press_inside = false;
                }
            }
        } else {
            /* Press frame: latch press_inside on the hovered field; if the
             * press hit no field, clear focus immediately. */
            if (hovered_is_field) {
                UCButtonState *st = uc_button_state(uc, (uint64_t)hovered);
                if (st) st->press_inside = true;
            } else {
                uc->focused_input = 0;
                uc->caret = 0;
            }
        }
    }

    /* ── Dropdown expand / select / outside-collapse ──────────────────
     * A press over the collapsed main rect toggles `expanded` on release-over
     * (press-inside → release-over latch).  While expanded, the option rows
     * (drawn below the main rect, not raycast-recorded) are ALSO click targets:
     * a click on row i sets selected_index=i and collapses; a click OUTSIDE the
     * main rect AND all popup rows collapses without changing selection
     * (mirroring the InputField defocus-on-outside-click pattern).  All
     * writeback goes through the MUTABLE jce_scene_get_ui_dropdown pointer
     * (Unity source-of-truth model) and runs fully headless. */
    if (ptr && ptr->valid) {
        for (int i = 0; i < fr->hit_count; i++) {
            JceEntity e = fr->hits[i].entity;
            JceUIDropdownComponent *dd = jce_scene_get_ui_dropdown(s, e);
            if (!dd) continue;
            UCButtonState *st = uc_button_state(uc, (uint64_t)e);
            if (!st) continue;

            const UCRect *mr = &fr->hits[i].rect;
            bool inter = dd->interactable && fr->hits[i].interactable;
            int  oc = uc_dd_option_count(dd);

            /* Resolve which click target the pointer is over this frame:
             *   -2 = collapsed main rect, >=0 = an (expanded) option row,
             *   -1 = neither (outside). */
            int target = -1;
            if (uc_point_in(mr, ptr->x, ptr->y)) {
                target = -2;
            } else if (dd->expanded) {
                for (int r = 0; r < oc; r++) {
                    UCRect row = uc_dd_row_rect(mr, r, oc, fr->screen_h);
                    if (uc_point_in(&row, ptr->x, ptr->y)) { target = r; break; }
                }
            }

            if (!inter) {
                /* Non-interactable: never expands; an already-open popup still
                 * collapses on an outside press so it can't get stuck open. */
                st->dd_press = -1;
                if (ptr->down && dd->expanded && target == -1)
                    dd->expanded = false;
                continue;
            }

            if (ptr->down) {
                /* Press frame: latch the target; an outside press collapses an
                 * open popup immediately (click-away). */
                if (st->dd_press == -1) {  /* only latch on the press edge */
                    st->dd_press = target;
                    if (target == -1 && dd->expanded)
                        dd->expanded = false;
                }
            } else {
                /* Release frame: commit only if release-over the SAME target. */
                int pressed = st->dd_press;
                st->dd_press = -1;
                if (pressed == -2 && target == -2) {
                    dd->expanded = !dd->expanded;       /* toggle the popup */
                } else if (pressed >= 0 && pressed == target && pressed < oc) {
                    if (dd->selected_index != pressed)
                        uc->last_value_changed = (uint64_t)e;  /* on_value_changed */
                    dd->selected_index = pressed;       /* pick the option */
                    dd->expanded = false;               /* and collapse */
                }
            }
        }
    } else {
        /* Pointer gone (cursor left the viewport): drop any in-flight dropdown
         * press so it cannot resume as a stale click next frame. */
        for (int i = 0; i < fr->hit_count; i++) {
            JceUIDropdownComponent *dd = jce_scene_get_ui_dropdown(s, fr->hits[i].entity);
            if (!dd) continue;
            UCButtonState *st = uc_button_state(uc, (uint64_t)fr->hits[i].entity);
            if (st) st->dd_press = -1;
        }
    }

    for (int i = 0; i < fr->hit_count; i++) {
        JceEntity e = fr->hits[i].entity;
        bool over = (hovered == e);

        /* ── Toggle ──────────────────────────────────────────────── */
        JceUIToggleComponent *tg = jce_scene_get_ui_toggle(s, e);
        if (tg) {
            UCButtonState *st = uc_button_state(uc, (uint64_t)e);
            if (!st) continue;
            if (!ptr || !ptr->valid) st->press_inside = false;
            if (!tg->interactable || !fr->hits[i].interactable) {
                st->press_inside = false;
            } else {
                if (ptr && ptr->down && over && !st->press_inside)
                    st->press_inside = true;
                if (st->press_inside && ptr && !ptr->down) {
                    if (over) {
                        tg->is_on = !tg->is_on;         /* click flips */
                        uc->last_value_changed = (uint64_t)e;  /* on_value_changed */
                    }
                    st->press_inside = false;
                }
            }
            continue;
        }

        /* ── Slider ──────────────────────────────────────────────── */
        JceUISliderComponent *sl = jce_scene_get_ui_slider(s, e);
        if (sl) {
            UCButtonState *st = uc_button_state(uc, (uint64_t)e);
            if (!st) continue;
            if (!ptr || !ptr->valid || !ptr->down) {
                st->drag_active = false; /* release ends the drag */
                continue;
            }
            if (!sl->interactable || !fr->hits[i].interactable) {
                st->drag_active = false;
                continue;
            }
            /* Begin a drag when the press is over this slider; keep tracking
             * once active even if the pointer slips slightly off the rect. */
            if (over) st->drag_active = true;
            if (st->drag_active) {
                float t = uc_slider_pointer_t(sl, &fr->hits[i].rect, ptr->x, ptr->y);
                float v = sl->min_value + (sl->max_value - sl->min_value) * t;
                if (sl->whole_numbers) v = floorf(v + 0.5f);
                if (v != sl->value) {
                    sl->value = v;
                    uc->last_value_changed = (uint64_t)e;  /* on_value_changed */
                }
            }
            continue;
        }
    }
}

/* ── Canvas enumeration ────────────────────────────────────────────── */

typedef struct { JceEntity list[64]; int count; } UCCanvasList;

/* A Canvas with a Canvas ancestor is NOT a root.
 *
 * Every collected canvas is rendered against the whole framebuffer, so a
 * Canvas parented under a UI element used to escape its parent's rect and
 * re-anchor to the screen -- and if it also carried a graphic it was drawn and
 * pushed into the raycast hit list TWICE: once through its parent's child walk
 * and once again here as a root.  Nesting is reachable from the editor: the
 * GameObject > UI > Canvas item parents to the current selection.
 *
 * Unity's rule is the same one: a nested Canvas inherits its parent Canvas's
 * space and does not become a second screen-space root. */
static bool uc_has_canvas_ancestor(JceScene *s, JceEntity e)
{
    JceEntity p = jce_scene_get_parent(s, e);
    for (int depth = 0; p != 0 && depth < UC_MAX_DEPTH; ++depth) {
        if (jce_scene_has_canvas(s, p)) return true;
        JceEntity next = jce_scene_get_parent(s, p);
        if (next == p) break;                 /* defensive: cycle */
        p = next;
    }
    return false;
}

static void uc_collect_canvas_cb(JceScene *s, JceEntity e, void *ud)
{
    UCCanvasList *cl = (UCCanvasList *)ud;
    if (cl->count >= 64) return;
    if (jce_scene_has_canvas(s, e) && !uc_has_canvas_ancestor(s, e))
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
    /* renderer may be NULL: a headless canvas runs the full layout + raycast +
     * UIButton click state machine but skips every GPU draw, so gameplay/tests
     * can drive click dispatch without a live bgfx context. */
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

    /* Remember the scene so the (canvas-only) text/key delivery functions can
     * resolve the focused field's component this frame. */
    uc->edit_scene = scene;

    /* Configure the UI view: bind framebuffer (panel FBO or backbuffer),
     * set the rect, and a top-left-origin orthographic projection in logical
     * pixels — matching JCE_VIEW_UI so UI coords are screen pixels.  The view
     * does NOT clear (it overlays the rendered scene).  Skipped on a headless
     * canvas (no renderer ⇒ no bgfx context to touch). */
    if (uc->renderer) {
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
    /* Component query, not a world walk: this used to visit every entity in
     * the scene and test has_canvas, and the count==0 early-out below only
     * ran AFTER that walk - so a scene with no canvas paid for all of it
     * (measured 4.14 ms/frame at 200k entities, with the overlay at its
     * shipped default of ON). */
    jce_scene_each_canvas(scene, uc_collect_canvas_cb, &canvases);
    /* Say once how many canvases this scene actually presented.  "My authored
     * UI is not on screen" is otherwise unanswerable from a log: a scene with
     * no Canvas component and a scene whose canvases were all filtered out
     * both produce complete silence, and so does a UI that rendered
     * perfectly.  One line, on the first render, distinguishes all three. */
    {
        static bool s_said_count = false;
        if (!s_said_count) {
            s_said_count = true;
            LOG_INFO(LOG_TAG, "ECS-UI: %d canvas(es) presented on first render",
                     canvases.count);
        }
    }
    if (canvases.count == 0) { JCE_PROFILE_ZONE_END; return; }
    uc_sort_canvases(scene, &canvases);

    UCFrame fr;
    fr.uc = uc; fr.scene = scene; fr.view_id = view_id;
    fr.hit_count = 0;
    uc->clip_log_count = 0;   /* the log describes THIS render, not the last */
    fr.ptr_x = pointer ? pointer->x : 0.0f;
    fr.ptr_y = pointer ? pointer->y : 0.0f;
    fr.ptr_valid = pointer && pointer->valid;
    fr.modal_active = false;
    fr.modal_owner   = 0;
    memset(&fr.modal_rect, 0, sizeof fr.modal_rect);
    uc->last_clicked = 0;
    uc->last_value_changed = 0;
    /* NOTE: last_submitted / last_text_changed are set OUTSIDE render (in
     * key_edit / text_input) and are cleared on read, so they are NOT reset
     * here — doing so would wipe a submit/edit delivered before this render. */

    /* Caret blink: ~1.6 Hz square wave driven by dt (only meaningful when a
     * field is focused; harmless otherwise).  Phase >= 0.5 ⇒ caret visible. */
    {
        float dt = dt_sec > 0 ? dt_sec : 0.0f;
        uc->caret_blink += dt * 1.6f;
        if (uc->caret_blink >= 1.0f) uc->caret_blink -= floorf(uc->caret_blink);
    }

    for (int i = 0; i < canvases.count; i++) {
        JceEntity canvas = canvases.list[i];
        JceCanvasComponent *cv = jce_scene_get_canvas(scene, canvas);
        if (!cv) continue;
        /* Disabled Canvas: skip layout/draw + its whole subtree. */
        if (!jce_scene_component_enabled(scene, canvas, JCE_COMP_FLAG_CANVAS)) continue;
        /* Screen-Space Overlay is the supported render mode.  World/Camera
         * canvases are laid out as overlays for now (documented partial).
         * CanvasScaler ("scale with screen size"): RectTransform anchors give
         * positional resolution-independence; the scale factor below makes the
         * px-sized parts (size_delta / font / borders) scale uniformly too. */
        fr.ui_scale      = 1.0f;
        fr.screen_h      = screen_h;
        fr.pixel_perfect = cv->pixel_perfect;
        /* The CanvasScaler applies in EVERY render mode.
         *
         * This used to be gated on OVERLAY, which made choosing Camera or
         * World in the Inspector worse than "not implemented": the canvas
         * still rasterised as a screen-space overlay (the root rect below is
         * the framebuffer unconditionally, and render_mode is read nowhere
         * else in this file), it just silently stopped being scaled.  A
         * control that quietly changes behaviour is worse than one that does
         * nothing.
         *
         * Camera- and World-space canvases are still NOT implemented -- see
         * the header, and the Inspector now says so at the point of choice
         * rather than leaving it to be discovered.  Measured: all 8 Canvas
         * components authored in this tree are OVERLAY, so nothing relies on
         * the other two, which is why the honest move is to say they are
         * unimplemented rather than to guess at an implementation.
         *
         * WORLD is excluded, and that exclusion is Unity's, not a hedge:
         * CanvasScaler dispatches on the mode and only World Space leaves the
         * Scale-With-Screen-Size path, because a world canvas is sized in
         * world units and scales through dynamicPixelsPerUnit /
         * referencePixelsPerUnit instead -- referenceResolution is genuinely
         * unused there.  The old gate excluded Camera AND World, so it was
         * right about World by accident and wrong about Camera. */
        if (cv->render_mode != JCE_CANVAS_WORLD &&
            cv->reference_resolution[0] > 0.0f &&
            cv->reference_resolution[1] > 0.0f) {
            /* ONE derivation, in jce_ui_canvas_scale_for -- see its header. */
            fr.ui_scale = jce_ui_canvas_scale_for(cv, screen_w, screen_h);
        }
        /* Canvas.scale_factor -- Unity's CanvasScaler "Constant Pixel Size"
         * factor, composed on top of the reference-resolution term exactly as
         * Unity composes scaleFactor with the scale mode.  It parsed, wrote and
         * showed in the Inspector but NOTHING read it, so authoring it did
         * nothing: leaving reference_resolution at 0 and setting scale_factor
         * -- the constant-pixel-size case -- gave a canvas that ignored both. */
        if (cv->scale_factor > 0.0f)
            fr.ui_scale *= cv->scale_factor;
        /* THE ROOT RECT IS THE ONE PLACE THE SAFE AREA ENTERS.  Everything
         * below lays out relative to it, so insetting here is the whole
         * feature -- and doing it per canvas is what keeps a full-bleed
         * background full-bleed while the button canvas above it clears the
         * notch.  ONE derivation, in jce_ui_canvas_root_rect; see its header. */
        UCRect root;
        jce_ui_canvas_root_rect(cv, screen_w, screen_h,
                                uc->safe_x, uc->safe_y, uc->safe_w, uc->safe_h,
                                &root.x, &root.y, &root.w, &root.h);
        uc_layout_draw(&fr, canvas, &root, 1.0f, /*blocks*/true,
                       /*interactable*/true, 0, /*clip*/NULL);
    }

    uc_update_buttons(&fr, pointer, dt_sec > 0 ? dt_sec : 0.0f);
    uc_update_widgets(&fr, pointer, dt_sec > 0 ? dt_sec : 0.0f);

    /* Publish the frame's hit list for the editor queries below. */
    uc->last_hit_count = fr.hit_count;
    for (int i = 0; i < fr.hit_count; i++) uc->last_hits[i] = fr.hits[i];
    uc->last_modal_rect   = fr.modal_rect;
    uc->last_modal_owner  = fr.modal_owner;
    uc->last_modal_active = fr.modal_active;
    uc->last_screen_w     = screen_w;
    uc->last_screen_h     = screen_h;

    JCE_PROFILE_ZONE_END;
}

uint64_t jce_ui_canvas_pick(const JceUICanvas *uc, float x, float y)
{
    if (!uc) return 0;
    /* Top-most = last recorded (drawn last -> on top), and an open popup
     * blocks what is under it, exactly as uc_update_buttons resolves hover.
     * The rects are already clipped to their ancestor ScrollView viewport, so
     * a scrolled-out row is not pickable -- which is the whole point of asking
     * the canvas instead of walking the scene. */
    for (int i = uc->last_hit_count - 1; i >= 0; i--) {
        const UCHit *h = &uc->last_hits[i];
        if (h->alpha <= 0.001f) continue;
        if (uc->last_modal_active && h->entity != uc->last_modal_owner &&
            uc_point_in(&uc->last_modal_rect, x, y)) continue;
        if (uc_point_in(&h->rect, x, y)) return (uint64_t)h->entity;
    }
    return 0;
}

int jce_ui_canvas_clip_log_count(const JceUICanvas *uc)
{
    return uc ? uc->clip_log_count : 0;
}

bool jce_ui_canvas_clip_log_at(const JceUICanvas *uc, int i,
                               float out_xywh[4], bool *out_active)
{
    if (!uc || i < 0 || i >= uc->clip_log_count) return false;
    if (out_active) *out_active = uc->clip_log[i].active;
    if (out_xywh) {
        out_xywh[0] = uc->clip_log[i].rect.x;
        out_xywh[1] = uc->clip_log[i].rect.y;
        out_xywh[2] = uc->clip_log[i].rect.w;
        out_xywh[3] = uc->clip_log[i].rect.h;
    }
    return true;
}

bool jce_ui_canvas_entity_rect(const JceUICanvas *uc, uint64_t entity,
                               float *out_x, float *out_y,
                               float *out_w, float *out_h)
{
    if (!uc || !entity) return false;
    for (int i = uc->last_hit_count - 1; i >= 0; i--) {
        if ((uint64_t)uc->last_hits[i].entity != entity) continue;
        const UCRect *r = &uc->last_hits[i].rect;
        if (out_x) *out_x = r->x;
        if (out_y) *out_y = r->y;
        if (out_w) *out_w = r->w;
        if (out_h) *out_h = r->h;
        return true;
    }
    return false;
}

bool jce_ui_canvas_entity_pivot(JceScene *scene, uint64_t entity,
                                float out_pivot2[2])
{
    if (!scene || !entity || !out_pivot2) return false;
    const JceRectTransform *rt = uc_entity_rect(scene, (JceEntity)entity);
    if (!rt) return false;
    out_pivot2[0] = rt->pivot[0];
    out_pivot2[1] = rt->pivot[1];
    return true;
}

void jce_ui_canvas_last_size(const JceUICanvas *uc, float *out_w, float *out_h)
{
    if (out_w) *out_w = uc ? uc->last_screen_w : 0.0f;
    if (out_h) *out_h = uc ? uc->last_screen_h : 0.0f;
}

uint64_t jce_ui_canvas_last_clicked(const JceUICanvas *uc)
{
    return uc ? uc->last_clicked : 0;
}

uint64_t jce_ui_canvas_last_value_changed(const JceUICanvas *uc)
{
    return uc ? uc->last_value_changed : 0;
}

uint64_t jce_ui_canvas_last_text_changed(JceUICanvas *uc)
{
    if (!uc) return 0;
    uint64_t e = uc->last_text_changed;   /* cleared on read (set outside render) */
    uc->last_text_changed = 0;
    return e;
}

/* ── InputField focus + edit channel ────────────────────────────────────
 *
 * The two delivery functions take only the canvas (mirroring the
 * fire-and-forget last_clicked/last_submitted API), so the canvas must know
 * WHICH scene holds the focused field.  jce_ui_canvas_render() stashes the
 * scene it last rendered in `uc->edit_scene`; that is the scene the focus was
 * established against, and the same one the caller feeds events for in the same
 * frame.  This is the single write-back point into the scene component (Unity
 * source-of-truth model, exactly like slider/toggle). */


float jce_ui_canvas_scale_for(const JceCanvasComponent *cv,
                              float screen_w, float screen_h)
{
    if (!cv) return 1.0f;
    if (cv->render_mode == JCE_CANVAS_WORLD) return 1.0f;
    if (cv->reference_resolution[0] <= 0.0f ||
        cv->reference_resolution[1] <= 0.0f) return 1.0f;

    const float rsx = screen_w / cv->reference_resolution[0];
    const float rsy = screen_h / cv->reference_resolution[1];
    if (!(rsx > 0.0f) || !(rsy > 0.0f)) return 1.0f;

    float m = cv->match_width_or_height;
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;

    /* EXACTLY 0.5 TAKES THE OLD PATH ON PURPOSE.  The pow() form is
     * mathematically the same there and is NOT bit-identical: a one-ULP
     * difference moves a layout by a pixel, and every scene written before
     * match_width_or_height existed parses to 0.5. */
    if (m == 0.5f) return sqrtf(rsx * rsy);
    return powf(rsx, 1.0f - m) * powf(rsy, m);
}

void jce_ui_canvas_root_rect(const JceCanvasComponent *cv,
                             float screen_w, float screen_h,
                             int safe_x, int safe_y, int safe_w, int safe_h,
                             float *out_x, float *out_y,
                             float *out_w, float *out_h)
{
    float x = 0.0f, y = 0.0f, w = screen_w, h = screen_h;

    /* THE SAFE AREA IS AN OPT-IN AND AN EMPTY RECT IS NOT ONE.  Both guards
     * fail toward the full drawable, which is what every scene did before
     * this existed: a canvas that silently shrank to nothing on a device
     * reporting a bad rect is a far worse failure than one that ignored the
     * notch there. */
    if (cv && cv->respect_safe_area && safe_w > 0 && safe_h > 0) {
        x = (float)safe_x;
        y = (float)safe_y;
        w = (float)safe_w;
        h = (float)safe_h;
    }

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

void jce_ui_canvas_set_safe_area(JceUICanvas *uc, int x, int y, int w, int h)
{
    if (!uc) return;
    /* A negative or empty rect is refused rather than stored: it would inset
     * every opted-in canvas to nothing, and "my UI vanished on one device" is
     * a much worse failure than "the safe area was ignored there". */
    if (w <= 0 || h <= 0) { uc->safe_w = 0; uc->safe_h = 0; return; }
    uc->safe_x = x < 0 ? 0 : x;
    uc->safe_y = y < 0 ? 0 : y;
    uc->safe_w = w;
    uc->safe_h = h;
}

void jce_ui_canvas_text_input(JceUICanvas *uc, const char *utf8)
{
    if (!uc || !uc->focused_input || !utf8 || !utf8[0] || !uc->edit_scene) return;
    JceUIInputFieldComponent *f =
        jce_scene_get_ui_input_field(uc->edit_scene, (JceEntity)uc->focused_input);
    if (!f) { uc->focused_input = 0; return; }
    /* Focused field disabled mid-edit -> ignore input (focus persists across frames). */
    { static int cid = -2; if (cid == -2) cid = jce_component_find("UIInputField");
      if (cid >= 0 && !jce_scene_comp_enabled(uc->edit_scene, (JceEntity)uc->focused_input, cid)) return; }
    if (f->read_only) return;

    /* TWO CAPS, IN TWO UNITS, because they are two different limits.
     *
     * The BUFFER is bytes -- text[256] is a fixed array and overrunning it is
     * memory corruption.  char_limit is CHARACTERS: jce_scene.h calls it 'max
     * chars', and it was enforced in bytes, so a limit of 4 admitted one
     * three-byte character and then the FIRST BYTE of the next one.  That is
     * not a short string, it is a broken one, and nothing that measures
     * length notices.
     *
     * Insertion is per character for the same reason: a byte-at-a-time loop
     * can stop between the bytes of one. */
    const int buf_cap  = (int)sizeof(f->text) - 1;
    const int char_cap = (f->char_limit > 0) ? f->char_limit : buf_cap;
    int chars = jce_utf8_count(f->text);
    int len = (int)strlen(f->text);
    int caret = uc->caret;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;

    const int in_len = (int)strlen(utf8);
    for (int i = 0; i < in_len; ) {
        const int next  = jce_utf8_next(utf8, i, in_len);
        const int clen  = next - i;
        const char lead = utf8[i];
        i = next;
        if (lead == '\n' || lead == '\r' || lead == '\t') continue; /* single-line */
        /* The filter sees the LEAD byte: for the ASCII content types a lead
         * byte >= 0x80 is correctly not a digit and not a letter. */
        if (!uc_if_accepts(f->content_type, lead, f->text, caret)) continue;
        if (chars >= char_cap) break;                  /* char_limit, in chars */
        if (len + clen > buf_cap) break;               /* buffer, in bytes */
        memmove(f->text + caret + clen, f->text + caret,
                (size_t)(len - caret + 1));
        memcpy(f->text + caret, utf8 + next - clen, (size_t)clen);
        caret += clen; len += clen; ++chars;
        uc->last_text_changed = uc->focused_input;     /* on_value_changed */
    }
    f->text[len] = '\0';
    uc->caret = caret;
    uc->caret_blink = 1.0f;                            /* re-show caret on edit */
}

void jce_ui_canvas_key_edit(JceUICanvas *uc, int scancode, uint16_t mod)
{
    (void)mod;
    if (!uc || !uc->focused_input || !uc->edit_scene) return;
    JceUIInputFieldComponent *f =
        jce_scene_get_ui_input_field(uc->edit_scene, (JceEntity)uc->focused_input);
    if (!f) { uc->focused_input = 0; return; }
    /* Focused field disabled mid-edit -> ignore key (focus persists across frames). */
    { static int cid = -2; if (cid == -2) cid = jce_component_find("UIInputField");
      if (cid >= 0 && !jce_scene_comp_enabled(uc->edit_scene, (JceEntity)uc->focused_input, cid)) return; }

    int len = (int)strlen(f->text);
    int caret = uc->caret;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;

    switch (scancode) {
        /* ONE CHARACTER, NOT ONE BYTE.  These four stepped by a byte, so
         * backspacing a three-byte character removed one third of it and
         * left two continuation bytes behind -- a corrupted string rather
         * than a wrong one, and invisible to anything that only counts
         * length.  jce_utf8_prev/next are in <jce/os/core/jce_str.h> rather
         * than here because a caret is not the only thing that steps. */
        case JCE_KEY_BACKSPACE:
            if (!f->read_only && caret > 0) {
                const int prev = jce_utf8_prev(f->text, caret);
                memmove(f->text + prev, f->text + caret,
                        (size_t)(len - caret + 1));
                caret = prev;
                uc->last_text_changed = uc->focused_input;  /* on_value_changed */
            }
            break;
        case JCE_KEY_DELETE:
            if (!f->read_only && caret < len) {
                const int next = jce_utf8_next(f->text, caret, len);
                memmove(f->text + caret, f->text + next,
                        (size_t)(len - next + 1));
                uc->last_text_changed = uc->focused_input;  /* on_value_changed */
            }
            break;
        case JCE_KEY_LEFT:
            caret = jce_utf8_prev(f->text, caret);
            break;
        case JCE_KEY_RIGHT:
            caret = jce_utf8_next(f->text, caret, len);
            break;
        case JCE_KEY_HOME:
            caret = 0;
            break;
        case JCE_KEY_END:
            caret = len;
            break;
        case JCE_KEY_RETURN:
        case JCE_KEY_KP_ENTER:
            uc->last_submitted = uc->focused_input;   /* commit; keep focus */
            break;
        case JCE_KEY_ESCAPE:
            uc->focused_input = 0;                     /* defocus */
            caret = 0;
            break;
        default:
            break;
    }
    uc->caret = caret;
    uc->caret_blink = 1.0f;
}

uint64_t jce_ui_canvas_focused_input(const JceUICanvas *uc)
{
    return uc ? uc->focused_input : 0;
}

uint64_t jce_ui_canvas_last_submitted(JceUICanvas *uc)
{
    if (!uc) return 0;
    uint64_t e = uc->last_submitted;      /* cleared on read (set outside render) */
    uc->last_submitted = 0;
    return e;
}

/* ── ScrollView wheel channel ────────────────────────────────────────────
 *
 * Applies a wheel delta to the scroll view under the pointer (tracked during
 * the most recent render's raycast pass, see uc_update_widgets).  Convention:
 * `dy` is +up (the JCE_EVENT_MOUSE_WHEEL sign); content scrolls UP when the
 * wheel rolls up, i.e. the offset DECREASES, so we apply -dy.  `dx` is +right;
 * scrolling right reveals content further right, increasing the offset, so we
 * apply +dx.  Per-axis enables + interactable are honoured, then each axis is
 * clamped to [0, max(0, content - viewport)] and the result written back into
 * the component's scroll_position (the single write-back, Unity source-of-truth
 * model, exactly like slider/toggle/input-field).  Runs fully headless — the
 * hovered scroll view + its viewport rect are cached from the last render. */
void jce_ui_canvas_scroll(JceUICanvas *uc, float dx, float dy)
{
    if (!uc || !uc->hovered_scroll || !uc->edit_scene) return;
    /* Hovered scroll view disabled -> ignore wheel (hover persists from last render). */
    { static int cid = -2; if (cid == -2) cid = jce_component_find("UIScrollView");
      if (cid >= 0 && !jce_scene_comp_enabled(uc->edit_scene, (JceEntity)uc->hovered_scroll, cid)) return; }
    if (dx == 0.0f && dy == 0.0f) return;

    JceUIScrollViewComponent *sv =
        jce_scene_get_ui_scroll_view(uc->edit_scene, (JceEntity)uc->hovered_scroll);
    if (!sv) { uc->hovered_scroll = 0; return; }
    if (!sv->interactable) return;

    float sens = (sv->scroll_sensitivity > 0.0f) ? sv->scroll_sensitivity : 30.0f;

    /* Wheel up (dy>0) scrolls content up ⇒ offset decreases. */
    if (sv->vertical)   sv->scroll_position[1] -= dy * sens;
    if (sv->horizontal) sv->scroll_position[0] += dx * sens;

    uc_scroll_clamp(sv, &uc->hovered_scroll_rect, uc->hovered_scroll_scale);
}
