/*
 * jce_panel_assets_internal.h  Shared state for asset browser panel files.
 */

#ifndef JCE_PANEL_ASSETS_INTERNAL_H
#define JCE_PANEL_ASSETS_INTERNAL_H

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/platform/jce_host_shell.h>

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_defaults.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_layout.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "scene/jce_editor_scene_render.h"
#include "viewers/jce_file_viewer.h"

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

/* ── Asset Browser types ─────────────────────────────────────────── */

struct FileEntry {
    std::string name;
    std::string path;
    std::string ext;
    bool is_dir;
    uintmax_t size;
    std::string modified_at;
};

enum AssetBrowserViewMode {
    ASSET_BROWSER_VIEW_GRID,
    ASSET_BROWSER_VIEW_DETAILS
};

/* ── Asset Browser state ─────────────────────────────────────────── */

struct AssetBrowserState {
    std::string current_path;
    /* Real project root followed from Open Project / recent project. */
    std::string followed_project_root;
    /* Effective browser root. May be a temporary simulated root. */
    std::string project_root;
    bool project_root_simulated;
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
    /* Visual feedback timers (seconds, decay each frame). */
    float clipboard_flash_t;
    float paste_flash_t;
    /* Per-entry-path "just touched" flash (path -> remaining seconds). */
    std::unordered_map<std::string, float> entry_flash;
    bool initialized;
    bool show_delete_confirm;
    bool show_delete_dialog_open;
    std::vector<std::string> pending_delete_paths;
    /* How many files reference what is about to be deleted, counted
     * when the confirm dialog is raised.  Deleting is legitimate;
     * deleting silently is the defect. */
    int                      pending_delete_refs = 0;
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
    AssetBrowserViewMode view_mode;
    /* Type filter (kind mask): 0=all, 1=images, 2=models, 3=audio, 4=text/code, 5=archives */
    int             kind_filter;
    /* Alpha-jump (Windows Explorer style): scroll to / select item starting with typed char. */
    int             jump_scroll_idx;   /* -1 = no pending scroll */
    char            jump_last_char;    /* last char used, for cycling */
    int             jump_next_start;   /* where to search from next time */
    double          jump_reset_time;   /* time of last jump, to detect cycling window */
};

extern AssetBrowserState s_assets;

/* ── Shared helpers ───────────────────────────────────────────────── */

std::string normalized_path_string(const std::string &p);
void        ensure_assets_init(void);
void        refresh_entries(void);
void        navigate_asset_directory(const std::string &path, bool clear_search);
bool        asset_browser_can_use_root(const std::string &path);
bool        set_asset_browser_simulated_root(const std::string &path);
bool        restore_asset_browser_project_root(void);
void        collect_search_results(const std::string &query);

ImVec4      asset_color_for_ext(const std::string &ext, bool is_dir);
const char *type_label_for_entry(const FileEntry &fe);

void copy_selection_to_clipboard(bool cut);
void copy_selection_from_view_to_clipboard(
    const std::vector<FileEntry> &view, bool cut);
void copy_path_to_clipboard(const std::string &path, bool cut);
void execute_clipboard_paste(void);
void collect_selected_for_deletion(void);
void collect_selected_from_view_for_deletion(
    const std::vector<FileEntry> &view);

/* ── Functions from jce_panel_assets_nav.cpp ──────────────────────── */

void draw_asset_directory_tree(float tree_w, float panel_h);
void draw_asset_breadcrumb_bar(void);
void draw_asset_search_bar(void);

/* ── Functions from jce_panel_assets_grid.cpp ─────────────────────── */

void draw_asset_grid_item(const FileEntry &fe, int index,
                          int cols, int &col, bool &want_ctx_popup);
void draw_asset_details_list(const std::vector<FileEntry> &display_entries,
                             bool &want_ctx_popup);
void draw_asset_item_context_menu(const std::vector<FileEntry> &display_entries);
void draw_asset_empty_area_menu(void);

#endif /* JCE_PANEL_ASSETS_INTERNAL_H */
