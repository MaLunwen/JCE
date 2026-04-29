/*
 * jce_panel_animator_sm.cpp -- Animator State Machine editor (P1-K).
 *
 * Inspired by Unity's Animator window.  Lets users author a finite-state
 * machine where each state references an animation clip, edges define
 * transitions guarded by parameter conditions, and a parameter table
 * holds runtime values (Float / Int / Bool / Trigger).
 *
 * Features:
 *  - Drag states on a 2D canvas (background grid, pan with MMB, zoom Ctrl+wheel).
 *  - Right-click empty canvas → "New State" context menu.
 *  - Right-click on a state → Make Default / Add Transition From / Delete.
 *  - "Add Transition" mode: pick source then destination state.
 *  - Selected state inspector: name, clip path, speed, looping.
 *  - Selected transition inspector: duration, condition list (param/op/value).
 *  - Parameter table with add / remove / type switch.
 *  - JSON save/load (.anim_sm.json) + Console logging.
 *
 * The editor is purely authoring; runtime evaluation is delegated to the
 * engine animation system.  The on-disk format is documented inline.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"

#include <jce/tools/jce_imgui.hpp>
extern "C" {
#include <jce/os/core/jce_json.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

enum ParamType { PT_FLOAT = 0, PT_INT = 1, PT_BOOL = 2, PT_TRIGGER = 3 };
enum CondOp    { CO_GT = 0, CO_LT = 1, CO_EQ = 2, CO_NEQ = 3, CO_TRUE = 4, CO_FALSE = 5 };

struct Param {
    char  name[48] = "param";
    int   type     = PT_FLOAT;
    float def_f    = 0.0f;
    int   def_i    = 0;
    int   def_b    = 0;
};

struct Condition {
    int   param_idx = 0;
    int   op        = CO_GT;
    float threshold = 0.0f;
};

struct Transition {
    int   from        = -1;
    int   to          = -1;
    float duration    = 0.25f;
    bool  has_exit    = false;
    float exit_time   = 1.0f;
    std::vector<Condition> conds;
};

struct State {
    char  name[48]   = "State";
    char  clip_path[260] = {0};
    float speed      = 1.0f;
    bool  looping    = true;
    ImVec2 pos       = ImVec2(120, 120);
};

struct Editor {
    std::vector<State>      states;
    std::vector<Transition> trans;
    std::vector<Param>      params;
    int   default_state  = -1;
    int   sel_state      = -1;
    int   sel_trans      = -1;
    int   dragging_state = -1;
    ImVec2 drag_offset   = ImVec2(0, 0);
    int   adding_from    = -1;     /* >=0 means waiting to pick destination */
    ImVec2 pan           = ImVec2(0, 0);
    float zoom           = 1.0f;
    bool  initialised    = false;
    char  path[260]      = {0};
};

Editor g;

const char *param_type_name(int t)
{
    switch (t) {
        case PT_FLOAT:   return "Float";
        case PT_INT:     return "Int";
        case PT_BOOL:    return "Bool";
        case PT_TRIGGER: return "Trigger";
    }
    return "?";
}

const char *cond_op_name(int op)
{
    switch (op) {
        case CO_GT:   return ">";
        case CO_LT:   return "<";
        case CO_EQ:   return "==";
        case CO_NEQ:  return "!=";
        case CO_TRUE: return "true";
        case CO_FALSE:return "false";
    }
    return "?";
}

void seed_default(void)
{
    if (g.initialised) return;
    g.initialised = true;
    State idle;   std::strncpy(idle.name,  "Idle", sizeof(idle.name) - 1);
                  idle.pos = ImVec2(140, 140);
    State move;   std::strncpy(move.name,  "Move", sizeof(move.name) - 1);
                  move.pos = ImVec2(380, 220);
    g.states.push_back(idle);
    g.states.push_back(move);
    g.default_state = 0;
    Param p; std::strncpy(p.name, "Speed", sizeof(p.name) - 1);
    g.params.push_back(p);
    Transition t; t.from = 0; t.to = 1; t.duration = 0.2f;
    Condition c; c.param_idx = 0; c.op = CO_GT; c.threshold = 0.1f;
    t.conds.push_back(c);
    g.trans.push_back(t);
    Transition t2; t2.from = 1; t2.to = 0; t2.duration = 0.2f;
    Condition c2; c2.param_idx = 0; c2.op = CO_LT; c2.threshold = 0.1f;
    t2.conds.push_back(c2);
    g.trans.push_back(t2);
}

