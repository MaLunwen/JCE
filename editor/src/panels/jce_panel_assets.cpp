/*
 * jce_panel_assets.cpp  Asset Browser panel (directory tree + file grid).
 * Uses C++17 <filesystem> for real directory scanning.
 */

#include "jce_editor_panels.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"
#include "jce_file_viewer.h"
#include "jce_editor_state.h"
#include "jce_editor_scene_render.h"

#include "jce_editor_layout.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <string>
#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

/* ── Asset Browser types & state ─────────────────────────────────── */

struct FileEntry {
    std::string name;
    std::string path;
    std::string ext;
    bool is_dir;
    uintmax_t size;
};

static struct {
    std::string current_path;
    std::string project_root;
    std::vector<FileEntry> entries;
    /* Multi-selection (Windows Explorer style) */
    std::set<int> selected_set;
    int last_clicked_idx;
    bool needs_refresh;
    /* Rename state */
    int renaming_idx;
    char rename_buf[256];
    bool rename_focus_needed;
    int suppress_empty_ctx_frames;
    /* Context menu */
    int context_idx;
    /* Clipboard (multi-file) */
    std::vector<std::string> clipboard_paths;
    bool clipboard_cut;
    bool initialized;
    /* Delete confirmation */
    bool show_delete_confirm;
    bool show_delete_dialog_open;
    std::vector<std::string> pending_delete_paths;
    std::vector<std::string> pending_delete_names;
    int pending_delete_dir_count;
    /* Search filter */
    char search_buf[128];
    bool search_active;
    /* Recursive search results cache */
    std::vector<FileEntry> search_results;
    std::string last_search_query;
    std::string last_search_root;
} s_assets;

static std::string normalized_path_string(const fs::path &p)
{
    try {
        return fs::weakly_canonical(p).string();
    } catch (...) {
        try {
            return fs::absolute(p).lexically_normal().string();
        } catch (...) {
            return p.string();
        }
    }
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static void ensure_init(void)
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
    s_assets.initialized       = true;
}

void jce_editor_assets_set_project(const char *path)
{
    ensure_init();
    if (!path || !path[0]) return;
    std::string normalized = normalized_path_string(fs::path(path));
    /* If same project, don't reset current_path (preserves directory selection). */
    if (s_assets.project_root == normalized) return;
    s_assets.project_root  = normalized;
    s_assets.current_path  = normalized;
    s_assets.selected_set.clear();
    s_assets.last_clicked_idx = -1;
    s_assets.needs_refresh = true;
}

bool jce_editor_assets_delete_dialog_open(void)
{
    return s_assets.show_delete_dialog_open;
}

static void refresh_entries(void)
{
    s_assets.entries.clear();
    try {
        for (auto &de : fs::directory_iterator(s_assets.current_path)) {
            FileEntry fe;
            fe.name   = de.path().filename().string();
            fe.path   = normalized_path_string(de.path());
            fe.is_dir = de.is_directory();
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

    /* Sort: directories first, then alphabetical by name. */
    std::sort(s_assets.entries.begin(), s_assets.entries.end(),
              [](const FileEntry &a, const FileEntry &b) {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  return a.name < b.name;
              });

    s_assets.needs_refresh = false;
}

/* ── Recursive search: collect matching entries from current_path and all subdirs ── */

static void collect_search_results(const std::string &query)
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

            /* Match against filename (case-insensitive substring). */
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

            /* Limit results to avoid UI stalls on huge trees. */
            if (s_assets.search_results.size() >= 500) break;
        }
    } catch (...) { /* ignore filesystem errors */ }

    /* Sort: directories first, then alphabetical. */
    std::sort(s_assets.search_results.begin(), s_assets.search_results.end(),
              [](const FileEntry &a, const FileEntry &b) {
                  if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                  return a.name < b.name;
              });

    s_assets.last_search_query = query;
    s_assets.last_search_root  = s_assets.current_path;
}

static ImVec4 asset_color_for_ext(const std::string &ext, bool is_dir)
{
    if (is_dir) return JCE_COLOR_ASSET_FOLDER;
    /* Code files */
    if (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".hpp"
        || ext == ".java" || ext == ".kt" || ext == ".lua" || ext == ".py")
        return JCE_COLOR_ASSET_CODE;
    /* Scene files */
    if (ext == ".scene") return JCE_COLOR_ASSET_SCENE;
    /* Data / config files */
    if (ext == ".json" || ext == ".xml" || ext == ".ini" || ext == ".mat")
        return JCE_COLOR_ASSET_DATA;
    /* Image files */
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp"
        || ext == ".tga" || ext == ".hdr")
        return JCE_COLOR_ASSET_IMAGE;
    return JCE_COLOR_ASSET_DEFAULT;
}

