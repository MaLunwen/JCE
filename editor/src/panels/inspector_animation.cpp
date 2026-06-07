/*
 * inspector_animation.cpp
 * Animation component inspector drawers: animator, skeletal animator,
 * sprite animator.
 */

#include "jce_panel_inspector_common.h"
#include <jce/middleware/animation/jce_anim_sm.h>
#include <jce/middleware/animation/jce_anim_sm_binding.h>

void draw_comp_animator(JceAnimatorComponent *anim)
{
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

    /* Optional state machine (.anim_sm.json). When set it drives the active
       clip via parameter transitions instead of the fixed Active Clip below. */
    jce_draw_path_input_asset(jce_editor_i18n("inspector.anim.stateMachine"),
                              skel->sm_path, sizeof(skel->sm_path),
                              JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(skel->sm_path, sizeof(skel->sm_path));
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
            skel->use_blend_tree = bt;
            insp_track_edit();
        }
        if (skel->use_blend_tree) {
            ImGui::DragFloat(jce_editor_i18n("inspector.anim.blendParam"), &skel->blend_param, 0.01f);
            if (ImGui::IsItemDeactivatedAfterEdit()) insp_track_edit();
            if (clip_count > 0) {
                ImGui::TextDisabled("%s", jce_editor_i18n("inspector.anim.thresholdHint"));
                for (int i = 0; i < clip_count; i++) {
                    ImGui::PushID(i);
                    snprintf(lbl, sizeof(lbl), "%s##bt",
                             skel->clip_names[i][0] ? skel->clip_names[i] : "clip");
                    ImGui::DragFloat(lbl, &skel->blend_thresholds[i], 0.01f);
                    if (ImGui::IsItemDeactivatedAfterEdit()) insp_track_edit();
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
    jce_draw_path_input_asset(jce_editor_i18n("inspector.avatar.path"), a->avatar_path, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(a->avatar_path, 128);

    jce_draw_path_input_asset(jce_editor_i18n("inspector.avatar.mask"), a->mask_path, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(a->mask_path, 128);

    jce_draw_path_input_asset(jce_editor_i18n("inspector.avatar.overrideController"),
                              a->override_controller, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(a->override_controller, 128);

    if (ImGui::Checkbox(jce_editor_i18n("inspector.avatar.applyRootMotion"), &a->apply_root_motion))
        insp_undo_bool(&a->apply_root_motion);
    if (ImGui::Checkbox(jce_editor_i18n("inspector.avatar.humanRig"), &a->human_rig))
        insp_undo_bool(&a->human_rig);
}
