/*
 * jce_guide_ch_inspector_components.cpp — User guide chapter: Inspector & Components
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

static const JceGuideBlock b_overview[] = {
    { JCE_GB_P,          "guide.inspector.overview.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector", JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_OPEN_PANEL, "Hierarchy", JCE_PANEL_HIERARCHY, "###hierarchy" },
    { JCE_GB_HOTKEY,     "guide.inspector.overview.hk1", JCE_HK_PANEL_INSPECTOR, nullptr },
    { JCE_GB_STEP,       "guide.inspector.overview.s1", 0, nullptr },
    { JCE_GB_STEP,       "guide.inspector.overview.s2", 0, nullptr },
    { JCE_GB_STEP,       "guide.inspector.overview.s3", 0, nullptr },
    { JCE_GB_STEP,       "guide.inspector.overview.s4", 0, nullptr },
    { JCE_GB_STEP,       "guide.inspector.overview.s5", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.tags_layers.title", JCE_PANEL_TAGS_LAYERS, "###tags_layers" },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.file>menu.file.project_settings" },
    { JCE_GB_TIP,        "guide.inspector.overview.tip1", 0, nullptr },
    { JCE_GB_HOTKEY,     "guide.inspector.overview.hk2", JCE_HK_EDIT_DELETE, nullptr },
    { JCE_GB_WARN,       "guide.inspector.overview.warn1", 0, nullptr },
};

static const JceGuideBlock b_addcomp[] = {
    { JCE_GB_P,      "guide.inspector.addcomp.p1", 0, nullptr },
    { JCE_GB_STEP,   "guide.inspector.addcomp.s1", 0, nullptr },
    { JCE_GB_STEP,   "guide.inspector.addcomp.s2", 0, nullptr },
    { JCE_GB_STEP,   "guide.inspector.addcomp.s3", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.addcomp.p2", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.addcomp.p3", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.addcomp.p4", 0, nullptr },
    { JCE_GB_TIP,    "guide.inspector.addcomp.tip1", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.addcomp.p5", 0, nullptr },
    { JCE_GB_WARN,   "guide.inspector.addcomp.warn1", 0, nullptr },
    { JCE_GB_HOTKEY, "guide.inspector.addcomp.hk1", JCE_HK_EDIT_UNDO, nullptr },
    { JCE_GB_HOTKEY, "guide.inspector.addcomp.hk2", JCE_HK_EDIT_REDO, nullptr },
    { JCE_GB_TIP,    "guide.inspector.addcomp.tip2", 0, nullptr },
    { JCE_GB_WARN,   "guide.inspector.addcomp.warn2", 0, nullptr },
};

static const JceGuideBlock b_presets[] = {
    { JCE_GB_P,    "guide.inspector.presets.p1", 0, nullptr },
    { JCE_GB_STEP, "guide.inspector.presets.s1", 0, nullptr },
    { JCE_GB_STEP, "guide.inspector.presets.s2", 0, nullptr },
    { JCE_GB_STEP, "guide.inspector.presets.s3", 0, nullptr },
    { JCE_GB_STEP, "guide.inspector.presets.s4", 0, nullptr },
    { JCE_GB_P,    "guide.inspector.presets.p2", 0, nullptr },
    { JCE_GB_TIP,  "guide.inspector.presets.tip1", 0, nullptr },
    { JCE_GB_WARN, "guide.inspector.presets.warn1", 0, nullptr },
};

static const JceGuideBlock b_multiselect[] = {
    { JCE_GB_P,      "guide.inspector.multiselect.p1", 0, nullptr },
    { JCE_GB_HOTKEY, "guide.inspector.multiselect.hk1", JCE_HK_EDIT_SELECT_ALL, nullptr },
    { JCE_GB_P,      "guide.inspector.multiselect.p2", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.multiselect.p3", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.multiselect.p4", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.multiselect.p5", 0, nullptr },
    { JCE_GB_P,      "guide.inspector.multiselect.p6", 0, nullptr },
    { JCE_GB_STEP,   "guide.inspector.multiselect.s1", 0, nullptr },
    { JCE_GB_STEP,   "guide.inspector.multiselect.s2", 0, nullptr },
    { JCE_GB_STEP,   "guide.inspector.multiselect.s3", 0, nullptr },
    { JCE_GB_WARN,   "guide.inspector.multiselect.warn1", 0, nullptr },
};

static const JceGuideBlock b_comps_render[] = {
    { JCE_GB_P,          "guide.inspector.comps_render.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector", JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b1", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b2", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b3", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b4", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b5", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b6", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b7", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b8", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b9", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b10", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b11", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b12", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b13", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b14", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b15", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b16", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b17", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b18", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b19", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b20", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_render.b21", 0, nullptr },
    { JCE_GB_WARN,       "guide.inspector.comps_render.warn1", 0, nullptr },
};

static const JceGuideBlock b_comps_physics[] = {
    { JCE_GB_P,          "guide.inspector.comps_physics.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.physicsDebugger", JCE_PANEL_PHYSICS_DEBUGGER, "###physics_debugger" },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b1", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b2", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b3", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b4", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b5", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b6", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b7", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b8", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b9", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b10", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b11", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b12", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b13", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b14", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b15", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b16", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b17", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b18", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b19", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b20", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b21", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_physics.b22", 0, nullptr },
    { JCE_GB_TIP,        "guide.inspector.comps_physics.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.inspector.comps_physics.warn1", 0, nullptr },
};

static const JceGuideBlock b_comps_logic[] = {
    { JCE_GB_P,          "guide.inspector.comps_logic.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.animationEditor", JCE_PANEL_ANIMATION_EDITOR, "###jce_anim_editor" },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b1", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b2", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b3", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b4", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b5", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b6", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b7", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b8", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b9", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b10", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b11", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b12", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b13", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b14", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b15", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b16", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b17", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b18", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b19", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b20", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b21", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b22", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b23", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b24", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_logic.b25", 0, nullptr },
    { JCE_GB_TIP,        "guide.inspector.comps_logic.tip1", 0, nullptr },
};

/* UI components — full runtime UI control set (containers + widgets). */
static const JceGuideBlock b_comps_ui[] = {
    { JCE_GB_P,          "guide.inspector.comps_ui.p1", 0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector", JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b1", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b2", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b3", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b4", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b5", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b6", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b7", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b8", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b9", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b10", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b11", 0, nullptr },
    { JCE_GB_BULLET,     "guide.inspector.comps_ui.b12", 0, nullptr },
    { JCE_GB_TIP,        "guide.inspector.comps_ui.tip1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.inspector.overview.title",      b_overview,      JCE_GUIDE_COUNT(b_overview) },
    { "guide.inspector.addcomp.title",       b_addcomp,       JCE_GUIDE_COUNT(b_addcomp) },
    { "guide.inspector.presets.title",       b_presets,       JCE_GUIDE_COUNT(b_presets) },
    { "guide.inspector.multiselect.title",   b_multiselect,   JCE_GUIDE_COUNT(b_multiselect) },
    { "guide.inspector.comps_render.title",  b_comps_render,  JCE_GUIDE_COUNT(b_comps_render) },
    { "guide.inspector.comps_physics.title", b_comps_physics, JCE_GUIDE_COUNT(b_comps_physics) },
    { "guide.inspector.comps_logic.title",   b_comps_logic,   JCE_GUIDE_COUNT(b_comps_logic) },
    { "guide.inspector.comps_ui.title",      b_comps_ui,      JCE_GUIDE_COUNT(b_comps_ui) },
};

const JceGuideChapter g_jce_guide_ch_inspector_components = {
    "guide.inspector.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