static const char *type_label_for_entry(const FileEntry &fe)
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
    /* Return the extension itself (without dot, up to 4 chars). */
    if (!fe.ext.empty()) return fe.ext.c_str() + 1; /* skip the '.' */
    return "FILE";
}

/* ── Directory tree (recursive) ──────────────────────────────────── */

static void draw_dir_tree(const fs::path &dir, int depth)
{
    if (depth > 5) return;
    try {
        std::vector<fs::path> subdirs;
        for (auto &de : fs::directory_iterator(dir)) {
            if (de.is_directory())
                subdirs.push_back(de.path());
        }
        std::sort(subdirs.begin(), subdirs.end());

        for (auto &sd : subdirs) {
            std::string dirname = sd.filename().string();

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                     | ImGuiTreeNodeFlags_SpanAvailWidth;

            /* Highlight the currently browsed directory. */
            bool is_current = false;
            try { is_current = fs::equivalent(sd, s_assets.current_path); }
            catch (...) { is_current = (sd.string() == s_assets.current_path); }
            if (is_current)
                flags |= ImGuiTreeNodeFlags_Selected;

            /* Check if this dir has subdirectories to decide leaf/branch. */
            bool has_children = false;
            try {
                for (auto &child : fs::directory_iterator(sd)) {
                    if (child.is_directory()) { has_children = true; break; }
                }
            } catch (...) {}

            if (!has_children)
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

            bool open = ImGui::TreeNodeEx(dirname.c_str(), flags);

            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                s_assets.current_path  = sd.string();
                s_assets.needs_refresh = true;
            }

            /* Right-click context menu on tree directory */
            if (ImGui::BeginPopupContextItem()) {
#ifdef _WIN32
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
                    std::string p = sd.string();
                    for (auto &ch : p) { if (ch == '/') ch = '\\'; }
                    ShellExecuteA(NULL, "explore", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
                    std::string cmd = "code \"" + sd.string() + "\"";
                    system(cmd.c_str());
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
                    std::string cmd = "start cmd /K cd /d \"" + sd.string() + "\"";
                    system(cmd.c_str());
                }
#endif
                ImGui::EndPopup();
            }

            if (open && has_children) {
                draw_dir_tree(sd, depth + 1);
                ImGui::TreePop();
            }
        }
    } catch (const std::exception &e) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Asset tree: %s", e.what());
    }
}

/* ── Clipboard / selection helpers (shared by context menu & keyboard) ── */

static void copy_selection_to_clipboard(bool cut)
{
    s_assets.clipboard_paths.clear();
    for (int si : s_assets.selected_set)
        if (si >= 0 && si < (int)s_assets.entries.size())
            s_assets.clipboard_paths.push_back(s_assets.entries[si].path);
    s_assets.clipboard_cut = cut;
}

