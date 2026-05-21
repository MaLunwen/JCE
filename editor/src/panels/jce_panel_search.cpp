/*
 * jce_panel_search.cpp  Global Search / Find References panel.
 *
 * Three tabs:
 *   - "In Scene"          : substring match over entity display names + tags.
 *   - "In Menus / Panels" : cross-locale i18n-aware search over every
 *                           registered panel title and every well-known
 *                           menu item key. A query in ANY installed
 *                           locale hits the SAME target (e.g. an English
 *                           UI can find "光照" → Lighting Settings).
 *   - "In Project"        : substring match over file paths AND the
 *                           "name" / "displayName" string fields inside
 *                           .scene / .prefab / .material / .asset JSON
 *                           files. Per-file (path, mtime) cache.
 *
 * All i18n strings are routed through jce_editor_i18n*(). All file I/O
 * goes through jce_fs_host_* / jce_json (no raw libc, no platform macros).
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"
#include "io/jce_editor_file_util.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <unordered_map>
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
#include <jce/os/core/jce_json.h>
}

/* ── State ─────────────────────────────────────────────────────────── */

static char s_query[128] = {0};
static int  s_tab        = 0; /* 0 scene, 1 menus/panels, 2 project */

struct SceneHit { JceEntity e; std::string label; };
struct PathHit  { std::string path; std::string label; /* "file :: name" or just path */ };
struct MpHit    { /* menu / panel hit */
    bool        is_panel;          /* true → open the panel, false → menu (no-op) */
    JceEditorPanel panel;          /* valid iff is_panel */
    std::string label;             /* human-visible rendered string in active locale */
    std::string path;              /* menu path like "File > Save", or panel rendered name */
    char        locale_tag[4];     /* "EN" / "ZH" of the locale that matched */
};

static std::vector<SceneHit> s_scene_hits;
static std::vector<MpHit>    s_mp_hits;
static std::vector<PathHit>  s_path_hits;
static char s_last_query[128] = {0};
static int  s_last_tab = -1;

/* ── Helpers ───────────────────────────────────────────────────────── */

static bool ci_contains(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (jce_strncasecmp(p, needle, n) == 0) return true;
    }
    return false;
}

static void locale_short_tag(JceLocale loc, char out[4])
{
    switch (loc) {
        case JCE_LOCALE_EN:    strcpy(out, "EN"); break;
        case JCE_LOCALE_ZH_CN: strcpy(out, "ZH"); break;
        case JCE_LOCALE_KO:    strcpy(out, "KO"); break;
        default:               snprintf(out, 4, "L%d", (int)loc); break;
    }
}

/* ── Panel registry (panel id ↔ i18n title key) ────────────────────── */

struct PanelKeyRow { JceEditorPanel id; const char *key; };

/* Mirrors how each panel's title is shown in jce_editor_layout.cpp
 * (Window menu + docked ImGui::Begin labels). Keep in sync when adding
 * new panels — the lint i18n_audit.py will catch missing translations. */