ImVec2 to_screen(const ImVec2 &origin, const ImVec2 &world)
{
    return ImVec2(origin.x + (world.x + g.pan.x) * g.zoom,
                  origin.y + (world.y + g.pan.y) * g.zoom);
}

ImVec2 to_world(const ImVec2 &origin, const ImVec2 &screen)
{
    return ImVec2((screen.x - origin.x) / g.zoom - g.pan.x,
                  (screen.y - origin.y) / g.zoom - g.pan.y);
}

JceJson *to_json(void)
{
    JceJson *root = jce_json_object();

    JceJson *params = jce_json_array();
    for (auto &p : g.params) {
        JceJson *o = jce_json_object();
        jce_json_set_string(o, "name", p.name);
        jce_json_set_int   (o, "type", p.type);
        jce_json_set_number(o, "defF", p.def_f);
        jce_json_set_int   (o, "defI", p.def_i);
        jce_json_set_int   (o, "defB", p.def_b);
        jce_json_array_push(params, o);
    }
    jce_json_set_child(root, "params", params);

    JceJson *states = jce_json_array();
    for (auto &s : g.states) {
        JceJson *o = jce_json_object();
        jce_json_set_string(o, "name", s.name);
        jce_json_set_string(o, "clip", s.clip_path);
        jce_json_set_number(o, "speed", s.speed);
        jce_json_set_bool  (o, "loop",  s.looping);
        jce_json_set_number(o, "x",     s.pos.x);
        jce_json_set_number(o, "y",     s.pos.y);
        jce_json_array_push(states, o);
    }
    jce_json_set_child(root, "states", states);

    JceJson *trans = jce_json_array();
    for (auto &t : g.trans) {
        JceJson *o = jce_json_object();
        jce_json_set_int   (o, "from", t.from);
        jce_json_set_int   (o, "to",   t.to);
        jce_json_set_number(o, "duration", t.duration);
        jce_json_set_bool  (o, "hasExit",  t.has_exit);
        jce_json_set_number(o, "exitTime", t.exit_time);
        JceJson *carr = jce_json_array();
        for (auto &c : t.conds) {
            JceJson *co = jce_json_object();
            jce_json_set_int   (co, "param", c.param_idx);
            jce_json_set_int   (co, "op",    c.op);
            jce_json_set_number(co, "thr",   c.threshold);
            jce_json_array_push(carr, co);
        }
        jce_json_set_child(o, "conds", carr);
        jce_json_array_push(trans, o);
    }
    jce_json_set_child(root, "transitions", trans);
    jce_json_set_int(root, "default", g.default_state);

    return root;
}