static void execute_clipboard_paste(void)
{
    try {
        for (auto &cp : s_assets.clipboard_paths) {
            fs::path src(cp);
            fs::path dst = fs::path(s_assets.current_path)
                        / src.filename();
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

static void collect_selected_for_deletion(void)
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

/* ── A. Directory tree panel (left side) ─────────────────────────── */

static void draw_asset_directory_tree(float tree_w, float panel_h)
{
    ImGui::BeginChild("AssetTree", ImVec2(tree_w, panel_h), ImGuiChildFlags_Borders);
    {
        std::string root_name = fs::path(s_assets.project_root).filename().string();
        if (root_name.empty() || root_name == "." || root_name == "/" || root_name == "\\")
            root_name = "Project";

        ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen
                                      | ImGuiTreeNodeFlags_OpenOnArrow
                                      | ImGuiTreeNodeFlags_SpanAvailWidth;
        /* Highlight root if it is the current directory. */
        bool root_is_current = false;
        try { root_is_current = fs::equivalent(s_assets.project_root, s_assets.current_path); }
        catch (...) { root_is_current = (s_assets.current_path == s_assets.project_root); }
        if (root_is_current)
            root_flags |= ImGuiTreeNodeFlags_Selected;

        if (ImGui::TreeNodeEx(root_name.c_str(), root_flags)) {
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                s_assets.current_path  = s_assets.project_root;
                s_assets.needs_refresh = true;
            }
            /* Right-click context menu on root node */
            if (ImGui::BeginPopupContextItem()) {
#ifdef _WIN32
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
                    std::string p = s_assets.project_root;
                    for (auto &ch : p) { if (ch == '/') ch = '\\'; }
                    ShellExecuteA(NULL, "explore", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
                    std::string cmd = "code \"" + s_assets.project_root + "\"";
                    system(cmd.c_str());
                }
                if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
                    std::string cmd = "start cmd /K cd /d \"" + s_assets.project_root + "\"";
                    system(cmd.c_str());
                }
#endif
                ImGui::EndPopup();
            }
            draw_dir_tree(s_assets.project_root, 0);
            ImGui::TreePop();
        }
    }
    ImGui::EndChild();
}

/* ── B. Breadcrumb bar (up button + path segments + refresh) ─────── */

static void draw_asset_breadcrumb_bar(void)
{
    {
        fs::path cur(s_assets.current_path);
        fs::path root(s_assets.project_root);
        bool at_root = false;
        try { at_root = fs::equivalent(cur, root); }
        catch (...) { at_root = (s_assets.current_path == s_assets.project_root); }

        /* Up button (always visible; grayed out at root) */
        ImGui::BeginDisabled(at_root);
        if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.up"))) {
            try {
                fs::path parent = fs::path(s_assets.current_path).parent_path();
                /* Don't navigate above project root. */
                bool above_root = false;
                try { above_root = !fs::equivalent(parent, root)
                                && parent.string().length() < root.string().length(); }
                catch (...) {}
                if (!above_root) {
                    s_assets.current_path  = parent.string();
                    s_assets.needs_refresh = true;
                    s_assets.selected_set.clear();
                    s_assets.last_clicked_idx = -1;
                }
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Navigate up: %s", e.what());
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        /* Build breadcrumb segments: Project > sub1 > sub2 */
        {
            std::string root_name = fs::path(s_assets.project_root).filename().string();
            if (root_name.empty() || root_name == "." || root_name == "/" || root_name == "\\")
                root_name = "Project";

            std::vector<std::pair<std::string, std::string>> crumbs;
            crumbs.push_back({root_name, s_assets.project_root});

            if (!at_root) {
                fs::path rel;
                try { rel = fs::relative(cur, root); }
                catch (...) {}
                if (!rel.empty() && rel != ".") {
                    fs::path accum(s_assets.project_root);
                    for (auto &part : rel) {
                        accum = accum / part;
                        crumbs.push_back({part.string(), accum.string()});
                    }
                }
            }

            for (size_t i = 0; i < crumbs.size(); i++) {
                if (i > 0) {
                    ImGui::SameLine(0, 2);
                    ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, ">");
                    ImGui::SameLine(0, 2);
                }
                bool is_last = (i == crumbs.size() - 1);
                if (is_last) {
                    ImGui::TextColored(JCE_COLOR_TEXT_PRIMARY,
                                       "%s", crumbs[i].first.c_str());
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.3f, 0.3f, 0.4f, 0.5f));
                    char bid[128];
                    snprintf(bid, sizeof(bid), "%s###bc_%d",
                             crumbs[i].first.c_str(), (int)i);
                    if (ImGui::SmallButton(bid)) {
                        s_assets.current_path  = crumbs[i].second;
                        s_assets.needs_refresh = true;
                        s_assets.selected_set.clear();
                        s_assets.last_clicked_idx = -1;
                    }
                    ImGui::PopStyleColor(2);
                }
            }
        }

        /* Refresh button right-aligned */
        {
            float btn_w = ImGui::CalcTextSize(jce_editor_i18n("assetBrowser.refresh")).x
                        + ImGui::GetStyle().FramePadding.x * 2;
            float avail_w = ImGui::GetContentRegionAvail().x;
            if (avail_w > btn_w + 8) {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btn_w);
            } else {
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(jce_editor_i18n("assetBrowser.refresh"))) {
                s_assets.needs_refresh = true;
            }
        }
    }
    ImGui::Separator();
}

/* ── C. Search / filter bar ──────────────────────────────────────── */