static const PanelKeyRow kPanelKeys[] = {
    { JCE_PANEL_HIERARCHY,           "Hierarchy"                     },
    { JCE_PANEL_INSPECTOR,           "Inspector"                     },
    { JCE_PANEL_CONSOLE,             "Console"                       },
    { JCE_PANEL_SCENE_VIEW,          "Scene"                         },
    { JCE_PANEL_GAME_VIEW,           "Game"                          },
    { JCE_PANEL_TIMELINE,            "Timeline"                      },
    { JCE_PANEL_ASSETS,              "Asset Browser"                 },
    { JCE_PANEL_FILE_VIEWER,         "File Viewer"                   },
    { JCE_PANEL_POSTFX,              "postfx.title"                  },
    { JCE_PANEL_PROFILER,            "window.profiler"               },
    { JCE_PANEL_PARTICLE_EDITOR,     "window.particleEditor"         },
    { JCE_PANEL_MATERIAL_GRAPH,      "window.materialGraph"          },
    { JCE_PANEL_IMPORT_PRESETS,      "window.importPresets"          },
    { JCE_PANEL_LIGHTMAP_BAKE,       "window.lightmapBake"           },
    { JCE_PANEL_AUDIO_MIXER,         "audioMixer.title"              },
    { JCE_PANEL_INPUT_MANAGER,       "inputManager.title"            },
    { JCE_PANEL_CURVE_EDITOR,        "window.curveEditor"            },
    { JCE_PANEL_ANIMATION_EDITOR,    "window.animationEditor"        },
    { JCE_PANEL_ANIMATOR_SM,         "window.animatorSM"             },
    { JCE_PANEL_SEQUENCER,           "window.sequencer"              },
    { JCE_PANEL_NAVMESH,             "window.navmesh"                },
    { JCE_PANEL_TERRAIN,             "window.terrain"                },
    { JCE_PANEL_PREFERENCES,         "panel.preferences.title"       },
    { JCE_PANEL_PACKAGE_MANAGER,     "packageManager.title"          },
    { JCE_PANEL_FRAME_DEBUGGER,      "frameDebugger.title"           },
    { JCE_PANEL_SPRITE_EDITOR,       "spriteEditor.title"            },
    { JCE_PANEL_TILE_PALETTE,        "tilePalette.title"             },
    { JCE_PANEL_VFX_GRAPH,           "vfxGraph.title"                },
    { JCE_PANEL_TEST_RUNNER,         "testRunner.title"              },
    { JCE_PANEL_BUILD_PROFILES,      "buildProfiles.title"           },
    { JCE_PANEL_TOOLBAR,             "window.toolbar"                },
    { JCE_PANEL_STATUS_BAR,          "window.statusBar"              },
    { JCE_PANEL_MEMORY_PROFILER,     "window.memoryProfiler"         },
    { JCE_PANEL_PHYSICS_DEBUGGER,    "window.physicsDebugger"        },
    { JCE_PANEL_LIGHT_EXPLORER,      "window.lightExplorer"          },
    { JCE_PANEL_REFLECTION_PROBES,   "window.reflectionProbes"       },
    { JCE_PANEL_SHADER_GRAPH,        "window.shaderGraph"            },
    { JCE_PANEL_SEARCH,              "window.search"                 },
    { JCE_PANEL_VERSION_CONTROL,     "window.versionControl"         },
    { JCE_PANEL_TIME_OF_DAY,         "window.timeOfDay"              },
    { JCE_PANEL_VCAM_MANAGER,        "window.vcamManager"            },
    { JCE_PANEL_REVERB_ZONES,        "window.reverbZones"            },
    { JCE_PANEL_SAVE_BROWSER,        "window.saveBrowser"            },
    { JCE_PANEL_BUNDLE_BROWSER,      "window.bundleBrowser"          },
    { JCE_PANEL_PHYSICS_LAYERS,      "panel.physics_layers.title"    },
    { JCE_PANEL_TAGS_LAYERS,         "panel.tags_layers.title"       },
    { JCE_PANEL_LIGHTING_SETTINGS,   "panel.lighting.title"          },
    { JCE_PANEL_BUILD_REPORT,        "window.buildReport"            },
    { JCE_PANEL_SYSTEMS,             "window.systems"                },
    { JCE_PANEL_PROJECT_SETTINGS,    "panel.project_settings.title"  },
    { JCE_PANEL_USER_PREFERENCES,    "panel.preferences.title"       },
    { JCE_PANEL_LAN_DISCOVERY,       "panel.lan_discovery.title"     },
    { JCE_PANEL_NETWORK_STATS,       "panel.network_stats.title"     },
    { JCE_PANEL_ANIMATION_RIGGING,   "window.animationRigging"       },
    { JCE_PANEL_RENDER_PIPELINE,     "panel.render_pipeline.title"   },
    { JCE_PANEL_PROFILE_ANALYZER,    "window.profileAnalyzer"        },
};
static constexpr int kPanelKeyCount =
    (int)(sizeof(kPanelKeys) / sizeof(kPanelKeys[0]));

