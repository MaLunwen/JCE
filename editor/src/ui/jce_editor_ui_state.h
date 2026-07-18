/*
 * jce_editor_ui_state.h  Small helpers for per-user editor UI state.
 */

#ifndef JCE_EDITOR_UI_STATE_H
#define JCE_EDITOR_UI_STATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int  jce_editor_ui_state_load_int(const char *key,
                                   int fallback,
                                   int min_value,
                                   int max_value);
void jce_editor_ui_state_save_int(const char *key, int value);

/* Float / string variants (generic KV on the config singleton; persisted
 * to editor-session.json).  Pass max < min to skip clamping.  load_str
 * returns false and writes `fallback` when the key is absent. */
float jce_editor_ui_state_load_float(const char *key, float fallback,
                                     float min_value, float max_value);
void  jce_editor_ui_state_save_float(const char *key, float value);
bool  jce_editor_ui_state_load_str(const char *key, char *out, size_t cap,
                                   const char *fallback);
void  jce_editor_ui_state_save_str(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_UI_STATE_H */
