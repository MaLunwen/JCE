/*
 * jce_fv_video.cpp  Video file viewer.
 *
 * Decoding full video streams requires a heavyweight codec library
 * (ffmpeg / libvpx / dav1d) which is not currently a dependency of
 * JCE_Editor.  Until that is added, this sub-viewer provides:
 *
 *   • Container detection from the header bytes read into tab->content
 *     (MP4 / MOV / M4V, Matroska / WebM, AVI, FLV, MPEG-PS, Ogg).
 *   • File metadata (size, path, extension).
 *   • "Open in external player" button that hands the file off to the
 *     platform's default video application.
 *   • "Copy path" helper for pipelines that want to inspect the file
 *     with an external tool.
 *
 * When a real decoder is wired up later, the render function can grow
 * a proper frame view and transport controls similar to jce_fv_audio.cpp.
 */

#include "jce_fv_common.h"

#define LOG_TAG "fv_video"

/* ── Per-tab video state (keyed by tab path) ─────────────────────── */

struct VideoState {
    char        path[512];
    bool        loaded;

    /* Sniffed container / codec info. */
    const char *container;   /* human-readable container name */
    char        brand[8];    /* mp4 major brand / ftyp ("isom", "mp42"…) */
    bool        has_brand;
};

#define VIDEO_STATE_MAX 16
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

/* ── Container sniffing ──────────────────────────────────────────── */

static bool mem_equal(const unsigned char *d, int len, int off,
                      const char *sig, int sig_len)
{
    if (off < 0 || off + sig_len > len) return false;
    return memcmp(d + off, sig, (size_t)sig_len) == 0;
}

static void sniff_container(VideoState *st,
                            const unsigned char *d, int len,
                            const char *ext)
{
    st->container = nullptr;
    st->has_brand = false;
    st->brand[0]  = '\0';

    if (!d || len < 12) {
        /* Fall back to extension-only detection. */
        if (ext && ext[0]) {
            if (strcmp(ext, ".mp4") == 0 || strcmp(ext, ".m4v") == 0)
                st->container = "MP4";
            else if (strcmp(ext, ".mov") == 0)
                st->container = "QuickTime MOV";
            else if (strcmp(ext, ".mkv") == 0)
                st->container = "Matroska";
            else if (strcmp(ext, ".webm") == 0)
                st->container = "WebM";
            else if (strcmp(ext, ".avi") == 0)
                st->container = "AVI";
            else if (strcmp(ext, ".flv") == 0)
                st->container = "FLV";
            else if (strcmp(ext, ".mpg") == 0 || strcmp(ext, ".mpeg") == 0)
                st->container = "MPEG-PS";
            else if (strcmp(ext, ".ogv") == 0)
                st->container = "Ogg";
            else if (strcmp(ext, ".wmv") == 0)
                st->container = "ASF/WMV";
            else if (strcmp(ext, ".3gp") == 0)
                st->container = "3GP";
        }
        return;
    }

    /* ISO BMFF family (MP4, MOV, M4V, 3GP): bytes 4..7 == "ftyp". */
    if (mem_equal(d, len, 4, "ftyp", 4)) {
        memcpy(st->brand, d + 8, 4);
        st->brand[4]   = '\0';
        st->has_brand  = true;

        if (mem_equal(d, len, 8, "qt  ", 4))
            st->container = "QuickTime MOV";
        else if (mem_equal(d, len, 8, "3gp", 3)
              || mem_equal(d, len, 8, "3g2", 3))
            st->container = "3GP";
        else
            st->container = "MP4 (ISO BMFF)";
        return;
    }

    /* EBML — Matroska / WebM: starts with 0x1A 0x45 0xDF 0xA3. */
    if (len >= 4 && d[0] == 0x1A && d[1] == 0x45
        && d[2] == 0xDF && d[3] == 0xA3)
    {
        /* Look for "webm" DocType within the first header block. */
        int scan_end = len < 256 ? len : 256;
        for (int i = 0; i + 4 < scan_end; ++i) {
            if (d[i] == 'w' && d[i + 1] == 'e'
                && d[i + 2] == 'b' && d[i + 3] == 'm') {
                st->container = "WebM";
                return;
            }
        }
        st->container = "Matroska";
        return;
    }

    /* RIFF container family. */
    if (mem_equal(d, len, 0, "RIFF", 4)) {
        if (mem_equal(d, len, 8, "AVI ", 4))
            st->container = "AVI";
        else
            st->container = "RIFF";
        return;
    }

    /* FLV: "FLV" + version byte. */
    if (mem_equal(d, len, 0, "FLV", 3)) {
        st->container = "FLV";
        return;
    }

    /* Ogg: "OggS". */
    if (mem_equal(d, len, 0, "OggS", 4)) {
        st->container = "Ogg";
        return;
    }

    /* MPEG program stream pack header: 00 00 01 BA. */
    if (len >= 4 && d[0] == 0x00 && d[1] == 0x00
        && d[2] == 0x01 && d[3] == 0xBA) {
        st->container = "MPEG-PS";
        return;
    }

    /* ASF / WMV GUID (30 26 B2 75 ...). */
    if (len >= 4 && d[0] == 0x30 && d[1] == 0x26
        && d[2] == 0xB2 && d[3] == 0x75) {
        st->container = "ASF/WMV";
        return;
    }

    /* Extension fallback. */
    st->container = "Unknown video";
    (void)ext;
}

