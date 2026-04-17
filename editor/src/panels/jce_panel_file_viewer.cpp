/*
 * jce_panel_file_viewer.cpp  Tab manager + dispatch for file viewer.
 *
 * Sub-viewers are in editor/src/viewers/:
 *   jce_fv_code.cpp   — Text/Code with syntax highlighting, find/replace, edit
 *   jce_fv_image.cpp  — Image with zoom, pan, checkered background
 *   jce_fv_model.cpp  — 3D model with wireframe, grid, rotation
 *   jce_fv_hex.cpp    — Hex dump + Scene viewer
 */

#include "viewers/jce_fv_common.h"
#include "jce_editor_state.h"
#include <imgui_internal.h>

#include <string>
#include <vector>
#include <filesystem>

extern "C" {
#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>
#include <jce/graphics/jce_pbr_material.h>
}

#include <bgfx/c99/bgfx.h>

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
        || strcmp(ext, ".mp3") == 0 || strcmp(ext, ".flac") == 0)
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
    tab->zoom = 1.0f;

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

    /* Read file. */
    FILE *fp = fopen(open_path, "rb");
    if (!fp) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "file viewer: cannot open '%s'", open_path);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(fp);
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "file viewer: failed to read size for '%s'", open_path);
        return;
    }

    if (file_size > FV_MAX_ASSET_BYTES) {
        char info_msg[384];
        fclose(fp);
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

    char *buf = (char *)ED_MALLOC((size_t)read_size + 1);
    if (!buf) { fclose(fp); return; }

    int actually_read = (int)fread(buf, 1, (size_t)read_size, fp);
    fclose(fp);
    buf[actually_read] = '\0';

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
    tab->zoom        = 1.0f;
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
        SDL_IOStream *io = SDL_IOFromConstMem(buf, (size_t)actually_read);
        if (io) {
            SDL_Surface *surf = IMG_Load_IO(io, true);
            /* Ensure RGBA32 — IMG_Load_IO may return RGB24 for images
               without alpha (e.g. normal maps).  texture_from_surface_ex()
               in jce_texture.c assumes 4 bytes/pixel, so feeding it an
               RGB24 surface causes a stride mismatch → colored stripes. */
            if (surf && surf->format != SDL_PIXELFORMAT_RGBA32) {
                SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
                SDL_DestroySurface(surf);
                surf = conv;
            }
            if (surf) {
                tab->img_w   = surf->w;
                tab->img_h   = surf->h;
                tab->gpu_tex = jce_texture_load_from_surface(surf, JCE_TEX_CLAMP);
                SDL_DestroySurface(surf);

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
                LOG_WARN(LOG_TAG, "IMG_Load_IO failed for '%s': %s",
                         name, SDL_GetError());
            }
        }

        /* Fallback for DDS/KTX/KTX2: bgfx natively decodes these
         * container formats via bgfx_create_texture(). */
        if (!jce_texture_valid(tab->gpu_tex) && actually_read > 0) {
            const bgfx_memory_t *mem =
                bgfx_copy(buf, (uint32_t)actually_read);
            if (mem) {
                bgfx_texture_info_t info;
                memset(&info, 0, sizeof(info));
                bgfx_texture_handle_t h =
                    bgfx_create_texture(mem, BGFX_TEXTURE_NONE
                                        | BGFX_SAMPLER_U_CLAMP
                                        | BGFX_SAMPLER_V_CLAMP,
                                        0, &info);
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
                        "loaded image (bgfx container) %dx%d tex=%u",
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
    const bool file_viewer_focused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
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
#ifdef _WIN32
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
                    char cmd[600];
                    snprintf(cmd, sizeof(cmd), "explorer /select,\"%s\"", tab->path);
                    system(cmd);
                }
#endif
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

    fv_audio_update_focus(active_audio_path, file_viewer_focused);
    fv_video_update_focus(active_video_path, file_viewer_focused);
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
