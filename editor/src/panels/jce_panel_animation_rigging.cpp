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

#include "jce_panel_common.h"
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
#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/renderer/jce_model.h>
#include <jce/os/core/jce_log.h>
#include "scene/jce_editor_scene_render.h"
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
    /* Names a LIMB, not three joints: the triple comes from the rig's
     * humanoid role map, so one authored goal works on any humanoid rig --
     * which is the case a named triple cannot serve at all, since two rigs
     * from different tools share no bone names. */
    RIG_KIND_HUMANOID_LIMB = 7,
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
    case RIG_KIND_HUMANOID_LIMB:
                                return jce_editor_i18n("panel.animRig.kindHumanoidLimb");
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
        /* Taken when the file is missing or unparseable.  jce_avatar_load is
         * no longer a stub -- it reads a real .avatar -- but it still answers
         * NULL rather than an empty asset, which is what makes this branch
         * mean "there is no such file" instead of "your rig has no bones".
         * Until 2026-08-31 it returned an empty struct and the line below
         * printed "<path> : 0" as though that were read from the user's file. */
        ImGui::TextDisabled("%s", av->avatar_path);
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.animRig.bonesUnavailable"));
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

/* HUMANOID: which joint of this rig plays each humanoid role.
 *
 * Unity's Configure Avatar, and the same purpose: a retarget is only as good
 * as its mapping, and a mapping you cannot SEE is one you cannot trust.  The
 * roles are auto-detected from joint names (jce_humanoid.h); a role this rig
 * does not name comes back blank rather than guessed, which is exactly what
 * the panel should show.
 *
 * The skeleton comes from the model the SkeletalAnimator names -- the same
 * cache the viewport draws from, so what is listed is the rig on screen. */
