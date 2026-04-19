/*
 * jce_panel_assets.cpp  Asset Browser panel — state, public API, shortcuts, delete dialog.
 *
 * Navigation (tree, breadcrumb, search) is in jce_panel_assets_nav.cpp.
 * Grid rendering & context menus are in jce_panel_assets_grid.cpp.
 */

#include "jce_panel_assets_internal.h"

#include <chrono>
#include <ctime>

/* ── State instance (shared via extern in internal header) ───────── */

AssetBrowserState s_assets;

/* ── Shared helpers ──────────────────────────────────────────────── */

std::string normalized_path_string(const fs::path &p)
{
    try {
        fs::path canonical = fs::weakly_canonical(p);
        fs::path cwd       = fs::current_path();
        fs::path rel        = fs::relative(canonical, cwd);
        /* fs::relative returns "" on failure; fall back to canonical. */
        if (!rel.empty() && rel != ".")
            return rel.string();
        return canonical.string();
    } catch (...) {
        try {
            return fs::absolute(p).lexically_normal().string();
        } catch (...) {
            return p.string();
        }
    }
}

static std::string format_modified_time(const fs::directory_entry &de)
{
    try {
        const auto file_tp = de.last_write_time();
        const auto sys_tp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            file_tp - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t raw_time = std::chrono::system_clock::to_time_t(sys_tp);

        std::tm local_tm = {};
#ifdef _WIN32
        if (localtime_s(&local_tm, &raw_time) != 0)
            return "";
#else
        if (!localtime_r(&raw_time, &local_tm))
            return "";
#endif

        char buf[32] = {0};
        if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local_tm) == 0)
            return "";
        return std::string(buf);
    } catch (...) {
        return "";
    }
}

void ensure_assets_init(void)
{
    if (s_assets.initialized) return;
    s_assets.project_root      = ".";
    s_assets.current_path      = s_assets.project_root;
    s_assets.last_clicked_idx  = -1;
    s_assets.renaming_idx      = -1;
    s_assets.rename_focus_needed = false;
    s_assets.suppress_empty_ctx_frames = 0;
    s_assets.context_idx       = -1;
    s_assets.needs_refresh     = true;
    s_assets.clipboard_cut     = false;
    s_assets.show_delete_confirm = false;
    s_assets.show_delete_dialog_open = false;
    s_assets.pending_delete_dir_count = 0;
    s_assets.pending_navigation_path.clear();
    s_assets.pending_navigation_clear_search = false;
    s_assets.next_auto_refresh_time = 0.0;
    s_assets.search_buf[0] = '\0';
    s_assets.search_active = false;
    s_assets.view_mode = ASSET_BROWSER_VIEW_DETAILS;
    s_assets.initialized       = true;
}

void refresh_entries(void)
{
    s_assets.entries.clear();
    try {
        for (auto &de : fs::directory_iterator(s_assets.current_path)) {
            FileEntry fe;
            fe.name   = de.path().filename().string();
            fe.path   = normalized_path_string(de.path());
            fe.is_dir = de.is_directory();
            fe.modified_at = format_modified_time(de);
            if (!fe.is_dir) {
                std::string ext = de.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return (char)std::tolower(c); });
                fe.ext  = ext;
                try { fe.size = de.file_size(); }
                catch (...) { fe.size = 0; }
            } else {
                fe.ext  = "";
                fe.size = 0;
            }
            s_assets.entries.push_back(std::move(fe));
        }
    } catch (const std::exception &e) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Asset browser: failed to read directory '%s': %s",
            s_assets.current_path.c_str(), e.what());
    }

    std::sort(s_assets.entries.begin(), s_assets.entries.end(),
              [](const FileEntry &a, const FileEntry &b) {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  return a.name < b.name;
              });

    /* Rebuild search results after any folder refresh. */
    s_assets.last_search_query.clear();
    s_assets.needs_refresh = false;
}

static bool directory_entries_changed(void)
{
    std::vector<std::pair<std::string, bool>> disk_entries;
    try {
        for (auto &de : fs::directory_iterator(
                 s_assets.current_path,
                 fs::directory_options::skip_permission_denied)) {
            disk_entries.emplace_back(
                de.path().filename().string(),
                de.is_directory());
        }
    } catch (...) {
        return false;
    }

    std::sort(disk_entries.begin(), disk_entries.end(),
              [](const std::pair<std::string, bool> &a,
                 const std::pair<std::string, bool> &b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });

    if (disk_entries.size() != s_assets.entries.size())
        return true;

    for (size_t i = 0; i < disk_entries.size(); i++) {
        if (disk_entries[i].first != s_assets.entries[i].name
            || disk_entries[i].second != s_assets.entries[i].is_dir)
            return true;
    }

    return false;
}

