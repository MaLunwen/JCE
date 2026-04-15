/*
 * jce_panel_timeline.cpp  Timeline panel (animation keyframes).
 * Extracted from jce_editor_panels.cpp.
 *
 * Time-based timeline that syncs with the SkeletalAnimator component
 * and the scene render model cache for real-time animation preview.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"
#include "scene/jce_editor_scene_render.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

extern "C" {
#include <jce/animation/jce_animation.h>
#include <jce/graphics/jce_model.h>
}

/* ── Timeline state ───────────────────────────────────────────────── */

static struct {
    float current_time;     /* seconds */
    float duration;         /* clip duration in seconds */
    bool  playing;
    float px_per_sec;       /* pixels per second for ruler */
    float playback_speed;
    bool  loop;
    bool  initialized;

    /* Cached from selected entity. */
    uint32_t bound_entity_id;
    char     clip_name[64];
    char     clip_names[8][64];
    int      clip_count;
    int      active_clip;
    bool     has_animator;
    bool     has_skeletal;
    char     skeleton_path[128];
} s_tl;

static void ensure_init(void)
{
    if (s_tl.initialized) return;
    memset(&s_tl, 0, sizeof(s_tl));
    s_tl.px_per_sec      = 120.0f;
    s_tl.playback_speed  = 1.0f;
    s_tl.loop            = true;
    s_tl.initialized     = true;
}

static void sync_from_entity(void)
{
    uint32_t focused = jce_state_get_focused();
    s_tl.bound_entity_id = focused;
    s_tl.has_animator    = false;
    s_tl.has_skeletal    = false;
    s_tl.clip_name[0]   = '\0';
    memset(s_tl.clip_names, 0, sizeof(s_tl.clip_names));
    s_tl.duration        = 0.0f;
    s_tl.clip_count      = 0;
    s_tl.active_clip     = 0;
    s_tl.skeleton_path[0] = '\0';

    if (!focused) return;

    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(focused, &comp_count);
    if (!comps) return;

    for (int i = 0; i < comp_count; i++) {
        if (comps[i].type == JCE_COMP_ANIMATOR) {
            s_tl.has_animator = true;
            snprintf(s_tl.clip_name, sizeof(s_tl.clip_name), "%s",
                     comps[i].data.animator.clip_name);
            if (comps[i].data.animator.speed <= 0.0f)
                comps[i].data.animator.speed = 1.0f;
            s_tl.playback_speed = comps[i].data.animator.speed;
            s_tl.loop           = comps[i].data.animator.loop;
            s_tl.playing        = comps[i].data.animator.playing;
        }
        if (comps[i].type == JCE_COMP_SKELETAL_ANIMATOR) {
            s_tl.has_skeletal = true;
            s_tl.clip_count   = comps[i].data.skeletal_animator.clip_count;
            if (s_tl.clip_count < 0) s_tl.clip_count = 0;
            if (s_tl.clip_count > 8) s_tl.clip_count = 8;
            s_tl.active_clip  = comps[i].data.skeletal_animator.active_clip;
            if (s_tl.active_clip < 0) s_tl.active_clip = 0;
            if (s_tl.clip_count > 0 && s_tl.active_clip >= s_tl.clip_count)
                s_tl.active_clip = s_tl.clip_count - 1;
            if (comps[i].data.skeletal_animator.speed <= 0.0f)
                comps[i].data.skeletal_animator.speed = 1.0f;
            s_tl.playback_speed = comps[i].data.skeletal_animator.speed;
            s_tl.loop           = comps[i].data.skeletal_animator.loop;
            s_tl.playing        = comps[i].data.skeletal_animator.playing;
            snprintf(s_tl.skeleton_path, sizeof(s_tl.skeleton_path), "%s",
                     comps[i].data.skeletal_animator.skeleton_path);
            for (int ci = 0; ci < s_tl.clip_count && ci < 8; ci++) {
                snprintf(s_tl.clip_names[ci], sizeof(s_tl.clip_names[ci]), "%s",
                         comps[i].data.skeletal_animator.clip_names[ci]);
            }
            if (s_tl.active_clip < s_tl.clip_count)
                snprintf(s_tl.clip_name, sizeof(s_tl.clip_name), "%s",
                         comps[i].data.skeletal_animator.clip_names[s_tl.active_clip]);

            /* Query real clip duration from model cache. */
            JceModel *mdl = jce_editor_scene_get_model(
                s_tl.skeleton_path, s_tl.bound_entity_id);
            if (mdl && s_tl.clip_count > 0) {
                JceAnimClip *clip = jce_model_get_anim(mdl,
                    (uint32_t)s_tl.active_clip);
                if (clip)
                    s_tl.duration = jce_anim_clip_duration(clip);
            }
        }
    }
}

