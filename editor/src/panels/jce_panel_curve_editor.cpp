/*
 * jce_panel_curve_editor.cpp  Animation Curve Editor (Phase C).
 *
 * Phase C adds:
 *  - Multi-channel curves (RGBA / XYZ / scalar...) with per-channel
 *    color, name, visibility and active-channel selection.
 *  - Mouse-wheel zoom around the cursor (Shift = horizontal only,
 *    Ctrl = vertical only).  Middle-button drag pans the view.
 *  - "Frame All" button refits viewport to all visible keys.
 *  - File format extended with "channels": [...] while still reading
 *    Phase-A/B single-curve files (auto-wrapped into channel 0).
 */

#include "io/jce_editor_file_util.h"
#include "jce_panel_common.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <jce/resource/jce_curve.h>
}

namespace {

/* The interpolation modes and the key layout are the ENGINE'S, so the number
 * this panel writes into the file and the number the evaluator switches on
 * are one symbol rather than two that happen to agree today. */
enum InterpMode {
    IM_LINEAR   = JCE_CURVE_LINEAR,
    IM_CUBIC    = JCE_CURVE_CUBIC,
    IM_CONSTANT = JCE_CURVE_CONSTANT
};

/* A C struct has no member initialisers, so the defaults the panel relied on
 * move to make_curve_key().  Every construction site goes through it.
 * Named for the curve on purpose: jce_gizmo_compound_collider.cpp has an
 * anonymous-namespace function of the same short name that builds a CACHE
 * KEY STRING.  Two unrelated meanings under one name is what the dedup
 * audit is for, and internal linkage does not make it less confusing. */
typedef JceCurveKey Key;

inline Key make_curve_key(float t = 0.0f, float v = 0.0f, int interp = IM_LINEAR)
{
    Key k;
    k.t = t; k.v = v;
    k.tan_in = 0.0f; k.tan_out = 0.0f;
    k.interp = interp;
    return k;
}

struct Channel {
    char  name[32]   = "curve";
    float color[3]   = { 0.45f, 0.85f, 0.78f };
    bool  visible    = true;
    std::vector<Key> keys;
};

struct State {
    std::vector<Channel> channels;
    int   active        = 0;
    float t_min = 0.0f, t_max = 1.0f;
    float v_min = 0.0f, v_max = 1.0f;
    int   dragging      = -1;
    int   drag_handle   =  0;
    int   selected      = -1;
    char  path[260]     = {0};
    bool  initialised   = false;
    bool  panning       = false;
};

State s;

void seed_default(void)
{
    if (s.initialised) return;
    s.initialised = true;
    Channel ch;
    std::strncpy(ch.name, "value", sizeof(ch.name) - 1);
    Key a = make_curve_key(0.0f, 0.0f, IM_CUBIC);
    Key b = make_curve_key(0.5f, 1.0f, IM_CUBIC);
    Key c = make_curve_key(1.0f, 0.0f);
    ch.keys = { a, b, c };
    s.channels.push_back(ch);
}

Channel *active_chan(void)
{
    if (s.channels.empty()) return nullptr;
    if (s.active < 0 || s.active >= (int)s.channels.size()) s.active = 0;
    return &s.channels[s.active];
}

void sort_keys(Channel &ch)
{
    std::sort(ch.keys.begin(), ch.keys.end(),
              [](const Key &a, const Key &b) { return a.t < b.t; });
}

JceJson *channel_to_json(const Channel &ch)
{
    JceJson *o = jce_json_object();
    jce_json_set_string(o, "name", ch.name);
    jce_json_set_bool  (o, "visible", ch.visible);
    jce_json_set_float_array(o, "color", ch.color, 3);
    JceJson *arr = jce_json_array();
    for (auto &k : ch.keys) {
        JceJson *ko = jce_json_object();
        jce_json_set_number(ko, "t",       k.t);
        jce_json_set_number(ko, "v",       k.v);
        jce_json_set_number(ko, "tanIn",   k.tan_in);
        jce_json_set_number(ko, "tanOut",  k.tan_out);
        jce_json_set_int   (ko, "interp",  k.interp);
        jce_json_array_push(arr, ko);
    }
    jce_json_set_child(o, "keys", arr);
    return o;
}

/* The AUTHORING extras only.  The keys and the channel name come from
 * jce_curve_parse -- see load_curve.  What is left here is what the engine's
 * JceCurve deliberately does not carry: how the curve LOOKS in this panel,
 * which is not part of the function that was authored. */
void channel_extras_from_json(JceJson *o, Channel &ch)
{
    if (!o) return;
    ch.visible = jce_json_get_bool(o, "visible", true) != 0;
    jce_json_get_floats(o, "color", ch.color, 3, nullptr);
}

void save_curve(const char *path)
{
    JceJson *root = jce_json_object();
    jce_json_set_number(root, "tMin", s.t_min);
    jce_json_set_number(root, "tMax", s.t_max);
    jce_json_set_number(root, "vMin", s.v_min);
    jce_json_set_number(root, "vMax", s.v_max);
    jce_json_set_int   (root, "active", s.active);
    JceJson *arr = jce_json_array();
    for (auto &ch : s.channels)
        jce_json_array_push(arr, channel_to_json(ch));
    jce_json_set_child(root, "channels", arr);
    if (ed_write_json_to_file(path, root))
        jce_editor_console_log("curve saved: %s", path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "curve save failed: %s", path);
}

void load_curve(const char *path)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "curve load failed: %s", path);
        return;
    }
    /* ONE KEY READER.  jce_curve_parse owns t/v/tanIn/tanOut/interp, the
     * out-of-range interp clamp, the key sort and the pre-channels
     * back-compat -- so the curve this panel EDITS is parsed by the same code
     * that parses the curve a game PLAYS.  The JSON pass beside it reads only
     * what the engine's JceCurve does not carry: the view range and each
     * channel's appearance. */
    JceCurve *eng  = jce_curve_parse(buf, sz);
    JceJson  *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!eng || !root) {
        if (eng)  jce_curve_destroy(eng);
        if (root) jce_json_free(root);
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "curve load failed (not a curve document): %s", path);
        return;
    }
    s.t_min  = (float)jce_json_get_number(root, "tMin", 0.0);
    s.t_max  = (float)jce_json_get_number(root, "tMax", 1.0);
    s.v_min  = (float)jce_json_get_number(root, "vMin", 0.0);
    s.v_max  = (float)jce_json_get_number(root, "vMax", 1.0);
    s.active = jce_json_get_int(root, "active", 0);
    s.channels.clear();
    JceJson *chans = jce_json_get(root, "channels");
    /* No "channels" array means a pre-channels document; the engine wrapped
     * its top-level keys into channel 0, so the extras live on the root. */
    const bool wrapped = !(chans && jce_json_is_array(chans));
    const int  n       = jce_curve_channel_count(eng);
    for (int i = 0; i < n; ++i) {
        Channel ch;
        const char *nm = jce_curve_channel_name(eng, i);
        if (nm && nm[0]) {
            std::strncpy(ch.name, nm, sizeof(ch.name) - 1);
            ch.name[sizeof(ch.name) - 1] = 0;
        }
        const JceCurveKey *k  = jce_curve_channel_keys(eng, i);
        const int          kn = jce_curve_key_count(eng, i);
        if (k && kn > 0) ch.keys.assign(k, k + kn);
        /* Index correspondence is the parser's contract: it emits one channel
         * per array element even for a malformed one, so extras never slide
         * onto the wrong curve. */
        channel_extras_from_json(wrapped ? root : jce_json_array_at(chans, i),
                                 ch);
        s.channels.push_back(ch);
    }
    jce_curve_destroy(eng);
    if (s.channels.empty()) seed_default();
    if (s.active < 0 || s.active >= (int)s.channels.size()) s.active = 0;
    jce_json_free(root);
    s.selected = -1;
    jce_editor_console_log("curve loaded: %s (%d channels)",
                           path, (int)s.channels.size());
}

