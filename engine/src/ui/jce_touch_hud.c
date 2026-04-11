/*
 * jce_touch_hud.c  On-screen touch controls (MCBE-style).
 *
 * Layout (landscape):
 * ┌──────────────────────────────────────────────┐
 * │                                     [Pause]  │
 * │                                              │
 * │                                              │  ← right half: look drag
 * │                                              │
 * │   ┌───┐                          [Jump]      │
 * │   │ ○ │  joystick                [Crouch]    │
 * │   └───┘                                      │
 * └──────────────────────────────────────────────┘
 *
 * Touch coordinates from SDL3 are normalized [0, 1].
 * Zone assignment on FINGER_DOWN; tracked by finger ID.
 *
 * On desktop, F9 creates this HUD for debugging touch controls
 * with mouse emulation (left-click = finger).
 */

#include <jce/ui/jce_touch_hud.h>

#include <SDL3/SDL.h>
#include <string.h>
#include <math.h>

#include <jce/platform/jce_window.h>
#include <jce/platform/jce_input.h>
#include <jce/graphics/jce_renderer.h>
#include <jce/graphics/jce_primitives.h>
#include <jce/graphics/jce_text.h>
#include "core/jce_memory.h"

/* ── Configuration ──────────────────────────────────────────────── */

#define STICK_RADIUS_FRAC   0.08f   /* joystick radius as fraction of screen width */
#define STICK_DEAD_ZONE     0.15f   /* inner dead zone (0..1 of stick radius)      */
#define LOOK_SENSITIVITY    0.25f   /* look speed multiplier                       */
#define BTN_SIZE_FRAC       0.09f   /* button size as fraction of screen height    */
#define BTN_MARGIN_FRAC     0.03f   /* gap between buttons (fraction of screen)    */

/* Default joystick resting position (normalized). */
#define STICK_REST_X        0.12f
#define STICK_REST_Y        0.70f

/* Joystick visual: concentric ring count for "circle" approximation. */
#define STICK_RING_SEGMENTS 20

/* ── Touch zones ────────────────────────────────────────────────── */

typedef enum {
    ZONE_NONE = 0,
    ZONE_JOYSTICK,
    ZONE_LOOK,
    ZONE_BTN_PAUSE,
    ZONE_BTN_JUMP,
    ZONE_BTN_CROUCH,
    ZONE_BTN_MENU_0,
    ZONE_BTN_MENU_1
} TouchZone;

/* ── Per-finger tracking ────────────────────────────────────────── */

#define MAX_FINGERS 10

typedef struct {
    SDL_FingerID id;
    TouchZone    zone;
    float        start_x, start_y;  /* normalized touch-down position   */
    float        cur_x,   cur_y;    /* normalized current position      */
    float        prev_x,  prev_y;   /* previous frame position (look Δ) */
    bool         active;
} FingerSlot;

/* ── HUD state ──────────────────────────────────────────────────── */

struct JceTouchHud {
    JceRenderer *renderer;
    JceWindow   *window;
    JceFont     *label_font;

    FingerSlot   fingers[MAX_FINGERS];

    /* Joystick output [-1, 1]. */
    float        move_dx, move_dz;
    float        stick_anchor_x;    /* normalized anchor */
    float        stick_anchor_y;
    bool         stick_active;

    /* Look output (degrees accumulated this frame). */
    float        look_yaw, look_pitch;

    /* Button edge detection. */
    bool         btn_pressed[JCE_TOUCH_BTN_COUNT];  /* true for one frame  */
    bool         btn_down[JCE_TOUCH_BTN_COUNT];     /* currently held      */

    /* Desktop mouse emulation (F9 debug mode). */
    bool         mouse_emulating;   /* left button held = simulated finger */

    /* Menu mode: when paused, draw Continue/Quit instead of normal HUD. */
    bool         menu_mode;

    /* Visibility: false hides the overlay but keeps input processing active. */
    bool         visible;
};

/* ── Button rectangles (normalized coords [0,1]) ────────────────── */