static void draw_asset_search_bar(void)
{
    {
        float search_w = ImGui::GetContentRegionAvail().x;
        ImGui::PushItemWidth(search_w);
        ImGui::InputTextWithHint("##asset_search",
                                 jce_editor_i18n("assetBrowser.searchAllSubdirs"),
                                 s_assets.search_buf,
                                 sizeof(s_assets.search_buf));
        ImGui::PopItemWidth();
        s_assets.search_active = (s_assets.search_buf[0] != '\0');

        /* Rebuild recursive search results when query or directory changes. */
        if (s_assets.search_active) {
            std::string q(s_assets.search_buf);
            if (q != s_assets.last_search_query
                || s_assets.current_path != s_assets.last_search_root) {
                collect_search_results(q);
            }
        } else {
            if (!s_assets.search_results.empty()) {
                s_assets.search_results.clear();
                s_assets.last_search_query.clear();
            }
        }
    }
    ImGui::Separator();
}

/* ── D. Single grid item (thumbnail + label + interactions) ──────── */

static void draw_asset_grid_item(const FileEntry &fe, int index,
                                 int cols, int &col, bool &want_ctx_popup)
{
    ImGui::PushID(index);
    ImGui::BeginGroup();

    /* ── Rename overlay (Java: renameFocusNeeded pattern) ── */
    if (s_assets.renaming_idx == index) {
        if (s_assets.rename_focus_needed) {
            ImGui::SetKeyboardFocusHere();
            s_assets.rename_focus_needed = false;
        }
        ImGui::SetNextItemWidth((float)JCE_THUMBNAIL_SIZE);
        if (ImGui::InputText("##rename", s_assets.rename_buf,
                            sizeof(s_assets.rename_buf),
                            ImGuiInputTextFlags_EnterReturnsTrue
                            | ImGuiInputTextFlags_AutoSelectAll))
        {
            try {
                fs::path old_p(fe.path);
                fs::path new_p = old_p.parent_path() / s_assets.rename_buf;
                fs::rename(old_p, new_p);
                jce_editor_console_log("Renamed '%s' -> '%s'",
                    fe.name.c_str(), s_assets.rename_buf);
                s_assets.needs_refresh = true;
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Rename failed: %s", e.what());
            }
            s_assets.renaming_idx = -1;
            s_assets.rename_focus_needed = false;
        }
        /* Cancel on Escape, or click outside (Java reference:
           !isItemActive && isMouseClicked(0) && !isItemHovered) */
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)
            || (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)
                && !ImGui::IsItemHovered()))
        {
            s_assets.renaming_idx = -1;
            s_assets.rename_focus_needed = false;
        }
        ImGui::EndGroup();
        ImGui::PopID();

        col++;
        if (col < cols) ImGui::SameLine();
        else col = 0;
        return;
    }

    /* ── Thumbnail button (Java reference style: full color) ── */
    ImVec4 col4 = asset_color_for_ext(fe.ext, fe.is_dir);
    ImGui::PushStyleColor(ImGuiCol_Button, col4);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4(col4.x * 1.1f, col4.y * 1.1f, col4.z * 1.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        ImVec4(col4.x * 0.9f, col4.y * 0.9f, col4.z * 0.9f, 1.0f));

    const char *label = type_label_for_entry(fe);
    /* Empty label for the button — text overlay is drawn separately. */
    bool clicked = ImGui::Button("##icon",
        ImVec2((float)JCE_THUMBNAIL_SIZE, (float)JCE_THUMBNAIL_SIZE));

    /* Centered type label overlay with shadow */
    {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImVec2 tsz = ImGui::CalcTextSize(label);
        float cx = mn.x + (mx.x - mn.x - tsz.x) * 0.5f;
        float cy = mn.y + (mx.y - mn.y - tsz.y) * 0.5f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(cx + 1, cy + 1),
            IM_COL32(0, 0, 0, 200), label);
        dl->AddText(ImVec2(cx, cy),
            IM_COL32(255, 255, 255, 255), label);
    }

    ImGui::PopStyleColor(3);

    /* Single click: select (Ctrl/Shift multi-select) */
    if (clicked) {
        bool ctrl  = ImGui::GetIO().KeyCtrl;
        bool shift = ImGui::GetIO().KeyShift;
        if (shift && s_assets.last_clicked_idx >= 0) {
            int mn = (s_assets.last_clicked_idx < index) ? s_assets.last_clicked_idx : index;
            int mx = (s_assets.last_clicked_idx > index) ? s_assets.last_clicked_idx : index;
            if (!ctrl) s_assets.selected_set.clear();
            for (int k = mn; k <= mx; k++)
                s_assets.selected_set.insert(k);
        } else if (ctrl) {
            if (s_assets.selected_set.count(index))
                s_assets.selected_set.erase(index);
            else
                s_assets.selected_set.insert(index);
            s_assets.last_clicked_idx = index;
        } else {
            s_assets.selected_set.clear();
            s_assets.selected_set.insert(index);
            s_assets.last_clicked_idx = index;
        }
    }

    /* Selection outline (blue) */
    if (s_assets.selected_set.count(index)) {
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(mn, mx,
            ImGui::ColorConvertFloat4ToU32(JCE_COLOR_ASSET_SELECTED),
            2.0f, 0, 2.0f);
    }

    /* File name label — selected items show full name,
     * unselected items clamp to 2 lines with ellipsis.
     * Extension is shown in a dimmer color for readability. */
    {
        bool is_selected = s_assets.selected_set.count(index) != 0;
        float name_w = (float)JCE_THUMBNAIL_SIZE;
        float line_h = ImGui::GetTextLineHeightWithSpacing();
        float max_h  = line_h * 2.0f;
        ImVec2 name_start = ImGui::GetCursorScreenPos();

        /* Split name into stem + extension. */
        std::string stem = fe.name;
        std::string ext_part;

        ImVec4 name_col = is_selected
            ? JCE_COLOR_ASSET_SEL_TEXT : JCE_COLOR_TEXT_PRIMARY;
        ImVec4 ext_col  = is_selected
            ? ImVec4(name_col.x * 0.75f, name_col.y * 0.75f,
                     name_col.z * 0.85f, name_col.w)
            : JCE_COLOR_TEXT_SECONDARY;

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + name_w);

        if (is_selected) {
            /* Selected: show full name (stem + dim extension),
             * no height clamp so the user can read it all. */
            ImGui::PushStyleColor(ImGuiCol_Text, name_col);
            ImGui::TextWrapped("%s", stem.c_str());
            ImGui::PopStyleColor();
            if (!ext_part.empty()) {
                ImGui::SameLine(0, 0);
                ImGui::PushStyleColor(ImGuiCol_Text, ext_col);
                ImGui::TextWrapped("%s", ext_part.c_str());
                ImGui::PopStyleColor();
            }
        } else {
            /* Unselected: clamp to 2 lines. */
            ImVec2 full_sz = ImGui::CalcTextSize(
                fe.name.c_str(), NULL, false, name_w);
            if (full_sz.y <= max_h) {
                /* Fits: show stem + dim extension. */
                ImGui::PushStyleColor(ImGuiCol_Text, name_col);
                ImGui::TextWrapped("%s", stem.c_str());
                ImGui::PopStyleColor();
                if (!ext_part.empty()) {
                    ImGui::SameLine(0, 0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ext_col);
                    ImGui::TextWrapped("%s", ext_part.c_str());
                    ImGui::PopStyleColor();
                }
            } else {
                /* Truncate stem, keep extension visible. */
                ImGui::PushStyleColor(ImGuiCol_Text, name_col);
                if (!ext_part.empty()) {
                    /* Try to show as much of the stem as fits with "..." + ext */
                    char trunc[256];
                    int slen = (int)stem.size();
                    int cut = slen;
                    for (cut = slen - 1; cut > 0; cut--) {
                        snprintf(trunc, sizeof(trunc), "%.*s...%s",
                                 cut, stem.c_str(), ext_part.c_str());
                        ImVec2 ts = ImGui::CalcTextSize(
                            trunc, NULL, false, name_w);
                        if (ts.y <= max_h) break;
                    }
                    if (cut <= 0)
                        snprintf(trunc, sizeof(trunc), "...%s",
                                 ext_part.c_str());
                    /* Render stem part + dim extension part. */
                    char stem_trunc[256];
                    snprintf(stem_trunc, sizeof(stem_trunc),
                             "%.*s...", (cut > 0 ? cut : 0),
                             stem.c_str());
                    ImGui::TextWrapped("%s", stem_trunc);
                    ImGui::SameLine(0, 0);
                    ImGui::PopStyleColor();
                    ImGui::PushStyleColor(ImGuiCol_Text, ext_col);
                    ImGui::TextWrapped("%s", ext_part.c_str());
                } else {
                    /* No extension (directory or extensionless). */
                    const char *src = fe.name.c_str();
                    int len = (int)strlen(src);
                    char trunc[256];
                    int cut = len;
                    for (cut = len - 1; cut > 0; cut--) {
                        snprintf(trunc, sizeof(trunc),
                                 "%.*s...", cut, src);
                        ImVec2 ts = ImGui::CalcTextSize(
                            trunc, NULL, false, name_w);
                        if (ts.y <= max_h) break;
                    }
                    if (cut <= 0)
                        snprintf(trunc, sizeof(trunc), "...");
                    ImGui::TextWrapped("%s", trunc);
                }
                ImGui::PopStyleColor();
            }
            /* Pad to constant 2-line height for grid alignment. */
            float used_h = ImGui::GetCursorScreenPos().y
                         - name_start.y;
            if (used_h < max_h)
                ImGui::Dummy(ImVec2(0, max_h - used_h));
        }

        ImGui::PopTextWrapPos();
    }

    ImGui::EndGroup();

    /* Use the group item (icon + file name) as the hit area. */
    bool cell_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    /* Double click: navigate or open. */
    if (cell_hovered && ImGui::IsMouseDoubleClicked(0)) {
        if (fe.is_dir) {
            s_assets.current_path  = fe.path;
            s_assets.needs_refresh = true;
            s_assets.selected_set.clear();
            s_assets.last_clicked_idx = -1;
            /* Clear search when navigating into a directory. */
            s_assets.search_buf[0] = '\0';
            s_assets.search_active = false;
            s_assets.search_results.clear();
            s_assets.last_search_query.clear();
        } else {
            /* Load .scene files directly into the viewport. */
            const char *ext = strrchr(fe.path.c_str(), '.');
            if (ext && (_stricmp(ext, ".scene") == 0)) {
                jce_state_load_scene_file(fe.path.c_str());
                /* Update scene dir so mesh resolution works. */
                std::string dir = fe.path;
                size_t sep = dir.find_last_of("/\\");
                if (sep != std::string::npos) dir.resize(sep);
                jce_editor_scene_set_scene_dir(dir.c_str());
            } else {
                jce_file_viewer_open(fe.path.c_str());
                jce_editor_layout_request_focus_file_viewer();
            }
        }
    }

    /* Right click: open item context menu.
     * If the item is already selected (part of a multi-selection),
     * keep the selection intact so the context menu can operate on
     * all selected items.  Only replace the selection when the
     * right-clicked item is NOT already selected. */
    if (cell_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        s_assets.context_idx = index;
        if (s_assets.selected_set.find(index) == s_assets.selected_set.end()) {
            s_assets.selected_set.clear();
            s_assets.selected_set.insert(index);
            s_assets.last_clicked_idx = index;
        }
        /* Prevent blank-area popup from stealing this right-click. */
        s_assets.suppress_empty_ctx_frames = 2;
        want_ctx_popup = true;
    }

    /* Tooltip: show full file path on hover. */
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", fe.path.c_str());

    /* Drag-drop source: allow dragging asset paths to inspector fields. */
    if (!fe.is_dir && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const char *p = fe.path.c_str();
        ImGui::SetDragDropPayload("JCE_ASSET_PATH", p, strlen(p) + 1);
        ImGui::Text("%s", fe.name.c_str());
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();

    col++;
    if (col < cols) ImGui::SameLine();
    else col = 0;
}