/* ONE EVALUATOR.  The body that used to live here is now
 * jce_curve_eval_keys in engine/src/resource/jce_curve.c, and the runtime
 * calls the same function -- so what this panel DRAWS is what a game PLAYS,
 * by construction rather than by two people keeping two copies in step. */
float eval_channel(const Channel &ch, float t)
{
    return jce_curve_eval_keys(ch.keys.empty() ? nullptr : ch.keys.data(),
                               (int)ch.keys.size(), t);
}

ImVec2 to_screen(ImVec2 p0, ImVec2 sz, float t, float v)
{
    float u = (t - s.t_min) / (s.t_max - s.t_min);
    float w = (v - s.v_min) / (s.v_max - s.v_min);
    return ImVec2(p0.x + u * sz.x,
                  p0.y + (1.0f - w) * sz.y);
}

void to_curve_space(ImVec2 p0, ImVec2 sz, ImVec2 m, float *out_t, float *out_v)
{
    float u = (m.x - p0.x) / sz.x;
    float w = (m.y - p0.y) / sz.y;
    *out_t = s.t_min + u * (s.t_max - s.t_min);
    *out_v = s.v_min + (1.0f - w) * (s.v_max - s.v_min);
}

float handle_dt(void) { return (s.t_max - s.t_min) * 0.08f; }

