/*
 * jce_editor_menu_bar.cpp — the editor's main menu bar.
 *
 * Split out of jce_editor_layout.cpp, which was 3,461 lines and frozen at the
 * size gate's baseline: past the 3,000-line cap, so every addition had to be
 * paid for by a removal.  This function was 757 of those lines, and moving it
 * is what check_file_size.py's own message asks for -- "move the addition into
 * a new translation unit".
 *
 * What it shares with the layout TU is in jce_editor_layout_internal.h and is
 * deliberately small: the menu bar OPENS modals whose bodies the layout draws,
 * so ten flags cross the boundary along with six helpers.  Nothing else in
 * either file became visible.
 */

#include "jce_editor_layout.h"
#include "dialogs/jce_editor_dialogs.h"
#include "dialogs/jce_dialog_asset_picker.h"
#include "core/jce_editor.h"
#include "core/jce_editor_automation.h"
#include "jce_editor_colors.h"
#include "jce_editor_layout_scene_commands.h"
#include "jce_editor_welcome_policy.h"
#include "core/jce_editor_defaults.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_script_backends.h"
#include "core/jce_editor_project.h"
#include "jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_config.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_timer.h>
#include <jce/application/jce_screenshot.h>
#include "scene/jce_editor_scene_render.h"   /* jce_editor_get_renderer */
#include "core/jce_editor_recorder.h"        /* F9 VP9/WebM recorder */
#include "core/jce_editor_toast.h"
#include "core/jce_hotkeys.h"
#include "scene/jce_editor_game_render.h"   /* jce_editor_game_render_is_mouse_captured */
#include "core/jce_workspace.h"
#include "panels/jce_panel_common.h"         /* multi-select duplicate / delete */
#include "panels/jce_panel_preferences.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include "scene/jce_editor_scene_render.h"
#include <jce/middleware/scene/jce_lod.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <cstdlib>   /* getenv (JCE_DBG_FOCUS_SCENE QA hook) */
#include <jce/tools/jce_imgui.hpp>
#include <jce/tools/jce_imgui_internal.h>
#include <stdio.h>
#include <string.h>
#include "jce_editor_layout_internal.h"

