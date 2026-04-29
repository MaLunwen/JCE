/*
 * jce_panel_animation_editor.cpp  Animation Clip Editor (Phase A).
 *
 * A standalone authoring panel for animation clips. Phase A scope:
 *
 *  - Clip { name, duration, fps, loop, tracks[] }
 *  - Track { name, color, type (event / float / vec3), events[] }
 *      Event { time, name }                 (type=event)
 *      Event { time, value (float|vec3) }   (type=float / vec3)
 *  - Add / remove / rename tracks; add / drag / delete events on a
 *    horizontal timeline; numeric inspector for the selected event.
 *  - Playback transport: Play / Pause / Stop with internal time cursor
 *    that loops or clamps based on clip.loop.
 *  - Save / Load to JSON via the engine JSON parser.
 *
 *  No coupling to the engine animation runtime yet — this panel is the
 *  authoring surface; consumers can be wired in a later phase.
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
#include <string>
#include <vector>

namespace {

enum TrackType { TT_EVENT = 0, TT_FLOAT = 1, TT_VEC3 = 2 };

struct Event {
    float time = 0.0f;
    char  name[64] = {0};   /* TT_EVENT: trigger label */
    float value[3] = {0,0,0};  /* TT_FLOAT uses [0]; TT_VEC3 uses xyz */
};

struct Track {
    char       name[64] = "track";
    int        type = TT_EVENT;
    float      color[3] = { 0.6f, 0.8f, 1.0f };
    bool       collapsed = false;
    std::vector<Event> events;
};

struct Clip {
    char       name[64] = "clip";
    float      duration = 5.0f;
    float      fps = 30.0f;
    bool       loop = true;
    std::vector<Track> tracks;
};

struct State {
    Clip   clip;
    char   path[260] = "animations/clip.anim.json";

    /* View. */
    float  zoom_pps = 80.0f;   /* pixels per second */
    float  scroll_x = 0.0f;
    bool   show_inspector = true;

    /* Playback. */
    bool   playing = false;
    float  cursor = 0.0f;
    double last_tick_time = 0.0;

    /* Selection. */
    int    sel_track = -1;
    int    sel_event = -1;

    /* Drag. */
    int    drag_track = -1;
    int    drag_event = -1;
    float  drag_grab_offset = 0.0f;
};

State s;

void clip_reset(Clip *c)
{
    *c = Clip();
    Track t1; std::snprintf(t1.name, sizeof(t1.name), "events");
    t1.type = TT_EVENT;
    Track t2; std::snprintf(t2.name, sizeof(t2.name), "speed");
    t2.type = TT_FLOAT;  t2.color[0] = 1.0f; t2.color[1] = 0.6f; t2.color[2] = 0.4f;
    Track t3; std::snprintf(t3.name, sizeof(t3.name), "position");
    t3.type = TT_VEC3;   t3.color[0] = 0.5f; t3.color[1] = 1.0f; t3.color[2] = 0.6f;
    c->tracks.push_back(t1);
    c->tracks.push_back(t2);
    c->tracks.push_back(t3);
}

const char *type_label(int t)
{
    switch (t) { case TT_EVENT: return "event";
                 case TT_FLOAT: return "float";
                 case TT_VEC3:  return "vec3";  default: return "?"; }
}

int type_from_label(const char *s_)
{
    if (s_ && std::strcmp(s_, "float") == 0) return TT_FLOAT;
    if (s_ && std::strcmp(s_, "vec3")  == 0) return TT_VEC3;
    return TT_EVENT;
}

/* ------------------------------------------------------------------ */
/*  JSON I/O                                                           */
/* ------------------------------------------------------------------ */

void save_clip(const char *path)
{
    JceJson *root = jce_json_object();
    jce_json_set_string(root, "name", s.clip.name);
    jce_json_set_number(root, "duration", s.clip.duration);
    jce_json_set_number(root, "fps", s.clip.fps);
    jce_json_set_bool  (root, "loop", s.clip.loop);
    JceJson *jt = jce_json_array();
    for (auto &t : s.clip.tracks) {
        JceJson *jto = jce_json_object();
        jce_json_set_string(jto, "name", t.name);
        jce_json_set_string(jto, "type", type_label(t.type));
        jce_json_set_float_array(jto, "color", t.color, 3);
        JceJson *jev = jce_json_array();
        for (auto &e : t.events) {
            JceJson *jeo = jce_json_object();
            jce_json_set_number(jeo, "time", e.time);
            if (t.type == TT_EVENT) {
                jce_json_set_string(jeo, "name", e.name);
            } else if (t.type == TT_FLOAT) {
                jce_json_set_number(jeo, "value", e.value[0]);
            } else {
                jce_json_set_float_array(jeo, "value", e.value, 3);
            }
            jce_json_array_push(jev, jeo);
        }
        jce_json_set_child(jto, "events", jev);
        jce_json_array_push(jt, jto);
    }
    jce_json_set_child(root, "tracks", jt);
    if (ed_write_json_to_file(path, root))
        jce_editor_console_log("animation: saved %s", path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "animation: save failed: %s", path);
}