/* ── E. Item context menu popup ──────────────────────────────────── */

static void draw_asset_item_context_menu(const std::vector<FileEntry> &display_entries)
{
    if (ImGui::BeginPopup("AssetContextMenu")) {
        int ci = s_assets.context_idx;
        bool valid = (ci >= 0 && ci < (int)display_entries.size());
        const FileEntry *cfe = valid ? &display_entries[ci] : nullptr;

        /* Open */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.open"))) {
            if (cfe) {
                if (cfe->is_dir) {
                    s_assets.current_path  = cfe->path;
                    s_assets.needs_refresh = true;
                    s_assets.selected_set.clear();
                    s_assets.last_clicked_idx = -1;
                } else {
                    jce_file_viewer_open(cfe->path.c_str());
                    jce_editor_layout_request_focus_file_viewer();
                }
            }
        }

        /* Open in VS Code */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInVSCode"))) {
            if (cfe) {
#ifdef _WIN32
                std::string cmd = "code \"" + cfe->path + "\"";
                system(cmd.c_str());
#endif
            }
        }

        /* Open in Explorer */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
            if (cfe) {
#ifdef _WIN32
                std::string p = cfe->path;
                for (auto &ch : p) { if (ch == '/') ch = '\\'; }
                if (cfe->is_dir) {
                    ShellExecuteA(NULL, "explore", p.c_str(), NULL, NULL, SW_SHOWNORMAL);
                } else {
                    std::string arg = "/select,\"" + p + "\"";
                    ShellExecuteA(NULL, NULL, "explorer.exe", arg.c_str(), NULL, SW_SHOWNORMAL);
                }
#endif
            }
        }

        ImGui::Separator();

        /* Rename (F2) */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.rename"), "F2")) {
            if (cfe) {
                s_assets.renaming_idx = ci;
                s_assets.rename_focus_needed = true;
                snprintf(s_assets.rename_buf, sizeof(s_assets.rename_buf),
                        "%s", cfe->name.c_str());
            }
        }

        /* Duplicate (Ctrl+D) */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.duplicate"), "Ctrl+D")) {
            if (cfe) {
                try {
                    fs::path src(cfe->path);
                    fs::path dst;
                    if (cfe->is_dir) {
                        dst = src.parent_path() / (cfe->name + "_copy");
                    } else {
                        std::string stem = src.stem().string();
                        std::string ext  = src.extension().string();
                        dst = src.parent_path() / (stem + "_copy" + ext);
                    }
                    fs::copy(src, dst, fs::copy_options::recursive);
                    jce_editor_console_log("Duplicated '%s'", cfe->name.c_str());
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Duplicate failed: %s", e.what());
                }
            }
        }

        ImGui::Separator();

        /* Copy Path */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copyPath"))) {
            if (cfe) {
                ImGui::SetClipboardText(cfe->path.c_str());
            }
        }

        /* Copy (Ctrl+C) */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.copy"), "Ctrl+C")) {
            if (cfe) copy_selection_to_clipboard(false);
        }

        /* Cut (Ctrl+X) */
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.cut"), "Ctrl+X")) {
            if (cfe) copy_selection_to_clipboard(true);
        }

        ImGui::Separator();

        /* Delete (Del) — red text, triggers confirmation */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.delete"), "Del")) {
            if (cfe) collect_selected_for_deletion();
        }
        ImGui::PopStyleColor();

        ImGui::EndPopup();
    }
}