/* Well-known top-level menu i18n keys (best-effort; not exhaustive).
 * Selecting a menu hit shows the textual path but does not invoke the
 * menu action (panel-equivalent hits go via the panel registry). */
struct MenuKeyRow { const char *parent_key; const char *child_key; };
static const MenuKeyRow kMenuKeys[] = {
    { "menu.file",     "menu.file.new"            },
    { "menu.file",     "menu.file.open"           },
    { "menu.file",     "menu.file.openScene"      },
    { "menu.file",     "menu.file.newScene"       },
    { "menu.file",     "menu.file.saveScene"      },
    { "menu.file",     "menu.file.saveAs"         },
    { "menu.file",     "menu.file.saveAll"        },
    { "menu.file",     "menu.file.importAssets"   },
    { "menu.file",     "menu.file.buildSettings"  },
    { "menu.file",     "menu.file.buildBundles"   },
    { "menu.file",     "menu.file.openBundle"     },
    { "menu.file",     "menu.file.close"          },
    { "menu.file",     "menu.file.exit"           },
    { "menu.edit",     "menu.edit.undo"           },
    { "menu.edit",     "menu.edit.redo"           },
    { "menu.edit",     "menu.edit.cut"            },
    { "menu.edit",     "menu.edit.copy"           },
    { "menu.edit",     "menu.edit.paste"          },
    { "menu.edit",     "menu.edit.duplicate"      },
    { "menu.edit",     "menu.edit.delete"         },
    { "menu.edit",     "menu.edit.selectAll"      },
    { "menu.edit",     "menu.edit.preferences"    },
    { "menu.edit",     "menu.edit.projectSettings"},
    { "menu.assets",   "menu.assets.import"       },
    { "menu.assets",   "menu.assets.refresh"      },
    { "menu.assets",   "menu.assets.createFolder" },
    { "menu.assets",   "menu.assets.createScene"  },
    { "menu.assets",   "menu.assets.createPrefab" },
    { "menu.assets",   "menu.assets.createMaterial"},
    { "menu.assets",   "menu.assets.createScript" },
    { "menu.component","menu.component.add"       },
    { "menu.component","menu.component.remove"    },
    { "menu.window",   "menu.window.resetLayout"  },
    { "menu.debug",    "menu.debug.toggleDemoLod" },
    { "menu.help",     "menu.help.about"          },
    { "menu.help",     "menu.help.documentation"  },
};
static constexpr int kMenuKeyCount =
    (int)(sizeof(kMenuKeys) / sizeof(kMenuKeys[0]));

/* ── Menus / Panels scan ───────────────────────────────────────────── */

static void scan_menus_panels(const char *q)
{
    s_mp_hits.clear();
    if (!q || !*q) return;

    const int n_loc = jce_editor_i18n_locale_count();

    /* Panels: for each panel walk every installed locale; first locale
     * that matches the query (or matches the raw key) → emit hit. */
    for (int i = 0; i < kPanelKeyCount; i++) {
        const PanelKeyRow &row = kPanelKeys[i];

        bool       matched = false;
        JceLocale  match_loc = JCE_LOCALE_EN;

        /* Raw key match (allows searching by identifier). */
        if (ci_contains(row.key, q)) {
            matched   = true;
            match_loc = jce_editor_i18n_get_locale();
        }

        /* Cross-locale string match. */
        for (int li = 0; !matched && li < n_loc; li++) {
            JceLocale loc = (JceLocale)li;
            const char *s = jce_editor_i18n_lookup_locale(loc, row.key);
            if (s && ci_contains(s, q)) {
                matched   = true;
                match_loc = loc;
                break;
            }
        }
        if (!matched) continue;

        MpHit h;
        h.is_panel = true;
        h.panel    = row.id;
        h.label    = jce_editor_i18n(row.key);   /* render in active locale */
        h.path     = h.label;
        locale_short_tag(match_loc, h.locale_tag);
        s_mp_hits.push_back(std::move(h));
    }

    /* Menu items: same logic, no action on activation. */
    for (int i = 0; i < kMenuKeyCount; i++) {
        const MenuKeyRow &row = kMenuKeys[i];

        bool       matched   = false;
        JceLocale  match_loc = JCE_LOCALE_EN;

        if (ci_contains(row.child_key, q)) {
            matched   = true;
            match_loc = jce_editor_i18n_get_locale();
        }
        for (int li = 0; !matched && li < n_loc; li++) {
            JceLocale loc = (JceLocale)li;
            const char *cs = jce_editor_i18n_lookup_locale(loc, row.child_key);
            const char *ps = jce_editor_i18n_lookup_locale(loc, row.parent_key);
            if ((cs && ci_contains(cs, q)) ||
                (ps && ci_contains(ps, q))) {
                matched   = true;
                match_loc = loc;
                break;
            }
        }
        if (!matched) continue;

        MpHit h;
        h.is_panel = false;
        h.panel    = JCE_PANEL_COUNT;
        h.label    = std::string(jce_editor_i18n(row.parent_key)) +
                     " > " + jce_editor_i18n(row.child_key);
        h.path     = h.label;
        locale_short_tag(match_loc, h.locale_tag);
        s_mp_hits.push_back(std::move(h));
    }
}

