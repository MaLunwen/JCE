/*
 * jce_panel_file_viewer.cpp  Tab manager + dispatch for file viewer.
 *
 * Sub-viewers are in editor/src/viewers/:
 *   jce_fv_code.cpp   — Text/Code with syntax highlighting, find/replace, edit
 *   jce_fv_image.cpp  — Image with zoom, pan, checkered background
 *   jce_fv_model.cpp  — 3D model with wireframe, grid, rotation
 *   jce_fv_hex.cpp    — Hex dump + Scene viewer
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_state.h"
#include "viewers/jce_fv_common.h"

#include <jce/tools/jce_imgui_internal.h>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_host_shell.h>
#include <jce/renderer/jce_image.h>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/resource/jce_image_decode.h>
}

#include <math.h>

#define LOG_TAG "file_viewer"

namespace fs = std::filesystem;

static std::string normalize_path_string(const char *path)
{
    if (!path || !path[0]) return std::string();
    try {
        return fs::weakly_canonical(fs::path(path)).generic_string();
    } catch (...) {
        std::string s(path);
        for (char &ch : s)
            if (ch == '\\') ch = '/';
        return s;
    }
}

static bool is_scene_file_path(const char *path)
{
    if (!path || !path[0]) return false;

    std::string p = normalize_path_string(path);
    for (char &ch : p)
        ch = (char)tolower((unsigned char)ch);

    const char *sp = strstr(p.c_str(), ".scene");
    if (!sp) return false;
    return (strcmp(sp, ".scene") == 0 || strcmp(sp, ".scene.json") == 0);
}

/* ══════════════════════════════════════════════════════════════════════
 *  STATIC STATE
 * ══════════════════════════════════════════════════════════════════════ */

static struct {
    FvTab tabs[FV_MAX_TABS];
    int   tab_count;
    int   active_tab;
    bool  want_focus;
    int   select_tab_req;  /* >= 0: switch to this tab index next draw */
    bool  request_autoplay; /* one-shot: auto-play when re-selecting A/V tab */
} s_fv;

/* ══════════════════════════════════════════════════════════════════════
 *  INTERNAL HELPERS
 * ══════════════════════════════════════════════════════════════════════ */

static void fv_close_tab(int idx)
{
    if (idx < 0 || idx >= s_fv.tab_count) return;

    FvTab *tab = &s_fv.tabs[idx];

    /* Cleanup viewer-specific state */
    if (tab->type == JCE_FV_MODEL)
        fv_model_close_tab(tab->path);
    if (tab->type == JCE_FV_AUDIO)
        fv_audio_close_tab(tab);
    if (tab->type == JCE_FV_VIDEO)
        fv_video_close_tab(tab);
    fv_code_close_tab(tab);

    if (jce_texture_valid(tab->gpu_tex))
        jce_texture_destroy(tab->gpu_tex);

    ED_FREE(tab->content);
    tab->content = NULL;

    for (int i = idx; i < s_fv.tab_count - 1; i++)
        s_fv.tabs[i] = s_fv.tabs[i + 1];
    s_fv.tab_count--;
    memset(&s_fv.tabs[s_fv.tab_count], 0, sizeof(FvTab));

    if (s_fv.active_tab >= s_fv.tab_count)
        s_fv.active_tab = s_fv.tab_count - 1;
}

/* ── Type detection ──────────────────────────────────────────────── */