/* ── F. Keyboard shortcuts (when grid focused, NOT during rename) ── */

static void handle_asset_keyboard_shortcuts(void)
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && s_assets.renaming_idx < 0
        && !ImGui::GetIO().WantTextInput)
    {
        bool has_sel = !s_assets.selected_set.empty();
        bool ctrl    = ImGui::GetIO().KeyCtrl;

        /* F2 - Rename (single selection only) */
        if (has_sel && s_assets.selected_set.size() == 1
            && ImGui::IsKeyPressed(ImGuiKey_F2))
        {
            int sel = *s_assets.selected_set.begin();
            s_assets.renaming_idx = sel;
            s_assets.rename_focus_needed = true;
            snprintf(s_assets.rename_buf, sizeof(s_assets.rename_buf),
                    "%s", s_assets.entries[sel].name.c_str());
        }

        /* Delete — triggers confirmation dialog (all selected) */
        if (has_sel && ImGui::IsKeyPressed(ImGuiKey_Delete))
            collect_selected_for_deletion();

        /* Ctrl+C - Copy (all selected) */
        if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_C))
            copy_selection_to_clipboard(false);

        /* Ctrl+X - Cut (all selected) */
        if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_X))
            copy_selection_to_clipboard(true);

        /* Ctrl+V - Paste (all clipboard items) */
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)
            && !s_assets.clipboard_paths.empty())
        {
            execute_clipboard_paste();
        }

        /* Ctrl+D - Duplicate (all selected) */
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

        /* Ctrl+A - Select All */
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            s_assets.selected_set.clear();
            for (int si = 0; si < (int)s_assets.entries.size(); si++)
                s_assets.selected_set.insert(si);
        }

        /* Escape - Deselect all */
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_assets.selected_set.clear();
            s_assets.last_clicked_idx = -1;
        }
    }
}