static VideoState *ensure_loaded(FvTab *tab)
{
    VideoState *st = find_state(tab->path);
    if (st) return st;

    st = alloc_state();
    if (!st) return nullptr;

    memset(st, 0, sizeof(*st));
    snprintf(st->path, sizeof(st->path), "%s", tab->path);

    sniff_container(st,
                    (const unsigned char *)tab->content,
                    tab->content_len,
                    tab->ext);

    st->loaded = true;

    LOG_INFO(LOG_TAG, "loaded '%s' container=%s%s%s",
             tab->display_name,
             st->container ? st->container : "unknown",
             st->has_brand ? " brand=" : "",
             st->has_brand ? st->brand : "");
    return st;
}

/* ── Launch external player ──────────────────────────────────────── */

static void launch_external_player(const char *path)
{
    if (!path || !path[0]) return;

    char cmd[768];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", path);
#elif defined(__APPLE__)
    snprintf(cmd, sizeof(cmd), "open \"%s\"", path);
#else
    /* Linux / BSD: xdg-open hands off to the user's default handler. */
    snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", path);
#endif

    int rc = system(cmd);
    if (rc != 0)
        LOG_WARN(LOG_TAG, "external player launch returned %d for '%s'",
                 rc, path);
}

/* ── Render ───────────────────────────────────────────────────────── */

void fv_render_video(FvTab *tab)
{
    VideoState *st = ensure_loaded(tab);

    /* Toolbar row: open externally + file summary. */
    if (ImGui::Button(jce_editor_i18n("viewer.openExternal")))
        launch_external_player(tab->path);

    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("assetBrowser.copyPath")))
        ImGui::SetClipboardText(tab->path);

    ImGui::SameLine();
    double kb = (double)tab->file_size / 1024.0;
    if (kb >= 1024.0)
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                           "%s  |  %.2f MB",
                           st && st->container ? st->container : "Video",
                           kb / 1024.0);
    else
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY,
                           "%s  |  %.1f KB",
                           st && st->container ? st->container : "Video",
                           kb);

    ImGui::Separator();
    ImGui::Spacing();

    /* Big centred placeholder — no inline decoder yet. */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  preview_h = avail.y - 140.0f;
    if (preview_h < 120.0f) preview_h = 120.0f;

    ImVec2 preview_pos  = ImGui::GetCursorScreenPos();
    ImVec2 preview_size = ImVec2(avail.x, preview_h);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(preview_pos,
                      ImVec2(preview_pos.x + preview_size.x,
                             preview_pos.y + preview_size.y),
                      IM_COL32(18, 18, 24, 255));
    dl->AddRect(preview_pos,
                ImVec2(preview_pos.x + preview_size.x,
                       preview_pos.y + preview_size.y),
                IM_COL32(60, 60, 80, 255));

    /* Play-triangle glyph, centred. */
    float cx = preview_pos.x + preview_size.x * 0.5f;
    float cy = preview_pos.y + preview_size.y * 0.5f;
    float r  = preview_size.y * 0.12f;
    if (r > 48.0f) r = 48.0f;
    if (r < 16.0f) r = 16.0f;

    ImVec2 p0(cx - r * 0.6f, cy - r);
    ImVec2 p1(cx - r * 0.6f, cy + r);
    ImVec2 p2(cx + r,        cy);
    dl->AddTriangleFilled(p0, p1, p2, IM_COL32(120, 180, 255, 220));

    /* Hint text under the glyph. */
    const char *hint = jce_editor_i18n("viewer.videoNoInlinePlayback");
    ImVec2 tsz = ImGui::CalcTextSize(hint);
    dl->AddText(ImVec2(cx - tsz.x * 0.5f, cy + r + 12.0f),
                IM_COL32(160, 160, 170, 255), hint);

    ImGui::Dummy(preview_size);
    ImGui::Spacing();

    /* Metadata block. */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
                       jce_editor_i18n("viewer.videoMetadata"));
    ImGui::Separator();

    if (ImGui::BeginTable("##vid_meta", 2,
                          ImGuiTableFlags_SizingStretchProp
                          | ImGuiTableFlags_NoBordersInBody))
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                           jce_editor_i18n("viewer.videoContainer"));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(
            (st && st->container) ? st->container : "—");

        if (st && st->has_brand) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                               jce_editor_i18n("viewer.videoBrand"));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st->brand);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                           jce_editor_i18n("viewer.videoExtension"));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tab->ext[0] ? tab->ext : "—");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                           jce_editor_i18n("viewer.videoSize"));
        ImGui::TableNextColumn();
        if (kb >= 1024.0)
            ImGui::Text("%.2f MB (%ld bytes)",
                        kb / 1024.0, tab->file_size);
        else
            ImGui::Text("%.1f KB (%ld bytes)",
                        kb, tab->file_size);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s",
                           jce_editor_i18n("viewer.videoPath"));
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", tab->path);

        ImGui::EndTable();
    }
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

void fv_video_close_tab(FvTab *tab)
{
    if (!tab) return;
    VideoState *st = find_state(tab->path);
    if (!st) return;
    memset(st, 0, sizeof(*st));
}
