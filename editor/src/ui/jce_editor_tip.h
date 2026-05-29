/* jce_editor_tip.h — tiny inline helpers for the common hover-tooltip idiom.
 *
 * Replaces the repetitive:
 *     if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
 *
 * Skips empty / null text so call sites do not need to pre-check.
 */
#ifndef JCE_EDITOR_TIP_H
#define JCE_EDITOR_TIP_H

#include <jce/tools/jce_imgui.hpp>

namespace jce_editor {

inline void help_tip(const char *text)
{
    if (text && text[0] && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
}

inline void help_tip_delayed(const char *text)
{
    if (text && text[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", text);
}

} // namespace jce_editor

#endif /* JCE_EDITOR_TIP_H */
