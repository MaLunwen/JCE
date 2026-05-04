/*
 * jce_panel_assets.cpp  Asset Browser panel — state, public API, shortcuts, delete dialog.
 *
 * Navigation (tree, breadcrumb, search) is in jce_panel_assets_nav.cpp.
 * Grid rendering & context menus are in jce_panel_assets_grid.cpp.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_timer.h>

#include "core/jce_editor_config.h"
#include "core/jce_hotkeys.h"
#include "jce_panel_assets_internal.h"
#include "scene/jce_asset_path_index.h"

/* ── State instance (shared via extern in internal header) ───────── */

AssetBrowserState s_assets;

/* ── Shared helpers ──────────────────────────────────────────────── */

std::string normalized_path_string(const std::string &p)
{
    if (p.empty()) return p;

    /* Identity form is always absolute + canonical so that path
       comparisons (breadcrumb, parent navigation, dir tree selection)
       are unambiguous regardless of process CWD changes.  Display
       layers can compute a relative form on demand. */
    char abs_buf[1024];
    if (jce_path_is_absolute(p.c_str())) {
        if (!jce_path_normalize(abs_buf, sizeof(abs_buf), p.c_str()))
            return p;
    } else {
        char cwd_buf[1024];
        if (!jce_fs_host_get_current_dir(cwd_buf, sizeof(cwd_buf)))
            return p;
        char joined[1024];
        if (!jce_path_join(joined, sizeof(joined), cwd_buf, p.c_str()))
            return p;
        if (!jce_path_normalize(abs_buf, sizeof(abs_buf), joined))
            return p;
    }
    jce_path_to_canonical(abs_buf, sizeof(abs_buf), abs_buf);
    return std::string(abs_buf);
}

static std::string format_modified_time(const char *path)
{
    int64_t epoch = 0;
    if (!jce_fs_host_get_mtime(path, &epoch))
        return "";

    char buf[32];
    if (jce_time_format_local(epoch, "%Y-%m-%d %H:%M",
                              buf, sizeof(buf)) == 0)
        return "";
    return std::string(buf);
}

void ensure_assets_init(void)
{
    if (s_assets.initialized) return;
    /* Use the launch CWD as the initial project root.  Anything else
       (e.g. ".") becomes ambiguous once the editor changes process cwd
       or is launched from a build folder. */
    char cwd_buf[1024];
    if (jce_fs_host_get_current_dir(cwd_buf, sizeof(cwd_buf)))
        s_assets.project_root = cwd_buf;
    else
        s_assets.project_root = ".";
    s_assets.current_path      = s_assets.project_root;
    s_assets.last_clicked_idx  = -1;
    s_assets.renaming_idx      = -1;
    s_assets.rename_focus_needed = false;
    s_assets.suppress_empty_ctx_frames = 0;
    s_assets.context_idx       = -1;
    s_assets.needs_refresh     = true;
    s_assets.clipboard_cut     = false;
    s_assets.clipboard_flash_t = 0.0f;
    s_assets.paste_flash_t     = 0.0f;
    s_assets.show_delete_confirm = false;
    s_assets.show_delete_dialog_open = false;
    s_assets.pending_delete_dir_count = 0;
    s_assets.pending_navigation_path.clear();
    s_assets.pending_navigation_clear_search = false;
    s_assets.next_auto_refresh_time = 0.0;
    s_assets.search_buf[0] = '\0';
    s_assets.search_active = false;
    s_assets.view_mode = ASSET_BROWSER_VIEW_GRID;
    s_assets.kind_filter = 0;
    {
        JceEditorConfig ecfg;
        if (jce_editor_config_load(&ecfg)) {
            int vm = ecfg.asset_browser_view_mode;
            if (vm == ASSET_BROWSER_VIEW_GRID || vm == ASSET_BROWSER_VIEW_DETAILS)
                s_assets.view_mode = (AssetBrowserViewMode)vm;
        }
    }
    s_assets.initialized       = true;

    /* Build initial asset path index over the launch CWD project root
     * so even sessions that never call set_project() can resolve asset
     * references.  Subsequent set_project() calls rebuild as needed. */
    if (!s_assets.project_root.empty()) {
        jce_asset_path_index_clear();
        jce_asset_path_index_rebuild(s_assets.project_root.c_str());
    }
}

