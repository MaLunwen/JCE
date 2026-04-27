/*
 * jce_fv_video.cpp  Video file viewer — MP4 metadata/timeline via jce_video.
 *
 * Mirrors jce_fv_audio.cpp: each tab owns a VideoState with an engine
 * JceVideo handle, a bgfx texture holding the most recent decoded
 * frame, and transport state. Playback/timeline state advances through
 * jce_video_advance() based on the ImGui frame delta time.
 *
 * Supported container: MP4/ISO-BMFF.
 * Decoding is backend-dependent; when unavailable, the viewer falls back
 * to metadata/timeline mode with a clear status message.
 */

#include "jce_fv_common.h"
#include "core/jce_editor_i18n.h"

extern "C" {
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/video/jce_video.h>
}

/* perf timing comes from <jce/core/jce_timer.h> */
#include <jce/os/core/jce_timer.h>

#define LOG_TAG "fv_video"

/* ── Per-tab video state (keyed by tab path) ─────────────────────── */

struct VideoState {
    char        path[512];
    bool        loaded;       /* slot in use */

    /* Engine handle.  JCE_VIDEO_INVALID when load failed but the slot
     * is still reserved so we don't retry every frame. */
    JceVideo    video;
    bool        load_failed;
    char        fail_reason[160];

    /* Metadata snapshot (valid only when video != JCE_VIDEO_INVALID). */
    JceVideoInfo info;

    /* GPU texture for the most-recently uploaded frame. */
    JceTexture  gpu_tex;
    int         tex_w;
    int         tex_h;
    uint64_t    uploaded_counter;

    /* Transport state. */
    bool        playing;
    bool        scrubbing;
    bool        resume_after_scrub;
    float       pending_seek;   /* seconds, used during scrub */
    float       last_seek_issued;
    double      sync_resume_at; /* ImGui time when drift-correction may resume */

    /* Deferred audio resume after seek: pause audio, seek video, then wait
     * until the worker has actually produced a frame at the seek target
     * before unpausing audio.  Eliminates "video freeze + audio ahead"
     * desync after slider release.  When pending_audio_resume_seek >= 0
     * the audio voice is held silent until either:
     *   (a) frame_counter advances past audio_resume_seek_counter, or
     *   (b) audio_resume_deadline elapses (timeout fallback). */
    double      pending_audio_resume_seek;  /* < 0 = not pending */
    uint64_t    audio_resume_seek_counter;
    double      audio_resume_deadline;

    /* Embedded audio playback (decoded MP4 audio track). */
    JceAudio   *audio;
    JceSound    sound;
    JceVoice    voice;
    bool        audio_available;

    /* Perf overlay (S1). */
    bool        show_perf_overlay;
    double      perf_upload_us_ema;
    double      perf_upload_us_last;
    uint64_t    perf_uploads;
};

#define VIDEO_STATE_MAX 32
static VideoState s_video[VIDEO_STATE_MAX];

/* Pull-callback trampoline for the streaming audio voice. Routes
 * miniaudio's onRead back into the video module's pull API. */
static uint32_t fv_video_audio_pull_trampoline(void *ud,
                                                int16_t *out,
                                                uint32_t frames)
{
    VideoState *st = (VideoState *)ud;
    if (!st || st->video == JCE_VIDEO_INVALID) return 0;
    return jce_video_audio_pull(st->video, out, frames);
}

/* Start (or restart) the streaming voice for `st`. Returns the new
 * voice handle or JCE_VOICE_INVALID on failure. */
static JceVoice fv_video_start_stream_voice(VideoState *st)
{
    if (!st || !st->audio || st->video == JCE_VIDEO_INVALID)
        return JCE_VOICE_INVALID;
    uint32_t ch = 0, sr = 0;
    double dur = 0.0;
    if (!jce_video_get_audio_format(st->video, &ch, &sr, &dur))
        return JCE_VOICE_INVALID;
    return jce_audio_play_stream(st->audio,
                                  &fv_video_audio_pull_trampoline,
                                  st,
                                  (uint16_t)ch, sr,
                                  1.0f, 1.0f);
}

static VideoState *find_state(const char *path)
{
    for (int i = 0; i < VIDEO_STATE_MAX; ++i)
        if (s_video[i].loaded && strcmp(s_video[i].path, path) == 0)
            return &s_video[i];
    return nullptr;
}

