/*
 * jce_guide_ch_scene_editing.cpp — User guide chapter: Scene Editing
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

static const JceGuideBlock b_hierarchy[] = {
    { JCE_GB_P,          "guide.scene.hierarchy.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Hierarchy", JCE_PANEL_HIERARCHY, "###hierarchy" },
    { JCE_GB_BULLET,     "guide.scene.hierarchy.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.hierarchy.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.hierarchy.b3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.hierarchy.b4",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.hierarchy.hk1",  JCE_HK_EDIT_RENAME,    nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.hierarchy.hk2",  JCE_HK_EDIT_DUPLICATE, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.hierarchy.hk3",  JCE_HK_EDIT_DELETE,    nullptr },
    { JCE_GB_P,          "guide.scene.hierarchy.p2",   0, nullptr },
    { JCE_GB_TIP,        "guide.scene.hierarchy.tip1", 0, nullptr },
    { JCE_GB_P,          "guide.scene.hierarchy.p3",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.gameObject>menu.gameObject.createCube" },
};

static const JceGuideBlock b_selection[] = {
    { JCE_GB_P,          "guide.scene.selection.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Scene", JCE_PANEL_SCENE_VIEW, "###scene_view" },
    { JCE_GB_BULLET,     "guide.scene.selection.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.selection.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.selection.b3",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.selection.hk1",  JCE_HK_VIEW_FRAME_SELECTED, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.selection.hk2",  JCE_HK_VIEW_FRAME_ALL,      nullptr },
    { JCE_GB_TIP,        "guide.scene.selection.tip1", 0, nullptr },
    { JCE_GB_P,          "guide.scene.selection.p2",   0, nullptr },
};

static const JceGuideBlock b_gizmo[] = {
    { JCE_GB_P,          "guide.scene.gizmo.p1",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.gizmo.hk1",  JCE_HK_GIZMO_TRANSLATE,    nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.gizmo.hk2",  JCE_HK_GIZMO_ROTATE,       nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.gizmo.hk3",  JCE_HK_GIZMO_SCALE,        nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.gizmo.hk4",  JCE_HK_GIZMO_TOGGLE_SPACE, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.gizmo.hk5",  JCE_HK_GIZMO_TOGGLE_PIVOT, nullptr },
    { JCE_GB_P,          "guide.scene.gizmo.p2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.gizmo.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.gizmo.b2",   0, nullptr },
    { JCE_GB_TIP,        "guide.scene.gizmo.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.scene.gizmo.warn1", 0, nullptr },
};

static const JceGuideBlock b_pivot[] = {
    { JCE_GB_P,          "guide.scene.pivot.p1",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.pivot.hk1",  JCE_HK_GIZMO_PIVOT_EDIT, nullptr },
    { JCE_GB_STEP,       "guide.scene.pivot.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.scene.pivot.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.scene.pivot.s3",   0, nullptr },
    { JCE_GB_STEP,       "guide.scene.pivot.s4",   0, nullptr },
    { JCE_GB_P,          "guide.scene.pivot.p2",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector", JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_TIP,        "guide.scene.pivot.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.scene.pivot.warn1", 0, nullptr },
};

static const JceGuideBlock b_assetdrop[] = {
    { JCE_GB_P,          "guide.scene.assetdrop.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Asset Browser", JCE_PANEL_ASSETS, "###assets" },
    { JCE_GB_BULLET,     "guide.scene.assetdrop.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.assetdrop.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.assetdrop.b3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.assetdrop.b4",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.assetdrop.b5",   0, nullptr },
    { JCE_GB_STEP,       "guide.scene.assetdrop.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.scene.assetdrop.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.scene.assetdrop.s3",   0, nullptr },
    { JCE_GB_TIP,        "guide.scene.assetdrop.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.scene.assetdrop.warn1", 0, nullptr },
};

static const JceGuideBlock b_undo[] = {
    { JCE_GB_P,          "guide.scene.undo.p1",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.undo.hk1",  JCE_HK_EDIT_UNDO,     nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.undo.hk2",  JCE_HK_EDIT_REDO,     nullptr },
    { JCE_GB_HOTKEY,     "guide.scene.undo.hk3",  JCE_HK_EDIT_REDO_ALT, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.edit>menu.edit.undo" },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.edit>menu.edit.redo" },
    { JCE_GB_BULLET,     "guide.scene.undo.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.undo.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.scene.undo.b3",   0, nullptr },
    { JCE_GB_WARN,       "guide.scene.undo.warn1", 0, nullptr },
    { JCE_GB_TIP,        "guide.scene.undo.tip1", 0, nullptr },
    { JCE_GB_P,          "guide.scene.undo.p2",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.file>menu.file.saveScene" },
};

static const JceGuideBlock b_ai[] = {
    { JCE_GB_P,          "guide.scene.ai.p1",     0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.aiAssistant",    JCE_PANEL_AI_ASSISTANT, "###ai_assistant" },
    { JCE_GB_MENU_PATH,  nullptr, 0, "menu.window>window.group.tools>window.aiAssistant" },
    { JCE_GB_STEP,       "guide.scene.ai.s1",     0, nullptr },
    { JCE_GB_STEP,       "guide.scene.ai.s2",     0, nullptr },
    { JCE_GB_STEP,       "guide.scene.ai.s3",     0, nullptr },
    { JCE_GB_STEP,       "guide.scene.ai.s4",     0, nullptr },
    { JCE_GB_STEP,       "guide.scene.ai.s5",     0, nullptr },
    { JCE_GB_WARN,       "guide.scene.ai.warn1",  0, nullptr },
    { JCE_GB_TIP,        "guide.scene.ai.tip1",   0, nullptr },
    { JCE_GB_P,          "guide.scene.ai.p2",     0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.scene.hierarchy.title", b_hierarchy, JCE_GUIDE_COUNT(b_hierarchy) },
    { "guide.scene.selection.title", b_selection, JCE_GUIDE_COUNT(b_selection) },
    { "guide.scene.gizmo.title",     b_gizmo,     JCE_GUIDE_COUNT(b_gizmo) },
    { "guide.scene.pivot.title",     b_pivot,     JCE_GUIDE_COUNT(b_pivot) },
    { "guide.scene.assetdrop.title", b_assetdrop, JCE_GUIDE_COUNT(b_assetdrop) },
    { "guide.scene.undo.title",      b_undo,      JCE_GUIDE_COUNT(b_undo) },
    { "guide.scene.ai.title",        b_ai,        JCE_GUIDE_COUNT(b_ai) },
};

const JceGuideChapter g_jce_guide_ch_scene_editing = {
    "guide.scene.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
