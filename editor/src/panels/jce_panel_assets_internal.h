/*
 * jce_panel_assets_internal.h  Shared state for asset browser panel files.
 */

#ifndef JCE_PANEL_ASSETS_INTERNAL_H
#define JCE_PANEL_ASSETS_INTERNAL_H

#include "jce_editor_panels.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"
#include "viewers/jce_file_viewer.h"
#include "jce_editor_state.h"
#include "scene/jce_editor_scene_render.h"
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

/* ── Asset Browser types ─────────────────────────────────────────── */

struct FileEntry {
    std::string name;
    std::string path;
    std::string ext;
    bool is_dir;
    uintmax_t size;
};

/* ── Asset Browser state ─────────────────────────────────────────── */

struct AssetBrowserState {
    std::string current_path;
    std::string project_root;
    std::vector<FileEntry> entries;
    std::set<int> selected_set;
    int last_clicked_idx;
    bool needs_refresh;
    int renaming_idx;
    char rename_buf[256];
    bool rename_focus_needed;
    int suppress_empty_ctx_frames;
    int context_idx;
    std::vector<std::string> clipboard_paths;
    bool clipboard_cut;
    bool initialized;
    bool show_delete_confirm;
    bool show_delete_dialog_open;
    std::vector<std::string> pending_delete_paths;
    std::vector<std::string> pending_delete_names;
    int pending_delete_dir_count;
    char search_buf[128];
    bool search_active;
    std::vector<FileEntry> search_results;
    std::string last_search_query;
    std::string last_search_root;
    std::string pending_navigation_path;
    bool pending_navigation_clear_search;
    double next_auto_refresh_time;
};

extern AssetBrowserState s_assets;

/* ── Shared helpers ───────────────────────────────────────────────── */

std::string normalized_path_string(const fs::path &p);
void        ensure_assets_init(void);
void        refresh_entries(void);
void        navigate_asset_directory(const std::string &path, bool clear_search);
void        collect_search_results(const std::string &query);

ImVec4      asset_color_for_ext(const std::string &ext, bool is_dir);
const char *type_label_for_entry(const FileEntry &fe);

void copy_selection_to_clipboard(bool cut);
void execute_clipboard_paste(void);
void collect_selected_for_deletion(void);

/* ── Functions from jce_panel_assets_nav.cpp ──────────────────────── */

void draw_asset_directory_tree(float tree_w, float panel_h);
void draw_asset_breadcrumb_bar(void);
void draw_asset_search_bar(void);

/* ── Functions from jce_panel_assets_grid.cpp ─────────────────────── */

void draw_asset_grid_item(const FileEntry &fe, int index,
                          int cols, int &col, bool &want_ctx_popup);
void draw_asset_item_context_menu(const std::vector<FileEntry> &display_entries);
void draw_asset_empty_area_menu(void);

#endif /* JCE_PANEL_ASSETS_INTERNAL_H */
