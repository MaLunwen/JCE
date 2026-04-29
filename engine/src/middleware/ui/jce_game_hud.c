/*
 * jce_game_hud.c -- in-game HUD implementation.
 *
 * Drawing strategy: pure 2D primitives + bitmap font.  Layout is
 * resolution-aware via screen_w/screen_h passed in by caller.  All
 * geometry computed each frame — no caching — keeps code simple and
 * cheap (HUD has trivial draw cost compared to scene).
 */

#include <jce/middleware/ui/jce_game_hud.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_text.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "game_hud"

#define MAX_PICKUPS 8

typedef struct {
    char     text[64];
    float    age;
    float    lifetime;
    bool     used;
} PickupNotif;

struct JceGameHud {
    JceRenderer        *renderer;
    JceFont            *font;
    int                 sw, sh;

    bool                visible;
    bool                show_crosshair;

    JceGameHudState     state;
    char                weapon_name_buf[64];
    char                objective_buf[128];

    /* Transient effects. */
    float               hitmarker_t;        /* counts up from 0 */
    float               damage_angle;       /* rad */
    float               damage_intensity;   /* 0..1 */
    float               damage_t;
    PickupNotif         pickups[MAX_PICKUPS];
};

JceGameHud *jce_game_hud_create(const JceGameHudDesc *desc)
{
    if (!desc || !desc->renderer) return NULL;
    JceGameHud *h = (JceGameHud *)JCE_CALLOC(1, sizeof(*h));
    if (!h) return NULL;
    h->renderer       = desc->renderer;
    h->font           = desc->font;
    h->sw             = desc->screen_w > 0 ? desc->screen_w : 1280;
    h->sh             = desc->screen_h > 0 ? desc->screen_h : 720;
    h->visible        = true;
    h->show_crosshair = true;
    h->state.health   = 1.0f;
    h->state.armor    = 0.0f;
    return h;
}

void jce_game_hud_destroy(JceGameHud *h) { if (h) JCE_FREE(h); }

void jce_game_hud_resize(JceGameHud *h, int w, int h_)
{
    if (!h) return;
    if (w > 0) h->sw = w;
    if (h_ > 0) h->sh = h_;
}

void jce_game_hud_set_state(JceGameHud *h, const JceGameHudState *st)
{
    if (!h || !st) return;
    h->state = *st;
    /* Snapshot strings into internal buffers — caller may overwrite. */
    if (st->weapon_name) {
        strncpy(h->weapon_name_buf, st->weapon_name, sizeof(h->weapon_name_buf) - 1);
        h->weapon_name_buf[sizeof(h->weapon_name_buf)-1] = 0;
        h->state.weapon_name = h->weapon_name_buf;
    } else {
        h->weapon_name_buf[0] = 0;
        h->state.weapon_name = h->weapon_name_buf;
    }
    if (st->objective) {
        strncpy(h->objective_buf, st->objective, sizeof(h->objective_buf) - 1);
        h->objective_buf[sizeof(h->objective_buf)-1] = 0;
        h->state.objective = h->objective_buf;
    } else {
        h->objective_buf[0] = 0;
        h->state.objective = NULL;
    }
}

void jce_game_hud_trigger_hitmarker(JceGameHud *h)  { if (h) h->hitmarker_t = 0.001f; }
void jce_game_hud_trigger_damage(JceGameHud *h, float angle, float intensity)
{
    if (!h) return;
    h->damage_angle     = angle;
    h->damage_intensity = intensity > 1.0f ? 1.0f : (intensity < 0.0f ? 0.0f : intensity);
    h->damage_t         = 0.001f;
}
void jce_game_hud_show_pickup(JceGameHud *h, const char *label, float seconds)
{
    if (!h || !label) return;
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!h->pickups[i].used) {
            strncpy(h->pickups[i].text, label, sizeof(h->pickups[i].text) - 1);
            h->pickups[i].text[sizeof(h->pickups[i].text)-1] = 0;
            h->pickups[i].age      = 0.0f;
            h->pickups[i].lifetime = seconds > 0.0f ? seconds : 2.5f;
            h->pickups[i].used     = true;
            return;
        }
    }
    /* Recycle oldest. */
    int oldest = 0; float oldest_age = -1.0f;
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (h->pickups[i].age > oldest_age) { oldest_age = h->pickups[i].age; oldest = i; }
    }
    strncpy(h->pickups[oldest].text, label, sizeof(h->pickups[oldest].text) - 1);
    h->pickups[oldest].text[sizeof(h->pickups[oldest].text)-1] = 0;
    h->pickups[oldest].age      = 0.0f;
    h->pickups[oldest].lifetime = seconds > 0.0f ? seconds : 2.5f;
    h->pickups[oldest].used     = true;
}

