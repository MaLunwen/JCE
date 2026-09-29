/*
 * jce_fv_audio.cpp  Audio file viewer — play/pause, seek, waveform.
 *
 * Creates a standalone JceAudio instance for previewing audio files
 * in the file viewer.  Supports WAV, OGG, MP3, and FLAC.
 */

#include "jce_fv_common.h"
#include <jce/api_audio.h>
#include <jce/api_core.h>

extern "C" {
#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_log.h>
}

#define LOG_TAG "fv_audio"

/* ── Per-tab audio state (keyed by tab path) ─────────────────────── */

struct AudioState {
    char       path[512];
    JceAudio  *audio;
    JceVoice   voice;
    float      duration;
    bool       playing;
    bool       loaded;
    bool       load_failed;  /* sticky: don't re-attempt every frame */

    JceAudioFile *file;
    uint32_t channels, samplerate;
    float waveform_progress;
    uint64_t trace_next_ms;
    uint8_t waveform_peaks[JCE_AUDIO_FILE_WAVEFORM_BINS];

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

/* Shared miniaudio engine across all audio tabs. Each tab still owns its
 * own streamed file + JceVoice, but creating one ma_engine per tab cost ~MB of
 * mixer state and dozens of internal allocations. We refcount via the
 * number of currently-loaded tabs. */
static JceAudio *s_shared_audio = nullptr;
static int       s_shared_refcount = 0;

static JceAudio *audio_engine_acquire(void)
{
    if (!s_shared_audio) {
        s_shared_audio = jce_audio_create();
        if (!s_shared_audio) return nullptr;
    }
    ++s_shared_refcount;
    return s_shared_audio;
}

static void audio_engine_release(void)
{
    if (s_shared_refcount <= 0) return;
    --s_shared_refcount;
    if (s_shared_refcount == 0 && s_shared_audio) {
        jce_audio_destroy(s_shared_audio);
        s_shared_audio = nullptr;
    }
}

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
    if (st->audio && st->voice != JCE_VOICE_INVALID)
        jce_audio_stop(st->audio, st->voice);
    jce_audio_file_close(st->file);
    bool had_engine = (st->audio != nullptr);
    memset(st, 0, sizeof(*st));
    if (had_engine)
        audio_engine_release();
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

static uint32_t audio_file_pull(void *user, int16_t *out, uint32_t frames)
{
    return jce_audio_file_pull((JceAudioFile *)user,out,frames);
}

static JceVoice start_voice(AudioState *st)
{
    if (jce_audio_file_eof(st->file)) jce_audio_file_seek(st->file,0.0);
    return jce_audio_play_stream(st->audio,audio_file_pull,st->file,
                                (uint16_t)st->channels,st->samplerate,1.0f,1.0f);
}

static void seek_state(AudioState *st, double seconds, bool resume)
{
    if (st->voice != JCE_VOICE_INVALID) jce_audio_stop(st->audio,st->voice);
    st->voice=JCE_VOICE_INVALID;
    jce_audio_file_seek(st->file,seconds);
    st->playing=false;
    if (resume) {
        st->voice=start_voice(st);
        st->playing=st->voice != JCE_VOICE_INVALID;
    }
}

static void finish_scrub(AudioState *st, int owner)
{
    if (!st || !st->scrubbing || st->scrub_owner != owner) return;

    seek_state(st,st->pending_seek,st->resume_after_scrub);

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
            /* Release inactive decoders and cancel waveform analysis.
             * Re-loaded transparently when the tab becomes active
             * again. */
            if (!is_active_tab) {
                free_state(st);
                continue;
            }
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
        st->voice = start_voice(st);
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

    st->audio = audio_engine_acquire();
    if (!st->audio) {
        LOG_ERROR(LOG_TAG, "failed to acquire shared audio engine for %s", tab->path);
        st->load_failed = true;
        return nullptr;
    }

    st->file=jce_audio_file_open(tab->path);
    if (!st->file) {
        LOG_ERROR(LOG_TAG,"failed to stream audio: %s",tab->path);
        audio_engine_release(); st->audio=nullptr;
        st->load_failed=true;
        return nullptr;
    }
    double duration=0.0;
    jce_audio_file_format(st->file,&st->channels,&st->samplerate,&duration);
    st->duration=(float)duration;
    st->loaded   = true;

    /* A restored or merely selected tab must remain silent until Play. */
    st->voice = JCE_VOICE_INVALID;
    st->playing = false;

    return st;
}

/* ── Bounded waveform published by background streaming analysis ── */

static void draw_waveform(ImDrawList *dl, ImVec2 pos, ImVec2 size,
                           const AudioState *st, float current_frac)
{
    /* Draw background. */
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      IM_COL32(30, 30, 40, 255));

    if (st->waveform_progress <= 0.0f) return;

    int bars = (int)size.x;
    if (bars <= 0) return;

    for (int b = 0; b < bars; ++b) {
        const int first = b * 2048 / bars;
        int end = (b + 1) * 2048 / bars;
        if (end <= first) end = first + 1;
        if (end > 2048) end = 2048;
        uint8_t peak_byte = 0;
        for (int i = first; i < end; ++i)
            if (st->waveform_peaks[i] > peak_byte)
                peak_byte = st->waveform_peaks[i];
        const float peak = (float)peak_byte / 255.0f;

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
                           jce_editor_i18n("viewer.audio.loadFailed"), tab->display_name);
        return;
    }

    const bool ui_focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    jce_audio_file_waveform(st->file,st->waveform_peaks,
                            sizeof(st->waveform_peaks),&st->waveform_progress);
    if (getenv("JCE_AUDIO_TRACE") && jce_time_ticks_ms()>=st->trace_next_ms) {
        st->trace_next_ms=jce_time_ticks_ms()+1000u;
        unsigned nonzero=0u;
        for (uint8_t peak: st->waveform_peaks) if (peak) ++nonzero;
        LOG_INFO(LOG_TAG,"audio trace playing=%d time=%.3f duration=%.3f waveform=%.3f bins=%u frame_ms=%.3f",
                 (int)st->playing,jce_audio_file_time(st->file),st->duration,
                 st->waveform_progress,nonzero,ImGui::GetIO().DeltaTime*1000.0f);
    }
    float dur = st->duration > 0.0f ? st->duration : 1.0f;
    float cur = (float)jce_audio_file_time(st->file);
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
                st->voice = start_voice(st);
            else
                jce_audio_resume(st->audio, st->voice);
            st->playing = (st->voice != JCE_VOICE_INVALID);
        } else {
            pause_state_playback(st);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(" |< ")) {
        seek_state(st,0.0,st->playing);
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
        seek_state(st,cur,st->playing);
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
    draw_waveform(dl, wf_pos, wf_size, st, frac);

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
        jce_audio_file_eof(st->file)) {
        jce_audio_stop(st->audio, st->voice);
        st->voice = JCE_VOICE_INVALID;
        st->playing = false;
    }

    /* Space key toggles play/pause when the file viewer is focused. */
    if (ui_focused && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (!st->playing) {
            stop_other_playback(tab->path);
            if (st->voice == JCE_VOICE_INVALID)
                st->voice = start_voice(st);
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