ImVec2 tangent_handle_pos(ImVec2 p0, ImVec2 sz, const Key &k, int side)
{
    float dt    = handle_dt() * (float)side;
    float slope = (side < 0) ? k.tan_in : k.tan_out;
    return to_screen(p0, sz, k.t + dt, k.v + dt * slope);
}

void zoom_around(ImVec2 p0, ImVec2 sz, ImVec2 anchor, float fx, float fy)
{
    /* Zoom factor < 1 zooms in; > 1 zooms out. anchor stays put. */
    float at, av;
    to_curve_space(p0, sz, anchor, &at, &av);
    s.t_min = at - (at - s.t_min) * fx;
    s.t_max = at + (s.t_max - at) * fx;
    s.v_min = av - (av - s.v_min) * fy;
    s.v_max = av + (s.v_max - av) * fy;
    if (s.t_max - s.t_min < 1e-4f) { s.t_max = s.t_min + 1e-4f; }
    if (s.v_max - s.v_min < 1e-4f) { s.v_max = s.v_min + 1e-4f; }
}

void frame_all(void)
{
    bool any = false;
    float tmn=0, tmx=1, vmn=0, vmx=1;
    for (auto &ch : s.channels) {
        if (!ch.visible) continue;
        for (auto &k : ch.keys) {
            if (!any) { tmn = tmx = k.t; vmn = vmx = k.v; any = true; }
            tmn = (k.t < tmn) ? k.t : tmn;
            tmx = (k.t > tmx) ? k.t : tmx;
            vmn = (k.v < vmn) ? k.v : vmn;
            vmx = (k.v > vmx) ? k.v : vmx;
        }
    }
    if (!any) return;
    float tp = (tmx - tmn) * 0.1f + 1e-3f;
    float vp = (vmx - vmn) * 0.1f + 1e-3f;
    s.t_min = tmn - tp; s.t_max = tmx + tp;
    s.v_min = vmn - vp; s.v_max = vmx + vp;
}

ImU32 chan_color(const Channel &ch, int alpha)
{
    int r = (int)(ch.color[0] * 255.0f);
    int g = (int)(ch.color[1] * 255.0f);
    int b = (int)(ch.color[2] * 255.0f);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return IM_COL32(r, g, b, alpha);
}

