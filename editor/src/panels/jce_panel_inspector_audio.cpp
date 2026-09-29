/*
 * jce_panel_inspector_audio.cpp
 * Audio component inspector drawers: audio source, listener, reverb zone,
 * occlusion.
 */

#include "jce_panel_inspector_common.h"

void draw_comp_audio_source(JceAudioSourceComponent *as, JceEntity e)
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
    /* Load type, Unity's AudioClip.loadType.  Next to Loop and Play On Awake
     * rather than in the 3D fold: it is a memory decision, not a spatial one.
     * Streaming is ONE authored choice covering three engine strategies, picked
     * by where the clip lives (jce_audio_load_streaming): a loose project reads
     * the file incrementally, a STORED pak entry is borrowed in place, and a
     * re-compressed pak entry is expanded once.  Unity makes the author choose
     * between the last two; here the wrong choice is not expressible.
     * The item list is NUL-separated, the shape ImGui::Combo wants -- the
     * attenuation Model combo below is written the same way. */
    {
        const char *kinds = "Decompress On Load\0Streaming\0\0";
        INSP_UNDO_DIRECT(as->load_type,
            ImGui::Combo(jce_editor_i18n_id("audioSource.loadType", "Load Type"),
                         &as->load_type, kinds));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n_id(
                "audioSource.loadTypeTip",
                "Streaming decodes while it plays instead of expanding the whole "
                "clip up front. A loose project reads it off disk; a packaged "
                "build plays it from the archive with no second copy. Right for "
                "music and ambience; a short effect should stay Decompress On "
                "Load."));
    }
    /* Voice priority.  NOT behind the 3D fold: the 64-voice pool is global, so
     * a 2D dialogue line is exactly the kind of sound an author needs to
     * protect.  Higher survives; the tooltip carries the direction because the
     * number alone reads like Unity's, where it is inverted. */
    INSP_UNDO_DIRECT(as->priority,
        ImGui::DragInt(jce_editor_i18n_id("audioSource.priority", "Priority"),
                       &as->priority, 1.0f, -128, 127));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n_id(
            "audioSource.priorityTip",
            "Higher survives when the voice pool is full. 0 is normal. "
            "(Unreal's direction; Unity's Priority is inverted.)"));

    /* Audition, during Play (Unity's AudioSource inspector Play/Stop).
     *
     * Until jce_runtime_audio_play existed there was NOTHING an author could
     * press to hear a source they had just tuned: an AudioSource sounded once,
     * at scene spawn, and only with playOnAwake -- so tuning volume, pitch,
     * loop or the attenuation block on any other source meant restarting Play
     * and hoping.  These call the SAME entry gameplay scripts use
     * (jce.audio_play / audio_stop), so what the author hears here is what the
     * shipped game does, attenuation and mixer bus included.
     *
     * Disabled outside Play on purpose: there is no runtime to sound into, and
     * a button that silently does nothing is worse than one that says why. */
    {
        JceRuntime *rt = jce_editor_play_get_runtime();
        const bool live = (rt != NULL);
        const bool sounding = live && jce_runtime_audio_is_playing(rt, e);
        ImGui::BeginDisabled(!live || as->clip_path[0] == '\0');
        if (ImGui::Button(sounding
                ? jce_editor_i18n_id("audioSource.restart", "Restart")
                : jce_editor_i18n_id("audioSource.audition", "Play")))
            (void)jce_runtime_audio_play(rt, e);
        ImGui::SameLine();
        ImGui::BeginDisabled(!sounding);
        if (ImGui::Button(jce_editor_i18n_id("audioSource.auditionStop", "Stop")))
            (void)jce_runtime_audio_stop(rt, e);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!live)
            ImGui::TextDisabled("(%s)", jce_editor_i18n_id(
                "audioSource.auditionNeedsPlay", "enter Play to audition"));
        else if (sounding)
            ImGui::TextDisabled("(%s)", jce_editor_i18n_id(
                "audioSource.auditionPlaying", "playing"));
    }

    /* 3D attenuation authoring (large-world audio) — only meaningful for
     * spatial sources.  attenuation_model 0 = engine default (Inverse). */
    if (as->spatial_blend > 0.0f) {
        ImGui::SeparatorText(jce_editor_i18n_id("audioSource.attenuation", "3D Attenuation"));
        /* CUSTOM ROLLOFF CURVE.  Unity draws four editable curves against
         * distance on its AudioSource; this is the volume one, and it is the
         * first BUILT-IN consumer of the Curve Editor's documents -- until
         * now only a script could read one (jce.curve_eval).
         *
         * The PATH is the switch: naming a curve turns the analytic model
         * below off entirely, because miniaudio would otherwise attenuate the
         * source a second time inside the mix.  So the block is disabled
         * rather than left live-but-ignored, which is the exact failure the
         * Max Distance control below was already an instance of. */
        jce_draw_path_input_asset(
            jce_editor_i18n_id("audioSource.rolloffCurve", "Custom Rolloff Curve"),
            as->rolloff_curve, sizeof as->rolloff_curve,
            JCE_ASSET_KIND_DATA);
        insp_track_edit();
        accept_asset_drop(as->rolloff_curve, sizeof as->rolloff_curve);
        const bool custom_rolloff = (as->rolloff_curve[0] != '\0');
        if (custom_rolloff)
            ImGui::TextDisabled("(%s)", jce_editor_i18n_id(
                "audioSource.rolloffCurveNote",
                "the curve decides volume over distance; the model below is "
                "off while it is set"));

        ImGui::BeginDisabled(custom_rolloff);
        const char *models = "Default (Inverse)\0None\0Inverse\0Linear\0Exponential\0\0";
        INSP_UNDO_DIRECT(as->attenuation_model,
            ImGui::Combo(jce_editor_i18n_id("audioSource.attenModel", "Model"),
                         &as->attenuation_model, models));
        ImGui::DragFloat(jce_editor_i18n_id("audioSource.minDistance", "Min Distance"),
                             &as->min_distance, 0.1f, 0.0f, 10000.0f, "%.1f");
        insp_track_edit();
        /* MAX DISTANCE IS INERT FOR INVERSE AND EXPONENTIAL, which
         * jce_audio.h:538-540 already documents and this panel used to draw
         * anyway -- identical to the controls that do something.  Model 0 is
         * "Default (Inverse)", so the field was dead for the DEFAULT source.
         * Disabled rather than hidden: an author who set it under Linear and
         * switched model needs to see the value is still there. */
        const bool max_dist_live = !(as->attenuation_model == 0 ||
                                     as->attenuation_model == 2 ||
                                     as->attenuation_model == 4);
        ImGui::BeginDisabled(!max_dist_live);
        ImGui::DragFloat(jce_editor_i18n_id("audioSource.maxDistance", "Max Distance"),
                             &as->max_distance, 0.5f, 0.0f, 100000.0f, "%.1f");
        insp_track_edit();
        ImGui::EndDisabled();
        if (!max_dist_live) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", jce_editor_i18n_id(
                "audioSource.maxDistIgnored", "ignored by this model"));
        }
        ImGui::DragFloat(jce_editor_i18n_id("audioSource.rolloff", "Rolloff Factor"),
                             &as->rolloff_factor, 0.01f, 0.0f, 4.0f, "%.2f");
        insp_track_edit();
        ImGui::EndDisabled();   /* custom_rolloff */
        ImGui::InputText(jce_editor_i18n_id("audioSource.mixerBus", "Mixer Bus"),
                             as->mixer_bus, sizeof as->mixer_bus);
        insp_track_edit();
        ImGui::TextDisabled("(%s)", jce_editor_i18n_id("audioSource.attenNote",
            "0 / blank = engine defaults (Inverse, 1..25m, rolloff 1, auto bus)"));
    }
}

