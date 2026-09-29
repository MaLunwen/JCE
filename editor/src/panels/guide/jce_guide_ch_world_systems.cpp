/*
 * jce_guide_ch_world_systems.cpp — User guide chapter: World & Systems
 * Generated content: every claim verified against the editor source.
 */
#include "panels/jce_panel_user_guide.h"

/* ── Topic 1: Terrain ──────────────────────────────────────────── */
static const JceGuideBlock b_terrain[] = {
    { JCE_GB_P,          "guide.world.terrain.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.terrain",            JCE_PANEL_TERRAIN, "###jce_terrain" },
    { JCE_GB_MENU_PATH,  nullptr,                     0, "menu.window>window.group.world>window.terrain" },
    { JCE_GB_STEP,       "guide.world.terrain.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.terrain.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.terrain.s3",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.terrain.s4",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.terrain.s5",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.terrain.s6",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.terrain.s7",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.terrain.s8",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "Inspector",                 JCE_PANEL_INSPECTOR, "###inspector" },
    { JCE_GB_TIP,        "guide.world.terrain.tip1",  0, nullptr },
    { JCE_GB_WARN,       "guide.world.terrain.warn1", 0, nullptr },
    { JCE_GB_P,          "guide.world.terrain.p2",    0, nullptr },
};

/* ── Topic 2: NavMesh ──────────────────────────────────────────── */
static const JceGuideBlock b_nav[] = {
    { JCE_GB_P,          "guide.world.nav.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.navmesh",        JCE_PANEL_NAVMESH, "###jce_navmesh" },
    { JCE_GB_MENU_PATH,  nullptr,                 0, "menu.window>window.group.world>window.navmesh" },
    { JCE_GB_STEP,       "guide.world.nav.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.nav.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.nav.s3",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.nav.s4",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.nav.s5",    0, nullptr },
    { JCE_GB_HOTKEY,     "guide.world.nav.hk1",   JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_WARN,       "guide.world.nav.warn1", 0, nullptr },
    { JCE_GB_TIP,        "guide.world.nav.tip1",  0, nullptr },
};

/* ── Topic 3: World Streaming ───────────────────────────────────── */
static const JceGuideBlock b_stream[] = {
    { JCE_GB_P,          "guide.world.stream.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.worldStreaming",    JCE_PANEL_WORLD_STREAMING, "###world_streaming" },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.window>window.group.world>window.worldStreaming" },
    { JCE_GB_STEP,       "guide.world.stream.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.stream.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.stream.s3",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.stream.s4",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.stream.s5",    0, nullptr },
    { JCE_GB_TIP,        "guide.world.stream.tip1",  0, nullptr },
    { JCE_GB_WARN,       "guide.world.stream.warn1", 0, nullptr },
    { JCE_GB_P,          "guide.world.stream.p2",    0, nullptr },
};

/* ── Topic 4: Tilemap ──────────────────────────────────────────── */
static const JceGuideBlock b_tilemap[] = {
    { JCE_GB_P,          "guide.world.tilemap.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "spriteEditor.title",        JCE_PANEL_SPRITE_EDITOR, "###sprite_editor" },
    { JCE_GB_OPEN_PANEL, "tilePalette.title",         JCE_PANEL_TILE_PALETTE, "###tile_palette" },
    { JCE_GB_MENU_PATH,  nullptr,                     0, "menu.window>window.group.tools>tilePalette.title" },
    { JCE_GB_STEP,       "guide.world.tilemap.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.tilemap.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.tilemap.s3",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.tilemap.s4",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.tilemap.s5",    0, nullptr },
    { JCE_GB_P,          "guide.world.tilemap.p2",    0, nullptr },
    { JCE_GB_WARN,       "guide.world.tilemap.warn1", 0, nullptr },
    { JCE_GB_TIP,        "guide.world.tilemap.tip1",  0, nullptr },
};

/* ── Topic 5: Particles ────────────────────────────────────────── */
static const JceGuideBlock b_fx[] = {
    { JCE_GB_P,          "guide.world.fx.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.materialGraph", JCE_PANEL_MATERIAL_GRAPH, "###jce_material_graph" },
    { JCE_GB_MENU_PATH,  nullptr,                0, "menu.window>window.group.workbenches>window.materialGraph>window.particleEditor" },
    { JCE_GB_STEP,       "guide.world.fx.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.fx.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.fx.s3",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.fx.s4",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.fx.s5",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.lighting.title", JCE_PANEL_LIGHTING_SETTINGS, "###lighting_settings" },
    { JCE_GB_WARN,       "guide.world.fx.warn1", 0, nullptr },
    { JCE_GB_TIP,        "guide.world.fx.tip1",  0, nullptr },
};

/* ── Topic 6: Behavior Trees ───────────────────────────────────── */
static const JceGuideBlock b_bt[] = {
    { JCE_GB_P,          "guide.world.bt.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.btVisualizer",  JCE_PANEL_BT_VISUALIZER, "###bt_visualizer" },
    { JCE_GB_MENU_PATH,  nullptr,                0, "menu.window>window.group.world>window.btVisualizer" },
    { JCE_GB_STEP,       "guide.world.bt.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.bt.s2",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.bt.s3",    0, nullptr },
    { JCE_GB_HOTKEY,     "guide.world.bt.hk1",   JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_TIP,        "guide.world.bt.tip1",  0, nullptr },
    { JCE_GB_TIP,        "guide.world.bt.tip2",  0, nullptr },
    { JCE_GB_WARN,       "guide.world.bt.warn1", 0, nullptr },
};

/* ── Topic 7: Networking ───────────────────────────────────────── */
static const JceGuideBlock b_net[] = {
    { JCE_GB_P,          "guide.world.net.p1",       0, nullptr },
    { JCE_GB_OPEN_PANEL, "panel.network_stats.title", JCE_PANEL_NETWORK_STATS, "###network_stats" },
    { JCE_GB_MENU_PATH,  nullptr,                    0, "menu.window>window.group.workbenches>panel.network_stats.title" },
    { JCE_GB_STEP,       "guide.world.net.s1",       0, nullptr },
    { JCE_GB_STEP,       "guide.world.net.s2",       0, nullptr },
    { JCE_GB_STEP,       "guide.world.net.s3",       0, nullptr },
    { JCE_GB_HOTKEY,     "guide.world.net.hk1",      JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_WARN,       "guide.world.net.warn1",    0, nullptr },
    { JCE_GB_TIP,        "guide.world.net.tip1",     0, nullptr },
};

/* ── Topic 8: Systems Panel ────────────────────────────────────── */
static const JceGuideBlock b_sys[] = {
    { JCE_GB_P,          "guide.world.sys.p1",    0, nullptr },
    { JCE_GB_OPEN_PANEL, "window.systems",        JCE_PANEL_SYSTEMS, "###systems" },
    { JCE_GB_MENU_PATH,  nullptr,                 0, "menu.window>window.group.tools>window.systems" },
    { JCE_GB_STEP,       "guide.world.sys.s1",    0, nullptr },
    { JCE_GB_STEP,       "guide.world.sys.s2",    0, nullptr },
    { JCE_GB_HOTKEY,     "guide.world.sys.hk1",   JCE_HK_PLAY_TOGGLE, nullptr },
    { JCE_GB_STEP,       "guide.world.sys.s3",    0, nullptr },
    { JCE_GB_WARN,       "guide.world.sys.warn1", 0, nullptr },
};

static const JceGuideTopic k_topics[] = {
    { "guide.world.terrain.title", b_terrain, JCE_GUIDE_COUNT(b_terrain) },
    { "guide.world.nav.title",     b_nav,     JCE_GUIDE_COUNT(b_nav)     },
    { "guide.world.stream.title",  b_stream,  JCE_GUIDE_COUNT(b_stream)  },
    { "guide.world.tilemap.title", b_tilemap, JCE_GUIDE_COUNT(b_tilemap) },
    { "guide.world.fx.title",      b_fx,      JCE_GUIDE_COUNT(b_fx)      },
    { "guide.world.bt.title",      b_bt,      JCE_GUIDE_COUNT(b_bt)      },
    { "guide.world.net.title",     b_net,     JCE_GUIDE_COUNT(b_net)     },
    { "guide.world.sys.title",     b_sys,     JCE_GUIDE_COUNT(b_sys)     },
};

const JceGuideChapter g_jce_guide_ch_world_systems = {
    "guide.world.title", k_topics, JCE_GUIDE_COUNT(k_topics)
};
