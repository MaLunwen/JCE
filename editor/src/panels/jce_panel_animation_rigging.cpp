/*
 * jce_panel_animation_rigging.cpp
 *
 * Animation Rigging panel (P4-C.5). Shows the avatar / skeleton bound to
 * the focused entity, plus a constraint stack stored in the entity's
 * JceIkConstraintComponent — the stack is real scene data: it serializes
 * with the scene (type "IkConstraints") and TwoBoneIK entries are solved
 * at runtime by the scene renderer after pose sampling
 * (sr_apply_ik_constraints).  Other kinds round-trip but are not solved
 * yet.
 */

#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_dnd.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <jce/api_scene.h>
#include <jce/middleware/animation/jce_avatar.h>
}

namespace {

enum RigConstraintKind {
    RIG_KIND_AIM           = 0,
    RIG_KIND_TWO_BONE_IK   = 1,
    RIG_KIND_MULTI_PARENT  = 2,
    RIG_KIND_POSITION      = 3,
    RIG_KIND_ROTATION      = 4,
    RIG_KIND_CCD           = 5,  /* n-bone cyclic-coordinate-descent chain */
    RIG_KIND_FABRIK        = 6,  /* forward-and-backward-reaching chain */
    RIG_KIND_COUNT
};

constexpr int kMaxConstraintsPerEntity =
    (int)(sizeof(JceIkConstraintComponent::constraints) /
          sizeof(JceIkConstraintComponent::constraints[0]));

/* Undo batching for continuous widgets (sliders/drags/text): open a history
 * batch when the item is grabbed, close it when released — one undo step per
 * gesture.  Mirrors the inspector's insp_track_edit() without pulling the
 * whole inspector TU header into this panel. */
bool g_batch_open = false;
void track_edit()
{
    if (ImGui::IsItemActivated() && !g_batch_open) {
        jce_state_begin_batch_edit();
        g_batch_open = true;
    }
    if (ImGui::IsItemDeactivated() && g_batch_open) {
        jce_state_end_batch_edit();
        g_batch_open = false;
    }
}

const char *kind_label(int kind)
{
    switch (kind) {
    case RIG_KIND_AIM:          return jce_editor_i18n("panel.animRig.kindAim");
    case RIG_KIND_TWO_BONE_IK:  return jce_editor_i18n("panel.animRig.kindTwoBoneIk");
    case RIG_KIND_MULTI_PARENT: return jce_editor_i18n("panel.animRig.kindMultiParent");
    case RIG_KIND_POSITION:     return jce_editor_i18n("panel.animRig.kindPosition");
    case RIG_KIND_ROTATION:     return jce_editor_i18n("panel.animRig.kindRotation");
    case RIG_KIND_CCD:          return jce_editor_i18n("panel.animRig.kindCcd");
    case RIG_KIND_FABRIK:       return jce_editor_i18n("panel.animRig.kindFabrik");
    default:                    return "?";
    }
}

void draw_bones_section(const JceAvatarComponent *av)
{
    ImGui::SeparatorText(jce_editor_i18n("panel.animRig.bones"));
    if (!av->avatar_path[0]) {
        ImGui::TextDisabled("—");
        return;
    }
    JceAvatarAsset *asset = jce_avatar_load(av->avatar_path);
    if (!asset) {
        ImGui::TextDisabled("%s", av->avatar_path);
        return;
    }
    uint32_t n = jce_avatar_bone_count(asset);
    ImGui::Text("%s : %u", av->avatar_path, n);
    for (uint32_t i = 0; i < n; i++) {
        const char *bn = jce_avatar_bone_name(asset, i);
        ImGui::BulletText("%s", bn ? bn : "<unnamed>");
    }
    jce_avatar_unload(asset);
}

/* Collect the avatar's bone names (same source the Bones section lists). */
std::vector<std::string> collect_bone_names(const JceAvatarComponent *av)
{
    std::vector<std::string> bones;
    if (!av || !av->avatar_path[0]) return bones;
    JceAvatarAsset *asset = jce_avatar_load(av->avatar_path);
    if (!asset) return bones;
    uint32_t n = jce_avatar_bone_count(asset);
    bones.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        const char *bn = jce_avatar_bone_name(asset, i);
        if (bn && bn[0]) bones.push_back(bn);
    }
    jce_avatar_unload(asset);
    return bones;
}

/* Bone picker: combo over the avatar bone list when one is bound, free-text
 * fallback otherwise (bone names are matched at runtime via
 * jce_skeleton_find_joint, so any skeleton joint name is valid). */
