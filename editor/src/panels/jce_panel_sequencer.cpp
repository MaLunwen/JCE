/*
 * jce_panel_sequencer.cpp -- Multi-track timeline / Sequencer (P1-L).
 *
 * Inspired by Unreal Sequencer / Unity Timeline.  Authors a list of
 * tracks with keyframes over a fixed duration; each keyframe is
 * (time, value).  Tracks come in three flavours:
 *
 *   - Property:  scalar lane evaluated by linear interpolation.
 *   - Event:     marker-only lane (no value) for triggering callbacks.
 *   - Color:     RGB lane stored as 3 floats per key.
 *
 * Features:
 *  - Scrub playhead, Play/Pause/Stop, loop toggle, FPS-based snap.
 *  - Click-drag in track lane to add a keyframe (Shift-drag to move
 *    selected key).  Right-click key for delete.
 *  - Track header: name editing, delete, color swatch (color tracks).
 *  - Structured bindings (entity drop + property combo) evaluated LIVE
 *    against the open scene while scrubbing/playing (edit mode only);
 *    originals are cached and restored on Stop / tab switch / save.
 *  - "Sync to Player" writes the asset path + per-track entity bindings
 *    into the focused entity's SequencePlayerComponent (undoable).
 *  - JSON save/load (.seq.json) and Console logging.
 */

#include "io/jce_editor_file_util.h"
#include "ui/jce_editor_dnd.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_editor_state.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/middleware/scene/jce_scene_sequencer.h>
#include <jce/middleware/scene/jce_sequencer.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

enum TrackType { TT_PROPERTY = 0, TT_EVENT = 1, TT_COLOR = 2 };

struct Key {
    float t   = 0.0f;
    float v   = 0.0f;
    float rgb[3] = { 1.0f, 1.0f, 1.0f };
};

struct Track {
    char  name[48] = "track";
    int   type     = TT_PROPERTY;
    float color[3] = { 0.6f, 0.85f, 1.0f };
    char  binding[128] = {0};   /* legacy free-form binding (round-trip only) */
    uint32_t bind_entity = 0;   /* scene entity (Hierarchy drop), 0 = unbound */
    int      bind_prop   = 0;   /* JceSeqPropId, JCE_SEQ_PROP_NONE = unbound  */
    std::vector<Key> keys;
};

/* Original component value captured before the first preview write to a
 * given (entity, prop) — restored verbatim by preview_restore(). */
struct PreviewEntry {
    uint32_t entity;
    int      prop;       /* JceSeqPropId */
    bool     is_color;
    float    v;
    float    rgb[3];
};

struct Editor {
    std::vector<Track> tracks;
    float duration = 5.0f;
    int   fps      = 30;
    float playhead = 0.0f;
    bool  playing  = false;
    bool  looping  = true;
    int   sel_track = -1;
    int   sel_key   = -1;
    int   drag_track = -1;
    int   drag_key   = -1;
    bool  initialised = false;
    char  path[260] = {0};
    /* horizontal axis */
    float t_view_min = 0.0f;
    float t_view_max = 5.0f;
    /* live scene preview */
    JceSequencer *preview_seq   = nullptr;  /* rebuilt from to_json() text */
    bool          preview_dirty = true;
    std::vector<PreviewEntry> preview_cache;
};

Editor s;

void mark_dirty(void) { s.preview_dirty = true; }

const char *track_type_name(int t)
{
    switch (t) {
        case TT_PROPERTY: return "Property";
        case TT_EVENT:    return "Event";
        case TT_COLOR:    return "Color";
    }
    return "?";
}

void seed_default(void)
{
    if (s.initialised) return;
    s.initialised = true;
    Track a; std::strncpy(a.name, "Position.X", sizeof(a.name) - 1);
    a.keys.push_back({ 0.0f, 0.0f });
    a.keys.push_back({ 2.5f, 5.0f });
    a.keys.push_back({ 5.0f, 0.0f });
    s.tracks.push_back(a);
    Track b; std::strncpy(b.name, "Tint", sizeof(b.name) - 1);
    b.type = TT_COLOR;
    Key k0; k0.t = 0.0f; k0.rgb[0] = 1; k0.rgb[1] = 1; k0.rgb[2] = 1;
    Key k1; k1.t = 5.0f; k1.rgb[0] = 1; k1.rgb[1] = 0.4f; k1.rgb[2] = 0.2f;
    b.keys.push_back(k0); b.keys.push_back(k1);
    s.tracks.push_back(b);
    Track c; std::strncpy(c.name, "Hit",   sizeof(c.name) - 1);
    c.type = TT_EVENT;
    c.keys.push_back({ 1.5f, 0.0f });
    c.keys.push_back({ 3.0f, 0.0f });
    s.tracks.push_back(c);
    s.t_view_max = s.duration;
}