/* ── Scene scan (unchanged) ────────────────────────────────────────── */

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

/* ── Project tab: path + JSON-content index ────────────────────────── */

struct JsonContentEntry {
    int64_t                  mtime;
    std::vector<std::string> names;   /* values of "name" / "displayName" */
};
static std::unordered_map<std::string, JsonContentEntry> s_json_cache;

/* True for JSON-bearing asset files whose name/displayName we want to
 * scan. Extensions match the project conventions documented in the task. */
static bool is_json_asset(const char *path)
{
    if (!path) return false;
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return strcmp(dot, ".scene")    == 0 ||
           strcmp(dot, ".prefab")   == 0 ||
           strcmp(dot, ".material") == 0 ||
           strcmp(dot, ".asset")    == 0;
}

static const char *path_leaf(const char *path)
{
    if (!path) return "";
    const char *slash = strrchr(path, '/');
    const char *bsl   = strrchr(path, '\\');
    const char *p = slash > bsl ? slash : bsl;
    return p ? p + 1 : path;
}

static void json_collect_names(const JceJson *node, std::vector<std::string> *out,
                               int *budget)
{
    if (!node || *budget <= 0) return;
    if (jce_json_is_object(node)) {
        for (JceJson *it = jce_json_first_child(node); it && *budget > 0;
             it = jce_json_next_sibling(it)) {
            const char *k = jce_json_member_key(it);
            if (k && (strcmp(k, "name") == 0 || strcmp(k, "displayName") == 0)
                && jce_json_is_string(it)) {
                const char *v = jce_json_string_value(it, NULL);
                if (v && *v) {
                    out->emplace_back(v);
                    (*budget)--;
                }
            }
            json_collect_names(it, out, budget);
        }
    } else if (jce_json_is_array(node)) {
        int n = jce_json_array_size(node);
        for (int i = 0; i < n && *budget > 0; i++)
            json_collect_names(jce_json_array_at(node, i), out, budget);
    }
}

/* Returns a pointer into s_json_cache; never NULL. Refreshes the cached
 * entry when the file's mtime advances. */
static const std::vector<std::string> *json_names_for(const char *path)
{
    int64_t mt = 0;
    jce_fs_host_get_mtime(path, &mt);

    auto it = s_json_cache.find(path);
    if (it != s_json_cache.end() && it->second.mtime == mt)
        return &it->second.names;

    JsonContentEntry entry;
    entry.mtime = mt;

    size_t sz = 0;
    char  *buf = (char *)ed_read_file(path, &sz);
    if (buf) {
        JceJson *root = jce_json_parse(buf, sz);
        if (root) {
            int budget = 64;   /* per-file string scan budget */
            json_collect_names(root, &entry.names, &budget);
            jce_json_free(root);
        }
        ED_FREE(buf);
    }

    auto ins = s_json_cache.insert_or_assign(std::string(path), std::move(entry));
    return &ins.first->second.names;
}