void bone_field(const char *label, char *buf, size_t buf_size,
                const std::vector<std::string> &bones)
{
    if (bones.empty()) {
        ImGui::InputText(label, buf, buf_size);
        track_edit();
        return;
    }
    const char *preview = buf[0] ? buf : "—";
    if (ImGui::BeginCombo(label, preview)) {
        for (const std::string &b : bones) {
            bool sel = (std::strcmp(buf, b.c_str()) == 0);
            if (ImGui::Selectable(b.c_str(), sel) && !sel) {
                jce_state_begin_batch_edit();
                std::snprintf(buf, buf_size, "%s", b.c_str());
                jce_state_end_batch_edit();
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

/* Entity reference field: drop an entity from the Hierarchy onto the button
 * (JCE_DND_ENTITY payload, same accept pattern as the physics Constraint's
 * connected-body field), with a clear button when set. */
void entity_field(const char *label, uint32_t *ent_id)
{
    JceScene *scene = jce_state_get_scene();
    char tgt[160];
    if (*ent_id == 0) {
        std::snprintf(tgt, sizeof tgt, "—");
    } else {
        const char *nm = scene
            ? jce_scene_entity_name(scene, (JceEntity)*ent_id) : NULL;
        if (nm && nm[0]) std::snprintf(tgt, sizeof tgt, "%s", nm);
        else             std::snprintf(tgt, sizeof tgt, "Entity #%u", *ent_id);
    }
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    char btn[192];
    std::snprintf(btn, sizeof btn, "%s###ent_%s", tgt, label);
    ImGui::Button(btn, ImVec2(-30.0f, 0.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload *pl =
                ImGui::AcceptDragDropPayload(JCE_DND_ENTITY)) {
            uint32_t id = *(const uint32_t *)pl->Data;
            if (id != *ent_id) {
                jce_state_begin_batch_edit();
                *ent_id = id;
                jce_state_end_batch_edit();
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (*ent_id != 0) {
        ImGui::SameLine();
        char clr[64];
        std::snprintf(clr, sizeof clr, "X###entclr_%s", label);
        if (ImGui::SmallButton(clr)) {
            jce_state_begin_batch_edit();
            *ent_id = 0;
            jce_state_end_batch_edit();
        }
    }
}

void draw_constraints_section(JceScene *scene, JceEntity e,
                              const std::vector<std::string> &bones)
{
    ImGui::SeparatorText(jce_editor_i18n("panel.animRig.constraints"));

    JceIkConstraintComponent *ik = jce_scene_get_ik_constraints(scene, e);
    int count = ik ? ik->count : 0;
    if (count < 0) count = 0;
    if (count > kMaxConstraintsPerEntity) count = kMaxConstraintsPerEntity;

    int remove_idx = -1;
    for (int i = 0; ik && i < count; i++) {
        JceIkConstraint &c = ik->constraints[i];
        ImGui::PushID(i);

        char hdr[96];
        std::snprintf(hdr, sizeof(hdr), "%s : %s###rigc",
                      kind_label(c.kind), c.name[0] ? c.name : "—");
        if (ImGui::TreeNode(hdr)) {
            ImGui::InputText("##rigName", c.name, sizeof(c.name));
            track_edit();
            if (ImGui::BeginCombo("##rigKind", kind_label(c.kind))) {
                for (int k = 0; k < RIG_KIND_COUNT; k++) {
                    bool sel = (c.kind == k);
                    if (ImGui::Selectable(kind_label(k), sel) && !sel) {
                        jce_state_begin_batch_edit();
                        c.kind = k;
                        jce_state_end_batch_edit();
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (ImGui::Checkbox(jce_editor_i18n("panel.animRig.enabled"),
                                &c.enabled)) {
                /* Same idiom as insp_undo_bool: revert, snapshot the pre-
                 * toggle state into the history batch, re-apply. */
                bool now = c.enabled;
                c.enabled = !now;
                jce_state_begin_batch_edit();
                c.enabled = now;
                jce_state_end_batch_edit();
            }
            if (c.weight < 0.0f) c.weight = 0.0f;
            if (c.weight > 1.0f) c.weight = 1.0f;
            ImGui::SliderFloat(jce_editor_i18n("panel.animRig.weight"),
                               &c.weight, 0.0f, 1.0f, "%.2f");
            track_edit();

            bone_field(jce_editor_i18n("panel.animRig.rootBone"),
                       c.root_bone, sizeof(c.root_bone), bones);
            bone_field(jce_editor_i18n("panel.animRig.midBone"),
                       c.mid_bone, sizeof(c.mid_bone), bones);
            bone_field(jce_editor_i18n("panel.animRig.endBone"),
                       c.end_bone, sizeof(c.end_bone), bones);

            entity_field(jce_editor_i18n("panel.animRig.targetEntity"),
                         &c.target_entity);
            entity_field(jce_editor_i18n("panel.animRig.poleEntity"),
                         &c.pole_entity);
            if (c.pole_entity == 0) {
                ImGui::DragFloat3(jce_editor_i18n("panel.animRig.poleOffset"),
                                  c.pole_offset, 0.05f);
                track_edit();
            }

            if (ImGui::SmallButton("X")) remove_idx = i;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (ik && remove_idx >= 0) {
        jce_state_begin_batch_edit();
        for (int j = remove_idx; j + 1 < count; j++)
            ik->constraints[j] = ik->constraints[j + 1];
        std::memset(&ik->constraints[count - 1], 0,
                    sizeof(ik->constraints[count - 1]));
        ik->count = count - 1;
        jce_state_end_batch_edit();
    }

    if (count >= kMaxConstraintsPerEntity) return;
    if (ImGui::Button(jce_editor_i18n("panel.animRig.addConstraint"))) {
        jce_state_begin_batch_edit();
        if (!ik) {
            /* Create the component on demand (Add Constraint = first use). */
            JceIkConstraintComponent fresh;
            std::memset(&fresh, 0, sizeof(fresh));
            jce_scene_set_ik_constraints(scene, e, &fresh);
            ik = jce_scene_get_ik_constraints(scene, e);
        }
        if (ik && ik->count < kMaxConstraintsPerEntity) {
            JceIkConstraint &n = ik->constraints[ik->count];
            std::memset(&n, 0, sizeof(n));
            n.kind    = RIG_KIND_TWO_BONE_IK;
            n.weight  = 1.0f;
            n.enabled = true;
            std::snprintf(n.name, sizeof(n.name), "Constraint %d",
                          ik->count + 1);
            ik->count++;
        }
        jce_state_end_batch_edit();
    }
}

}  /* anonymous namespace */

extern "C" void jce_editor_panel_animation_rigging_content(void)
{
    uint32_t focused = jce_state_get_focused();
    if (focused == 0 || !jce_state_entity_exists(focused)) {
        ImGui::TextWrapped("%s", jce_editor_i18n("panel.animRig.noTarget"));
        return;
    }
    JceScene *scene = jce_state_get_scene();
    if (!scene) {
        ImGui::TextWrapped("%s", jce_editor_i18n("panel.animRig.noTarget"));
        return;
    }
    JceEntity e = jce_state_to_ecs_entity(focused);
    JceAvatarComponent *av = jce_scene_get_avatar(scene, e);
    bool has_animator = jce_scene_has_skeletal_animator(scene, e);
    /* Avatar OR Skeletal Animator — either provides a rig to constrain. */
    if (!av && !has_animator) {
        ImGui::TextWrapped("%s", jce_editor_i18n("panel.animRig.noTarget"));
        return;
    }

    if (av) draw_bones_section(av);

    /* The runtime solver hangs off the Skeletal Animator's palette: with
     * constraints authored but no animator the stack is inert — say so. */
    JceIkConstraintComponent *ik = jce_scene_get_ik_constraints(scene, e);
    if (!has_animator && ik && ik->count > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
        ImGui::TextWrapped("%s", jce_editor_i18n("panel.animRig.noAnimatorWarn"));
        ImGui::PopStyleColor();
    }

    std::vector<std::string> bones = collect_bone_names(av);
    draw_constraints_section(scene, e, bones);
}

extern "C" void animation_rigging_draw_content(void)
{
    jce_editor_panel_animation_rigging_content();
}

extern "C" void jce_editor_panel_animation_rigging(void)
{
    /* Shim: Animation Rigging has been merged into the Animation
     * Editor workbench as a tab.  Activating this panel now redirects
     * to that workbench and requests the Rigging tab.  Symbol kept so
     * menu/hotkey entries registered against JCE_PANEL_ANIMATION_RIGGING
     * keep working. */
    bool *visible = jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_RIGGING);
    if (!visible || !*visible) return;
    *visible = false;

    bool *ae_vis = jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR);
    if (ae_vis) *ae_vis = true;

    char title[128];
    std::snprintf(title, sizeof(title), "%s###jce_anim_editor",
                  jce_editor_i18n("animationEditor.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_animation_editor_request_tab(5);
}
