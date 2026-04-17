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

extern "C" {
#include <jce/video/jce_video.h>
#include <jce/audio/jce_audio.h>
}

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

    /* Embedded audio playback (decoded MP4 audio track). */
    JceAudio   *audio;
    JceSound    sound;
    JceVoice    voice;
    bool        audio_available;
};

#define VIDEO_STATE_MAX 8
static VideoState s_video[VIDEO_STATE_MAX];

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
    uint32_t pcm_frames = 0, pcm_ch = 0, pcm_rate = 0;
    const int16_t *pcm = jce_video_get_audio_pcm(st->video,
                                                  &pcm_frames, &pcm_ch, &pcm_rate);
    if (pcm && pcm_frames > 0 && pcm_ch > 0 && pcm_rate > 0) {
        st->audio = jce_audio_create();
        if (st->audio) {
            uint32_t pcm_size = pcm_frames * pcm_ch * (uint32_t)sizeof(int16_t);
            st->sound = jce_audio_load_pcm(st->audio, pcm, pcm_size,
                                            (uint16_t)pcm_ch, pcm_rate, 16);
            if (st->sound != JCE_SOUND_INVALID) {
                st->audio_available = true;
                /* Auto-play audio in sync with auto-playing video. */
                if (st->playing)
                    st->voice = jce_audio_play(st->audio, st->sound,
                                                false, 1.0f, 1.0f);
            } else {
                jce_audio_destroy(st->audio);
                st->audio = nullptr;
            }
        }
    }

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
        && st->tex_w == w && st->tex_h == h
        && jce_texture_update_rgba(st->gpu_tex, rgba, uw, uh)) {
        st->uploaded_counter = counter;
        return;
    }

    if (jce_texture_valid(st->gpu_tex))
        jce_texture_destroy(st->gpu_tex);

    st->gpu_tex = jce_texture_from_rgba(rgba, uw, uh);
    st->tex_w   = w;
    st->tex_h   = h;
    st->uploaded_counter = counter;
}

/* ── Per-frame playback tick ─────────────────────────────────────── */

static void tick_playback(VideoState *st, bool ui_focused)
{
    if (!st || st->video == JCE_VIDEO_INVALID) return;

    (void)ui_focused;

    if (st->scrubbing) {
        /* While the user is dragging the seek widget we only update the
         * preview frame, not the playback clock. */
        return;
    }

    if (!st->playing) return;

    if (jce_video_has_ended(st->video)) {
        st->playing = false;
        if (st->audio_available && st->voice != JCE_VOICE_INVALID)
            jce_audio_pause(st->audio, st->voice);
        return;
    }

    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.25f) dt = 0.25f;   /* clamp long frames */
    jce_video_advance(st->video, (double)dt);

    /* ── A/V sync: audio is the master clock ─────────────────────── */
    if (st->audio_available && st->voice != JCE_VOICE_INVALID) {
        float audio_t = jce_audio_get_time(st->audio, st->voice);
        float video_t = (float)jce_video_get_time(st->video);
        float drift   = audio_t - video_t;
        /* Skip sync during initial ramp-up: audio needs a moment to start
         * producing valid time, while video may decode several frames in the
         * first tick.  A seek at this stage flushes reference frames and
         * causes "B-Slice cannot be continued" errors in OpenH264. */
        bool warmup = (audio_t < 0.5f && video_t < 0.5f);
        if (!warmup && fabsf(drift) > 0.15f)
            jce_video_seek(st->video, (double)audio_t, false);
    }

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

    /* The slider release happens after the normal per-frame upload pass. */
    upload_latest_frame(st);

    /* Seek audio to the same position. */
    if (st->audio_available && st->voice != JCE_VOICE_INVALID)
        jce_audio_seek(st->audio, st->voice, st->pending_seek);

    st->scrubbing = false;
    if (st->resume_after_scrub) {
        st->playing = true;
        if (st->audio_available && st->voice != JCE_VOICE_INVALID)
            jce_audio_resume(st->audio, st->voice);
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
            pause_state_playback(st);
            st->scrubbing = false;
            st->resume_after_scrub = false;
            st->pending_seek = 0.0f;
        }
    }
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
                    jce_audio_seek(st->audio, st->voice, 0.0f);
            }
            if (st->audio_available) {
                if (st->voice == JCE_VOICE_INVALID)
                    st->voice = jce_audio_play(st->audio, st->sound,
                                                false, 1.0f, 1.0f);
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
            jce_audio_seek(st->audio, st->voice, 0.0f);
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

    if (has_video && !ui_focused)
        ImGui::TextDisabled("Playback pauses when viewer is not focused");

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

    ImGui::Separator();

    /* ── Frame display ───────────────────────────────────────────── */

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y < 80.0f) avail.y = 80.0f;

    if (has_video && jce_texture_valid(st->gpu_tex)
        && st->tex_w > 0 && st->tex_h > 0)
    {
        /* Letterbox-fit the frame inside the remaining area. */
        float sx = avail.x / (float)st->tex_w;
        float sy = avail.y / (float)st->tex_h;
        float scale = (sx < sy) ? sx : sy;
        if (scale < 0.01f) scale = 0.01f;

        float disp_w = (float)st->tex_w * scale;
        float disp_h = (float)st->tex_h * scale;

        float ox = (avail.x - disp_w) * 0.5f;
        float oy = (avail.y - disp_h) * 0.5f;
        if (ox < 0.0f) ox = 0.0f;
        if (oy < 0.0f) oy = 0.0f;

        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        /* Matte behind the frame so the letterbox bars are clearly framed. */
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + avail.x, origin.y + avail.y),
                          IM_COL32(12, 12, 16, 255));

        ImGui::SetCursorScreenPos(ImVec2(origin.x + ox, origin.y + oy));
        ImGui::Image((ImTextureID)(uintptr_t)st->gpu_tex.idx,
                      ImVec2(disp_w, disp_h));

        ImGui::SetCursorScreenPos(
            ImVec2(origin.x, origin.y + avail.y));
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

        const char *msg = has_video
            ? (st->info.metadata_only
                ? "MP4 parsed, but runtime decoder backend is unavailable."
                : "Decoding...")
            : (st->load_failed
                ? st->fail_reason
                : "No frame available");
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
