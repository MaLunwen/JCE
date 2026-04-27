/*
 * jce_editor_toast.cpp — transient on-screen notifications.
 */

#include "jce_editor_toast.h"

#include <jce/tools/jce_imgui.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TOAST_MAX        8
#define TOAST_LIFETIME   4.0f
#define TOAST_FADE       0.35f
#define TOAST_WIDTH      320.0f
#define TOAST_PAD_X      12.0f
#define TOAST_PAD_Y      8.0f
#define TOAST_GAP_Y      6.0f

typedef struct {
    JceToastLevel level;
    char          msg[256];
    float         age;     /* seconds since pushed */
    bool          alive;
} Toast;

static Toast s_toasts[TOAST_MAX];

static int find_free_slot(void)
{
    /* First try a dead slot. */
    for (int i = 0; i < TOAST_MAX; i++)
        if (!s_toasts[i].alive) return i;
    /* Otherwise evict the oldest. */
    int oldest = 0;
    for (int i = 1; i < TOAST_MAX; i++)
        if (s_toasts[i].age > s_toasts[oldest].age) oldest = i;
    return oldest;
}

extern "C" void jce_toast_push(JceToastLevel level, const char *fmt, ...)
{
    if (!fmt) return;
    int idx = find_free_slot();
    Toast *t = &s_toasts[idx];
    t->level = level;
    t->age   = 0.0f;
    t->alive = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->msg, sizeof(t->msg), fmt, ap);
    va_end(ap);
}

extern "C" void jce_toast_info(const char *fmt, ...)
{
    if (!fmt) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jce_toast_push(JCE_TOAST_INFO, "%s", buf);
}
extern "C" void jce_toast_success(const char *fmt, ...)
{
    if (!fmt) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jce_toast_push(JCE_TOAST_SUCCESS, "%s", buf);
}
extern "C" void jce_toast_warn(const char *fmt, ...)
{
    if (!fmt) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jce_toast_push(JCE_TOAST_WARNING, "%s", buf);
}
extern "C" void jce_toast_error(const char *fmt, ...)
{
    if (!fmt) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jce_toast_push(JCE_TOAST_ERROR, "%s", buf);
}

static void level_colors(JceToastLevel lv, ImU32 *bg, ImU32 *border, const char **icon)
{
    switch (lv) {
        case JCE_TOAST_SUCCESS:
            *bg     = IM_COL32(28,  90,  45,  235);
            *border = IM_COL32(80, 200, 120, 255);
            *icon   = "[OK]";
            break;
        case JCE_TOAST_WARNING:
            *bg     = IM_COL32(110, 80,  10,  235);
            *border = IM_COL32(240, 180, 40,  255);
            *icon   = "[!]";
            break;
        case JCE_TOAST_ERROR:
            *bg     = IM_COL32(120, 30,  35,  235);
            *border = IM_COL32(235, 80,  85,  255);
            *icon   = "[X]";
            break;
        default: /* INFO */
            *bg     = IM_COL32(40,  60,  90,  235);
            *border = IM_COL32(80, 150, 220, 255);
            *icon   = "[i]";
            break;
    }
}

extern "C" void jce_editor_toast_draw(void)
{
    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.5f) dt = 0.5f;       /* clamp huge deltas after stalls */

    /* Age out toasts. */
    for (int i = 0; i < TOAST_MAX; i++) {
        if (!s_toasts[i].alive) continue;
        s_toasts[i].age += dt;
        if (s_toasts[i].age > TOAST_LIFETIME + TOAST_FADE)
            s_toasts[i].alive = false;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float right_edge  = vp->WorkPos.x + vp->WorkSize.x - 16.0f;
    float bottom_edge = vp->WorkPos.y + vp->WorkSize.y - 16.0f;

    int draw_idx = 0;
    float y_cursor = bottom_edge;

    for (int i = 0; i < TOAST_MAX; i++) {
        if (!s_toasts[i].alive) continue;
        Toast *t = &s_toasts[i];

        /* Compute alpha based on age. */
        float alpha = 1.0f;
        if (t->age < TOAST_FADE)
            alpha = t->age / TOAST_FADE;
        else if (t->age > TOAST_LIFETIME)
            alpha = 1.0f - (t->age - TOAST_LIFETIME) / TOAST_FADE;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        ImU32 bg, border;
        const char *icon;
        level_colors(t->level, &bg, &border, &icon);

        /* Measure text wrapped to TOAST_WIDTH-2*pad. */
        ImVec2 text_sz = ImGui::CalcTextSize(t->msg, NULL, false,
                                             TOAST_WIDTH - 2.0f * TOAST_PAD_X - 30.0f);
        if (text_sz.y < 18.0f) text_sz.y = 18.0f;
        float h = text_sz.y + 2.0f * TOAST_PAD_Y;

        y_cursor -= h;
        ImVec2 a(right_edge - TOAST_WIDTH, y_cursor);
        ImVec2 b(right_edge, y_cursor + h);

        /* Apply alpha. */
        auto fade = [&](ImU32 c) {
            int aa = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
            return (c & ~(0xFFu << IM_COL32_A_SHIFT))
                 | ((ImU32)aa << IM_COL32_A_SHIFT);
        };

        ImDrawList *dl = ImGui::GetForegroundDrawList((ImGuiViewport *)vp);
        dl->AddRectFilled(a, b, fade(bg), 6.0f);
        dl->AddRect      (a, b, fade(border), 6.0f, 0, 1.5f);

        /* Icon. */
        ImVec2 ip(a.x + TOAST_PAD_X, a.y + TOAST_PAD_Y);
        dl->AddText(ip, fade(IM_COL32(255, 255, 255, 255)), icon);

        /* Message — wrapped manually using AddText with wrap_width. */
        ImVec2 tp(a.x + TOAST_PAD_X + 30.0f, a.y + TOAST_PAD_Y);
        dl->AddText(NULL, 0.0f, tp, fade(IM_COL32(245, 245, 245, 255)),
                    t->msg, NULL, TOAST_WIDTH - 2.0f * TOAST_PAD_X - 30.0f);

        y_cursor -= TOAST_GAP_Y;
        if (++draw_idx >= TOAST_MAX) break;
    }
}
