/*
 * jce_panel_animation_editor.cpp  Animation Clip Editor (frame events).
 *
 * Authoring surface for the engine's animation frame-event sidecar — the
 * <skeleton_path>.anim.json file that jce_scene_renderer.c lazily loads
 * next to a SkeletalAnimator's skeleton asset:
 *
 *   { "<clipName>": [ { "time": s, "name"?, "id"?, "f0"?, "f1"?,
 *                       "i0"? }, ... ], ... }
 *
 *  - One timeline lane per engine clip; the lane name must match the
 *    model's clip name (case-sensitive) for its events to fire at runtime.
 *  - Add / drag / delete events; inspector edits the optional string name
 *    plus the engine payload (id, f0, f1, i0).
 *  - Default save path derives "<skeleton>.anim.json" from the focused
 *    entity's SkeletalAnimator, mirroring the engine's sidecar lookup; the
 *    path field stays editable as Save-As.
 *  - Legacy panel files ({name,duration,fps,loop,tracks:[...]}) are
 *    detected by the "tracks" key and converted in-memory (event tracks →
 *    named events on a lane named after the legacy clip; float/vec3 tracks
 *    dropped — no engine consumer). Re-saving writes the engine schema.
 */

#include "io/jce_editor_file_util.h"
#include "jce_panel_common.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "ui/jce_theme_palette.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/middleware/scene/jce_scene.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* One authored frame event. Mirrors the engine's JceAnimEvent
 * (jce_anim_ik.h): optional string label + numeric id/payload. */
struct Event {
    float time = 0.0f;
    char  name[48] = {0};   /* optional label; "" = unnamed */
    int   id = 0;           /* user-defined event id (e.g. footstep) */
    float f0 = 0.0f;        /* small float payload */
    float f1 = 0.0f;
    int   i0 = 0;           /* small int payload */
};

/* One event lane = one engine clip. The lane name must match a clip name
 * in the skeleton's model (case-sensitive) — the runtime looks the clip
 * name up as a top-level key of the sidecar. */
struct Track {
    char       name[64] = "clip";
    float      color[3] = { 0.6f, 0.8f, 1.0f };
    std::vector<Event> events;
};

/* The sidecar document. duration/loop are editor preview settings only —
 * NOT serialized (the engine takes the real duration from the model). */
struct Clip {
    float      duration = 5.0f;
    bool       loop = true;
    std::vector<Track> tracks;
};

struct State {
    Clip   clip;
    char   path[260] = "animations/clip.anim.json";
    bool   path_user_set = false; /* user typed/browsed → stop auto-derive */
    char   warn[256] = {0};       /* one-time banner (legacy conversion) */

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
}

/* Rotating lane palette (colors are an editor nicety; the engine schema
 * carries no presentation data). */
void lane_color(int idx, float out[3])
{
    static const float pal[6][3] = {
        { 0.55f, 0.78f, 1.00f }, { 1.00f, 0.62f, 0.42f },
        { 0.55f, 1.00f, 0.62f }, { 0.95f, 0.80f, 0.40f },
        { 0.80f, 0.60f, 1.00f }, { 0.45f, 0.90f, 0.90f },
    };
    const float *c = pal[((idx % 6) + 6) % 6];
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
}

void sort_events(Track &t)
{
    std::sort(t.events.begin(), t.events.end(),
              [](const Event &a, const Event &b){ return a.time < b.time; });
}

/* The focused entity's SkeletalAnimator, if any (shared by the default
 * save-path derivation and "Clips From Selection"). */
JceSkeletalAnimatorComponent *focused_skeletal_animator(void)
{
    uint32_t focused = jce_state_get_focused();
    if (!focused) return nullptr;
    JceScene *scene = jce_state_get_scene();
    if (!scene || !jce_state_entity_exists(focused)) return nullptr;
    return jce_scene_get_skeletal_animator(
        scene, jce_state_to_ecs_entity(focused));
}

