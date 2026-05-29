/*
 * jce_dialog_scene.cpp  Thin shims for Scene dialog menu/hotkey entries.
 *
 * The standalone New/Open/Save-As modals were retired in P5-A.5.  Each
 * action now drives the host's native file-picker through the existing
 * async wrappers in jce_dialog_project.cpp, then defers to the same
 * scene load/save APIs the rest of the editor already uses.  This file
 * is kept (not deleted) so menu and hotkey callers that link against
 * the public symbols keep resolving.
 *
 * Public symbols retained:
 *   - jce_editor_dialog_new_scene(bool *p_open)
 *   - jce_editor_dialog_open_scene(bool *p_open)
 *   - jce_editor_dialog_save_as(bool *p_open)
 *   - jce_editor_dialog_unsaved_changes(bool *p_open, int *result)
 *     (kept as a small genuine modal — Save/Don't-Save/Cancel is part
 *      of the quit confirmation contract and has no direct-action
 *      equivalent.)
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/application/jce_project.h>

#include "jce_editor_dialogs_internal.h"
#include "core/jce_editor_project.h"

/* ── Helpers ──────────────────────────────────────────────────────── */

namespace {

constexpr const char *kSceneFilter =
    "Scene Files (*.scene *.scene.json);;All Files (*.*)";

bool has_scene_ext(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n >= 6  && strcmp(name + n - 6,  ".scene")      == 0) return true;
    if (n >= 11 && strcmp(name + n - 11, ".scene.json") == 0) return true;
    return false;
}

/* Default starting path for a Save/New dialog.
 * Prefers the active JceProject's source_assets dir (schema v2) so
 * scenes land where the cook pipeline expects them.  Falls back to
 * "<project>/assets/scenes" for legacy projects and "<cwd>/<name>"
 * when no project is loaded. */
void compute_default_scene_path(char *out, size_t out_size,
                                const char *fallback_name)
{
    out[0] = '\0';
    char dir[1024] = {0};

    const JceProject *jp = jce_editor_project_get();
    if (jp && jp->project_root[0] != '\0') {
        const char *src = (jp->source_assets[0] != '\0')
                              ? jp->source_assets : "assets";
        char tmp[1024];
        if (jce_path_is_absolute(src)) {
            snprintf(tmp, sizeof(tmp), "%s", src);
        } else {
            jce_path_join(tmp, sizeof(tmp), jp->project_root, src);
        }
        jce_path_join(dir, sizeof(dir), tmp, "scenes");
    } else if (s_current_project_root[0] != '\0') {
        char tmp[1024];
        jce_path_join(tmp, sizeof(tmp), s_current_project_root, "assets");
        jce_path_join(dir, sizeof(dir), tmp, "scenes");
    } else if (!jce_fs_host_get_current_dir(dir, sizeof(dir))) {
        snprintf(dir, sizeof(dir), ".");
    }
    jce_fs_host_create_directory(dir);
    jce_path_join(out, out_size, dir, fallback_name);
}

/* Soft warn (Console) when the picked scene path is outside the
 * project's source_assets tree.  The pipeline still cooks/builds, but
 * artifacts outside source_assets won't be packaged by the cooker. */
void warn_if_outside_source_assets(const char *path)
{
    const JceProject *jp = jce_editor_project_get();
    if (!jp || !jp->project_root[0] || !path || !path[0]) return;
    const char *src = (jp->source_assets[0] != '\0')
                          ? jp->source_assets : "assets";
    char src_abs[1024];
    if (jce_path_is_absolute(src)) {
        snprintf(src_abs, sizeof(src_abs), "%s", src);
    } else {
        jce_path_join(src_abs, sizeof(src_abs), jp->project_root, src);
    }
    /* Normalise separators for prefix compare. */
    char a[1024]; char b[1024];
    snprintf(a, sizeof(a), "%s", src_abs);
    snprintf(b, sizeof(b), "%s", path);
    for (char *p = a; *p; ++p) if (*p == '\\') *p = '/';
    for (char *p = b; *p; ++p) if (*p == '\\') *p = '/';
    size_t na = strlen(a);
    if (strncmp(b, a, na) != 0) {
        jce_editor_console_log_level(JCE_CONSOLE_WARNING,
            "Scene '%s' is outside source_assets ('%s') — it will be "
            "excluded from the cook step.  Move it under the source_assets "
            "tree to package it with the game.", path, src_abs);
    }
}

/* Single-shot async dialog state, identical layout for all three shims. */
struct AsyncDlg {
    char path[1024];
    bool dispatched;
    bool ready;
    bool cancelled;
};

void reset_async(AsyncDlg &d)
{
    d.path[0]    = '\0';
    d.dispatched = false;
    d.ready      = false;
    d.cancelled  = false;
}

/* Make sure the resulting path ends with a scene extension. */
void ensure_scene_ext(char *path, size_t cap)
{
    char base[256];
    jce_path_basename(base, sizeof(base), path);
    if (!has_scene_ext(base)) {
        size_t cur = strlen(path);
        snprintf(path + cur, cap - cur, ".scene");
    }
}

} /* namespace */

/* ── New Scene ───────────────────────────────────────────────────────
 * Native save-file picker → write "{}" → load via jce_state.
 * ----------------------------------------------------------------- */