void draw_comp_music_track(JceMusicTrackComponent *m)
{
    if (!m) return;
    jce_draw_path_input_asset(jce_editor_i18n("musicTrack.track"),
                              m->track_path, sizeof m->track_path,
                              JCE_ASSET_KIND_AUDIO);
    insp_track_edit();
    accept_asset_drop(m->track_path, sizeof m->track_path);
    if (ImGui::Checkbox(jce_editor_i18n("musicTrack.playOnAwake"), &m->play_on_awake))
        insp_undo_bool(&m->play_on_awake);
    ImGui::DragFloat(jce_editor_i18n("musicTrack.initialIntensity"),
                     &m->initial_intensity, 0.01f, 0.0f, 1.0f, "%.2f");
    insp_track_edit();
    ImGui::DragInt(jce_editor_i18n("musicTrack.bpm"), &m->bpm, 1.0f, 1, 400);
    insp_track_edit();
}

void draw_comp_video_player(JceVideoPlayerComponent *vp)
{
    if (!vp) return;
    char lbl[256];

    jce_draw_path_input_asset(jce_editor_i18n("videoPlayer.clip"),
                              vp->clip_path, sizeof vp->clip_path);
    insp_track_edit();
    accept_asset_drop(vp->clip_path, sizeof vp->clip_path);

    snprintf(lbl, sizeof(lbl), "%s###vp_loop", jce_editor_i18n("videoPlayer.loop"));
    if (ImGui::Checkbox(lbl, &vp->loop))
        insp_undo_bool(&vp->loop);

    snprintf(lbl, sizeof(lbl), "%s###vp_autoplay", jce_editor_i18n("videoPlayer.autoplay"));
    if (ImGui::Checkbox(lbl, &vp->autoplay))
        insp_undo_bool(&vp->autoplay);

    /* Transport: a play/pause toggle on the live `playing` runtime field
     * (not serialized).  jce_scene_video_update opens the clip on demand. */
    snprintf(lbl, sizeof(lbl), "%s###vp_play",
             vp->playing ? jce_editor_i18n("videoPlayer.pause")
                         : jce_editor_i18n("videoPlayer.play"));
    if (ImGui::Button(lbl))
        vp->playing = !vp->playing;

    /* Status: show the decoded resolution once a frame has landed. */
    if (jce_texture_valid(vp->output_tex) && vp->tex_w > 0 && vp->tex_h > 0) {
        ImGui::SameLine();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%dx%d",
                           vp->tex_w, vp->tex_h);
    } else if (vp->playing) {
        ImGui::SameLine();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                           jce_editor_i18n("videoPlayer.decoding"));
    }
}