/* Default save path mirrors the engine's sidecar derivation
 * (jce_scene_renderer.c: snprintf "%s.anim.json" onto skeleton_path) so a
 * plain Save lands exactly where sr_anim_events_load will look. Stops as
 * soon as the user edits the path field (Save-As). */
void derive_default_path(void)
{
    if (s.path_user_set) return;
    JceSkeletalAnimatorComponent *sk = focused_skeletal_animator();
    if (!sk || !sk->skeleton_path[0]) return;
    std::snprintf(s.path, sizeof(s.path), "%s.anim.json", sk->skeleton_path);
}

/* Add one event lane per clip on the focused entity's SkeletalAnimator,
 * skipping lanes that already exist (names must match to fire). */
void sync_clips_from_selection(void)
{
    JceSkeletalAnimatorComponent *sk = focused_skeletal_animator();
    if (!sk) return;
    int n = sk->clip_count;
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    for (int i = 0; i < n; ++i) {
        const char *cn = sk->clip_names[i];
        if (!cn[0]) continue;
        bool exists = false;
        for (auto &t : s.clip.tracks)
            if (std::strcmp(t.name, cn) == 0) { exists = true; break; }
        if (exists) continue;
        Track t;
        std::snprintf(t.name, sizeof(t.name), "%s", cn);
        lane_color((int)s.clip.tracks.size(), t.color);
        s.clip.tracks.push_back(t);
    }
}

/* ------------------------------------------------------------------ */
/*  JSON I/O                                                           */
/* ------------------------------------------------------------------ */

