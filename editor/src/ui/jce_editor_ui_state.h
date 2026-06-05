/*
 * jce_editor_ui_state.h  Small helpers for per-user editor UI state.
 */

#ifndef JCE_EDITOR_UI_STATE_H
#define JCE_EDITOR_UI_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

int  jce_editor_ui_state_load_int(const char *key,
                                   int fallback,
                                   int min_value,
                                   int max_value);
void jce_editor_ui_state_save_int(const char *key, int value);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_UI_STATE_H */