static JceFileViewerType fv_detect_ext(const char *ext)
{
    if (!ext || !ext[0]) return JCE_FV_TEXT;

    if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0
        || strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".bmp") == 0
        || strcmp(ext, ".tga") == 0 || strcmp(ext, ".hdr") == 0
        || strcmp(ext, ".gif") == 0 || strcmp(ext, ".webp") == 0
        || strcmp(ext, ".ico") == 0 || strcmp(ext, ".psd") == 0
        || strcmp(ext, ".dds") == 0 || strcmp(ext, ".ktx") == 0
        || strcmp(ext, ".ktx2") == 0)
        return JCE_FV_IMAGE;

    if (strcmp(ext, ".gltf") == 0 || strcmp(ext, ".glb") == 0
        || strcmp(ext, ".obj") == 0 || strcmp(ext, ".fbx") == 0
        || strcmp(ext, ".dae") == 0 || strcmp(ext, ".3ds") == 0)
        return JCE_FV_MODEL;

    /* .mat.json is detected via the full path in jce_file_viewer_open(),
     * since the ext here will be just ".json".  See compound-extension
     * check below. */

    if (strcmp(ext, ".scene") == 0)
        return JCE_FV_SCENE;

    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".cpp") == 0
        || strcmp(ext, ".h") == 0 || strcmp(ext, ".hpp") == 0
        || strcmp(ext, ".java") == 0 || strcmp(ext, ".kt") == 0
        || strcmp(ext, ".py") == 0 || strcmp(ext, ".lua") == 0
        || strcmp(ext, ".glsl") == 0 || strcmp(ext, ".hlsl") == 0
        || strcmp(ext, ".sc") == 0 || strcmp(ext, ".sh") == 0
        || strcmp(ext, ".vert") == 0 || strcmp(ext, ".frag") == 0
        || strcmp(ext, ".json") == 0 || strcmp(ext, ".xml") == 0
        || strcmp(ext, ".ini") == 0 || strcmp(ext, ".txt") == 0
        || strcmp(ext, ".md") == 0 || strcmp(ext, ".yaml") == 0
        || strcmp(ext, ".yml") == 0 || strcmp(ext, ".toml") == 0
        || strcmp(ext, ".cfg") == 0 || strcmp(ext, ".mat") == 0
        || strcmp(ext, ".jce") == 0 || strcmp(ext, ".csv") == 0
        || strcmp(ext, ".rs") == 0 || strcmp(ext, ".go") == 0
        || strcmp(ext, ".js") == 0 || strcmp(ext, ".ts") == 0
        || strcmp(ext, ".html") == 0 || strcmp(ext, ".css") == 0
        || strcmp(ext, ".bat") == 0 || strcmp(ext, ".ps1") == 0
        || strcmp(ext, ".gradle") == 0 || strcmp(ext, ".kts") == 0
        || strcmp(ext, ".properties") == 0)
        return JCE_FV_TEXT;

    if (strcmp(ext, ".wav") == 0 || strcmp(ext, ".ogg") == 0
        || strcmp(ext, ".mp3") == 0 || strcmp(ext, ".flac") == 0
        || strcmp(ext, ".opus") == 0 || strcmp(ext, ".oga") == 0)
        return JCE_FV_AUDIO;

    if (strcmp(ext, ".mp4") == 0 || strcmp(ext, ".m4v") == 0
        || strcmp(ext, ".webm") == 0 || strcmp(ext, ".mov") == 0
        || strcmp(ext, ".mkv") == 0 || strcmp(ext, ".avi") == 0
        || strcmp(ext, ".flv") == 0 || strcmp(ext, ".wmv") == 0
        || strcmp(ext, ".mpg") == 0 || strcmp(ext, ".mpeg") == 0
        || strcmp(ext, ".3gp") == 0 || strcmp(ext, ".ogv") == 0)
        return JCE_FV_VIDEO;

    return JCE_FV_BINARY;
}

static bool fv_looks_like_text(const char *data, int len)
{
    int check = (len > 8192) ? 8192 : len;
    for (int i = 0; i < check; i++)
        if (data[i] == '\0') return false;
    return true;
}

