/*
 * jce_workspace.h  Maya-style Workspace / Menu Set system.
 *
 * A Workspace bundles three independent UI affordances under a single
 * named preset:
 *
 *   1. Dock layout (reuses the 10 layout presets owned by
 *      jce_editor_layout.cpp; see s_layout_preset_pending).
 *   2. Menu-set mask — a bit per workspace driving conditional
 *      visibility of "task-specific" menu groups (Modeling / Rigging /
 *      Animation / FX / Rendering). Always-visible menus (File / Edit /
 *      Window / Help / Workspace) carry a mask of 0xFFFFFFFFu.
 *   3. Default panel set — panels that must be visible when the
 *      workspace is activated. Missing panels are silently skipped.
 *
 * Switching workspace via jce_workspace_set_active() applies all three at
 * once and persists the choice to JceEditorConfig.workspace_id.
 */

#ifndef JCE_WORKSPACE_H
#define JCE_WORKSPACE_H

#include "ui/jce_editor_panels.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_WORKSPACE_DEFAULT = 0,
    JCE_WORKSPACE_MODELING,
    JCE_WORKSPACE_RIGGING,
    JCE_WORKSPACE_ANIMATION,
    JCE_WORKSPACE_FX,
    JCE_WORKSPACE_RENDERING,
    JCE_WORKSPACE_UV_EDITING,
    JCE_WORKSPACE_SCULPTING,
    JCE_WORKSPACE_COUNT
} JceWorkspaceId;

typedef struct {
    const char         *id_str;             /* "modeling" — persisted */
    const char         *i18n_label_key;     /* "workspace.modeling"   */
    int                 layout_preset_idx;  /* 0..9 (see jce_editor_layout.cpp) */
    uint32_t            menu_set_mask;      /* bit per workspace (1u << id) */
    const JceEditorPanel *default_panels;   /* panels forced visible on activation */
    int                 default_panels_count;
} JceWorkspaceDef;

/* Lookup the static definition for a workspace. Returns NULL for invalid id. */
const JceWorkspaceDef *jce_workspace_def(JceWorkspaceId id);

/* Currently active workspace. Starts as JCE_WORKSPACE_DEFAULT until
   jce_workspace_init() restores the persisted value. */
JceWorkspaceId jce_workspace_get_active(void);

/* Switch workspace: applies layout preset + sets menu mask + forces
   default panels visible + persists id to JceEditorConfig. No-op when
   id == current and not forced. */
void jce_workspace_set_active(JceWorkspaceId id);

/* Initialize from persisted JceEditorConfig.workspace_id. Must be called
   after jce_editor_panels_init() and jce_hotkeys_init(). */
void jce_workspace_init(void);

/* Convenience: resolve an id_str ("modeling") to its enum value.
   Returns JCE_WORKSPACE_DEFAULT when no match. */
JceWorkspaceId jce_workspace_id_from_string(const char *id_str);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WORKSPACE_H */
