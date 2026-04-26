/*
 * jce_ui_settings.h  Built-in settings panel (Video + Audio tabs).
 *
 * Engine-level settings overlay with fullscreen/VSync/resolution display
 * on the Video tab, and master/music/SFX volume sliders on the Audio tab.
 * The markup is loaded from engine/ui in the PAK archive.
 *
 * Games can hook into the apply/populate cycle via callbacks.
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 */

#ifndef JCE_UI_SETTINGS_H
#define JCE_UI_SETTINGS_H


#include <jce/os/core/jce_defs.h>
#include <jce/middleware/ui/jce_ui.h>
#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;
typedef struct JceWindow   JceWindow;
typedef struct JceAudio    JceAudio;
typedef struct JceConfig   JceConfig;

typedef struct JceSettingsPanel JceSettingsPanel;

/* Callback: notified after the user clicks Apply or OK. */
typedef void (*jce_settings_on_apply_fn)(JceSettingsPanel *panel, void *ud);

/* Callback: notified when the panel opens (populate custom controls). */
typedef void (*jce_settings_on_populate_fn)(JceSettingsPanel *panel, void *ud);

/* Callback: notified when the user requests closing the game. */
typedef void (*jce_settings_on_close_game_fn)(JceSettingsPanel *panel, void *ud);

/* Configuration. */
typedef struct JceSettingsPanelDesc {
    JceUIContext      *ui;             /* required */
    JceRenderer       *renderer;       /* required */
    JceWindow         *window;         /* required */
    JceAudio          *audio;          /* NULL = omit audio tab behavior */
    const JceConfig   *config;         /* NULL = use defaults */
    const char        *font_family;    /* NULL = built-in default */

    /* Extension callbacks (all optional). */
    jce_settings_on_apply_fn    on_apply;
    jce_settings_on_populate_fn on_populate;
    jce_settings_on_close_game_fn on_close_game;
    void                       *callback_userdata;
} JceSettingsPanelDesc;

/* Tracked volume state (engine audio API has no getters). */
typedef struct JceSettingsVolumes {
    float master;
    float music;
    float sfx;
} JceSettingsVolumes;

JceSettingsPanel  *jce_settings_create(const JceSettingsPanelDesc *desc);
void               jce_settings_destroy(JceSettingsPanel *panel);

void               jce_settings_open(JceSettingsPanel *panel);
void               jce_settings_close(JceSettingsPanel *panel);
bool               jce_settings_is_open(const JceSettingsPanel *panel);

/* Apply pending settings to engine.
   Called internally by OK/Apply buttons. */
void               jce_settings_apply(JceSettingsPanel *panel);

/* Keyboard navigation helpers for arrow-key driven menus. */
void               jce_settings_focus_prev(JceSettingsPanel *panel);
void               jce_settings_focus_next(JceSettingsPanel *panel);
void               jce_settings_adjust(JceSettingsPanel *panel, int delta);
void               jce_settings_activate(JceSettingsPanel *panel);

/* Refresh all i18n labels (call after jce_i18n_set_lang()). */
void               jce_settings_update_i18n(JceSettingsPanel *panel);

/* Change body font-family (e.g. on language switch). */
void               jce_settings_set_font_family(JceSettingsPanel *panel,
                                                const char *family);

JceUIDocHandle     jce_settings_get_doc(const JceSettingsPanel *panel);

JceSettingsVolumes jce_settings_get_volumes(const JceSettingsPanel *panel);
void               jce_settings_set_volumes(JceSettingsPanel *panel,
                                            const JceSettingsVolumes *v);

JCE_EXTERN_C_END

#endif /* JCE_UI_SETTINGS_H */
