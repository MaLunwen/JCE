/*
 * jce_panel_inspector_animation.cpp
 * Animation component inspector drawers: animator, skeletal animator,
 * sprite animator.
 */

#include "jce_panel_inspector_common.h"
#include <jce/middleware/animation/jce_anim_sm.h>
#include <jce/middleware/animation/jce_anim_sm_binding.h>

void draw_comp_animator(JceAnimatorComponent *anim)
{
    insp_unwired_badge();
    if (anim->speed <= 0.0f) anim->speed = 1.0f;

    ImGui::InputText(jce_editor_i18n("timeline.clip"), anim->clip_name, 64);
    insp_track_edit();
    accept_asset_drop(anim->clip_name, 64);
    ImGui::DragFloat(jce_editor_i18n("timeline.speed"), &anim->speed, 0.01f, 0.01f, 10.0f);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("timeline.loop"), &anim->loop))
        insp_undo_bool(&anim->loop);
    if (ImGui::Button(anim->playing
                      ? jce_editor_i18n("toolbar.stop")
                      : jce_editor_i18n("toolbar.play")))
        anim->playing = !anim->playing;
}

void draw_comp_skeletal_animator(JceSkeletalAnimatorComponent *skel)
{
    char lbl[256];

    if (skel->speed <= 0.0f) skel->speed = 1.0f;

    /* glTF/GLB are classified as MODEL assets — filter to MODEL so the Browse
     * dialog actually lists them (DATA hid every .glb). Use the field's real
     * size, not a hard-coded 128, so long relative paths aren't truncated. */
    jce_draw_path_input_asset(jce_editor_i18n("inspector.skeleton"),
                              skel->skeleton_path, sizeof(skel->skeleton_path),
                              JCE_ASSET_KIND_MODEL);
    insp_track_edit();
    accept_asset_drop(skel->skeleton_path, sizeof(skel->skeleton_path));

    /* Optional RETARGET source skeleton. When set (and different from the
       entity's own skeleton) the active clip is treated as authored for THIS
       source rig and transferred onto the entity's skeleton at runtime. Empty
       ⇒ no retargeting (legacy single-skeleton playback). */
    jce_draw_path_input_asset(jce_editor_i18n("inspector.anim.retargetSource"),
                              skel->retarget_source_skeleton,
                              sizeof(skel->retarget_source_skeleton),
                              JCE_ASSET_KIND_MODEL);
    insp_track_edit();
    accept_asset_drop(skel->retarget_source_skeleton,
                      sizeof(skel->retarget_source_skeleton));
    if (skel->retarget_source_skeleton[0]) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.anim.retargetHint"));
        /* Only offered when a retarget source is set, because that is the only
         * case it does anything: with no transfer there is no source effector
         * to preserve, and a checkbox that is inert on most entities is one
         * people learn to ignore on the entities where it matters. */
        ImGui::Checkbox(jce_editor_i18n("inspector.anim.effectorIk"),
                        &skel->retarget_effector_ik);
        insp_track_edit();
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.anim.effectorIkHint"));
        ImGui::SliderFloat(jce_editor_i18n("inspector.anim.twist"),
                           &skel->retarget_twist, 0.0f, 1.0f, "%.2f");
        insp_track_edit();
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.anim.twistHint"));
    }

    /* Optional state machine (.anim_sm.json). When set it drives the active
       clip via parameter transitions instead of the fixed Active Clip below. */
    jce_draw_path_input_asset(jce_editor_i18n("inspector.anim.stateMachine"),
                              skel->sm_path, sizeof(skel->sm_path),
                              JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(skel->sm_path, sizeof(skel->sm_path));

    /* Auto-locomotion: when set, the runtime computes movement speed and feeds
     * it into the SM "Speed" param / blend-tree blend_param each frame. The
     * field, serialization, and runtime opt-in all existed; this exposes the
     * previously editor-less toggle (jce_sr_anim.c gates on sa->auto_speed). */
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.anim.autoSpeed",
                        "Auto-feed locomotion Speed"), &skel->auto_speed))
        insp_undo_bool(&skel->auto_speed);

    if (skel->sm_path[0]) {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.anim.smDrivesClip"));
        uint32_t ent = jce_state_get_focused();
        JceAnimSmBinding *smb = ent ? jce_editor_scene_get_anim_sm(ent) : nullptr;
        if (smb) {
            const JceAnimSmEval *ev = jce_anim_sm_binding_eval(smb);
            if (ev && ev->state_name)
                ImGui::Text(jce_editor_i18n("inspector.anim.stateFmt"), ev->state_name);
            JceAnimSm *sm = jce_anim_sm_binding_runtime(smb);
            int pc = sm ? jce_anim_sm_param_count(sm) : 0;
            /* Live param controls — local cache (no SM getter), pushed to the
               binding on edit; reset when the selected entity changes. */
            static uint32_t s_ent = 0;
            static float    s_f[32];
            static int      s_i[32];
            static bool     s_b[32];
            if (ent != s_ent) {
                s_ent = ent;
                memset(s_f, 0, sizeof(s_f));
                memset(s_i, 0, sizeof(s_i));
                memset(s_b, 0, sizeof(s_b));
            }
            for (int i = 0; i < pc && i < 32; i++) {
                const char *pn = jce_anim_sm_param_name(sm, i);
                if (!pn || !*pn) continue;
                ImGui::PushID(i);
                switch (jce_anim_sm_param_type(sm, i)) {
                case JCE_ANIM_SM_PARAM_FLOAT:
                    if (ImGui::DragFloat(pn, &s_f[i], 0.01f))
                        jce_anim_sm_binding_set_float(smb, pn, s_f[i]);
                    break;
                case JCE_ANIM_SM_PARAM_INT:
                    if (ImGui::DragInt(pn, &s_i[i]))
                        jce_anim_sm_binding_set_int(smb, pn, s_i[i]);
                    break;
                case JCE_ANIM_SM_PARAM_BOOL:
                    if (ImGui::Checkbox(pn, &s_b[i]))
                        jce_anim_sm_binding_set_bool(smb, pn, s_b[i]);
                    break;
                case JCE_ANIM_SM_PARAM_TRIGGER:
                    if (ImGui::Button(pn))
                        jce_anim_sm_binding_set_trigger(smb, pn);
                    break;
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.anim.stateRuntimeHint"));
        }
    }

    if (skel->skeleton_path[0] && skel->clip_count == 0) {
        JceModel *mdl = jce_editor_scene_get_model(skel->skeleton_path, 0);
        if (mdl) {
            uint32_t n = jce_model_anim_count(mdl);
            if (n > 8) n = 8;
            skel->clip_count = (int)n;
            for (uint32_t ci = 0; ci < n; ++ci) {
                JceAnimClip *clip = jce_model_get_anim(mdl, ci);
                const char *name = clip ? jce_anim_clip_name(clip) : "clip";
                snprintf(skel->clip_names[ci], sizeof(skel->clip_names[ci]),
                         "%s", name ? name : "clip");
            }
        }
    }

    snprintf(lbl, sizeof(lbl), "%s###skelSpeed", jce_editor_i18n("timeline.speed"));
    ImGui::DragFloat(lbl, &skel->speed, 0.01f, 0.01f, 10.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###skelLoop", jce_editor_i18n("timeline.loop"));
    if (ImGui::Checkbox(lbl, &skel->loop))
        insp_undo_bool(&skel->loop);

    int clip_count = skel->clip_count;
    if (clip_count < 0) clip_count = 0;
    if (clip_count > 8) clip_count = 8;
    if (skel->active_clip < 0) skel->active_clip = 0;
    if (clip_count > 0 && skel->active_clip >= clip_count)
        skel->active_clip = clip_count - 1;

    if (clip_count > 0) {
        int prev_clip = skel->active_clip;
        ImGui::Combo(jce_editor_i18n("inspector.activeClip"), &skel->active_clip,
            [](void *data, int idx) -> const char* {
                auto *sa = (JceSkeletalAnimatorComponent *)data;
                return sa->clip_names[idx]; },
            skel,
            clip_count);
        if (skel->active_clip != prev_clip)
            insp_undo_int(&skel->active_clip, prev_clip);
    }

    /* ── Blend tree (1D) ─ cross-blend clips by a scalar (e.g. speed) ── */
    {
        bool bt = skel->use_blend_tree;
        if (ImGui::Checkbox(jce_editor_i18n("inspector.anim.blendTree1d"), &bt)) {
            INSP_UNDO_SCOPE();
            skel->use_blend_tree = bt;
            /* First enable with unauthored thresholds (all zero): seed an
             * ascending spread so the 1D tree has valid brackets instead of
             * degenerate all-equal thresholds (random clip pairs/weights). */
            if (bt) {
                bool all_zero = true;
                for (int i = 0; i < clip_count && i < 8; i++)
                    if (skel->blend_thresholds[i] != 0.0f) all_zero = false;
                if (all_zero)
                    for (int i = 1; i < clip_count && i < 8; i++)
                        skel->blend_thresholds[i] = 2.0f * (float)i;
            }
        }
        if (skel->use_blend_tree) {
            /* Dimensionality: 1D scalar, 2D Cartesian, or 2D Directional.
             * In 2D modes blend_thresholds[] becomes each sample's X position
             * and blend_pos_y[] its Y, with (blend_param, blend_param_y) the
             * query point. */
            if (skel->blend_mode < 0 || skel->blend_mode > 2) skel->blend_mode = 0;
            const char *bmodes[3] = {
                jce_editor_i18n("inspector.anim.blendMode.1d"),
                jce_editor_i18n("inspector.anim.blendMode.2dCartesian"),
                jce_editor_i18n("inspector.anim.blendMode.2dDirectional"),
            };
            INSP_UNDO_DIRECT(skel->blend_mode,
                ImGui::Combo(jce_editor_i18n("inspector.anim.blendMode"),
                             &skel->blend_mode, bmodes, 3));
            bool is2d = (skel->blend_mode != 0);

            ImGui::DragFloat(jce_editor_i18n("inspector.anim.blendParam"), &skel->blend_param, 0.01f);
            insp_track_edit();
            if (is2d) {
                ImGui::DragFloat(jce_editor_i18n("inspector.anim.blendParamY"),
                                 &skel->blend_param_y, 0.01f);
                insp_track_edit();
            }
            if (clip_count > 0) {
                ImGui::TextDisabled("%s", is2d
                    ? jce_editor_i18n("inspector.anim.blendPosHint")
                    : jce_editor_i18n("inspector.anim.thresholdHint"));
                for (int i = 0; i < clip_count; i++) {
                    ImGui::PushID(i);
                    snprintf(lbl, sizeof(lbl), "%s##bt",
                             skel->clip_names[i][0] ? skel->clip_names[i] : "clip");
                    if (is2d) {
                        float xy[2] = { skel->blend_thresholds[i],
                                        skel->blend_pos_y[i] };
                        if (ImGui::DragFloat2(lbl, xy, 0.01f)) {
                            skel->blend_thresholds[i] = xy[0];
                            skel->blend_pos_y[i]      = xy[1];
                        }
                    } else {
                        ImGui::DragFloat(lbl, &skel->blend_thresholds[i], 0.01f);
                    }
                    insp_track_edit();
                    ImGui::PopID();
                }
            }
        }
    }

    if (skel->skeleton_path[0]) {
        JceAnimPlayer *pl = jce_editor_scene_get_anim_player(skel->skeleton_path, 0);
        if (pl) {
            float t = jce_anim_player_get_time(pl);
            JceModel *mdl = jce_editor_scene_get_model(skel->skeleton_path, 0);
            float dur = 1.0f;
            if (mdl) {
                JceAnimClip *clip = jce_model_get_anim(mdl, (uint32_t)skel->active_clip);
                if (clip) dur = jce_anim_clip_duration(clip);
            }
            float frac = (dur > 0.0f) ? (t / dur) : 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            snprintf(lbl, sizeof(lbl), "%.2fs / %.2fs", t, dur);
            ImGui::ProgressBar(frac, ImVec2(-1, 0), lbl);
        }
    }

    snprintf(lbl, sizeof(lbl), "%s###skelPlay",
             skel->playing ? jce_editor_i18n("toolbar.stop")
                           : jce_editor_i18n("toolbar.play"));
    if (ImGui::Button(lbl))
        skel->playing = !skel->playing;
}

void draw_comp_sprite_animator(JceSpriteAnimatorComponent *sa)
{
    jce_draw_path_input_asset(jce_editor_i18n("spriteAnimator.sheetPath"), sa->sheet_path, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(sa->sheet_path, 128);
    jce_draw_path_input_asset(jce_editor_i18n("spriteAnimator.atlasPath"), sa->atlas_path, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(sa->atlas_path, 128);
    ImGui::DragInt(jce_editor_i18n("spriteAnimator.frameWidth"), &sa->frame_width, 1, 1, 4096);
    insp_track_edit();
    ImGui::DragInt(jce_editor_i18n("spriteAnimator.frameHeight"), &sa->frame_height, 1, 1, 4096);
    insp_track_edit();
    ImGui::InputText(jce_editor_i18n("spriteAnimator.animation"), sa->current_anim, 64);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("spriteAnimator.speed"), &sa->speed, 0.01f, 0.0f, 10.0f, "%.2f");
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("spriteAnimator.loop"), &sa->loop))
        insp_undo_bool(&sa->loop);
    if (ImGui::Checkbox(jce_editor_i18n("spriteAnimator.playing"), &sa->playing))
        insp_undo_bool(&sa->playing);
}

void draw_comp_avatar(JceAvatarComponent *a)
{
    /* Marked per FIELD, not per component.  jce_sr_anim.c reads mask_path,
     * layers[], apply_root_motion and -- since 2026-09-08 -- override_controller;
     * it reads avatar_path zero times.  The component-wide badge that used to
     * stand here told the user that none of it worked. */
    jce_draw_path_input_asset(jce_editor_i18n("inspector.avatar.path"), a->avatar_path, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(a->avatar_path, 128);
    insp_unwired_field_badge();

    jce_draw_path_input_asset(jce_editor_i18n("inspector.avatar.mask"), a->mask_path, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(a->mask_path, 128);

    jce_draw_path_input_asset(jce_editor_i18n("inspector.avatar.overrideController"),
                              a->override_controller, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(a->override_controller, 128);
    /* The badge that stood here is GONE, and the three things it said are each
     * no longer true: the loader reads its path, it reports the pairs it found,
     * and jce_sr_anim.c resolves every clip name through it.  It is kept in the
     * history because the shape was instructive -- "overrideController" was
     * already an asset-dependency key in jce_bundle_deps.c, so a path dropped
     * here was followed by the packer and cooked into the shipped PAK: bytes in
     * the player's download for a feature that did nothing.  A dead field can
     * cost the player money before it costs the developer a bug report. */

    if (ImGui::Checkbox(jce_editor_i18n("inspector.avatar.applyRootMotion"), &a->apply_root_motion))
        insp_undo_bool(&a->apply_root_motion);
    if (ImGui::Checkbox(jce_editor_i18n("inspector.avatar.humanRig"), &a->human_rig))
        insp_undo_bool(&a->human_rig);

    /* ── Additive / override layer stack (FEATURE 3.3) ──────────────────
     * Each layer samples a clip (by name in the rigged model), optionally
     * bone-masked, and is composited over the SkeletalAnimator base pose at
     * runtime via jce_anim_player_blend_layers. */
    if (a->layer_count < 0) a->layer_count = 0;
    if (a->layer_count > JCE_AVATAR_MAX_LAYERS) a->layer_count = JCE_AVATAR_MAX_LAYERS;
    ImGui::SeparatorText(jce_editor_i18n("inspector.avatar.layers"));
    int remove_idx = -1;
    for (int i = 0; i < a->layer_count; i++) {
        JceAvatarLayer *L = &a->layers[i];
        ImGui::PushID(i);
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "%s %d##avlayer", jce_editor_i18n("inspector.avatar.layers"), i);
        if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::InputText(jce_editor_i18n("inspector.avatar.layerClip"), L->clip, sizeof(L->clip));
            insp_track_edit();
            accept_asset_drop(L->clip, sizeof(L->clip));

            jce_draw_path_input_asset(jce_editor_i18n("inspector.avatar.layerMask"),
                                      L->mask_path, sizeof(L->mask_path),
                                      JCE_ASSET_KIND_DATA);
            insp_track_edit();
            accept_asset_drop(L->mask_path, sizeof(L->mask_path));

            ImGui::DragFloat(jce_editor_i18n("inspector.avatar.layerWeight"),
                             &L->weight, 0.01f, 0.0f, 1.0f);
            insp_track_edit();

            if (L->mode < 0 || L->mode > 1) L->mode = 0;
            const char *lmodes[2] = {
                jce_editor_i18n("inspector.avatar.layerMode.additive"),
                jce_editor_i18n("inspector.avatar.layerMode.override"),
            };
            INSP_UNDO_DIRECT(L->mode,
                ImGui::Combo(jce_editor_i18n("inspector.avatar.layerMode"),
                             &L->mode, lmodes, 2));

            if (ImGui::SmallButton(jce_editor_i18n("inspector.avatar.layerRemove")))
                remove_idx = i;
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (remove_idx >= 0) {
        for (int i = remove_idx; i < a->layer_count - 1; i++)
            a->layers[i] = a->layers[i + 1];
        a->layer_count--;
        memset(&a->layers[a->layer_count], 0, sizeof(a->layers[a->layer_count]));
        insp_track_edit();
    }
    if (a->layer_count < JCE_AVATAR_MAX_LAYERS &&
        ImGui::Button(jce_editor_i18n("inspector.avatar.layerAdd"))) {
        INSP_UNDO_SCOPE();
        JceAvatarLayer *L = &a->layers[a->layer_count++];
        memset(L, 0, sizeof(*L));
        L->weight = 1.0f;
        L->mode   = 0;
    }
}

/* IK Constraints: minimal card — the full constraint stack is authored in
 * the Animation Rigging tab of the Animation Editor workbench. */
void draw_comp_ik_constraints(JceIkConstraintComponent *ik)
{
    if (!ik) return;
    ImGui::Text("%s: %d", jce_editor_i18n("inspector.ik.count"), ik->count);
    if (ImGui::Button(jce_editor_i18n("inspector.ik.openRigging"))) {
        bool *ae_vis = jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR);
        if (ae_vis) *ae_vis = true;
        jce_panel_animation_editor_request_tab(5);
    }
}

/* Foot IK: ground-adaptive foot placement.  Bone-name fields resolve in the
 * SkeletalAnimator's skeleton; an empty ankle disables that leg.  The runtime
 * supplies the ground raycast (via the renderer's ground-query hook) — the
 * pass is a no-op in tools until Play. */
void draw_comp_foot_ik(JceFootIkComponent *f)
{
    if (!f) return;

    if (ImGui::Checkbox(jce_editor_i18n("inspector.footik.enabled"), &f->enabled))
        insp_undo_bool(&f->enabled);

    ImGui::InputText(jce_editor_i18n("inspector.footik.pelvisBone"),
                     f->pelvis_bone, sizeof(f->pelvis_bone));
    insp_track_edit();

    ImGui::SeparatorText(jce_editor_i18n("inspector.footik.leftLeg"));
    ImGui::InputText(jce_editor_i18n("inspector.footik.hip"),
                     f->hip_bone[0], sizeof(f->hip_bone[0]));
    insp_track_edit();
    ImGui::InputText(jce_editor_i18n("inspector.footik.knee"),
                     f->knee_bone[0], sizeof(f->knee_bone[0]));
    insp_track_edit();
    ImGui::InputText(jce_editor_i18n("inspector.footik.ankle"),
                     f->ankle_bone[0], sizeof(f->ankle_bone[0]));
    insp_track_edit();

    ImGui::SeparatorText(jce_editor_i18n("inspector.footik.rightLeg"));
    ImGui::PushID("footik_r");
    ImGui::InputText(jce_editor_i18n("inspector.footik.hip"),
                     f->hip_bone[1], sizeof(f->hip_bone[1]));
    insp_track_edit();
    ImGui::InputText(jce_editor_i18n("inspector.footik.knee"),
                     f->knee_bone[1], sizeof(f->knee_bone[1]));
    insp_track_edit();
    ImGui::InputText(jce_editor_i18n("inspector.footik.ankle"),
                     f->ankle_bone[1], sizeof(f->ankle_bone[1]));
    insp_track_edit();
    ImGui::PopID();

    ImGui::SeparatorText(jce_editor_i18n("inspector.footik.tuning"));
    ImGui::DragFloat(jce_editor_i18n("inspector.footik.maxStep"),
                     &f->max_step_height, 0.01f, 0.0f, 5.0f, "%.2f m");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.footik.footOffset"),
                     &f->foot_offset, 0.005f, 0.0f, 1.0f, "%.3f m");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.footik.castUp"),
                     &f->cast_up, 0.01f, 0.0f, 5.0f, "%.2f m");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.footik.castDown"),
                     &f->cast_down, 0.01f, 0.0f, 5.0f, "%.2f m");
    insp_track_edit();
    ImGui::SliderFloat(jce_editor_i18n("inspector.footik.blend"),
                       &f->blend, 0.0f, 1.0f, "%.2f");
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n("inspector.footik.rotateToNormal"),
                        &f->rotate_to_normal))
        insp_undo_bool(&f->rotate_to_normal);

    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.footik.hint"));
}