static void fv_open_info_tab(const char *open_path,
                             const char *name,
                             const char *ext,
                             long file_size,
                             const char *message)
{
    size_t msg_len;
    char *buf;
    FvTab *tab;

    if (!open_path || !name || !message) {
        return;
    }

    if (s_fv.tab_count >= FV_MAX_TABS)
        fv_close_tab(0);
    if (s_fv.tab_count >= FV_MAX_TABS)
        return;

    msg_len = strlen(message);
    buf = (char *)ED_MALLOC(msg_len + 1u);
    if (!buf) {
        return;
    }
    memcpy(buf, message, msg_len + 1u);

    tab = &s_fv.tabs[s_fv.tab_count];
    memset(tab, 0, sizeof(*tab));
    snprintf(tab->path, sizeof(tab->path), "%s", open_path);
    snprintf(tab->display_name, sizeof(tab->display_name), "%s", name);
    snprintf(tab->ext, sizeof(tab->ext), "%s", ext ? ext : "");
    tab->content = buf;
    tab->content_len = (int)msg_len;
    tab->file_size = file_size;
    tab->type = JCE_FV_TEXT;
    tab->open = true;
    tab->gpu_tex.idx = UINT16_MAX;
    tab->zoom = 0.0f;  /* 0 = fit on first render (zoomable helper auto-fits) */

    s_fv.active_tab = s_fv.tab_count;
    s_fv.tab_count++;
    s_fv.want_focus = true;
    *jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER) = true;
}

/* ══════════════════════════════════════════════════════════════════════
 *  PUBLIC API
 * ══════════════════════════════════════════════════════════════════════ */

JceFileViewerType jce_file_viewer_detect_type(const char *path)
{
    if (!path) return JCE_FV_BINARY;
    const char *dot = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '.') dot = p;
    if (!dot) return JCE_FV_BINARY;

    char ext[16] = {0};
    for (int i = 0; dot[i] && i < 15; i++)
        ext[i] = (char)tolower((unsigned char)dot[i]);
    return fv_detect_ext(ext);
}

