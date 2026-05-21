/*
 * jce_panel_animation_rigging.cpp
 *
 * Animation Rigging panel (P4-C.5). Shows the avatar / skeleton bound to
 * the focused entity, plus a constraint stack the user can append to.
 * Today the stack is purely an authoring surface — the runtime solver
 * lands in P5. Keeping the UI in place means scenes can already carry
 * rigging intent through serialization.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>

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
    RIG_KIND_COUNT
};

struct RigConstraint {
    int   kind;            /* RigConstraintKind */
    char  name[48];
    float weight;          /* 0..1 */
};

constexpr int kMaxConstraintsPerEntity = 16;

struct EntityRigState {
    uint32_t      ent_id   = 0;
    bool          in_use   = false;
    int           count    = 0;
    RigConstraint slots[kMaxConstraintsPerEntity] = {};
};

constexpr int kMaxTrackedEntities = 32;
EntityRigState g_rig[kMaxTrackedEntities];

EntityRigState *rig_for(uint32_t ent_id, bool create)
{
    if (ent_id == 0) return nullptr;
    for (int i = 0; i < kMaxTrackedEntities; i++) {
        if (g_rig[i].in_use && g_rig[i].ent_id == ent_id)
            return &g_rig[i];
    }
    if (!create) return nullptr;
    for (int i = 0; i < kMaxTrackedEntities; i++) {
        if (!g_rig[i].in_use) {
            g_rig[i] = EntityRigState{};
            g_rig[i].ent_id = ent_id;
            g_rig[i].in_use = true;
            return &g_rig[i];
        }
    }
    return nullptr;
}

const char *kind_label(int kind)
{
    switch (kind) {
    case RIG_KIND_AIM:          return jce_editor_i18n("panel.animRig.kindAim");
    case RIG_KIND_TWO_BONE_IK:  return jce_editor_i18n("panel.animRig.kindTwoBoneIk");
    case RIG_KIND_MULTI_PARENT: return jce_editor_i18n("panel.animRig.kindMultiParent");
    case RIG_KIND_POSITION:     return jce_editor_i18n("panel.animRig.kindPosition");
    case RIG_KIND_ROTATION:     return jce_editor_i18n("panel.animRig.kindRotation");
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

void draw_constraints_section(EntityRigState *st)
{
    ImGui::SeparatorText(jce_editor_i18n("panel.animRig.constraints"));

    int remove_idx = -1;
    for (int i = 0; i < st->count; i++) {
        RigConstraint &c = st->slots[i];
        ImGui::PushID(i);

        char hdr[96];
        std::snprintf(hdr, sizeof(hdr), "%s : %s###rigc",
                      kind_label(c.kind), c.name[0] ? c.name : "—");
        if (ImGui::TreeNode(hdr)) {
            ImGui::InputText("##rigName", c.name, sizeof(c.name));
            if (ImGui::BeginCombo("##rigKind", kind_label(c.kind))) {
                for (int k = 0; k < RIG_KIND_COUNT; k++) {
                    bool sel = (c.kind == k);
                    if (ImGui::Selectable(kind_label(k), sel)) c.kind = k;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (c.weight < 0.0f) c.weight = 0.0f;
            if (c.weight > 1.0f) c.weight = 1.0f;
            ImGui::SliderFloat(jce_editor_i18n("panel.animRig.weight"),
                               &c.weight, 0.0f, 1.0f, "%.2f");
            if (ImGui::SmallButton("X")) remove_idx = i;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove_idx >= 0) {
        for (int j = remove_idx; j + 1 < st->count; j++)
            st->slots[j] = st->slots[j + 1];
        st->count--;
    }

    if (st->count >= kMaxConstraintsPerEntity) return;
    if (ImGui::Button(jce_editor_i18n("panel.animRig.addConstraint"))) {
        RigConstraint &n = st->slots[st->count++];
        n = RigConstraint{};
        n.kind   = RIG_KIND_AIM;
        n.weight = 1.0f;
        std::snprintf(n.name, sizeof(n.name), "Constraint %d", st->count);
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
    if (!av) {
        ImGui::TextWrapped("%s", jce_editor_i18n("panel.animRig.noTarget"));
        return;
    }

    draw_bones_section(av);

    EntityRigState *st = rig_for(focused, true);
    if (st) draw_constraints_section(st);
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