void draw_humanoid_section(JceScene *scene, JceEntity e,
                           const JceAvatarComponent *av)
{
    ImGui::SeparatorText(jce_editor_i18n("panel.animRig.humanoid"));

    JceSkeletalAnimatorComponent *sa = jce_scene_get_skeletal_animator(scene, e);
    if (!sa || !sa->skeleton_path[0]) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.animRig.humanoidNoRig"));
        return;
    }
    JceModel *model = jce_editor_scene_get_model(sa->skeleton_path,
                                                 (uint32_t)e);
    JceSkeleton *skel = model ? jce_model_get_skeleton(model) : NULL;
    if (!skel) {
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.animRig.humanoidNoRig"));
        return;
    }

    /* THE SAVED AVATAR IS THE AUTHORITY when there is one.  Auto-detection is
     * what you get before anything is authored; showing the detector's answer
     * over a stored file would hide the case that matters most -- an avatar
     * whose joint names no longer resolve against the rig it points at, which
     * jce_avatar_bind reports as roles that came back unmapped. */
    JceAvatarAsset *saved = (av && av->avatar_path[0])
                          ? jce_avatar_load(av->avatar_path) : NULL;
    if (saved) jce_avatar_bind(saved, skel);

    JceHumanoidMap detected;
    if (!jce_humanoid_map_build(skel, &detected)) {
        jce_avatar_unload(saved);
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.animRig.humanoidNoRig"));
        return;
    }
    const JceHumanoidMap *mp = saved ? jce_avatar_map(saved) : &detected;
    const JceHumanoidMap map = *mp;

    ImGui::Text("%s: %u / %d  (%s)",
                jce_editor_i18n("panel.animRig.humanoidMapped"),
                saved ? jce_avatar_mapped_count(saved) : map.mapped_count,
                (int)JCE_HB_COUNT,
                jce_editor_i18n(saved ? "panel.animRig.humanoidFromFile"
                                      : "panel.animRig.humanoidDetected"));
    if (av && !av->human_rig)
        ImGui::TextDisabled("%s", jce_editor_i18n("panel.animRig.humanoidGeneric"));

    if (ImGui::BeginTable("##humanoid_map", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.animRig.humanoidRole"));
        ImGui::TableSetupColumn(jce_editor_i18n("panel.animRig.humanoidJoint"));
        ImGui::TableSetupColumn(jce_editor_i18n("panel.animRig.humanoidMuscle"));
        ImGui::TableSetupColumn(jce_editor_i18n("panel.animRig.humanoidHinge"));
        ImGui::TableHeadersRow();
        for (int b = 0; b < JCE_HB_COUNT; b++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(jce_humanoid_bone_name((JceHumanoidBone)b));
            ImGui::TableSetColumnIndex(1);
            if (map.joint[b] >= 0) {
                const char *jn =
                    jce_skeleton_joint_name(skel, (uint32_t)map.joint[b]);
                ImGui::TextUnformatted(jn ? jn : "?");
            } else if (saved && jce_avatar_joint_name(saved,
                                                      (JceHumanoidBone)b)) {
                /* The file names a joint this rig does not have.  That is a
                 * broken avatar, and it is the one thing this panel exists to
                 * make visible -- silently falling back to the detector would
                 * show a working mapping the runtime will not use. */
                ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.30f, 1.0f), "%s: %s",
                                   jce_editor_i18n("panel.animRig.humanoidMissing"),
                                   jce_avatar_joint_name(saved,
                                                         (JceHumanoidBone)b));
            } else {
                /* Blank, not a guess.  A left arm driven by a right arm's
                 * motion is worse than an arm that does not move. */
                ImGui::TextDisabled("%s", jce_editor_i18n("panel.animRig.humanoidUnmapped"));
            }

            /* THE MUSCLE RANGE -- Unity's Muscles & Settings, read-only.
             *
             * It is here rather than on its own tab because the question it
             * answers is about THIS row: "how far may a retarget push this
             * bone from its rest", and the row above it is what plays that
             * role in this rig.  A limit shown away from the joint it binds
             * is a number nobody can check.
             *
             * A role with no derivable bone AXIS is called out rather than
             * shown with limits it will never be held to: the clamp needs a
             * direction to decompose about, and a tip (a hand, a toe) or a
             * role whose child is missing from the rig has none.  That is the
             * same blank-not-a-guess rule the joint column follows. */
            ImGui::TableSetColumnIndex(2);
            JceHumanoidMuscleLimits lim;
            jce_vec3 axis;
            const bool has_axis =
                jce_humanoid_bone_axis(&map, skel, (JceHumanoidBone)b, &axis);
            if (map.joint[b] < 0) {
                ImGui::TextDisabled("-");
            } else if (!has_axis) {
                ImGui::TextDisabled("%s",
                                    jce_editor_i18n("panel.animRig.humanoidNoAxis"));
            } else if (jce_humanoid_muscle_limits((JceHumanoidBone)b, &lim)) {
                ImGui::Text("%.0f..%.0f deg  /  %.0f deg",
                            (double)lim.twist_min, (double)lim.twist_max,
                            (double)lim.swing_max);
            }

            /* WHETHER THIS RIG CAN BE PROTECTED FROM AN INVERTED HINGE.
             *
             * The muscle range to the left bounds HOW FAR a retarget may push
             * a bone; it cannot express WHICH WAY, so a cone plus a signed
             * twist passes a backwards knee -- the most recognisable
             * retargeting artefact there is.  The hinge clamp bounds the
             * direction, and for an elbow it can only do so when the artist
             * left a rest bend to read the plane from.  PSX_BagMan's arms rest
             * at exactly 180 degrees and cannot be protected; CesiumMan's rest
             * at 147 and can.
             *
             * That difference is invisible everywhere else: a rig with no
             * derivable plane retargets without complaint and simply lets an
             * inverted elbow through.  This column is where it says so, and
             * it asks the ENGINE (jce_humanoid_hinge_axis) rather than
             * re-deriving the answer -- a second derivation of this exact
             * quantity is what made the clamp's own test disagree with it by
             * 22 degrees.
             *
             * The vector shown is the FOLDING direction: rotate the bone about
             * it by a positive angle and the joint closes. */
            ImGui::TableSetColumnIndex(3);
            jce_vec3 hinge;
            float fold_sign = 0.0f;
            if (b != JCE_HB_LEFT_LOWER_LEG && b != JCE_HB_RIGHT_LOWER_LEG &&
                b != JCE_HB_LEFT_LOWER_ARM && b != JCE_HB_RIGHT_LOWER_ARM) {
                ImGui::TextDisabled("-");
            } else if (jce_humanoid_hinge_axis(&map, skel, (JceHumanoidBone)b,
                                               &hinge, &fold_sign)) {
                ImGui::Text("%+.2f, %+.2f, %+.2f",
                            (double)(hinge.x * fold_sign),
                            (double)(hinge.y * fold_sign),
                            (double)(hinge.z * fold_sign));
            } else {
                ImGui::TextDisabled("%s",
                    jce_editor_i18n("panel.animRig.humanoidHingeNone"));
            }
        }
        ImGui::EndTable();
    }

    if (av && av->avatar_path[0] &&
        ImGui::Button(jce_editor_i18n("panel.animRig.humanoidSave"))) {
        JceAvatarAsset *built = jce_avatar_build(skel);
        if (built) {
            const bool ok = jce_avatar_save(built, av->avatar_path);
            LOG_INFO("editor", "avatar %s -> %s",
                     ok ? "saved" : "FAILED to save", av->avatar_path);
            jce_avatar_unload(built);
        }
    }
    jce_avatar_unload(saved);
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
 * jce_skeleton_find_joint, so any skeleton joint name is valid).
 *
 * The combo branch is UNREACHABLE today and has always been: collect_bone_names
 * asks jce_avatar_bone_count, which is a stub returning a literal 0, so the
 * list is always empty and this always falls back to free text.  Said out loud
 * because the sentence above describes a choice that is not currently made. */
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

            if (c.kind == RIG_KIND_HUMANOID_LIMB) {
                /* One limb picker instead of three bone boxes.  Leaving the
                 * bone fields visible here would show three controls the
                 * solver does not read on this kind -- a control that does
                 * nothing is worse than no control, because the reader spends
                 * their time on it before concluding the feature is broken. */
                static const char *kLimbs[] = { "LeftArm", "RightArm",
                                                "LeftLeg", "RightLeg" };
                int cur = -1;
                for (int li = 0; li < 4; li++)
                    if (std::strcmp(c.root_bone, kLimbs[li]) == 0) cur = li;
                const char *preview = (cur >= 0) ? kLimbs[cur] : "—";
                if (ImGui::BeginCombo(jce_editor_i18n("panel.animRig.limb"),
                                      preview)) {
                    for (int li = 0; li < 4; li++) {
                        const bool sel = (cur == li);
                        if (ImGui::Selectable(kLimbs[li], sel) && !sel) {
                            /* Same shape as the kind combo just above: one
                             * batch per discrete pick, because the activate /
                             * deactivate pair track_edit() relies on never
                             * fires for a Selectable. */
                            jce_state_begin_batch_edit();
                            std::snprintf(c.root_bone, sizeof(c.root_bone),
                                          "%s", kLimbs[li]);
                            c.mid_bone[0] = 0;
                            c.end_bone[0] = 0;
                            jce_state_end_batch_edit();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("%s",
                    jce_editor_i18n("panel.animRig.limbHint"));
                /* SAY SO WHEN IT WILL NOT SOLVE.  The analytic solver requires
                 * root -> mid -> end to be a parent chain, and on an ordinary
                 * Blender IK rig the foot is a CONTROL bone off the root --
                 * PSX_BagMan in this tree is one.  The engine correctly
                 * refuses that limb and leaves the pose alone, which on screen
                 * is a goal that does nothing with no explanation anywhere.
                 * Checking it HERE, against the rig this entity actually
                 * names, is the only place the answer is available before
                 * somebody spends an afternoon on it. */
                if (cur >= 0) {
                    JceSkeletalAnimatorComponent *sa =
                        jce_scene_get_skeletal_animator(scene, e);
                    JceModel *mdl = (sa && sa->skeleton_path[0])
                        ? jce_editor_scene_get_model(sa->skeleton_path,
                                                     (uint32_t)e) : nullptr;
                    JceSkeleton *sk = mdl ? jce_model_get_skeleton(mdl)
                                          : nullptr;
                    JceHumanoidMap hm;
                    if (sk && jce_humanoid_map_build(sk, &hm)) {
                        JceHumanoidBone up, lo, en;
                        if (jce_humanoid_limb_bones((JceHumanoidLimb)cur,
                                                    &up, &lo, &en)) {
                            const int32_t ju = hm.joint[up];
                            const int32_t jl = hm.joint[lo];
                            const int32_t je = hm.joint[en];
                            const char *why = nullptr;
                            if (ju < 0 || jl < 0 || je < 0)
                                why = jce_editor_i18n(
                                    "panel.animRig.limbMissing");
                            else if (jce_skeleton_joint_parent(sk,
                                         (uint32_t)je) != jl ||
                                     jce_skeleton_joint_parent(sk,
                                         (uint32_t)jl) != ju)
                                why = jce_editor_i18n(
                                    "panel.animRig.limbNotAChain");
                            if (why)
                                ImGui::TextColored(
                                    ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                    "%s", why);
                        }
                    }
                }
            } else {
                bone_field(jce_editor_i18n("panel.animRig.rootBone"),
                           c.root_bone, sizeof(c.root_bone), bones);
                bone_field(jce_editor_i18n("panel.animRig.midBone"),
                           c.mid_bone, sizeof(c.mid_bone), bones);
                bone_field(jce_editor_i18n("panel.animRig.endBone"),
                           c.end_bone, sizeof(c.end_bone), bones);
            }

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
    draw_humanoid_section(scene, e, av);

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
    if (jce_panel_redirect_to_workbench(JCE_PANEL_ANIMATION_RIGGING,
                                        JCE_PANEL_ANIMATION_EDITOR,
                                        "animationEditor.title",
                                        "jce_anim_editor"))
        jce_panel_animation_editor_request_tab(5);
}