void sort_keys(Track &t)
{
    std::sort(t.keys.begin(), t.keys.end(),
              [](const Key &a, const Key &b) { return a.t < b.t; });
}

JceJson *to_json(void)
{
    JceScene *scene = jce_state_get_scene();
    JceJson *root = jce_json_object();
    jce_json_set_number(root, "duration", s.duration);
    jce_json_set_int   (root, "fps",      s.fps);
    jce_json_set_bool  (root, "loop",     s.looping);
    JceJson *arr = jce_json_array();
    for (auto &t : s.tracks) {
        JceJson *o = jce_json_object();
        jce_json_set_string(o, "name", t.name);
        jce_json_set_int   (o, "type", t.type);
        /* Structured binding (additive keys) + the legacy "binding" string
         * kept as "<id>/<prop>" so older loaders still round-trip. */
        const char *prop_name = jce_seq_prop_name((JceSeqPropId)t.bind_prop);
        if (t.bind_entity != 0 && prop_name[0]) {
            char legacy[160];
            std::snprintf(legacy, sizeof legacy, "%u/%s",
                          t.bind_entity, prop_name);
            jce_json_set_string(o, "binding", legacy);
        } else {
            jce_json_set_string(o, "binding", t.binding);
        }
        jce_json_set_string(o, "bindProp", prop_name);
        jce_json_set_number(o, "bindEntity", (double)t.bind_entity);
        {
            const char *enm = "";
            if (t.bind_entity != 0 && scene) {
                const char *nm = jce_scene_entity_registered_name(
                    scene, jce_state_to_ecs_entity(t.bind_entity));
                if (nm && nm[0]) enm = nm;
            }
            jce_json_set_string(o, "bindEntityName", enm);
        }
        jce_json_set_float_array(o, "color", t.color, 3);
        JceJson *karr = jce_json_array();
        for (auto &k : t.keys) {
            JceJson *ko = jce_json_object();
            jce_json_set_number(ko, "t", k.t);
            jce_json_set_number(ko, "v", k.v);
            jce_json_set_float_array(ko, "rgb", (float *)k.rgb, 3);
            jce_json_array_push(karr, ko);
        }
        jce_json_set_child(o, "keys", karr);
        jce_json_array_push(arr, o);
    }
    jce_json_set_child(root, "tracks", arr);
    return root;
}

void from_json(JceJson *root)
{
    s.tracks.clear();
    s.duration = (float)jce_json_get_number(root, "duration", 5.0);
    s.fps      = jce_json_get_int(root, "fps", 30);
    s.looping  = jce_json_get_bool(root, "loop", 1) != 0;
    JceJson *arr = jce_json_get(root, "tracks");
    if (arr && jce_json_is_array(arr)) {
        int n = jce_json_array_size(arr);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(arr, i);
            if (!o) continue;
            Track t;
            const char *nm = jce_json_get_string(o, "name", "track");
            std::strncpy(t.name, nm ? nm : "track", sizeof(t.name) - 1);
            t.type = jce_json_get_int(o, "type", TT_PROPERTY);
            const char *bd = jce_json_get_string(o, "binding", "");
            std::strncpy(t.binding, bd ? bd : "", sizeof(t.binding) - 1);
            /* Structured binding keys; fall back to parsing a legacy
             * "<digits>/<prop>" binding string (other strings rejected). */
            t.bind_prop = (int)jce_seq_prop_from_name(
                jce_json_get_string(o, "bindProp", ""));
            t.bind_entity =
                (uint32_t)jce_json_get_number(o, "bindEntity", 0.0);
            if (t.bind_entity == 0 && t.bind_prop == JCE_SEQ_PROP_NONE &&
                t.binding[0]) {
                const char *p = t.binding;
                while (*p >= '0' && *p <= '9') ++p;
                if (p != t.binding && *p == '/' && p[1]) {
                    uint32_t id = 0;
                    for (const char *d = t.binding; d < p; ++d)
                        id = id * 10u + (uint32_t)(*d - '0');
                    int prop = (int)jce_seq_prop_from_name(p + 1);
                    if (prop != JCE_SEQ_PROP_NONE) {
                        t.bind_entity = id;
                        t.bind_prop   = prop;
                    }
                }
            }
            jce_json_get_floats(o, "color", t.color, 3, nullptr);
            JceJson *karr = jce_json_get(o, "keys");
            if (karr && jce_json_is_array(karr)) {
                int kn = jce_json_array_size(karr);
                for (int j = 0; j < kn; ++j) {
                    JceJson *ko = jce_json_array_at(karr, j);
                    if (!ko) continue;
                    Key k;
                    k.t = (float)jce_json_get_number(ko, "t", 0.0);
                    k.v = (float)jce_json_get_number(ko, "v", 0.0);
                    jce_json_get_floats(ko, "rgb", k.rgb, 3, nullptr);
                    t.keys.push_back(k);
                }
            }
            sort_keys(t);
            s.tracks.push_back(t);
        }
    }
    s.t_view_min = 0.0f;
    s.t_view_max = s.duration;
    s.sel_track  = -1;
    s.sel_key    = -1;
    mark_dirty();
}