void from_json(JceJson *root)
{
    g.params.clear();
    g.states.clear();
    g.trans.clear();
    g.default_state = -1;

    JceJson *params = jce_json_get(root, "params");
    if (params && jce_json_is_array(params)) {
        int n = jce_json_array_size(params);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(params, i);
            if (!o) continue;
            Param p;
            const char *nm = jce_json_get_string(o, "name", "param");
            std::strncpy(p.name, nm ? nm : "param", sizeof(p.name) - 1);
            p.type  = jce_json_get_int(o, "type", PT_FLOAT);
            p.def_f = (float)jce_json_get_number(o, "defF", 0.0);
            p.def_i = jce_json_get_int(o, "defI", 0);
            p.def_b = jce_json_get_int(o, "defB", 0);
            g.params.push_back(p);
        }
    }
    JceJson *states = jce_json_get(root, "states");
    if (states && jce_json_is_array(states)) {
        int n = jce_json_array_size(states);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(states, i);
            if (!o) continue;
            State s;
            const char *nm = jce_json_get_string(o, "name", "State");
            std::strncpy(s.name, nm ? nm : "State", sizeof(s.name) - 1);
            const char *cp = jce_json_get_string(o, "clip", "");
            std::strncpy(s.clip_path, cp ? cp : "", sizeof(s.clip_path) - 1);
            s.speed   = (float)jce_json_get_number(o, "speed", 1.0);
            s.looping = jce_json_get_bool(o, "loop", 1) != 0;
            s.pos.x   = (float)jce_json_get_number(o, "x", 120.0);
            s.pos.y   = (float)jce_json_get_number(o, "y", 120.0);
            g.states.push_back(s);
        }
    }
    JceJson *trans = jce_json_get(root, "transitions");
    if (trans && jce_json_is_array(trans)) {
        int n = jce_json_array_size(trans);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(trans, i);
            if (!o) continue;
            Transition t;
            t.from      = jce_json_get_int(o, "from", -1);
            t.to        = jce_json_get_int(o, "to",   -1);
            t.duration  = (float)jce_json_get_number(o, "duration", 0.25);
            t.has_exit  = jce_json_get_bool(o, "hasExit", 0) != 0;
            t.exit_time = (float)jce_json_get_number(o, "exitTime", 1.0);
            JceJson *carr = jce_json_get(o, "conds");
            if (carr && jce_json_is_array(carr)) {
                int cn = jce_json_array_size(carr);
                for (int j = 0; j < cn; ++j) {
                    JceJson *co = jce_json_array_at(carr, j);
                    if (!co) continue;
                    Condition c;
                    c.param_idx = jce_json_get_int(co, "param", 0);
                    c.op        = jce_json_get_int(co, "op",    CO_GT);
                    c.threshold = (float)jce_json_get_number(co, "thr", 0.0);
                    t.conds.push_back(c);
                }
            }
            g.trans.push_back(t);
        }
    }
    g.default_state = jce_json_get_int(root, "default", -1);
    g.sel_state = -1;
    g.sel_trans = -1;
}

void save_to(const char *path)
{
    if (ed_write_json_to_file(path, to_json()))
        jce_editor_console_log("animator-sm saved: %s", path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "animator-sm save failed: %s", path);
}

void load_from(const char *path)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "animator-sm load failed: %s", path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "animator-sm parse failed: %s", path);
        return;
    }
    from_json(root);
    jce_json_free(root);
    jce_editor_console_log("animator-sm loaded: %s (states=%d trans=%d)",
                           path, (int)g.states.size(), (int)g.trans.size());
}

void delete_state(int idx)
{
    if (idx < 0 || idx >= (int)g.states.size()) return;
    g.states.erase(g.states.begin() + idx);
    /* Remove or remap transitions. */
    for (int i = (int)g.trans.size() - 1; i >= 0; --i) {
        Transition &t = g.trans[i];
        if (t.from == idx || t.to == idx) {
            g.trans.erase(g.trans.begin() + i);
            continue;
        }
        if (t.from > idx) --t.from;
        if (t.to   > idx) --t.to;
    }
    if (g.default_state == idx) g.default_state = g.states.empty() ? -1 : 0;
    else if (g.default_state > idx) --g.default_state;
    g.sel_state = -1;
    g.sel_trans = -1;
}

void delete_param(int idx)
{
    if (idx < 0 || idx >= (int)g.params.size()) return;
    g.params.erase(g.params.begin() + idx);
    for (auto &t : g.trans) {
        for (int i = (int)t.conds.size() - 1; i >= 0; --i) {
            if (t.conds[i].param_idx == idx)
                t.conds.erase(t.conds.begin() + i);
            else if (t.conds[i].param_idx > idx)
                --t.conds[i].param_idx;
        }
    }
}

/* ───── Canvas drawing ─────────────────────────────────────────── */