void navigate_asset_directory(const std::string &path, bool clear_search)
{
    if (path.empty()) return;

    /* Defer navigation application to panel frame boundary so we never
       rebuild entry vectors while the grid is still iterating them. */
    s_assets.pending_navigation_path = normalized_path_string(fs::path(path));
    s_assets.pending_navigation_clear_search = clear_search;
    s_assets.next_auto_refresh_time = ImGui::GetTime() + 0.35;
}

void collect_search_results(const std::string &query)
{
    s_assets.search_results.clear();
    if (query.empty()) return;

    std::string query_lower = query;
    for (auto &c : query_lower) c = (char)std::tolower((unsigned char)c);

    try {
        for (auto &de : fs::recursive_directory_iterator(
                 s_assets.current_path,
                 fs::directory_options::skip_permission_denied)) {
            FileEntry fe;
            fe.name   = de.path().filename().string();
            fe.is_dir = de.is_directory();
            fe.modified_at = format_modified_time(de);

            std::string name_lower = fe.name;
            for (auto &c : name_lower) c = (char)std::tolower((unsigned char)c);
            if (name_lower.find(query_lower) == std::string::npos)
                continue;

            fe.path = normalized_path_string(de.path());
            if (!fe.is_dir) {
                std::string ext = de.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c_) { return (char)std::tolower(c_); });
                fe.ext  = ext;
                try { fe.size = de.file_size(); }
                catch (...) { fe.size = 0; }
            } else {
                fe.ext  = "";
                fe.size = 0;
            }
            s_assets.search_results.push_back(std::move(fe));

            if (s_assets.search_results.size() >= 500) break;
        }
    } catch (...) {}

    std::sort(s_assets.search_results.begin(), s_assets.search_results.end(),
              [](const FileEntry &a, const FileEntry &b) {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  return a.name < b.name;
              });

    s_assets.last_search_query = query;
    s_assets.last_search_root  = s_assets.current_path;
}

ImVec4 asset_color_for_ext(const std::string &ext, bool is_dir)
{
    if (is_dir) return JCE_COLOR_ASSET_FOLDER;
    if (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".hpp"
        || ext == ".java" || ext == ".kt" || ext == ".lua" || ext == ".py")
        return JCE_COLOR_ASSET_CODE;
    if (ext == ".scene") return JCE_COLOR_ASSET_SCENE;
    if (ext == ".json" || ext == ".xml" || ext == ".ini" || ext == ".mat")
        return JCE_COLOR_ASSET_DATA;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp"
        || ext == ".tga" || ext == ".hdr")
        return JCE_COLOR_ASSET_IMAGE;
    return JCE_COLOR_ASSET_DEFAULT;
}

const char *type_label_for_entry(const FileEntry &fe)
{
    if (fe.is_dir) return "/";
    if (fe.ext == ".c" || fe.ext == ".cpp" || fe.ext == ".h" || fe.ext == ".hpp"
        || fe.ext == ".java" || fe.ext == ".kt" || fe.ext == ".lua" || fe.ext == ".py")
        return "CODE";
    if (fe.ext == ".scene") return "SCENE";
    if (fe.ext == ".json" || fe.ext == ".xml" || fe.ext == ".ini" || fe.ext == ".mat")
        return "DATA";
    if (fe.ext == ".png" || fe.ext == ".jpg" || fe.ext == ".jpeg" || fe.ext == ".bmp"
        || fe.ext == ".tga" || fe.ext == ".hdr")
        return "IMG";
    if (fe.ext == ".wav" || fe.ext == ".ogg" || fe.ext == ".mp3")
        return "SND";
    if (fe.ext == ".obj" || fe.ext == ".fbx" || fe.ext == ".gltf" || fe.ext == ".glb")
        return "3D";
    if (!fe.ext.empty()) return fe.ext.c_str() + 1;
    return "FILE";
}

void copy_selection_to_clipboard(bool cut)
{
    s_assets.clipboard_paths.clear();
    for (int si : s_assets.selected_set)
        if (si >= 0 && si < (int)s_assets.entries.size())
            s_assets.clipboard_paths.push_back(s_assets.entries[si].path);
    s_assets.clipboard_cut = cut;
}