/* Full-Body IK: coordinated multi-effector solve (FABRIK-on-tree).  Each
 * effector pulls a named bone toward a world target; a script / gameplay sets
 * the targets at runtime.  Presence-gated; the renderer pass (sr_apply_full_
 * body_ik) is a no-op until an effector names a resolvable bone. */
void draw_comp_full_body_ik(JceFullBodyIkComponent *f)
{
    if (!f) return;

    if (ImGui::Checkbox(jce_editor_i18n("inspector.fbbik.enabled"), &f->enabled))
        insp_undo_bool(&f->enabled);
    ImGui::SliderFloat(jce_editor_i18n("inspector.fbbik.blend"),
                       &f->blend, 0.0f, 1.0f, "%.2f");
    insp_track_edit();
    ImGui::DragInt(jce_editor_i18n("inspector.fbbik.iterations"),
                   &f->iterations, 1, 1, 50);
    insp_track_edit();

    int cap = (int)(sizeof(f->effectors) / sizeof(f->effectors[0]));
    if (f->effector_count < 0)   f->effector_count = 0;
    if (f->effector_count > cap) f->effector_count = cap;
    ImGui::SliderInt(jce_editor_i18n("inspector.fbbik.count"),
                     &f->effector_count, 0, cap);
    insp_track_edit();

    for (int i = 0; i < f->effector_count; i++) {
        ImGui::PushID(i);
        ImGui::SeparatorText(jce_editor_i18n("inspector.fbbik.effector"));
        JceFullBodyIkEffector *k = &f->effectors[i];
        ImGui::InputText(jce_editor_i18n("inspector.fbbik.bone"),
                         k->bone, sizeof(k->bone));
        insp_track_edit();
        ImGui::DragFloat3(jce_editor_i18n("inspector.fbbik.target"),
                          &k->target.x, 0.01f);
        insp_track_edit();
        ImGui::SliderFloat(jce_editor_i18n("inspector.fbbik.weight"),
                           &k->weight, 0.0f, 1.0f, "%.2f");
        insp_track_edit();
        ImGui::PopID();
    }

    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.fbbik.hint"));
}

