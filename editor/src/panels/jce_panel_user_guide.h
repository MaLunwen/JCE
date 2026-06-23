/*
 * jce_panel_user_guide.h — Official operation guide panel.
 *
 * Data model for the in-editor user guide: chapters → topics → blocks.
 * All user-visible text flows through the editor i18n system (guide.*
 * keys, four locales).  Blocks can carry LIVE elements: hotkey chips
 * render the CURRENT chord from the hotkey registry (so user rebinds
 * show up), and "open panel" action buttons let the reader jump to the
 * documented panel and follow along hands-on.
 *
 * Content lives in per-chapter translation units under panels/guide/
 * (one extern JceGuideChapter each) so chapters can be authored and
 * maintained independently; the renderer in jce_panel_user_guide.cpp
 * stitches them together and appends a built-in, registry-generated
 * hotkey reference chapter.
 */
#ifndef JCE_PANEL_USER_GUIDE_H
#define JCE_PANEL_USER_GUIDE_H

#include "ui/jce_editor_panels.h"
#include "core/jce_hotkeys.h"

typedef enum {
    JCE_GB_H1 = 0,    /* section heading: text = i18n key                  */
    JCE_GB_P,         /* wrapped paragraph: text = i18n key                */
    JCE_GB_BULLET,    /* bullet item: text = i18n key                      */
    JCE_GB_STEP,      /* auto-numbered step: text = i18n key               */
    JCE_GB_TIP,       /* tip callout: text = i18n key                      */
    JCE_GB_WARN,      /* warning callout: text = i18n key                  */
    JCE_GB_HOTKEY,    /* desc text = i18n key, aux = JceHotkeyId (live)    */
    JCE_GB_OPEN_PANEL,/* action button: text = panel-title i18n key,
                         aux = JceEditorPanel, str = stable "###id"        */
    JCE_GB_MENU_PATH, /* str = "menu.a>menu.a.b" rendered via i18n as
                         "A → B" (menu path the reader should follow)      */
    JCE_GB_SEP        /* thin separator                                    */
} JceGuideBlockType;

typedef struct {
    uint8_t     type;   /* JceGuideBlockType */
    const char *text;   /* i18n key (or NULL where unused) */
    int         aux;    /* JceHotkeyId / JceEditorPanel (or 0) */
    const char *str;    /* stable window id / menu key path (or NULL) */
} JceGuideBlock;

typedef struct {
    const char          *title_key;   /* i18n key */
    const JceGuideBlock *blocks;
    int                  block_count;
} JceGuideTopic;

typedef struct {
    const char          *title_key;   /* i18n key */
    const JceGuideTopic *topics;
    int                  topic_count;
} JceGuideChapter;

#define JCE_GUIDE_COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/* Content chapters (panels/guide/jce_guide_ch_*.cpp). Order here is the
 * order presented to the reader. */
extern const JceGuideChapter g_jce_guide_ch_getting_started;
extern const JceGuideChapter g_jce_guide_ch_scene_editing;
extern const JceGuideChapter g_jce_guide_ch_inspector_components;
extern const JceGuideChapter g_jce_guide_ch_assets;
extern const JceGuideChapter g_jce_guide_ch_rendering;
extern const JceGuideChapter g_jce_guide_ch_animation;
extern const JceGuideChapter g_jce_guide_ch_physics;
extern const JceGuideChapter g_jce_guide_ch_audio_media;
extern const JceGuideChapter g_jce_guide_ch_world_systems;
extern const JceGuideChapter g_jce_guide_ch_play_runtime;
extern const JceGuideChapter g_jce_guide_ch_build_distribution;
extern const JceGuideChapter g_jce_guide_ch_diagnostics_customization;

#endif /* JCE_PANEL_USER_GUIDE_H */