void jce_game_hud_set_visible(JceGameHud *h, bool v)   { if (h) h->visible        = v; }
void jce_game_hud_set_crosshair(JceGameHud *h, bool v) { if (h) h->show_crosshair = v; }

void jce_game_hud_update(JceGameHud *h, float dt)
{
    if (!h) return;
    const float HIT_DUR    = 0.35f;
    const float DAMAGE_DUR = 0.8f;
    if (h->hitmarker_t > 0.0f) {
        h->hitmarker_t += dt;
        if (h->hitmarker_t > HIT_DUR) h->hitmarker_t = 0.0f;
    }
    if (h->damage_t > 0.0f) {
        h->damage_t += dt;
        if (h->damage_t > DAMAGE_DUR) h->damage_t = 0.0f;
    }
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!h->pickups[i].used) continue;
        h->pickups[i].age += dt;
        if (h->pickups[i].age >= h->pickups[i].lifetime) h->pickups[i].used = false;
    }
}

/* ---- draw helpers ---------------------------------------------------- */

static uint32_t hud_color_a(uint8_t r, uint8_t g, uint8_t b, float a01)
{
    if (a01 < 0.0f) a01 = 0.0f; if (a01 > 1.0f) a01 = 1.0f;
    return jce_rgba(r, g, b, (uint8_t)(a01 * 255.0f));
}

static void draw_bar(JceRenderer *r, float x, float y, float w, float hgt,
                     float fill, uint32_t fg, uint32_t bg)
{
    jce_draw_filled_rect(r, x, y, w, hgt, bg);
    if (fill > 0.0f) {
        if (fill > 1.0f) fill = 1.0f;
        jce_draw_filled_rect(r, x, y, w * fill, hgt, fg);
    }
    jce_draw_rect_outline(r, x, y, w, hgt, jce_rgba(0, 0, 0, 200));
}

static void draw_text(JceGameHud *h, float x, float y, const char *txt, uint32_t color)
{
    if (h->font && txt && txt[0]) jce_text_draw(h->renderer, h->font, x, y, txt, color);
}

/* ---- main draw ------------------------------------------------------- */

