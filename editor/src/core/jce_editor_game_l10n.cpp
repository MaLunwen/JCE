/*
 * jce_editor_game_l10n.cpp  Editor-side GAME localization model.
 *
 * See header for the contract.  Storage is a per-locale ordered map of
 * key→value plus a sorted union key list (minus hidden _meta.* entries).
 * Save writes the UNION of keys to every locale file (missing values as
 * "") so locale files never drift out of key parity.
 */

#include "core/jce_editor_game_l10n.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <jce/middleware/ui/jce_localization.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define LOG_TAG "gl10n"

namespace {

struct Gl10nState {
    bool        loaded = false;
    bool        dirty  = false;
    std::string dir;                    /* <root>/<source_assets>/i18n */
    std::vector<std::string> locales;   /* "en" first, then sorted */
    std::vector<std::string> keys;      /* sorted union, no _meta.* */
    std::map<std::string, std::map<std::string, std::string>> tables;
};

Gl10nState g_l10n;

/* Legacy fixed-enum jce_i18n keys (engine/src/os/core/jce_i18n.c
 * s_key_map) — consumed by the built-in pause/settings menu.  Keep in
 * sync with that table. */
const char *const k_protected_keys[] = {
    "paused", "continue", "quit", "controls_hint", "text_demo",
    "settings", "video", "audio", "fullscreen", "resolution",
    "vsync", "master_volume", "music_volume", "sfx_volume",
    "ok", "cancel", "apply", "on", "off",
};

bool is_meta_key(const char *k)
{
    return std::strncmp(k, "_meta.", 6) == 0;
}

void sort_locales(void)
{
    std::sort(g_l10n.locales.begin(), g_l10n.locales.end(),
              [](const std::string &a, const std::string &b) {
                  bool ae = (a == "en"), be = (b == "en");
                  if (ae != be) return ae;   /* "en" first */
                  return a < b;
              });
}

void rebuild_key_union(void)
{
    g_l10n.keys.clear();
    for (const auto &loc : g_l10n.tables) {
        for (const auto &kv : loc.second) {
            if (is_meta_key(kv.first.c_str())) continue;
            g_l10n.keys.push_back(kv.first);
        }
    }
    std::sort(g_l10n.keys.begin(), g_l10n.keys.end());
    g_l10n.keys.erase(std::unique(g_l10n.keys.begin(), g_l10n.keys.end()),
                      g_l10n.keys.end());
}

/* Re-point the process-global jce_loc table at `dir` while preserving the
 * active preview locale across the re-init (jce_loc_init clears it). */
void repoint_jce_loc(const char *dir)
{
    char prev[64] = {0};
    const char *cur = jce_loc_get_locale();
    if (cur && cur[0])
        snprintf(prev, sizeof(prev), "%s", cur);
    jce_loc_init(dir && dir[0] ? dir : nullptr);
    if (dir && dir[0])
        jce_loc_set_locale(prev[0] ? prev : "en");
}

bool parse_locale_file(const char *path, const char *code)
{
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
                      "could not parse locale file: %s", path);
        return false;
    }
    auto &table = g_l10n.tables[code];
    for (JceJson *it = jce_json_first_child(root); it;
         it = jce_json_next_sibling(it)) {
        const char *k = jce_json_member_key(it);
        const char *v = jce_json_string_value(it, nullptr);
        if (k && v) table[k] = v;
    }
    jce_json_free(root);
    return true;
}

} // namespace

bool jce_editor_gl10n_load(const char *project_root, const char *source_assets)
{
    jce_editor_gl10n_unload();
    if (!project_root || !project_root[0]) return false;

    const char *src = (source_assets && source_assets[0]) ? source_assets
                                                          : "assets";
    char src_dir[1024], i18n_dir[1024];
    jce_path_join(src_dir, sizeof(src_dir), project_root, src);
    jce_path_join(i18n_dir, sizeof(i18n_dir), src_dir, "i18n");

    g_l10n.dir    = i18n_dir;
    g_l10n.loaded = true;

    /* Enumerate <dir>/*.json (leaf names; stem = locale code). */
    std::vector<std::string> files;
    auto cb = [](const char *name, bool is_dir, void *ud) -> bool {
        if (!is_dir) {
            size_t n = std::strlen(name);
            if (n > 5 && std::strcmp(name + n - 5, ".json") == 0)
                static_cast<std::vector<std::string> *>(ud)->push_back(name);
        }
        return true;
    };
    jce_fs_host_list_dir(i18n_dir, cb, &files);
    std::sort(files.begin(), files.end());

    int parsed = 0;
    for (const auto &f : files) {
        std::string code = f.substr(0, f.size() - 5);
        if (code.empty()) continue;
        char path[1024];
        jce_path_join(path, sizeof(path), i18n_dir, f.c_str());
        g_l10n.locales.push_back(code);
        if (parse_locale_file(path, code.c_str())) ++parsed;
    }
    sort_locales();
    rebuild_key_union();

    /* Editor-side jce_loc init point: game view / Play preview resolve
     * locale_key against this host dir from now on. */
    repoint_jce_loc(i18n_dir);

    jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                  "game string tables: %d locale(s), %d key(s) from %s",
                  (int)g_l10n.locales.size(), (int)g_l10n.keys.size(),
                  i18n_dir);
    return parsed > 0;
}

void jce_editor_gl10n_unload(void)
{
    if (!g_l10n.loaded && g_l10n.dir.empty()) return;
    g_l10n = Gl10nState{};
    /* Detach jce_loc from the previous project (empties the table so the
     * preview falls back to authored UIText text). */
    repoint_jce_loc(nullptr);
}