/* Ragdoll: physics-driven skeletal blend (death blend / hit reactions).
 * Presence-gated; the runtime builds a live JceRagdoll from the entity's
 * SkeletalAnimator skeleton at Play and drives the pose by blend_weight. */
void draw_comp_ragdoll(JceRagdollComponent *r)
{
    if (!r) return;

    if (ImGui::Checkbox(jce_editor_i18n("inspector.ragdoll.enable"), &r->enable))
        insp_undo_bool(&r->enable);
    ImGui::SliderFloat(jce_editor_i18n("inspector.ragdoll.blendWeight"),
                       &r->blend_weight, 0.0f, 1.0f, "%.2f");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.ragdoll.radius"),
                     &r->radius, 0.005f, 0.001f, 2.0f, "%.3f m");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.ragdoll.heightScale"),
                     &r->height_scale, 0.01f, 0.01f, 10.0f, "%.2f");
    insp_track_edit();
    /* JOINT LIMITS.  Every joint used to be an unlimited ball, so elbows and
     * knees hyperextended and heads rotated without bound; the ragdoll now
     * takes each bone's angular range from its humanoid role.  The range
     * starts BELOW zero on purpose: a negative value is the explicit opt-out
     * that restores the old unlimited joints, and it is expressible only
     * because a scale cannot otherwise be negative. */
    ImGui::DragFloat(jce_editor_i18n_id("inspector.ragdoll.jointLimitScale",
                                        "Joint Limit Scale"),
                     &r->joint_limit_scale, 0.01f, -1.0f, 4.0f, "%.2f");
    insp_track_edit();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n_id(
            "inspector.ragdoll.jointLimitHint",
            "Scales each bone's angular limit, taken from its humanoid role. "
            "0 = the engine default (1.0); below 1 is stiffer, above 1 looser. "
            "A NEGATIVE value removes the limits entirely, which is what the "
            "ragdoll did before they existed. Bones whose name matches no "
            "humanoid role stay unlimited either way."));
    ImGui::TextDisabled("%s", jce_editor_i18n("inspector.ragdoll.hint"));
}

