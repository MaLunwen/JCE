/*
 * jce_ui_settings.c  Built-in settings panel implementation.
 *
 * Loads the engine/ui RML document with Video + Audio tabs and
 * manages the full populate -> read -> apply cycle.
 */

#include <jce/application/jce_config.h>
#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/ui/jce_ui_settings.h>
#include <jce/os/core/jce_i18n.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_renderer.h>

#include "os/core/jce_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "ui.settings"

struct JceSettingsPanel {
    JceUIContext      *ui;
    JceRenderer       *renderer;
    JceWindow         *window;
    JceAudio          *audio;
    const JceConfig   *config;
    JceUIDocHandle     doc;
    bool               is_open;
    int                active_tab;  /* 0 = video, 1 = audio */

    /* Element handles — tabs. */
    JceUIElementHandle el_settings_title;
    JceUIElementHandle el_tab_video;
    JceUIElementHandle el_tab_audio;
    JceUIElementHandle el_panel_video;
    JceUIElementHandle el_panel_audio;

    /* Element handles — video tab. */
    JceUIElementHandle el_lbl_fullscreen;
    JceUIElementHandle el_lbl_resolution;
    JceUIElementHandle el_lbl_vsync;
    JceUIElementHandle el_sel_fullscreen;
    JceUIElementHandle el_txt_resolution;
    JceUIElementHandle el_chk_vsync;

    /* Element handles — audio tab. */
    JceUIElementHandle el_lbl_master_vol;
    JceUIElementHandle el_lbl_music_vol;
    JceUIElementHandle el_lbl_sfx_vol;
    JceUIElementHandle el_rng_master;
    JceUIElementHandle el_rng_music;
    JceUIElementHandle el_rng_sfx;

    /* Element handles — buttons. */
    JceUIElementHandle el_btn_cancel;
    JceUIElementHandle el_btn_apply;
    JceUIElementHandle el_btn_ok;
    JceUIElementHandle el_btn_quit;

    /* Pending values (before apply). */
    bool               pending_fullscreen;
    bool               pending_vsync;
    float              pending_master_vol;
    float              pending_music_vol;
    float              pending_sfx_vol;

    /* Tracked volumes (no engine audio getters). */
    JceSettingsVolumes volumes;

    /* Game callbacks. */
    jce_settings_on_apply_fn    on_apply;
    jce_settings_on_populate_fn on_populate;
    jce_settings_on_close_game_fn on_close_game;
    void                       *cb_userdata;

    int                nav_focus;
};

typedef enum JceSettingsFocusId {
    JCE_SETTINGS_FOCUS_TAB_VIDEO,
    JCE_SETTINGS_FOCUS_TAB_AUDIO,
    JCE_SETTINGS_FOCUS_FULLSCREEN,
    JCE_SETTINGS_FOCUS_VSYNC,
    JCE_SETTINGS_FOCUS_MASTER,
    JCE_SETTINGS_FOCUS_MUSIC,
    JCE_SETTINGS_FOCUS_BTN_OK,
    JCE_SETTINGS_FOCUS_BTN_CANCEL,
    JCE_SETTINGS_FOCUS_BTN_APPLY,
    JCE_SETTINGS_FOCUS_BTN_QUIT,
} JceSettingsFocusId;

/* ── Internal helpers ─────────────────────────────────────────────── */