void load_clip(const char *path)
{
    size_t sz = 0;
    char  *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "animation: cannot read %s", path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "animation: parse failed: %s", path);
        return;
    }
    Clip c;
    const char *nm = jce_json_get_string(root, "name", "clip");
    std::snprintf(c.name, sizeof(c.name), "%s", nm ? nm : "clip");
    c.duration = (float)jce_json_get_number(root, "duration", 5.0);
    c.fps      = (float)jce_json_get_number(root, "fps", 30.0);
    c.loop     = jce_json_get_bool  (root, "loop", true);
    JceJson *jt = jce_json_get(root, "tracks");
    if (jt && jce_json_is_array(jt)) {
        int n = jce_json_array_size(jt);
        for (int i = 0; i < n; ++i) {
            JceJson *jto = jce_json_array_at(jt, i);
            if (!jto) continue;
            Track t;
            const char *tn = jce_json_get_string(jto, "name", "track");
            std::snprintf(t.name, sizeof(t.name), "%s", tn ? tn : "track");
            t.type = type_from_label(jce_json_get_string(jto, "type", "event"));
            jce_json_get_floats(jto, "color", t.color, 3, nullptr);
            JceJson *jev = jce_json_get(jto, "events");
            if (jev && jce_json_is_array(jev)) {
                int en = jce_json_array_size(jev);
                for (int j = 0; j < en; ++j) {
                    JceJson *jeo = jce_json_array_at(jev, j);
                    if (!jeo) continue;
                    Event e;
                    e.time = (float)jce_json_get_number(jeo, "time", 0.0);
                    if (t.type == TT_EVENT) {
                        const char *en_ = jce_json_get_string(jeo, "name", "");
                        std::snprintf(e.name, sizeof(e.name), "%s",
                                      en_ ? en_ : "");
                    } else if (t.type == TT_FLOAT) {
                        e.value[0] = (float)jce_json_get_number(jeo, "value", 0.0);
                    } else {
                        jce_json_get_floats(jeo, "value", e.value, 3, nullptr);
                    }
                    t.events.push_back(e);
                }
            }
            std::sort(t.events.begin(), t.events.end(),
                      [](const Event &a, const Event &b){ return a.time < b.time; });
            c.tracks.push_back(t);
        }
    }
    jce_json_free(root);
    s.clip = c;
    s.cursor = 0.0f; s.sel_track = -1; s.sel_event = -1;
    jce_editor_console_log("animation: loaded %s (%d tracks)",
                           path, (int)s.clip.tracks.size());
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

void draw_toolbar(void)
{
    ImGui::InputText(jce_editor_i18n("animationEditor.field.clipName"), s.clip.name, sizeof(s.clip.name));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.new"))) {
        clip_reset(&s.clip);
        s.cursor = 0.0f; s.sel_track = -1; s.sel_event = -1;
    }

    ImGui::InputText(jce_editor_i18n("animationEditor.field.file"), s.path, sizeof(s.path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.save")) && s.path[0]) save_clip(s.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.load")) && s.path[0]) load_clip(s.path);

    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat(jce_editor_i18n("animationEditor.field.duration"), &s.clip.duration, 0.1f, 0.1f, 600.0f, "%.2f s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::DragFloat(jce_editor_i18n("animationEditor.field.fps"), &s.clip.fps, 0.5f, 1.0f, 240.0f, "%.0f");
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n("animationEditor.field.loop"), &s.clip.loop);
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n("animationEditor.field.inspector"), &s.show_inspector);

    /* Transport. */
    if (ImGui::Button(s.playing ? jce_editor_i18n("animationEditor.button.pause") : jce_editor_i18n("animationEditor.button.play")))
        s.playing = !s.playing;
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.stop"))) { s.playing = false; s.cursor = 0.0f; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::DragFloat(jce_editor_i18n("animationEditor.field.cursor"), &s.cursor, 0.01f, 0.0f, s.clip.duration, "%.3f s");

    /* Zoom. */
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat(jce_editor_i18n("animationEditor.field.zoom"), &s.zoom_pps, 10.0f, 400.0f, "%.0f");
}