/* ── G. Empty area context menu (right-click on blank space) ─────── */

static void draw_asset_empty_area_menu(void)
{
    if (s_assets.suppress_empty_ctx_frames > 0)
        s_assets.suppress_empty_ctx_frames--;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && s_assets.suppress_empty_ctx_frames == 0
        && !ImGui::IsPopupOpen("AssetContextMenu")
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup))
    {
        ImGui::OpenPopup("EmptyAreaCtx");
    }

    if (ImGui::BeginPopup("EmptyAreaCtx"))
    {
        if (ImGui::BeginMenu(jce_editor_i18n("assetBrowser.create"))) {
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.newFolder"))) {
                try {
                    fs::path nf = fs::path(s_assets.current_path) / "New Folder";
                    int cnt = 1;
                    while (fs::exists(nf)) {
                        char name[64];
                        snprintf(name, sizeof(name), "New Folder %d", cnt++);
                        nf = fs::path(s_assets.current_path) / name;
                    }
                    fs::create_directory(nf);
                    jce_editor_console_log("Created '%s'", nf.filename().string().c_str());
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "New folder: %s", e.what());
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.newScene"))) {
                try {
                    fs::path nf = fs::path(s_assets.current_path) / "New Scene.scene";
                    FILE *fp = fopen(nf.string().c_str(), "w");
                    if (fp) { fputs("{}", fp); fclose(fp); }
                    s_assets.needs_refresh = true;
                } catch (...) {}
            }
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.newScript"))) {
                try {
                    fs::path nf = fs::path(s_assets.current_path) / "NewScript.c";
                    FILE *fp = fopen(nf.string().c_str(), "w");
                    if (fp) { fputs("/* New Script */\n", fp); fclose(fp); }
                    s_assets.needs_refresh = true;
                } catch (...) {}
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.paste"), "Ctrl+V",
                            false, !s_assets.clipboard_paths.empty()))
        {
            execute_clipboard_paste();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.selectAll"), "Ctrl+A")) {
            s_assets.selected_set.clear();
            for (int si = 0; si < (int)s_assets.entries.size(); si++)
                s_assets.selected_set.insert(si);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.refresh"))) {
            s_assets.needs_refresh = true;
        }