void save_to(const char *path)
{
    if (ed_write_json_to_file(path, to_json()))
        jce_editor_console_log("sequencer saved: %s", path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "sequencer save failed: %s", path);
}

void load_from(const char *path)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "sequencer load failed: %s", path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    from_json(root);
    jce_json_free(root);
    jce_editor_console_log("sequencer loaded: %s (tracks=%d)",
                           path, (int)s.tracks.size());
}

/* ───── Live scene preview ─────────────────────────────────────────
 *
 * The panel's track list is serialized to JSON text and re-parsed through
 * the ENGINE loader (jce_sequencer_load_text) whenever it changes, so the
 * preview evaluates byte-identical curves to the runtime integrator.
 * preview_apply() then writes evaluated values into the live scene via the
 * shared jce_seq_prop_* appliers, caching each (entity, prop)'s original
 * value on first touch; preview_restore() puts everything back.  Preview
 * only runs in edit mode (play state STOPPED) — during Play the runtime's
 * jce_scene_sequencer_update owns the scene. */

void rebuild_preview(void)
{
    if (s.preview_seq) {
        jce_sequencer_free(s.preview_seq);
        s.preview_seq = nullptr;
    }
    JceJson *root = to_json();
    char *txt = jce_json_print(root, false);
    jce_json_free(root);
    if (txt) {
        s.preview_seq = jce_sequencer_load_text(txt, std::strlen(txt));
        jce_json_free_string(txt);
    }
    s.preview_dirty = false;
}

void preview_cache_store(JceScene *scene, uint32_t ent_id, int prop,
                         bool is_color)
{
    for (const PreviewEntry &pe : s.preview_cache)
        if (pe.entity == ent_id && pe.prop == prop)
            return;                       /* original already captured */
    PreviewEntry pe;
    pe.entity   = ent_id;
    pe.prop     = prop;
    pe.is_color = is_color;
    pe.v        = 0.0f;
    pe.rgb[0] = pe.rgb[1] = pe.rgb[2] = 1.0f;
    JceEntity e = jce_state_to_ecs_entity(ent_id);
    bool ok = is_color
        ? jce_seq_prop_get_color(scene, e, (JceSeqPropId)prop, pe.rgb)
        : jce_seq_prop_get_float(scene, e, (JceSeqPropId)prop, &pe.v);
    if (ok)
        s.preview_cache.push_back(pe);
}

void preview_restore(void)
{
    JceScene *scene = jce_state_get_scene();
    if (scene) {
        for (const PreviewEntry &pe : s.preview_cache) {
            JceEntity e = jce_state_to_ecs_entity(pe.entity);
            if (!jce_scene_has_transform(scene, e))
                continue;                 /* dead / scene swapped */
            if (pe.is_color)
                jce_seq_prop_apply_color(scene, e, (JceSeqPropId)pe.prop, pe.rgb);
            else
                jce_seq_prop_apply_float(scene, e, (JceSeqPropId)pe.prop, pe.v);
        }
    }
    s.preview_cache.clear();
}

void preview_apply(float t)
{
    if (jce_state_get_play_state() != JCE_PLAY_STOPPED) return;
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;
    if (s.preview_dirty) rebuild_preview();
    if (!s.preview_seq) return;

    int n = (int)s.tracks.size();
    int en = jce_sequencer_track_count(s.preview_seq);
    if (en < n) n = en;
    for (int i = 0; i < n; ++i) {
        Track &tr = s.tracks[i];
        if (tr.type == TT_EVENT) continue;
        if (tr.bind_entity == 0 || tr.bind_prop == JCE_SEQ_PROP_NONE) continue;
        const JceSeqPropId prop = (JceSeqPropId)tr.bind_prop;
        JceEntity e = jce_state_to_ecs_entity(tr.bind_entity);
        if (!jce_seq_prop_supported(scene, e, prop)) continue;
        if (tr.type == TT_COLOR) {
            if (!jce_seq_prop_is_color(prop)) continue;
            preview_cache_store(scene, tr.bind_entity, tr.bind_prop, true);
            float rgb[3];
            jce_sequencer_track_eval_color(s.preview_seq, i, t, rgb);
            jce_seq_prop_apply_color(scene, e, prop, rgb);
        } else {
            if (jce_seq_prop_is_color(prop)) continue;
            preview_cache_store(scene, tr.bind_entity, tr.bind_prop, false);
            float v = jce_sequencer_track_eval_float(s.preview_seq, i, t);
            jce_seq_prop_apply_float(scene, e, prop, v);
        }
    }
}