void execute_clipboard_paste(void)
{
    try {
        for (auto &cp : s_assets.clipboard_paths) {
            fs::path src(cp);
            fs::path dst = fs::path(s_assets.current_path) / src.filename();
            if (s_assets.clipboard_cut) {
                fs::rename(src, dst);
                jce_editor_console_log("Moved '%s'",
                    src.filename().string().c_str());
            } else {
                fs::copy(src, dst, fs::copy_options::recursive);
                jce_editor_console_log("Pasted '%s'",
                    src.filename().string().c_str());
            }
        }
        if (s_assets.clipboard_cut)
            s_assets.clipboard_paths.clear();
        s_assets.needs_refresh = true;
    } catch (const std::exception &e) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Paste failed: %s", e.what());
    }
}

void collect_selected_for_deletion(void)
{
    s_assets.pending_delete_paths.clear();
    s_assets.pending_delete_names.clear();
    s_assets.pending_delete_dir_count = 0;
    for (int si : s_assets.selected_set) {
        if (si >= 0 && si < (int)s_assets.entries.size()) {
            s_assets.pending_delete_paths.push_back(s_assets.entries[si].path);
            s_assets.pending_delete_names.push_back(s_assets.entries[si].name);
            if (s_assets.entries[si].is_dir)
                s_assets.pending_delete_dir_count++;
        }
    }
    s_assets.show_delete_confirm = true;
}

/* ── Public API ───────────────────────────────────────────────────── */

void jce_editor_assets_set_project(const char *path)
{
    ensure_assets_init();
    if (!path || !path[0]) return;
    std::string normalized = normalized_path_string(fs::path(path));
    if (s_assets.project_root == normalized) return;
    s_assets.project_root  = normalized;
    navigate_asset_directory(normalized, true);
}

bool jce_editor_assets_delete_dialog_open(void)
{
    return s_assets.show_delete_dialog_open;
}

/* ── Keyboard shortcuts ──────────────────────────────────────────── */

static void handle_asset_keyboard_shortcuts(void)
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && s_assets.renaming_idx < 0
        && !ImGui::GetIO().WantTextInput)
    {
        bool has_sel = !s_assets.selected_set.empty();
        bool ctrl    = ImGui::GetIO().KeyCtrl;

        if (has_sel && s_assets.selected_set.size() == 1
            && ImGui::IsKeyPressed(ImGuiKey_F2))
        {
            int sel = *s_assets.selected_set.begin();
            s_assets.renaming_idx = sel;
            s_assets.rename_focus_needed = true;
            snprintf(s_assets.rename_buf, sizeof(s_assets.rename_buf),
                    "%s", s_assets.entries[sel].name.c_str());
        }

        if (has_sel && ImGui::IsKeyPressed(ImGuiKey_Delete))
            collect_selected_for_deletion();

        if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_C))
            copy_selection_to_clipboard(false);

        if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_X))
            copy_selection_to_clipboard(true);

        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)
            && !s_assets.clipboard_paths.empty())
        {
            execute_clipboard_paste();
        }

        if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
            try {
                for (int si : s_assets.selected_set) {
                    if (si < 0 || si >= (int)s_assets.entries.size()) continue;
                    const FileEntry &fe = s_assets.entries[si];
                    fs::path src(fe.path);
                    fs::path dst;
                    if (fe.is_dir) {
                        dst = src.parent_path() / (fe.name + "_copy");
                    } else {
                        std::string stem = src.stem().string();
                        std::string ext  = src.extension().string();
                        dst = src.parent_path() / (stem + "_copy" + ext);
                    }
                    fs::copy(src, dst, fs::copy_options::recursive);
                    jce_editor_console_log("Duplicated '%s'", fe.name.c_str());
                }
                s_assets.needs_refresh = true;
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Duplicate failed: %s", e.what());
            }
        }

        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            s_assets.selected_set.clear();
            for (int si = 0; si < (int)s_assets.entries.size(); si++)
                s_assets.selected_set.insert(si);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_assets.selected_set.clear();
            s_assets.last_clicked_idx = -1;
        }
    }
}

/* ── Delete confirmation dialog ──────────────────────────────────── */