void draw_canvas(void)
{
    Channel *act = active_chan();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    if (sz.x < 200.0f) sz.x = 200.0f;
    if (sz.y < 200.0f) sz.y = 200.0f;
    ImVec2 p1 = ImVec2(p0.x + sz.x, p0.y + sz.y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, jce_theme::canvas_bg());
    for (int i = 0; i <= 10; ++i) {
        float x = p0.x + sz.x * (i / 10.0f);
        float y = p0.y + sz.y * (i / 10.0f);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), jce_theme::grid_minor());
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), jce_theme::grid_minor());
    }
    if (s.v_min < 0.0f && s.v_max > 0.0f) {
        ImVec2 z = to_screen(p0, sz, s.t_min, 0.0f);
        dl->AddLine(ImVec2(p0.x, z.y), ImVec2(p1.x, z.y),
                    jce_theme::grid_major(), 1.5f);
    }

    /* Draw all visible channels; active drawn last (on top) thicker. */
    auto draw_one = [&](const Channel &ch, float thickness, int alpha) {
        if (ch.keys.size() < 2) return;
        const int N = 240;
        ImVec2 prev = to_screen(p0, sz, s.t_min, eval_channel(ch, s.t_min));
        ImU32  col  = chan_color(ch, alpha);
        for (int i = 1; i <= N; ++i) {
            float t = s.t_min + (s.t_max - s.t_min) * (i / (float)N);
            ImVec2 cur = to_screen(p0, sz, t, eval_channel(ch, t));
            dl->AddLine(prev, cur, col, thickness);
            prev = cur;
        }
    };
    for (size_t i = 0; i < s.channels.size(); ++i) {
        if ((int)i == s.active) continue;
        if (!s.channels[i].visible) continue;
        draw_one(s.channels[i], 1.5f, 140);
    }
    if (act && act->visible) draw_one(*act, 2.5f, 255);

    ImGui::InvisibleButton("canvas", sz,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool canvas_hovered = ImGui::IsItemHovered();
    ImVec2 m = ImGui::GetIO().MousePos;

    /* Pan (middle button drag). */
    if (canvas_hovered && ImGui::IsMouseDragging(2)) s.panning = true;
    if (s.panning) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        float du = -d.x / sz.x;
        float dw =  d.y / sz.y;
        float tspan = s.t_max - s.t_min;
        float vspan = s.v_max - s.v_min;
        s.t_min += du * tspan; s.t_max += du * tspan;
        s.v_min += dw * vspan; s.v_max += dw * vspan;
        if (!ImGui::IsMouseDown(2)) s.panning = false;
    }

    /* Zoom (mouse wheel). */
    if (canvas_hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float f = (wheel > 0.0f) ? 0.85f : 1.18f;
            float fx = ImGui::GetIO().KeyCtrl  ? 1.0f : f;
            float fy = ImGui::GetIO().KeyShift ? 1.0f : f;
            zoom_around(p0, sz, m, fx, fy);
        }
    }

    /* Hit test only against active channel. */
    int   hit_key       = -1;
    int   hit_handle    =  0;
    float best_d2       = 100.0f;
    if (act) {
        for (size_t i = 0; i < act->keys.size(); ++i) {
            ImVec2 sp = to_screen(p0, sz, act->keys[i].t, act->keys[i].v);
            float dx = m.x - sp.x, dy = m.y - sp.y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) { best_d2 = d2; hit_key = (int)i; hit_handle = 0; }
        }
        if (s.selected >= 0 && s.selected < (int)act->keys.size()) {
            const Key &sk = act->keys[s.selected];
            if (sk.interp == IM_CUBIC || (s.selected > 0 &&
                act->keys[s.selected - 1].interp == IM_CUBIC)) {
                ImVec2 hin  = tangent_handle_pos(p0, sz, sk, -1);
                ImVec2 hout = tangent_handle_pos(p0, sz, sk, +1);
                float dx, dy, d2;
                dx = m.x - hin.x;  dy = m.y - hin.y;  d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; hit_key = s.selected; hit_handle = 1; }
                dx = m.x - hout.x; dy = m.y - hout.y; d2 = dx * dx + dy * dy;
                if (d2 < best_d2) { best_d2 = d2; hit_key = s.selected; hit_handle = 2; }
            }
        }
    }

    /* Render keys + tangents. */
    if (act) {
        ImU32 cdot = chan_color(*act, 255);
        for (size_t i = 0; i < act->keys.size(); ++i) {
            ImVec2 sp = to_screen(p0, sz, act->keys[i].t, act->keys[i].v);
            ImU32 col = ((int)i == s.selected) ? jce_theme::selection_outline() : cdot;
            dl->AddCircleFilled(sp, 5.0f, col);
            dl->AddCircle      (sp, 6.0f, jce_theme::node_outline(), 0, 1.5f);
        }
        if (s.selected >= 0 && s.selected < (int)act->keys.size()) {
            const Key &sk = act->keys[s.selected];
            ImVec2 sp = to_screen(p0, sz, sk.t, sk.v);
            bool has_in  = (s.selected > 0 &&
                           act->keys[s.selected - 1].interp == IM_CUBIC);
            bool has_out = (sk.interp == IM_CUBIC);
            if (has_in) {
                ImVec2 hin = tangent_handle_pos(p0, sz, sk, -1);
                dl->AddLine(sp, hin, IM_COL32(180, 120, 220, 255), 1.5f);
                dl->AddCircleFilled(hin, 4.0f, IM_COL32(200, 140, 240, 255));
            }
            if (has_out) {
                ImVec2 hout = tangent_handle_pos(p0, sz, sk, +1);
                dl->AddLine(sp, hout, IM_COL32(180, 120, 220, 255), 1.5f);
                dl->AddCircleFilled(hout, 4.0f, IM_COL32(200, 140, 240, 255));
            }
        }
    }

    /* Mouse interactions on active channel. */
    if (canvas_hovered && act) {
        if (ImGui::IsMouseClicked(0)) {
            if (hit_key >= 0) {
                s.dragging    = hit_key;
                s.drag_handle = hit_handle;
                s.selected    = hit_key;
            } else {
                float t, v;
                to_curve_space(p0, sz, m, &t, &v);
                Key k = make_curve_key(t, v, IM_CUBIC);
                act->keys.push_back(k);
                sort_keys(*act);
                for (size_t i = 0; i < act->keys.size(); ++i) {
                    if (act->keys[i].t == t && act->keys[i].v == v) {
                        s.dragging    = (int)i;
                        s.drag_handle = 0;
                        s.selected    = (int)i;
                        break;
                    }
                }
            }
        }
        if (ImGui::IsMouseClicked(1) && hit_key >= 0 && hit_handle == 0
            && act->keys.size() > 1) {
            act->keys.erase(act->keys.begin() + hit_key);
            s.dragging = -1;
            s.selected = -1;
        }
    }

    if (act && s.dragging >= 0 && s.dragging < (int)act->keys.size()) {
        if (ImGui::IsMouseDown(0)) {
            float t, v;
            to_curve_space(p0, sz, m, &t, &v);
            Key &k = act->keys[s.dragging];
            if (s.drag_handle == 0) {
                k.t = t; k.v = v;
            } else {
                float dt = t - k.t;
                if (s.drag_handle == 1 && dt > -1e-6f) dt = -1e-6f;
                if (s.drag_handle == 2 && dt <  1e-6f) dt =  1e-6f;
                float slope = (v - k.v) / dt;
                if (s.drag_handle == 1) k.tan_in  = slope;
                else                    k.tan_out = slope;
            }
        }
        if (ImGui::IsMouseReleased(0)) {
            if (s.drag_handle == 0) {
                int prev_id = s.dragging;
                float pt = act->keys[prev_id].t;
                float pv = act->keys[prev_id].v;
                sort_keys(*act);
                for (size_t i = 0; i < act->keys.size(); ++i) {
                    if (act->keys[i].t == pt && act->keys[i].v == pv) {
                        s.selected = (int)i; break;
                    }
                }
            }
            s.dragging = -1;
        }
    }

    char buf[96];
    std::snprintf(buf, sizeof(buf), "t [%.3f, %.3f]   wheel=zoom  MMB=pan",
                  s.t_min, s.t_max);
    dl->AddText(ImVec2(p0.x + 6.0f, p1.y - 18.0f),
                jce_theme::text_secondary(), buf);
    std::snprintf(buf, sizeof(buf), "v [%.3f, %.3f]", s.v_min, s.v_max);
    dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f),
                jce_theme::text_secondary(), buf);
}

