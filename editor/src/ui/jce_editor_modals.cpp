/* jce_editor_modals.cpp — implementation of reusable modal helpers. */

#include "ui/jce_editor_modals.h"
#include "core/jce_editor_i18n.h"

#include <cstdio>

namespace jce_modal {

Result confirm_delete(bool *p_open,
                      const char *popup_id,
                      const char *confirm_key,
                      const char *cancel_key,
                      float width,
                      std::function<void()> draw_body)
{
    if (!p_open || !*p_open) return NONE;

    if (!ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width > 0.0f ? width : 400.0f, 0.0f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    char title[256];
    std::snprintf(title, sizeof(title), "%s%s",
                  jce_editor_i18n("dialog.confirmDelete"), popup_id);

    /* Mirror p_open into a local so ImGui's close-X writes to a value
       we control; we update *p_open on the result branches below. */
    bool keep_open = true;
    if (!ImGui::BeginPopupModal(title, &keep_open,
                                ImGuiWindowFlags_NoCollapse
                              | ImGuiWindowFlags_NoDocking
                              | ImGuiWindowFlags_AlwaysAutoResize)) {
        /* ImGui closed it (X button) — propagate cancel out. */
        if (!keep_open) { *p_open = false; return CANCEL; }
        return NONE;
    }

    if (draw_body) draw_body();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const float btn_w  = 120.0f;
    const float gap    = ImGui::GetStyle().ItemSpacing.x;
    const float row_w  = btn_w * 2.0f + gap;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                         + ImGui::GetContentRegionAvail().x - row_w);

    Result out = NONE;

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.80f, 0.20f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.30f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.70f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button(jce_editor_i18n(confirm_key), ImVec2(btn_w, 0))) {
        out = CONFIRM;
        ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n(cancel_key), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        out = CANCEL;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();

    if (out != NONE) *p_open = false;
    return out;
}

} /* namespace jce_modal */