void save_clip(const char *path)
{
    JceJson *root = jce_json_object();

    /* "_meta": panel preview settings.  The runtime loader only queries the
     * model's clip names as top-level keys, so this object is invisible to
     * it (sr_anim_events_load ignores unknown keys). */
    JceJson *meta = jce_json_object();
    jce_json_set_number(meta, "duration", s.clip.duration);
    jce_json_set_bool  (meta, "loop", s.clip.loop);
    jce_json_set_child(root, "_meta", meta);

    /* One top-level array per lane, keyed by clip name.  Optional event
     * fields are omitted at their defaults to keep the sidecar minimal. */
    for (auto &t : s.clip.tracks) {
        if (jce_json_has(root, t.name)) {
            jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                "animation: duplicate lane '%s' skipped (clip names must be "
                "unique)", t.name);
            continue;
        }
        JceJson *jev = jce_json_array();
        for (auto &e : t.events) {
            JceJson *jeo = jce_json_object();
            jce_json_set_number(jeo, "time", e.time);
            if (e.name[0])     jce_json_set_string(jeo, "name", e.name);
            if (e.id != 0)     jce_json_set_int   (jeo, "id",   e.id);
            if (e.f0 != 0.0f)  jce_json_set_number(jeo, "f0",   e.f0);
            if (e.f1 != 0.0f)  jce_json_set_number(jeo, "f1",   e.f1);
            if (e.i0 != 0)     jce_json_set_int   (jeo, "i0",   e.i0);
            jce_json_array_push(jev, jeo);
        }
        jce_json_set_child(root, t.name, jev);
    }
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
    s.warn[0] = '\0';
    int lane_idx = 0;

    if (jce_json_has(root, "tracks")) {
        /* Legacy panel schema: {name,duration,fps,loop,tracks:[...]}.
         * Event tracks become lanes; float/vec3 tracks have no engine
         * consumer and are dropped (counted for the banner). */
        c.duration = (float)jce_json_get_number(root, "duration", 5.0);
        c.loop     = jce_json_get_bool(root, "loop", true);
        int dropped = 0;
        JceJson *jt = jce_json_get(root, "tracks");
        int n = jce_json_is_array(jt) ? jce_json_array_size(jt) : 0;
        for (int i = 0; i < n; ++i) {
            JceJson *jto = jce_json_array_at(jt, i);
            if (!jto) continue;
            const char *ty = jce_json_get_string(jto, "type", "event");
            if (ty && std::strcmp(ty, "event") != 0) { ++dropped; continue; }
            Track t;
            const char *tn = jce_json_get_string(jto, "name", "clip");
            std::snprintf(t.name, sizeof(t.name), "%s", tn ? tn : "clip");
            lane_color(lane_idx++, t.color);
            JceJson *jev = jce_json_get(jto, "events");
            int en = (jev && jce_json_is_array(jev))
                         ? jce_json_array_size(jev) : 0;
            for (int j = 0; j < en; ++j) {
                JceJson *jeo = jce_json_array_at(jev, j);
                if (!jeo) continue;
                Event e;
                e.time = (float)jce_json_get_number(jeo, "time", 0.0);
                const char *en_ = jce_json_get_string(jeo, "name", "");
                std::snprintf(e.name, sizeof(e.name), "%s", en_ ? en_ : "");
                t.events.push_back(e);
            }
            sort_events(t);
            c.tracks.push_back(t);
        }
        std::snprintf(s.warn, sizeof(s.warn),
            "Converted legacy clip format; %d non-event track(s) dropped — "
            "re-saving writes the engine schema.", dropped);
    } else {
        /* Engine sidecar schema: every top-level array is one clip lane;
         * the optional "_meta" object carries panel preview settings. */
        JceJson *meta = jce_json_get(root, "_meta");
        if (meta && jce_json_is_object(meta)) {
            c.duration = (float)jce_json_get_number(meta, "duration", 5.0);
            c.loop     = jce_json_get_bool(meta, "loop", true);
        }
        for (JceJson *it = jce_json_first_child(root); it;
             it = jce_json_next_sibling(it)) {
            const char *key = jce_json_member_key(it);
            if (!key || !key[0]) continue;
            if (std::strcmp(key, "_meta") == 0) continue;
            if (!jce_json_is_array(it)) continue;
            Track t;
            std::snprintf(t.name, sizeof(t.name), "%s", key);
            lane_color(lane_idx++, t.color);
            int en = jce_json_array_size(it);
            for (int j = 0; j < en; ++j) {
                JceJson *jeo = jce_json_array_at(it, j);
                if (!jeo) continue;
                Event e;
                e.time = (float)jce_json_get_number(jeo, "time", 0.0);
                const char *nm = jce_json_get_string(jeo, "name", "");
                std::snprintf(e.name, sizeof(e.name), "%s", nm ? nm : "");
                e.id = jce_json_get_int(jeo, "id", 0);
                e.f0 = (float)jce_json_get_number(jeo, "f0", 0.0);
                e.f1 = (float)jce_json_get_number(jeo, "f1", 0.0);
                e.i0 = jce_json_get_int(jeo, "i0", 0);
                t.events.push_back(e);
            }
            sort_events(t);
            c.tracks.push_back(t);
        }
    }
    jce_json_free(root);

    /* Grow the preview duration so every loaded event stays reachable. */
    for (auto &t : c.tracks)
        for (auto &e : t.events)
            if (e.time > c.duration) c.duration = e.time;

    s.clip = c;
    s.cursor = 0.0f; s.sel_track = -1; s.sel_event = -1;
    jce_editor_console_log("animation: loaded %s (%d lanes)",
                           path, (int)s.clip.tracks.size());
}

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