static void get_btn_rect(JceTouchButton btn,
                          float *x0, float *y0, float *x1, float *y1)
{
    /* Button width adjusted for typical 16:9 aspect. */
    float bw = BTN_SIZE_FRAC * (9.0f / 16.0f);
    float bh = BTN_SIZE_FRAC;
    float margin = BTN_MARGIN_FRAC;

    switch (btn) {
    case JCE_TOUCH_BTN_PAUSE:
        /* Top-right corner. */
        *x0 = 1.0f - bw - margin;
        *y0 = margin;
        *x1 = *x0 + bw;
        *y1 = *y0 + bh;
        break;
    case JCE_TOUCH_BTN_JUMP:
        /* MCBE layout: right side, offset up and to the right from Crouch. */
        *x0 = 1.0f - bw - margin;
        *y0 = 1.0f - 2.0f * bh - 3.0f * margin;
        *x1 = *x0 + bw;
        *y1 = *y0 + bh;
        break;
    case JCE_TOUCH_BTN_CROUCH:
        /* MCBE layout: left-below Jump for separation. */
        *x0 = 1.0f - 2.0f * bw - 2.5f * margin;
        *y0 = 1.0f - bh - margin;
        *x1 = *x0 + bw;
        *y1 = *y0 + bh;
        break;
    case JCE_TOUCH_BTN_MENU_0:
        /* Continue: centred horizontally, at 40% screen height. */
        *x0 = 0.5f - bw * 1.5f;
        *y0 = 0.40f;
        *x1 = 0.5f + bw * 1.5f;
        *y1 = 0.40f + bh * 1.2f;
        break;
    case JCE_TOUCH_BTN_MENU_1:
        /* Quit: centred horizontally, below Continue. */
        *x0 = 0.5f - bw * 1.5f;
        *y0 = 0.40f + bh * 1.2f + margin * 2.0f;
        *x1 = 0.5f + bw * 1.5f;
        *y1 = 0.40f + bh * 2.4f + margin * 2.0f;
        break;
    default:
        *x0 = *y0 = *x1 = *y1 = 0;
        break;
    }
}

/* ── Zone detection from normalized touch position ──────────────── */

static TouchZone detect_zone(float nx, float ny, bool menu_mode)
{
    if (menu_mode) {
        /* In menu mode only Continue/Quit buttons are active. */
        for (int b = JCE_TOUCH_BTN_MENU_0; b <= JCE_TOUCH_BTN_MENU_1; b++) {
            float bx0, by0, bx1, by1;
            get_btn_rect((JceTouchButton)b, &bx0, &by0, &bx1, &by1);
            float pad = BTN_MARGIN_FRAC * 0.8f;
            if (nx >= bx0 - pad && nx <= bx1 + pad &&
                ny >= by0 - pad && ny <= by1 + pad) {
                return (b == JCE_TOUCH_BTN_MENU_0) ? ZONE_BTN_MENU_0
                                                    : ZONE_BTN_MENU_1;
            }
        }
        return ZONE_NONE;
    }

    /* Buttons have priority (small, precise targets). */
    for (int b = 0; b <= JCE_TOUCH_BTN_CROUCH; b++) {
        float bx0, by0, bx1, by1;
        get_btn_rect((JceTouchButton)b, &bx0, &by0, &bx1, &by1);
        /* Expand hit area for easier tapping. */
        float pad = BTN_MARGIN_FRAC * 0.8f;
        if (nx >= bx0 - pad && nx <= bx1 + pad &&
            ny >= by0 - pad && ny <= by1 + pad) {
            switch (b) {
            case JCE_TOUCH_BTN_PAUSE:  return ZONE_BTN_PAUSE;
            case JCE_TOUCH_BTN_JUMP:   return ZONE_BTN_JUMP;
            case JCE_TOUCH_BTN_CROUCH: return ZONE_BTN_CROUCH;
            default: break;
            }
        }
    }

    /* Left 40% of screen = joystick. */
    if (nx < 0.40f)
        return ZONE_JOYSTICK;

    /* Everything else on the right = look. */
    return ZONE_LOOK;
}

/* ── Finger management ──────────────────────────────────────────── */

static FingerSlot *find_finger(JceTouchHud *hud, SDL_FingerID id)
{
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (hud->fingers[i].active && hud->fingers[i].id == id)
            return &hud->fingers[i];
    }
    return NULL;
}

static FingerSlot *alloc_finger(JceTouchHud *hud)
{
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!hud->fingers[i].active)
            return &hud->fingers[i];
    }
    return NULL;
}

