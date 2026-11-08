#include "jce_editor_font_picker.h"
#include "core/jce_editor_i18n.h"
#include <jce/tools/jce_imgui.hpp>
#include <jce/os/core/jce_defs.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>

static bool font_picker_path_equal(const char *a, const char *b)
{
    const bool windows = JCE_PLATFORM_WINDOWS != 0;
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (windows) {
            if (x == '\\') x = '/';
            if (y == '\\') y = '/';
            x = (unsigned char)std::tolower(x); y = (unsigned char)std::tolower(y);
        }
        if (x != y) return false;
    }
    return *a == *b;
}

static bool font_picker_contains(const char *text, const char *filter)
{
    if (!filter[0]) return true;
    const char *end = text + std::strlen(text);
    const char *filter_end = filter + std::strlen(filter);
    return std::search(text, end, filter, filter_end,
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }) != end;
}

bool jce_editor_draw_font_picker(const char *label, char *path, size_t capacity,
    const JceFontEntry *entries, int count, const char *automatic_label)
{
    if (!label || !path || !capacity || !entries || count < 0) return false;
    int selected = -1;
    for (int i = 0; i < count && path[0]; ++i)
        if (font_picker_path_equal(path, entries[i].path)) { selected = i; break; }
    // An explicit unlisted path is still explicit; never display it as Auto.
    const char *preview = path[0] ? (selected >= 0 ? entries[selected].display_name : path)
                                  : automatic_label;
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, 320));
    if (ImGui::BeginCombo(label, preview)) {
        static char filter[128] = {};
        if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::InputTextWithHint("##Search", jce_editor_i18n("common.search"), filter, sizeof filter);
        if (ImGui::Selectable(automatic_label, !path[0])) {
            changed = path[0] != 0; path[0] = 0;
        }
        for (int i = 0; i < count; ++i) {
            const JceFontEntry &entry = entries[i];
            if (!font_picker_contains(entry.display_name, filter) &&
                !font_picker_contains(entry.path, filter)) continue;
            const size_t length = std::strlen(entry.path);
            ImGui::PushID(i);
            ImGui::BeginDisabled(length >= capacity);
            if (ImGui::Selectable(entry.display_name, i == selected)) {
                changed = !font_picker_path_equal(path, entry.path);
                std::memcpy(path, entry.path, length + 1);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", entry.path);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (path[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path);
    ImGui::PopID();
    return changed;
}