void refresh_entries(void)
{
    s_assets.entries.clear();
    
    struct ListCtx {
        std::vector<FileEntry> *entries;
        std::string current_path;
    } ctx;
    ctx.entries = &s_assets.entries;
    ctx.current_path = s_assets.current_path;
    
    auto cb = [](const char *name, bool is_dir, void *ud) -> bool {
        ListCtx *c = static_cast<ListCtx*>(ud);
        
        FileEntry fe;
        fe.name = name;
        
        char full[1024];
        jce_path_join(full, sizeof(full), c->current_path.c_str(), name);
        fe.path = normalized_path_string(full);
        fe.is_dir = is_dir;
        fe.modified_at = format_modified_time(full);
        
        if (!fe.is_dir) {
            char ext_buf[64];
            jce_path_extension(ext_buf, sizeof(ext_buf), name);
            std::string ext = ext_buf;
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            fe.ext = ext;
            
            uint64_t sz = 0;
            jce_fs_host_get_size(full, &sz);
            fe.size = sz;
        } else {
            fe.ext  = "";
            fe.size = 0;
        }
        
        c->entries->push_back(std::move(fe));
        return true;
    };
    
    if (!jce_fs_host_list_dir(s_assets.current_path.c_str(), cb, &ctx)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Asset browser: failed to read directory '%s'",
            s_assets.current_path.c_str());
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
    
    struct ListCtx {
        std::vector<std::pair<std::string, bool>> *entries;
    } ctx;
    ctx.entries = &disk_entries;
    
    auto cb = [](const char *name, bool is_dir, void *ud) -> bool {
        ListCtx *c = static_cast<ListCtx*>(ud);
        c->entries->emplace_back(name, is_dir);
        return true;
    };
    
    if (!jce_fs_host_list_dir(s_assets.current_path.c_str(), cb, &ctx))
        return false;

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
    s_assets.pending_navigation_path = normalized_path_string(path);
    s_assets.pending_navigation_clear_search = clear_search;
    s_assets.next_auto_refresh_time = ImGui::GetTime() + 0.35;
}

