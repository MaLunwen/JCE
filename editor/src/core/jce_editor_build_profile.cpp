/*
 * jce_editor_build_profile.cpp  Per-project build/run profile overlay.
 */

#include "jce_editor_build_profile.h"
#include "jce_editor_config.h"
#include "jce_editor_project_state.h"

#include <stdio.h>
#include <string.h>

/* The machine-local build/run fields carried by JceEditorConfig.  Kept as
 * a table so snapshot and restore can never drift out of sync. */
namespace {

struct StrField { const char *key; char *(*ptr)(JceEditorConfig *); size_t cap; };

/* Accessors: the fields are fixed char arrays inside JceEditorConfig. */
#define BP_STR(field, keyname)                                             \
    { keyname, [](JceEditorConfig *c) -> char * { return c->field; },       \
      sizeof(((JceEditorConfig *)0)->field) }

const StrField kStrFields[] = {
    BP_STR(game_executable_path,   "build.exe"),
    BP_STR(game_working_directory, "build.workdir"),
    BP_STR(game_target_name,       "build.target"),
    BP_STR(build_configure_preset, "build.configure_preset"),
    BP_STR(build_preset,           "build.preset"),
    BP_STR(build_output_path,      "build.output"),
    BP_STR(build_project_root,     "build.project_root"),
};

} /* namespace */

extern "C" void jce_editor_build_profile_snapshot(void)
{
    if (!jce_editor_pstate_active()) return;
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    jce_editor_pstate_set_int("build.run_mode", cfg.run_mode);
    for (const auto &f : kStrFields)
        jce_editor_pstate_set_str(f.key, f.ptr(&cfg));
}

extern "C" void jce_editor_build_profile_restore(void)
{
    if (!jce_editor_pstate_active()) return;
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);

    /* Presence probe: build.target is always non-empty once seeded. */
    char probe[8];
    bool seeded = jce_editor_pstate_get_str("build.target", probe, sizeof(probe));
    if (!seeded) {
        /* First sight of this project: seed the store from the current
         * (global-default or last-used) profile so the next switch is
         * authoritative, but leave the live cfg untouched. */
        jce_editor_build_profile_snapshot();
        return;
    }

    bool changed = false;
    int rm = jce_editor_pstate_get_int("build.run_mode", cfg.run_mode);
    if (rm != cfg.run_mode) { cfg.run_mode = rm; changed = true; }
    for (const auto &f : kStrFields) {
        char buf[512];
        if (jce_editor_pstate_get_str(f.key, buf, sizeof(buf))) {
            char *dst = f.ptr(&cfg);
            if (strncmp(dst, buf, f.cap) != 0) {
                strncpy(dst, buf, f.cap - 1);
                dst[f.cap - 1] = '\0';
                changed = true;
            }
        }
    }
    if (changed) jce_editor_config_save(&cfg);
}

/* ── Asset-browser favourites (Locations sidebar) ──────────────────── */

/* C++ linkage (matches the definition in dialogs/jce_dialog_project.cpp). */
extern char s_current_project_root[512];

extern "C" void jce_editor_favorites_snapshot(void)
{
    if (!jce_editor_pstate_active()) return;
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    jce_editor_pstate_set_int("fav.count", cfg.asset_favorite_count);
    for (int i = 0; i < 12; i++) {
        char key[16];
        snprintf(key, sizeof(key), "fav.%d", i);
        jce_editor_pstate_set_str(key,
            i < cfg.asset_favorite_count ? cfg.asset_favorites[i] : "");
    }
}

extern "C" void jce_editor_favorites_restore(void)
{
    if (!jce_editor_pstate_active()) return;
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);

    int saved_n = jce_editor_pstate_get_int("fav.count", -1);
    if (saved_n < 0) {
        /* First sight: keep only the global favourites that live under this
         * project's root, then seed the store from that filtered set. */
        size_t root_len = strlen(s_current_project_root);
        int kept = 0;
        for (int i = 0; i < cfg.asset_favorite_count; i++) {
            bool under = root_len > 0 &&
                strncmp(cfg.asset_favorites[i], s_current_project_root,
                        root_len) == 0;
            if (under && kept != i)
                strncpy(cfg.asset_favorites[kept], cfg.asset_favorites[i],
                        sizeof(cfg.asset_favorites[0]) - 1);
            if (under) kept++;
        }
        for (int i = kept; i < cfg.asset_favorite_count; i++)
            cfg.asset_favorites[i][0] = '\0';
        cfg.asset_favorite_count = kept;
        jce_editor_config_save(&cfg);
        jce_editor_favorites_snapshot();
        return;
    }

    if (saved_n > 12) saved_n = 12;
    for (int i = 0; i < saved_n; i++) {
        char key[16];
        snprintf(key, sizeof(key), "fav.%d", i);
        char buf[512];
        if (!jce_editor_pstate_get_str(key, buf, sizeof(buf))) buf[0] = '\0';
        strncpy(cfg.asset_favorites[i], buf, sizeof(cfg.asset_favorites[0]) - 1);
        cfg.asset_favorites[i][sizeof(cfg.asset_favorites[0]) - 1] = '\0';
    }
    for (int i = saved_n; i < 12; i++) cfg.asset_favorites[i][0] = '\0';
    cfg.asset_favorite_count = saved_n;
    jce_editor_config_save(&cfg);
}
