/*
 * jce_fv_audio.cpp  Audio file viewer — play/pause, seek, waveform.
 *
 * Creates a standalone JceAudio instance for previewing audio files
 * in the file viewer.  Supports WAV, OGG, MP3, and FLAC.
 */

#include "jce_fv_common.h"

extern "C" {
#include <jce/audio/jce_audio.h>
#include <jce/core/jce_log.h>
}

#define LOG_TAG "fv_audio"

/* ── Per-tab audio state (keyed by tab path) ─────────────────────── */

struct AudioState {
    char       path[512];
    JceAudio  *audio;
    JceSound   sound;
    JceVoice   voice;
    float      duration;
    bool       playing;
    bool       loaded;
    bool       load_failed;  /* sticky: don't re-attempt every frame */

    /* Decoded PCM for waveform rendering. */
    const int16_t *pcm_samples;
    uint32_t       pcm_frame_count;
    uint32_t       pcm_channels;

    /* Scrub control: avoid per-frame seek noise while dragging. */
    bool           scrubbing;
    bool           resume_after_scrub;
    float          pending_seek;
    int            scrub_owner;
};

#define AUDIO_STATE_MAX 16
enum {
    AUDIO_SCRUB_NONE = 0,
    AUDIO_SCRUB_SLIDER,
    AUDIO_SCRUB_WAVEFORM,
};
static AudioState s_audio[AUDIO_STATE_MAX];

static AudioState *find_state(const char *path)
{
    for (int i = 0; i < AUDIO_STATE_MAX; ++i)
        if ((s_audio[i].loaded || s_audio[i].load_failed)
            && strcmp(s_audio[i].path, path) == 0)
            return &s_audio[i];
    return nullptr;
}

static AudioState *alloc_state(void)
{
    for (int i = 0; i < AUDIO_STATE_MAX; ++i)
        if (!s_audio[i].loaded && !s_audio[i].load_failed) return &s_audio[i];
    return nullptr;
}

static void free_state(AudioState *st)
{
    if (!st) return;
    if (st->voice != JCE_VOICE_INVALID)
        jce_audio_stop(st->audio, st->voice);
    if (st->sound != JCE_SOUND_INVALID)
        jce_audio_unload(st->audio, st->sound);
    if (st->audio)
        jce_audio_destroy(st->audio);
    memset(st, 0, sizeof(*st));
}

static void pause_state_playback(AudioState *st)
{
    if (!st) return;
    if (st->voice != JCE_VOICE_INVALID)
        jce_audio_pause(st->audio, st->voice);
    st->playing = false;
}

static void stop_other_playback(const char *active_path)
{
    for (int i = 0; i < AUDIO_STATE_MAX; ++i) {
        AudioState *st = &s_audio[i];
        if (!st->loaded) continue;
        if (active_path && strcmp(st->path, active_path) == 0) continue;

        pause_state_playback(st);
        st->scrubbing = false;
        st->resume_after_scrub = false;
        st->pending_seek = 0.0f;
        st->scrub_owner = AUDIO_SCRUB_NONE;
    }
}

static void begin_scrub(AudioState *st, int owner, float seek_time)
{
    if (!st) return;
    if (!st->scrubbing) {
        st->resume_after_scrub = st->playing;
        if (st->resume_after_scrub)
            pause_state_playback(st);
    }
    st->scrubbing = true;
    st->scrub_owner = owner;
    st->pending_seek = seek_time;
}

static void update_scrub(AudioState *st, int owner, float seek_time)
{
    if (!st || !st->scrubbing || st->scrub_owner != owner) return;
    st->pending_seek = seek_time;
}

static void finish_scrub(AudioState *st, int owner)
{
    if (!st || !st->scrubbing || st->scrub_owner != owner) return;

    if (st->voice != JCE_VOICE_INVALID)
        jce_audio_seek(st->audio, st->voice, st->pending_seek);

    if (st->resume_after_scrub && st->voice != JCE_VOICE_INVALID) {
        jce_audio_resume(st->audio, st->voice);
        st->playing = true;
    }

    st->scrubbing = false;
    st->resume_after_scrub = false;
    st->pending_seek = 0.0f;
    st->scrub_owner = AUDIO_SCRUB_NONE;
}