float t_to_x(float t, float x0, float w)
{
    float r = (t - s.t_view_min) / (s.t_view_max - s.t_view_min);
    return x0 + r * w;
}
float x_to_t(float x, float x0, float w)
{
    float r = (x - x0) / w;
    return s.t_view_min + r * (s.t_view_max - s.t_view_min);
}

float snap_t(float t)
{
    if (s.fps <= 0) return t;
    float step = 1.0f / (float)s.fps;
    return std::floor(t / step + 0.5f) * step;
}

/* ───── Drawing ────────────────────────────────────────────────── */

/* "Sync to Player": write the asset path + per-track entity bindings into
 * the focused entity's SequencePlayerComponent (creating it with defaults
 * when absent).  Wrapped in a batch edit so it lands on the undo stack. */
void sync_to_player(void)
{
    JceScene *scene = jce_state_get_scene();
    uint32_t focused = jce_state_get_focused();
    if (!scene || focused == 0) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "sequencer: select an entity before Sync to Player");
        return;
    }
    if (!s.path[0]) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "sequencer: set Path (save the sequence) before Sync to Player");
        return;
    }
    JceEntity e = jce_state_to_ecs_entity(focused);

    JceSequencePlayerComponent sp;
    if (JceSequencePlayerComponent *cur = jce_scene_get_sequence_player(scene, e)) {
        sp = *cur;
    } else {
        std::memset(&sp, 0, sizeof sp);
        sp.speed         = 1.0f;
        sp.play_on_awake = true;
    }
    char rel[1024];
    const char *src = jce_editor_path_relative_or(rel, sizeof rel, s.path);
    std::snprintf(sp.seq_path, sizeof sp.seq_path, "%s", src);

    int n = (int)s.tracks.size();
    if (n > JCE_SEQ_PLAYER_MAX_BINDINGS) n = JCE_SEQ_PLAYER_MAX_BINDINGS;
    sp.binding_count = n;
    for (int i = 0; i < n; ++i)
        sp.bindings[i] = (uint64_t)s.tracks[i].bind_entity;

    jce_state_begin_batch_edit();
    jce_scene_set_sequence_player(scene, e, &sp);
    jce_state_end_batch_edit();
    jce_editor_console_log("sequencer: synced %d binding(s) + '%s' to '%s'",
                           n, sp.seq_path,
                           jce_scene_entity_name(scene, e));
}

void draw_toolbar(void)
{
    if (ImGui::Button(jce_editor_i18n_id(s.playing ? "sequencer.button.pause" : "sequencer.button.play", "seq_play"))) s.playing = !s.playing;
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.stop", "seq_stop"))) {
        s.playing = false; s.playhead = 0.0f;
        preview_restore();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n_id("sequencer.field.loop", "seq_loop"), &s.looping))
        mark_dirty();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::DragFloat(jce_editor_i18n_id("sequencer.field.duration", "seq_dur"), &s.duration, 0.1f, 0.1f, 600.0f, "%.2fs")) {
        if (s.t_view_max > s.duration) s.t_view_max = s.duration;
        mark_dirty();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    if (ImGui::DragInt(jce_editor_i18n_id("sequencer.field.fps", "seq_fps"), &s.fps, 1, 1, 240))
        mark_dirty();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat(jce_editor_i18n_id("sequencer.field.playhead", "seq_ph"), &s.playhead, 0.01f, 0.0f, s.duration))
        preview_apply(s.playhead);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.save", "seq_save"))) {
        if (!s.path[0]) std::strncpy(s.path, "untitled.seq.json", sizeof(s.path) - 1);
        save_to(s.path);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.load", "seq_load"))) {
        if (s.path[0]) load_from(s.path);
        else jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                                         "sequencer: set Path before Load");
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.syncToPlayer", "seq_sync")))
        sync_to_player();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(360);
    jce_draw_path_input(jce_editor_i18n_id("sequencer.field.path", "seq_path"), s.path, sizeof(s.path), JcePathKind::FileAbs);
}

