/*
 * jce_fv_video.cpp  Video file viewer — MPEG-1 playback via jce_video.
 *
 * Mirrors jce_fv_audio.cpp: each tab owns a VideoState with an engine
 * JceVideo handle, a bgfx texture holding the most recent decoded
 * frame, and transport state.  Playback drives plm_decode() through
 * jce_video_advance() based on the ImGui frame delta time.
 *
 * Supported container: MPEG Program Stream (.mpg / .mpeg / .m1v).
 * Other extensions are still detected as JCE_FV_VIDEO so they open in
 * a tab, but the engine rejects them and we show a clear diagnostic.
 */

#include "jce_fv_common.h"

extern "C" {
#include <jce/video/jce_video.h>
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
    if (st->video != JCE_VIDEO_INVALID) {
        jce_video_unload(st->video);
        st->video = JCE_VIDEO_INVALID;
    }
    memset(st, 0, sizeof(*st));
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
            "decoder rejected stream — this viewer supports MPEG-1 "
            "(.mpg/.mpeg/.m1v); other containers need a codec.");
        LOG_WARN(LOG_TAG, "load failed: %s", tab->path);
        return st;
    }

    jce_video_get_info(st->video, &st->info);
    LOG_INFO(LOG_TAG, "loaded %s %dx%d %.2ffps dur=%.2fs",
             tab->display_name, st->info.width, st->info.height,
             st->info.framerate, st->info.duration);
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

    /* bgfx has no cheap in-place 2D update wrapper in this codebase, so
     * we recreate the texture each frame.  For MPEG-1 preview rates
     * (~24-30 fps at a few hundred Kpx) this is well within budget. */
    if (jce_texture_valid(st->gpu_tex))
        jce_texture_destroy(st->gpu_tex);

    st->gpu_tex = jce_texture_from_rgba(rgba, (uint32_t)w, (uint32_t)h);
    st->tex_w   = w;
    st->tex_h   = h;
    st->uploaded_counter = counter;
}

/* ── Per-frame playback tick ─────────────────────────────────────── */

static void tick_playback(VideoState *st, bool ui_focused)
{
    if (!st || st->video == JCE_VIDEO_INVALID) return;

    /* Pause when the tab isn't focused, same contract as the audio
     * viewer — avoids runaway decode on background tabs. */
    if (!ui_focused) return;

    if (st->scrubbing) {
        /* While the user is dragging the seek widget we only update the
         * preview frame, not the playback clock. */
        return;
    }

    if (!st->playing) return;

    if (jce_video_has_ended(st->video)) {
        st->playing = false;
        return;
    }

    float dt = ImGui::GetIO().DeltaTime;
    if (dt > 0.25f) dt = 0.25f;   /* clamp long frames */
    jce_video_advance(st->video, (double)dt);

    if (jce_video_has_ended(st->video))
        st->playing = false;
}

/* ── Scrub helpers ────────────────────────────────────────────────── */

static void begin_scrub(VideoState *st, float seek_time)
{
    if (!st) return;
    if (!st->scrubbing) {
        st->resume_after_scrub = st->playing;
        st->playing            = false;
    }
    st->scrubbing    = true;
    st->pending_seek = seek_time;
    /* Fast preview seek (snap to nearest intra-frame) while dragging. */
    jce_video_seek(st->video, (double)seek_time, false);
}

static void update_scrub(VideoState *st, float seek_time)
{
    if (!st || !st->scrubbing) return;
    if (fabsf(seek_time - st->pending_seek) < 0.01f) return;
    st->pending_seek = seek_time;
    jce_video_seek(st->video, (double)seek_time, false);
}

static void finish_scrub(VideoState *st)
{
    if (!st || !st->scrubbing) return;
    /* Exact seek at release for pixel-accurate landing. */
    jce_video_seek(st->video, (double)st->pending_seek, true);
    st->scrubbing = false;
    if (st->resume_after_scrub)
        st->playing = true;
    st->resume_after_scrub = false;
    st->pending_seek       = 0.0f;
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
    const float dur  = has_video ? (float)st->info.duration : 0.0f;
    float cur_time   = has_video ? (float)jce_video_get_time(st->video) : 0.0f;
    if (st->scrubbing) cur_time = st->pending_seek;

    ImGui::BeginDisabled(!has_video || !ui_focused);
    if (ImGui::Button(st->playing ? "  ||  " : "  >  ")) {
        if (!st->playing) {
            if (jce_video_has_ended(st->video))
                jce_video_rewind(st->video);
            st->playing = true;
        } else {
            st->playing = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(" |< ")) {
        jce_video_rewind(st->video);
        cur_time = 0.0f;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    double kb = (double)tab->file_size / 1024.0;
    if (has_video) {
        if (kb >= 1024.0)
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                "%s  |  %dx%d  |  %.2f fps  |  %.2f MB",
                tab->display_name, st->info.width, st->info.height,
                st->info.framerate, kb / 1024.0);
        else
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                "%s  |  %dx%d  |  %.2f fps  |  %.1f KB",
                tab->display_name, st->info.width, st->info.height,
                st->info.framerate, kb);
    } else {
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
            "%s  |  %.1f KB", tab->display_name, kb);
    }

    if (has_video && !ui_focused)
        ImGui::TextDisabled("Focus this panel to play");

    /* ── Seek slider ─────────────────────────────────────────────── */

    ImGui::PushItemWidth(-1);
    ImGui::BeginDisabled(!has_video);
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
            ? "Decoding..."
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
