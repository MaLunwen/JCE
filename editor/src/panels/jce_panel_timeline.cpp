/*
 * jce_panel_timeline.cpp  Timeline panel (animation keyframes).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

/* ── Timeline state ───────────────────────────────────────────────── */

static struct {
    int   current_frame;
    int   total_frames;
    bool  playing;
    float px_per_frame;
    bool  initialized;
} s_tl;

static void ensure_init(void)
{
    if (s_tl.initialized) return;
    memset(&s_tl, 0, sizeof(s_tl));
    s_tl.total_frames = JCE_TIMELINE_TOTAL_FRAMES;
    s_tl.px_per_frame = JCE_TIMELINE_PX_PER_FRAME;
    s_tl.initialized  = true;
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_timeline_content(void)
{
    ensure_init();

    /* Playback transport */
    if (ImGui::SmallButton("|<")) s_tl.current_frame = 0;
    ImGui::SameLine();
    if (ImGui::SmallButton("<<")) {
        s_tl.current_frame -= 60;
        if (s_tl.current_frame < 0) s_tl.current_frame = 0;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(s_tl.playing ? "||" : ">"))
        s_tl.playing = !s_tl.playing;
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
    ImGui::Text("Frame: %d / %d", s_tl.current_frame, s_tl.total_frames);

    ImGui::Separator();

    /* Two-column layout: tracks | timeline */
    float track_list_w = 150.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    /* Left: Track list */
    ImGui::BeginChild("TrackList", ImVec2(track_list_w, avail.y), ImGuiChildFlags_Borders);
    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "Tracks");
    ImGui::Separator();

    const char *tracks[] = { "Transform", "Animation", "Events" };
    for (int i = 0; i < 3; i++)
        ImGui::Selectable(tracks[i]);
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
    for (int t = 0; t < 3; t++) {
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
        if (s_tl.current_frame >= s_tl.total_frames)
            s_tl.current_frame = 0;
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_timeline(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE);
    if (!*vis) return;

    if (ImGui::Begin("Timeline###Timeline", vis))
        jce_editor_panel_timeline_content();
    ImGui::End();
}