bool jce_editor_gl10n_loaded(void)      { return g_l10n.loaded; }
const char *jce_editor_gl10n_dir(void)  { return g_l10n.dir.c_str(); }

int jce_editor_gl10n_locale_count(void)
{
    return (int)g_l10n.locales.size();
}

const char *jce_editor_gl10n_locale_code_at(int i)
{
    if (i < 0 || i >= (int)g_l10n.locales.size()) return "";
    return g_l10n.locales[(size_t)i].c_str();
}

int jce_editor_gl10n_key_count(void)
{
    return (int)g_l10n.keys.size();
}

const char *jce_editor_gl10n_key_at(int i)
{
    if (i < 0 || i >= (int)g_l10n.keys.size()) return "";
    return g_l10n.keys[(size_t)i].c_str();
}

const char *jce_editor_gl10n_get(const char *locale, const char *key)
{
    if (!locale || !key) return nullptr;
    auto t = g_l10n.tables.find(locale);
    if (t == g_l10n.tables.end()) return nullptr;
    auto v = t->second.find(key);
    if (v == t->second.end()) return nullptr;
    return v->second.c_str();
}

void jce_editor_gl10n_set(const char *locale, const char *key,
                          const char *value)
{
    if (!locale || !locale[0] || !key || !key[0]) return;
    g_l10n.tables[locale][key] = value ? value : "";
    if (!is_meta_key(key)) {
        auto it = std::lower_bound(g_l10n.keys.begin(), g_l10n.keys.end(),
                                   key);
        if (it == g_l10n.keys.end() || *it != key)
            g_l10n.keys.insert(it, key);
    }
    g_l10n.dirty = true;
}

bool jce_editor_gl10n_add_key(const char *key)
{
    if (!g_l10n.loaded || !key || !key[0] || is_meta_key(key)) return false;
    auto it = std::lower_bound(g_l10n.keys.begin(), g_l10n.keys.end(), key);
    if (it != g_l10n.keys.end() && *it == key) return false;
    g_l10n.keys.insert(it, key);
    g_l10n.dirty = true;
    return true;
}

bool jce_editor_gl10n_remove_key(const char *key)
{
    if (!g_l10n.loaded || !key || !key[0]) return false;
    if (jce_editor_gl10n_key_protected(key)) return false;
    auto it = std::lower_bound(g_l10n.keys.begin(), g_l10n.keys.end(), key);
    if (it == g_l10n.keys.end() || *it != key) return false;
    g_l10n.keys.erase(it);
    for (auto &loc : g_l10n.tables)
        loc.second.erase(key);
    g_l10n.dirty = true;
    return true;
}

bool jce_editor_gl10n_add_locale(const char *code)
{
    if (!g_l10n.loaded || !code || !code[0]) return false;
    /* Lowercase "lang[_country]" tags only — they become filenames. */
    for (const char *p = code; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_'))
            return false;
    }
    for (const auto &l : g_l10n.locales)
        if (l == code) return false;
    g_l10n.locales.push_back(code);
    sort_locales();
    g_l10n.tables[code];   /* materialise an empty table */
    g_l10n.dirty = true;
    return true;
}

bool jce_editor_gl10n_key_protected(const char *key)
{
    if (!key) return false;
    for (const char *pk : k_protected_keys)
        if (std::strcmp(pk, key) == 0) return true;
    return false;
}

bool jce_editor_gl10n_save_all(void)
{
    if (!g_l10n.loaded || g_l10n.dir.empty()) return false;
    if (!jce_fs_host_create_directory(g_l10n.dir.c_str())) {
        jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
                      "could not create i18n dir: %s", g_l10n.dir.c_str());
        return false;
    }

    bool all_ok = true;
    for (const auto &code : g_l10n.locales) {
        const auto &table = g_l10n.tables[code];

        JceJson *root = jce_json_object();
        if (!root) { all_ok = false; continue; }

        /* Hidden _meta.* entries first ('_' sorts before letters, matching
         * the on-disk convention), then the sorted key union — absent
         * values are written as "" so files keep key parity. */
        for (const auto &kv : table) {
            if (is_meta_key(kv.first.c_str()))
                jce_json_set_string(root, kv.first.c_str(),
                                    kv.second.c_str());
        }
        for (const auto &k : g_l10n.keys) {
            auto v = table.find(k);
            jce_json_set_string(root, k.c_str(),
                                v != table.end() ? v->second.c_str() : "");
        }

        char path[1024];
        jce_path_join(path, sizeof(path), g_l10n.dir.c_str(),
                      (code + ".json").c_str());
        if (!jce_json_write_file(path, root, true, true)) {
            jce_log_write(JCE_LOG_LEVEL_ERROR, LOG_TAG, __FILE__, __LINE__,
                          "save failed: %s", path);
            all_ok = false;
        }
    }

    if (all_ok) g_l10n.dirty = false;

    /* Hot-refresh the live preview / Play session: re-fire the active
     * locale so jce_loc re-reads the files we just wrote.  (Copy the tag —
     * jce_loc_set_locale snprintf's into the same buffer get returns.) */
    char cur[64] = {0};
    const char *tag = jce_loc_get_locale();
    if (tag && tag[0]) {
        snprintf(cur, sizeof(cur), "%s", tag);
        jce_loc_set_locale(cur);
    }
    return all_ok;
}

bool jce_editor_gl10n_dirty(void) { return g_l10n.dirty; }