void jce_file_viewer_open(const char *path)
{
    if (!path || !path[0]) return;

    std::string normalized = normalize_path_string(path);
    const char *open_path = normalized.empty() ? path : normalized.c_str();

    /* Already open -> switch to its tab. */
    for (int i = 0; i < s_fv.tab_count; i++) {
        std::string tab_norm = normalize_path_string(s_fv.tabs[i].path);
        const char *tab_path = tab_norm.empty() ? s_fv.tabs[i].path : tab_norm.c_str();
        if (strcmp(tab_path, open_path) == 0) {
            s_fv.select_tab_req = i;
            s_fv.want_focus = true;
            s_fv.request_autoplay = true;
            *jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER) = true;
            if (is_scene_file_path(open_path))
                jce_state_load_scene_file(open_path);
            return;
        }
    }

    /* Evict oldest if full. */
    if (s_fv.tab_count >= FV_MAX_TABS)
        fv_close_tab(0);

    /* Extract file name. */
    const char *name = open_path;
    for (const char *p = open_path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;

    /* Extract lowercase extension. */
    char ext[16] = {0};
    {
        const char *dot = NULL;
        for (const char *p = name; *p; p++)
            if (*p == '.') dot = p;
        if (dot) {
            int j = 0;
            for (const char *p = dot; *p && j < 15; p++, j++)
                ext[j] = (char)tolower((unsigned char)*p);
        }
    }

    /* Stat file size first so we can reject oversize previews before
     * any allocation; std::filesystem keeps this dependency-free. */
    long file_size = -1;
    {
        std::error_code ec;
        auto sz = std::filesystem::file_size(open_path, ec);
        if (ec) {
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "file viewer: cannot open '%s'", open_path);
            return;
        }
        file_size = (long)sz;
    }

    if (file_size < 0) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "file viewer: failed to read size for '%s'", open_path);
        return;
    }

    if (file_size > FV_MAX_ASSET_BYTES) {
        char info_msg[384];
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "file viewer: '%s' is too large (%ld bytes). Max single asset is %d MB.",
            open_path, file_size, FV_MAX_ASSET_BYTES / (1024 * 1024));

        snprintf(info_msg, sizeof(info_msg),
            "Preview unavailable for this file.\n\n"
            "Path: %s\n"
            "Size: %.2f MB\n"
            "Limit: %d MB\n\n"
            "The file exceeds the File Viewer preview size limit.",
            open_path,
            (double)file_size / (1024.0 * 1024.0),
            FV_MAX_ASSET_BYTES / (1024 * 1024));
        fv_open_info_tab(open_path, name, ext, file_size, info_msg);
        return;
    }

    JceFileViewerType ftype = fv_detect_ext(ext);

    int read_size;
    if (ftype == JCE_FV_IMAGE)
        read_size = (int)file_size;
    else if (ftype == JCE_FV_AUDIO)
        read_size = (int)file_size;
    else if (ftype == JCE_FV_VIDEO) {
        /* Video is decoded in-engine (jce_video) from the full byte
         * buffer.  Respect the shared FV_MAX_ASSET_BYTES (200 MB) cap
         * that also governs audio/model loads. */
        const long video_cap = FV_MAX_ASSET_BYTES;
        read_size = (file_size > video_cap) ? (int)video_cap
                                            : (int)file_size;
    }
    else if (ftype == JCE_FV_MODEL) {
        /* Keep model bytes up to the global per-asset cap so GLB files
         * can be inspected consistently in the model viewer. */
        const long model_cap = FV_MAX_ASSET_BYTES;
        read_size = (file_size > model_cap) ? (int)model_cap : (int)file_size;
    }
    else
        read_size = (file_size > FV_MAX_CONTENT) ? FV_MAX_CONTENT : (int)file_size;

    /* Read file (capped at FV_MAX_ASSET_BYTES already validated above
     * via stat helper before opening — see file_size check earlier). */
    size_t got = 0, total = 0;
    char *buf = (char *)ed_read_file_capped(open_path,
                                            (size_t)read_size,
                                            &got, &total);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "file viewer: cannot open '%s'", open_path);
        return;
    }
    int actually_read = (int)got;

    /* Refine type detection. */
    if (ftype == JCE_FV_TEXT) {
        std::string lower_path(open_path);
        for (char &ch : lower_path)
            ch = (char)tolower((unsigned char)ch);

        /* .mat.json compound extension → MATERIAL viewer. */
        size_t pl = lower_path.size();
        if (pl >= 9 && lower_path.substr(pl - 9) == ".mat.json")
            ftype = JCE_FV_MATERIAL;

        const char *sp = strstr(lower_path.c_str(), ".scene");
        if (sp && (strcmp(sp, ".scene") == 0 || strcmp(sp, ".scene.json") == 0))
            ftype = JCE_FV_SCENE;
    }
    if (ftype == JCE_FV_TEXT && !fv_looks_like_text(buf, actually_read))
        ftype = JCE_FV_BINARY;

    /* Fill tab. */
    FvTab *tab = &s_fv.tabs[s_fv.tab_count];
    memset(tab, 0, sizeof(*tab));
    snprintf(tab->path, sizeof(tab->path), "%s", open_path);
    snprintf(tab->display_name, sizeof(tab->display_name), "%s", name);
    snprintf(tab->ext, sizeof(tab->ext), "%s", ext);
    tab->content     = buf;
    tab->content_len = actually_read;
    tab->file_size   = file_size;
    tab->type        = ftype;
    tab->open        = true;
    tab->gpu_tex.idx = UINT16_MAX;
    tab->img_w       = 0;
    tab->img_h       = 0;
    tab->zoom        = 0.0f;  /* 0 = fit on first render (zoomable helper auto-fits) */
    tab->pan_x       = 0.0f;
    tab->pan_y       = 0.0f;
    tab->edit_mode   = false;
    tab->edit_buf    = NULL;
    tab->modified    = false;
    tab->find_buf[0] = '\0';
    tab->replace_buf[0] = '\0';
    tab->show_find_replace = false;
    tab->find_index  = 0;

    /* Load GPU texture for images. */
    if (ftype == JCE_FV_IMAGE && actually_read > 0) {
        JceImage img;
        bool decoded = false;

        /* Try generic LDR decode (PNG/JPG/BMP/TGA/GIF/PSD/PIC/PNM/HDR8). */
        if (jce_image_decode(buf, (size_t)actually_read, &img)) {
            decoded = true;
        }

        /* Float HDR fallback for .hdr (Radiance) — tone-map to RGBA8 for preview. */
        if (!decoded && ext && strcmp(ext, ".hdr") == 0) {
            int hdr_w = 0, hdr_h = 0;
            float *hdr_pixels = jce_image_load_hdr_from_memory(
                buf, (uint64_t)actually_read, &hdr_w, &hdr_h);
            if (hdr_pixels && hdr_w > 0 && hdr_h > 0
                && jce_image_create_blank((uint32_t)hdr_w, (uint32_t)hdr_h, &img)) {
                const float *src = hdr_pixels;
                uint8_t *dst = img.pixels;
                int npx = hdr_w * hdr_h;
                for (int px = 0; px < npx; px++) {
                    for (int ch = 0; ch < 3; ch++) {
                        float v = src[px * 4 + ch];
                        v = v / (v + 1.0f);              /* Reinhard */
                        v = powf(v, 1.0f / 2.2f);        /* gamma */
                        int iv = (int)(v * 255.0f + 0.5f);
                        if (iv > 255) iv = 255;
                        if (iv < 0) iv = 0;
                        dst[px * 4 + ch] = (uint8_t)iv;
                    }
                    dst[px * 4 + 3] = 255;
                }
                decoded = true;
            }
            if (hdr_pixels) jce_image_free_hdr(hdr_pixels);
        }

        if (decoded) {
            tab->img_w   = (int)img.width;
            tab->img_h   = (int)img.height;
            tab->gpu_tex = jce_texture_from_rgba(img.pixels,
                                                 img.width, img.height);
            jce_image_free(&img);

            /* Default zoom: fit in ~512px. */
            if (tab->img_w > 0 && tab->img_h > 0) {
                float max_dim = (float)((tab->img_w > tab->img_h)
                                        ? tab->img_w : tab->img_h);
                if (max_dim > 512.0f)
                    tab->zoom = 512.0f / max_dim;
            }

            LOG_INFO(LOG_TAG, "loaded image %dx%d tex=%u",
                     tab->img_w, tab->img_h, tab->gpu_tex.idx);
        } else {
            LOG_WARN(LOG_TAG, "image decode failed for '%s'", name);
        }

        /* Fallback for DDS/KTX/KTX2: bgfx natively decodes these
         * container formats via jce_texture_create_from_encoded(). */
        if (!jce_texture_valid(tab->gpu_tex) && actually_read > 0) {
            const JceGfxMemory *mem =
                jce_gfx_memory_copy(buf, (uint32_t)actually_read);
            if (mem) {
                JceTextureInfo info;
                memset(&info, 0, sizeof(info));
                JceTextureHandle h =
                    jce_texture_create_from_encoded(mem,
                                                    JCE_SAMPLER_U_CLAMP
                                                    | JCE_SAMPLER_V_CLAMP,
                                                    &info);
                if (h.idx != UINT16_MAX) {
                    tab->gpu_tex.idx = h.idx;
                    tab->img_w = (int)info.width;
                    tab->img_h = (int)info.height;

                    if (tab->img_w > 0 && tab->img_h > 0) {
                        float max_dim = (float)((tab->img_w > tab->img_h)
                                                ? tab->img_w : tab->img_h);
                        if (max_dim > 512.0f)
                            tab->zoom = 512.0f / max_dim;
                    }
                    LOG_INFO(LOG_TAG,
                        "loaded image (gfx container) %dx%d tex=%u",
                        tab->img_w, tab->img_h, tab->gpu_tex.idx);
                }
            }
        }
    }

    s_fv.active_tab = s_fv.tab_count;
    s_fv.tab_count++;
    s_fv.want_focus = true;

    *jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER) = true;

    if (ftype == JCE_FV_SCENE)
        jce_state_load_scene_file(open_path);

    LOG_INFO(LOG_TAG, "opened '%s' (type %d)", name, (int)ftype);
}