void fv_audio_update_focus(const char *active_tab_path, bool allow_playback)
{
    for (int i = 0; i < AUDIO_STATE_MAX; ++i) {
        AudioState *st = &s_audio[i];
        if (!st->loaded) continue;

        bool is_active_tab = (active_tab_path && strcmp(st->path, active_tab_path) == 0);
        bool can_keep_playing = allow_playback && is_active_tab;
        if (!can_keep_playing) {
            pause_state_playback(st);
            st->scrubbing = false;
            st->resume_after_scrub = false;
            st->pending_seek = 0.0f;
            st->scrub_owner = AUDIO_SCRUB_NONE;
        }
    }
}

void fv_audio_request_play(const char *path)
{
    if (!path) return;
    AudioState *st = find_state(path);
    if (!st || !st->loaded || st->playing) return;

    stop_other_playback(path);
    if (st->voice == JCE_VOICE_INVALID)
        st->voice = jce_audio_play(st->audio, st->sound, false, 1.0f, 1.0f);
    else
        jce_audio_resume(st->audio, st->voice);
    st->playing = (st->voice != JCE_VOICE_INVALID);
}

static AudioState *ensure_loaded(FvTab *tab)
{
    AudioState *st = find_state(tab->path);
    if (st) {
        /* Already attempted: return loaded state, or nullptr if it failed.
         * Without this guard, a broken file would re-init miniaudio and
         * spam the decode pipeline every frame, freezing the UI. */
        return st->loaded ? st : nullptr;
    }

    st = alloc_state();
    if (!st) return nullptr;

    memset(st, 0, sizeof(*st));
    snprintf(st->path, sizeof(st->path), "%s", tab->path);

    st->audio = jce_audio_create();
    if (!st->audio) {
        LOG_ERROR(LOG_TAG, "failed to create audio engine for %s", tab->path);
        st->load_failed = true;
        return nullptr;
    }

    if (tab->content && tab->content_len > 0) {
        st->sound = jce_audio_load_memory(st->audio, tab->content,
                                           (uint32_t)tab->content_len,
                                           tab->path);
    } else {
        st->sound = JCE_SOUND_INVALID;
    }

    if (st->sound == JCE_SOUND_INVALID) {
        LOG_ERROR(LOG_TAG, "failed to decode audio: %s", tab->path);
        jce_audio_destroy(st->audio);
        st->audio = nullptr;
        st->load_failed = true;  /* sticky: skip retry next frame */
        return nullptr;
    }

    st->duration = jce_audio_get_duration(st->audio, st->sound);
    st->pcm_samples = jce_audio_get_pcm_data(st->audio, st->sound,
                                              &st->pcm_frame_count,
                                              &st->pcm_channels);
    st->loaded   = true;

    /* Auto-play on load (same behaviour as the video viewer). */
    stop_other_playback(tab->path);
    st->voice = jce_audio_play(st->audio, st->sound, false, 1.0f, 1.0f);
    st->playing = (st->voice != JCE_VOICE_INVALID);

    return st;
}

/* ── Simple waveform from raw PCM data ───────────────────────────── */

static void draw_waveform(ImDrawList *dl, ImVec2 pos, ImVec2 size,
                           const int16_t *samples, uint32_t frame_count,
                           uint32_t channels, float current_frac)
{
    /* Draw background. */
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      IM_COL32(30, 30, 40, 255));

    if (!samples || frame_count == 0) return;

    uint32_t sample_count = frame_count * (channels > 0 ? channels : 1);

    int bars = (int)size.x;
    if (bars <= 0) return;

    for (int b = 0; b < bars; ++b) {
        uint32_t s0 = (uint32_t)((uint64_t)b * sample_count / bars);
        uint32_t s1 = (uint32_t)((uint64_t)(b + 1) * sample_count / bars);
        if (s1 > sample_count) s1 = sample_count;

        float peak = 0.0f;
        for (uint32_t s = s0; s < s1; s += 4) { /* stride for perf */
            float v = fabsf((float)samples[s] / 32768.0f);
            if (v > peak) peak = v;
        }

        float bar_h = peak * size.y * 0.5f;
        float cx = pos.x + (float)b;
        float cy = pos.y + size.y * 0.5f;

        float frac = (float)b / (float)bars;
        ImU32 col = (frac < current_frac)
                        ? IM_COL32(100, 180, 255, 200)
                        : IM_COL32(60, 60, 80, 180);

        dl->AddLine(ImVec2(cx, cy - bar_h), ImVec2(cx, cy + bar_h), col);
    }

    /* Playhead line. */
    float ph_x = pos.x + current_frac * size.x;
    dl->AddLine(ImVec2(ph_x, pos.y), ImVec2(ph_x, pos.y + size.y),
                IM_COL32(255, 255, 255, 200), 2.0f);
}