/* ── Create / Destroy ───────────────────────────────────────────── */

JceTouchHud *jce_touch_hud_create(JceRenderer *renderer, JceWindow *window,
                                   JceFont *label_font)
{
    JceTouchHud *hud = (JceTouchHud *)JCE_CALLOC(1, sizeof(*hud));
    if (!hud) return NULL;
    hud->renderer   = renderer;
    hud->window     = window;
    hud->label_font = label_font;
    hud->visible    = true;
    return hud;
}

void jce_touch_hud_destroy(JceTouchHud *hud)
{
    JCE_FREE(hud);
}

/* ── Update (call once per frame) ───────────────────────────────── */

void jce_touch_hud_update(JceTouchHud *hud, const JceInput *input, float dt_ms)
{
    if (!hud || !input) return;
    (void)dt_ms;

    /* Reset per-frame deltas. */
    hud->look_yaw   = 0;
    hud->look_pitch  = 0;

    /* Track which buttons are held this frame. */
    bool btn_down_now[JCE_TOUCH_BTN_COUNT] = {false};

    /* Mark all existing finger slots as "not seen yet this frame". */
    bool seen[MAX_FINGERS] = {false};

    /* ── Gather finger events: real touch + mouse emulation ── */

    /* Real touch fingers. */
    int touch_count = jce_input_touch_count(input);
    for (int t = 0; t < touch_count; t++) {
        SDL_FingerID fid;
        float fx, fy, fp;
        if (!jce_input_touch_get(input, t, &fid, &fx, &fy, &fp))
            continue;

        FingerSlot *slot = find_finger(hud, fid);
        if (slot) {
            slot->prev_x = slot->cur_x;
            slot->prev_y = slot->cur_y;
            slot->cur_x  = fx;
            slot->cur_y  = fy;
            seen[slot - hud->fingers] = true;
        } else {
            slot = alloc_finger(hud);
            if (!slot) continue;

            slot->active  = true;
            slot->id      = fid;
            slot->zone    = detect_zone(fx, fy, hud->menu_mode);
            slot->start_x = fx;
            slot->start_y = fy;
            slot->cur_x   = fx;
            slot->cur_y   = fy;
            slot->prev_x  = fx;
            slot->prev_y  = fy;
            seen[slot - hud->fingers] = true;

            if (slot->zone == ZONE_JOYSTICK) {
                hud->stick_anchor_x = fx;
                hud->stick_anchor_y = fy;
                hud->stick_active   = true;
            }
        }
    }

    /* Desktop mouse emulation: left-click acts as a virtual finger.
       Use a reserved finger ID that won't collide with real touch IDs. */
    {
        const SDL_FingerID MOUSE_FINGER_ID = (SDL_FingerID)(-99);
        bool mouse_down = jce_input_mouse_button(input, SDL_BUTTON_LEFT);

        if (mouse_down) {
            uint32_t ww, wh;
            jce_window_get_size(hud->window, &ww, &wh);

            float mx, my;
            jce_input_mouse_pos(input, &mx, &my);
            float nx = mx / (float)ww;
            float ny = my / (float)wh;

            FingerSlot *slot = find_finger(hud, MOUSE_FINGER_ID);
            if (slot) {
                slot->prev_x = slot->cur_x;
                slot->prev_y = slot->cur_y;
                slot->cur_x  = nx;
                slot->cur_y  = ny;
                seen[slot - hud->fingers] = true;
            } else {
                slot = alloc_finger(hud);
                if (slot) {
                    slot->active  = true;
                    slot->id      = MOUSE_FINGER_ID;
                    slot->zone    = detect_zone(nx, ny, hud->menu_mode);
                    slot->start_x = nx;
                    slot->start_y = ny;
                    slot->cur_x   = nx;
                    slot->cur_y   = ny;
                    slot->prev_x  = nx;
                    slot->prev_y  = ny;
                    seen[slot - hud->fingers] = true;

                    if (slot->zone == ZONE_JOYSTICK) {
                        hud->stick_anchor_x = nx;
                        hud->stick_anchor_y = ny;
                        hud->stick_active   = true;
                    }
                }
            }
            hud->mouse_emulating = true;
        } else if (hud->mouse_emulating) {
            /* Mouse released — remove the virtual finger. */
            FingerSlot *slot = find_finger(hud, MOUSE_FINGER_ID);
            if (slot) {
                if (slot->zone == ZONE_JOYSTICK) {
                    hud->stick_active = false;
                    hud->move_dx = 0;
                    hud->move_dz = 0;
                }
                slot->active = false;
            }
            hud->mouse_emulating = false;
        }
    }

    /* Release fingers no longer present in the input system. */
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (hud->fingers[i].active && !seen[i]) {
            if (hud->fingers[i].zone == ZONE_JOYSTICK) {
                hud->stick_active = false;
                hud->move_dx = 0;
                hud->move_dz = 0;
            }
            hud->fingers[i].active = false;
        }
    }

    /* Process active fingers by zone. */
    for (int i = 0; i < MAX_FINGERS; i++) {
        const FingerSlot *f = &hud->fingers[i];
        if (!f->active) continue;

        switch (f->zone) {
        case ZONE_JOYSTICK: {
            float dx = f->cur_x - hud->stick_anchor_x;
            float dy = f->cur_y - hud->stick_anchor_y;
            /* Normalize displacement by stick radius. */
            float radius = STICK_RADIUS_FRAC;
            float nx = dx / radius;
            float ny = dy / radius;
            float len = sqrtf(nx * nx + ny * ny);
            if (len > 1.0f) { nx /= len; ny /= len; }
            if (len < STICK_DEAD_ZONE) { nx = 0; ny = 0; }
            hud->move_dx = nx;
            hud->move_dz = ny;
            break;
        }
        case ZONE_LOOK: {
            float ldx = f->cur_x - f->prev_x;
            float ldy = f->cur_y - f->prev_y;
            hud->look_yaw   += ldx * LOOK_SENSITIVITY * 360.0f;
            hud->look_pitch  -= ldy * LOOK_SENSITIVITY * 360.0f;
            break;
        }
        case ZONE_BTN_PAUSE:
            btn_down_now[JCE_TOUCH_BTN_PAUSE] = true;
            break;
        case ZONE_BTN_JUMP:
            btn_down_now[JCE_TOUCH_BTN_JUMP] = true;
            break;
        case ZONE_BTN_CROUCH:
            btn_down_now[JCE_TOUCH_BTN_CROUCH] = true;
            break;
        case ZONE_BTN_MENU_0:
            btn_down_now[JCE_TOUCH_BTN_MENU_0] = true;
            break;
        case ZONE_BTN_MENU_1:
            btn_down_now[JCE_TOUCH_BTN_MENU_1] = true;
            break;
        default:
            break;
        }
    }

    /* Edge detection: "pressed" = down this frame, not previous. */
    for (int b = 0; b < JCE_TOUCH_BTN_COUNT; b++) {
        hud->btn_pressed[b] = btn_down_now[b] && !hud->btn_down[b];
        hud->btn_down[b]    = btn_down_now[b];
    }
}

