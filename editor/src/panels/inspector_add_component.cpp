/*
 * inspector_add_component.cpp
 *
 * "Add Component" button + filterable popup menu.  Carved out of
 * jce_panel_inspector.cpp to keep the dispatcher under its size budget.
 *
 * The popup body uses the editor component registry so flag-backed and
 * synthetic components follow the same identity path.
 */

#include "jce_panel_inspector_common.h"

namespace {

bool substr_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle)
        return false;
    for (const char *h = hay; *h; ++h) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b) {
            unsigned char ca = (unsigned char)*a;
            unsigned char cb = (unsigned char)*b;
            if (ca >= 'A' && ca <= 'Z')
                ca = (unsigned char)(ca + ('a' - 'A'));
            if (cb >= 'A' && cb <= 'Z')
                cb = (unsigned char)(cb + ('a' - 'A'));
            if (ca != cb)
                break;
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

bool component_matches_filter(const JceEditorComponentDescriptor *desc,
                              const char *filter)
{
    if (!desc || !filter || !*filter)
        return true;

    if (substr_ci(desc->display_name, filter))
        return true;

    if (!desc->i18n_key)
        return false;

    int n_loc = jce_editor_i18n_locale_count();
    for (int li = 0; li < n_loc; ++li) {
        const char *loc_name = jce_editor_i18n_lookup_locale(
            (JceLocale)li, desc->i18n_key);
        if (substr_ci(loc_name, filter))
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
    return jce_editor_component_slot_present(scene, entity, flags, desc->slot);
}

void add_component_slot(uint32_t focused, JceEditorComponentSlot slot)
{
    const JceEditorComponentDescriptor *desc =
        jce_editor_component_find(slot);
    if (!desc)
        return;

    if (desc->legacy_flag != 0) {
        jce_state_add_component(focused, desc->legacy_flag);
        return;
    }

    if (jce_editor_component_slot_is_video_player(slot)) {
        JceScene *scene = jce_state_get_scene();
        JceEntity ce = jce_state_to_ecs_entity(focused);
        if (!scene || !ce || jce_scene_has_video_player(scene, ce))
            return;
        JceVideoPlayerComponent def;
        memset(&def, 0, sizeof(def));
        def.autoplay   = true;
        /* JCE_TEXTURE_INVALID is a C compound literal (illegal in C++ → C4576);
         * use the C++ braced-init equivalent. */
        def.output_tex = JceTexture{ UINT16_MAX };
        jce_state_begin_batch_edit();
        jce_scene_set_video_player(scene, ce, &def);
        jce_state_end_batch_edit();
        return;
    }

    if (jce_editor_component_slot_is_nav_agent(slot)) {
        JceScene *scene = jce_state_get_scene();
        JceEntity ce = jce_state_to_ecs_entity(focused);
        if (!scene || !ce || jce_scene_has_nav_agent(scene, ce))
            return;
        JceNavAgentComponent def;
        memset(&def, 0, sizeof(def));
        def.radius          = 0.5f;
        def.height          = 2.0f;
        def.max_speed       = 3.5f;
        def.max_accel       = 8.0f;
        def.arrive_radius   = 1.5f;
        def.waypoint_radius = 0.5f;
        def.auto_repath     = true;
        def.enabled         = true;
        jce_state_begin_batch_edit();
        jce_scene_set_nav_agent(scene, ce, &def);
        jce_state_end_batch_edit();
        return;
    }

    if (jce_editor_component_slot_is_ik_constraints(slot)) {
        JceScene *scene = jce_state_get_scene();
        JceEntity ce = jce_state_to_ecs_entity(focused);
        if (!scene || !ce || jce_scene_has_ik_constraints(scene, ce))
            return;
        JceIkConstraintComponent def;
        memset(&def, 0, sizeof(def));
        def.count = 0;   /* empty stack; author in the Rigging panel */
        jce_state_begin_batch_edit();
        jce_scene_set_ik_constraints(scene, ce, &def);
        jce_state_end_batch_edit();
        return;
    }

    if (jce_editor_component_slot_is_sequence_player(slot)) {
        JceScene *scene = jce_state_get_scene();
        JceEntity ce = jce_state_to_ecs_entity(focused);
        if (!scene || !ce || jce_scene_has_sequence_player(scene, ce))
            return;
        JceSequencePlayerComponent def;
        memset(&def, 0, sizeof(def));
        def.speed         = 1.0f;
        def.play_on_awake = true;
        jce_state_begin_batch_edit();
        jce_scene_set_sequence_player(scene, ce, &def);
        jce_state_end_batch_edit();
        return;
    }

    if (!jce_editor_component_slot_is_compound_collider(slot))
        return;

    JceScene *scene = jce_state_get_scene();
    JceEntity ce = jce_state_to_ecs_entity(focused);
    if (!scene || !ce || jce_scene_has_compound_collider(scene, ce))
        return;

    JceCompoundColliderComponent def;
    jce_editor_component_compound_default(&def);
    jce_state_begin_batch_edit();
    jce_scene_set_compound_collider(scene, ce, &def);
    jce_state_end_batch_edit();
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
        JceEditorComponentSlot first_match = 0;
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
                first_match = desc->slot;
            }

            if (ImGui::MenuItem(desc->display_name)) {
                add_component_slot(focused, desc->slot);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();

        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && have_first_match) {
            add_component_slot(focused, first_match);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
