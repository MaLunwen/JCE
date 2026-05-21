/*
 * jce_workspace.cpp  Implementation of the Maya-style Workspace system.
 *
 * The workspace table is the single source of truth for which dock
 * layout, menu set, and default panels go together. The menu bar in
 * jce_editor_layout.cpp reads jce_workspace_get_active() each frame and
 * filters task-specific menu groups against the matching mask.
 */

#include "jce_workspace.h"
#include "jce_editor_config.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_layout.h"

#include <string.h>

/* ── Workspace definitions ────────────────────────────────────────────
 *
 * Each entry maps a workspace to:
 *   - its persisted id string
 *   - its i18n label key (consumed by the dropdown + hotkey labels)
 *   - the layout preset index reused from jce_editor_layout.cpp
 *   - a menu-set mask (bit position == workspace id)
 *   - a small list of panels that should be visible on activation.
 *     Missing panel enums are silently dropped (see apply_workspace_).
 */

/* Default panels per workspace.
 *
 * P8-E cleanup: previously these lists referenced shim panel enums
 * (TIMELINE / ANIMATOR_SM / CURVE_EDITOR / VFX_GRAPH / PARTICLE_EDITOR /
 * LIGHTMAP_BAKE / FRAME_DEBUGGER / ANIMATION_RIGGING). Activating a shim
 * sets its visibility, which on the next draw redirects to a workbench
 * host AND requests one specific tab — so for a five-shim list only the
 * last request_tab wins and the cascade flips visibility flags on and
 * off the same frame. Replaced with direct workbench-host enums so each
 * workspace opens its hosts cleanly at their last-active tab. */

static const JceEditorPanel s_panels_default[] = {
    JCE_PANEL_SCENE_VIEW, JCE_PANEL_HIERARCHY, JCE_PANEL_INSPECTOR,
    JCE_PANEL_ASSETS, JCE_PANEL_CONSOLE,
};

static const JceEditorPanel s_panels_modeling[] = {
    JCE_PANEL_SCENE_VIEW, JCE_PANEL_HIERARCHY, JCE_PANEL_INSPECTOR,
    JCE_PANEL_ASSETS,
};

static const JceEditorPanel s_panels_rigging[] = {
    JCE_PANEL_SCENE_VIEW, JCE_PANEL_HIERARCHY, JCE_PANEL_INSPECTOR,
    JCE_PANEL_ANIMATION_EDITOR, /* host: Rigging tab lives inside */
};

static const JceEditorPanel s_panels_animation[] = {
    JCE_PANEL_SCENE_VIEW, JCE_PANEL_GAME_VIEW,
    JCE_PANEL_ANIMATION_EDITOR, /* host: Timeline / Curves / StateMachine tabs */
};

static const JceEditorPanel s_panels_fx[] = {
    JCE_PANEL_SCENE_VIEW,
    JCE_PANEL_MATERIAL_GRAPH, /* host: VFX / Particle / Shader tabs */
    JCE_PANEL_AUDIO_MIXER,
};

static const JceEditorPanel s_panels_rendering[] = {
    JCE_PANEL_SCENE_VIEW,
    JCE_PANEL_LIGHTING_SETTINGS, /* host: Lightmap / Reflection / ToD / Pipeline */
    JCE_PANEL_MATERIAL_GRAPH,    /* host: Material / Shader tabs */
    JCE_PANEL_PROFILER,          /* host: Frame Debugger / Memory tabs */
};

static const JceEditorPanel s_panels_uv[] = {
    JCE_PANEL_SCENE_VIEW, JCE_PANEL_INSPECTOR,
};

static const JceEditorPanel s_panels_sculpting[] = {
    JCE_PANEL_SCENE_VIEW, JCE_PANEL_INSPECTOR,
};

#define JCE_WS_BIT(id) (1u << (unsigned)(id))
#define JCE_WS_PANELS(arr) (arr), (int)(sizeof(arr) / sizeof((arr)[0]))

