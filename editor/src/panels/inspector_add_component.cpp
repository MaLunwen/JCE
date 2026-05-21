/*
 * inspector_add_component.cpp
 *
 * "Add Component" button + filterable popup menu.  Carved out of
 * jce_panel_inspector.cpp to keep the dispatcher under its size budget.
 *
 * The popup body and its data table live here together because the
 * `s_add_options[]` array is consulted exclusively by the popup loop.
 */

#include "jce_panel_inspector_common.h"

namespace {

struct AddCompOption {
    uint64_t flag;
    bool     is_light;       /* True for the 3 light flags (light section). */
    bool     is_collider;    /* True for box/sphere collider (mutually exclusive). */
};

const AddCompOption s_add_options[] = {
    { JCE_COMP_FLAG_MESH_RENDERER,        false, false },
    { JCE_COMP_FLAG_CAMERA,               false, false },
    { JCE_COMP_FLAG_DIR_LIGHT,            true,  false },
    { JCE_COMP_FLAG_POINT_LIGHT,          true,  false },
    { JCE_COMP_FLAG_SPOT_LIGHT,           true,  false },
    { JCE_COMP_FLAG_SKYBOX,               false, false },
    { JCE_COMP_FLAG_SPRITE_RENDERER,      false, false },
    { JCE_COMP_FLAG_SPRITE_ANIMATOR,      false, false },
    { JCE_COMP_FLAG_ANIMATOR,             false, false },
    { JCE_COMP_FLAG_SKELETAL_ANIMATOR,    false, false },
    { JCE_COMP_FLAG_RIGIDBODY,            false, false },
    { JCE_COMP_FLAG_RIGIDBODY_2D,         false, false },
    { JCE_COMP_FLAG_BOX_COLLIDER,         false, true  },
    { JCE_COMP_FLAG_SPHERE_COLLIDER,      false, true  },
    { JCE_COMP_FLAG_CHARACTER_CONTROLLER, false, false },
    { JCE_COMP_FLAG_AUDIO_SOURCE,         false, false },
    { JCE_COMP_FLAG_SCRIPT,               false, false },
    { JCE_COMP_FLAG_CONSTRAINT,           false, false },
    { JCE_COMP_FLAG_TERRAIN,              false, false },
    { JCE_COMP_FLAG_PARTICLE_EMITTER,     false, false },
    { JCE_COMP_FLAG_BEHAVIOR_TREE,        false, false },
    { JCE_COMP_FLAG_LOD_GROUP,            false, false },
    { JCE_COMP_FLAG_VIRTUAL_CAMERA,       false, false },
    { JCE_COMP_FLAG_TRIGGER_VOLUME,       false, false },
    { JCE_COMP_FLAG_CAPSULE_COLLIDER,     false, false },
    { JCE_COMP_FLAG_MESH_COLLIDER,        false, false },
    { JCE_COMP_FLAG_COLLIDER_2D,          false, false },
    { JCE_COMP_FLAG_TRAIL_RENDERER,       false, false },
    { JCE_COMP_FLAG_LINE_RENDERER,        false, false },
    { JCE_COMP_FLAG_REFLECTION_PROBE,     false, false },
    { JCE_COMP_FLAG_DECAL,                false, false },
    { JCE_COMP_FLAG_LIGHT_PROBE_GROUP,    false, false },
    { JCE_COMP_FLAG_AUDIO_LISTENER,       false, false },
    { JCE_COMP_FLAG_AUDIO_REVERB_ZONE,    false, false },
    { JCE_COMP_FLAG_AUDIO_OCCLUSION,      false, false },
    { JCE_COMP_FLAG_SPAWN_MANAGER,        false, false },
    { JCE_COMP_FLAG_WEAPON,               false, false },
    { JCE_COMP_FLAG_SAVE_POINT,           false, false },
    { JCE_COMP_FLAG_WHEEL_COLLIDER,       false, false },
    { JCE_COMP_FLAG_CONSTANT_FORCE,       false, false },
    { JCE_COMP_FLAG_CONFIGURABLE_JOINT,   false, false },
    { JCE_COMP_FLAG_JOINT_2D,             false, false },
    { JCE_COMP_FLAG_BILLBOARD_RENDERER,   false, false },
    { JCE_COMP_FLAG_CANVAS,               false, false },
    { JCE_COMP_FLAG_CANVAS_GROUP,         false, false },
    { JCE_COMP_FLAG_LAYOUT_GROUP,         false, false },
    { JCE_COMP_FLAG_UI_IMAGE,             false, false },
    { JCE_COMP_FLAG_UI_TEXT,              false, false },
    { JCE_COMP_FLAG_UI_BUTTON,            false, false },
    { JCE_COMP_FLAG_CLOTH,                false, false },
    { JCE_COMP_FLAG_NET_TRANSFORM,        false, false },
    { JCE_COMP_FLAG_NET_ANIMATOR,         false, false },
    { JCE_COMP_FLAG_NET_RIGIDBODY,        false, false },
    { JCE_COMP_FLAG_VFX_GRAPH,            false, false },
    { JCE_COMP_FLAG_TILEMAP,              false, false },
    { JCE_COMP_FLAG_TILEMAP_COLLIDER_2D,  false, false },
    { JCE_COMP_FLAG_AVATAR,               false, false },
    { JCE_COMP_FLAG_VOLUME,               false, false },
    { JCE_COMP_FLAG_OCCLUSION_PORTAL,     false, false },
};

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
        const int n_opts = (int)(sizeof(s_add_options) / sizeof(s_add_options[0]));
        int first_match = -1;
        for (int i = 0; i < n_opts; i++) {
            const AddCompOption &opt = s_add_options[i];
            if (flags & opt.flag) continue;
            if (opt.is_light && (flags & INSP_LIGHT_MASK)) continue;
            const char *cname = jce_comp_flag_display_name(opt.flag);
            if (!cname) continue;
            if (s_addcomp_filter[0]) {
                /* Match against the EN display name AND every loaded
                   locale's translation of the component's i18n key, so
                   users can search in any language present in the
                   editor's locale tables — not just the active one. */
                auto substr_ci = [](const char *hay, const char *needle) -> bool {
                    if (!hay || !needle || !*needle) return false;
                    for (const char *h = hay; *h; ++h) {
                        const char *a = h, *b = needle;
                        while (*a && *b && ((*a | 32) == (*b | 32))) { ++a; ++b; }
                        if (!*b) return true;
                    }
                    return false;
                };
                bool match = substr_ci(cname, s_addcomp_filter);
                if (!match) {
                    const char *i18n_key = jce_comp_flag_i18n_key(opt.flag);
                    if (i18n_key) {
                        int n_loc = jce_editor_i18n_locale_count();
                        for (int li = 0; li < n_loc && !match; ++li) {
                            const char *loc_name = jce_editor_i18n_lookup_locale(
                                (JceLocale)li, i18n_key);
                            if (substr_ci(loc_name, s_addcomp_filter)) match = true;
                        }
                    }
                }
                if (!match) continue;
            }
            if (first_match < 0) first_match = i;
            if (ImGui::MenuItem(cname)) {
                jce_state_add_component(focused, opt.flag);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();

        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && first_match >= 0) {
            jce_state_add_component(focused, s_add_options[first_match].flag);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