struct WalkCtx {
    const char           *q;
    std::vector<PathHit> *out;
    int                   max_hits;
};

static bool project_walk_cb(const char *path, bool is_dir, void *user)
{
    auto *c = static_cast<WalkCtx *>(user);
    if (is_dir) return true;
    if ((int)c->out->size() >= c->max_hits) return false;

    /* Path/filename hit (legacy behaviour). */
    if (ci_contains(path, c->q)) {
        PathHit h;
        h.path  = path;
        h.label = path;
        c->out->push_back(std::move(h));
        if ((int)c->out->size() >= c->max_hits) return false;
    }

    /* JSON-content hits (.scene/.prefab/.material/.asset). */
    if (is_json_asset(path)) {
        const std::vector<std::string> *names = json_names_for(path);
        if (names) {
            const char *leaf = path_leaf(path);
            for (const auto &name : *names) {
                if ((int)c->out->size() >= c->max_hits) return false;
                if (ci_contains(name.c_str(), c->q)) {
                    PathHit h;
                    h.path  = path;
                    h.label = std::string(leaf) + " :: " + name;
                    c->out->push_back(std::move(h));
                }
            }
        }
    }
    return true;
}

/* ── Dispatcher ────────────────────────────────────────────────────── */

static void rescan(void)
{
    s_scene_hits.clear();
    s_mp_hits.clear();
    s_path_hits.clear();
    if (s_query[0] == '\0') return;

    if (s_tab == 0) {
        JceScene *scene = jce_state_get_scene();
        if (!scene) return;
        ScCtx ctx{ s_query, &s_scene_hits };
        jce_scene_each_entity(scene, scene_scan_cb, &ctx);
    } else if (s_tab == 1) {
        scan_menus_panels(s_query);
    } else {
        const char *root = jce_editor_assets_get_project();
        if (!root || !*root) return;
        WalkCtx ctx{ s_query, &s_path_hits, /*max_hits=*/500 };
        jce_fs_host_walk(root, project_walk_cb, &ctx);
    }
}

/* ── UI ────────────────────────────────────────────────────────────── */

static void open_panel(JceEditorPanel p)
{
    bool *vis = jce_editor_panel_visible_ptr(p);
    if (vis) *vis = true;
}

extern "C" void jce_editor_panel_search_content(void)
{
    ImGui::SetNextItemWidth(-100);
    bool query_changed = ImGui::InputTextWithHint(
        "##q", jce_editor_i18n("search.hint.anyLang"), s_query, sizeof(s_query));
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
        if (ImGui::BeginTabItem(jce_editor_i18n("search.tab.menusPanels"))) {
            if (s_tab != 1) { s_tab = 1; rescan(); }
            ImGui::Text("%s %zu", jce_editor_i18n("search.hits"), s_mp_hits.size());
            ImGui::Separator();
            if (ImGui::BeginChild("##mp", ImVec2(0,0))) {
                int idx = 0;
                for (auto &h : s_mp_hits) {
                    ImGui::PushID(idx++);
                    /* "[EN]  Lighting Settings" */
                    char row[256];
                    snprintf(row, sizeof(row), "[%s]  %s",
                             h.locale_tag, h.path.c_str());
                    if (ImGui::Selectable(row) && h.is_panel)
                        open_panel(h.panel);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("search.tab.project"))) {
            if (s_tab != 2) { s_tab = 2; rescan(); }
            ImGui::Text("%s %zu", jce_editor_i18n("search.hits"), s_path_hits.size());
            ImGui::Separator();
            if (ImGui::BeginChild("##pj", ImVec2(0,0))) {
                int idx = 0;
                for (auto &h : s_path_hits) {
                    ImGui::PushID(idx++);
                    if (ImGui::Selectable(h.label.c_str()))
                        jce_file_viewer_open(h.path.c_str());
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
