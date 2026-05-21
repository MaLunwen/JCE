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

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <jce/jce_version.h>
}

#define PACKAGES_PATH ".jce/packages.json"

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

/* ── Persistence ────────────────────────────────────────────────────── */
static void pkgs_save(void)
{
    size_t cap = 256;
    for (auto &p : s_pkgs)
        cap += p.name.size() + p.version.size() + p.description.size() + 96;

    char *buf = (char *)ED_MALLOC(cap);
    if (!buf) return;
    size_t off = 0;
    int    w   = std::snprintf(buf + off, cap - off, "{\n  \"packages\": [\n");
    if (w < 0) { ED_FREE(buf); return; }
    off += (size_t)w;

    int written = 0;
    for (size_t i = 0; i < s_pkgs.size(); ++i) {
        const PkgEntry &p = s_pkgs[i];
        /* Persist user_added entries verbatim; persist built-in only if disabled. */
        if (!p.user_added && p.enabled) continue;
        w = std::snprintf(buf + off, cap - off,
            "%s    { \"name\": \"%s\", \"version\": \"%s\","
            " \"enabled\": %d, \"user_added\": %d,"
            " \"description\": \"%s\" }",
            (written == 0 ? "" : ",\n"),
            p.name.c_str(), p.version.c_str(),
            p.enabled    ? 1 : 0,
            p.user_added ? 1 : 0,
            p.description.c_str());
        if (w < 0 || (size_t)w >= cap - off) { ED_FREE(buf); return; }
        off += (size_t)w;
        ++written;
    }
    w = std::snprintf(buf + off, cap - off, "\n  ]\n}\n");
    if (w < 0) { ED_FREE(buf); return; }
    off += (size_t)w;
    ed_write_file(PACKAGES_PATH, buf, off);
    ED_FREE(buf);
}

static bool extract_str(const char *block, const char *key, std::string &out)
{
    char keypat[32];
    std::snprintf(keypat, sizeof(keypat), "\"%s\"", key);
    const char *k = std::strstr(block, keypat);
    if (!k) return false;
    const char *q1 = std::strchr(k + std::strlen(keypat), '"');
    const char *q2 = q1 ? std::strchr(q1 + 1, '"') : nullptr;
    if (!q1 || !q2) return false;
    out.assign(q1 + 1, q2 - q1 - 1);
    return true;
}

static bool extract_int(const char *block, const char *key, int *out)
{
    char keypat[32];
    std::snprintf(keypat, sizeof(keypat), "\"%s\"", key);
    const char *k = std::strstr(block, keypat);
    if (!k) return false;
    return std::sscanf(k + std::strlen(keypat), " : %d", out) == 1;
}

static void pkgs_load_overlay(void)
{
    size_t len = 0;
    char  *raw = (char *)ed_read_file(PACKAGES_PATH, &len);
    if (!raw) return;
    if (len > (1 << 20)) { ED_FREE(raw); return; }

    const char *p = raw;
    while (p && *p) {
        const char *brace = std::strchr(p, '{');
        if (!brace) break;
        const char *end = std::strchr(brace, '}');
        if (!end) break;
        std::string block(brace, end - brace + 1);

        std::string name, version, description;
        int enabled = 1, user_added = 0;
        if (extract_str(block.c_str(), "name", name) && !name.empty()) {
            extract_str(block.c_str(), "version",     version);
            extract_str(block.c_str(), "description", description);
            extract_int(block.c_str(), "enabled",     &enabled);
            extract_int(block.c_str(), "user_added",  &user_added);

            /* Match against built-ins by name; otherwise append. */
            bool merged = false;
            for (auto &b : s_pkgs) {
                if (b.name == name) {
                    b.enabled = (enabled != 0);
                    if (!b.user_added && user_added)
                        b.user_added = true;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                PkgEntry e;
                e.name        = std::move(name);
                e.version     = std::move(version);
                e.description = std::move(description);
                e.enabled     = (enabled    != 0);
                e.user_added  = (user_added != 0);
                s_pkgs.push_back(std::move(e));
            }
        }
        p = end + 1;
    }
    ED_FREE(raw);
}

static void ensure_init(void)
{
    if (s_initialized) return;
    s_initialized = true;
    s_pkgs.clear();
    seed_builtin();
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
        ImGui::TextWrapped("%s", p.description.c_str());
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
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_PACKAGE_MANAGER);
    if (!vis || !*vis) return;
    *vis = false;

    bool *bb_vis = jce_editor_panel_visible_ptr(JCE_PANEL_BUNDLE_BROWSER);
    if (bb_vis) *bb_vis = true;

    char title[128];
    std::snprintf(title, sizeof(title), "%s###bundle_browser",
                  jce_editor_i18n("panel.bundle_browser.title"));
    ImGui::SetWindowFocus(title);
    jce_panel_bundle_browser_request_tab(2);
}