static VideoState *alloc_state(void)
{
    for (int i = 0; i < VIDEO_STATE_MAX; ++i)
        if (!s_video[i].loaded) return &s_video[i];
    return nullptr;
}

static void release_texture(VideoState *st)
{
    if (!st) return;
    if (jce_texture_valid(st->gpu_tex))
        jce_texture_destroy(st->gpu_tex);
    st->gpu_tex.idx         = UINT16_MAX;
    st->tex_w               = 0;
    st->tex_h               = 0;
    st->uploaded_counter    = 0;
}

static void free_state(VideoState *st)
{
    if (!st) return;
    release_texture(st);
    if (st->voice != JCE_VOICE_INVALID && st->audio)
        jce_audio_stop(st->audio, st->voice);
    if (st->sound != JCE_SOUND_INVALID && st->audio)
        jce_audio_unload(st->audio, st->sound);
    if (st->audio)
        jce_audio_destroy(st->audio);
    if (st->video != JCE_VIDEO_INVALID) {
        jce_video_unload(st->video);
        st->video = JCE_VIDEO_INVALID;
    }
    memset(st, 0, sizeof(*st));
    st->pending_audio_resume_seek = -1.0;
}

static void pause_state_playback(VideoState *st)
{
    if (!st) return;
    if (st->audio_available && st->voice != JCE_VOICE_INVALID)
        jce_audio_pause(st->audio, st->voice);
    st->playing = false;
}

static void stop_other_playback(const char *active_path)
{
    for (int i = 0; i < VIDEO_STATE_MAX; ++i) {
        VideoState *st = &s_video[i];
        if (!st->loaded) continue;
        if (active_path && strcmp(st->path, active_path) == 0) continue;

        pause_state_playback(st);
        st->scrubbing = false;
        st->resume_after_scrub = false;
        st->pending_seek = 0.0f;
    }
}

/* ── Load ─────────────────────────────────────────────────────────── */

static VideoState *ensure_loaded(FvTab *tab)
{
    VideoState *st = find_state(tab->path);
    if (st) return st;

    st = alloc_state();
    if (!st) return nullptr;

    memset(st, 0, sizeof(*st));
    snprintf(st->path, sizeof(st->path), "%s", tab->path);
    st->gpu_tex.idx = UINT16_MAX;
    st->loaded      = true;     /* reserve the slot up-front */
    st->pending_audio_resume_seek = -1.0;

    if (!tab->content || tab->content_len <= 0) {
        st->load_failed = true;
        snprintf(st->fail_reason, sizeof(st->fail_reason),
                 "empty file (%d bytes)", tab->content_len);
        return st;
    }

    st->video = jce_video_load_memory(tab->content,
                                       (uint32_t)tab->content_len,
                                       tab->path);
    if (st->video == JCE_VIDEO_INVALID) {
        st->load_failed = true;
        snprintf(st->fail_reason, sizeof(st->fail_reason),
            "MP4 parser rejected stream: invalid container/track metadata.");
        LOG_WARN(LOG_TAG, "load failed: %s", tab->path);
        return st;
    }

    jce_video_get_info(st->video, &st->info);
    if (st->info.framerate <= 0.0)
        st->info.framerate = 30.0;

    if (!st->info.metadata_only
        && (st->info.width <= 0 || st->info.height <= 0 || st->info.framerate <= 0.0)) {
        st->load_failed = true;
        snprintf(st->fail_reason, sizeof(st->fail_reason),
            "invalid video metadata (%dx%d %.2ffps).",
            st->info.width, st->info.height, st->info.framerate);
        jce_video_unload(st->video);
        st->video = JCE_VIDEO_INVALID;
        LOG_WARN(LOG_TAG, "invalid metadata for %s", tab->path);
        return st;
    }

    st->playing = !st->info.metadata_only;
    LOG_INFO(LOG_TAG, "loaded %s %dx%d %.2ffps codec=%s metadata_only=%d dur=%.2fs",
             tab->display_name, st->info.width, st->info.height,
             st->info.framerate,
             st->info.video_codec[0] ? st->info.video_codec : "unkn",
             (int)st->info.metadata_only,
             st->info.duration);

    /* ── Embedded audio ──────────────────────────────────────────── */
    /* Audio is now decoded asynchronously in the background.
     * We check for readiness each frame in tick_playback(). */

    return st;
}