void draw_ruler(ImDrawList *dl, ImVec2 origin, float width, float height)
{
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                      jce_theme::header_bg());
    /* Major + minor ticks. */
    float span = s.t_view_max - s.t_view_min;
    if (span <= 0.0f) span = 1.0f;
    float step = 1.0f;
    while (span / step > 12.0f) step *= 2.0f;
    while (span / step < 4.0f && step > 0.05f) step *= 0.5f;
    float ts = std::floor(s.t_view_min / step) * step;
    for (float t = ts; t <= s.t_view_max + 0.0001f; t += step) {
        float x = t_to_x(t, origin.x, width);
        dl->AddLine(ImVec2(x, origin.y + 4),
                    ImVec2(x, origin.y + height),
                    jce_theme::grid_major());
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "%.2fs", t);
        dl->AddText(ImVec2(x + 3, origin.y + 2),
                    jce_theme::text_primary(), lbl);
    }
}

void draw_lane(ImDrawList *dl, int track_idx, ImVec2 origin, float width, float height)
{
    Track &t = s.tracks[track_idx];
    bool sel_track = (track_idx == s.sel_track);
    ImU32 bg = sel_track ? jce_theme::col_from(ImGuiCol_HeaderActive)
                         : jce_theme::track_even();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), bg);
    dl->AddLine(ImVec2(origin.x, origin.y + height),
                ImVec2(origin.x + width, origin.y + height),
                jce_theme::separator());

    /* For property tracks draw a polyline. */
    if (t.type == TT_PROPERTY && t.keys.size() >= 2) {
        float vmin = t.keys[0].v, vmax = t.keys[0].v;
        for (auto &k : t.keys) { vmin = std::min(vmin, k.v); vmax = std::max(vmax, k.v); }
        float vspan = (vmax - vmin); if (vspan < 0.001f) vspan = 1.0f;
        ImVec2 prev;
        bool have_prev = false;
        for (auto &k : t.keys) {
            float x = t_to_x(k.t, origin.x, width);
            float yr = (k.v - vmin) / vspan;
            float y = origin.y + height - 6 - yr * (height - 12);
            if (have_prev)
                dl->AddLine(prev, ImVec2(x, y),
                            ImColor(t.color[0], t.color[1], t.color[2], 1.0f), 1.5f);
            prev = ImVec2(x, y);
            have_prev = true;
        }
    }

    /* Keys. */
    for (int i = 0; i < (int)t.keys.size(); ++i) {
        Key &k = t.keys[i];
        float x = t_to_x(k.t, origin.x, width);
        float y = origin.y + height * 0.5f;
        bool sel = (track_idx == s.sel_track && i == s.sel_key);
        ImU32 fill = (t.type == TT_COLOR)
                     ? ImColor(k.rgb[0], k.rgb[1], k.rgb[2], 1.0f)
                     : ImColor(t.color[0], t.color[1], t.color[2], 1.0f);
        ImU32 border = sel ? jce_theme::selection_outline()
                           : jce_theme::node_outline();
        if (t.type == TT_EVENT) {
            ImVec2 p0(x - 5, y - 6), p1(x + 5, y - 6),
                   p2(x + 5, y + 6), p3(x - 5, y + 6);
            dl->AddQuadFilled(p0, p1, p2, p3, fill);
            dl->AddQuad      (p0, p1, p2, p3, border);
        } else {
            ImVec2 a(x, y - 6), b(x + 6, y), c(x, y + 6), d(x - 6, y);
            dl->AddQuadFilled(a, b, c, d, fill);
            dl->AddQuad      (a, b, c, d, border);
        }
    }
}

