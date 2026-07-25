/*
 * jce_panel_package_manager.cpp  Package Manager window (Sprint 2 / 0.8.21)
 *
 * Mirrors Unity's Window > Package Manager. JCE has no third-party
 * package registry today, so this panel surfaces the static list of
 * built-in subsystems (renderer, physics, audio, ai, animation, ui,
 * net, video, scene/ecs) plus any user-installed entries from
 * .jce/packages.json.  It gives a single place to see what's available
 * and to author a per-project enable/disable list that the engine
 * runtime (or a future loader) can consume to skip subsystem init.
 *
 * Built-in entries are read-only; user entries can be added/removed
 * and toggled.  Persistence is JSON at .jce/packages.json:
 *
 *     { "packages": [
 *         { "name": "...", "version": "...", "enabled": true,
 *           "user_added": true, "description": "..." }
 *     ] }
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/os/core/jce_json.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <jce/jce_version.h>
}

#include "core/jce_editor_config.h"   /* jce_editor_dotjce_path (legacy ~/.jce) */

/* Open-project root — owned by dialog_project.cpp (same explicit-root
 * pattern as jce_project_settings / jce_pak_key).  Declared at global
 * scope: inside an anonymous namespace the extern would acquire internal
 * linkage and never bind to the definition. */
extern char s_current_project_root[512];

/* The enable list is PER-PROJECT (the header always said so — disabling
 * jce.physics for a 2D project must not disable it everywhere), so it lives
 * under the OPEN project's root (<root>/.jce/packages.json).  Without a
 * project we fall back to the legacy per-user ~/.jce copy. */
static const char *packages_dir(void) {
    static char d[1024];
    std::snprintf(d, sizeof(d), "%s/.jce", s_current_project_root);
    return d;
}
static const char *packages_path(void) {
    static char p[1024];
    if (s_current_project_root[0])
        std::snprintf(p, sizeof(p), "%s/.jce/packages.json",
                      s_current_project_root);
    else
        jce_editor_dotjce_path("packages.json", p, sizeof(p));
    return p;
}
#define PACKAGES_PATH packages_path()

struct PkgEntry {
    std::string name;
    std::string version;
    std::string description;
    bool        enabled    = true;
    bool        user_added = false;   /* false = built-in subsystem (read-only) */
};

static std::vector<PkgEntry> s_pkgs;
static bool                  s_initialized = false;
static int                   s_selected    = -1;
static char                  s_new_name[64]    = {0};
static char                  s_new_version[32] = {0};

/* ── Built-in seed list (mirrors engine/src/middleware structure) ─── */
static void seed_builtin(void)
{
    static const struct { const char *n; const char *v; const char *d; } B[] = {
        { "jce.renderer",       JCE_VERSION_STR, "PBR + IBL + CSM + post-processing pipeline (bgfx)" },
        { "jce.physics",        JCE_VERSION_STR, "Bullet 3D + Box2D physics middleware" },
        { "jce.animation",      JCE_VERSION_STR, "ozz skinning, blend trees, state machines" },
        { "jce.audio",          JCE_VERSION_STR, "miniaudio mixer, spatializer, occlusion" },
        { "jce.ai",             JCE_VERSION_STR, "Behavior Tree + Recast Navmesh + steering" },
        { "jce.scene",          JCE_VERSION_STR, "flecs ECS + prefab + LOD + spatial partition" },
        { "jce.ui",             JCE_VERSION_STR, "RmlUI (HTML/CSS) + ImGui editor + game HUD" },
        { "jce.net",            JCE_VERSION_STR, "ENet + protobuf + WebSocket transports" },
        { "jce.video",          JCE_VERSION_STR, "H.264 / H.265 / VP8 / AV1 / MP4 / WebM" },
        { "jce.resource",       JCE_VERSION_STR, "PAK + async loader + world streaming" },
        { "jce.os.platform",    JCE_VERSION_STR, "SDL3 window/input + action mapping + Android JNI" },
    };
    for (auto &b : B) {
        PkgEntry p;
        p.name        = b.n;
        p.version     = b.v;
        p.description = b.d;
        p.enabled     = true;
        p.user_added  = false;
        s_pkgs.push_back(std::move(p));
    }
}

/* Built-in package descriptions are seeded in English (and persisted as
 * such); translate them opportunistically at display time. */
static const char *pkg_desc_i18n(const PkgEntry &p)
{
    if (p.user_added) return p.description.c_str();
    char key[96];
    std::string suffix = p.name;
    for (auto &ch : suffix) if (ch == '.') ch = '_';
    snprintf(key, sizeof(key), "packageManager.desc.%s", suffix.c_str());
    return jce_editor_i18n_or(key, p.description.c_str());
}