/* ── Queries ────────────────────────────────────────────────────── */

void jce_touch_hud_get_move(const JceTouchHud *hud, float *dx, float *dz)
{
    if (!hud) { if (dx) *dx = 0; if (dz) *dz = 0; return; }
    if (dx) *dx = hud->move_dx;
    if (dz) *dz = hud->move_dz;
}

void jce_touch_hud_get_look(const JceTouchHud *hud, float *yaw, float *pitch)
{
    if (!hud) { if (yaw) *yaw = 0; if (pitch) *pitch = 0; return; }
    if (yaw)   *yaw   = hud->look_yaw;
    if (pitch) *pitch = hud->look_pitch;
}

bool jce_touch_hud_button(const JceTouchHud *hud, JceTouchButton btn)
{
    if (!hud || btn < 0 || btn >= JCE_TOUCH_BTN_COUNT) return false;
    return hud->btn_pressed[btn];
}

bool jce_touch_hud_button_down(const JceTouchHud *hud, JceTouchButton btn)
{
    if (!hud || btn < 0 || btn >= JCE_TOUCH_BTN_COUNT) return false;
    return hud->btn_down[btn];
}

/* ── Menu mode ──────────────────────────────────────────────────── */

void jce_touch_hud_set_menu_mode(JceTouchHud *hud, bool menu_mode)
{
    if (!hud) return;
    hud->menu_mode = menu_mode;
}