void draw_timeline(void)
{
    ImGui::BeginChild("##seq_timeline", ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail  = ImGui::GetContentRegionAvail();
    float header_w = 180.0f;
    float ruler_h  = 22.0f;
    float lane_h   = 28.0f;
    float track_area_x = origin.x + header_w;
    float track_area_w = avail.x - header_w;
    if (track_area_w < 50.0f) track_area_w = 50.0f;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    /* Ruler. */
    draw_ruler(dl, ImVec2(track_area_x, origin.y), track_area_w, ruler_h);
    /* Track headers + lanes. */
    for (int i = 0; i < (int)s.tracks.size(); ++i) {
        ImVec2 hdr_min(origin.x, origin.y + ruler_h + i * lane_h);
        ImVec2 hdr_max(origin.x + header_w, hdr_min.y + lane_h);
        bool seltrack = (i == s.sel_track);
        dl->AddRectFilled(hdr_min, hdr_max,
                          seltrack ? jce_theme::col_from(ImGuiCol_HeaderActive)
                                   : jce_theme::col_from(ImGuiCol_Header));
        char buf[80];
        std::snprintf(buf, sizeof(buf), "%s [%s]", s.tracks[i].name,
                      track_type_name(s.tracks[i].type));
        dl->AddText(ImVec2(hdr_min.x + 6, hdr_min.y + 6),
                    jce_theme::text_primary(), buf);
        draw_lane(dl, i, ImVec2(track_area_x, hdr_min.y), track_area_w, lane_h);
    }

    /* Playhead. */
    float ph_x = t_to_x(s.playhead, track_area_x, track_area_w);
    dl->AddLine(ImVec2(ph_x, origin.y),
                ImVec2(ph_x, origin.y + ruler_h + s.tracks.size() * lane_h),
                jce_theme::playhead(), 2.0f);

    /* Interaction. */
    ImVec2 mp = ImGui::GetIO().MousePos;
    ImGui::InvisibleButton("##seq_canvas",
        ImVec2(avail.x, ruler_h + std::max((int)s.tracks.size(), 1) * lane_h),
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();

    /* Header click → select track. */
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (mp.x < track_area_x && mp.y > origin.y + ruler_h) {
            int row = (int)((mp.y - origin.y - ruler_h) / lane_h);
            if (row >= 0 && row < (int)s.tracks.size()) {
                s.sel_track = row;
                s.sel_key   = -1;
            }
        }
        /* Ruler click → set playhead. */
        else if (mp.y < origin.y + ruler_h && mp.x > track_area_x) {
            float t = x_to_t(mp.x, track_area_x, track_area_w);
            if (t < 0) t = 0;
            if (t > s.duration) t = s.duration;
            s.playhead = snap_t(t);
            preview_apply(s.playhead);
        }
        /* Lane click → select / hit-test key, or insert new key. */
        else if (mp.y >= origin.y + ruler_h && mp.x > track_area_x) {
            int row = (int)((mp.y - origin.y - ruler_h) / lane_h);
            if (row >= 0 && row < (int)s.tracks.size()) {
                Track &tr = s.tracks[row];
                int hit_key = -1;
                for (int i = 0; i < (int)tr.keys.size(); ++i) {
                    float kx = t_to_x(tr.keys[i].t, track_area_x, track_area_w);
                    if (std::fabs(kx - mp.x) < 7.0f) { hit_key = i; break; }
                }
                if (hit_key >= 0) {
                    s.sel_track = row;
                    s.sel_key   = hit_key;
                    s.drag_track = row;
                    s.drag_key   = hit_key;
                } else if (ImGui::GetIO().KeyShift) {
                    /* Shift-click empty area → add new key. */
                    Key nk;
                    nk.t = snap_t(x_to_t(mp.x, track_area_x, track_area_w));
                    if (tr.type != TT_EVENT) nk.v = 0.0f;
                    tr.keys.push_back(nk);
                    sort_keys(tr);
                    mark_dirty();
                    s.sel_track = row;
                    s.sel_key   = (int)tr.keys.size() - 1;
                } else {
                    s.sel_track = row;
                    s.sel_key   = -1;
                }
            }
        }
    }

    /* Drag selected key. */
    if (s.drag_key >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (s.drag_track >= 0 && s.drag_track < (int)s.tracks.size()) {
            Track &tr = s.tracks[s.drag_track];
            if (s.drag_key < (int)tr.keys.size()) {
                float t = snap_t(x_to_t(mp.x, track_area_x, track_area_w));
                if (t < 0) t = 0;
                if (t > s.duration) t = s.duration;
                if (tr.keys[s.drag_key].t != t) {
                    tr.keys[s.drag_key].t = t;
                    mark_dirty();
                }
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (s.drag_key >= 0 && s.drag_track >= 0 &&
            s.drag_track < (int)s.tracks.size())
        {
            sort_keys(s.tracks[s.drag_track]);
            /* Lost selected index after sort: restore by closest match. */
            s.sel_key = -1;
        }
        s.drag_key = -1;
        s.drag_track = -1;
    }

    /* Right-click on key → delete. */
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        int row = (int)((mp.y - origin.y - ruler_h) / lane_h);
        if (row >= 0 && row < (int)s.tracks.size() && mp.x > track_area_x) {
            Track &tr = s.tracks[row];
            for (int i = 0; i < (int)tr.keys.size(); ++i) {
                float kx = t_to_x(tr.keys[i].t, track_area_x, track_area_w);
                if (std::fabs(kx - mp.x) < 7.0f) {
                    tr.keys.erase(tr.keys.begin() + i);
                    if (s.sel_track == row && s.sel_key == i) s.sel_key = -1;
                    mark_dirty();
                    break;
                }
            }
        }
    }

    ImGui::EndChild();
}

void draw_inspector(void)
{
    if (s.sel_track < 0 || s.sel_track >= (int)s.tracks.size()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("sequencer.label.noTrack"));
        return;
    }
    Track &t = s.tracks[s.sel_track];
    ImGui::InputText(jce_editor_i18n_id("sequencer.field.name", "seq_t_name"), t.name, sizeof(t.name));
    if (ImGui::BeginCombo(jce_editor_i18n_id("sequencer.field.type", "seq_t_type"), track_type_name(t.type))) {
        for (int k = 0; k < 3; ++k)
            if (ImGui::Selectable(track_type_name(k), t.type == k) && t.type != k) {
                t.type = k;
                mark_dirty();
            }
        ImGui::EndCombo();
    }

    /* ── Structured binding: entity drop + property combo ──────────
     * (Panel state, NOT scene state — no batch-edit wrap; the undoable
     * write happens in Sync to Player.) */
    if (t.type == TT_EVENT) {
        ImGui::TextDisabled("%s", jce_editor_i18n("sequencer.empty.event"));
    } else {
        JceScene *scene = jce_state_get_scene();
        /* Entity field (entity_field pattern from the Rigging panel). */
        {
            char tgt[160];
            if (t.bind_entity == 0) {
                std::snprintf(tgt, sizeof tgt, "%s",
                              jce_editor_i18n("sequencer.label.propNone"));
            } else {
                const char *nm = scene
                    ? jce_scene_entity_name(scene,
                          jce_state_to_ecs_entity(t.bind_entity)) : NULL;
                if (nm && nm[0]) std::snprintf(tgt, sizeof tgt, "%s", nm);
                else std::snprintf(tgt, sizeof tgt, "Entity #%u", t.bind_entity);
            }
            ImGui::TextUnformatted(jce_editor_i18n("sequencer.field.bindEntity"));
            ImGui::SameLine();
            char btn[192];
            std::snprintf(btn, sizeof btn, "%s###seq_bind_ent", tgt);
            ImGui::Button(btn, ImVec2(-30.0f, 0.0f));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload *pl =
                        ImGui::AcceptDragDropPayload(JCE_DND_ENTITY)) {
                    uint32_t id = *(const uint32_t *)pl->Data;
                    if (id != t.bind_entity) {
                        t.bind_entity = id;
                        mark_dirty();
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (t.bind_entity != 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton("X###seq_bind_ent_clr")) {
                    t.bind_entity = 0;
                    mark_dirty();
                }
            }
        }
        /* Property combo: float props for Property tracks, color props for
         * Color tracks; filtered by jce_seq_prop_supported once an entity
         * is bound. */
        {
            const bool want_color = (t.type == TT_COLOR);
            JceEntity be = t.bind_entity
                ? jce_state_to_ecs_entity(t.bind_entity) : JCE_ENTITY_INVALID;
            const char *cur_name = jce_seq_prop_name((JceSeqPropId)t.bind_prop);
            if (!cur_name[0])
                cur_name = jce_editor_i18n("sequencer.label.propNone");
            if (ImGui::BeginCombo(jce_editor_i18n_id("sequencer.field.bindProp",
                                                     "seq_t_prop"), cur_name)) {
                if (ImGui::Selectable(jce_editor_i18n("sequencer.label.propNone"),
                                      t.bind_prop == JCE_SEQ_PROP_NONE) &&
                    t.bind_prop != JCE_SEQ_PROP_NONE) {
                    t.bind_prop = JCE_SEQ_PROP_NONE;
                    mark_dirty();
                }
                for (int p = JCE_SEQ_PROP_NONE + 1; p < JCE_SEQ_PROP_COUNT; ++p) {
                    const JceSeqPropId id = (JceSeqPropId)p;
                    if (jce_seq_prop_is_color(id) != want_color) continue;
                    if (scene && be != JCE_ENTITY_INVALID &&
                        !jce_seq_prop_supported(scene, be, id)) continue;
                    bool sel = (t.bind_prop == p);
                    if (ImGui::Selectable(jce_seq_prop_name(id), sel) && !sel) {
                        t.bind_prop = p;
                        mark_dirty();
                    }
                }
                ImGui::EndCombo();
            }
        }
    }

    if (ImGui::ColorEdit3(jce_editor_i18n_id("sequencer.field.color", "seq_t_col"), t.color))
        mark_dirty();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.addKey", "seq_addkey"))) {
        Key nk;
        nk.t = snap_t(s.playhead);
        if (t.type == TT_COLOR) {
            nk.rgb[0] = t.color[0]; nk.rgb[1] = t.color[1]; nk.rgb[2] = t.color[2];
        }
        t.keys.push_back(nk);
        sort_keys(t);
        mark_dirty();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.deleteTrack", "seq_deltrk"))) {
        s.tracks.erase(s.tracks.begin() + s.sel_track);
        s.sel_track = -1; s.sel_key = -1;
        mark_dirty();
        return;
    }

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("sequencer.section.keys"));
    if (ImGui::BeginTable("##seq_keys", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn(jce_editor_i18n("sequencer.col.idx"),  ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableSetupColumn(jce_editor_i18n("sequencer.col.time"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(jce_editor_i18n("sequencer.col.value"));
        ImGui::TableSetupColumn(jce_editor_i18n("sequencer.col.op"),   ImGuiTableColumnFlags_WidthFixed, 36);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)t.keys.size(); ++i) {
            Key &k = t.keys[i];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##kt", &k.t, 0.01f, 0.0f, s.duration)) {
                k.t = snap_t(k.t);
                mark_dirty();
            }
            ImGui::TableSetColumnIndex(2);
            if (t.type == TT_PROPERTY) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragFloat("##kv", &k.v, 0.01f))
                    mark_dirty();
            } else if (t.type == TT_COLOR) {
                if (ImGui::ColorEdit3("##krgb", k.rgb,
                                      ImGuiColorEditFlags_NoInputs |
                                      ImGuiColorEditFlags_NoLabel))
                    mark_dirty();
            } else {
                ImGui::TextDisabled("%s", jce_editor_i18n("sequencer.empty.event"));
            }
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton("X")) {
                t.keys.erase(t.keys.begin() + i);
                mark_dirty();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void draw_track_list(void)
{
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.addPropTrack", "seq_addprop"))) {
        Track t;
        std::snprintf(t.name, sizeof(t.name), "Track%d", (int)s.tracks.size());
        s.tracks.push_back(t);
        mark_dirty();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.addColorTrack", "seq_addcol"))) {
        Track t;
        std::snprintf(t.name, sizeof(t.name), "Color%d", (int)s.tracks.size());
        t.type = TT_COLOR;
        s.tracks.push_back(t);
        mark_dirty();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("sequencer.button.addEventTrack", "seq_addevt"))) {
        Track t;
        std::snprintf(t.name, sizeof(t.name), "Event%d", (int)s.tracks.size());
        t.type = TT_EVENT;
        s.tracks.push_back(t);
        mark_dirty();
    }
}