/* Morph Weights: per-instance static blendshape authoring (FEATURE 3.1).
 * The target count is authored manually (capped at JCE_MORPH_MAX_WEIGHTS): the
 * model's morph-target count is only reachable through renderer-internal
 * accessors, and glTF morph-target names are not retained by the importer, so
 * sliders are labelled by index.  Authoring a weight sets its override bit so
 * the value pins the blendshape (even at 0.0) over any clip-driven morph-weight
 * track at runtime. */
void draw_comp_morph_weights(JceScene *scene, JceEntity e,
                             JceMorphWeightsComponent *mw)
{
    (void)scene; (void)e;
    if (!mw) return;

    if (mw->count < 0) mw->count = 0;
    if (mw->count > JCE_MORPH_MAX_WEIGHTS) mw->count = JCE_MORPH_MAX_WEIGHTS;

    ImGui::Text(jce_editor_i18n("inspector.morph.count"), mw->count);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    int count = mw->count;
    if (ImGui::InputInt("##morphcount", &count)) {
        if (count < 0) count = 0;
        if (count > JCE_MORPH_MAX_WEIGHTS) count = JCE_MORPH_MAX_WEIGHTS;
        mw->count = count;
    }

    char lbl[64];
    for (int i = 0; i < mw->count; i++) {
        ImGui::PushID(i);
        snprintf(lbl, sizeof(lbl), "%s %d##morphw",
                 jce_editor_i18n("inspector.morph.target"), i);
        float v = mw->weights[i];
        if (ImGui::SliderFloat(lbl, &v, 0.0f, 1.0f, "%.3f")) {
            mw->weights[i]   = v;
            mw->override_mask |= (1u << i);   /* authoring pins this target */
        }
        insp_track_edit();

        ImGui::SameLine();
        bool ov = (mw->override_mask & (1u << i)) != 0;
        if (ImGui::Checkbox(jce_editor_i18n("inspector.morph.override"), &ov)) {
            INSP_UNDO_SCOPE();
            if (ov) mw->override_mask |=  (1u << i);
            else    mw->override_mask &= ~(1u << i);
        }
        ImGui::PopID();
    }

    if (mw->count > 0)
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.morph.hint"));
}