void jce_file_viewer_request_focus(void)
{
    s_fv.want_focus = true;
}

void jce_file_viewer_draw_content(void)
{
    const char *active_audio_path = NULL;
    const char *active_video_path = NULL;

    if (s_fv.want_focus) {
        ImGui::SetWindowFocus();
        s_fv.want_focus = false;
    }

    if (s_fv.tab_count <= 0) {
        float w = ImGui::GetContentRegionAvail().x;
        float h = ImGui::GetContentRegionAvail().y;
        const char *msg = jce_editor_i18n("viewer.noFile");
        ImVec2 tsz = ImGui::CalcTextSize(msg);
        ImGui::SetCursorPos(ImVec2((w - tsz.x) * 0.5f, h * 0.4f));
        ImGui::TextDisabled("%s", msg);
        const char *hint = jce_editor_i18n("viewer.hint");
        ImVec2 hsz = ImGui::CalcTextSize(hint);
        ImGui::SetCursorPosX((w - hsz.x) * 0.5f);
        ImGui::TextDisabled("%s", hint);
        fv_audio_update_focus(NULL, false);
        fv_video_update_focus(NULL, false);
        return;
    }

    ImGuiTabBarFlags bar_flags = ImGuiTabBarFlags_Reorderable
                               | ImGuiTabBarFlags_AutoSelectNewTabs
                               | ImGuiTabBarFlags_FittingPolicyScroll
                               | ImGuiTabBarFlags_TabListPopupButton;

    if (ImGui::BeginTabBar("##FvTabs", bar_flags)) {
        /* Mouse-wheel horizontal scroll over the tab strip. ImGui's tab bar
         * doesn't ship this by default; nudge ScrollingTarget directly. */
        if (ImGuiTabBar *wbar = ImGui::GetCurrentTabBar()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f
                && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
                && ImGui::IsMouseHoveringRect(wbar->BarRect.Min, wbar->BarRect.Max, false)) {
                const float step = ImGui::GetFontSize() * 4.0f;
                wbar->ScrollingTarget = ImClamp(
                    wbar->ScrollingTarget - wheel * step,
                    0.0f, ImMax(0.0f, wbar->WidthAllTabs - wbar->BarRect.GetWidth()));
                wbar->ScrollingTargetDistToVisibility = 0.0f;
            }
        }

        /* Programmatic tab selection: directly set the tab bar's selected ID
         * so the switch happens this frame rather than after 2-frame scheduling. */
        if (s_fv.select_tab_req >= 0 && s_fv.select_tab_req < s_fv.tab_count) {
            const int req = s_fv.select_tab_req;
            s_fv.select_tab_req = -1;
            ImGuiTabBar *bar = ImGui::GetCurrentTabBar();
            if (bar) {
                FvTab *rt = &s_fv.tabs[req];
                char req_title[80];
                if (rt->modified)
                    snprintf(req_title, sizeof(req_title), "%s *", rt->display_name);
                else
                    snprintf(req_title, sizeof(req_title), "%s", rt->display_name);
                ImGui::PushID(req);
                ImGuiID tid = ImGui::GetID(req_title);
                ImGui::PopID();
                /* Setting both ensures the switch happens this frame and persists. */
                bar->SelectedTabId     = tid;
                bar->NextSelectedTabId = tid;
                bar->NextScrollToTabId = tid;
            }
        }

        for (int i = 0; i < s_fv.tab_count; /* below */) {
            FvTab *tab = &s_fv.tabs[i];
            ImGui::PushID(i);

            /* Show modified indicator in tab title */
            char tab_title[80];
            if (tab->modified)
                snprintf(tab_title, sizeof(tab_title), "%s *", tab->display_name);
            else
                snprintf(tab_title, sizeof(tab_title), "%s", tab->display_name);

            bool tab_open = tab->open;
            bool tab_selected = ImGui::BeginTabItem(tab_title, &tab_open);

            /* Right-click context menu on tab (must be right after BeginTabItem) */
            if (ImGui::BeginPopupContextItem("##tabctx")) {
                if (ImGui::MenuItem(jce_editor_i18n("fileViewer.closeTab")))
                    tab_open = false;
                if (ImGui::MenuItem(jce_editor_i18n("fileViewer.closeOtherTabs"))) {
                    for (int j = s_fv.tab_count - 1; j >= 0; j--)
                        if (j != i) fv_close_tab(j);
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                if (ImGui::MenuItem(jce_editor_i18n("fileViewer.closeAllTabs"))) {
                    jce_file_viewer_close_all();
                    ImGui::EndPopup();
                    ImGui::PopID();
                    break;
                }
                ImGui::Separator();
                /* Copy file path */
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copyPath"))) {
                    ImGui::SetClipboardText(tab->path);
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
                    jce_host_reveal_path(tab->path);
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
                    /* Use parent dir for files; tab->path itself if a directory. */
                    if (jce_fs_host_exists_dir(tab->path)) {
                        jce_host_open_terminal(tab->path);
                    } else {
                        std::string p = tab->path;
                        size_t s = p.find_last_of("/\\");
                        jce_host_open_terminal(s == std::string::npos
                                               ? "."
                                               : p.substr(0, s).c_str());
                    }
                }
                ImGui::EndPopup();
            }

            if (tab_selected) {
                s_fv.active_tab = i;

                /* Dispatch to sub-viewer */
                switch (tab->type) {
                case JCE_FV_IMAGE:    fv_render_image(tab);    break;
                case JCE_FV_MODEL:    fv_render_model(tab);    break;
                case JCE_FV_SCENE:    fv_render_scene(tab);    break;
                case JCE_FV_MATERIAL: fv_render_material(tab); break;
                case JCE_FV_AUDIO:
                    active_audio_path = tab->path;
                    fv_render_audio(tab);
                    break;
                case JCE_FV_VIDEO:
                    active_video_path = tab->path;
                    fv_render_video(tab);
                    break;
                case JCE_FV_BINARY:   fv_render_hex(tab);      break;
                default:              fv_render_code(tab);      break;
                }
                ImGui::EndTabItem();
            }

            ImGui::PopID();

            if (!tab_open) {
                fv_close_tab(i);
            } else {
                i++;
            }
        }
        ImGui::EndTabBar();
    }

    /* A/V keeps playing as long as its tab is the active (visible) tab,
     * regardless of whether the File Viewer window itself is focused. */
    fv_audio_update_focus(active_audio_path, true);
    fv_video_update_focus(active_video_path, true);

    /* One-shot auto-play when the user re-opens an existing A/V tab. */
    if (s_fv.request_autoplay) {
        s_fv.request_autoplay = false;
        if (s_fv.active_tab >= 0 && s_fv.active_tab < s_fv.tab_count) {
            FvTab *at = &s_fv.tabs[s_fv.active_tab];
            if (at->type == JCE_FV_AUDIO)
                fv_audio_request_play(at->path);
            else if (at->type == JCE_FV_VIDEO)
                fv_video_request_play(at->path);
        }
    }
}