/* ── Frame upload ─────────────────────────────────────────────────── */

static void upload_latest_frame(VideoState *st)
{
    if (!st || st->video == JCE_VIDEO_INVALID) return;

    uint64_t counter = jce_video_get_frame_counter(st->video);
    if (counter == 0 || counter == st->uploaded_counter) return;

    int w = 0, h = 0;
    double frame_time = 0.0;
    const uint8_t *rgba = jce_video_get_frame_rgba(st->video,
                                                    &w, &h, &frame_time);
    if (!rgba || w <= 0 || h <= 0) return;

    const uint32_t uw = (uint32_t)w;
    const uint32_t uh = (uint32_t)h;

    if (jce_texture_valid(st->gpu_tex)
        && st->tex_w == w && st->tex_h == h) {
        const uint64_t t0 = jce_time_perf_counter();
        /* S5: zero-copy ref upload — bgfx borrows display_rgba until bgfx_frame().
         * display_rgba is stable for the full render frame (repopulated only on
         * the next jce_video_advance call, which runs before the next upload). */
        const bool ok = jce_texture_update_rgba_ref(st->gpu_tex, rgba, uw, uh);
        const uint64_t t1 = jce_time_perf_counter();
        if (ok) {
            const double us = (double)(t1 - t0) * 1e6
                            / (double)jce_time_perf_freq();
            st->perf_upload_us_last = us;
            st->perf_upload_us_ema = (st->perf_upload_us_ema <= 0.0)
                                     ? us
                                     : st->perf_upload_us_ema * 0.9 + us * 0.1;
            st->perf_uploads++;
            st->uploaded_counter = counter;
            return;
        }
    }

    if (jce_texture_valid(st->gpu_tex))
        jce_texture_destroy(st->gpu_tex);

    const uint64_t t0 = jce_time_perf_counter();
    st->gpu_tex = jce_texture_from_rgba(rgba, uw, uh);
    const uint64_t t1 = jce_time_perf_counter();
    const double us = (double)(t1 - t0) * 1e6
                    / (double)jce_time_perf_freq();
    st->perf_upload_us_last = us;
    st->perf_upload_us_ema = (st->perf_upload_us_ema <= 0.0)
                             ? us
                             : st->perf_upload_us_ema * 0.9 + us * 0.1;
    st->perf_uploads++;
    st->tex_w   = w;
    st->tex_h   = h;
    st->uploaded_counter = counter;
}

/* ── Per-frame playback tick ─────────────────────────────────────── */