float time_to_x(float t, float origin_x)
{
    return origin_x + (t * s.zoom_pps) - s.scroll_x;
}

float x_to_time(float x, float origin_x)
{
    return ((x + s.scroll_x) - origin_x) / s.zoom_pps;
}

void draw_ruler(ImVec2 p0, float width, float origin_x)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float h = 22.0f;
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + h),
                      jce_theme::header_bg());
    /* Tick every 1s; minor every 0.1s. */
    float t0 = x_to_time(p0.x, origin_x);
    float t1 = x_to_time(p0.x + width, origin_x);
    if (t0 < 0) t0 = 0;
    if (t1 > s.clip.duration) t1 = s.clip.duration;
    for (float t = std::floor(t0 * 10.0f) / 10.0f; t <= t1; t += 0.1f) {
        float x = time_to_x(t, origin_x);
        if (x < p0.x || x > p0.x + width) continue;
        float frac = std::fmod(t, 1.0f);
        if (std::fabs(frac) < 0.01f || std::fabs(frac - 1.0f) < 0.01f) {
            dl->AddLine(ImVec2(x, p0.y + h * 0.4f),
                        ImVec2(x, p0.y + h),
                        jce_theme::text_primary());
            char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%.0fs", t);
            dl->AddText(ImVec2(x + 2, p0.y + 2),
                        jce_theme::text_primary(), lbl);
        } else {
            dl->AddLine(ImVec2(x, p0.y + h * 0.7f),
                        ImVec2(x, p0.y + h),
                        jce_theme::text_secondary());
        }
    }
    /* Clip end marker. */
    float xe = time_to_x(s.clip.duration, origin_x);
    dl->AddLine(ImVec2(xe, p0.y), ImVec2(xe, p0.y + h),
                jce_theme::playhead(), 2.0f);
}

bool event_value_inspector(Track &t, Event &e)
{
    bool changed = false;
    if (t.type == TT_EVENT) {
        if (ImGui::InputText(jce_editor_i18n("animationEditor.field.name"), e.name, sizeof(e.name))) changed = true;
    } else if (t.type == TT_FLOAT) {
        if (ImGui::DragFloat(jce_editor_i18n("animationEditor.field.value"), &e.value[0], 0.01f)) changed = true;
    } else {
        if (ImGui::DragFloat3(jce_editor_i18n("animationEditor.field.value"), e.value, 0.01f)) changed = true;
    }
    if (ImGui::DragFloat(jce_editor_i18n("animationEditor.field.time"), &e.time, 0.01f, 0.0f, s.clip.duration, "%.3f s"))
        changed = true;
    return changed;
}

