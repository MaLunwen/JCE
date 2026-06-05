/*
 * jce_editor_ui_state.cpp  Per-user editor UI state persistence.
 */

#include "jce_editor_ui_state.h"

#include "core/jce_editor_config.h"

static int clamp_int(int value, int min_value, int max_value)
{
    if (max_value < min_value)
        return value;
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

extern "C" int jce_editor_ui_state_load_int(const char *key,
                                             int fallback,
                                             int min_value,
                                             int max_value)
{
    JceEditorConfig cfg;
    if (!jce_editor_config_load(&cfg))
        jce_editor_config_defaults(&cfg);

    int value = jce_editor_config_get_ui_int_or(&cfg, key, fallback);
    return clamp_int(value, min_value, max_value);
}

extern "C" void jce_editor_ui_state_save_int(const char *key, int value)
{
    JceEditorConfig cfg;
    if (!jce_editor_config_load(&cfg))
        jce_editor_config_defaults(&cfg);

    if (jce_editor_config_set_ui_int(&cfg, key, value))
        (void)jce_editor_config_save(&cfg);
}