void tick_playback(void)
{
    if (!s.playing) return;
    float dt = ImGui::GetIO().DeltaTime;
    s.playhead += dt;
    if (s.playhead >= s.duration) {
        if (s.looping) s.playhead = std::fmod(s.playhead, s.duration);
        else { s.playhead = s.duration; s.playing = false; }
    }
    preview_apply(s.playhead);
}

void draw_content(void)
{
    seed_default();
    /* Play-mode transition: the runtime integrator owns the scene during
     * Play — put back any edit-mode preview values before it reads them. */
    if (jce_state_get_play_state() != JCE_PLAY_STOPPED &&
        !s.preview_cache.empty())
        preview_restore();
    draw_toolbar();
    ImGui::Separator();
    draw_track_list();
    ImGui::Separator();

    float h = ImGui::GetContentRegionAvail().y;
    float top = h * 0.66f;
    if (top < 200.0f) top = 200.0f;

    ImGui::BeginChild("##seq_top", ImVec2(0, top), false);
    draw_timeline();
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginChild("##seq_bottom", ImVec2(0, 0), true);
    draw_inspector();
    ImGui::EndChild();

    tick_playback();
}

} /* namespace */

extern "C" void sequencer_draw_content(void)
{
    draw_content();
}

/* Restore every component value the live preview touched (no-op when the
 * cache is empty).  Called from the scene-save entry point (so previewed
 * values are never serialized), on scene load (before entity ids are
 * recycled), and by the Animation workbench when the Sequencer tab is
 * hidden or deselected. */
extern "C" void jce_panel_sequencer_preview_flush(void)
{
    preview_restore();
}

extern "C" void jce_editor_panel_sequencer(void)
{
    /* Shim: Sequencer has been merged into the Animation Editor
     * workbench as a tab.  Activating this panel now redirects to
     * that workbench and requests the Sequencer tab.  Symbol kept so
     * menu/hotkey entries registered against JCE_PANEL_SEQUENCER
     * keep working. */
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_SEQUENCER);
    if (!vis || !*vis) return;
    *vis = false;

    bool *ae_vis = jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR);
    if (ae_vis) *ae_vis = true;

    char title[128];
    snprintf(title, sizeof(title), "%s###jce_anim_editor",
             jce_editor_i18n("animationEditor.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_animation_editor_request_tab(3);
}