static void tick_playback(VideoState *st, bool ui_focused)
{
    if (!st || st->video == JCE_VIDEO_INVALID) return;

    (void)ui_focused;

    /* ── Deferred audio resume after seek ───────────────────────
     * If a seek issued by finish_scrub asked us to wait for the worker
     * to actually produce a frame at the seek target before unpausing
     * audio, check progress here.  Resume audio (with audio_seek to
     * land at the same PTS) when either:
     *   - the worker has produced a new frame at/after the seek target;
     *   - the deadline elapsed (timeout fallback so audio doesn't stall
     *     forever if the decoder can't reach the target). */
    if (st->pending_audio_resume_seek >= 0.0
        && st->audio_available && st->voice != JCE_VOICE_INVALID) {
        const uint64_t cnt = jce_video_get_frame_counter(st->video);
        const double   vtime = jce_video_get_time(st->video);
        const double   target = st->pending_audio_resume_seek;
        const bool video_caught_up =
            (cnt > st->audio_resume_seek_counter
             && vtime + 0.05 >= target);
        const bool deadline_hit = (ImGui::GetTime() >= st->audio_resume_deadline);
        if (video_caught_up || deadline_hit) {
            jce_video_audio_seek(st->video, target);
            jce_audio_resume(st->audio, st->voice);
            st->pending_audio_resume_seek = -1.0;
        }
    }

    if (st->scrubbing) {
        /* While the user is dragging the seek widget we only update the
         * preview frame, not the playback clock. */
        return;
    }

    if (!st->playing) return;

    /* ── Deferred audio setup (async decode may have finished) ──── */
    if (!st->audio_available && st->video != JCE_VIDEO_INVALID) {
        JceVideoAudioStatus astatus = jce_video_get_audio_status(st->video);
        if (astatus == JCE_VIDEO_AUDIO_STATUS_READY) {
            uint32_t fmt_ch = 0, fmt_sr = 0;
            double   fmt_dur = 0.0;
            if (jce_video_get_audio_format(st->video,
                                           &fmt_ch, &fmt_sr, &fmt_dur)
                && fmt_ch > 0 && fmt_sr > 0) {
                st->audio = jce_audio_create();
                if (st->audio) {
                    /* Streaming voice: the audio engine pulls from the
                     * video's JceAudioStream on demand. The pull
                     * trampoline routes ma_data_source onRead() back
                     * into jce_video_audio_pull(). */
                    st->audio_available = true;
                    /* Sound handle is unused for streaming voices, but
                     * keep it valid-ish so the rest of the viewer's
                     * "is audio loaded" predicates still work. */
                    st->sound = (JceSound)1;
                    if (st->playing) {
                        st->voice = jce_audio_play_stream(
                            st->audio,
                            &fv_video_audio_pull_trampoline,
                            st,
                            (uint16_t)fmt_ch, fmt_sr,
                            1.0f, 1.0f);
                    }
                }
            }
        }
    }

    if (jce_video_has_ended(st->video)) {
        st->playing = false;
        if (st->audio_available && st->voice != JCE_VOICE_INVALID)
            jce_audio_pause(st->audio, st->voice);
        return;
    }

    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.25f) dt = 0.25f;   /* clamp long frames */
    jce_video_advance(st->video, (double)dt);

    /* ── A/V sync ────────────────────────────────────────────────
     * We previously auto-corrected drift by re-seeking the video to
     * audio_t whenever |drift| > 150 ms. That created a death loop
     * after seeks near the end: the video decoder needs N ticks to
     * walk from a keyframe to the requested time, during which
     * slot->time gets capped backward, drift grows, we re-seek
     * forward, decoder restarts, repeat. Net effect: picture never
     * catches up.
     *
     * The fix: trust the user's seek. Audio drives the clock for
     * has_ended / UI display; video plays at its own decoded pace.
     * Brief decoder stalls produce at most a fraction of a second of
     * drift that resolves itself. */
#if 0
    if (st->audio_available && st->voice != JCE_VOICE_INVALID) {
        float audio_t = (float)jce_video_audio_get_time(st->video);
        float video_t = (float)jce_video_get_time(st->video);
        float drift   = audio_t - video_t;
        bool warmup = (audio_t < 0.5f && video_t < 0.5f);
        if (!warmup && fabsf(drift) > 0.15f)
            jce_video_seek(st->video, (double)audio_t, false);
    }
#endif
    (void)st->sync_resume_at;

    if (jce_video_has_ended(st->video)) {
        st->playing = false;
        if (st->audio_available && st->voice != JCE_VOICE_INVALID)
            jce_audio_pause(st->audio, st->voice);
    }
}

/* ── Scrub helpers ────────────────────────────────────────────────── */

static void begin_scrub(VideoState *st, float seek_time)
{
    static const float kSeekDeadbandSec = 0.002f;

    if (!st) return;
    if (!st->scrubbing) {
        st->resume_after_scrub = st->playing;
        st->playing            = false;
        if (st->audio_available && st->voice != JCE_VOICE_INVALID)
            jce_audio_pause(st->audio, st->voice);
    }
    st->scrubbing    = true;
    st->pending_seek = seek_time;

    if (fabsf(seek_time - st->last_seek_issued) >= kSeekDeadbandSec) {
        /* Fast preview seek (snap to nearest intra-frame) while dragging. */
        jce_video_seek(st->video, (double)seek_time, false);
        st->last_seek_issued = seek_time;
    }
}

static void update_scrub(VideoState *st, float seek_time)
{
    static const float kSeekDeadbandSec = 0.002f;

    if (!st || !st->scrubbing) return;
    st->pending_seek = seek_time;

    if (fabsf(seek_time - st->last_seek_issued) < kSeekDeadbandSec)
        return;

    jce_video_seek(st->video, (double)seek_time, false);
    st->last_seek_issued = seek_time;
}

