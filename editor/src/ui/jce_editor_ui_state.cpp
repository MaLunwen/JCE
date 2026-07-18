/*
 * jce_editor_ui_state.cpp  Per-user editor UI state persistence.
 *
 * Thin wrappers over the config singleton's generic KV stores.  Cheap to
 * call per frame: load is a table lookup on the in-memory singleton and
 * save marks the debounced flush dirty (no per-call disk I/O).
 */

#include "jce_editor_ui_state.h"

#include "core/jce_editor_config.h"

#include <string.h>

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

    int cur;
    if (jce_editor_config_get_ui_int(&cfg, key, &cur) && cur == value)
        return;   /* unchanged: skip the whole-struct save */
    if (jce_editor_config_set_ui_int(&cfg, key, value))
        (void)jce_editor_config_save(&cfg);
}

extern "C" float jce_editor_ui_state_load_float(const char *key,
                                                float fallback,
                                                float min_value,
                                                float max_value)
{
    float v = jce_editor_config_get_ui_float_or(key, fallback);
    if (max_value >= min_value) {
        if (v < min_value) v = min_value;
        if (v > max_value) v = max_value;
    }
    return v;
}

extern "C" void jce_editor_ui_state_save_float(const char *key, float value)
{
    jce_editor_config_set_ui_float(key, value);
}

extern "C" bool jce_editor_ui_state_load_str(const char *key,
                                             char *out, size_t cap,
                                             const char *fallback)
{
    if (jce_editor_config_get_ui_str(key, out, cap))
        return true;
    if (out && cap) {
        if (fallback) { strncpy(out, fallback, cap - 1); out[cap - 1] = '\0'; }
        else out[0] = '\0';
    }
    return false;
}

extern "C" void jce_editor_ui_state_save_str(const char *key, const char *value)
{
    jce_editor_config_set_ui_str(key, value);
}
