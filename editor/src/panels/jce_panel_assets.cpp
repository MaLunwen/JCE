/*
 * jce_panel_assets.cpp  Asset Browser panel (directory tree + file grid).
 * Uses C++17 <filesystem> for real directory scanning.
 */

#include "jce_editor_panels.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <string>

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
    int selected_idx;
    bool needs_refresh;
    /* Rename state */
    int renaming_idx;
    char rename_buf[256];
    /* Context menu */
    int context_idx;
    /* Clipboard */
    std::string clipboard_path;
    bool clipboard_cut;
    bool initialized;
} s_assets;

/* ── Helpers ─────────────────────────────────────────────────────── */

static void ensure_init(void)
{
    if (s_assets.initialized) return;
    s_assets.project_root  = ".";
    s_assets.current_path  = s_assets.project_root;
    s_assets.selected_idx  = -1;
    s_assets.renaming_idx  = -1;
    s_assets.context_idx   = -1;
    s_assets.needs_refresh = true;
    s_assets.clipboard_cut = false;
    s_assets.initialized   = true;
}

static void refresh_entries(void)
{
    s_assets.entries.clear();
    try {
        for (auto &de : fs::directory_iterator(s_assets.current_path)) {
            FileEntry fe;
            fe.name   = de.path().filename().string();
            fe.path   = de.path().string();
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

static ImVec4 asset_color_for_ext(const std::string &ext, bool is_dir)
{
    if (is_dir) return JCE_COLOR_ASSET_FOLDER;
    if (ext == ".scene" || ext == ".json") return JCE_COLOR_ASSET_SCENE;
    if (ext == ".mat")                     return JCE_COLOR_ASSET_MATERIAL;
    if (ext == ".png" || ext == ".jpg" || ext == ".bmp" || ext == ".tga")
        return JCE_COLOR_ASSET_TEXTURE;
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3")
        return JCE_COLOR_ASSET_AUDIO;
    if (ext == ".lua" || ext == ".py" || ext == ".c" || ext == ".cpp" || ext == ".h")
        return JCE_COLOR_ASSET_SCRIPT;
    return JCE_COLOR_TEXT_PRIMARY;
}

static const char *icon_letter_for_entry(const FileEntry &fe)
{
    if (fe.is_dir) return "D";
    if (fe.ext == ".png" || fe.ext == ".jpg" || fe.ext == ".bmp" || fe.ext == ".tga")
        return "T";
    if (fe.ext == ".obj" || fe.ext == ".fbx" || fe.ext == ".gltf" || fe.ext == ".glb")
        return "M";
    if (fe.ext == ".lua" || fe.ext == ".py" || fe.ext == ".c" || fe.ext == ".cpp" || fe.ext == ".h")
        return "S";
    if (fe.ext == ".wav" || fe.ext == ".ogg" || fe.ext == ".mp3")
        return "A";
    if (fe.ext == ".scene" || fe.ext == ".json" || fe.ext == ".mat")
        return "C";
    return "F";
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
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_ASSET_FOLDER);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                     | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                     | ImGuiTreeNodeFlags_SpanAvailWidth;

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

            ImGui::PopStyleColor();

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

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_assets_content(void)
{
    ensure_init();

    if (s_assets.needs_refresh)
        refresh_entries();

    /* ── Path bar ─────────────────────────────────────────────────── */
    {
        fs::path cur(s_assets.current_path);
        fs::path root(s_assets.project_root);
        bool at_root = false;
        try { at_root = fs::equivalent(cur, root); }
        catch (...) { at_root = (s_assets.current_path == s_assets.project_root); }

        if (!at_root) {
            if (ImGui::Button("^")) {
                try {
                    fs::path parent = fs::path(s_assets.current_path).parent_path();
                    s_assets.current_path  = parent.string();
                    s_assets.needs_refresh = true;
                    s_assets.selected_idx  = -1;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Navigate up: %s", e.what());
                }
            }
            ImGui::SameLine();
        }

        if (ImGui::Button(jce_editor_i18n("assetBrowser.refresh"))) {
            s_assets.needs_refresh = true;
        }
        ImGui::SameLine();

        ImGui::TextColored(JCE_COLOR_TEXT_SECONDARY, "%s:",
                           jce_editor_i18n("project.path"));
        ImGui::SameLine();
        ImGui::Text("%s", s_assets.current_path.c_str());
    }
    ImGui::Separator();

    /* ── Split: directory tree | file grid ─────────────────────────── */
    float tree_w = ImGui::GetContentRegionAvail().x * JCE_ASSET_TREE_WIDTH_RATIO;
    ImVec2 avail = ImGui::GetContentRegionAvail();

    /* Left: Directory tree */
    ImGui::BeginChild("AssetTree", ImVec2(tree_w, avail.y), ImGuiChildFlags_Borders);
    {
        std::string root_name = fs::path(s_assets.project_root).filename().string();
        if (root_name.empty()) root_name = ".";
        ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_ASSET_FOLDER);
        if (ImGui::TreeNodeEx(root_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PopStyleColor();
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                s_assets.current_path  = s_assets.project_root;
                s_assets.needs_refresh = true;
            }
            draw_dir_tree(s_assets.project_root, 0);
            ImGui::TreePop();
        } else {
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    /* Right: File grid */
    ImGui::BeginChild("AssetGrid", ImVec2(0, avail.y), ImGuiChildFlags_Borders);
    {
        float cell_size = JCE_THUMBNAIL_SIZE + JCE_ASSET_CELL_PADDING * 2;
        float panel_w   = ImGui::GetContentRegionAvail().x;
        int cols = (int)(panel_w / cell_size);
        if (cols < 1) cols = 1;

        int col = 0;
        for (int i = 0; i < (int)s_assets.entries.size(); i++) {
            const FileEntry &fe = s_assets.entries[i];

            ImGui::PushID(i);
            ImGui::BeginGroup();

            /* ── Rename overlay ──────────────────────────────────── */
            if (s_assets.renaming_idx == i) {
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
                }
                /* Cancel on Escape or click away */
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)
                    || (!ImGui::IsItemActive() && !ImGui::IsItemFocused()
                        && ImGui::IsMouseClicked(0)))
                {
                    s_assets.renaming_idx = -1;
                }
                ImGui::EndGroup();
                ImGui::PopID();

                col++;
                if (col < cols) ImGui::SameLine();
                else col = 0;
                continue;
            }

            /* ── Thumbnail button ────────────────────────────────── */
            ImVec4 col4 = asset_color_for_ext(fe.ext, fe.is_dir);
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(col4.x * 0.3f, col4.y * 0.3f, col4.z * 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(col4.x * 0.5f, col4.y * 0.5f, col4.z * 0.5f, 1.0f));

            const char *icon = icon_letter_for_entry(fe);
            bool clicked = ImGui::Button(icon,
                ImVec2((float)JCE_THUMBNAIL_SIZE, (float)JCE_THUMBNAIL_SIZE));
            ImGui::PopStyleColor(2);

            /* Single click: select */
            if (clicked)
                s_assets.selected_idx = i;

            /* Double click: navigate or open */
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                if (fe.is_dir) {
                    s_assets.current_path  = fe.path;
                    s_assets.needs_refresh = true;
                    s_assets.selected_idx  = -1;
                } else {
                    jce_file_viewer_open(fe.path.c_str());
                }
            }

            /* Right click: context menu */
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                s_assets.context_idx  = i;
                s_assets.selected_idx = i;
                ImGui::OpenPopup("AssetContextMenu");
            }

            /* Selection outline */
            if (s_assets.selected_idx == i) {
                ImVec2 mn = ImGui::GetItemRectMin();
                ImVec2 mx = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRect(mn, mx,
                    ImGui::ColorConvertFloat4ToU32(JCE_COLOR_SELECTION_OUTLINE),
                    2.0f, 0, 2.0f);
            }

            /* File name label */
            ImGui::PushStyleColor(ImGuiCol_Text, col4);
            ImGui::TextWrapped("%s", fe.name.c_str());
            ImGui::PopStyleColor();

            ImGui::EndGroup();
            ImGui::PopID();

            col++;
            if (col < cols) ImGui::SameLine();
            else col = 0;
        }

        /* ── Context menu popup ──────────────────────────────────── */
        if (ImGui::BeginPopup("AssetContextMenu")) {
            int ci = s_assets.context_idx;
            bool valid = (ci >= 0 && ci < (int)s_assets.entries.size());
            const FileEntry *cfe = valid ? &s_assets.entries[ci] : nullptr;

            /* Open */
            if (ImGui::MenuItem("Open")) {
                if (cfe) {
                    if (cfe->is_dir) {
                        s_assets.current_path  = cfe->path;
                        s_assets.needs_refresh = true;
                        s_assets.selected_idx  = -1;
                    } else {
                        jce_file_viewer_open(cfe->path.c_str());
                    }
                }
            }

            /* Rename (F2) */
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.rename"), "F2")) {
                if (cfe) {
                    s_assets.renaming_idx = ci;
                    snprintf(s_assets.rename_buf, sizeof(s_assets.rename_buf),
                             "%s", cfe->name.c_str());
                }
            }

            /* Duplicate (Ctrl+D) */
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.duplicate"), "Ctrl+D")) {
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
            if (ImGui::MenuItem("Copy Path")) {
                if (cfe)
                    s_assets.clipboard_path = cfe->path;
            }

            /* Copy (Ctrl+C) */
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"), "Ctrl+C")) {
                if (cfe) {
                    s_assets.clipboard_path = cfe->path;
                    s_assets.clipboard_cut  = false;
                }
            }

            /* Cut (Ctrl+X) */
            if (ImGui::MenuItem("Cut", "Ctrl+X")) {
                if (cfe) {
                    s_assets.clipboard_path = cfe->path;
                    s_assets.clipboard_cut  = true;
                }
            }

            /* Paste (Ctrl+V) */
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V",
                                false, !s_assets.clipboard_path.empty()))
            {
                try {
                    fs::path src(s_assets.clipboard_path);
                    fs::path dst = fs::path(s_assets.current_path)
                                   / src.filename();
                    if (s_assets.clipboard_cut) {
                        fs::rename(src, dst);
                        s_assets.clipboard_path.clear();
                        jce_editor_console_log("Moved '%s'",
                            src.filename().string().c_str());
                    } else {
                        fs::copy(src, dst, fs::copy_options::recursive);
                        jce_editor_console_log("Pasted '%s'",
                            src.filename().string().c_str());
                    }
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Paste failed: %s", e.what());
                }
            }

            ImGui::Separator();

            /* New Folder */
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.newFolder"))) {
                try {
                    fs::path nf = fs::path(s_assets.current_path) / "New Folder";
                    fs::create_directory(nf);
                    jce_editor_console_log("Created folder 'New Folder'");
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "New folder failed: %s", e.what());
                }
            }

            ImGui::Separator();

            /* Delete (Del) */
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.delete"), "Del")) {
                if (cfe) {
                    try {
                        fs::remove_all(cfe->path);
                        jce_editor_console_log("Deleted '%s'", cfe->name.c_str());
                        s_assets.needs_refresh = true;
                        s_assets.selected_idx  = -1;
                    } catch (const std::exception &e) {
                        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                            "Delete failed: %s", e.what());
                    }
                }
            }

            /* Refresh */
            if (ImGui::MenuItem(jce_editor_i18n("assetBrowser.refresh"))) {
                s_assets.needs_refresh = true;
            }

            ImGui::EndPopup();
        }

        /* ── Keyboard shortcuts (when grid focused) ──────────────── */
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            int sel = s_assets.selected_idx;
            bool has_sel = (sel >= 0 && sel < (int)s_assets.entries.size());

            /* F2 - Rename */
            if (has_sel && ImGui::IsKeyPressed(ImGuiKey_F2)) {
                s_assets.renaming_idx = sel;
                snprintf(s_assets.rename_buf, sizeof(s_assets.rename_buf),
                         "%s", s_assets.entries[sel].name.c_str());
            }

            /* Delete */
            if (has_sel && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                try {
                    fs::remove_all(s_assets.entries[sel].path);
                    jce_editor_console_log("Deleted '%s'",
                        s_assets.entries[sel].name.c_str());
                    s_assets.needs_refresh = true;
                    s_assets.selected_idx  = -1;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Delete failed: %s", e.what());
                }
            }

            bool ctrl = ImGui::GetIO().KeyCtrl;

            /* Ctrl+C - Copy */
            if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
                s_assets.clipboard_path = s_assets.entries[sel].path;
                s_assets.clipboard_cut  = false;
            }

            /* Ctrl+X - Cut */
            if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_X)) {
                s_assets.clipboard_path = s_assets.entries[sel].path;
                s_assets.clipboard_cut  = true;
            }

            /* Ctrl+V - Paste */
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)
                && !s_assets.clipboard_path.empty())
            {
                try {
                    fs::path src(s_assets.clipboard_path);
                    fs::path dst = fs::path(s_assets.current_path)
                                   / src.filename();
                    if (s_assets.clipboard_cut) {
                        fs::rename(src, dst);
                        s_assets.clipboard_path.clear();
                        jce_editor_console_log("Moved '%s'",
                            src.filename().string().c_str());
                    } else {
                        fs::copy(src, dst, fs::copy_options::recursive);
                        jce_editor_console_log("Pasted '%s'",
                            src.filename().string().c_str());
                    }
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Paste failed: %s", e.what());
                }
            }

            /* Ctrl+D - Duplicate */
            if (has_sel && ctrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
                const FileEntry &fe = s_assets.entries[sel];
                try {
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
                    s_assets.needs_refresh = true;
                } catch (const std::exception &e) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Duplicate failed: %s", e.what());
                }
            }
        }
    }
    ImGui::EndChild();
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_assets(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS);
    if (!*vis) return;

    if (ImGui::Begin("Assets###AssetBrowser", vis))
        jce_editor_panel_assets_content();
    ImGui::End();
}
