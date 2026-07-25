/*
 * jce_panel_inspector_add_component.cpp
 *
 * "Add Component" button + filterable popup menu.  Carved out of
 * jce_panel_inspector.cpp to keep the dispatcher under its size budget.
 *
 * The popup body uses the editor component registry so flag-backed and
 * synthetic components follow the same identity path.
 */

#include "jce_panel_inspector_common.h"
#include "jce_panel_common.h"

namespace {

bool component_matches_filter(const JceEditorComponentDescriptor *desc,
                              const char *filter)
{
    if (!desc || !filter || !*filter)
        return true;

    if (jce_panel_contains_ci(desc->display_name, filter))
        return true;

    if (!desc->i18n_key)
        return false;

    int n_loc = jce_editor_i18n_locale_count();
    for (int li = 0; li < n_loc; ++li) {
        const char *loc_name = jce_editor_i18n_lookup_locale(
            (JceLocale)li, desc->i18n_key);
        if (jce_panel_contains_ci(loc_name, filter))
            return true;
    }
    return false;
}

bool component_already_present(const JceEditorComponentDescriptor *desc,
                               JceScene *scene,
                               JceEntity entity,
                               uint64_t flags)
{
    if (!desc)
        return true;
    if ((desc->legacy_flag & INSP_LIGHT_MASK) != 0)
        return (flags & INSP_LIGHT_MASK) != 0;
    if (!scene || desc->comp_id == JCE_COMP_ID_INVALID)
        return true;
    return jce_scene_has_comp(scene, entity, desc->comp_id);
}

void add_component_id(uint32_t focused, int comp_id)
{
    /* One path for every row: jce_state_add_component_id resolves the
     * dense comp_id through the editor registry and runs the row's
     * registered default-init (jce_editor_component_defaults.cpp). */
    jce_state_add_component_id(focused, comp_id);
}

}  /* anonymous namespace */

void insp_add_component_button_and_popup(uint32_t focused, uint64_t flags)
{
    ImGui::Spacing();
    float btn_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(jce_editor_i18n("inspector.addComponent"), ImVec2(btn_w, 0))) {
        ImGui::OpenPopup("AddComponentPopup");
    }

    /* Force a sensible default size on the popup's first appearing frame.
       Without this the popup auto-sizes to its (initially zero) content
       width, so the BeginChild list inside collapses to a few pixels and
       the items become invisible — only the *second* open recovers
       because ImGui then reuses the stored window size. */
    ImGui::SetNextWindowSize(ImVec2(280.0f, 260.0f), ImGuiCond_Appearing);

    if (ImGui::BeginPopup("AddComponentPopup")) {
        static char s_addcomp_filter[64] = {0};
        static bool s_addcomp_focus = false;
        if (ImGui::IsWindowAppearing()) {
            s_addcomp_filter[0] = '\0';
            s_addcomp_focus = true;
        }
        if (s_addcomp_focus) {
            ImGui::SetKeyboardFocusHere();
            s_addcomp_focus = false;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##addcomp_search",
            jce_editor_i18n("inspector.searchComponents"),
            s_addcomp_filter, sizeof(s_addcomp_filter));

        ImGui::BeginChild("##addcomp_list", ImVec2(0, 200), false);
        bool have_first_match = false;
        int first_match = JCE_COMP_ID_INVALID;
        JceScene *scene = jce_state_get_scene();
        JceEntity ce = jce_state_to_ecs_entity(focused);

        int n_desc = jce_editor_component_descriptor_count();
        for (int i = 0; i < n_desc; i++) {
            const JceEditorComponentDescriptor *desc =
                jce_editor_component_descriptor_at(i);
            if (!desc || !desc->addable)
                continue;
            if (component_already_present(desc, scene, ce, flags))
                continue;
            if (!component_matches_filter(desc, s_addcomp_filter))
                continue;

            if (!have_first_match) {
                have_first_match = true;
                first_match = desc->comp_id;
            }

            if (ImGui::MenuItem(desc->display_name)) {
                add_component_id(focused, desc->comp_id);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();

        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && have_first_match) {
            add_component_id(focused, first_match);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