/* ── Drawing helpers ────────────────────────────────────────────── */

/* Draw a filled circle approximated by N triangles (fan). */
static void draw_filled_circle(const JceRenderer *r,
                                float cx, float cy, float radius,
                                uint32_t color, int segments)
{
    const float PI2 = 6.28318530f;
    for (int i = 0; i < segments; i++) {
        float a0 = PI2 * (float)i / (float)segments;
        float a1 = PI2 * (float)(i + 1) / (float)segments;

        /* Approximate each slice as a small filled rect (two triangles).
           Since we only have rect primitives, draw thin pie-slice rects
           from center. Instead, use overlapping rects in a cross pattern. */
        (void)a0; (void)a1;
    }

    /* Practical approach: draw a filled diamond (4 overlapping rects)
       to approximate a circle, which looks much better than one square. */
    float s  = radius;

    /* Core: largest inscribed square (45° rotated = same size). */
    float inner = radius * 0.82f;
    jce_draw_filled_rect(r, cx - inner, cy - inner, inner * 2, inner * 2, color);

    /* Top/bottom slivers to round out. */
    float ext = radius * 0.55f;
    jce_draw_filled_rect(r, cx - ext, cy - s, ext * 2, s - inner, color);
    jce_draw_filled_rect(r, cx - ext, cy + inner, ext * 2, s - inner, color);
    /* Left/right slivers. */
    jce_draw_filled_rect(r, cx - s, cy - ext, s - inner, ext * 2, color);
    jce_draw_filled_rect(r, cx + inner, cy - ext, s - inner, ext * 2, color);
}

/* Draw a circle outline as a ring of small rects. */
static void draw_circle_outline(const JceRenderer *r,
                                 float cx, float cy, float radius,
                                 float thickness, uint32_t color)
{
    const float PI2 = 6.28318530f;
    const int segs = 32;
    float half_t = thickness * 0.5f;

    for (int i = 0; i < segs; i++) {
        float a = PI2 * (float)i / (float)segs;
        float px = cx + cosf(a) * radius;
        float py = cy + sinf(a) * radius;
        jce_draw_filled_rect(r, px - half_t, py - half_t,
                             thickness, thickness, color);
    }
}

/* ── Drawing ────────────────────────────────────────────────────── */

void jce_touch_hud_set_visible(JceTouchHud *hud, bool visible)
{
    if (hud) hud->visible = visible;
}

bool jce_touch_hud_is_visible(const JceTouchHud *hud)
{
    return hud ? hud->visible : false;
}