void draw_selected_inspector(void)
{
    Channel *act = active_chan();
    if (!act || s.selected < 0 || s.selected >= (int)act->keys.size()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("curveEditor.label.noKey"));
        return;
    }
    Key &k = act->keys[s.selected];
    ImGui::Text(jce_editor_i18n("curveEditor.label.key"), s.selected, act->name);
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("curveEditor.button.flatten", "ce_flat"))) { k.tan_in = k.tan_out = 0.0f; }
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("curveEditor.button.auto", "ce_auto"))) {
        float slope = 0.0f;
        if (s.selected > 0 && s.selected + 1 < (int)act->keys.size()) {
            const Key &p = act->keys[s.selected - 1];
            const Key &n = act->keys[s.selected + 1];
            float dt = n.t - p.t;
            if (dt > 1e-6f) slope = (n.v - p.v) / dt;
        }
        k.tan_in = k.tan_out = slope;
    }
    ImGui::DragFloat(jce_editor_i18n_id("curveEditor.field.time",    "ce_t"),   &k.t, 0.01f);
    ImGui::DragFloat(jce_editor_i18n_id("curveEditor.field.value",   "ce_v"),   &k.v, 0.01f);
    ImGui::DragFloat(jce_editor_i18n_id("curveEditor.field.tanIn",   "ce_ti"),  &k.tan_in,  0.05f);
    ImGui::DragFloat(jce_editor_i18n_id("curveEditor.field.tanOut",  "ce_to"),  &k.tan_out, 0.05f);
    const char *modes[] = { jce_editor_i18n("curveEditor.interp.linear"), jce_editor_i18n("curveEditor.interp.cubic"), jce_editor_i18n("curveEditor.interp.constant") };
    int mi = k.interp;
    if (ImGui::Combo(jce_editor_i18n_id("curveEditor.field.interpOut", "ce_iout"), &mi, modes, IM_ARRAYSIZE(modes)))
        k.interp = mi;
}

