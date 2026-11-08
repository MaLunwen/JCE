#pragma once

#include "core/jce_editor_i18n.h"
#include "core/jce_hotkeys.h"

#include <cstdio>

/* Match the registry's stable ID and the translation used by its display name. */
inline bool jce_panel_hotkey_matches_filter(JceHotkeyId id, const char *query)
{
    if (!query || !*query) return true;
    const char *action = jce_hotkey_id_string(id);
    if (jce_editor_i18n_search_match(nullptr, action, query, nullptr)) return true;
    char key[96];
    std::snprintf(key, sizeof(key), "hotkey.%s", action);
    return jce_editor_i18n_search_match(key, jce_hotkey_name(id), query, nullptr);
}