void jce_editor_dialog_new_scene(bool *p_open)
{
    if (!p_open || !*p_open) return;
    static AsyncDlg s;

    if (!s.dispatched) {
        reset_async(s);
        s.dispatched = true;
        char defp[1024];
        compute_default_scene_path(defp, sizeof(defp), "NewScene.scene");
        save_file_dialog_async(jce_editor_i18n("menu.file.newScene"),
                               defp[0] ? defp : NULL, kSceneFilter,
                               s.path, sizeof(s.path),
                               &s.ready, &s.cancelled);
        return;
    }
    if (s.cancelled) { reset_async(s); *p_open = false; return; }
    if (!s.ready)    return;

    ensure_scene_ext(s.path, sizeof(s.path));
    warn_if_outside_source_assets(s.path);

    char parent[1024];
    jce_path_parent(parent, sizeof(parent), s.path);
    if (parent[0] != '\0') jce_fs_host_create_directory(parent);

    static const char empty_scene[] = "{}";
    if (!ed_write_file(s.path, empty_scene, sizeof(empty_scene) - 1)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Failed to create scene file: %s", s.path);
    } else if (jce_state_load_scene_file(s.path)) {
        jce_editor_layout_request_focus_scene_view();
        jce_editor_console_log("Created scene: %s", s.path);
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Created but failed to load scene: %s", s.path);
    }
    reset_async(s);
    *p_open = false;
}

/* ── Open Scene ──────────────────────────────────────────────────────
 * Native open-file picker → load via jce_state.
 * ----------------------------------------------------------------- */

void jce_editor_dialog_open_scene(bool *p_open)
{
    if (!p_open || !*p_open) return;
    static AsyncDlg s;

    if (!s.dispatched) {
        reset_async(s);
        s.dispatched = true;
        char defp[1024];
        compute_default_scene_path(defp, sizeof(defp), "");
        open_file_dialog_async(jce_editor_i18n("menu.file.openScene"),
                               defp[0] ? defp : NULL, kSceneFilter,
                               s.path, sizeof(s.path),
                               &s.ready, &s.cancelled);
        return;
    }
    if (s.cancelled) { reset_async(s); *p_open = false; return; }
    if (!s.ready)    return;

    if (jce_fs_host_exists_file(s.path) && jce_state_load_scene_file(s.path)) {
        jce_editor_layout_request_focus_scene_view();
        jce_editor_console_log("Opened scene: %s", s.path);
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Failed to open scene: %s", s.path);
    }
    reset_async(s);
    *p_open = false;
}

/* ── Save As ─────────────────────────────────────────────────────────
 * Native save-file picker → save via jce_state.  Note: the global
 * quit-after-save-as flow in jce_editor_layout.cpp waits until
 * `!s_show_save_as`, so we only flip *p_open after the picker resolves.
 * ----------------------------------------------------------------- */

void jce_editor_dialog_save_as(bool *p_open)
{
    if (!p_open || !*p_open) return;
    static AsyncDlg s;

    if (!s.dispatched) {
        reset_async(s);
        s.dispatched = true;

        char defp[1024] = {0};
        const char *cur = jce_state_get_current_scene_path();
        if (cur && cur[0] != '\0') {
            snprintf(defp, sizeof(defp), "%s", cur);
        } else {
            compute_default_scene_path(defp, sizeof(defp), "Scene.scene");
        }
        save_file_dialog_async(jce_editor_i18n("menu.file.saveAs"),
                               defp[0] ? defp : NULL, kSceneFilter,
                               s.path, sizeof(s.path),
                               &s.ready, &s.cancelled);
        return;
    }
    if (s.cancelled) { reset_async(s); *p_open = false; return; }
    if (!s.ready)    return;

    ensure_scene_ext(s.path, sizeof(s.path));
    warn_if_outside_source_assets(s.path);

    char parent[1024];
    jce_path_parent(parent, sizeof(parent), s.path);
    if (parent[0] != '\0') jce_fs_host_create_directory(parent);

    if (jce_state_save_scene_file(s.path)) {
        jce_editor_layout_request_focus_scene_view();
        jce_editor_console_log("Saved scene as: %s", s.path);
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Save scene failed: %s", s.path);
    }
    reset_async(s);
    *p_open = false;
}

/* ── Unsaved Changes ─────────────────────────────────────────────────
 * Genuine modal: Save / Don't Save / Cancel.  Drives the quit-flow
 * state machine in jce_editor_layout.cpp (s_unsaved_result).
 * ----------------------------------------------------------------- */

void jce_editor_dialog_unsaved_changes(bool *p_open, int *result)
{
    if (!p_open || !*p_open) return;

    const char *popup_id = "###UnsavedDialog";
    if (*p_open && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    char _title[256];
    snprintf(_title, sizeof(_title), "%s%s",
             jce_editor_i18n("unsaved.title"), popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 180), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::BeginPopupModal(_title, p_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_NoResize)) {
        return;
    }

    ImGui::Spacing();
    ImGui::TextColored(JCE_COLOR_CONSOLE_WARN, " ? ");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", jce_editor_i18n("unsaved.message"));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w = 100.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 3 + spacing * 2;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                         + ImGui::GetCursorPosX());

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.24f, 0.62f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.70f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.52f, 0.20f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("dialog.save"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Enter)
        || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (result) *result = 1;
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.30f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("unsaved.dontSave"), ImVec2(btn_w, 0))) {
        if (result) *result = 2;
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        if (result) *result = 3;
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (result) *result = 3;
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }

    ImGui::EndPopup();
}