static void draw_asset_delete_dialog(void)
{
    if (s_assets.show_delete_confirm) {
        s_assets.show_delete_dialog_open = true;
        s_assets.show_delete_confirm = false;
    }
    if (s_assets.show_delete_dialog_open) {
        const char *popup_id = "###AssetDeleteConfirm";
        if (s_assets.show_delete_dialog_open && !ImGui::IsPopupOpen(popup_id))
            ImGui::OpenPopup(popup_id);

        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowViewport(vp->ID);

        char title[256];
        snprintf(title, sizeof(title), "%s%s", jce_editor_i18n("dialog.confirmDelete"), popup_id);
        if (ImGui::BeginPopupModal(title,
                    &s_assets.show_delete_dialog_open,
                    ImGuiWindowFlags_NoCollapse
                  | ImGuiWindowFlags_NoDocking
                  | ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s", jce_editor_i18n("assetBrowser.deleteDialogPrompt"));
            if (s_assets.pending_delete_names.size() > 1)
                ImGui::Text("(%d %s)", (int)s_assets.pending_delete_names.size(),
                            jce_editor_i18n("assetBrowser.items"));
            ImGui::Spacing();
            for (auto &name : s_assets.pending_delete_names)
                ImGui::TextWrapped("  %s", name.c_str());
            if (s_assets.pending_delete_dir_count > 0) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_WARNING);
                if (s_assets.pending_delete_dir_count == 1)
                    ImGui::TextWrapped("%s", jce_editor_i18n("assetBrowser.deleteFolderWarningSingle"));
                else
                    ImGui::TextWrapped("%s: %d",
                                       jce_editor_i18n("assetBrowser.deleteFolderWarningMulti"),
                                       s_assets.pending_delete_dir_count);
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const float btn_w = 120.0f;
            const float btn_gap = ImGui::GetStyle().ItemSpacing.x;
            const float total_btn_w = btn_w * 2.0f + btn_gap;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total_btn_w);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.3f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button(jce_editor_i18n("assetBrowser.deleteConfirm"), ImVec2(btn_w, 0))) {
                try {
                    for (size_t di = 0; di < s_assets.pending_delete_paths.size(); di++) {
                        fs::remove_all(s_assets.pending_delete_paths[di]);
                        jce_editor_console_log("Deleted '%s'",
                            s_assets.pending_delete_names[di].c_str());
                    }
                    s_assets.needs_refresh = true;
                    s_assets.selected_set.clear();
                    s_assets.last_clicked_idx = -1;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Delete failed: %s", e.what());
                }
                ImGui::CloseCurrentPopup();
                s_assets.show_delete_dialog_open = false;
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
                ImGui::CloseCurrentPopup();
                s_assets.show_delete_dialog_open = false;
            }
            ImGui::EndPopup();
        }
    }
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_assets_content(void)
{
    ensure_assets_init();

    if (!s_assets.pending_navigation_path.empty()) {
        s_assets.current_path = s_assets.pending_navigation_path;
        s_assets.pending_navigation_path.clear();

        s_assets.selected_set.clear();
        s_assets.last_clicked_idx = -1;
        s_assets.context_idx = -1;

        if (s_assets.pending_navigation_clear_search) {
            s_assets.search_buf[0] = '\0';
            s_assets.search_active = false;
            s_assets.search_results.clear();
            s_assets.last_search_query.clear();
            s_assets.last_search_root.clear();
        }
        s_assets.pending_navigation_clear_search = false;

        s_assets.needs_refresh = true;
    }

    double now = ImGui::GetTime();
    if (now >= s_assets.next_auto_refresh_time) {
        s_assets.next_auto_refresh_time = now + 0.35;
        if (!s_assets.needs_refresh && directory_entries_changed())
            s_assets.needs_refresh = true;
    }

    if (s_assets.needs_refresh)
        refresh_entries();

    float tree_w = ImGui::GetContentRegionAvail().x * JCE_ASSET_TREE_WIDTH_RATIO;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    draw_asset_directory_tree(tree_w, avail.y);

    ImGui::SameLine();

    ImGui::BeginChild("FileArea", ImVec2(0, avail.y), false);
    {
        draw_asset_breadcrumb_bar();
        draw_asset_search_bar();

        {
            const std::vector<FileEntry> &display_entries =
                s_assets.search_active ? s_assets.search_results : s_assets.entries;
            bool want_ctx_popup = false;

            if (s_assets.view_mode == ASSET_BROWSER_VIEW_GRID) {
                float cell_size = JCE_THUMBNAIL_SIZE + JCE_ASSET_CELL_PADDING * 2;
                float panel_w   = ImGui::GetContentRegionAvail().x;
                int cols = (int)(panel_w / cell_size);
                if (cols < 1) cols = 1;

                int col = 0;
                for (int i = 0; i < (int)display_entries.size(); i++)
                    draw_asset_grid_item(display_entries[i], i, cols, col, want_ctx_popup);
            } else {
                draw_asset_details_list(display_entries, want_ctx_popup);
            }

            if (want_ctx_popup)
                ImGui::OpenPopup("AssetContextMenu");

            draw_asset_item_context_menu(display_entries);
            handle_asset_keyboard_shortcuts();
            draw_asset_empty_area_menu();
        }

        draw_asset_delete_dialog();
    }
    ImGui::EndChild();
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_assets(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###AssetBrowser", jce_editor_i18n("assetBrowser.title"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_assets_content();
    ImGui::End();
}