void draw_comp_audio_listener(JceAudioListenerComponent *l)
{
    if (!l) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.al.volume", "al"), &l->volume, 0.01f, 0.0f, 1.0f, "%.2f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.al.paused", "al"), &l->paused))         insp_undo_bool(&l->paused);
    /* Read now: a gate over every voice's own spatial flag, so off means the
     * whole mix is heard flat and on restores each source to what it authored.
     * The unwired badge that stood here came off in the same commit that
     * wired it -- leaving it would say the opposite of what the code does. */
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
    if (ImGui::Combo(jce_editor_i18n_id("inspector.rz.preset", "rz"), &p, presets, IM_ARRAYSIZE(presets)))
        insp_undo_set(&r->preset, p);
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
        /* EARLY reflections level: READ NOW.  The Freeverb tail gained a
         * separate early stage -- one clean tap before the diffuse tail --
         * so this composes with Room in millibels the way `reverb` does for
         * the late level, and the badge that stood here is gone with the
         * reason for it.  A badge that outlives its defect is the second lie.
         *
         * Worth keeping the history: it had already been MOVED once, from two
         * widgets down (after `reverb`) to here, because where it sat it read
         * as covering the wrong field. */
        ImGui::DragFloat(jce_editor_i18n_id("inspector.rz.reflectionsDelay", "rz"),  &r->reflections_delay, 0.001f, 0.0f, 0.3f,       "%.3f"); insp_track_edit();
        /* LATE reverb level: read now (composed with Room in millibels, as
         * EAX does), so the badge that used to sit here is gone with it. */
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