void draw_grid(ImDrawList *dl, ImVec2 origin, ImVec2 size)
{
    const ImU32 col_bg   = jce_theme::canvas_bg();
    const ImU32 col_grid = jce_theme::grid_minor();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), col_bg);
    float step = 32.0f * g.zoom;
    if (step < 8.0f) step = 8.0f;
    float ox = std::fmod(g.pan.x * g.zoom, step);
    float oy = std::fmod(g.pan.y * g.zoom, step);
    for (float x = ox; x < size.x; x += step)
        dl->AddLine(ImVec2(origin.x + x, origin.y),
                    ImVec2(origin.x + x, origin.y + size.y), col_grid);
    for (float y = oy; y < size.y; y += step)
        dl->AddLine(ImVec2(origin.x, origin.y + y),
                    ImVec2(origin.x + size.x, origin.y + y), col_grid);
}

bool point_in_state(const State &s, const ImVec2 &origin, const ImVec2 &mp,
                    ImVec2 *out_node_min = nullptr, ImVec2 *out_node_max = nullptr)
{
    ImVec2 a = to_screen(origin, s.pos);
    ImVec2 b = ImVec2(a.x + 130.0f * g.zoom, a.y + 46.0f * g.zoom);
    if (out_node_min) *out_node_min = a;
    if (out_node_max) *out_node_max = b;
    return mp.x >= a.x && mp.x <= b.x && mp.y >= a.y && mp.y <= b.y;
}

void draw_arrow(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness)
{
    dl->AddLine(a, b, col, thickness);
    ImVec2 d = ImVec2(b.x - a.x, b.y - a.y);
    float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 0.001f) return;
    d.x /= len; d.y /= len;
    ImVec2 perp(-d.y, d.x);
    float head = 10.0f;
    ImVec2 tip = b;
    ImVec2 base = ImVec2(b.x - d.x * head, b.y - d.y * head);
    ImVec2 lh = ImVec2(base.x + perp.x * head * 0.5f, base.y + perp.y * head * 0.5f);
    ImVec2 rh = ImVec2(base.x - perp.x * head * 0.5f, base.y - perp.y * head * 0.5f);
    dl->AddTriangleFilled(tip, lh, rh, col);
}