void collect_search_results(const std::string &query)
{
    s_assets.search_results.clear();
    if (query.empty()) return;

    std::string query_lower = query;
    for (auto &c : query_lower) c = (char)std::tolower((unsigned char)c);

    struct WalkCtx {
        std::string query_lower;
        std::vector<FileEntry> *results;
    } ctx;
    ctx.query_lower = query_lower;
    ctx.results = &s_assets.search_results;
    
    auto cb = [](const char *path, bool is_dir, void *ud) -> bool {
        WalkCtx *c = static_cast<WalkCtx*>(ud);
        
        if (c->results->size() >= 500)
            return false;
        
        char basename[256];
        jce_path_basename(basename, sizeof(basename), path);
        
        std::string name = basename;
        std::string name_lower = name;
        for (auto &ch : name_lower) ch = (char)std::tolower((unsigned char)ch);
        
        if (name_lower.find(c->query_lower) == std::string::npos)
            return true;
        
        FileEntry fe;
        fe.name = name;
        fe.is_dir = is_dir;
        fe.modified_at = format_modified_time(path);
        fe.path = normalized_path_string(path);
        
        if (!fe.is_dir) {
            char ext_buf[64];
            jce_path_extension(ext_buf, sizeof(ext_buf), path);
            std::string ext = ext_buf;
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c_) { return (char)std::tolower(c_); });
            fe.ext = ext;
            
            uint64_t sz = 0;
            jce_fs_host_get_size(path, &sz);
            fe.size = sz;
        } else {
            fe.ext  = "";
            fe.size = 0;
        }
        
        c->results->push_back(std::move(fe));
        return true;
    };
    
    jce_fs_host_walk(s_assets.current_path.c_str(), cb, &ctx);

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
    if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx"
        || ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".inl"
        || ext == ".java" || ext == ".kt" || ext == ".lua" || ext == ".py"
        || ext == ".js" || ext == ".ts" || ext == ".rs" || ext == ".go"
        || ext == ".cs" || ext == ".swift" || ext == ".m" || ext == ".mm")
        return JCE_COLOR_ASSET_CODE;
    if (ext == ".scene") return JCE_COLOR_ASSET_SCENE;
    if (ext == ".prefab" || ext == ".asset") return JCE_COLOR_ASSET_PREFAB;
    if (ext == ".json" || ext == ".xml" || ext == ".ini" || ext == ".yaml"
        || ext == ".yml" || ext == ".toml" || ext == ".csv")
        return JCE_COLOR_ASSET_DATA;
    if (ext == ".mat") return JCE_COLOR_ASSET_MATERIAL;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".geom"
        || ext == ".tesc" || ext == ".tese" || ext == ".glsl" || ext == ".hlsl"
        || ext == ".sc" || ext == ".sh" || ext == ".bin")
        return JCE_COLOR_ASSET_SHADER;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".jfif"
        || ext == ".bmp" || ext == ".tga" || ext == ".hdr" || ext == ".gif"
        || ext == ".webp" || ext == ".tif" || ext == ".tiff" || ext == ".dds"
        || ext == ".ktx" || ext == ".ktx2" || ext == ".exr" || ext == ".psd"
        || ext == ".svg" || ext == ".ico")
        return JCE_COLOR_ASSET_IMAGE;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac"
        || ext == ".opus" || ext == ".aac" || ext == ".m4a" || ext == ".aiff"
        || ext == ".wma" || ext == ".mid" || ext == ".midi")
        return JCE_COLOR_ASSET_AUDIO;
    if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi"
        || ext == ".mov" || ext == ".wmv" || ext == ".flv" || ext == ".m4v"
        || ext == ".mpg" || ext == ".mpeg" || ext == ".ts")
        return JCE_COLOR_ASSET_VIDEO;
    if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".glb"
        || ext == ".dae" || ext == ".3ds" || ext == ".blend" || ext == ".stl"
        || ext == ".ply")
        return JCE_COLOR_ASSET_MODEL;
    if (ext == ".anim" || ext == ".ozz" || ext == ".bvh")
        return JCE_COLOR_ASSET_ANIM;
    if (ext == ".ttf" || ext == ".otf" || ext == ".woff" || ext == ".woff2"
        || ext == ".fnt")
        return JCE_COLOR_ASSET_FONT;
    if (ext == ".zip" || ext == ".7z" || ext == ".tar" || ext == ".gz"
        || ext == ".bz2" || ext == ".xz" || ext == ".rar" || ext == ".pak")
        return JCE_COLOR_ASSET_ARCHIVE;
    if (ext == ".md" || ext == ".txt" || ext == ".rst" || ext == ".pdf"
        || ext == ".doc" || ext == ".docx" || ext == ".rtf" || ext == ".log")
        return JCE_COLOR_ASSET_DOC;
    return JCE_COLOR_ASSET_DEFAULT;
}

const char *type_label_for_entry(const FileEntry &fe)
{
    if (fe.is_dir) return "/";
    if (!fe.ext.empty()) return fe.ext.c_str() + 1;
    return "FILE";
}

void copy_selection_to_clipboard(bool cut)
{
    copy_selection_from_view_to_clipboard(s_assets.entries, cut);
}

void copy_selection_from_view_to_clipboard(
    const std::vector<FileEntry> &view, bool cut)
{
    s_assets.clipboard_paths.clear();
    for (int si : s_assets.selected_set)
        if (si >= 0 && si < (int)view.size())
            s_assets.clipboard_paths.push_back(view[si].path);
    s_assets.clipboard_cut = cut;
    s_assets.clipboard_flash_t = 0.6f;
    if (!s_assets.clipboard_paths.empty()) {
        for (auto &p : s_assets.clipboard_paths)
            s_assets.entry_flash[p] = 0.6f;
        jce_editor_console_log("%s %d item(s)",
            cut ? "Cut" : "Copied", (int)s_assets.clipboard_paths.size());
    }
}

void copy_path_to_clipboard(const std::string &path, bool cut)
{
    if (path.empty()) return;
    s_assets.clipboard_paths.clear();
    s_assets.clipboard_paths.push_back(path);
    s_assets.clipboard_cut = cut;
    s_assets.clipboard_flash_t = 0.6f;
    
    char basename[256];
    jce_path_basename(basename, sizeof(basename), path.c_str());
    
    jce_editor_console_log("%s '%s'",
        cut ? "Cut" : "Copied",
        basename);
}

