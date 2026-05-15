/*
 * jce_editor_playmode_tint.cpp  ImGui style override for play mode.
 *
 * Three style slots get tinted:
 *   ImGuiCol_WindowBg   — dock + panel background
 *   ImGuiCol_TitleBgActive — focused window header
 *   ImGuiCol_TabActive    — currently-selected dock tab
 *
 * Push count tracked statically so pop() is safe to call even if
 * push() was a no-op (playing was false).
 */

#include "jce_editor_playmode_tint.h"

#include <jce/tools/jce_imgui.hpp>

namespace {

float s_tint_r = 1.00f;
float s_tint_g = 0.78f;
float s_tint_b = 0.40f;
int   s_pushed = 0;

ImVec4 tint_overlay(ImVec4 base, float strength)
{
    base.x = base.x * (1.0f - strength) + s_tint_r * strength;
    base.y = base.y * (1.0f - strength) + s_tint_g * strength;
    base.z = base.z * (1.0f - strength) + s_tint_b * strength;
    return base;
}

} /* namespace */

extern "C" void jce_editor_playmode_tint_set_color(float r, float g, float b)
{
    s_tint_r = r;
    s_tint_g = g;
    s_tint_b = b;
}

extern "C" void jce_editor_playmode_tint_push(bool playing)
{
    if (!playing) { s_pushed = 0; return; }
    ImGuiStyle &style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                           tint_overlay(style.Colors[ImGuiCol_WindowBg], 0.18f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,
                           tint_overlay(style.Colors[ImGuiCol_TitleBgActive], 0.40f));
    ImGui::PushStyleColor(ImGuiCol_TabActive,
                           tint_overlay(style.Colors[ImGuiCol_TabActive], 0.40f));
    s_pushed = 3;
}

extern "C" void jce_editor_playmode_tint_pop(bool playing)
{
    (void)playing;
    if (s_pushed > 0) {
        ImGui::PopStyleColor(s_pushed);
        s_pushed = 0;
    }
}