/* Write playback state back to the entity's component data. */
static void sync_to_entity(void)
{
    if (!s_tl.bound_entity_id) return;

    if (s_tl.playback_speed <= 0.0f)
        s_tl.playback_speed = 1.0f;

    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(s_tl.bound_entity_id, &comp_count);
    if (!comps) return;

    for (int i = 0; i < comp_count; i++) {
        if (comps[i].type == JCE_COMP_ANIMATOR) {
            comps[i].data.animator.speed   = s_tl.playback_speed;
            comps[i].data.animator.loop    = s_tl.loop;
            comps[i].data.animator.playing = s_tl.playing;
        }
        if (comps[i].type == JCE_COMP_SKELETAL_ANIMATOR) {
            comps[i].data.skeletal_animator.speed       = s_tl.playback_speed;
            comps[i].data.skeletal_animator.loop        = s_tl.loop;
            comps[i].data.skeletal_animator.playing     = s_tl.playing;
            comps[i].data.skeletal_animator.active_clip = s_tl.active_clip;
        }
    }

    /* Sync scrubbed time to animation player in model cache. */
    if (s_tl.has_skeletal && s_tl.skeleton_path[0]) {
        JceAnimPlayer *pl = jce_editor_scene_get_anim_player(
            s_tl.skeleton_path, s_tl.bound_entity_id);
        if (pl)
            jce_anim_player_set_time(pl, s_tl.current_time);
    }
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_timeline_content(void)
{
    ensure_init();
    sync_from_entity();

    float dur = s_tl.duration > 0.0f ? s_tl.duration : 1.0f;

    /* Read current time from animation player if available. */
    if (s_tl.has_skeletal && s_tl.skeleton_path[0] && s_tl.playing) {
        JceAnimPlayer *pl = jce_editor_scene_get_anim_player(
            s_tl.skeleton_path, s_tl.bound_entity_id);
        if (pl)
            s_tl.current_time = jce_anim_player_get_time(pl);
    }

    /* Info bar: show bound entity name. */
    if (s_tl.bound_entity_id) {
        JceEntityInfo *ent = jce_state_get_entity(s_tl.bound_entity_id);
        if (ent) {
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s: %s",
                               jce_editor_i18n("timeline.entity"), ent->name);
            ImGui::SameLine();
            if (s_tl.clip_name[0])
                ImGui::Text("| %s: %s", jce_editor_i18n("timeline.clip"), s_tl.clip_name);
            else
                ImGui::TextDisabled("| %s", jce_editor_i18n("timeline.noClipBound"));
        }
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("timeline.selectAnimatorHint"));
    }

    /* Playback transport */
    if (ImGui::SmallButton("|<")) { s_tl.current_time = 0.0f; sync_to_entity(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("<<")) {
        s_tl.current_time -= 1.0f;
        if (s_tl.current_time < 0.0f) s_tl.current_time = 0.0f;
        sync_to_entity();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(s_tl.playing ? "||" : ">")) {
        s_tl.playing = !s_tl.playing;
        sync_to_entity();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">>")) {
        s_tl.current_time += 1.0f;
        if (s_tl.current_time > dur) s_tl.current_time = dur;
        sync_to_entity();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">|")) { s_tl.current_time = dur; sync_to_entity(); }

    ImGui::SameLine();
    ImGui::Text("%.2fs / %.2fs", s_tl.current_time, dur);

    /* Speed and loop controls. */
    ImGui::SameLine(0, 20);
    ImGui::PushItemWidth(80);
    if (ImGui::DragFloat(jce_editor_i18n("timeline.speed"), &s_tl.playback_speed,
                         0.01f, 0.01f, 10.0f))
        sync_to_entity();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n("timeline.loop"), &s_tl.loop))
        sync_to_entity();

    /* Clip selector for skeletal animator. */
    if (s_tl.has_skeletal && s_tl.clip_count > 1) {
        ImGui::SameLine(0, 20);
        ImGui::PushItemWidth(120);
        int prev_clip = s_tl.active_clip;
        if (ImGui::Combo(jce_editor_i18n("timeline.clip"), &s_tl.active_clip,
            [](void *data, int idx) -> const char* {
                auto *names = (char (*)[64])data;
                return names[idx][0] ? names[idx] : "clip";
            },
            s_tl.clip_names, s_tl.clip_count)) {
            s_tl.current_time = 0.0f;
            sync_to_entity();
            /* Force resync of clip name / duration on next frame. */
            s_tl.bound_entity_id = 0;
        }
        if (s_tl.active_clip != prev_clip && s_tl.active_clip < s_tl.clip_count)
            snprintf(s_tl.clip_name, sizeof(s_tl.clip_name), "%s",
                     s_tl.clip_names[s_tl.active_clip]);
        ImGui::PopItemWidth();
    }

    ImGui::Separator();

    /* Two-column layout: tracks | timeline */
    float track_list_w = 150.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    /* Left: Track list */
    ImGui::BeginChild("TrackList", ImVec2(track_list_w, avail.y), ImGuiChildFlags_Borders);
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s", jce_editor_i18n("timeline.tracks"));
    ImGui::Separator();

    ImGui::Selectable(jce_editor_i18n("transform.title"));
    if (s_tl.has_animator || s_tl.has_skeletal)
        ImGui::Selectable(jce_editor_i18n("timeline.animation"));
    ImGui::Selectable(jce_editor_i18n("timeline.events"));

    ImGui::EndChild();

    ImGui::SameLine();

    /* Right: Timeline area */
    ImGui::BeginChild("TimelineArea", ImVec2(0, avail.y), ImGuiChildFlags_Borders,
                       ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float total_w = dur * s_tl.px_per_sec;
    if (total_w < 120.0f) total_w = 120.0f;

    /* Ruler marks (every 0.1s minor, every 1.0s major) */
    {
        float step_minor = 0.1f;
        float step_major = 1.0f;
        for (float t = 0.0f; t <= dur + 0.001f; t += step_minor) {
            float x = origin.x + t * s_tl.px_per_sec;
            bool major = (fmodf(t + 0.001f, step_major) < step_minor * 0.5f);
            float h = major ? 16.0f : 8.0f;
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + h),
                        major ? IM_COL32(200, 200, 200, 255)
                              : IM_COL32(100, 100, 100, 255));
            if (major) {
                char label[16];
                snprintf(label, sizeof(label), "%.0fs", t);
                dl->AddText(ImVec2(x + 2, origin.y), IM_COL32(200, 200, 200, 255), label);
            }
        }
    }

    /* Track rows */
    float row_y = origin.y + 24.0f;
    int track_count = 2 + (s_tl.has_animator || s_tl.has_skeletal ? 1 : 0);
    for (int t = 0; t < track_count; t++) {
        ImU32 bg = (t % 2 == 0) ? ImGui::ColorConvertFloat4ToU32(JCE_COLOR_TL_TRACK_EVEN)
                                 : ImGui::ColorConvertFloat4ToU32(JCE_COLOR_TL_TRACK_ODD);
        dl->AddRectFilled(ImVec2(origin.x, row_y),
                          ImVec2(origin.x + total_w, row_y + 24.0f), bg);
        row_y += 24.0f;
    }

    /* Playhead */
    float ph_x = origin.x + s_tl.current_time * s_tl.px_per_sec;
    dl->AddLine(ImVec2(ph_x, origin.y),
                ImVec2(ph_x, origin.y + avail.y),
                ImGui::ColorConvertFloat4ToU32(JCE_COLOR_TL_PLAYHEAD), 2.0f);

    ImGui::Dummy(ImVec2(total_w, 80));

    /* Click / drag to scrub playhead */
    if (ImGui::IsWindowHovered() &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         (ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)))) {
        float mx = ImGui::GetMousePos().x - origin.x;
        float t = mx / s_tl.px_per_sec;
        if (t < 0.0f) t = 0.0f;
        if (t > dur)  t = dur;
        s_tl.current_time = t;
        sync_to_entity();
    }

    ImGui::EndChild();

    /* Read-back time from player when playing (player is advanced by draw). */
    if (s_tl.playing && s_tl.has_skeletal && s_tl.skeleton_path[0]) {
        JceAnimPlayer *pl = jce_editor_scene_get_anim_player(
            s_tl.skeleton_path, s_tl.bound_entity_id);
        if (pl)
            s_tl.current_time = jce_anim_player_get_time(pl);
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_timeline(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###Timeline", jce_editor_i18n("Timeline"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_timeline_content();
    ImGui::End();
}