void draw_menu_bar(void)
{
    /* P8-E v20: keep Window menu open across clicks AND focus target panel.
     *
     * Strategy: never call SetWindowFocus from a menu item — that mutates
     * NavWindow and ImGui's next frame collapses the popup chain via
     * ClosePopupsOverWindow. Instead we record the target window name and
     * each frame try ImGuiTabBar::NextSelectedTabId on its dock node's
     * tab bar. That's the same field ImGui's own tab-click code writes,
     * so it activates the tab cleanly without touching navigation state.
     *
     * First-time-open panels need 1-2 frames to be created and re-attached
     * to the dockspace; we retry up to 30 frames. A floating (un-docked)
     * panel has no tab bar so we fall back to SetWindowFocus once the TTL
     * expires — at that point the menu has likely been dismissed anyway. */
    pump_pending_panel_focus();
    auto focus_dock_tab = [](const char *name) {
        jce_editor_panel_request_focus(name);
    };

#define JCE_OPEN_WB(host_enum, host_id, req_fn, idx)                       \
    do {                                                                   \
        bool *_vis = jce_editor_panel_visible_ptr(host_enum);              \
        if (_vis) *_vis = true;                                            \
        req_fn(idx);                                                       \
        focus_dock_tab(host_id);                                           \
    } while (0)

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    if (!ImGui::BeginMenuBar()) {
        ImGui::PopStyleVar();
        return;
    }

    /* Match requested larger row spacing and overall bar height. */
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(18, 8));

    /* Disable entire menu bar when a modal dialog is open. */
    bool dialog_active = should_block_editor_interaction();
    if (dialog_active) ImGui::BeginDisabled(true);

    /* ── File ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.file"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.newScene"),    "Ctrl+N"))
            request_gated_action(PGA_NEW_SCENE);
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.openScene"),   "Ctrl+O"))
            request_gated_action(PGA_OPEN_SCENE);
        /* Recent Scenes submenu — fed from JceEditorConfig.recent_scene_paths.
         * Missing files are grayed out (with a (missing) suffix) and clicking
         * them removes them from the list. */
        {
            JceEditorConfig _ecfg;
            (void)jce_editor_config_load(&_ecfg);
            bool has_any = (_ecfg.recent_scene_count > 0);
            if (ImGui::BeginMenu(jce_editor_i18n("menu.file.openRecentScene"), has_any)) {
                int remove_idx = -1;
                /* Display order: scenes under the CURRENT project first, then
                 * a separator, then the rest (cross-project jump list stays
                 * whole — this is purely presentational).  Storage untouched. */
                extern char s_current_project_root[512];
                int order[20]; int n_order = 0; int n_first = 0;
                size_t root_len = strlen(s_current_project_root);
                auto under_project = [&](const char *p) -> bool {
                    if (root_len == 0) return false;
                    return strncmp(p, s_current_project_root, root_len) == 0;
                };
                for (int pass = 0; pass < 2; pass++) {
                    for (int i = 0; i < _ecfg.recent_scene_count &&
                                    n_order < (int)(sizeof(order)/sizeof(order[0])); i++) {
                        const char *p = _ecfg.recent_scene_paths[i];
                        if (!p || !p[0]) continue;
                        bool in_proj = under_project(p);
                        if ((pass == 0) == in_proj) order[n_order++] = i;
                    }
                    if (pass == 0) n_first = n_order;   /* in-project group size */
                }
                for (int oi = 0; oi < n_order; oi++) {
                    /* Separator between the current-project group and the rest. */
                    if (oi == n_first && n_first > 0 && n_first < n_order)
                        ImGui::Separator();
                    int i = order[oi];
                    const char *p = _ecfg.recent_scene_paths[i];
                    if (!p || !p[0]) continue;
                    bool exists = jce_fs_host_exists_file(p);
                    char label[600];
                    if (exists) {
                        snprintf(label, sizeof(label), "%s", p);
                    } else {
                        snprintf(label, sizeof(label), "%s %s",
                                 p, jce_editor_i18n("menu.file.recentMissing"));
                    }
                    if (!exists) ImGui::BeginDisabled(true);
                    if (ImGui::MenuItem(label)) {
                        /* Gate the discard behind the unsaved-changes modal;
                         * the path is copied into the pending-action buffer
                         * so the deferred load is safe across frames. */
                        request_gated_action(PGA_OPEN_RECENT_SCENE, p);
                    }
                    if (!exists) {
                        ImGui::EndDisabled();
                        /* Right-click to purge missing entries. */
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            remove_idx = i;
                    }
                }
                if (remove_idx >= 0) {
                    for (int j = remove_idx; j < _ecfg.recent_scene_count - 1; j++) {
                        strncpy(_ecfg.recent_scene_paths[j],
                                _ecfg.recent_scene_paths[j + 1],
                                sizeof(_ecfg.recent_scene_paths[j]) - 1);
                        _ecfg.recent_scene_paths[j][sizeof(_ecfg.recent_scene_paths[j]) - 1] = '\0';
                    }
                    _ecfg.recent_scene_count--;
                    if (_ecfg.recent_scene_count >= 0 && _ecfg.recent_scene_count < 10)
                        _ecfg.recent_scene_paths[_ecfg.recent_scene_count][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(jce_editor_i18n("menu.file.clearRecent"))) {
                    _ecfg.recent_scene_count = 0;
                    for (int i = 0; i < 10; i++) _ecfg.recent_scene_paths[i][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::EndMenu();
            }
            /* Recent Projects submenu — same shape, but switching projects
             * goes through the Open Project dialog which performs the
             * restart-required checks. We pre-fill the path field. */
            bool has_any_proj = (_ecfg.recent_count > 0);
            if (ImGui::BeginMenu(jce_editor_i18n("menu.file.openRecentProject"), has_any_proj)) {
                int remove_idx = -1;
                for (int i = 0; i < _ecfg.recent_count; i++) {
                    const char *p = _ecfg.recent_projects[i];
                    if (!p || !p[0]) continue;
                    bool exists = jce_fs_host_exists_dir(p);
                    char label[600];
                    if (exists) {
                        snprintf(label, sizeof(label), "%s", p);
                    } else {
                        snprintf(label, sizeof(label), "%s %s",
                                 p, jce_editor_i18n("menu.file.recentMissing"));
                    }
                    if (!exists) ImGui::BeginDisabled(true);
                    if (ImGui::MenuItem(label)) {
                        request_gated_action(PGA_OPEN_RECENT_PROJECT, p);
                    }
                    if (!exists) {
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            remove_idx = i;
                    }
                }
                if (remove_idx >= 0) {
                    for (int j = remove_idx; j < _ecfg.recent_count - 1; j++) {
                        strncpy(_ecfg.recent_projects[j],
                                _ecfg.recent_projects[j + 1],
                                sizeof(_ecfg.recent_projects[j]) - 1);
                        _ecfg.recent_projects[j][sizeof(_ecfg.recent_projects[j]) - 1] = '\0';
                    }
                    _ecfg.recent_count--;
                    if (_ecfg.recent_count >= 0 && _ecfg.recent_count < 10)
                        _ecfg.recent_projects[_ecfg.recent_count][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(jce_editor_i18n("menu.file.clearRecent"))) {
                    _ecfg.recent_count = 0;
                    for (int i = 0; i < 10; i++) _ecfg.recent_projects[i][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::EndMenu();
            }
        }
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.saveScene"),   "Ctrl+S"))
            save_scene_or_open_save_as();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.saveAs"),   "Ctrl+Shift+S"))
            s_show_save_as = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.new")))
            s_show_new_project = true;
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.open")))
            request_gated_action(PGA_OPEN_PROJECT);
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.buildSettings"), "Ctrl+B"))
            s_show_build = true;
        if (ImGui::MenuItem(jce_editor_i18n_or("menu.file.buildBundles",
                                                "Build Bundles..."),
                            nullptr))
            s_show_bundles = true;
        if (ImGui::MenuItem(jce_editor_i18n_or("menu.file.packCurrentScene",
                                                "Pack Current Scene as Bundle"),
                            "Ctrl+Shift+B"))
            cmd_pack_current_scene_();
        if (ImGui::MenuItem(jce_editor_i18n_or("menu.file.openBundle",
                                                "Open Bundle..."),
                            nullptr))
            s_show_open_bundle = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n_or("menu.file.project_settings",
                                                "Project Settings..."),
                            "Ctrl+Shift+P")) {
            bool *v = jce_editor_panel_visible_ptr(JCE_PANEL_PROJECT_SETTINGS);
            if (v) *v = true;
            jce_editor_panel_request_focus("###project_settings");
        }
        /* RELOAD NATIVE SCRIPT MODULES.
         *
         * A C++ gameplay class lives in a .jcec the PROJECT builds, and the
         * ordinary project-open reload deliberately KEEPS a module it already
         * holds -- so rebuilding one left the editor running yesterday's code
         * until the project was closed and reopened.
         *
         * DISABLED RATHER THAN REFUSED IN A LOG, because the precondition is
         * the interesting part: unloading a module with live instances would
         * unmap code those instances dispatch into, and the only thing that
         * creates one is the runtime script system during Play.  Stopping Play
         * IS the unload-safe point, so the menu says that where the user is
         * looking instead of after they have clicked. */
        {
            extern char s_current_project_root[512];
            const bool has_project = s_current_project_root[0] != 0;
            const bool stopped = jce_state_get_play_state() == JCE_PLAY_STOPPED;
            const bool can = has_project && stopped;
            if (!can) ImGui::BeginDisabled(true);
            if (ImGui::MenuItem(jce_editor_i18n_or(
                    "menu.file.reloadNativeScripts",
                    "Reload Native Script Modules"))) {
                jce_editor_script_modules_reload_native(s_current_project_root);
            }
            if (!can) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", jce_editor_i18n_or(
                    !has_project ? "menu.file.reloadNativeScripts.noProject"
                                 : (!stopped
                                        ? "menu.file.reloadNativeScripts.playing"
                                        : "menu.file.reloadNativeScripts.tip"),
                    !has_project
                        ? "Open a project first: a native script module is "
                          "built by the project, not by the editor."
                        : (!stopped
                               ? "Stop Play first. Unloading a module while "
                                 "script instances are live would unmap code "
                                 "they dispatch into."
                               : "Unload and re-load this project's .jcec "
                                 "modules from disk, so a rebuilt C++ script "
                                 "takes effect without reopening the project.")));
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.exit"), "Alt+F4"))
            jce_editor_layout_request_quit();
        ImGui::EndMenu();
    }

    /* ── Edit ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.edit"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.undo"),  "Ctrl+Z", false, jce_state_can_undo()))
            jce_state_undo();
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.redo"),
                            "Ctrl+Y / Ctrl+Shift+Z", false, jce_state_can_redo()))
            jce_state_redo();
        ImGui::Separator();
        {
            int sel_count = 0;
            const uint32_t *sel = jce_state_get_selection(&sel_count);
            uint32_t focused = jce_state_get_focused();
            bool has_target = (sel_count > 0) || (focused != 0);
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"),  "Ctrl+C", false, has_target)) {
                if (sel_count > 0)        jce_state_copy_entities(sel, sel_count, false);
                else if (focused)         jce_state_copy_entity(focused);
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.cut"),   "Ctrl+X", false, has_target)) {
                if (sel_count > 0)        jce_state_copy_entities(sel, sel_count, true);
                else if (focused)         jce_state_copy_entities(&focused, 1, true);
            }
            bool can_paste = jce_state_clipboard_count() > 0 || jce_state_has_copied();
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false, can_paste)) {
                uint32_t parent = focused; /* paste under focused if any, else root */
                if (jce_state_clipboard_count() > 0) {
                    uint32_t new_ids[64];
                    int n = jce_state_paste_entities(parent, new_ids, 64);
                    if (n > 0) jce_state_select_entity(new_ids[0], false);
                    for (int i = 1; i < n; ++i) jce_state_select_entity(new_ids[i], true);
                } else if (jce_state_has_copied()) {
                    uint32_t nid = jce_state_paste_entity(parent);
                    if (nid) jce_state_select_entity(nid, false);
                }
            }
        }
        /* Same body as the Ctrl+D the item advertises — it used to
         * duplicate only the focused entity, silently ignoring the rest of
         * a multi-selection. */
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.duplicate"), "Ctrl+D"))
            jce_panel_duplicate_selection();
        {
            int sc = 0;
            jce_state_get_selection(&sc);
            const bool can_snap = sc > 0 || jce_state_get_focused() != 0;
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.snapToGround"),
                                "End", false, can_snap))
                jce_scene_view_snap_selection_to_ground();
        }
        /* Likewise: the item advertises Del / Backspace, so it must offer
         * the same whole-selection delete those keys perform. */
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.delete"), "Del / Backspace"))
            jce_panel_delete_selection();
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.preferences"), "Ctrl+,")) {
            bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_USER_PREFERENCES);
            if (vis) *vis = true;
        }
        /* Project Settings moved to File menu (Ctrl+Shift+P) to match
         * Unity / Unreal convention; do not duplicate here. */
        ImGui::Separator();
        {
            /* The shortcut is READ, not typed.  This line said "F5" while F5
             * was bound to Play / Toggle, so the menu told the user a key that
             * did something else -- and a rebind in the hotkey editor could
             * never have corrected it. */
            char sc[64];
            jce_hotkey_chord_label(jce_hotkey_get(JCE_HK_SHADERS_RELOAD),
                                   sc, sizeof(sc));
            if (ImGui::MenuItem(jce_editor_i18n("shaders.reload"), sc))
                jce_editor_reload_shaders();
        }
        ImGui::EndMenu();
    }

    /* ── GameObject (was "Create") ─────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.gameObject"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createEmpty"))) {
            uint32_t id = jce_state_create_entity("New Entity", 0);
            jce_state_select_entity(id, false);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(jce_editor_i18n("menu.gameObject.3dObject"))) {
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCube"))) {
                uint32_t id = jce_state_create_entity("Cube", 0);
                jce_state_select_entity(id, false);
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere"))) {
                uint32_t id = jce_state_create_entity("Sphere", 0);
                jce_state_select_entity(id, false);
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane"))) {
                uint32_t id = jce_state_create_entity("Plane", 0);
                jce_state_select_entity(id, false);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera"))) {
            uint32_t id = jce_state_create_entity("Camera", 0);
            jce_state_select_entity(id, false);
        }
        if (ImGui::BeginMenu(jce_editor_i18n("menu.gameObject.createLight"))) {
            if (ImGui::MenuItem(jce_editor_i18n("light.directional"))) {
                uint32_t id = jce_state_create_entity("Directional Light", 0);
                jce_state_select_entity(id, false);
            }
            if (ImGui::MenuItem(jce_editor_i18n("light.point"))) {
                uint32_t id = jce_state_create_entity("Point Light", 0);
                jce_state_select_entity(id, false);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    /* ── Window (was "View") ───────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.window"))) {
        /* P8-E v20: keep the Window menu open across clicks. MenuItems
         * default to closing their parent popup on activation; flip the
         * AutoClosePopups item-flag off so the user can toggle multiple
         * panels in one session. Focus on each click is queued via
         * focus_dock_tab and applied next frame on the dock-node tab bar
         * (no SetWindowFocus → no popup collapse). */
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);

        auto panel_toggle = [&focus_dock_tab](const char *label, JceEditorPanel kind,
                               const char *window_id,
                               const char *accel = NULL) {
            bool *vis = jce_editor_panel_visible_ptr(kind);
            const bool clicked = ImGui::MenuItem(label, accel, vis);
            if (clicked && vis && *vis) focus_dock_tab(window_id);
        };
        /* P8-A: Window menu re-organised around the 8 Workbenches +
         * primary panels. */

        /* Core panels (real, not shims) */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.core"))) {
            panel_toggle(jce_editor_i18n("Hierarchy"),     JCE_PANEL_HIERARCHY,   "###hierarchy");
            panel_toggle(jce_editor_i18n("Inspector"),     JCE_PANEL_INSPECTOR,   "###inspector");
            panel_toggle(jce_editor_i18n("Console"),       JCE_PANEL_CONSOLE,     "###console");
            panel_toggle(jce_editor_i18n("Asset Browser"), JCE_PANEL_ASSETS,      "###assets");
            panel_toggle(jce_editor_i18n("File Viewer"),   JCE_PANEL_FILE_VIEWER, "###file_viewer");
            ImGui::EndMenu();
        }

        /* Scene / viewport panels */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.scene"))) {
            panel_toggle(jce_editor_i18n("Scene"), JCE_PANEL_SCENE_VIEW, "###scene_view");
            panel_toggle(jce_editor_i18n("Game"),  JCE_PANEL_GAME_VIEW,  "###game_view");
            /* i18n_or rather than a new key: sort_i18n.py --check --strict
             * fails CI if en.json defines a key the other 13 locales lack. */
            {
                bool gmax = jce_editor_panel_is_maximized(JCE_PANEL_GAME_VIEW);
                if (ImGui::MenuItem(jce_editor_i18n("window.game.maximize"),
                                    "Shift+F11", &gmax))
                    jce_editor_panel_set_maximized(JCE_PANEL_GAME_VIEW, gmax);
            }
            ImGui::EndMenu();
        }

        /* Workbenches (7) — host toggle + flat tool list with section headers.
         * P8-E v13:
         *   - Clicking the host MenuItem focuses the host window when enabled.
         *   - Tool section (section label + items) is hidden when the host
         *     is not visible — avoids dangling shortcuts to a closed panel.
         *   - Standard ImGui::MenuItem widget keeps 1/2/3-level visuals and
         *     click behavior identical; the AutoClosePopups=false wrapper
         *     above keeps the menu open across consecutive clicks. */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.workbenches"))) {
            const float indent_w = ImGui::GetFontSize();

            /* host_item: same as panel_toggle, but also returns the host
             * visibility so callers can conditionally render the tools
             * section. */
            auto host_item = [&panel_toggle](const char *label, JceEditorPanel kind,
                                             const char *host_id) -> bool {
                panel_toggle(label, kind, host_id);
                bool *vis = jce_editor_panel_visible_ptr(kind);
                return vis ? *vis : false;
            };

            /* W1 Animation */
            if (host_item(jce_editor_i18n("window.animationEditor"),
                          JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.animation.tools"));
                ImGui::Indent(indent_w);
                const int ae_tab = jce_panel_animation_editor_current_tab();
                if (ImGui::MenuItem(jce_editor_i18n("window.animatorSM"),       NULL, ae_tab == 1)) JCE_OPEN_WB(JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor", jce_panel_animation_editor_request_tab, 1);
                if (ImGui::MenuItem(jce_editor_i18n("window.curveEditor"),      NULL, ae_tab == 2)) JCE_OPEN_WB(JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor", jce_panel_animation_editor_request_tab, 2);
                if (ImGui::MenuItem(jce_editor_i18n("window.sequencer"),        NULL, ae_tab == 3)) JCE_OPEN_WB(JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor", jce_panel_animation_editor_request_tab, 3);
                if (ImGui::MenuItem(jce_editor_i18n("Timeline"),                NULL, ae_tab == 4)) JCE_OPEN_WB(JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor", jce_panel_animation_editor_request_tab, 4);
                if (ImGui::MenuItem(jce_editor_i18n("window.animationRigging"), NULL, ae_tab == 5)) JCE_OPEN_WB(JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor", jce_panel_animation_editor_request_tab, 5);
                ImGui::Unindent(indent_w);
            }
            ImGui::Separator();

            /* W2 Profiling */
            if (host_item(jce_editor_i18n("window.profiler"),
                          JCE_PANEL_PROFILER, "###profiler")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.profiling.tools"));
                ImGui::Indent(indent_w);
                const int pr_tab = jce_panel_profiler_current_tab();
                if (ImGui::MenuItem(jce_editor_i18n("window.memoryProfiler"),  NULL, pr_tab == 1)) JCE_OPEN_WB(JCE_PANEL_PROFILER, "###profiler", jce_panel_profiler_request_tab, 1);
                if (ImGui::MenuItem(jce_editor_i18n("window.profileAnalyzer"), NULL, pr_tab == 2)) JCE_OPEN_WB(JCE_PANEL_PROFILER, "###profiler", jce_panel_profiler_request_tab, 2);
                if (ImGui::MenuItem(jce_editor_i18n("frameDebugger.title"),    NULL, pr_tab == 3)) JCE_OPEN_WB(JCE_PANEL_PROFILER, "###profiler", jce_panel_profiler_request_tab, 3);
                ImGui::Unindent(indent_w);
            }
            panel_toggle(jce_editor_i18n("window.physicsDebugger"),
                         JCE_PANEL_PHYSICS_DEBUGGER, "###physics_debugger");
            ImGui::Separator();

            /* W3 Rendering (Lighting Settings host) */
            if (host_item(jce_editor_i18n("panel.lighting.title"),
                          JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.rendering.tools"));
                ImGui::Indent(indent_w);
                const int ls_tab = jce_panel_lighting_settings_current_tab();
                const int ls_inn = jce_panel_lighting_settings_current_inner_tab();
                if (ImGui::MenuItem(jce_editor_i18n("postfx.title"),                NULL, ls_tab == 1)) JCE_OPEN_WB(JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings", jce_panel_lighting_settings_request_tab, 1);
                if (ImGui::MenuItem(jce_editor_i18n("window.lightmapBake"),         NULL, ls_tab == 2)) JCE_OPEN_WB(JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings", jce_panel_lighting_settings_request_tab, 2);
                if (ImGui::MenuItem(jce_editor_i18n("window.reflectionProbes"),     NULL, ls_tab == 3)) JCE_OPEN_WB(JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings", jce_panel_lighting_settings_request_tab, 3);
                if (ImGui::MenuItem(jce_editor_i18n("panel.render_pipeline.title"), NULL, ls_tab == 4)) JCE_OPEN_WB(JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings", jce_panel_lighting_settings_request_tab, 4);
                /* Time of Day & Light Explorer are inner tabs inside outer Lighting tab (idx=0). */
                if (ImGui::MenuItem(jce_editor_i18n("window.timeOfDay"),     NULL, ls_tab == 0 && ls_inn == 1)) {
                    bool *_v = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS);
                    if (_v) *_v = true;
                    jce_panel_lighting_settings_request_tab(0);
                    jce_editor_lighting_settings_focus_tab_time_of_day();
                    focus_dock_tab("###lighting_settings");
                }
                if (ImGui::MenuItem(jce_editor_i18n("window.lightExplorer"), NULL, ls_tab == 0 && ls_inn == 2)) {
                    bool *_v = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS);
                    if (_v) *_v = true;
                    jce_panel_lighting_settings_request_tab(0);
                    jce_editor_lighting_settings_focus_tab_light_explorer();
                    focus_dock_tab("###lighting_settings");
                }
                ImGui::Unindent(indent_w);
            }
            ImGui::Separator();

            /* W4 Graph Authoring */
            if (host_item(jce_editor_i18n("window.materialGraph"),
                          JCE_PANEL_MATERIAL_GRAPH, "###jce_material_graph")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.graphs.tools"));
                ImGui::Indent(indent_w);
                const int mg_tab = jce_panel_material_graph_current_tab();
                if (ImGui::MenuItem(jce_editor_i18n("window.shaderGraph"),    NULL, mg_tab == 1)) JCE_OPEN_WB(JCE_PANEL_MATERIAL_GRAPH, "###jce_material_graph", jce_panel_material_graph_request_tab, 1);
                if (ImGui::MenuItem(jce_editor_i18n("vfxGraph.title"),        NULL, mg_tab == 2)) JCE_OPEN_WB(JCE_PANEL_MATERIAL_GRAPH, "###jce_material_graph", jce_panel_material_graph_request_tab, 2);
                if (ImGui::MenuItem(jce_editor_i18n("window.particleEditor"), NULL, mg_tab == 3)) JCE_OPEN_WB(JCE_PANEL_MATERIAL_GRAPH, "###jce_material_graph", jce_panel_material_graph_request_tab, 3);
                if (ImGui::MenuItem(jce_editor_i18n("window.shaderInspector"),NULL, mg_tab == 4)) JCE_OPEN_WB(JCE_PANEL_MATERIAL_GRAPH, "###jce_material_graph", jce_panel_material_graph_request_tab, 4);
                ImGui::Unindent(indent_w);
            }
            ImGui::Separator();

            /* W5 Asset Pipeline */
            if (host_item(jce_editor_i18n("window.bundleBrowser"),
                          JCE_PANEL_BUNDLE_BROWSER, "###bundle_browser")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.assets.tools"));
                ImGui::Indent(indent_w);
                const int bb_tab = jce_panel_bundle_browser_current_tab();
                if (ImGui::MenuItem(jce_editor_i18n("window.importPresets"), NULL, bb_tab == 1)) JCE_OPEN_WB(JCE_PANEL_BUNDLE_BROWSER, "###bundle_browser", jce_panel_bundle_browser_request_tab, 1);
                if (ImGui::MenuItem(jce_editor_i18n("packageManager.title"), NULL, bb_tab == 2)) JCE_OPEN_WB(JCE_PANEL_BUNDLE_BROWSER, "###bundle_browser", jce_panel_bundle_browser_request_tab, 2);
                ImGui::Unindent(indent_w);
            }
            ImGui::Separator();

            /* W6 Network */
            if (host_item(jce_editor_i18n("panel.network_stats.title"),
                          JCE_PANEL_NETWORK_STATS, "###network_stats")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.network.tools"));
                ImGui::Indent(indent_w);
                const int ns_tab = jce_panel_network_stats_current_tab();
                if (ImGui::MenuItem(jce_editor_i18n("panel.lan_discovery.title"), NULL, ns_tab == 1)) JCE_OPEN_WB(JCE_PANEL_NETWORK_STATS, "###network_stats", jce_panel_network_stats_request_tab, 1);
                ImGui::Unindent(indent_w);
            }
            ImGui::Separator();

            /* W7 Build */
            if (host_item(jce_editor_i18n("buildProfiles.title"),
                          JCE_PANEL_BUILD_PROFILES, "###build_profiles")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.build.tools"));
                ImGui::Indent(indent_w);
                const int bp_tab = jce_panel_build_profiles_current_tab();
                if (ImGui::MenuItem(jce_editor_i18n("window.buildReport"), NULL, bp_tab == 1)) JCE_OPEN_WB(JCE_PANEL_BUILD_PROFILES, "###build_profiles", jce_panel_build_profiles_request_tab, 1);
                ImGui::Unindent(indent_w);
            }
            ImGui::Separator();

            /* W8 Audio */
            if (host_item(jce_editor_i18n("audioMixer.title"),
                          JCE_PANEL_AUDIO_MIXER, "###audio_mixer")) {
                ImGui::TextDisabled("%s", jce_editor_i18n("window.workbench.audio.tools"));
                ImGui::Indent(indent_w);
                const int audio_tab = jce_editor_audio_mixer_current_tab();
                if (ImGui::MenuItem(jce_editor_i18n("audioMixer.tab.mixer"),
                                    NULL, audio_tab == 0)) {
                    bool *_v = jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER);
                    if (_v) *_v = true;
                    jce_editor_audio_mixer_focus_mixer_tab();
                    focus_dock_tab("###audio_mixer");
                }
                if (ImGui::MenuItem(jce_editor_i18n("window.reverbZones"),
                                    NULL, audio_tab == 2)) {
                    bool *_v = jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER);
                    bool *_rv = jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES);
                    if (_v) *_v = true;
                    if (_rv) *_rv = false;
                    jce_editor_audio_mixer_focus_reverb_tab();
                    focus_dock_tab("###audio_mixer");
                }
                ImGui::Unindent(indent_w);
            }

            ImGui::EndMenu();
        }

        /* World */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.world"))) {
            panel_toggle(jce_editor_i18n("window.terrain"), JCE_PANEL_TERRAIN, "###jce_terrain");
            panel_toggle(jce_editor_i18n("window.navmesh"), JCE_PANEL_NAVMESH, "###jce_navmesh");
            panel_toggle(jce_editor_i18n("window.btVisualizer"), JCE_PANEL_BT_VISUALIZER, "###bt_visualizer");
            panel_toggle(jce_editor_i18n("window.worldStreaming"), JCE_PANEL_WORLD_STREAMING, "###world_streaming");
            ImGui::EndMenu();
        }

        /* Authoring tools — non-Workbench standalones */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.tools"))) {
            panel_toggle(jce_editor_i18n("window.aiAssistant"), JCE_PANEL_AI_ASSISTANT, "###ai_assistant");
            panel_toggle(jce_editor_i18n("inputManager.title"),  JCE_PANEL_INPUT_MANAGER,  "###input_manager");
            panel_toggle(jce_editor_i18n("spriteEditor.title"),  JCE_PANEL_SPRITE_EDITOR,  "###sprite_editor");
            panel_toggle(jce_editor_i18n("tilePalette.title"),   JCE_PANEL_TILE_PALETTE,   "###tile_palette");
            panel_toggle(jce_editor_i18n("window.vcamManager"),  JCE_PANEL_VCAM_MANAGER,   "###vcam_manager");
            panel_toggle(jce_editor_i18n("window.saveBrowser"),  JCE_PANEL_SAVE_BROWSER,   "###save_browser");
            panel_toggle(jce_editor_i18n("testRunner.title"),    JCE_PANEL_TEST_RUNNER,    "###test_runner");
            panel_toggle(jce_editor_i18n("window.systems"),      JCE_PANEL_SYSTEMS,        "###systems");
            panel_toggle(jce_editor_i18n("window.versionControl"), JCE_PANEL_VERSION_CONTROL, "###version_control");
            ImGui::Separator();
            panel_toggle(jce_editor_i18n("window.search"), JCE_PANEL_SEARCH, "###search", "Ctrl+K");
            panel_toggle(jce_editor_i18n("window.userGuide"), JCE_PANEL_USER_GUIDE, "###user_guide");
            /* Project Settings is a modal (P8-C). It lives under Edit and
             * File menus; not exposed as a dockable Window entry. */
            ImGui::EndMenu();
        }
        ImGui::Separator();
        /* Status Bar — visibility only, no dedicated focusable window.
           (The previous "Toolbar" entry was removed: its play controls
           duplicated the menu-bar right-aligned Play/Stop, and its
           gizmo radios duplicated the Scene viewport's inline toolbar.
           Q/W/E/R/X/Z hotkeys are still processed globally — see
           jce_editor_panel_toolbar_inline().) */
        ImGui::MenuItem(jce_editor_i18n("window.statusBar"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_STATUS_BAR));
        ImGui::Separator();
        if (ImGui::BeginMenu(jce_editor_i18n("window.layoutPresets"))) {
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.default")))   { s_layout_preset_pending = 0; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.wide")))      { s_layout_preset_pending = 1; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.animation"))) { s_layout_preset_pending = 2; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.twoByTwo"))){ s_layout_preset_pending = 3; s_reset_layout_requested = true; }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.programmer"))){ s_layout_preset_pending = 4; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.twoD")))      { s_layout_preset_pending = 5; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.mobilePortrait"))){ s_layout_preset_pending = 6; s_reset_layout_requested = true; }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.cinematic")))  { s_layout_preset_pending = 7; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.profiling")))  { s_layout_preset_pending = 8; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.lighting")))   { s_layout_preset_pending = 9; s_reset_layout_requested = true; }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(jce_editor_i18n("menu.window.resetLayout"))) {
            s_layout_preset_pending = 0;
            s_reset_layout_requested = true;
        }
        ImGui::PopItemFlag();
        ImGui::EndMenu();
    }

    /* ── Workspace-specific menu groups (Maya-style menu sets) ────────
     *
     * Each group is wrapped in a mask check so it only appears when the
     * active workspace requests it. The groups are placeholder shells —
     * one MenuItem per entry that toggles visibility of the most
     * relevant existing panel. Real authoring tools can fill in later
     * sub-items without changing the workspace plumbing.
     */
    {
        JceWorkspaceId ws_active = jce_workspace_get_active();
        const uint32_t active_bit = 1u << (unsigned)ws_active;

        const uint32_t MASK_MODELING  = 1u << (unsigned)JCE_WORKSPACE_MODELING;
        const uint32_t MASK_RIGGING   = 1u << (unsigned)JCE_WORKSPACE_RIGGING;
        const uint32_t MASK_ANIMATION = 1u << (unsigned)JCE_WORKSPACE_ANIMATION;
        const uint32_t MASK_FX        = 1u << (unsigned)JCE_WORKSPACE_FX;
        const uint32_t MASK_RENDERING = 1u << (unsigned)JCE_WORKSPACE_RENDERING;

        auto open_panel = [](JceEditorPanel p) {
            bool *v = jce_editor_panel_visible_ptr(p);
            if (v) *v = true;
        };

        if (active_bit & MASK_MODELING) {
            if (ImGui::BeginMenu(jce_editor_i18n("menu.modeling.label"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.modeling.mesh")))
                    open_panel(JCE_PANEL_SCENE_VIEW);
                ImGui::EndMenu();
            }
        }
        if (active_bit & MASK_RIGGING) {
            if (ImGui::BeginMenu(jce_editor_i18n("menu.rigging.label"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.rigging.skeleton")))
                    open_panel(JCE_PANEL_ANIMATION_RIGGING);
                if (ImGui::MenuItem(jce_editor_i18n("menu.rigging.skin")))
                    open_panel(JCE_PANEL_ANIMATION_RIGGING);
                ImGui::EndMenu();
            }
        }
        if (active_bit & MASK_ANIMATION) {
            if (ImGui::BeginMenu(jce_editor_i18n("menu.animation.label"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.animation.key")))
                    open_panel(JCE_PANEL_CURVE_EDITOR);
                if (ImGui::MenuItem(jce_editor_i18n("menu.animation.timeline")))
                    open_panel(JCE_PANEL_TIMELINE);
                ImGui::EndMenu();
            }
        }
        if (active_bit & MASK_FX) {
            if (ImGui::BeginMenu(jce_editor_i18n("menu.fx.label"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.fx.particles")))
                    open_panel(JCE_PANEL_PARTICLE_EDITOR);
                ImGui::EndMenu();
            }
        }
        if (active_bit & MASK_RENDERING) {
            if (ImGui::BeginMenu(jce_editor_i18n("menu.rendering.label"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.rendering.lighting")))
                    open_panel(JCE_PANEL_LIGHTING_SETTINGS);
                if (ImGui::MenuItem(jce_editor_i18n("menu.rendering.bake")))
                    open_panel(JCE_PANEL_LIGHTMAP_BAKE);
                if (ImGui::MenuItem(jce_editor_i18n("menu.rendering.postfx")))
                    open_panel(JCE_PANEL_POSTFX);
                ImGui::EndMenu();
            }
        }
    }

    /* ── Workspace dropdown (Maya-style) ──────────────────────────────
     *
     * Sits between Window and Debug. Maya-style with a dim prefix label,
     * separators on both sides, and chord hints in the dropdown rows.
     * Hotkeys Ctrl+Shift+1..7 cycle workspaces. */
    {
        JceWorkspaceId ws_active = jce_workspace_get_active();
        const JceWorkspaceDef *active_def = jce_workspace_def(ws_active);
        const char *active_label = active_def
            ? jce_editor_i18n(active_def->i18n_label_key)
            : "Default";

        ImGui::Separator();

        const char *prefix = jce_editor_i18n("menu.workspace.label");
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(prefix);
        ImGui::PopStyleColor();

        /* Compute widest entry so the combo box never clips. */
        float combo_w = ImGui::CalcTextSize(active_label).x;
        for (int i = 0; i < (int)JCE_WORKSPACE_COUNT; ++i) {
            const JceWorkspaceDef *d = jce_workspace_def((JceWorkspaceId)i);
            if (!d) continue;
            float w = ImGui::CalcTextSize(jce_editor_i18n(d->i18n_label_key)).x;
            if (w > combo_w) combo_w = w;
        }
        combo_w += ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 4.0f;

        ImGui::SetNextItemWidth(combo_w);
        if (ImGui::BeginCombo("##jce_workspace_combo", active_label)) {
            for (int i = 0; i < (int)JCE_WORKSPACE_COUNT; ++i) {
                const JceWorkspaceDef *d = jce_workspace_def((JceWorkspaceId)i);
                if (!d) continue;
                bool selected = ((int)ws_active == i);
                char row[128];
                if (i < 7)
                    snprintf(row, sizeof(row), "%s\tCtrl+Shift+%d",
                             jce_editor_i18n(d->i18n_label_key), i + 1);
                else
                    snprintf(row, sizeof(row), "%s",
                             jce_editor_i18n(d->i18n_label_key));
                if (ImGui::Selectable(row, selected))
                    jce_workspace_set_active((JceWorkspaceId)i);
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nCtrl+Shift+1..7", prefix);

        ImGui::Separator();
    }

    /* ── Tools ─────────────────────────────────────────────────────────
     *
     * REQ-ARCH-02's first call site: the editor and the CLI reaching ONE
     * Automation API rather than growing two implementations that agree.
     * physics.probe measures the open scene with the real solver -- something
     * the editor has never been able to do -- and it is READ-ONLY, so it needs
     * no edit scope: nothing changed, so there is nothing to undo.  A tool
     * that WRITES must be wrapped in jce_state_begin_batch_edit; the ownership
     * is stated in editor/src/core/AGENTS.md and
     * tools/lint/check_editor_automation_undo.py fails the build otherwise.
     */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.tools"))) {
        const bool busy = jce_editor_automation_is_running();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.validatePhysics")))
            jce_editor_automation_probe_current_scene();
        /* WRITES the project, through a changeset.  Not wrapped in an editor
         * edit scope on purpose: it changes a FILE, and the undo for the file
         * layer is changeset.rollback.  See core/jce_editor_automation.h. */
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.pinThresholds")))
            jce_editor_automation_pin_thresholds();
        ImGui::Separator();
        /* REQ-PHY-03: the automatic collider choice must come with its REASON.
         * Plan is READ-ONLY and prints the reasons to the Console; apply is a
         * separate item on purpose, so the reading happens between them.  One
         * item that did both would make the reason a thing printed after the
         * decision, which is not a review. */
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.authorPlan")))
            jce_editor_automation_author_plan();
        /* WRITES the project, through a changeset -- same ownership as
         * pinThresholds: it changes a FILE, so its undo is changeset.rollback
         * and not Ctrl+Z. */
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.authorApply")))
            jce_editor_automation_author_apply();
        /* The ragdoll pair.  Apply writes the SkeletalAnimator as well as the
         * Ragdoll, because the runtime spawn is gated on it -- a scene with
         * one and not the other has a ragdoll that never exists and says
         * nothing about it. */
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.ragdollPlan")))
            jce_editor_automation_ragdoll_plan();
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.ragdollApply")))
            jce_editor_automation_ragdoll_apply();
        ImGui::Separator();
        /* The AI-scene chain: a recipe is compiled by THE ENGINE'S OWN
         * compiler, so the editor and a headless run produce one plan with
         * one hash rather than two compilers that agree until they do not. */
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.compileRecipe")))
            jce_editor_automation_compile_recipe();
        if (ImGui::MenuItem(jce_editor_i18n("menu.tools.materialise")))
            jce_editor_automation_materialise();
        ImGui::Separator();
        /* ENUMERATED, not a hard-coded pair.  The door decides which languages
         * it has a template it can vouch for; a list repeated here would be a
         * second authority, and the first thing it would do is disagree. */
        if (ImGui::BeginMenu(jce_editor_i18n("menu.tools.newScript"))) {
            const int langs = jce_editor_automation_script_language_count();
            for (int i = 0; i < langs; ++i) {
                const char *lang = jce_editor_automation_script_language(i);
                if (ImGui::MenuItem(lang))
                    jce_editor_automation_script_new(lang);
            }
            ImGui::EndMenu();
        }
        if (busy) ImGui::EndDisabled();
        /* An open change is a decision the human owes, so it is shown even
         * when nothing is running -- and the commit names its cost, because
         * validate runs a real lint and a menu item that looks instant and
         * takes minutes is the same defect as a silent one. */
        const char *open_cs = jce_editor_automation_open_change();
        if (open_cs && open_cs[0]) {
            ImGui::Separator();
            ImGui::TextDisabled("%s %s",
                                jce_editor_i18n("menu.tools.openChange"), open_cs);
            if (busy) ImGui::BeginDisabled();
            if (ImGui::MenuItem(jce_editor_i18n("menu.tools.commitOpen")))
                jce_editor_automation_commit_open();
            if (ImGui::MenuItem(jce_editor_i18n("menu.tools.rollbackOpen")))
                jce_editor_automation_rollback_open();
            if (busy) ImGui::EndDisabled();
        }
        if (busy) {
            /* Named, not a spinner: which STEP is holding the one slot is the
             * question somebody staring at a disabled menu actually has -- and
             * a write is four calls, of which changeset.validate runs a real
             * lint and takes minutes.  "Busy" would be true and useless. */
            ImGui::Separator();
            ImGui::TextDisabled("%s %s", jce_editor_i18n("menu.tools.running"),
                                jce_editor_automation_current_tool());
            ImGui::TextDisabled("%s", jce_editor_i18n("menu.tools.slowStep"));
        }
        ImGui::EndMenu();
    }

    /* ── Help ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.debug"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.debug.toggleDemoLod"),
                            nullptr, s_demo_lod_enabled)) {
            cmd_toggle_demo_lod_();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(jce_editor_i18n("menu.help"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.help.welcome")))
            s_show_welcome = true;
        if (ImGui::MenuItem(jce_editor_i18n("menu.help.guide"))) {
            *jce_editor_panel_visible_ptr(JCE_PANEL_USER_GUIDE) = true;
            jce_editor_panel_request_focus("###user_guide");
        }
        if (ImGui::MenuItem(jce_editor_i18n("menu.help.about")))
            s_show_about = true;
        ImGui::EndMenu();
    }

    /* ── Play controls (right-aligned, matches reference) ──────────── */
    {
        /* Calculate width of play + stop buttons + spacing for right-alignment. */
        float btn_w = ImGui::CalcTextSize(" > ").x + ImGui::GetStyle().FramePadding.x * 2
                    + ImGui::GetStyle().ItemSpacing.x
                    + ImGui::CalcTextSize(" [] ").x + ImGui::GetStyle().FramePadding.x * 2;
        float right_edge = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(right_edge - btn_w);

        JcePlayState ps = jce_state_get_play_state();

        /* Use the theme's Button color when stopped so light themes don't
           render a near-black button on a white menu bar. */
        ImVec4 stopped_btn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImGui::PushStyleColor(ImGuiCol_Button,
            ps == JCE_PLAY_PLAYING ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f)
                                   : stopped_btn);
        if (ImGui::SmallButton(ps == JCE_PLAY_STOPPED ? " > " : " || ")) {
            if (ps == JCE_PLAY_STOPPED) jce_state_play();
            else jce_state_pause();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();
        bool can_stop = (ps != JCE_PLAY_STOPPED);
        if (!can_stop) ImGui::BeginDisabled();
        if (ImGui::SmallButton(" [] ")) jce_state_stop();
        if (!can_stop) ImGui::EndDisabled();
    }

    if (dialog_active) ImGui::EndDisabled();

    ImGui::PopStyleVar(2); /* ItemSpacing + FramePadding */
    ImGui::EndMenuBar();
}