void jce_game_hud_draw(JceGameHud *h)
{
    if (!h || !h->visible) return;
    JceRenderer *r = h->renderer;
    const float W = (float)h->sw, H = (float)h->sh;

    /* ---- damage indicator (radial flash) ---- */
    if (h->damage_t > 0.0f) {
        float t = 1.0f - (h->damage_t / 0.8f);
        float a = h->damage_intensity * t * 0.7f;
        uint32_t col = hud_color_a(255, 30, 30, a);
        /* Simple full-screen flash strength scaled by indicator angle:
         * draw 4 triangle-ish bars at edges weighted by angle. */
        float ang = h->damage_angle;
        float w_top    = fmaxf(0.0f, cosf(ang)) * a;
        float w_bot    = fmaxf(0.0f, -cosf(ang)) * a;
        float w_right  = fmaxf(0.0f, sinf(ang)) * a;
        float w_left   = fmaxf(0.0f, -sinf(ang)) * a;
        const float B = 80.0f;
        if (w_top  > 0.01f) jce_draw_filled_rect(r, 0,     0,       W,           B * w_top * 4.0f,    hud_color_a(255,30,30,w_top));
        if (w_bot  > 0.01f) jce_draw_filled_rect(r, 0,     H - B*w_bot*4.0f, W,   B * w_bot * 4.0f,    hud_color_a(255,30,30,w_bot));
        if (w_left > 0.01f) jce_draw_filled_rect(r, 0,     0,       B * w_left*4.0f, H,                hud_color_a(255,30,30,w_left));
        if (w_right> 0.01f) jce_draw_filled_rect(r, W - B*w_right*4.0f, 0, B * w_right*4.0f, H,        hud_color_a(255,30,30,w_right));
        (void)col;
    }

    /* ---- crosshair ---- */
    if (h->show_crosshair) {
        const float cx = W * 0.5f, cy = H * 0.5f;
        const float L = 8.0f, T = 2.0f;
        uint32_t cc = jce_rgba(255, 255, 255, 200);
        jce_draw_filled_rect(r, cx - L,         cy - T*0.5f, L*2.0f, T, cc);
        jce_draw_filled_rect(r, cx - T*0.5f,    cy - L,      T,      L*2.0f, cc);
    }

    /* ---- hit marker (4 small bars) ---- */
    if (h->hitmarker_t > 0.0f) {
        float t = 1.0f - (h->hitmarker_t / 0.35f);
        uint32_t hc = hud_color_a(255, 255, 0, t);
        float cx = W * 0.5f, cy = H * 0.5f;
        float L = 12.0f, gap = 6.0f, T = 2.0f;
        jce_draw_filled_rect(r, cx - gap - L, cy - T*0.5f, L, T, hc);
        jce_draw_filled_rect(r, cx + gap,     cy - T*0.5f, L, T, hc);
        jce_draw_filled_rect(r, cx - T*0.5f, cy - gap - L, T, L, hc);
        jce_draw_filled_rect(r, cx - T*0.5f, cy + gap,     T, L, hc);
    }

    /* ---- bottom-left: health + armor bars ---- */
    {
        const float bw = 240.0f, bh = 14.0f, gap = 4.0f;
        const float bx = 24.0f, by = H - 24.0f - bh*2.0f - gap;
        draw_bar(r, bx, by,         bw, bh, h->state.health,
                 jce_rgba(220, 40, 40, 230), jce_rgba(40, 0, 0, 200));
        draw_bar(r, bx, by + bh + gap, bw, bh, h->state.armor,
                 jce_rgba(80, 160, 230, 230), jce_rgba(0, 20, 40, 200));
        if (h->font) {
            char buf[32];
            snprintf(buf, sizeof(buf), "HP %d", (int)(h->state.health * 100.0f));
            draw_text(h, bx + 6.0f, by - 2.0f, buf, jce_rgba(255, 255, 255, 255));
        }
    }

    /* ---- bottom-right: ammo + weapon name ---- */
    {
        const float pad = 24.0f;
        const float aw = 200.0f, ah = 60.0f;
        const float ax = W - pad - aw, ay = H - pad - ah;
        jce_draw_filled_rect(r, ax, ay, aw, ah, jce_rgba(0, 0, 0, 140));
        jce_draw_rect_outline(r, ax, ay, aw, ah, jce_rgba(255, 255, 255, 200));
        if (h->font) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d / %d", h->state.ammo_clip, h->state.ammo_reserve);
            draw_text(h, ax + 14.0f, ay + 8.0f,  buf, jce_rgba(255, 230, 80, 255));
            if (h->state.weapon_name && h->state.weapon_name[0]) {
                draw_text(h, ax + 14.0f, ay + 32.0f, h->state.weapon_name,
                          jce_rgba(220, 220, 220, 255));
            }
        }
    }

    /* ---- top-right: wanted-level dots ---- */
    if (h->state.wanted_level > 0) {
        const float pad = 24.0f;
        const float dot_w = 18.0f, dot_h = 18.0f, dgap = 6.0f;
        int n = h->state.wanted_level > 6 ? 6 : h->state.wanted_level;
        float total = (dot_w + dgap) * (float)n - dgap;
        float dx = W - pad - total;
        float dy = pad;
        for (int i = 0; i < n; i++) {
            jce_draw_filled_rect(r, dx + (dot_w + dgap) * (float)i, dy,
                                 dot_w, dot_h, jce_rgba(255, 200, 0, 255));
        }
    }

    /* ---- top-center: objective banner ---- */
    if (h->state.objective && h->state.objective[0] && h->font) {
        float tw = 0, th = 0;
        jce_text_measure(h->font, h->state.objective, &tw, &th);
        float bx = W * 0.5f - tw * 0.5f - 16.0f;
        float by = 24.0f;
        jce_draw_filled_rect(r, bx, by, tw + 32.0f, th + 12.0f, jce_rgba(0, 0, 0, 180));
        draw_text(h, bx + 16.0f, by + 6.0f, h->state.objective,
                  jce_rgba(255, 255, 255, 255));
    }

    /* ---- center-bottom: pickup notifications stack ---- */
    if (h->font) {
        float py = H - 160.0f;
        for (int i = 0; i < MAX_PICKUPS; i++) {
            if (!h->pickups[i].used) continue;
            float t  = h->pickups[i].age / h->pickups[i].lifetime;
            float a  = (t < 0.8f) ? 1.0f : 1.0f - (t - 0.8f) * 5.0f;
            if (a < 0.0f) a = 0.0f;
            uint32_t col = hud_color_a(255, 240, 160, a);
            float tw=0, th=0;
            jce_text_measure(h->font, h->pickups[i].text, &tw, &th);
            draw_text(h, W * 0.5f - tw * 0.5f, py, h->pickups[i].text, col);
            py -= th + 4.0f;
        }
    }

    /* ---- top-left: score (integer) ---- */
    if (h->font && h->state.score > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "$%u", h->state.score);
        draw_text(h, 24.0f, 24.0f, buf, jce_rgba(120, 240, 120, 255));
    }
}
