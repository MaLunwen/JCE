/*
 * inspector_audio.cpp
 * Audio component inspector drawers: audio source, listener, reverb zone,
 * occlusion.
 */

#include "jce_panel_inspector_common.h"

void draw_comp_audio_source(JceAudioSourceComponent *as)
{
    char lbl[256];
    jce_draw_path_input_asset(jce_editor_i18n("audioSource.clip"), as->clip_path, 128, JCE_ASSET_KIND_AUDIO);
    insp_track_edit();
    accept_asset_drop(as->clip_path, 128);
    ImGui::DragFloat(jce_editor_i18n("audioSource.volume"), &as->volume, 0.01f, 0.0f, 1.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("audioSource.pitch"), &as->pitch, 0.01f, 0.01f, 3.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.spatialBlend"), &as->spatial_blend, 0.01f, 0.0f, 1.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###audio", jce_editor_i18n("audioSource.loop"));
    if (ImGui::Checkbox(lbl, &as->loop))
        insp_undo_bool(&as->loop);
    if (ImGui::Checkbox(jce_editor_i18n("audioSource.playOnAwake"), &as->play_on_awake))
        insp_undo_bool(&as->play_on_awake);
}

void draw_comp_audio_listener(JceAudioListenerComponent *l)
{
    if (!l) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.al.volume", "al"), &l->volume, 0.01f, 0.0f, 1.0f, "%.2f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.al.paused", "al"), &l->paused))         insp_undo_bool(&l->paused);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.al.spatialize", "al"), &l->spatialize)) insp_undo_bool(&l->spatialize);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.al.dopplerFactor", "al"), &l->doppler_factor, 0.01f, 0.0f, 10.0f, "%.2f"); insp_track_edit();
    ImGui::TextDisabled(jce_editor_i18n("inspector.al.singletonNote"));
}

void draw_comp_audio_reverb_zone(JceAudioReverbZoneComponent *r)
{
    if (!r) return;
    static const char *presets[] = {
        "Off", "Generic", "Padded Cell", "Room", "Bathroom", "Living Room",
        "Stone Room", "Auditorium", "Concert Hall", "Cave", "Arena", "Hangar",
        "Hallway", "Stone Corridor", "Alley", "Forest", "City", "Mountains",
        "Quarry", "Plain", "Parking Lot", "Sewer Pipe", "Underwater",
        "(reserved 23)", "(reserved 24)", "(reserved 25)", "User"
    };
    int p = r->preset; if (p < 0 || p > 26) p = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.rz.preset", "rz"), &p, presets, IM_ARRAYSIZE(presets))) {
        r->preset = p; insp_track_edit();
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.minDistance", "rz"), &r->min_distance, 0.1f, 0.0f, 100000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.maxDistance", "rz"), &r->max_distance, 0.1f, 0.0f, 100000.0f, "%.2f"); insp_track_edit();
    if (r->preset == JCE_REVERB_ZONE_PRESET_USER) {
        ImGui::Separator();
        ImGui::TextDisabled(jce_editor_i18n("inspector.rz.customParams"));
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.room", "rz"),          &r->room,              1.0f, -10000.0f, 0.0f,    "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.roomHf", "rz"),       &r->room_hf,           1.0f, -10000.0f, 0.0f,    "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.decayTime", "rz"),     &r->decay_time,        0.01f, 0.1f, 20.0f,       "%.2f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.decayHfRatio", "rz"),     &r->decay_hf_ratio,    0.01f, 0.1f, 2.0f,        "%.2f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reflections", "rz"),   &r->reflections,       1.0f, -10000.0f, 1000.0f, "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reflectionsDelay", "rz"),  &r->reflections_delay, 0.001f, 0.0f, 0.3f,       "%.3f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reverb", "rz"),        &r->reverb,            1.0f, -10000.0f, 2000.0f, "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reverbDelay", "rz"),       &r->reverb_delay,      0.001f, 0.0f, 0.1f,       "%.3f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.hfReference", "rz"),  &r->hf_reference,      10.0f, 1000.0f, 20000.0f, "%.0f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.diffusion", "rz"),     &r->diffusion,         1.0f, 0.0f, 100.0f,       "%.1f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.density", "rz"),       &r->density,           1.0f, 0.0f, 100.0f,       "%.1f"); insp_track_edit();
    }
}

void draw_comp_audio_occlusion(JceAudioOcclusionComponent *o)
{
    if (!o) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.ao.radius", "ao"),            &o->radius,            0.1f,  0.0f, 100000.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.ao.attenuation", "ao"),  &o->attenuation_db,    0.1f, -96.0f, 0.0f,     "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.ao.lowpassCutoff", "ao"), &o->lowpass_cutoff_hz, 10.0f, 20.0f, 22000.0f, "%.0f"); insp_track_edit();
    ImGui::DragInt  (jce_editor_i18n_id("inspector.ao.layerMask", "ao"),        &o->layer_mask,        1.0f, -1, 0xFFFFFF); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.ao.affectsReverb", "ao"), &o->affects_reverb))
        insp_undo_bool(&o->affects_reverb);
}