static void finish_scrub(VideoState *st)
{
    if (!st || !st->scrubbing) return;
    /* Exact seek at release for pixel-accurate landing. */
    jce_video_seek(st->video, (double)st->pending_seek, true);
    st->last_seek_issued = st->pending_seek;
    /* Give the video decoder ~1.5 s of grace to walk from the
     * keyframe to the requested position before A/V drift correction
     * starts second-guessing it. */
    st->sync_resume_at = ImGui::GetTime() + 1.5;

    /* The slider release happens after the normal per-frame upload pass. */
    upload_latest_frame(st);

    st->scrubbing = false;
    if (st->resume_after_scrub) {
        st->playing = true;
        if (st->audio_available && st->voice != JCE_VOICE_INVALID) {
            /* DON'T unpause audio yet — defer until the video worker has
             * actually produced a frame at the seek target.  Otherwise
             * audio races ahead while the decoder walks from the
             * keyframe (often 100–500 ms for HEVC), giving the classic
             * "audio plays, picture frozen" desync after release.
             *
             * tick_playback monitors frame_counter and resumes audio
             * (with audio_seek) once a fresh frame arrives, or after a
             * 750 ms timeout fallback. */
            st->pending_audio_resume_seek    = (double)st->pending_seek;
            st->audio_resume_seek_counter    = jce_video_get_frame_counter(st->video);
            st->audio_resume_deadline        = ImGui::GetTime() + 0.75;
        }
    } else if (st->audio_available && st->voice != JCE_VOICE_INVALID) {
        /* Not resuming playback — just reseat audio at the new position
         * so the next play press starts in sync. */
        jce_video_audio_seek(st->video, (double)st->pending_seek);
    }
    st->resume_after_scrub = false;
    st->pending_seek       = 0.0f;
}

void fv_video_update_focus(const char *active_tab_path, bool allow_playback)
{
    for (int i = 0; i < VIDEO_STATE_MAX; ++i) {
        VideoState *st = &s_video[i];
        bool is_active_tab;
        bool can_keep_playing;

        if (!st->loaded) continue;

        is_active_tab = (active_tab_path && strcmp(st->path, active_tab_path) == 0);
        can_keep_playing = allow_playback && is_active_tab;
        if (!can_keep_playing) {
            /* Fully release the webm/MP4 parser, decoded PCM buffer, and
             * GPU texture for any tab that's not the active video viewer.
             * Each loaded video can pin 80MB+ of mkvparser cluster index
             * + decoded PCM; keeping 5 inactive tabs around bloated heap
             * to several GB. The tab's file content stays cached by the
             * file viewer, so switching back transparently re-loads. */
            if (!is_active_tab) {
                free_state(st);
                continue;
            }
            pause_state_playback(st);
            st->scrubbing = false;
            st->resume_after_scrub = false;
            st->pending_seek = 0.0f;
        }
    }
}

void fv_video_request_play(const char *path)
{
    if (!path) return;
    VideoState *st = find_state(path);
    if (!st || !st->loaded || st->playing) return;
    if (st->video == JCE_VIDEO_INVALID || st->info.metadata_only) return;

    stop_other_playback(st->path);
    if (jce_video_has_ended(st->video)) {
        jce_video_rewind(st->video);
        if (st->audio_available && st->voice != JCE_VOICE_INVALID)
            jce_video_audio_seek(st->video, 0.0);
    }
    if (st->audio_available) {
        if (st->voice == JCE_VOICE_INVALID)
            st->voice = fv_video_start_stream_voice(st);
        else
            jce_audio_resume(st->audio, st->voice);
    }
    st->playing = true;
}

/* ── Render ───────────────────────────────────────────────────────── */