void draw_tracks(ImVec2 p0, float width, float origin_x)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float row_h = 38.0f;
    ImGuiIO &io = ImGui::GetIO();

    /* Per-track row. */
    for (int ti = 0; ti < (int)s.clip.tracks.size(); ++ti) {
        Track &t = s.clip.tracks[ti];
        ImVec2 r0 = ImVec2(p0.x, p0.y + ti * row_h);
        ImVec2 r1 = ImVec2(p0.x + width, r0.y + row_h);
        ImU32 bg = (ti & 1) ? jce_theme::track_odd()
                            : jce_theme::track_even();
        dl->AddRectFilled(r0, r1, bg);
        dl->AddLine(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y - 1),
                    jce_theme::separator());

        /* Click empty area in track to add event at that time. */
        ImGui::PushID(ti);
        ImGui::SetCursorScreenPos(r0);
        ImGui::InvisibleButton("##track_row", ImVec2(width, row_h));
        bool row_hovered = ImGui::IsItemHovered();
        if (row_hovered && ImGui::IsMouseDoubleClicked(0)) {
            float tt = x_to_time(io.MousePos.x, origin_x);
            if (tt < 0) tt = 0;
            if (tt > s.clip.duration) tt = s.clip.duration;
            Event e; e.time = tt;
            if (t.type == TT_EVENT) std::snprintf(e.name, sizeof(e.name),
                                                  "evt_%d", (int)t.events.size());
            t.events.push_back(e);
            std::sort(t.events.begin(), t.events.end(),
                      [](const Event &a, const Event &b){ return a.time < b.time; });
            s.sel_track = ti;
            /* Find the inserted index by scanning for matching time. */
            for (int k = 0; k < (int)t.events.size(); ++k)
                if (t.events[k].time == tt) { s.sel_event = k; break; }
        }
        ImGui::PopID();

        /* Draw events. */
        ImU32 col_main = IM_COL32((int)(t.color[0] * 255),
                                  (int)(t.color[1] * 255),
                                  (int)(t.color[2] * 255), 255);
        ImU32 col_sel  = jce_theme::selection_outline();
        for (int ei = 0; ei < (int)t.events.size(); ++ei) {
            Event &e = t.events[ei];
            float ex = time_to_x(e.time, origin_x);
            if (ex < p0.x - 8 || ex > p0.x + width + 8) continue;
            float cy = (r0.y + r1.y) * 0.5f;
            bool selected = (s.sel_track == ti && s.sel_event == ei);
            ImU32 col = selected ? col_sel : col_main;
            /* Diamond marker. */
            ImVec2 a(ex, cy - 8), b(ex + 7, cy), c(ex, cy + 8), d(ex - 7, cy);
            dl->AddQuadFilled(a, b, c, d, col);
            dl->AddQuad(a, b, c, d, jce_theme::node_outline(), 1.0f);
            /* Label */
            char lbl[80];
            if (t.type == TT_EVENT) std::snprintf(lbl, sizeof(lbl), "%s", e.name);
            else if (t.type == TT_FLOAT)
                std::snprintf(lbl, sizeof(lbl), "%.2f", e.value[0]);
            else std::snprintf(lbl, sizeof(lbl), "(%.1f,%.1f,%.1f)",
                               e.value[0], e.value[1], e.value[2]);
            dl->AddText(ImVec2(ex + 9, cy - 6), col, lbl);

            /* Hit-test for select / drag. */
            ImGui::PushID(ei + 1000);
            ImGui::SetCursorScreenPos(ImVec2(ex - 8, cy - 8));
            ImGui::InvisibleButton("##evt", ImVec2(16, 16));
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
                s.sel_track = ti;
                s.sel_event = ei;
                s.drag_track = ti;
                s.drag_event = ei;
                s.drag_grab_offset = io.MousePos.x - ex;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
                ImGui::OpenPopup("evt_ctx");
            }
            if (ImGui::BeginPopup("evt_ctx")) {
                if (ImGui::MenuItem(jce_editor_i18n("animationEditor.menu.deleteEvent"))) {
                    t.events.erase(t.events.begin() + ei);
                    if (s.sel_track == ti && s.sel_event == ei) {
                        s.sel_event = -1;
                    }
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }

    /* Drag in progress. */
    if (s.drag_event >= 0 && ImGui::IsMouseDown(0)) {
        if (s.drag_track < (int)s.clip.tracks.size()) {
            Track &t = s.clip.tracks[s.drag_track];
            if (s.drag_event < (int)t.events.size()) {
                float nx = io.MousePos.x - s.drag_grab_offset;
                float nt = x_to_time(nx, origin_x);
                if (nt < 0) nt = 0;
                if (nt > s.clip.duration) nt = s.clip.duration;
                t.events[s.drag_event].time = nt;
            }
        }
    } else if (s.drag_event >= 0 && !ImGui::IsMouseDown(0)) {
        /* Drop: re-sort and update selection index. */
        if (s.drag_track < (int)s.clip.tracks.size()) {
            Track &t = s.clip.tracks[s.drag_track];
            if (s.drag_event < (int)t.events.size()) {
                float kept = t.events[s.drag_event].time;
                std::sort(t.events.begin(), t.events.end(),
                          [](const Event &a, const Event &b){ return a.time < b.time; });
                for (int k = 0; k < (int)t.events.size(); ++k)
                    if (t.events[k].time == kept) {
                        s.sel_event = k;
                        break;
                    }
            }
        }
        s.drag_event = -1;
        s.drag_track = -1;
    }

    /* Playhead. */
    float xc = time_to_x(s.cursor, origin_x);
    dl->AddLine(ImVec2(xc, p0.y),
                ImVec2(xc, p0.y + s.clip.tracks.size() * row_h),
                jce_theme::playhead(), 2.0f);
}