void execute_clipboard_paste(void)
{
    int n = 0;
    for (auto &cp : s_assets.clipboard_paths) {
        std::string fname;
        {
            /* Extract basename from cp via plain string ops. */
            size_t s = cp.find_last_of("/\\");
            fname = (s == std::string::npos) ? cp : cp.substr(s + 1);
        }
        std::string desired = s_assets.current_path + "/" + fname;

        if (s_assets.clipboard_cut) {
            if (desired == cp) continue; /* same folder no-op */
            if (!jce_fs_host_rename(cp.c_str(), desired.c_str())) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Move failed: '%s'", fname.c_str());
                continue;
            }
            jce_editor_console_log("Moved '%s'", fname.c_str());
            s_assets.entry_flash[desired] = 0.6f;
        } else {
            char unique[1200];
            const char *dst_path = desired.c_str();
            if (jce_fs_host_exists_file(desired.c_str())
                || jce_fs_host_exists_dir(desired.c_str()))
            {
                if (jce_fs_host_make_unique_path(desired.c_str(),
                                                 unique, sizeof(unique)))
                {
                    dst_path = unique;
                }
            }
            if (!jce_fs_host_copy_recursive(cp.c_str(), dst_path)) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Copy failed: '%s'", fname.c_str());
                continue;
            }
            jce_editor_console_log("Pasted '%s' -> '%s'",
                fname.c_str(), dst_path);
            s_assets.entry_flash[dst_path] = 0.6f;
        }
        ++n;
    }
    if (s_assets.clipboard_cut)
        s_assets.clipboard_paths.clear();
    s_assets.needs_refresh = true;
    if (n > 0) s_assets.paste_flash_t = 0.6f;
}

void collect_selected_for_deletion(void)
{
    collect_selected_from_view_for_deletion(s_assets.entries);
}

