/*
 * jce_editor_config.h  Editor configuration persistence.
 */

#ifndef JCE_EDITOR_CONFIG_H
#define JCE_EDITOR_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char language[8];          /* "en" or "zh_cn" */
    int  font_size;            /* 12-32 */
    char theme[16];            /* "Dark", "Light", "Blue" */
    char renderer[16];         /* "OpenGL", "Vulkan" */
    char last_project[512];
    char recent_projects[10][512];
    int  recent_count;
} JceEditorConfig;

/* Load config from .jce/editor-config.json. Returns false if not found. */
bool jce_editor_config_load(JceEditorConfig *cfg);

/* Save config to .jce/editor-config.json. */
bool jce_editor_config_save(const JceEditorConfig *cfg);

/* Set defaults. */
void jce_editor_config_defaults(JceEditorConfig *cfg);

/* Add a path to recent projects (front of list, deduped, max 10). */
void jce_editor_config_add_recent(JceEditorConfig *cfg, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_CONFIG_H */