/* ── Persistence ────────────────────────────────────────────────────── */
static void pkgs_save(void)
{
    JceJson *root = jce_json_object();
    if (!root) return;
    JceJson *arr = jce_json_array();
    if (!arr) { jce_json_free(root); return; }
    jce_json_set_child(root, "packages", arr);

    for (const PkgEntry &p : s_pkgs) {
        /* Persist user_added entries verbatim; persist built-in only if disabled. */
        if (!p.user_added && p.enabled) continue;
        JceJson *e = jce_json_object();
        if (!e) break;
        jce_json_array_push(arr, e);
        jce_json_set_string(e, "name",        p.name.c_str());
        jce_json_set_string(e, "version",     p.version.c_str());
        /* 1/0 rather than true/false: that is what every file already on
         * disk carries, and the loader accepts either. */
        jce_json_set_int   (e, "enabled",     p.enabled    ? 1 : 0);
        jce_json_set_int   (e, "user_added",  p.user_added ? 1 : 0);
        jce_json_set_string(e, "description", p.description.c_str());
    }

    /* Project .jce/ dir may not exist yet (fresh project). */
    if (s_current_project_root[0])
        jce_fs_host_create_directory(packages_dir());
    /* Serialised through the JSON facade so a package name carrying a quote
     * or backslash is escaped instead of corrupting the store, and written
     * atomically (temp+rename) because this is authored editor state. */
    ed_write_json_to_file(PACKAGES_PATH, root);
}

static void pkgs_load_overlay(void)
{
    size_t len = 0;
    char  *raw = (char *)ed_read_file(PACKAGES_PATH, &len);
    if (!raw) return;
    if (len > (1 << 20)) { ED_FREE(raw); return; }

    JceJson *root = jce_json_parse(raw, len);
    ED_FREE(raw);
    if (!root) return;
    JceJson *arr = jce_json_get(root, "packages");
    if (!jce_json_is_array(arr)) { jce_json_free(root); return; }

    int n = jce_json_array_size(arr);
    for (int i = 0; i < n; ++i) {
        const JceJson *o = jce_json_array_at(arr, i);
        if (!jce_json_is_object(o)) continue;

        std::string name = jce_json_get_string(o, "name", "");
        if (name.empty()) continue;
        bool enabled    = jce_json_get_bool(o, "enabled",    true);
        bool user_added = jce_json_get_bool(o, "user_added", false);

        /* Match against built-ins by name; otherwise append. */
        bool merged = false;
        for (auto &b : s_pkgs) {
            if (b.name == name) {
                b.enabled = enabled;
                if (!b.user_added && user_added)
                    b.user_added = true;
                merged = true;
                break;
            }
        }
        if (!merged) {
            PkgEntry e;
            e.name        = std::move(name);
            e.version     = jce_json_get_string(o, "version",     "");
            e.description = jce_json_get_string(o, "description", "");
            e.enabled     = enabled;
            e.user_added  = user_added;
            s_pkgs.push_back(std::move(e));
        }
    }
    jce_json_free(root);
}

/* One-time forward-migration: older editors stored the enable list per-user
 * in ~/.jce/packages.json.  If the open project has no copy yet but the
 * global one exists, copy it forward so authored enables/user entries carry
 * over.  The global file is kept: it stays the fallback when no project is
 * open, and it seeds other not-yet-migrated projects. */
static void migrate_legacy_packages(void)
{
    if (!s_current_project_root[0]) return;   /* no project: global IS the store */
    if (jce_fs_host_exists_file(PACKAGES_PATH)) return;   /* project copy wins */
    char legacy[1024];
    jce_editor_dotjce_path("packages.json", legacy, sizeof(legacy));
    if (!legacy[0] || !jce_fs_host_exists_file(legacy)) return;
    jce_fs_host_create_directory(packages_dir());
    jce_fs_host_copy_file(legacy, PACKAGES_PATH);
}

/* Project root the current list was loaded for. */
static char s_pkgs_root[512] = {0};

static void ensure_init(void)
{
    /* Follow the open project (the store is project-scoped): a project
     * switch reloads from the new root instead of saving the previous
     * project's list into it. */
    if (s_initialized &&
        std::strcmp(s_pkgs_root, s_current_project_root) != 0)
        s_initialized = false;
    if (s_initialized) return;
    s_initialized = true;
    std::snprintf(s_pkgs_root, sizeof(s_pkgs_root), "%s",
                  s_current_project_root);
    s_selected = -1;
    s_pkgs.clear();
    seed_builtin();
    migrate_legacy_packages();
    pkgs_load_overlay();
}