void collect_selected_from_view_for_deletion(
    const std::vector<FileEntry> &view)
{
    s_assets.pending_delete_paths.clear();
    s_assets.pending_delete_names.clear();
    s_assets.pending_delete_dir_count = 0;
    for (int si : s_assets.selected_set) {
        if (si >= 0 && si < (int)view.size()) {
            s_assets.pending_delete_paths.push_back(view[si].path);
            s_assets.pending_delete_names.push_back(view[si].name);
            if (view[si].is_dir)
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
    std::string normalized = normalized_path_string(path);
    if (s_assets.project_root == normalized) return;
    s_assets.project_root  = normalized;
    navigate_asset_directory(normalized, true);

    /* Rebuild the project-wide asset path index so resolvers can do
     * O(1) basename lookups instead of recursive filesystem walks.
     * Mirrors Unity's import-time GUID/path table at a coarser
     * granularity (basename only).  Rebuild is one-shot per project
     * switch — fast even on large packs (~10k files). */
    jce_asset_path_index_clear();
    jce_asset_path_index_rebuild(normalized.c_str());
}

const char *jce_editor_assets_get_project(void)
{
    ensure_assets_init();
    return s_assets.project_root.c_str();
}

bool jce_editor_assets_delete_dialog_open(void)
{
    return s_assets.show_delete_dialog_open;
}

/* ── Keyboard shortcuts ──────────────────────────────────────────── */

static void handle_asset_keyboard_shortcuts(
    const std::vector<FileEntry> &view)
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && s_assets.renaming_idx < 0
        && !ImGui::GetIO().WantTextInput)
    {
        bool has_sel = !s_assets.selected_set.empty();

        if (has_sel && s_assets.selected_set.size() == 1
            && jce_hotkey_pressed(JCE_HK_EDIT_RENAME))
        {
            int sel = *s_assets.selected_set.begin();
            if (sel >= 0 && sel < (int)view.size()) {
                s_assets.renaming_idx = sel;
                s_assets.rename_focus_needed = true;
                snprintf(s_assets.rename_buf, sizeof(s_assets.rename_buf),
                        "%s", view[sel].name.c_str());
            }
        }

        if (has_sel && jce_hotkey_pressed(JCE_HK_EDIT_DELETE))
            collect_selected_from_view_for_deletion(view);

        if (has_sel && jce_hotkey_pressed(JCE_HK_EDIT_COPY))
            copy_selection_from_view_to_clipboard(view, false);

        if (has_sel && jce_hotkey_pressed(JCE_HK_EDIT_CUT))
            copy_selection_from_view_to_clipboard(view, true);

        if (jce_hotkey_pressed(JCE_HK_EDIT_PASTE)
            && !s_assets.clipboard_paths.empty())
        {
            execute_clipboard_paste();
        }

        if (has_sel && jce_hotkey_pressed(JCE_HK_EDIT_DUPLICATE)) {
            int dup_count = 0;
            for (int si : s_assets.selected_set) {
                if (si < 0 || si >= (int)view.size()) continue;
                const FileEntry &fe = view[si];
                char unique[1200];
                if (!jce_fs_host_make_unique_path(fe.path.c_str(),
                                                   unique, sizeof(unique)))
                {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Duplicate failed: cannot find unique name for '%s'",
                        fe.name.c_str());
                    continue;
                }
                if (!jce_fs_host_copy_recursive(fe.path.c_str(), unique)) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Duplicate failed: '%s'", fe.name.c_str());
                    continue;
                }
                jce_editor_console_log("Duplicated '%s'", fe.name.c_str());
                s_assets.entry_flash[unique] = 0.6f;
                ++dup_count;
            }
            if (dup_count > 0) s_assets.needs_refresh = true;
        }

        if (jce_hotkey_pressed(JCE_HK_EDIT_SELECT_ALL)) {
            s_assets.selected_set.clear();
            for (int si = 0; si < (int)view.size(); si++)
                s_assets.selected_set.insert(si);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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
                for (size_t di = 0; di < s_assets.pending_delete_paths.size(); di++) {
                    jce_fs_host_remove_recursive(s_assets.pending_delete_paths[di].c_str());
                    jce_editor_console_log("Deleted '%s'",
                        s_assets.pending_delete_names[di].c_str());
                }
                s_assets.needs_refresh = true;
                s_assets.selected_set.clear();
                s_assets.last_clicked_idx = -1;
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

    /* Decay clipboard / paste flash effects. */
    {
        float dt = ImGui::GetIO().DeltaTime;
        if (s_assets.clipboard_flash_t > 0.0f)
            s_assets.clipboard_flash_t -= dt;
        if (s_assets.paste_flash_t > 0.0f)
            s_assets.paste_flash_t -= dt;
        for (auto it = s_assets.entry_flash.begin();
             it != s_assets.entry_flash.end(); )
        {
            it->second -= dt;
            if (it->second <= 0.0f)
                it = s_assets.entry_flash.erase(it);
            else
                ++it;
        }
    }

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

        /* (Type filter is now embedded in the search bar — see draw_asset_search_bar.) */

        {
            const std::vector<FileEntry> &raw_entries =
                s_assets.search_active ? s_assets.search_results : s_assets.entries;

            /* Apply kind_filter on top */
            std::vector<FileEntry> filtered;
            const std::vector<FileEntry> *display_ptr = &raw_entries;
            if (s_assets.kind_filter != 0) {
                filtered.reserve(raw_entries.size());
                for (const auto &fe : raw_entries) {
                    if (fe.is_dir) { filtered.push_back(fe); continue; }
                    std::string e = fe.ext;
                    for (auto &c : e) c = (char)tolower((unsigned char)c);
                    bool keep = false;
                    switch (s_assets.kind_filter) {
                    case 1: /* Images */
                        keep = (e==".png"||e==".jpg"||e==".jpeg"||e==".bmp"||e==".tga"||e==".dds"||e==".ktx"||e==".gif"||e==".webp"||e==".hdr");
                        break;
                    case 2: /* Models */
                        keep = (e==".gltf"||e==".glb"||e==".obj"||e==".fbx"||e==".dae"||e==".stl"||e==".ply"||e==".usd"||e==".usdc"||e==".usdz");
                        break;
                    case 3: /* Audio */
                        keep = (e==".wav"||e==".mp3"||e==".ogg"||e==".flac"||e==".opus"||e==".aac"||e==".m4a");
                        break;
                    case 4: /* Code/text */
                        keep = (e==".c"||e==".cpp"||e==".h"||e==".hpp"||e==".sc"||e==".sh"||e==".lua"||e==".py"||e==".js"||e==".ts"||e==".json"||e==".yaml"||e==".yml"||e==".toml"||e==".xml"||e==".md"||e==".txt"||e==".ini");
                        break;
                    case 5: /* Archive */
                        keep = (e==".zip"||e==".pak"||e==".7z"||e==".tar"||e==".gz"||e==".rar");
                        break;
                    }
                    if (keep) filtered.push_back(fe);
                }
                display_ptr = &filtered;
            }
            const std::vector<FileEntry> &display_entries = *display_ptr;
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
            handle_asset_keyboard_shortcuts(display_entries);
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