void fv_render_video(FvTab *tab)
{
    VideoState *st = ensure_loaded(tab);
    if (!st) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
            "Failed to allocate video state for %s", tab->display_name);
        return;
    }

    const bool ui_focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    tick_playback(st, ui_focused);
    upload_latest_frame(st);

    /* ── Toolbar ─────────────────────────────────────────────────── */

    const bool has_video = (st->video != JCE_VIDEO_INVALID);
    const bool can_decode = has_video && !st->info.metadata_only;
    const float dur  = has_video ? (float)st->info.duration : 0.0f;
    float cur_time   = has_video ? (float)jce_video_get_time(st->video) : 0.0f;
    if (st->scrubbing) cur_time = st->pending_seek;

    ImGui::BeginDisabled(!can_decode);
    if (ImGui::Button(st->playing ? "  ||  " : "  >  ")) {
        if (!st->playing) {
            stop_other_playback(st->path);
            if (jce_video_has_ended(st->video)) {
                jce_video_rewind(st->video);
                if (st->audio_available && st->voice != JCE_VOICE_INVALID)
                    jce_video_audio_seek(st->video, 0.0);
            }
            if (st->audio_available) {
                if (st->voice == JCE_VOICE_INVALID)
                    st->voice = fv_video_start_stream_voice(st);
                else
                    jce_audio_resume(st->audio, st->voice);
            }
            st->playing = true;
        } else {
            pause_state_playback(st);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(" |< ")) {
        jce_video_rewind(st->video);
        if (st->audio_available && st->voice != JCE_VOICE_INVALID)
            jce_video_audio_seek(st->video, 0.0);
        st->last_seek_issued = 0.0f;
        cur_time = 0.0f;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    double kb = (double)tab->file_size / 1024.0;
    if (has_video) {
        if (kb >= 1024.0)
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                "%s  |  %dx%d  |  %.2f fps  |  %s  |  %.2f MB",
                tab->display_name, st->info.width, st->info.height,
                st->info.framerate,
                st->info.video_codec[0] ? st->info.video_codec : "unkn",
                kb / 1024.0);
        else
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                "%s  |  %dx%d  |  %.2f fps  |  %s  |  %.1f KB",
                tab->display_name, st->info.width, st->info.height,
                st->info.framerate,
                st->info.video_codec[0] ? st->info.video_codec : "unkn",
                kb);
    } else {
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
            "%s  |  %.1f KB", tab->display_name, kb);
    }

    /* ── Seek slider ─────────────────────────────────────────────── */

    ImGui::PushItemWidth(-1);
    ImGui::BeginDisabled(!can_decode);
    float slider = cur_time;
    bool slider_changed = ImGui::SliderFloat("##vidseek",
        &slider, 0.0f, dur > 0.0f ? dur : 1.0f, "%.2fs");
    bool slider_active = ImGui::IsItemActive();
    if (has_video) {
        if (slider_active) {
            if (!st->scrubbing)
                begin_scrub(st, slider);
            else if (slider_changed)
                update_scrub(st, slider);
        } else if (st->scrubbing) {
            finish_scrub(st);
        }
    }
    ImGui::EndDisabled();
    ImGui::PopItemWidth();

    ImGui::Text("%.2fs / %.2fs", cur_time, dur);

    /* ── Perf overlay (S1 — debug) ───────────────────────────────── */
    ImGui::SameLine();
    ImGui::Checkbox("perf", &st->show_perf_overlay);
    if (st->show_perf_overlay && has_video) {
        JceVideoPerfStats ps;
        if (jce_video_get_perf_stats(st->video, &ps)) {
            const double dec  = ps.decode_us_ema  / 1000.0;
            const double cv   = ps.convert_us_ema / 1000.0;
            const double push = ps.push_us_ema    / 1000.0;
            const double pop  = ps.pop_us_ema     / 1000.0;
            const double up   = st->perf_upload_us_ema / 1000.0;
            const double worker_total = dec + cv + push;
            const double ui_total     = pop + up;
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                "worker: dec %5.2f | yuv→rgba %5.2f | push %5.2f = %5.2f ms"
                "   ui: pop %5.2f | upload %5.2f = %5.2f ms"
                "   q %d/%d   dec %llu  disp %llu  drop %llu",
                dec, cv, push, worker_total,
                pop, up, ui_total,
                ps.q_count, ps.q_capacity,
                (unsigned long long)ps.frames_decoded,
                (unsigned long long)ps.frames_displayed,
                (unsigned long long)ps.frames_dropped);
            const double budget = 1000.0 / (st->info.framerate > 0.0
                                            ? st->info.framerate : 60.0);
            const ImVec4 worker_color = (worker_total > budget)
                                        ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                        : ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
            ImGui::TextColored(worker_color,
                "  budget %.2f ms (%.1f fps) | worker headroom %+.2f ms | ui headroom %+.2f ms",
                budget, st->info.framerate,
                budget - worker_total, budget - ui_total);
        }
    }

    /* Space key toggles play/pause when the file viewer is focused. */
    if (can_decode && ui_focused && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        if (!st->playing) {
            stop_other_playback(st->path);
            if (jce_video_has_ended(st->video)) {
                jce_video_rewind(st->video);
                if (st->audio_available && st->voice != JCE_VOICE_INVALID)
                    jce_video_audio_seek(st->video, 0.0);
            }
            if (st->audio_available) {
                if (st->voice == JCE_VOICE_INVALID)
                    st->voice = fv_video_start_stream_voice(st);
                else
                    jce_audio_resume(st->audio, st->voice);
            }
            st->playing = true;
        } else {
            pause_state_playback(st);
        }
    }

    ImGui::Separator();

    /* ── Frame display (shared zoomable canvas) ──────────────────── */

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 80.0f) avail.y = 80.0f;

    if (has_video && jce_texture_valid(st->gpu_tex)
        && st->tex_w > 0 && st->tex_h > 0)
    {
        /* Inline zoom toolbar above the canvas. Reuses FvTab zoom/pan
         * storage which is otherwise unused by the video viewer. */
        ImGui::SetNextItemWidth(120);
        float zoom_pct = (tab->zoom > 0.0f ? tab->zoom : 1.0f) * 100.0f;
        if (ImGui::SliderFloat("##vzoom", &zoom_pct, 10.0f, 800.0f, "%.0f%%")) {
            tab->zoom = zoom_pct / 100.0f;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n_id("viewer.video.fit", "vfit"))) {
            FvZoomable zp{};
            zp.content_w = st->tex_w; zp.content_h = st->tex_h;
            zp.zoom = &tab->zoom; zp.pan_x = &tab->pan_x; zp.pan_y = &tab->pan_y;
            ImVec2 a = ImGui::GetContentRegionAvail();
            fv_zoomable_fit(&zp, a);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("1:1##v11")) {
            FvZoomable zp{};
            zp.zoom = &tab->zoom; zp.pan_x = &tab->pan_x; zp.pan_y = &tab->pan_y;
            fv_zoomable_one_to_one(&zp);
        }
        ImGui::SameLine();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
            "wheel=zoom  drag=pan  dbl-click=Fit/1:1");

        FvZoomable zp{};
        zp.tex         = st->gpu_tex;
        zp.content_w   = st->tex_w;
        zp.content_h   = st->tex_h;
        zp.zoom        = &tab->zoom;
        zp.pan_x       = &tab->pan_x;
        zp.pan_y       = &tab->pan_y;
        zp.allow_double_click_toggle = true;
        zp.matte_color = IM_COL32(12, 12, 16, 255);
        fv_render_zoomable(&zp);
    } else {
        /* Placeholder when no frame has been decoded yet, or on load failure. */
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + avail.x, origin.y + avail.y),
                          IM_COL32(18, 18, 24, 255));
        dl->AddRect(origin,
                    ImVec2(origin.x + avail.x, origin.y + avail.y),
                    IM_COL32(60, 60, 80, 255));

        const char *msg = "Decoding...";
        char patent_msg[192];
        if (has_video) {
            if (st->info.metadata_only) {
                const char *vc = st->info.video_codec;
                bool is_h264  = (strcmp(vc, "avc1") == 0 || strcmp(vc, "avc3") == 0);
                bool is_h265  = (strcmp(vc, "hvc1") == 0 || strcmp(vc, "hev1") == 0);
                if (is_h264 || is_h265) {
                    snprintf(patent_msg, sizeof(patent_msg),
                        "%s is patent-encumbered and disabled in this build.\n",
                        is_h264 ? "H.264 / AVC" : "H.265 / HEVC");
                    msg = patent_msg;
                } else {
                    msg = "Container parsed, but runtime decoder backend is unavailable.";
                }
            }
        } else {
            msg = st->load_failed ? st->fail_reason : "No frame available";
        }
        ImVec2 tsz = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(origin.x + (avail.x - tsz.x) * 0.5f,
                           origin.y + (avail.y - tsz.y) * 0.5f),
                    has_video ? IM_COL32(160, 160, 170, 255)
                              : IM_COL32(255, 120, 120, 255),
                    msg);

        ImGui::Dummy(avail);
    }
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

void fv_video_close_tab(FvTab *tab)
{
    if (!tab) return;
    VideoState *st = find_state(tab->path);
    free_state(st);
}
