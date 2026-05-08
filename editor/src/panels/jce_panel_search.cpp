/*
 * jce_panel_search.cpp  Global Search / Find References panel.
 *
 * Two tabs:
 *   - "In Scene": substring match over entity display names + tags;
 *     selecting a result pings the entity in Hierarchy/Inspector.
 *   - "In Project": substring match over file paths under the current
 *     project root (case-insensitive).
 *
 * Project search is intentionally simple — recursive directory walk +
 * filename match. A future revision can index file contents.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define jce_strncasecmp _strnicmp
#else
#  include <strings.h>
#  define jce_strncasecmp strncasecmp
#endif

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
}

static char s_query[128] = {0};
static int  s_tab        = 0; /* 0 scene, 1 project */

struct SceneHit { JceEntity e; std::string label; };
struct PathHit  { std::string path; };

static std::vector<SceneHit> s_scene_hits;
static std::vector<PathHit>  s_path_hits;
static char s_last_query[128] = {0};
static int  s_last_tab = -1;

static bool ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (jce_strncasecmp(p, needle, n) == 0) return true;
    }
    return false;
}

struct ScCtx { const char *q; std::vector<SceneHit> *out; };

static void scene_scan_cb(JceScene *s, JceEntity e, void *ud)
{
    ScCtx *c = (ScCtx *)ud;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (!m) return;
    if (ci_contains(m->name, c->q) || ci_contains(m->tag, c->q)) {
        SceneHit h; h.e = e;
        h.label = std::string(m->name[0] ? m->name : "(unnamed)") +
                   (m->tag[0] ? std::string("  [") + m->tag + "]" : std::string());
        c->out->push_back(std::move(h));
    }
}

static void rescan(void)
{
    s_scene_hits.clear();
    s_path_hits.clear();
    if (s_query[0] == '\0') return;

    if (s_tab == 0) {
        JceScene *scene = jce_state_get_scene();
        if (!scene) return;
        ScCtx ctx{ s_query, &s_scene_hits };
        jce_scene_each_entity(scene, scene_scan_cb, &ctx);
    } else {
        const char *root = jce_editor_assets_get_project();
        if (!root || !*root) return;
        const int max_hits = 500;
        struct WalkCtx {
            const char *q;
            std::vector<PathHit> *out;
            int max_hits;
        };
        WalkCtx ctx{ s_query, &s_path_hits, max_hits };
        auto cb = [](const char *path, bool is_dir, void *user) -> bool {
            auto *c = static_cast<WalkCtx *>(user);
            if (is_dir) return true;
            if ((int)c->out->size() >= c->max_hits) return false;
            if (ci_contains(path, c->q)) c->out->push_back({ std::string(path) });
            return true;
        };
        jce_fs_host_walk(root, cb, &ctx);
    }
}

extern "C" void jce_editor_panel_search_content(void)
{
    ImGui::SetNextItemWidth(-100);
    bool query_changed = ImGui::InputTextWithHint("##q", jce_editor_i18n("search.queryHint"), s_query,
                                                   sizeof(s_query));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("common.search")) || query_changed) {
        snprintf(s_last_query, sizeof(s_last_query), "%s", s_query);
        s_last_tab = s_tab;
        rescan();
    }

    if (ImGui::BeginTabBar("##search_tabs")) {
        if (ImGui::BeginTabItem(jce_editor_i18n("search.tab.scene"))) {
            if (s_tab != 0) { s_tab = 0; rescan(); }
            ImGui::Text("%s %zu", jce_editor_i18n("search.hits"), s_scene_hits.size());
            ImGui::Separator();
            if (ImGui::BeginChild("##sc", ImVec2(0,0))) {
                for (auto &h : s_scene_hits) {
                    ImGui::PushID((int)h.e);
                    if (ImGui::Selectable(h.label.c_str()))
                        jce_state_select_entity((uint32_t)h.e, false);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("search.tab.project"))) {
            if (s_tab != 1) { s_tab = 1; rescan(); }
            ImGui::Text("%s %zu", jce_editor_i18n("search.hits"), s_path_hits.size());
            ImGui::Separator();
            if (ImGui::BeginChild("##pj", ImVec2(0,0))) {
                for (auto &h : s_path_hits) {
                    if (ImGui::Selectable(h.path.c_str()))
                        jce_file_viewer_open(h.path.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