/* ── Render ───────────────────────────────────────────────────────── */

void fv_render_audio(FvTab *tab)
{
    AudioState *st = ensure_loaded(tab);
    if (!st) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                           "Failed to load audio: %s", tab->display_name);
        return;
    }

    const bool ui_focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    float dur = st->duration > 0.0f ? st->duration : 1.0f;
    float cur = 0.0f;
    if (st->voice != JCE_VOICE_INVALID)
        cur = jce_audio_get_time(st->audio, st->voice);
    if (st->scrubbing)
        cur = st->pending_seek;
    float frac = cur / dur;
    if (frac > 1.0f) frac = 1.0f;

    /* Info line. */
    ImGui::Text("%s  |  %.1fs", tab->display_name, dur);
    ImGui::Separator();

    /* Transport controls. */
    if (ImGui::Button(st->playing ? "  ||  " : "  >  ")) {
        if (!st->playing) {
            stop_other_playback(tab->path);
            if (st->voice == JCE_VOICE_INVALID)
                st->voice = jce_audio_play(st->audio, st->sound,
                                            false, 1.0f, 1.0f);
            else
                jce_audio_resume(st->audio, st->voice);
            st->playing = (st->voice != JCE_VOICE_INVALID);
        } else {
            pause_state_playback(st);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(" |< ")) {
        if (st->voice != JCE_VOICE_INVALID)
            jce_audio_seek(st->audio, st->voice, 0.0f);
    }
    ImGui::SameLine();
    ImGui::Text("%.2fs / %.2fs", cur, dur);

    /* Time seek slider. */
    ImGui::PushItemWidth(-1);
    bool seek_changed = ImGui::SliderFloat("##seek", &cur, 0.0f, dur, "%.2fs");
    bool seek_active = ImGui::IsItemActive();
    if (seek_active) {
        begin_scrub(st, AUDIO_SCRUB_SLIDER, cur);
        if (seek_changed)
            update_scrub(st, AUDIO_SCRUB_SLIDER, cur);
    } else if (st->scrubbing && st->scrub_owner == AUDIO_SCRUB_SLIDER) {
        finish_scrub(st, AUDIO_SCRUB_SLIDER);
    } else if (seek_changed && st->voice != JCE_VOICE_INVALID) {
        jce_audio_seek(st->audio, st->voice, cur);
    }
    ImGui::PopItemWidth();

    ImGui::Spacing();

    /* Waveform. */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float wf_h = avail.y > 120.0f ? 120.0f : avail.y;
    if (wf_h < 40.0f) wf_h = 40.0f;
    ImVec2 wf_pos = ImGui::GetCursorScreenPos();
    ImVec2 wf_size = ImVec2(avail.x, wf_h);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    draw_waveform(dl, wf_pos, wf_size, st->pcm_samples,
                  st->pcm_frame_count, st->pcm_channels, frac);

    /* Invisible button for click-to-seek on waveform. */
    ImGui::InvisibleButton("##waveform", wf_size);
    if (ImGui::IsItemActive()) {
        float mx = ImGui::GetMousePos().x - wf_pos.x;
        float t = (mx / wf_size.x) * dur;
        if (t < 0.0f) t = 0.0f;
        if (t > dur)  t = dur;
        begin_scrub(st, AUDIO_SCRUB_WAVEFORM, t);
        update_scrub(st, AUDIO_SCRUB_WAVEFORM, t);
    } else if (st->scrubbing && st->scrub_owner == AUDIO_SCRUB_WAVEFORM) {
        finish_scrub(st, AUDIO_SCRUB_WAVEFORM);
    }

    /* Auto-stop when playback reaches the end. */
    if (st->playing && st->voice != JCE_VOICE_INVALID &&
        !jce_audio_is_playing(st->audio, st->voice)) {
        st->playing = false;
    }

    /* Space key toggles play/pause when the file viewer is focused. */
    if (ui_focused && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (!st->playing) {
            stop_other_playback(tab->path);
            if (st->voice == JCE_VOICE_INVALID)
                st->voice = jce_audio_play(st->audio, st->sound,
                                            false, 1.0f, 1.0f);
            else
                jce_audio_resume(st->audio, st->voice);
            st->playing = (st->voice != JCE_VOICE_INVALID);
        } else {
            pause_state_playback(st);
        }
    }
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

void fv_audio_close_tab(FvTab *tab)
{
    AudioState *st = find_state(tab->path);
    free_state(st);
}