void draw_channel_panel(void)
{
    if (ImGui::Button(jce_editor_i18n("curveEditor.button.addChannel"))) {
        Channel ch;
        std::snprintf(ch.name, sizeof(ch.name), "ch%d", (int)s.channels.size());
        float palette[6][3] = {
            { 0.92f, 0.36f, 0.36f }, { 0.45f, 0.85f, 0.45f },
            { 0.40f, 0.55f, 0.95f }, { 0.95f, 0.80f, 0.30f },
            { 0.85f, 0.45f, 0.95f }, { 0.45f, 0.90f, 0.90f },
        };
        int pi = (int)(s.channels.size() % 6);
        ch.color[0] = palette[pi][0];
        ch.color[1] = palette[pi][1];
        ch.color[2] = palette[pi][2];
        Key a = make_curve_key(0.0f, 0.0f);
        Key b = make_curve_key(1.0f, 0.0f);
        ch.keys = { a, b };
        s.channels.push_back(ch);
        s.active = (int)s.channels.size() - 1;
        s.selected = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("curveEditor.button.removeActive", "ce_rm")) && s.channels.size() > 1) {
        s.channels.erase(s.channels.begin() + s.active);
        if (s.active >= (int)s.channels.size()) s.active = (int)s.channels.size() - 1;
        s.selected = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("curveEditor.button.frameAll", "ce_frame"))) frame_all();

    if (ImGui::BeginTable("##chans", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(jce_editor_i18n("curveEditor.col.active"), ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("curveEditor.col.vis"),    ImGuiTableColumnFlags_WidthFixed, 38.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("curveEditor.col.color"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("curveEditor.col.name"));
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)s.channels.size(); ++i) {
            Channel &ch = s.channels[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool is_act = (i == s.active);
            if (ImGui::RadioButton("##act", is_act)) {
                s.active = i;
                s.selected = -1;
            }
            ImGui::TableNextColumn();
            ImGui::Checkbox("##vis", &ch.visible);
            ImGui::TableNextColumn();
            ImGui::ColorEdit3("##col", ch.color, ImGuiColorEditFlags_NoInputs);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##nm", ch.name, sizeof(ch.name));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void draw_content(void)
{
    seed_default();

    jce_draw_path_input(jce_editor_i18n_id("curveEditor.field.file", "ce_file"), s.path, sizeof(s.path), JcePathKind::FileAbs);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("curveEditor.button.save", "ce_save")) && s.path[0]) save_curve(s.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("curveEditor.button.load", "ce_load")) && s.path[0]) load_curve(s.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("curveEditor.button.clear", "ce_clr"))) {
        for (auto &ch : s.channels) {
            ch.keys.clear();
            Key a = make_curve_key(0.0f, 0.0f);
            Key b = make_curve_key(1.0f, 0.0f);
            ch.keys = { a, b };
        }
        s.selected = -1;
    }

    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("##tmin", &s.t_min, 0.01f, -1e6f, s.t_max - 0.001f, jce_editor_i18n("curveEditor.range.tMinFmt"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("##tmax", &s.t_max, 0.01f, s.t_min + 0.001f, 1e6f, jce_editor_i18n("curveEditor.range.tMaxFmt"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("##vmin", &s.v_min, 0.01f, -1e6f, s.v_max - 0.001f, jce_editor_i18n("curveEditor.range.vMinFmt"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("##vmax", &s.v_max, 0.01f, s.v_min + 0.001f, 1e6f, jce_editor_i18n("curveEditor.range.vMaxFmt"));

    Channel *act = active_chan();
    int n_keys = act ? (int)act->keys.size() : 0;
    ImGui::Text(jce_editor_i18n("curveEditor.label.activeChannel"),
                act ? act->name : "?", n_keys);

    if (ImGui::CollapsingHeader(jce_editor_i18n("curveEditor.section.channels"), ImGuiTreeNodeFlags_DefaultOpen))
        draw_channel_panel();

    if (ImGui::CollapsingHeader(jce_editor_i18n("curveEditor.section.selectedKey"), ImGuiTreeNodeFlags_DefaultOpen))
        draw_selected_inspector();

    ImGui::Separator();
    draw_canvas();
}

} /* namespace */

extern "C" void curve_editor_draw_content(void)
{
    draw_content();
}

extern "C" void jce_editor_panel_curve_editor(void)
{
    /* Shim: Curve Editor has been merged into the Animation Editor
     * workbench as a tab.  Activating this panel now redirects to
     * that workbench and requests the Curves tab.  Symbol kept so
     * menu/hotkey entries registered against JCE_PANEL_CURVE_EDITOR
     * keep working. */
    if (jce_panel_redirect_to_workbench(JCE_PANEL_CURVE_EDITOR,
                                        JCE_PANEL_ANIMATION_EDITOR,
                                        "animationEditor.title",
                                        "jce_anim_editor"))
        jce_panel_animation_editor_request_tab(2);
}