void draw_canvas(void)
{
    ImGui::BeginChild("##sm_canvas", ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size   = ImGui::GetContentRegionAvail();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    draw_grid(dl, origin, size);

    ImGui::InvisibleButton("##sm_canvas_btn", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool active  = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    ImVec2 mp    = ImGui::GetIO().MousePos;

    /* Pan with MMB drag. */
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        g.pan.x += d.x / g.zoom;
        g.pan.y += d.y / g.zoom;
    }
    /* Zoom with Ctrl+wheel. */
    if (hovered && ImGui::GetIO().KeyCtrl) {
        float w = ImGui::GetIO().MouseWheel;
        if (w != 0.0f) {
            ImVec2 wp_before = to_world(origin, mp);
            g.zoom *= (w > 0.0f ? 1.1f : 1.0f / 1.1f);
            if (g.zoom < 0.25f) g.zoom = 0.25f;
            if (g.zoom > 3.0f)  g.zoom = 3.0f;
            ImVec2 wp_after  = to_world(origin, mp);
            g.pan.x += (wp_after.x - wp_before.x);
            g.pan.y += (wp_after.y - wp_before.y);
        }
    }

    /* Draw transitions first (under states). */
    for (size_t i = 0; i < g.trans.size(); ++i) {
        const Transition &t = g.trans[i];
        if (t.from < 0 || t.from >= (int)g.states.size()) continue;
        if (t.to   < 0 || t.to   >= (int)g.states.size()) continue;
        ImVec2 a_min, a_max, b_min, b_max;
        point_in_state(g.states[t.from], origin, ImVec2(-1, -1), &a_min, &a_max);
        point_in_state(g.states[t.to],   origin, ImVec2(-1, -1), &b_min, &b_max);
        ImVec2 a = ImVec2((a_min.x + a_max.x) * 0.5f, (a_min.y + a_max.y) * 0.5f);
        ImVec2 b = ImVec2((b_min.x + b_max.x) * 0.5f, (b_min.y + b_max.y) * 0.5f);
        bool sel = ((int)i == g.sel_trans);
        ImU32 col = sel ? jce_theme::selection_outline()
                        : jce_theme::text_secondary();
        /* Shorten endpoints to node edges for cleaner arrowhead. */
        ImVec2 d = ImVec2(b.x - a.x, b.y - a.y);
        float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len > 1.0f) {
            d.x /= len; d.y /= len;
            float pad = 70.0f * g.zoom * 0.5f;
            a.x += d.x * pad; a.y += d.y * pad;
            b.x -= d.x * pad; b.y -= d.y * pad;
        }
        draw_arrow(dl, a, b, col, sel ? 3.0f : 1.8f);
        /* Pickable midpoint. */
        ImVec2 mid = ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float dx = mp.x - mid.x, dy = mp.y - mid.y;
            if (dx * dx + dy * dy < 100.0f) {
                g.sel_trans = (int)i;
                g.sel_state = -1;
            }
        }
    }

    /* In-progress transition rubber-band line. */
    if (g.adding_from >= 0 && g.adding_from < (int)g.states.size()) {
        ImVec2 a_min, a_max;
        point_in_state(g.states[g.adding_from], origin, ImVec2(-1, -1), &a_min, &a_max);
        ImVec2 a = ImVec2((a_min.x + a_max.x) * 0.5f, (a_min.y + a_max.y) * 0.5f);
        draw_arrow(dl, a, mp, jce_theme::col_from(ImGuiCol_PlotLinesHovered), 2.0f);
    }

    /* States. */
    int hover_state = -1;
    for (size_t i = 0; i < g.states.size(); ++i) {
        ImVec2 nmin, nmax;
        bool inside = point_in_state(g.states[i], origin, mp, &nmin, &nmax);
        if (inside) hover_state = (int)i;
        bool sel = ((int)i == g.sel_state);
        ImU32 fill = ((int)i == g.default_state)
                     ? (jce_theme::is_light()
                            ? IM_COL32(190, 230, 195, 240)
                            : IM_COL32(80, 130, 90, 235))
                     : jce_theme::col_from(ImGuiCol_Button);
        if (sel) fill = jce_theme::col_from(ImGuiCol_ButtonActive);
        ImU32 border = inside ? jce_theme::col_from(ImGuiCol_Text)
                              : jce_theme::col_from(ImGuiCol_Border);
        dl->AddRectFilled(nmin, nmax, fill, 6.0f);
        dl->AddRect      (nmin, nmax, border, 6.0f, 0, 2.0f);
        dl->AddText(ImVec2(nmin.x + 8, nmin.y + 6),
                    jce_theme::text_primary(), g.states[i].name);
        if ((int)i == g.default_state)
            dl->AddText(ImVec2(nmin.x + 8, nmin.y + 24),
                        jce_theme::text_secondary(), "(default)");
    }

    /* Click handling. */
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (g.adding_from >= 0) {
            if (hover_state >= 0 && hover_state != g.adding_from) {
                Transition t;
                t.from = g.adding_from;
                t.to   = hover_state;
                g.trans.push_back(t);
                g.sel_trans = (int)g.trans.size() - 1;
                g.sel_state = -1;
            }
            g.adding_from = -1;
        } else if (hover_state >= 0) {
            g.sel_state = hover_state;
            g.sel_trans = -1;
            g.dragging_state = hover_state;
            ImVec2 ws = to_screen(origin, g.states[hover_state].pos);
            g.drag_offset = ImVec2(mp.x - ws.x, mp.y - ws.y);
        } else if (g.sel_trans < 0) {
            g.sel_state = -1;
        }
    }
    if (g.dragging_state >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (g.dragging_state < (int)g.states.size()) {
            ImVec2 desired_screen = ImVec2(mp.x - g.drag_offset.x, mp.y - g.drag_offset.y);
            ImVec2 w = to_world(origin, desired_screen);
            g.states[g.dragging_state].pos = w;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g.dragging_state = -1;

    /* Right-click context menus. */
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hover_state >= 0) {
            g.sel_state = hover_state;
            ImGui::OpenPopup("##sm_state_ctx");
        } else {
            ImGui::OpenPopup("##sm_canvas_ctx");
        }
    }
    if (ImGui::BeginPopup("##sm_canvas_ctx")) {
        if (ImGui::MenuItem(jce_editor_i18n("animatorSM.menu.newState"))) {
            State s;
            std::snprintf(s.name, sizeof(s.name), "State%d", (int)g.states.size());
            s.pos = to_world(origin, mp);
            g.states.push_back(s);
            if (g.default_state < 0) g.default_state = 0;
            g.sel_state = (int)g.states.size() - 1;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##sm_state_ctx")) {
        if (g.sel_state >= 0 && g.sel_state < (int)g.states.size()) {
            ImGui::Text("%s", g.states[g.sel_state].name);
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("animatorSM.menu.makeDefault")))      g.default_state = g.sel_state;
            if (ImGui::MenuItem(jce_editor_i18n("animatorSM.menu.addTransition"))) g.adding_from = g.sel_state;
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("animatorSM.menu.delete")))            delete_state(g.sel_state);
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