void jce_file_viewer_draw_window(bool *p_visible)
{
    if (!p_visible || !*p_visible) {
        fv_audio_update_focus(NULL, false);
        fv_video_update_focus(NULL, false);
        return;
    }

    if (s_fv.want_focus) {
        ImGui::SetNextWindowFocus();
        s_fv.want_focus = false;
    }

    char title[256];
    snprintf(title, sizeof(title), "%s###FileViewer", jce_editor_i18n("File Viewer"));
    if (ImGui::Begin(title, p_visible))
        jce_file_viewer_draw_content();
    ImGui::End();
}

void jce_file_viewer_close_all(void)
{
    for (int i = 0; i < s_fv.tab_count; i++) {
        FvTab *tab = &s_fv.tabs[i];
        if (tab->type == JCE_FV_MODEL)
            fv_model_close_tab(tab->path);
        if (tab->type == JCE_FV_AUDIO)
            fv_audio_close_tab(tab);
        if (tab->type == JCE_FV_VIDEO)
            fv_video_close_tab(tab);
        fv_code_close_tab(tab);
        if (jce_texture_valid(tab->gpu_tex))
            jce_texture_destroy(tab->gpu_tex);
        ED_FREE(tab->content);
        tab->content = NULL;
    }
    s_fv.tab_count      = 0;
    s_fv.active_tab     = -1;
    s_fv.select_tab_req = -1;
}

void jce_file_viewer_shutdown(void)
{
    jce_file_viewer_close_all();
    fv_model_shutdown();
}
