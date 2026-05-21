/*
 * inspector_animation.cpp
 * Animation component inspector drawers: animator, skeletal animator,
 * sprite animator.
 */

#include "jce_panel_inspector_common.h"

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

    jce_draw_path_input_asset(jce_editor_i18n("inspector.skeleton"), skel->skeleton_path, 128, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(skel->skeleton_path, 128);

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