static float settings_clampf(float value, float min_value, float max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static bool settings_has_quit_button(const JceSettingsPanel *p)
{
    return p && p->on_close_game && jce_ui_elem_valid(p->el_btn_quit);
}

static void settings_sync_pending_controls(JceSettingsPanel *p)
{
    char buf[32];

    if (!p || !p->ui) return;

    if (jce_ui_elem_valid(p->el_sel_fullscreen)) {
        jce_ui_elem_set_value(p->ui, p->el_sel_fullscreen,
                              p->pending_fullscreen ? "on" : "off");
    }

    if (jce_ui_elem_valid(p->el_chk_vsync)) {
        if (p->pending_vsync)
            jce_ui_elem_set_attribute(p->ui, p->el_chk_vsync, "checked", "checked");
        else
            jce_ui_elem_remove_attribute(p->ui, p->el_chk_vsync, "checked");
    }

    if (jce_ui_elem_valid(p->el_rng_master)) {
        snprintf(buf, sizeof(buf), "%d",
                 (int)(settings_clampf(p->pending_master_vol, 0.0f, 1.0f) * 100.0f + 0.5f));
        jce_ui_elem_set_value(p->ui, p->el_rng_master, buf);
    }
    if (jce_ui_elem_valid(p->el_rng_music)) {
        snprintf(buf, sizeof(buf), "%d",
                 (int)(settings_clampf(p->pending_music_vol, 0.0f, 1.0f) * 100.0f + 0.5f));
        jce_ui_elem_set_value(p->ui, p->el_rng_music, buf);
    }
    if (jce_ui_elem_valid(p->el_rng_sfx)) {
        snprintf(buf, sizeof(buf), "%d",
                 (int)(settings_clampf(p->pending_sfx_vol, 0.0f, 1.0f) * 100.0f + 0.5f));
        jce_ui_elem_set_value(p->ui, p->el_rng_sfx, buf);
    }
}

static void settings_set_tab_visual(JceSettingsPanel *p,
                                    JceUIElementHandle elem,
                                    bool active, bool focused)
{
    if (!p || !p->ui || !jce_ui_elem_valid(elem)) return;

    jce_ui_elem_set_property(p->ui, elem, "background-color",
        active ? (focused ? "#db5c49" : "#c74332")
               : (focused ? "#5f7184" : "#3a4959"));
    jce_ui_elem_set_property(p->ui, elem, "border-color",
        focused ? "#f1cd73" : (active ? "#f1cd73" : "#607385"));
    jce_ui_elem_set_property(p->ui, elem, "color",
        (active || focused) ? "#ffffff" : "#c6d0d8");
}

static void settings_set_button_visual(JceSettingsPanel *p,
                                       JceUIElementHandle elem,
                                       const char *base_bg,
                                       const char *focus_bg,
                                       const char *base_border,
                                       bool focused)
{
    if (!p || !p->ui || !jce_ui_elem_valid(elem)) return;

    jce_ui_elem_set_property(p->ui, elem, "background-color",
                             focused ? focus_bg : base_bg);
    jce_ui_elem_set_property(p->ui, elem, "border-color",
                             focused ? "#f1cd73" : base_border);
    jce_ui_elem_set_property(p->ui, elem, "color", "#edf3f8");
}

static void settings_set_control_visual(JceSettingsPanel *p,
                                        JceUIElementHandle label,
                                        JceUIElementHandle control,
                                        bool focused,
                                        const char *base_bg,
                                        const char *focus_bg,
                                        const char *base_border)
{
    if (!p || !p->ui) return;

    if (jce_ui_elem_valid(label)) {
        jce_ui_elem_set_property(p->ui, label, "color",
                                 focused ? "#f0c84c" : "#d9e0e6");
    }
    if (jce_ui_elem_valid(control)) {
        jce_ui_elem_set_property(p->ui, control, "background-color",
                                 focused ? focus_bg : base_bg);
        jce_ui_elem_set_property(p->ui, control, "border-color",
                                 focused ? "#f1cd73" : base_border);
    }
}

static const JceSettingsFocusId *settings_focus_order(const JceSettingsPanel *p,
                                                      int *count)
{
    static const JceSettingsFocusId video_order[] = {
        JCE_SETTINGS_FOCUS_TAB_VIDEO,
        JCE_SETTINGS_FOCUS_TAB_AUDIO,
        JCE_SETTINGS_FOCUS_FULLSCREEN,
        JCE_SETTINGS_FOCUS_VSYNC,
        JCE_SETTINGS_FOCUS_BTN_OK,
        JCE_SETTINGS_FOCUS_BTN_CANCEL,
        JCE_SETTINGS_FOCUS_BTN_APPLY,
    };
    static const JceSettingsFocusId video_order_with_quit[] = {
        JCE_SETTINGS_FOCUS_TAB_VIDEO,
        JCE_SETTINGS_FOCUS_TAB_AUDIO,
        JCE_SETTINGS_FOCUS_FULLSCREEN,
        JCE_SETTINGS_FOCUS_VSYNC,
        JCE_SETTINGS_FOCUS_BTN_OK,
        JCE_SETTINGS_FOCUS_BTN_CANCEL,
        JCE_SETTINGS_FOCUS_BTN_APPLY,
        JCE_SETTINGS_FOCUS_BTN_QUIT,
    };
    static const JceSettingsFocusId audio_order[] = {
        JCE_SETTINGS_FOCUS_TAB_VIDEO,
        JCE_SETTINGS_FOCUS_TAB_AUDIO,
        JCE_SETTINGS_FOCUS_MASTER,
        JCE_SETTINGS_FOCUS_MUSIC,
        JCE_SETTINGS_FOCUS_BTN_OK,
        JCE_SETTINGS_FOCUS_BTN_CANCEL,
        JCE_SETTINGS_FOCUS_BTN_APPLY,
    };
    static const JceSettingsFocusId audio_order_with_quit[] = {
        JCE_SETTINGS_FOCUS_TAB_VIDEO,
        JCE_SETTINGS_FOCUS_TAB_AUDIO,
        JCE_SETTINGS_FOCUS_MASTER,
        JCE_SETTINGS_FOCUS_MUSIC,
        JCE_SETTINGS_FOCUS_BTN_OK,
        JCE_SETTINGS_FOCUS_BTN_CANCEL,
        JCE_SETTINGS_FOCUS_BTN_APPLY,
        JCE_SETTINGS_FOCUS_BTN_QUIT,
    };

    if (count) {
        if (p->active_tab == 0)
            *count = settings_has_quit_button(p)
                   ? (int)(sizeof(video_order_with_quit) / sizeof(video_order_with_quit[0]))
                   : (int)(sizeof(video_order) / sizeof(video_order[0]));
        else
            *count = settings_has_quit_button(p)
                   ? (int)(sizeof(audio_order_with_quit) / sizeof(audio_order_with_quit[0]))
                   : (int)(sizeof(audio_order) / sizeof(audio_order[0]));
    }

    if (p->active_tab == 0)
        return settings_has_quit_button(p) ? video_order_with_quit : video_order;
    return settings_has_quit_button(p) ? audio_order_with_quit : audio_order;
}

static void settings_refresh_navigation(JceSettingsPanel *p)
{
    if (!p || !p->ui) return;

    settings_set_tab_visual(p, p->el_tab_video,
                            p->active_tab == 0,
                            p->nav_focus == JCE_SETTINGS_FOCUS_TAB_VIDEO);
    settings_set_tab_visual(p, p->el_tab_audio,
                            p->active_tab == 1,
                            p->nav_focus == JCE_SETTINGS_FOCUS_TAB_AUDIO);

    settings_set_control_visual(p, p->el_lbl_fullscreen, p->el_sel_fullscreen,
                                p->nav_focus == JCE_SETTINGS_FOCUS_FULLSCREEN,
                                "#516171", "#627588", "#2d557f");
    settings_set_control_visual(p, p->el_lbl_vsync, p->el_chk_vsync,
                                p->nav_focus == JCE_SETTINGS_FOCUS_VSYNC,
                                p->pending_vsync ? "#4c83b9" : "#3d5674",
                                p->pending_vsync ? "#70a8df" : "#58738f",
                                "#173252");
    settings_set_control_visual(p, p->el_lbl_master_vol, p->el_rng_master,
                                p->nav_focus == JCE_SETTINGS_FOCUS_MASTER,
                                "#173252", "#24486f", "#173252");
    settings_set_control_visual(p, p->el_lbl_music_vol, p->el_rng_music,
                                p->nav_focus == JCE_SETTINGS_FOCUS_MUSIC,
                                "#173252", "#24486f", "#173252");

    settings_set_button_visual(p, p->el_btn_ok,
                               "#5f89b2", "#7fa8ce", "#203851",
                               p->nav_focus == JCE_SETTINGS_FOCUS_BTN_OK);
    settings_set_button_visual(p, p->el_btn_cancel,
                               "#49627f", "#64819f", "#203851",
                               p->nav_focus == JCE_SETTINGS_FOCUS_BTN_CANCEL);
    settings_set_button_visual(p, p->el_btn_apply,
                               "#49627f", "#64819f", "#203851",
                               p->nav_focus == JCE_SETTINGS_FOCUS_BTN_APPLY);

    if (jce_ui_elem_valid(p->el_btn_quit)) {
        if (settings_has_quit_button(p)) {
            jce_ui_elem_set_property(p->ui, p->el_btn_quit, "display", "inline-block");
            settings_set_button_visual(p, p->el_btn_quit,
                                       "#8b473f", "#b65f53", "#4d201b",
                                       p->nav_focus == JCE_SETTINGS_FOCUS_BTN_QUIT);
        } else {
            jce_ui_elem_set_property(p->ui, p->el_btn_quit, "display", "none");
        }
    }
}

static void settings_move_focus(JceSettingsPanel *p, int delta)
{
    int count = 0;
    const JceSettingsFocusId *order;
    int index = 0;

    if (!p || delta == 0) return;

    order = settings_focus_order(p, &count);
    if (!order || count <= 0) return;

    for (index = 0; index < count; ++index) {
        if (order[index] == (JceSettingsFocusId)p->nav_focus)
            break;
    }
    if (index >= count)
        index = 0;

    index = (index + delta + count) % count;
    p->nav_focus = order[index];
    settings_refresh_navigation(p);
}

static void settings_move_button_focus(JceSettingsPanel *p, int delta)
{
    static const JceSettingsFocusId buttons[] = {
        JCE_SETTINGS_FOCUS_BTN_OK,
        JCE_SETTINGS_FOCUS_BTN_CANCEL,
        JCE_SETTINGS_FOCUS_BTN_APPLY,
        JCE_SETTINGS_FOCUS_BTN_QUIT,
    };
    int button_count = settings_has_quit_button(p) ? 4 : 3;
    int index = 0;

    if (!p || delta == 0) return;

    for (index = 0; index < button_count; ++index) {
        if (buttons[index] == (JceSettingsFocusId)p->nav_focus)
            break;
    }
    if (index >= button_count)
        index = 0;

    index = (index + delta + button_count) % button_count;
    p->nav_focus = buttons[index];
    settings_refresh_navigation(p);
}

static void settings_update_tabs(JceSettingsPanel *p)
{
    if (!p->ui) return;
    bool vid = (p->active_tab == 0);

    if (jce_ui_elem_valid(p->el_panel_video))
        jce_ui_elem_set_property(p->ui, p->el_panel_video,
            "display", vid ? "block" : "none");
    if (jce_ui_elem_valid(p->el_panel_audio))
        jce_ui_elem_set_property(p->ui, p->el_panel_audio,
            "display", vid ? "none" : "block");
    settings_refresh_navigation(p);
}

static void settings_populate(JceSettingsPanel *p)
{
    if (!p->ui) return;
    char buf[32];

    /* Fullscreen select. */
    if (jce_ui_elem_valid(p->el_sel_fullscreen)) {
        p->pending_fullscreen = jce_window_is_fullscreen(p->window);
    }

    /* Resolution display. */
    if (jce_ui_elem_valid(p->el_txt_resolution)) {
        uint32_t w, h;
        jce_window_get_size(p->window, &w, &h);
        snprintf(buf, sizeof(buf), "%ux%u", w, h);
        jce_ui_elem_set_text(p->ui, p->el_txt_resolution, buf);
    }

    /* VSync checkbox. */
    if (jce_ui_elem_valid(p->el_chk_vsync)) {
        p->pending_vsync = jce_renderer_get_vsync(p->renderer);
    }

    /* Volume sliders. */
    p->pending_master_vol = p->volumes.master;
    p->pending_music_vol  = p->volumes.music;
    p->pending_sfx_vol    = p->volumes.sfx;

    settings_sync_pending_controls(p);
}

static void settings_read_ui(JceSettingsPanel *p)
{
    if (!p->ui) return;

    if (jce_ui_elem_valid(p->el_sel_fullscreen)) {
        const char *val = jce_ui_elem_get_value(p->ui,
            p->el_sel_fullscreen);
        p->pending_fullscreen = (val && strcmp(val, "on") == 0);
    }

    if (jce_ui_elem_valid(p->el_chk_vsync)) {
        const char *chk = jce_ui_elem_get_attribute(p->ui,
            p->el_chk_vsync, "checked");
        p->pending_vsync = (chk && chk[0] != '\0');
    }

    if (jce_ui_elem_valid(p->el_rng_master)) {
        const char *v = jce_ui_elem_get_value(p->ui, p->el_rng_master);
        if (v && v[0]) p->pending_master_vol = (float)atoi(v) / 100.0f;
    }
    if (jce_ui_elem_valid(p->el_rng_music)) {
        const char *v = jce_ui_elem_get_value(p->ui, p->el_rng_music);
        if (v && v[0]) p->pending_music_vol = (float)atoi(v) / 100.0f;
    }
    if (jce_ui_elem_valid(p->el_rng_sfx)) {
        const char *v = jce_ui_elem_get_value(p->ui, p->el_rng_sfx);
        if (v && v[0]) p->pending_sfx_vol = (float)atoi(v) / 100.0f;
    }
}

/* ── Event callbacks ──────────────────────────────────────────────── */

static void on_tab_click(JceUIElementHandle elem, const char *event_type,
                         void *userdata)
{
    (void)event_type;
    JceSettingsPanel *p = (JceSettingsPanel *)userdata;
    if (elem.idx == p->el_tab_video.idx) {
        p->active_tab = 0;
        p->nav_focus = JCE_SETTINGS_FOCUS_TAB_VIDEO;
    } else if (elem.idx == p->el_tab_audio.idx) {
        p->active_tab = 1;
        p->nav_focus = JCE_SETTINGS_FOCUS_TAB_AUDIO;
    }
    settings_update_tabs(p);
}

static void on_control_focus(JceUIElementHandle elem, const char *event_type,
                             void *userdata)
{
    (void)event_type;
    JceSettingsPanel *p = (JceSettingsPanel *)userdata;
    if (!p) return;

    if (elem.idx == p->el_sel_fullscreen.idx)
        p->nav_focus = JCE_SETTINGS_FOCUS_FULLSCREEN;
    else if (elem.idx == p->el_chk_vsync.idx)
        p->nav_focus = JCE_SETTINGS_FOCUS_VSYNC;
    else if (elem.idx == p->el_rng_master.idx)
        p->nav_focus = JCE_SETTINGS_FOCUS_MASTER;
    else if (elem.idx == p->el_rng_music.idx)
        p->nav_focus = JCE_SETTINGS_FOCUS_MUSIC;

    settings_refresh_navigation(p);
}

static void on_btn_cancel(JceUIElementHandle elem, const char *event_type,
                          void *userdata)
{
    (void)elem; (void)event_type;
    jce_settings_close((JceSettingsPanel *)userdata);
}

static void on_btn_apply(JceUIElementHandle elem, const char *event_type,
                         void *userdata)
{
    (void)elem; (void)event_type;
    jce_settings_apply((JceSettingsPanel *)userdata);
}

static void on_btn_ok(JceUIElementHandle elem, const char *event_type,
                      void *userdata)
{
    (void)elem; (void)event_type;
    JceSettingsPanel *p = (JceSettingsPanel *)userdata;
    jce_settings_apply(p);
    jce_settings_close(p);
}

static void on_btn_quit(JceUIElementHandle elem, const char *event_type,
                        void *userdata)
{
    (void)elem; (void)event_type;
    JceSettingsPanel *p = (JceSettingsPanel *)userdata;
    if (p && p->on_close_game)
        p->on_close_game(p, p->cb_userdata);
}

/* ── Public API ───────────────────────────────────────────────────── */

JceSettingsPanel *jce_settings_create(const JceSettingsPanelDesc *desc)
{
    if (!desc || !desc->ui || !desc->renderer || !desc->window)
        return NULL;

    JceSettingsPanel *p = (JceSettingsPanel *)JCE_CALLOC(1, sizeof(*p));
    if (!p) return NULL;

    p->ui           = desc->ui;
    p->renderer     = desc->renderer;
    p->window       = desc->window;
    p->audio        = desc->audio;
    p->config       = desc->config;
    p->on_apply     = desc->on_apply;
    p->on_populate  = desc->on_populate;
    p->on_close_game = desc->on_close_game;
    p->cb_userdata  = desc->callback_userdata;
    p->nav_focus    = JCE_SETTINGS_FOCUS_FULLSCREEN;
    p->el_lbl_sfx_vol = JCE_UI_ELEM_INVALID;
    p->el_rng_sfx     = JCE_UI_ELEM_INVALID;

    /* Default volumes. */
    p->volumes.master = desc->config ? desc->config->master_volume : 1.0f;
    p->volumes.music  = desc->config ? desc->config->music_volume  : 0.8f;
    p->volumes.sfx    = desc->config ? desc->config->sfx_volume    : 1.0f;

    /* Load file-based document from engine/ui. */
    p->doc = jce_ui_doc_load_file(p->ui, "engine_settings.rml");
    if (!jce_ui_doc_valid(p->doc)) {
        LOG_ERROR(LOG_TAG, "failed to load settings document: engine_settings.rml");
        JCE_FREE(p);
        return NULL;
    }

    /* Cache element handles. */
    p->el_settings_title = jce_ui_find_element(p->ui, p->doc, "settings-title");
    p->el_tab_video      = jce_ui_find_element(p->ui, p->doc, "tab-video");
    p->el_tab_audio      = jce_ui_find_element(p->ui, p->doc, "tab-audio");
    p->el_panel_video    = jce_ui_find_element(p->ui, p->doc, "panel-video");
    p->el_panel_audio    = jce_ui_find_element(p->ui, p->doc, "panel-audio");
    p->el_lbl_fullscreen = jce_ui_find_element(p->ui, p->doc, "lbl-fullscreen");
    p->el_lbl_resolution = jce_ui_find_element(p->ui, p->doc, "lbl-resolution");
    p->el_lbl_vsync      = jce_ui_find_element(p->ui, p->doc, "lbl-vsync");
    p->el_lbl_master_vol = jce_ui_find_element(p->ui, p->doc, "lbl-master-vol");
    p->el_lbl_music_vol  = jce_ui_find_element(p->ui, p->doc, "lbl-music-vol");
    p->el_sel_fullscreen = jce_ui_find_element(p->ui, p->doc, "sel-fullscreen");
    p->el_txt_resolution = jce_ui_find_element(p->ui, p->doc, "txt-resolution");
    p->el_chk_vsync      = jce_ui_find_element(p->ui, p->doc, "chk-vsync");
    p->el_rng_master     = jce_ui_find_element(p->ui, p->doc, "rng-master");
    p->el_rng_music      = jce_ui_find_element(p->ui, p->doc, "rng-music");
    p->el_btn_cancel     = jce_ui_find_element(p->ui, p->doc, "btn-cancel");
    p->el_btn_apply      = jce_ui_find_element(p->ui, p->doc, "btn-apply");
    p->el_btn_ok         = jce_ui_find_element(p->ui, p->doc, "btn-ok");
    p->el_btn_quit       = jce_ui_find_element(p->ui, p->doc, "btn-quit");

    /* Register click event callbacks. */
    jce_ui_elem_on(p->ui, p->el_tab_video,  "click", on_tab_click,  p);
    jce_ui_elem_on(p->ui, p->el_tab_audio,  "click", on_tab_click,  p);
    jce_ui_elem_on(p->ui, p->el_sel_fullscreen, "click", on_control_focus, p);
    jce_ui_elem_on(p->ui, p->el_chk_vsync,      "click", on_control_focus, p);
    jce_ui_elem_on(p->ui, p->el_rng_master,     "click", on_control_focus, p);
    jce_ui_elem_on(p->ui, p->el_rng_music,      "click", on_control_focus, p);
    jce_ui_elem_on(p->ui, p->el_btn_cancel, "click", on_btn_cancel, p);
    jce_ui_elem_on(p->ui, p->el_btn_apply,  "click", on_btn_apply,  p);
    jce_ui_elem_on(p->ui, p->el_btn_ok,     "click", on_btn_ok,     p);
    if (p->on_close_game && jce_ui_elem_valid(p->el_btn_quit))
        jce_ui_elem_on(p->ui, p->el_btn_quit, "click", on_btn_quit, p);

    /* Apply font family if specified. */
    if (desc->font_family)
        jce_settings_set_font_family(p, desc->font_family);

    settings_refresh_navigation(p);

    LOG_INFO(LOG_TAG, "settings panel created");
    return p;
}

void jce_settings_destroy(JceSettingsPanel *panel)
{
    if (!panel) return;
    if (panel->ui && jce_ui_doc_valid(panel->doc))
        jce_ui_doc_close(panel->ui, panel->doc);
    JCE_FREE(panel);
}

void jce_settings_open(JceSettingsPanel *panel)
{
    if (!panel || !panel->ui || !jce_ui_doc_valid(panel->doc)) return;
    panel->is_open    = true;
    panel->active_tab = 0;
    panel->nav_focus  = JCE_SETTINGS_FOCUS_FULLSCREEN;
    settings_populate(panel);
    settings_update_tabs(panel);
    jce_settings_update_i18n(panel);
    if (panel->on_populate)
        panel->on_populate(panel, panel->cb_userdata);
    jce_ui_doc_show(panel->ui, panel->doc);
}

void jce_settings_close(JceSettingsPanel *panel)
{
    if (!panel || !panel->ui) return;
    panel->is_open = false;
    if (jce_ui_doc_valid(panel->doc))
        jce_ui_doc_hide(panel->ui, panel->doc);
}

bool jce_settings_is_open(const JceSettingsPanel *panel)
{
    return panel ? panel->is_open : false;
}

void jce_settings_apply(JceSettingsPanel *panel)
{
    if (!panel || !panel->ui) return;
    uint32_t w = 0, h = 0;

    settings_read_ui(panel);
    jce_window_get_size(panel->window, &w, &h);

    /* Fullscreen toggle. */
    bool cur_fs = jce_window_is_fullscreen(panel->window);
    if (panel->pending_fullscreen != cur_fs) {
        jce_window_toggle_fullscreen(panel->window);
        jce_window_get_size(panel->window, &w, &h);
        jce_renderer_resize(panel->renderer, w, h);
        jce_ui_resize(panel->ui, w, h);
    }

    /* VSync. */
    jce_renderer_set_vsync_for_size(panel->renderer, panel->pending_vsync,
                                    w, h);

    /* Master volume. */
    if (panel->audio && panel->pending_master_vol != panel->volumes.master) {
        panel->volumes.master = panel->pending_master_vol;
        jce_audio_set_master_volume(panel->audio, panel->volumes.master);
    }

    /* Music volume — tracked but no voice handle here;
       the game callback should handle per-voice adjustments. */
    panel->volumes.music = panel->pending_music_vol;
    panel->volumes.sfx   = panel->pending_sfx_vol;

    /* Re-sync visible controls against the live engine state. */
    settings_populate(panel);
    settings_update_tabs(panel);
    jce_settings_update_i18n(panel);

    /* Notify game. */
    if (panel->on_apply)
        panel->on_apply(panel, panel->cb_userdata);
}

void jce_settings_update_i18n(JceSettingsPanel *panel)
{
    if (!panel || !panel->ui) return;

    if (jce_ui_elem_valid(panel->el_settings_title))
        jce_ui_elem_set_text(panel->ui, panel->el_settings_title,
                             jce_i18n_get(JCE_STR_SETTINGS));
    if (jce_ui_elem_valid(panel->el_tab_video))
        jce_ui_elem_set_text(panel->ui, panel->el_tab_video,
                             jce_i18n_get(JCE_STR_VIDEO));
    if (jce_ui_elem_valid(panel->el_tab_audio))
        jce_ui_elem_set_text(panel->ui, panel->el_tab_audio,
                             jce_i18n_get(JCE_STR_AUDIO));
    if (jce_ui_elem_valid(panel->el_lbl_fullscreen))
        jce_ui_elem_set_text(panel->ui, panel->el_lbl_fullscreen,
                             jce_i18n_get(JCE_STR_FULLSCREEN));
    if (jce_ui_elem_valid(panel->el_lbl_resolution))
        jce_ui_elem_set_text(panel->ui, panel->el_lbl_resolution,
                             jce_i18n_get(JCE_STR_RESOLUTION));
    if (jce_ui_elem_valid(panel->el_lbl_vsync))
        jce_ui_elem_set_text(panel->ui, panel->el_lbl_vsync,
                             jce_i18n_get(JCE_STR_VSYNC));
    if (jce_ui_elem_valid(panel->el_lbl_master_vol))
        jce_ui_elem_set_text(panel->ui, panel->el_lbl_master_vol,
                             jce_i18n_get(JCE_STR_MASTER_VOLUME));
    if (jce_ui_elem_valid(panel->el_lbl_music_vol))
        jce_ui_elem_set_text(panel->ui, panel->el_lbl_music_vol,
                             jce_i18n_get(JCE_STR_MUSIC_VOLUME));
    if (jce_ui_elem_valid(panel->el_lbl_sfx_vol))
        jce_ui_elem_set_text(panel->ui, panel->el_lbl_sfx_vol,
                             jce_i18n_get(JCE_STR_SFX_VOLUME));
    if (jce_ui_elem_valid(panel->el_btn_cancel))
        jce_ui_elem_set_text(panel->ui, panel->el_btn_cancel,
                             jce_i18n_get(JCE_STR_CANCEL));
    if (jce_ui_elem_valid(panel->el_btn_apply))
        jce_ui_elem_set_text(panel->ui, panel->el_btn_apply,
                             jce_i18n_get(JCE_STR_APPLY));
    if (jce_ui_elem_valid(panel->el_btn_ok))
        jce_ui_elem_set_text(panel->ui, panel->el_btn_ok,
                             jce_i18n_get(JCE_STR_OK));
    if (jce_ui_elem_valid(panel->el_btn_quit))
        jce_ui_elem_set_text(panel->ui, panel->el_btn_quit,
                             jce_i18n_get(JCE_STR_QUIT));

    settings_refresh_navigation(panel);
}

void jce_settings_focus_prev(JceSettingsPanel *panel)
{
    settings_move_focus(panel, -1);
}

void jce_settings_focus_next(JceSettingsPanel *panel)
{
    settings_move_focus(panel, 1);
}

void jce_settings_adjust(JceSettingsPanel *panel, int delta)
{
    if (!panel || delta == 0) return;

    settings_read_ui(panel);

    switch ((JceSettingsFocusId)panel->nav_focus) {
    case JCE_SETTINGS_FOCUS_TAB_VIDEO:
    case JCE_SETTINGS_FOCUS_TAB_AUDIO: {
        int next_tab = panel->active_tab + (delta > 0 ? 1 : -1);
        if (next_tab < 0) next_tab = 0;
        if (next_tab > 1) next_tab = 1;
        panel->active_tab = next_tab;
        panel->nav_focus = (next_tab == 0)
                         ? JCE_SETTINGS_FOCUS_TAB_VIDEO
                         : JCE_SETTINGS_FOCUS_TAB_AUDIO;
        settings_update_tabs(panel);
        break;
    }
    case JCE_SETTINGS_FOCUS_FULLSCREEN:
        panel->pending_fullscreen = (delta > 0);
        settings_sync_pending_controls(panel);
        settings_refresh_navigation(panel);
        break;
    case JCE_SETTINGS_FOCUS_VSYNC:
        panel->pending_vsync = (delta > 0);
        settings_sync_pending_controls(panel);
        settings_refresh_navigation(panel);
        break;
    case JCE_SETTINGS_FOCUS_MASTER:
        panel->pending_master_vol = settings_clampf(
            panel->pending_master_vol + ((delta > 0) ? 0.05f : -0.05f),
            0.0f, 1.0f);
        settings_sync_pending_controls(panel);
        settings_refresh_navigation(panel);
        break;
    case JCE_SETTINGS_FOCUS_MUSIC:
        panel->pending_music_vol = settings_clampf(
            panel->pending_music_vol + ((delta > 0) ? 0.05f : -0.05f),
            0.0f, 1.0f);
        settings_sync_pending_controls(panel);
        settings_refresh_navigation(panel);
        break;
    case JCE_SETTINGS_FOCUS_BTN_OK:
    case JCE_SETTINGS_FOCUS_BTN_CANCEL:
    case JCE_SETTINGS_FOCUS_BTN_APPLY:
    case JCE_SETTINGS_FOCUS_BTN_QUIT:
        settings_move_button_focus(panel, delta > 0 ? 1 : -1);
        break;
    }
}

void jce_settings_activate(JceSettingsPanel *panel)
{
    if (!panel) return;

    settings_read_ui(panel);

    switch ((JceSettingsFocusId)panel->nav_focus) {
    case JCE_SETTINGS_FOCUS_TAB_VIDEO:
        panel->active_tab = 0;
        settings_update_tabs(panel);
        break;
    case JCE_SETTINGS_FOCUS_TAB_AUDIO:
        panel->active_tab = 1;
        settings_update_tabs(panel);
        break;
    case JCE_SETTINGS_FOCUS_FULLSCREEN:
        panel->pending_fullscreen = !panel->pending_fullscreen;
        settings_sync_pending_controls(panel);
        settings_refresh_navigation(panel);
        break;
    case JCE_SETTINGS_FOCUS_VSYNC:
        panel->pending_vsync = !panel->pending_vsync;
        settings_sync_pending_controls(panel);
        settings_refresh_navigation(panel);
        break;
    case JCE_SETTINGS_FOCUS_BTN_OK:
        jce_settings_apply(panel);
        jce_settings_close(panel);
        break;
    case JCE_SETTINGS_FOCUS_BTN_CANCEL:
        jce_settings_close(panel);
        break;
    case JCE_SETTINGS_FOCUS_BTN_APPLY:
        jce_settings_apply(panel);
        break;
    case JCE_SETTINGS_FOCUS_BTN_QUIT:
        if (panel->on_close_game)
            panel->on_close_game(panel, panel->cb_userdata);
        break;
    case JCE_SETTINGS_FOCUS_MASTER:
    case JCE_SETTINGS_FOCUS_MUSIC:
        break;
    }
}

void jce_settings_set_font_family(JceSettingsPanel *panel, const char *family)
{
    if (!panel || !panel->ui || !family) return;
    JceUIElementHandle body = jce_ui_doc_get_body(panel->ui, panel->doc);
    if (jce_ui_elem_valid(body))
        jce_ui_elem_set_property(panel->ui, body, "font-family", family);
}

JceUIDocHandle jce_settings_get_doc(const JceSettingsPanel *panel)
{
    return panel ? panel->doc : JCE_UI_DOC_INVALID;
}

JceSettingsVolumes jce_settings_get_volumes(const JceSettingsPanel *panel)
{
    if (panel) return panel->volumes;
    return (JceSettingsVolumes){0};
}

void jce_settings_set_volumes(JceSettingsPanel *panel,
                              const JceSettingsVolumes *v)
{
    if (!panel || !v) return;
    panel->volumes = *v;
}