static const JceWorkspaceDef s_workspaces[JCE_WORKSPACE_COUNT] = {
    { "default",   "workspace.default",   0, JCE_WS_BIT(JCE_WORKSPACE_DEFAULT),
        JCE_WS_PANELS(s_panels_default) },
    { "modeling",  "workspace.modeling",  0, JCE_WS_BIT(JCE_WORKSPACE_MODELING),
        JCE_WS_PANELS(s_panels_modeling) },
    { "rigging",   "workspace.rigging",   2, JCE_WS_BIT(JCE_WORKSPACE_RIGGING),
        JCE_WS_PANELS(s_panels_rigging) },
    { "animation", "workspace.animation", 2, JCE_WS_BIT(JCE_WORKSPACE_ANIMATION),
        JCE_WS_PANELS(s_panels_animation) },
    { "fx",        "workspace.fx",        0, JCE_WS_BIT(JCE_WORKSPACE_FX),
        JCE_WS_PANELS(s_panels_fx) },
    { "rendering", "workspace.rendering", 9, JCE_WS_BIT(JCE_WORKSPACE_RENDERING),
        JCE_WS_PANELS(s_panels_rendering) },
    { "uvEditing", "workspace.uvEditing", 0, JCE_WS_BIT(JCE_WORKSPACE_UV_EDITING),
        JCE_WS_PANELS(s_panels_uv) },
    { "sculpting", "workspace.sculpting", 0, JCE_WS_BIT(JCE_WORKSPACE_SCULPTING),
        JCE_WS_PANELS(s_panels_sculpting) },
};

#undef JCE_WS_BIT
#undef JCE_WS_PANELS

static JceWorkspaceId s_active = JCE_WORKSPACE_DEFAULT;

/* Force the listed panels visible. Panel enums outside [0,JCE_PANEL_COUNT)
   are dropped silently — guards against future enum churn. */
static void apply_workspace_panels_(const JceWorkspaceDef *def)
{
    if (!def || !def->default_panels) return;
    for (int i = 0; i < def->default_panels_count; ++i) {
        JceEditorPanel p = def->default_panels[i];
        if (p < 0 || p >= JCE_PANEL_COUNT) continue;
        bool *v = jce_editor_panel_visible_ptr(p);
        if (v) *v = true;
    }
}

extern "C" const JceWorkspaceDef *jce_workspace_def(JceWorkspaceId id)
{
    if (id < 0 || id >= JCE_WORKSPACE_COUNT) return NULL;
    return &s_workspaces[id];
}

extern "C" JceWorkspaceId jce_workspace_get_active(void)
{
    return s_active;
}

extern "C" JceWorkspaceId jce_workspace_id_from_string(const char *id_str)
{
    if (!id_str || !id_str[0]) return JCE_WORKSPACE_DEFAULT;
    for (int i = 0; i < (int)JCE_WORKSPACE_COUNT; ++i) {
        if (strcmp(s_workspaces[i].id_str, id_str) == 0)
            return (JceWorkspaceId)i;
    }
    return JCE_WORKSPACE_DEFAULT;
}

extern "C" void jce_workspace_set_active(JceWorkspaceId id)
{
    if (id < 0 || id >= JCE_WORKSPACE_COUNT) return;
    const JceWorkspaceDef *def = &s_workspaces[id];
    s_active = id;

    /* 1. Schedule the layout preset switch — actual rebuild happens
       inside the next jce_editor_layout_draw() frame. */
    jce_editor_layout_request_preset(def->layout_preset_idx);

    /* 2. Force the workspace's default panels visible. The layout
       reset that fires next frame will also turn most panels back on,
       but this call guarantees the workspace-critical ones are up
       even if a future preset chooses to hide them. */
    apply_workspace_panels_(def);

    /* 3. Persist the choice. Load → mutate → save mirrors the pattern
       used by the recent-projects menu and the Preferences panel. */
    JceEditorConfig cfg;
    if (!jce_editor_config_load(&cfg))
        jce_editor_config_defaults(&cfg);
    strncpy(cfg.workspace_id, def->id_str, sizeof(cfg.workspace_id) - 1);
    cfg.workspace_id[sizeof(cfg.workspace_id) - 1] = '\0';
    jce_editor_config_save(&cfg);
}

extern "C" void jce_workspace_init(void)
{
    JceEditorConfig cfg;
    if (!jce_editor_config_load(&cfg))
        jce_editor_config_defaults(&cfg);
    s_active = jce_workspace_id_from_string(cfg.workspace_id);
    /* Note: no layout / panel application on init — the editor's saved
       imgui.ini already encodes the user's last dock arrangement, and
       forcing a preset rebuild here would clobber it. The menu mask
       still takes effect because the menu bar reads s_active live. */
}