#ifdef _WIN32
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInExplorer"))) {
            std::string cmd = "explorer \"" + s_assets.current_path + "\"";
            system(cmd.c_str());
        }
        if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.openInTerminal"))) {
            std::string cmd = "start cmd /K cd /d \"" + s_assets.current_path + "\"";
            system(cmd.c_str());
        }
#endif
        ImGui::EndPopup();
    }
}

/* ── H. Delete confirmation dialog ───────────────────────────────── */

static void draw_asset_delete_dialog(void)
{
    if (s_assets.show_delete_confirm) {
        s_assets.show_delete_dialog_open = true;
        s_assets.show_delete_confirm = false;
    }
    if (s_assets.show_delete_dialog_open) {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_Appearing);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::SetNextWindowFocus();

        char title[256];
        snprintf(title, sizeof(title), "%s###AssetDeleteConfirm", jce_editor_i18n("dialog.confirmDelete"));
        if (ImGui::Begin(title,
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
                s_assets.show_delete_dialog_open = false;
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine();

            if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
                s_assets.show_delete_dialog_open = false;
            }
        }
        ImGui::End();
    }
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_assets_content(void)
{
    ensure_init();

    if (s_assets.needs_refresh)
        refresh_entries();

    /* ── Split: directory tree | (breadcrumb + file grid) ───────── */
    float tree_w = ImGui::GetContentRegionAvail().x * JCE_ASSET_TREE_WIDTH_RATIO;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    /* Left: Directory tree */
    draw_asset_directory_tree(tree_w, avail.y);

    ImGui::SameLine();

    /* Right: Breadcrumb + file grid */
    ImGui::BeginChild("FileArea", ImVec2(0, avail.y), false);
    {
        draw_asset_breadcrumb_bar();
        draw_asset_search_bar();

        {
            /* When searching, iterate the recursive results; otherwise the flat listing. */
            const std::vector<FileEntry> &display_entries =
                s_assets.search_active ? s_assets.search_results : s_assets.entries;

            float cell_size = JCE_THUMBNAIL_SIZE + JCE_ASSET_CELL_PADDING * 2;
            float panel_w   = ImGui::GetContentRegionAvail().x;
            int cols = (int)(panel_w / cell_size);
            if (cols < 1) cols = 1;

            int col = 0;
            bool want_ctx_popup = false;
            for (int i = 0; i < (int)display_entries.size(); i++)
                draw_asset_grid_item(display_entries[i], i, cols, col, want_ctx_popup);

            /* Open context menu outside the PushID loop (ID stack must match) */
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