void draw_toolbar(void)
{
    /* Keep the save path tracking the focused SkeletalAnimator's sidecar
     * until the user edits the field (then it becomes Save-As). */
    derive_default_path();

    if (ImGui::Button(jce_editor_i18n("animationEditor.button.syncClips")))
        sync_clips_from_selection();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.new"))) {
        clip_reset(&s.clip);
        s.cursor = 0.0f; s.sel_track = -1; s.sel_event = -1;
        s.warn[0] = '\0';
    }

    if (jce_draw_path_input(jce_editor_i18n("animationEditor.field.file"), s.path, sizeof(s.path), JcePathKind::FileAbs))
        s.path_user_set = true;
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.save")) && s.path[0]) save_clip(s.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("animationEditor.button.load")) && s.path[0]) load_clip(s.path);

    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat(jce_editor_i18n("animationEditor.field.duration"), &s.clip.duration, 0.1f, 0.1f, 600.0f, "%.2f s");
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

    /* One-time banner (legacy conversion notice). */
    if (s.warn[0]) {
        if (ImGui::SmallButton("x##ae_warn_dismiss")) s.warn[0] = '\0';
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.30f, 1.0f));
        ImGui::TextWrapped("%s", s.warn);
        ImGui::PopStyleColor();
    }
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

/* Edits the engine event payload: optional label + id/f0/f1/i0. */
bool event_value_inspector(Event &e)
{
    bool changed = false;
    if (ImGui::InputText(jce_editor_i18n("animationEditor.field.eventName"), e.name, sizeof(e.name))) changed = true;
    if (ImGui::DragInt(jce_editor_i18n("animationEditor.field.eventId"), &e.id, 0.1f)) changed = true;
    if (ImGui::DragFloat(jce_editor_i18n("animationEditor.field.eventF0"), &e.f0, 0.01f)) changed = true;
    if (ImGui::DragFloat(jce_editor_i18n("animationEditor.field.eventF1"), &e.f1, 0.01f)) changed = true;
    if (ImGui::DragInt(jce_editor_i18n("animationEditor.field.eventI0"), &e.i0, 0.1f)) changed = true;
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
            std::snprintf(e.name, sizeof(e.name),
                          "evt_%d", (int)t.events.size());
            t.events.push_back(e);
            sort_events(t);
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
            /* Label: the optional name, else the numeric id. */
            char lbl[80];
            if (e.name[0]) std::snprintf(lbl, sizeof(lbl), "%s", e.name);
            else           std::snprintf(lbl, sizeof(lbl), "#%d", e.id);
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
        std::snprintf(t.name, sizeof(t.name), "clip_%d",
                      (int)s.clip.tracks.size());
        lane_color((int)s.clip.tracks.size(), t.color);
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
        std::snprintf(lbl, sizeof(lbl), "%s##lane", t.name);
        if (ImGui::Selectable(lbl, s.sel_track == i)) {
            s.sel_track = i; s.sel_event = -1;
        }
        if (ImGui::BeginPopupContextItem("trk_ctx")) {
            ImGui::SetNextItemWidth(140);
            ImGui::InputText(jce_editor_i18n("animationEditor.field.name"), t.name, sizeof(t.name));
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
        ImGui::Text(jce_editor_i18n("animationEditor.label.track"), t.name);
        if (s.sel_event >= 0 && s.sel_event < (int)t.events.size()) {
            ImGui::Separator();
            event_value_inspector(t.events[s.sel_event]);
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

static void draw_editor_tab(void)
{
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

/* ──────────────────────────────────────────────────────────────────
 * Animation Workbench (P7-W1)
 *
 * Animation Editor hosts Animator State Machine / Curve Editor /
 * Sequencer / Timeline / Animation Rigging as sibling tabs of an
 * outer "Animation" TabBar.  Sibling panels remain registered
 * (JCE_PANEL_ANIMATOR_SM / _CURVE_EDITOR / _SEQUENCER / _TIMELINE /
 * _ANIMATION_RIGGING) and route here via
 * jce_panel_animation_editor_request_tab().
 * ────────────────────────────────────────────────────────────────── */

extern "C" void animator_sm_draw_content(void);
extern "C" void curve_editor_draw_content(void);
extern "C" void sequencer_draw_content(void);
extern "C" void timeline_draw_content(void);
extern "C" void animation_rigging_draw_content(void);

namespace {
/* Tabs: 0=Editor 1=SM 2=Curves 3=Sequencer 4=Timeline 5=Rigging */
JcePanelTabState g_tabs{ "panel.animation.current_tab", /*max_tab=*/5 };
} /* anonymous namespace */

extern "C" void jce_panel_animation_editor_request_tab(int idx)
{
    jce_panel_tab_request(g_tabs, idx);
}

extern "C" int jce_panel_animation_editor_current_tab(void)
{
    return jce_panel_tab_current(g_tabs);
}

extern "C" void jce_editor_panel_animation_editor(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR);
    if (!vis || !*vis) {
        /* Workbench hidden → the Sequencer tab can no longer restore its
         * live preview itself; flush it here (no-op when not previewing). */
        jce_panel_sequencer_preview_flush();
        return;
    }
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_anim_editor", jce_editor_i18n("animationEditor.title"));
    if (ImGui::Begin(_wt, vis, ImGuiWindowFlags_NoFocusOnAppearing)) {
        jce_panel_tab_ensure_loaded(g_tabs);
        if (ImGui::BeginTabBar("##animation_tabs")) {
            ImGuiTabItemFlags f_ed  = jce_panel_tab_flags(g_tabs, 0);
            ImGuiTabItemFlags f_sm  = jce_panel_tab_flags(g_tabs, 1);
            ImGuiTabItemFlags f_cv  = jce_panel_tab_flags(g_tabs, 2);
            ImGuiTabItemFlags f_sq  = jce_panel_tab_flags(g_tabs, 3);
            ImGuiTabItemFlags f_tl  = jce_panel_tab_flags(g_tabs, 4);
            ImGuiTabItemFlags f_rg  = jce_panel_tab_flags(g_tabs, 5);

            char ed_label[96];
            char sm_label[96];
            char cv_label[96];
            char sq_label[96];
            char tl_label[96];
            char rg_label[96];
            snprintf(ed_label, sizeof(ed_label), "%s###aw_tab_editor",
                     jce_editor_i18n("animationEditor.title"));
            snprintf(sm_label, sizeof(sm_label), "%s###aw_tab_sm",
                     jce_editor_i18n("animatorSM.title"));
            snprintf(cv_label, sizeof(cv_label), "%s###aw_tab_curves",
                     jce_editor_i18n("curveEditor.title"));
            snprintf(sq_label, sizeof(sq_label), "%s###aw_tab_sequencer",
                     jce_editor_i18n("sequencer.title"));
            snprintf(tl_label, sizeof(tl_label), "%s###aw_tab_timeline",
                     jce_editor_i18n("Timeline"));
            snprintf(rg_label, sizeof(rg_label), "%s###aw_tab_rigging",
                     jce_editor_i18n("panel.animRig.title"));

            if (ImGui::BeginTabItem(ed_label, nullptr, f_ed)) {
                jce_panel_tab_set_current(g_tabs, 0);
                draw_editor_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(sm_label, nullptr, f_sm)) {
                jce_panel_tab_set_current(g_tabs, 1);
                animator_sm_draw_content();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(cv_label, nullptr, f_cv)) {
                jce_panel_tab_set_current(g_tabs, 2);
                curve_editor_draw_content();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(sq_label, nullptr, f_sq)) {
                jce_panel_tab_set_current(g_tabs, 3);
                sequencer_draw_content();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(tl_label, nullptr, f_tl)) {
                jce_panel_tab_set_current(g_tabs, 4);
                timeline_draw_content();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(rg_label, nullptr, f_rg)) {
                jce_panel_tab_set_current(g_tabs, 5);
                animation_rigging_draw_content();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
            g_tabs.request = -1;
        }
        /* Sequencer tab deselected → restore its live scene preview. */
        if (g_tabs.current != 3)
            jce_panel_sequencer_preview_flush();
    } else {
        /* Window collapsed → the Sequencer tab isn't drawn either. */
        jce_panel_sequencer_preview_flush();
    }
    ImGui::End();
}