/* ── UI ─────────────────────────────────────────────────────────────── */
extern "C" void jce_editor_panel_package_manager_content(void)
{
    ensure_init();

    /* Toolbar */
    if (ImGui::Button(jce_editor_i18n("packageManager.refresh"))) {
        s_initialized = false;
        ensure_init();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("packageManager.save"))) {
        pkgs_save();
    }
    ImGui::SameLine();
    bool can_remove = (s_selected >= 0 &&
                       s_selected < (int)s_pkgs.size() &&
                       s_pkgs[s_selected].user_added);
    ImGui::BeginDisabled(!can_remove);
    if (ImGui::Button(jce_editor_i18n("packageManager.remove"))) {
        s_pkgs.erase(s_pkgs.begin() + s_selected);
        if (s_selected >= (int)s_pkgs.size()) --s_selected;
        pkgs_save();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(%d)", (int)s_pkgs.size());

    ImGui::Separator();

    /* Two-column: list | details */
    ImGui::BeginChild("##pkg_list", ImVec2(280, -120), true);
    for (int i = 0; i < (int)s_pkgs.size(); ++i) {
        ImGui::PushID(i);
        bool sel = (s_selected == i);
        bool en  = s_pkgs[i].enabled;
        if (ImGui::Checkbox("##en", &en)) {
            s_pkgs[i].enabled = en;
            pkgs_save();
        }
        ImGui::SameLine();
        char label[160];
        std::snprintf(label, sizeof(label), "%s %s##sel",
                      s_pkgs[i].name.c_str(),
                      s_pkgs[i].user_added
                          ? jce_editor_i18n("packageManager.tagUser")
                          : "");
        if (ImGui::Selectable(label, sel)) s_selected = i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##pkg_detail", ImVec2(0, -120), true);
    if (s_selected >= 0 && s_selected < (int)s_pkgs.size()) {
        PkgEntry &p = s_pkgs[s_selected];
        ImGui::Text("%s", p.name.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("%s %s",
            jce_editor_i18n("packageManager.version"), p.version.c_str());
        ImGui::TextDisabled("%s",
            p.user_added
                ? jce_editor_i18n("packageManager.sourceUser")
                : jce_editor_i18n("packageManager.sourceBuiltin"));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", pkg_desc_i18n(p));
    } else {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("packageManager.selectHint"));
    }
    ImGui::EndChild();

    /* Add user package */
    ImGui::Separator();
    ImGui::Text("%s", jce_editor_i18n("packageManager.addTitle"));
    ImGui::SetNextItemWidth(220);
    ImGui::InputText(jce_editor_i18n("packageManager.name"),
                     s_new_name, sizeof(s_new_name));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputText(jce_editor_i18n("packageManager.version"),
                     s_new_version, sizeof(s_new_version));
    ImGui::SameLine();
    ImGui::BeginDisabled(s_new_name[0] == 0);
    if (ImGui::Button(jce_editor_i18n("packageManager.add"))) {
        PkgEntry e;
        e.name        = s_new_name;
        e.version     = s_new_version[0] ? s_new_version : "0.0.0";
        e.description = "";
        e.enabled     = true;
        e.user_added  = true;
        s_pkgs.push_back(std::move(e));
        s_selected = (int)s_pkgs.size() - 1;
        s_new_name[0] = 0;
        s_new_version[0] = 0;
        pkgs_save();
    }
    ImGui::EndDisabled();

    ImGui::TextDisabled("%s %s",
        jce_editor_i18n("packageManager.path"), PACKAGES_PATH);
    ImGui::TextDisabled("%s",
        jce_editor_i18n("packageManager.runtimeNote"));
}

extern "C" void package_manager_draw_content(void)
{
    jce_editor_panel_package_manager_content();
}

/* Shim: Package Manager has been merged into the Bundle Browser
 * "Asset Pipeline" workbench as a tab.  Activating this panel now
 * redirects to that workbench and requests the Package Manager tab.
 * Symbol kept so the menu/hotkey entries registered against
 * JCE_PANEL_PACKAGE_MANAGER keep working. */
extern "C" void jce_editor_panel_package_manager(void)
{
    if (jce_panel_redirect_to_workbench(JCE_PANEL_PACKAGE_MANAGER,
                                        JCE_PANEL_BUNDLE_BROWSER,
                                        "panel.bundle_browser.title",
                                        "bundle_browser"))
        jce_panel_bundle_browser_request_tab(2);
}