void draw_track_list_panel(void)
{
    ImGui::BeginChild("##tracks_left", ImVec2(220, 0), true);
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.addTrack"))) {
        Track t;
        std::snprintf(t.name, sizeof(t.name), "track_%d",
                      (int)s.clip.tracks.size());
        s.clip.tracks.push_back(t);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.removeTrack")) && s.sel_track >= 0 &&
        s.sel_track < (int)s.clip.tracks.size()) {
        s.clip.tracks.erase(s.clip.tracks.begin() + s.sel_track);
        if (s.sel_track >= (int)s.clip.tracks.size())
            s.sel_track = (int)s.clip.tracks.size() - 1;
        s.sel_event = -1;
    }
    ImGui::Separator();
    for (int i = 0; i < (int)s.clip.tracks.size(); ++i) {
        Track &t = s.clip.tracks[i];
        ImGui::PushID(i);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "[%s] %s", type_label(t.type), t.name);
        if (ImGui::Selectable(lbl, s.sel_track == i)) {
            s.sel_track = i; s.sel_event = -1;
        }
        if (ImGui::BeginPopupContextItem("trk_ctx")) {
            ImGui::SetNextItemWidth(140);
            ImGui::InputText(jce_editor_i18n("animationEditor.field.name"), t.name, sizeof(t.name));
            int tt = t.type;
            if (ImGui::Combo(jce_editor_i18n_id("animationEditor.field.type", "ae_t_type"), &tt, "event\0float\0vec3\0")) {
                t.type = tt; t.events.clear();
            }
            ImGui::ColorEdit3(jce_editor_i18n("animationEditor.field.color"), t.color, ImGuiColorEditFlags_NoInputs);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void draw_inspector_panel(void)
{
    ImGui::BeginChild("##inspector", ImVec2(260, 0), true);
    ImGui::TextDisabled("%s", jce_editor_i18n("animationEditor.section.inspector"));
    ImGui::Separator();
    if (s.sel_track >= 0 && s.sel_track < (int)s.clip.tracks.size()) {
        Track &t = s.clip.tracks[s.sel_track];
        ImGui::Text(jce_editor_i18n("animationEditor.label.track"), t.name, type_label(t.type));
        if (s.sel_event >= 0 && s.sel_event < (int)t.events.size()) {
            ImGui::Separator();
            event_value_inspector(t, t.events[s.sel_event]);
        } else {
            ImGui::TextDisabled("%s", jce_editor_i18n("animationEditor.empty.noEvent"));
        }
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("animationEditor.empty.noTrack"));
    }
    ImGui::EndChild();
}

void tick_playback(void)
{
    double now = ImGui::GetTime();
    if (s.last_tick_time == 0.0) s.last_tick_time = now;
    float dt = (float)(now - s.last_tick_time);
    s.last_tick_time = now;
    if (!s.playing) return;
    s.cursor += dt;
    if (s.cursor >= s.clip.duration) {
        if (s.clip.loop) {
            s.cursor = std::fmod(s.cursor, s.clip.duration);
        } else {
            s.cursor = s.clip.duration;
            s.playing = false;
        }
    }
}

void draw_content(void)
{
    if (s.clip.tracks.empty()) clip_reset(&s.clip);
    tick_playback();
    draw_toolbar();
    ImGui::Separator();

    draw_track_list_panel();
    ImGui::SameLine();

    /* Center: timeline canvas. */
    float left = (s.show_inspector ? 280.0f : 0.0f);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 canvas_sz = ImVec2(avail.x - left, avail.y);
    if (canvas_sz.x < 100.0f) canvas_sz.x = 100.0f;

    ImGui::BeginChild("##timeline", canvas_sz, true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float  width = ImGui::GetContentRegionAvail().x;
    float  origin_x = p0.x;
    int    n_rows = std::max(1, (int)s.clip.tracks.size());
    float  total_h = 22.0f + n_rows * 38.0f;
    /* Reserve full virtual width = duration * zoom + padding. */
    float  virtual_w = std::max(width, s.clip.duration * s.zoom_pps + 80.0f);
    ImGui::Dummy(ImVec2(virtual_w, total_h));

    draw_ruler(p0, virtual_w, origin_x);
    draw_tracks(ImVec2(p0.x, p0.y + 22.0f), virtual_w, origin_x);

    /* Click ruler to scrub the playhead. */
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##ruler", ImVec2(virtual_w, 22.0f));
    if (ImGui::IsItemActive()) {
        float tt = x_to_time(ImGui::GetIO().MousePos.x, origin_x);
        if (tt < 0) tt = 0;
        if (tt > s.clip.duration) tt = s.clip.duration;
        s.cursor = tt;
    }
    ImGui::EndChild();

    if (s.show_inspector) {
        ImGui::SameLine();
        draw_inspector_panel();
    }
}

} /* namespace */

extern "C" void jce_editor_panel_animation_editor(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR);
    if (!vis || !*vis) return;
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_anim_editor", jce_editor_i18n("animationEditor.title"));
    if (ImGui::Begin(_wt, vis)) {
        draw_content();
    }
    ImGui::End();
}
