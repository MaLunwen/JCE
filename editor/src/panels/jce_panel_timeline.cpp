/*
 * jce_panel_timeline.cpp  Timeline panel (animation keyframes).
 * Extracted from jce_editor_panels.cpp.
 *
 * Now integrates with the engine animation system to display
 * real animation data from entities with Animator or SkeletalAnimator
 * components.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <jce/animation/jce_animation.h>
}

/* ── Timeline state ───────────────────────────────────────────────── */

static struct {
    int   current_frame;
    int   total_frames;
    bool  playing;
    float px_per_frame;
    float playback_speed;
    bool  loop;
    bool  initialized;

    /* Cached from selected entity. */
    uint32_t bound_entity_id;
    char     clip_name[64];
    float    clip_duration;
    int      clip_count;
    int      active_clip;
    bool     has_animator;
    bool     has_skeletal;
} s_tl;

static void ensure_init(void)
{
    if (s_tl.initialized) return;
    memset(&s_tl, 0, sizeof(s_tl));
    s_tl.total_frames   = JCE_TIMELINE_TOTAL_FRAMES;
    s_tl.px_per_frame   = JCE_TIMELINE_PX_PER_FRAME;
    s_tl.playback_speed = 1.0f;
    s_tl.loop           = true;
    s_tl.initialized    = true;
}

static void sync_from_entity(void)
{
    uint32_t focused = jce_state_get_focused();
    if (focused == s_tl.bound_entity_id && focused != 0)
        return;

    s_tl.bound_entity_id = focused;
    s_tl.has_animator    = false;
    s_tl.has_skeletal    = false;
    s_tl.clip_name[0]   = '\0';
    s_tl.clip_duration   = 0.0f;
    s_tl.clip_count      = 0;
    s_tl.active_clip     = 0;

    if (!focused) return;

    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(focused, &comp_count);
    if (!comps) return;

    for (int i = 0; i < comp_count; i++) {
        if (comps[i].type == JCE_COMP_ANIMATOR) {
            s_tl.has_animator = true;
            snprintf(s_tl.clip_name, sizeof(s_tl.clip_name), "%s",
                     comps[i].data.animator.clip_name);
            s_tl.playback_speed = comps[i].data.animator.speed;
            s_tl.loop           = comps[i].data.animator.loop;
            s_tl.playing        = comps[i].data.animator.playing;
        }
        if (comps[i].type == JCE_COMP_SKELETAL_ANIMATOR) {
            s_tl.has_skeletal = true;
            s_tl.clip_count   = comps[i].data.skeletal_animator.clip_count;
            s_tl.active_clip  = comps[i].data.skeletal_animator.active_clip;
            s_tl.playback_speed = comps[i].data.skeletal_animator.speed;
            s_tl.loop           = comps[i].data.skeletal_animator.loop;
            s_tl.playing        = comps[i].data.skeletal_animator.playing;
            if (s_tl.active_clip < s_tl.clip_count)
                snprintf(s_tl.clip_name, sizeof(s_tl.clip_name), "%s",
                         comps[i].data.skeletal_animator.clip_names[s_tl.active_clip]);
        }
    }
}

/* Write playback state back to the entity's component data. */
static void sync_to_entity(void)
{
    if (!s_tl.bound_entity_id) return;

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
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_timeline_content(void)
{
    ensure_init();
    sync_from_entity();
    char lbl[128];

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
    if (ImGui::SmallButton("|<")) s_tl.current_frame = 0;
    ImGui::SameLine();
    if (ImGui::SmallButton("<<")) {
        s_tl.current_frame -= 60;
        if (s_tl.current_frame < 0) s_tl.current_frame = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(s_tl.playing ? "||" : ">")) {
        s_tl.playing = !s_tl.playing;
        sync_to_entity();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">>")) {
        s_tl.current_frame += 60;
        if (s_tl.current_frame >= s_tl.total_frames)
            s_tl.current_frame = s_tl.total_frames - 1;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">|"))
        s_tl.current_frame = s_tl.total_frames - 1;

    ImGui::SameLine();
    ImGui::Text("%s: %d / %d", jce_editor_i18n("timeline.frame"),
               s_tl.current_frame, s_tl.total_frames);

    /* Speed and loop controls. */
    ImGui::SameLine(0, 20);
    ImGui::PushItemWidth(80);
    if (ImGui::DragFloat(jce_editor_i18n("timeline.speed"), &s_tl.playback_speed,
                         0.01f, 0.0f, 10.0f))
        sync_to_entity();
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Checkbox(jce_editor_i18n("timeline.loop"), &s_tl.loop))
        sync_to_entity();

    /* Clip selector for skeletal animator. */
    if (s_tl.has_skeletal && s_tl.clip_count > 1) {
        ImGui::SameLine(0, 20);
        ImGui::PushItemWidth(120);
        snprintf(lbl, sizeof(lbl), "%s#", jce_editor_i18n("timeline.clip"));
        if (ImGui::InputInt(lbl, &s_tl.active_clip)) {
            if (s_tl.active_clip < 0) s_tl.active_clip = 0;
            if (s_tl.active_clip >= s_tl.clip_count)
                s_tl.active_clip = s_tl.clip_count - 1;
            sync_to_entity();
        }
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

    /* Dynamic tracks based on what's available. */
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
    float total_w = s_tl.total_frames * s_tl.px_per_frame;

    /* Ruler marks */
    for (int f = 0; f <= s_tl.total_frames; f += 10) {
        float x = origin.x + f * s_tl.px_per_frame;
        bool major = (f % 60 == 0);
        float h = major ? 16.0f : 8.0f;
        dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + h),
                    major ? IM_COL32(200, 200, 200, 255) : IM_COL32(100, 100, 100, 255));
        if (major) {
            char label[16];
            snprintf(label, sizeof(label), "%d", f);
            dl->AddText(ImVec2(x + 2, origin.y), IM_COL32(200, 200, 200, 255), label);
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
    float ph_x = origin.x + s_tl.current_frame * s_tl.px_per_frame;
    dl->AddLine(ImVec2(ph_x, origin.y),
                ImVec2(ph_x, origin.y + avail.y),
                ImGui::ColorConvertFloat4ToU32(JCE_COLOR_TL_PLAYHEAD), 2.0f);

    ImGui::Dummy(ImVec2(total_w, 80));

    /* Click to set playhead */
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float mx = ImGui::GetMousePos().x - origin.x;
        int f = (int)(mx / s_tl.px_per_frame);
        if (f >= 0 && f < s_tl.total_frames)
            s_tl.current_frame = f;
    }

    ImGui::EndChild();

    /* Advance frame if playing */
    if (s_tl.playing) {
        s_tl.current_frame++;
        if (s_tl.current_frame >= s_tl.total_frames) {
            if (s_tl.loop)
                s_tl.current_frame = 0;
            else {
                s_tl.current_frame = s_tl.total_frames - 1;
                s_tl.playing = false;
                sync_to_entity();
            }
        }
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
