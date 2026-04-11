/*
 * jce_panel_file_viewer.cpp  Tab manager + dispatch for file viewer.
 *
 * Sub-viewers are in editor/src/viewers/:
 *   jce_fv_code.cpp   — Text/Code with syntax highlighting, find/replace, edit
 *   jce_fv_image.cpp  — Image with zoom, pan, checkered background
 *   jce_fv_model.cpp  — 3D model with wireframe, grid, rotation
 *   jce_fv_hex.cpp    — Hex dump + Scene viewer
 */

#include "jce_fv_common.h"
#include "jce_editor_state.h"
#include <imgui_internal.h>

#include <string>
#include <vector>
#include <filesystem>

extern "C" {
#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>
}

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
    fv_code_close_tab(tab);

    if (jce_texture_valid(tab->gpu_tex))
        jce_texture_destroy(tab->gpu_tex);

    free(tab->content);
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
        || strcmp(ext, ".ico") == 0 || strcmp(ext, ".psd") == 0)
        return JCE_FV_IMAGE;

    if (strcmp(ext, ".gltf") == 0 || strcmp(ext, ".glb") == 0
        || strcmp(ext, ".obj") == 0 || strcmp(ext, ".fbx") == 0
        || strcmp(ext, ".dae") == 0 || strcmp(ext, ".3ds") == 0)
        return JCE_FV_MODEL;

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

    return JCE_FV_BINARY;
}

static bool fv_looks_like_text(const char *data, int len)
{
    int check = (len > 8192) ? 8192 : len;
    for (int i = 0; i < check; i++)
        if (data[i] == '\0') return false;
    return true;
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

    JceFileViewerType ftype = fv_detect_ext(ext);

    int read_size;
    if (ftype == JCE_FV_IMAGE || ftype == JCE_FV_MODEL)
        read_size = (int)file_size;
    else
        read_size = (file_size > FV_MAX_CONTENT) ? FV_MAX_CONTENT : (int)file_size;

    char *buf = (char *)malloc((size_t)read_size + 1);
    if (!buf) { fclose(fp); return; }

    int actually_read = (int)fread(buf, 1, (size_t)read_size, fp);
    fclose(fp);
    buf[actually_read] = '\0';

    /* Refine type detection. */
    if (ftype == JCE_FV_TEXT) {
        std::string lower_path(open_path);
        for (char &ch : lower_path)
            ch = (char)tolower((unsigned char)ch);
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
                case JCE_FV_IMAGE:  fv_render_image(tab);  break;
                case JCE_FV_MODEL:  fv_render_model(tab);  break;
                case JCE_FV_SCENE:  fv_render_scene(tab);  break;
                case JCE_FV_BINARY: fv_render_hex(tab);    break;
                default:            fv_render_code(tab);   break;
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
}

void jce_file_viewer_draw_window(bool *p_visible)
{
    if (!p_visible || !*p_visible) return;

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
        fv_code_close_tab(tab);
        if (jce_texture_valid(tab->gpu_tex))
            jce_texture_destroy(tab->gpu_tex);
        free(tab->content);
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