/* ───── Inspectors ─────────────────────────────────────────────── */

void draw_state_inspector(void)
{
    if (g.sel_state < 0 || g.sel_state >= (int)g.states.size()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("animatorSM.empty.noState"));
        return;
    }
    State &s = g.states[g.sel_state];
    ImGui::InputText(jce_editor_i18n_id("animatorSM.field.name", "s"), s.name,      sizeof(s.name));
    ImGui::InputText(jce_editor_i18n_id("animatorSM.field.clip", "s"), s.clip_path, sizeof(s.clip_path));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload("JCE_ASSET_PATH")) {
            std::strncpy(s.clip_path, (const char *)pl->Data, sizeof(s.clip_path) - 1);
            s.clip_path[sizeof(s.clip_path) - 1] = 0;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::DragFloat(jce_editor_i18n_id("animatorSM.field.speed", "s"), &s.speed, 0.01f, 0.0f, 8.0f);
    ImGui::Checkbox(jce_editor_i18n_id("animatorSM.field.loop", "s"),   &s.looping);
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.setDefault"))) g.default_state = g.sel_state;
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.addTransitionFromThis"))) g.adding_from = g.sel_state;
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.deleteState"))) delete_state(g.sel_state);
}

void draw_transition_inspector(void)
{
    if (g.sel_trans < 0 || g.sel_trans >= (int)g.trans.size()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("animatorSM.empty.noTransition"));
        return;
    }
    Transition &t = g.trans[g.sel_trans];
    const char *fn = (t.from >= 0 && t.from < (int)g.states.size())
                     ? g.states[t.from].name : "?";
    const char *tn = (t.to   >= 0 && t.to   < (int)g.states.size())
                     ? g.states[t.to].name   : "?";
    ImGui::Text("%s  →  %s", fn, tn);
    ImGui::DragFloat(jce_editor_i18n_id("animatorSM.field.duration", "t"), &t.duration, 0.01f, 0.0f, 5.0f);
    ImGui::Checkbox (jce_editor_i18n_id("animatorSM.field.hasExitTime", "t"), &t.has_exit);
    if (t.has_exit)
        ImGui::DragFloat(jce_editor_i18n_id("animatorSM.field.exitTime", "t"), &t.exit_time, 0.01f, 0.0f, 1.0f);

    ImGui::SeparatorText(jce_editor_i18n("animatorSM.section.conditions"));
    for (int i = 0; i < (int)t.conds.size(); ++i) {
        Condition &c = t.conds[i];
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(110);
        const char *p_label = (c.param_idx >= 0 && c.param_idx < (int)g.params.size())
                              ? g.params[c.param_idx].name : jce_editor_i18n("animatorSM.empty.none");
        if (ImGui::BeginCombo("##p", p_label)) {
            for (int k = 0; k < (int)g.params.size(); ++k) {
                bool sel = (k == c.param_idx);
                if (ImGui::Selectable(g.params[k].name, sel)) c.param_idx = k;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        if (ImGui::BeginCombo("##op", cond_op_name(c.op))) {
            for (int k = 0; k < 6; ++k)
                if (ImGui::Selectable(cond_op_name(k), c.op == k)) c.op = k;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (c.op != CO_TRUE && c.op != CO_FALSE) {
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("##thr", &c.threshold, 0.01f);
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("X")) {
            t.conds.erase(t.conds.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.addCondition"))) {
        Condition c;
        t.conds.push_back(c);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.deleteTransition"))) {
        g.trans.erase(g.trans.begin() + g.sel_trans);
        g.sel_trans = -1;
    }
}

void draw_param_table(void)
{
    if (ImGui::BeginTable("##params", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn(jce_editor_i18n("animatorSM.col.name"));
        ImGui::TableSetupColumn(jce_editor_i18n("animatorSM.col.type"), ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn(jce_editor_i18n("animatorSM.col.default"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)g.params.size(); ++i) {
            Param &p = g.params[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##nm", p.name, sizeof(p.name));
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##ty", param_type_name(p.type))) {
                for (int k = 0; k < 4; ++k)
                    if (ImGui::Selectable(param_type_name(k), p.type == k)) p.type = k;
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(120);
            if (p.type == PT_FLOAT)        ImGui::DragFloat("##d", &p.def_f, 0.01f);
            else if (p.type == PT_INT)     ImGui::DragInt  ("##d", &p.def_i, 1);
            else if (p.type == PT_BOOL) {
                bool b = p.def_b != 0; if (ImGui::Checkbox("##d", &b)) p.def_b = b ? 1 : 0;
            } else                          ImGui::TextDisabled("%s", jce_editor_i18n("animatorSM.empty.trigger"));
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) { delete_param(i); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.addParam"))) {
        Param p;
        std::snprintf(p.name, sizeof(p.name), "param%d", (int)g.params.size());
        g.params.push_back(p);
    }
}

void draw_toolbar(void)
{
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.save"))) {
        if (!g.path[0]) std::strncpy(g.path, "untitled.anim_sm.json", sizeof(g.path) - 1);
        save_to(g.path);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animatorSM.button.load"))) {
        if (g.path[0]) load_from(g.path);
        else jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                                         "animator-sm: set Path before Load");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(380);
    ImGui::InputText(jce_editor_i18n_id("animatorSM.field.path", "sm"), g.path, sizeof(g.path));
    ImGui::SameLine();
    if (g.adding_from >= 0)
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f),
                           "%s", jce_editor_i18n("animatorSM.hint.pickTarget"));
}

void draw_content(void)
{
    seed_default();
    draw_toolbar();
    ImGui::Separator();

    /* Layout: left = states+trans inspector, right = canvas, bottom = params. */
    float right_w = ImGui::GetContentRegionAvail().x * 0.30f;
    if (right_w < 240.0f) right_w = 240.0f;

    ImGui::BeginChild("##sm_left", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.62f), false);
    {
        float lw = ImGui::GetContentRegionAvail().x - right_w - 6.0f;
        ImGui::BeginChild("##sm_canvas_pane", ImVec2(lw, 0), false);
        draw_canvas();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##sm_inspector_pane", ImVec2(right_w, 0), true);
        if (ImGui::CollapsingHeader(jce_editor_i18n("animatorSM.section.state"), ImGuiTreeNodeFlags_DefaultOpen))
            draw_state_inspector();
        if (ImGui::CollapsingHeader(jce_editor_i18n("animatorSM.section.transition"), ImGuiTreeNodeFlags_DefaultOpen))
            draw_transition_inspector();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("animatorSM.section.parameters"));
    draw_param_table();
}

} /* namespace */

extern "C" void jce_editor_panel_animator_sm(void)
{
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_anim_sm", jce_editor_i18n("animatorSM.title"));
    if (!ImGui::Begin(_wt,
                      jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATOR_SM)))
    {
        ImGui::End();
        return;
    }
    draw_content();
    ImGui::End();
}
