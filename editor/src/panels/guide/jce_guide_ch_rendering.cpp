/*
 * jce_guide_ch_rendering.cpp — User guide chapter: Rendering & Lighting
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* ── Topic 1: The Lighting Workbench at a Glance ──────────────────── */
static const JceGuideBlock b_workbench[] = {
    { JCE_GB_P,          "guide.render.workbench.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.lighting.title",        JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings" },
    { JCE_GB_MENU_PATH,  nullptr,                       0, "menu.window>window.group.workbenches>panel.lighting.title" },
    { JCE_GB_P,          "guide.render.workbench.p2",   0, nullptr },
    { JCE_GB_H1,         "guide.render.workbench.h1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.workbench.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.workbench.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.workbench.b3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.workbench.b4",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.workbench.b5",   0, nullptr },
    { JCE_GB_TIP,        "guide.render.workbench.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.render.workbench.warn1", 0, nullptr },
    { JCE_GB_H1,         "guide.render.workbench.h2",   0, nullptr },
    { JCE_GB_P,          "guide.render.workbench.p3",   0, nullptr },
    { JCE_GB_TIP,        "guide.render.workbench.tip2", 0, nullptr },
    { JCE_GB_P,          "guide.render.workbench.p4",   0, nullptr },
    { JCE_GB_SEP,        nullptr,                       0, nullptr },
    { JCE_GB_P,          "guide.render.workbench.p5",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.render.workbench.hk1",  JCE_HK_FILE_SAVE, nullptr },
};

/* ── Topic 2: Light Components ────────────────────────────────────── */
static const JceGuideBlock b_lights[] = {
    { JCE_GB_P,          "guide.render.lights.p1",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.gameObject>menu.gameObject.createLight>light.directional" },
    { JCE_GB_STEP,       "guide.render.lights.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.lights.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.lights.s3",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.lights.s4",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.lights.s5",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector",                JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_P,          "guide.render.lights.p2",   0, nullptr },
    { JCE_GB_WARN,       "guide.render.lights.warn1", 0, nullptr },
    { JCE_GB_WARN,       "guide.render.lights.warn2", 0, nullptr },
    { JCE_GB_P,          "guide.render.lights.p3",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.render.lights.hk1",  JCE_HK_PANEL_PROJECT_SETTINGS, nullptr },
    { JCE_GB_TIP,        "guide.render.lights.tip1", 0, nullptr },
    { JCE_GB_HOTKEY,     "guide.render.lights.hk2",  JCE_HK_VIEW_FRAME_SELECTED, nullptr },
};

/* ── Topic 3: Cameras & Virtual Cameras ───────────────────────────── */
static const JceGuideBlock b_camera[] = {
    { JCE_GB_P,          "guide.render.camera.p1",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.gameObject>menu.gameObject.createCamera" },
    { JCE_GB_OPEN_PANEL, "Game",                     JCE_PANEL_GAME_VIEW, "###game_view" },
    { JCE_GB_SEP,        nullptr,                    0, nullptr },
    { JCE_GB_P,          "guide.render.camera.p2",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.camera.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.camera.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.camera.s3",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.render.camera.hk1",  JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_OPEN_PANEL, "window.vcamManager",       JCE_PANEL_VCAM_MANAGER, "###vcam_manager" },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.window>window.group.tools>window.vcamManager" },
    { JCE_GB_P,          "guide.render.camera.p3",   0, nullptr },
    { JCE_GB_TIP,        "guide.render.camera.tip1", 0, nullptr },
};

/* ── Topic 4: Post-Processing ─────────────────────────────────────── */
static const JceGuideBlock b_postfx[] = {
    { JCE_GB_P,          "guide.render.postfx.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.lighting.title",     JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings" },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.window>window.group.workbenches>postfx.title" },
    { JCE_GB_BULLET,     "guide.render.postfx.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.postfx.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.postfx.b3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.postfx.b4",   0, nullptr },
    { JCE_GB_P,          "guide.render.postfx.p2",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.postfx.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.postfx.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.postfx.s3",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.render.postfx.hk1",  JCE_HK_UI_SCREENSHOT, nullptr },
    { JCE_GB_TIP,        "guide.render.postfx.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.render.postfx.warn1", 0, nullptr },
    { JCE_GB_P,          "guide.render.postfx.p3",   0, nullptr },
};

/* ── Topic 5: Material Graph & Shader Graph ───────────────────────── */
static const JceGuideBlock b_matgraph[] = {
    { JCE_GB_P,          "guide.render.matgraph.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.materialGraph",       JCE_PANEL_MATERIAL_GRAPH, "###jce_material_graph" },
    { JCE_GB_MENU_PATH,  nullptr,                      0, "menu.window>window.group.workbenches>window.materialGraph" },
    { JCE_GB_P,          "guide.render.matgraph.p2",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.matgraph.s1",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.matgraph.s2",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.matgraph.s3",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.matgraph.s4",   0, nullptr },
    { JCE_GB_STEP,       "guide.render.matgraph.s5",   0, nullptr },
    { JCE_GB_P,          "guide.render.matgraph.p3",   0, nullptr },
    { JCE_GB_TIP,        "guide.render.matgraph.tip1", 0, nullptr },
    { JCE_GB_WARN,       "guide.render.matgraph.warn1", 0, nullptr },
    { JCE_GB_P,          "guide.render.matgraph.p4",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                      0, "menu.edit>shaders.reload" },
};

/* ── Topic 6: Render Pipeline & Performance ───────────────────────── */
static const JceGuideBlock b_pipeline[] = {
    { JCE_GB_P,          "guide.render.pipeline.p1",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.lighting.title",       JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings" },
    { JCE_GB_MENU_PATH,  nullptr,                      0, "menu.window>window.group.workbenches>panel.render_pipeline.title" },
    { JCE_GB_BULLET,     "guide.render.pipeline.b1",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.pipeline.b2",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.pipeline.b3",   0, nullptr },
    { JCE_GB_BULLET,     "guide.render.pipeline.b4",   0, nullptr },
    { JCE_GB_TIP,        "guide.render.pipeline.tip1", 0, nullptr },
    { JCE_GB_P,          "guide.render.pipeline.p2",   0, nullptr },
    { JCE_GB_HOTKEY,     "guide.render.pipeline.hk1",  JCE_HK_PANEL_PROJECT_SETTINGS, nullptr },
    { JCE_GB_H1,         "guide.render.pipeline.h1",   0, nullptr },
    { JCE_GB_P,          "guide.render.pipeline.p3",   0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.profiler",            JCE_PANEL_PROFILER, "###profiler" },
    { JCE_GB_MENU_PATH,  nullptr,                      0, "menu.window>window.group.workbenches>frameDebugger.title" },
    { JCE_GB_P,          "guide.render.pipeline.p4",   0, nullptr },
    { JCE_GB_MENU_PATH,  nullptr,                      0, "menu.debug>menu.debug.toggleDemoLod" },
    { JCE_GB_WARN,       "guide.render.pipeline.warn1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.render.workbench.title", b_workbench, JCE_GUIDE_COUNT(b_workbench) },
    { "guide.render.lights.title",    b_lights,    JCE_GUIDE_COUNT(b_lights) },
    { "guide.render.camera.title",    b_camera,    JCE_GUIDE_COUNT(b_camera) },
    { "guide.render.postfx.title",    b_postfx,    JCE_GUIDE_COUNT(b_postfx) },
    { "guide.render.matgraph.title",  b_matgraph,  JCE_GUIDE_COUNT(b_matgraph) },
    { "guide.render.pipeline.title",  b_pipeline,  JCE_GUIDE_COUNT(b_pipeline) },
};

const JceGuideChapter g_jce_guide_ch_rendering = {
    "guide.render.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