void jce_touch_hud_draw(JceTouchHud *hud)
{
    if (!hud || !hud->visible) return;

    int lw, lh;
    jce_window_get_logical(hud->window, &lw, &lh);
    float sw = (float)lw;
    float sh = (float)lh;

    /* ── MCBE-style color palette ─────────────────────────── */

    /* Buttons: semi-transparent dark with lighter pressed state. */
    uint32_t col_btn          = jce_rgba( 80,  80,  80,  80); /* normal button   */
    uint32_t col_btn_pressed  = jce_rgba(200, 200, 200, 120); /* pressed button  */
    uint32_t col_btn_outline  = jce_rgba(160, 160, 160,  60); /* button outline  */

    /* ── Menu mode: draw Continue/Quit buttons only ───────── */

    if (hud->menu_mode) {
        static const char *menu_labels[] = { "Continue", "Quit" };
        static const JceTouchButton menu_btns[] = {
            JCE_TOUCH_BTN_MENU_0, JCE_TOUCH_BTN_MENU_1
        };
        for (int i = 0; i < 2; i++) {
            float bx0, by0, bx1, by1;
            get_btn_rect(menu_btns[i], &bx0, &by0, &bx1, &by1);

            bool held = hud->btn_down[menu_btns[i]];

            float px = bx0 * sw;
            float py = by0 * sh;
            float pw = (bx1 - bx0) * sw;
            float ph = (by1 - by0) * sh;

            /* Rounded-rect style menu button. */
            jce_draw_filled_rect(hud->renderer, px, py, pw, ph,
                                 held ? col_btn_pressed : col_btn);
            jce_draw_rect_outline(hud->renderer, px, py, pw, ph,
                                  col_btn_outline);

            if (hud->label_font) {
                float tw, th;
                jce_text_measure(hud->label_font, menu_labels[i], &tw, &th);
                float lx = px + (pw - tw) * 0.5f;
                float ly = py + (ph - th) * 0.5f;
                jce_text_draw(hud->renderer, hud->label_font,
                              lx, ly, menu_labels[i],
                              jce_rgba(255, 255, 255, held ? 255 : 200));
            }
        }
        return;
    }

    /* ── Normal HUD ───────────────────────────────────────── */

    /* Joystick: dark background ring + lighter knob. */
    uint32_t col_stick_bg     = jce_rgba( 60,  60,  60, 100); /* dark gray bg    */
    uint32_t col_stick_ring   = jce_rgba(180, 180, 180,  80); /* outline ring    */
    uint32_t col_stick_knob   = jce_rgba(200, 200, 200, 160); /* lighter knob    */
    uint32_t col_stick_knob_a = jce_rgba(255, 255, 255, 200); /* active knob     */

    /* ── Joystick ─────────────────────────────────────────── */

    float stick_r_px = STICK_RADIUS_FRAC * sw;

    /* Always show the resting joystick base (MCBE style). */
    float rest_cx = STICK_REST_X * sw;
    float rest_cy = STICK_REST_Y * sh;

    /* When active, the anchor follows the touch-down point. */
    float base_cx = hud->stick_active ? hud->stick_anchor_x * sw : rest_cx;
    float base_cy = hud->stick_active ? hud->stick_anchor_y * sh : rest_cy;

    /* Outer circle (background). */
    draw_filled_circle(hud->renderer, base_cx, base_cy,
                       stick_r_px, col_stick_bg, 0);

    /* Outline ring. */
    draw_circle_outline(hud->renderer, base_cx, base_cy,
                        stick_r_px, 2.0f, col_stick_ring);

    /* Inner knob. */
    float knob_r = stick_r_px * 0.40f;
    float kx = base_cx + hud->move_dx * stick_r_px * 0.8f;
    float ky = base_cy + hud->move_dz * stick_r_px * 0.8f;
    uint32_t knob_col = hud->stick_active ? col_stick_knob_a : col_stick_knob;
    draw_filled_circle(hud->renderer, kx, ky, knob_r, knob_col, 0);

    /* Knob outline. */
    draw_circle_outline(hud->renderer, kx, ky, knob_r,
                        1.5f, col_stick_ring);

    /* ── Action buttons (MCBE rounded look) ───────────────── */

    static const char *btn_labels[] = {
        "||",   /* Pause  */
        "Up",   /* Jump / Ascend  */
        "Dn"    /* Crouch / Descend */
    };

    for (int b = 0; b <= JCE_TOUCH_BTN_CROUCH; b++) {
        float bx0, by0, bx1, by1;
        get_btn_rect((JceTouchButton)b, &bx0, &by0, &bx1, &by1);

        bool held = hud->btn_down[b];

        float px = bx0 * sw;
        float py = by0 * sh;
        float pw = (bx1 - bx0) * sw;
        float ph = (by1 - by0) * sh;

        /* Button: circular if roughly square, else rounded rect. */
        float btn_cx = px + pw * 0.5f;
        float btn_cy = py + ph * 0.5f;
        float btn_r  = (pw < ph ? pw : ph) * 0.5f;

        /* Filled circle background. */
        draw_filled_circle(hud->renderer, btn_cx, btn_cy, btn_r,
                           held ? col_btn_pressed : col_btn, 0);
        /* Outline. */
        draw_circle_outline(hud->renderer, btn_cx, btn_cy, btn_r,
                            1.5f, col_btn_outline);

        /* Label centered in button. */
        if (hud->label_font) {
            float tw, th;
            jce_text_measure(hud->label_font, btn_labels[b], &tw, &th);
            float lx = btn_cx - tw * 0.5f;
            float ly = btn_cy - th * 0.5f;
            jce_text_draw(hud->renderer, hud->label_font,
                          lx, ly, btn_labels[b],
                          jce_rgba(255, 255, 255, held ? 255 : 180));
        }
    }
}
