/*
 * ck_app.c  Caged Kingdom demo application logic.
 *
 * All game/demo state and per-frame logic lives here.
 * main.c only calls create/destroy/update/event.
 *
 * This file uses ONLY JCE engine APIs — no direct SDL dependency.
 */

#include "ck_app.h"
#include "ck_engine_smoke.h"

#include <jce/api.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_scene_serial.h>

#include <stdlib.h>
#include <string.h>

/* Platform detection comes from <jce/...> headers below; no raw
   __APPLE__ / TargetConditionals.h needed in game code. */

#define FT_HISTORY      120
#define FT_GRAPH_COLS    60
#define FT_SAMPLE_MS     33.0f   /* graph sample interval (~30 Hz) */

struct CkApp {
    JceServices   svc;            /* owned copy of subsystem pointers */
    JceSound      snd_bounce;
    JceSound      snd_music;
    JceVoice      music_voice;

    JceTexture    tex_demo;
    JceFont      *font_main;
    JceFont      *font_i18n;    /* JCE.ttf with CJK glyphs */

    JceTimer     *timer;

    /* 3D scene advances exactly once per update; resize-only redraws reuse
       the latest state without stepping simulation/animation again. */
    float         scene_render_dt_sec;
    bool          scene_render_dt_fresh;

    /* 3D scene (ECS + engine renderer). */
    JceCamera          *camera;
    JceCameraController *cam_ctrl;
    JceScene           *scene;
    JceSceneRenderer   *scene_renderer;

    /* Asset handles for manager-loaded resources. */
    JceAssetHandle h_tex_demo;

    bool          quit_requested;
    bool          paused;
    int           pause_selection;
    bool          debug_hud;
    bool          wireframe;
    bool          f3_was_down;
    bool          f3_combo_used;
    bool          mouse_captured;
    bool          touch_native;

    /* Frametime graph ring buffer (time-interval sampled). */
    float         ft_history[FT_HISTORY];
    int           ft_index;
    float         ft_accum;         /* elapsed ms since last graph sample */
    float         ft_accum_sum;     /* sum of dt_ms in current interval  */
    int           ft_accum_count;   /* frame count in current interval   */

    /* System info (updated once per second). */
    JceSysInfo    sysinfo;

    /* Touch HUD: auto-created on mobile, F9-toggled on desktop. */
    JceTouchHud  *touch_hud;

    /* RmlUI system. */
    JceUIContext  *ui;

    /* Engine panels (debug HUD + settings). */
    JceDebugHud      *engine_hud;
    JceSettingsPanel *engine_settings;

    /* Pause menu (game-specific, loaded from PAK). */
    JceUIDocHandle doc_pause;
    JceUIElementHandle el_pause_title;
    JceUIElementHandle el_pause_continue;
    JceUIElementHandle el_pause_quit;
    JceUIElementHandle el_pause_hint;
};

/* ================================================================== */
/* Engine settings apply callback                                      */
/* ================================================================== */

static void on_ck_settings_applied(JceSettingsPanel *panel, void *ud)
{
    CkApp *app = (CkApp *)ud;
    JceSettingsVolumes v = jce_settings_get_volumes(panel);

    /* Apply music volume to the active music voice. */
    if (app->svc.audio && app->music_voice != JCE_VOICE_INVALID)
        jce_audio_set_volume(app->svc.audio, app->music_voice, v.music);
}

static void on_ck_settings_close_game(JceSettingsPanel *panel, void *ud)
{
    (void)panel;
    CkApp *app = (CkApp *)ud;
    if (app)
        app->quit_requested = true;
}

/* -- Font switching ------------------------------------------------ */

static const char *ck_debug_hud_font_family(void)
{
    return "FOT-MatisseElegantoPro-EB";
}

static const char *ck_panel_font_family(void)
{
    return (jce_i18n_get_lang() == JCE_LANG_ZH_CN) ? "JCE" : "Caveat";
}

static void ck_update_font_family(CkApp *app)
{
    if (!app->ui) return;
    const char *panel_family = ck_panel_font_family();

    /* Pause menu (game-specific doc). */
    if (jce_ui_doc_valid(app->doc_pause)) {
        JceUIElementHandle body = jce_ui_doc_get_body(app->ui, app->doc_pause);
        if (jce_ui_elem_valid(body))
            jce_ui_elem_set_property(app->ui, body, "font-family", panel_family);
    }

    /* Settings follows locale; debug HUD stays on the stylized CK font. */
    jce_settings_set_font_family(app->engine_settings, panel_family);
    jce_debug_hud_set_font_family(app->engine_hud, ck_debug_hud_font_family());
}

/* ================================================================== */
/* RmlUI setup helpers                                                 */
/* ================================================================== */

static void ck_ui_init(CkApp *app)
{
    uint32_t w, h;
    jce_window_get_size(app->svc.window, &w, &h);

    JceUIContextDesc desc = {0};
    desc.width    = w;
    desc.height   = h;
    desc.renderer = app->svc.renderer;
    desc.pak      = app->svc.pak;
    app->ui = jce_ui_create(&desc, jce_allocator_default());
    if (!app->ui) return;

    /* Register fonts available to RML documents. */
    if (!jce_ui_load_font(app->ui, "fonts/Caveat.ttf"))
        LOG_WARN("ui", "Failed to load font: Caveat.ttf");
    if (!jce_ui_load_font(app->ui, "fonts/JCE.ttf"))
        LOG_WARN("ui", "Failed to load font: JCE.ttf");
    if (!jce_ui_load_font(app->ui, "fonts/FOT-MatisseElegantoPro-EB.otf"))
        LOG_WARN("ui", "Failed to load font: FOT-MatisseElegantoPro-EB.otf");

    /* -- Pause menu document (game-specific) -------------------------- */
    app->doc_pause = jce_ui_doc_load_file(app->ui, "pause_menu.rml");
    if (jce_ui_doc_valid(app->doc_pause)) {
        app->el_pause_title    = jce_ui_find_element(app->ui, app->doc_pause, "pause-title");
        app->el_pause_continue = jce_ui_find_element(app->ui, app->doc_pause, "btn-continue");
        app->el_pause_quit     = jce_ui_find_element(app->ui, app->doc_pause, "btn-quit");
        app->el_pause_hint     = jce_ui_find_element(app->ui, app->doc_pause, "hint");
    }

    /* -- Engine debug HUD (file-based engine/ui doc) ------------------ */
    app->engine_hud = jce_debug_hud_create(&(JceDebugHudDesc){
        .ui        = app->ui,
        .renderer  = app->svc.renderer,
        .window    = app->svc.window,
        .font_family = ck_debug_hud_font_family(),
    });

    /* -- Engine settings panel (file-based engine/ui doc) ------------- */
    app->engine_settings = jce_settings_create(&(JceSettingsPanelDesc){
        .ui               = app->ui,
        .renderer         = app->svc.renderer,
        .window           = app->svc.window,
        .audio            = app->svc.audio,
        .config           = app->svc.config,
        .font_family      = ck_panel_font_family(),
        .on_apply         = on_ck_settings_applied,
        .on_close_game    = on_ck_settings_close_game,
        .callback_userdata = app,
    });
}

/* ================================================================== */

CkApp *ck_app_create(const JceServices *svc)
{
    CkApp *app = (CkApp *)jce_malloc(sizeof(*app));
    if (!app) return NULL;
    memset(app, 0, sizeof(*app));
    app->svc = *svc;

    LOG_INFO("ck_app", "[init] step 1: audio");
    /* Load sounds. */
    if (app->svc.audio) {
        app->snd_bounce = jce_audio_load(app->svc.audio, app->svc.pak,
                             "sounds/bounce.wav");
        app->snd_music = jce_audio_load(app->svc.audio, app->svc.pak,
                             "sounds/Aria Math - C418.ogg");
        if (app->snd_music != JCE_SOUND_INVALID) {
            float vol = app->svc.config ? app->svc.config->music_volume : 0.8f;
            app->music_voice = jce_audio_play(app->svc.audio, app->snd_music,
                                              true, vol, 1.0f);
        }
    }

    LOG_INFO("ck_app", "[init] step 2: textures");
    /* Load textures via asset manager. */
    if (app->svc.assets) {
        app->h_tex_demo = jce_asset_load(app->svc.assets,
                              "textures/texture.jpg", JCE_ASSET_TEXTURE);
        app->tex_demo = jce_asset_get_texture(app->svc.assets, app->h_tex_demo);
    } else {
        /* Fallback: direct loading if no asset manager. */
        app->tex_demo = jce_texture_load(app->svc.pak, "textures/texture.jpg");
    }

    LOG_INFO("ck_app", "[init] step 3: i18n");
    /* Load i18n translations from PAK before opening fonts (defensive ordering). */
    jce_i18n_init(app->svc.pak);

    LOG_INFO("ck_app", "[init] step 4: font_main");
    /* Load demo font (32pt, must run on main thread). */
    app->font_main = jce_font_open(app->svc.pak, "fonts/Caveat.ttf", 32.0f);
    LOG_INFO("ck_app", "[init] step 4: font_main=%s", app->font_main ? "ok" : "NULL");

    LOG_INFO("ck_app", "[init] step 5: font_i18n");
    /* Build CJK font atlas using i18n collected codepoints. */
    {
        uint32_t cps[256];
        int cp_count = jce_i18n_collect_codepoints(cps, 256);
        app->font_i18n = jce_font_open_ex(app->svc.pak, "fonts/JCE.ttf",
                                           28.0f, cps, cp_count);
    }
    LOG_INFO("ck_app", "[init] step 5: font_i18n=%s", app->font_i18n ? "ok" : "NULL");

    LOG_INFO("ck_app", "[init] step 6: camera");
    /* 3D scene setup. */
    {
        JceCameraDesc cam_desc = {0};
        cam_desc.mode       = JCE_CAMERA_PERSPECTIVE;
        cam_desc.position   = jce_v3(0.0f, 2.0f, 5.0f);
        cam_desc.target     = jce_v3(0.0f, 0.5f, 0.0f);
        cam_desc.fov_deg    = 60.0f;
        cam_desc.near_plane = 0.1f;
        cam_desc.far_plane  = 100.0f;
        app->camera = jce_camera_create(&cam_desc);
    }
    app->cam_ctrl = jce_camctrl_create(app->camera, NULL);

    LOG_INFO("ck_app", "[init] step 7: scene load");
    /* Create ECS scene and load entities from PAK-packed JSON via VFS.
       No programmatic fallback — ck must consume scene data, not build it.
       Try the new editor-saved extension `.scene` first, then fall back
       to the legacy `.scene.json` for backwards compatibility. */
    app->scene = jce_scene_create();
    {
        JceFileSystem *fs = jce_fs_create();
        jce_fs_mount_pak(fs, app->svc.pak);
        const char *candidates[] = {
            "scenes/main.scene",
            "scenes/main.scene.json",
        };
        bool loaded = false;
        for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
            if (jce_scene_serial_load_vfs(app->scene, fs, candidates[i])) {
                LOG_INFO("ck_app", "scene loaded from PAK: %s", candidates[i]);
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            LOG_ERROR("ck_app",
                      "no startup scene found in PAK (tried scenes/main.scene "
                      "and scenes/main.scene.json)");
        }
        jce_fs_destroy(fs);
    }

    LOG_INFO("ck_app", "[init] step 8: scene_renderer");
    /* Engine scene renderer (resolves textures/models from PAK). */
    app->scene_renderer = jce_scene_renderer_create(
        app->svc.renderer, app->svc.pak, NULL);
    LOG_INFO("ck_app", "[init] step 8: scene_renderer=%s",
             app->scene_renderer ? "ok" : "NULL");

    LOG_INFO("ck_app", "[init] step 9: window icon");
    /* Set window icon from PAK. */
    {
        const JcePakAsset *icon = jce_pak_find(app->svc.pak, "CK_icon.png");
        if (icon) {
            void *buf = jce_malloc((size_t)icon->original_size);
            if (buf) {
                size_t sz = jce_pak_decompress(icon, buf,
                                           (size_t)icon->original_size);
                if (sz > 0)
                    jce_window_set_icon(app->svc.window, buf, sz);
                jce_free(buf);
            }
        } else {
            LOG_WARN("ck_app", "CK_icon.png not found in PAK");
        }
    }

    app->debug_hud = svc->config ? svc->config->debug_text : true;

    LOG_INFO("ck_app", "[init] step 10: timer + sysinfo");
    app->timer = jce_timer_create(0.0);
    jce_sysinfo_init(&app->sysinfo);

    LOG_INFO("ck_app", "[init] step 11: touch HUD");
    /* Touch HUD: auto-create on native touch platforms. */
    {
        bool is_touch_platform = false;
#if JCE_PLATFORM_TOUCH
        is_touch_platform = true;
#endif
        // cppcheck-suppress knownConditionTrueFalse
        if (is_touch_platform) {
            app->touch_hud = jce_touch_hud_create( svc->renderer,
                                    svc->window, app->font_main);
            app->touch_native = true;
        }
    }

    /* Desktop: capture mouse for FPS-style look. */
    if (!app->touch_hud) {
        jce_window_set_relative_mouse_mode(app->svc.window, true);
        app->mouse_captured = true;
    }

    LOG_INFO("ck_app", "[init] step 12: UI");
    /* Initialize RmlUI and load UI documents + engine panels. */
    ck_ui_init(app);

    LOG_INFO("ck_app", "[init] step 13: font family + hud show");
    /* Set initial font family based on current language. */
    ck_update_font_family(app);

    /* Show debug HUD if enabled by config. */
    if (app->debug_hud && app->engine_hud)
        jce_debug_hud_show(app->engine_hud);

    LOG_INFO("ck_app", "[init] complete");
    return app;
}

void ck_app_destroy(CkApp *app)
{
    if (!app) return;

    /* Close settings first to prevent event callbacks during UI teardown. */
    jce_settings_close(app->engine_settings);

    /* Destroy engine panels before UI context. */
    jce_settings_destroy(app->engine_settings);
    jce_debug_hud_destroy(app->engine_hud);

    if (app->mouse_captured)
        jce_window_set_relative_mouse_mode(app->svc.window, false);
    jce_ui_destroy(app->ui);
    app->ui = NULL;
    jce_touch_hud_destroy(app->touch_hud);
    jce_timer_destroy(app->timer);

    /* Scene renderer + ECS scene + camera. */
    jce_scene_renderer_destroy(app->scene_renderer);
    jce_scene_destroy(app->scene);
    jce_camctrl_destroy(app->cam_ctrl);
    jce_camera_destroy(app->camera);

    /* Release demo texture (used by 2D overlay path historically). */
    if (app->svc.assets) {
        jce_asset_release(app->svc.assets, app->h_tex_demo);
    } else {
        jce_texture_destroy(app->tex_demo);
    }
    jce_font_close(app->font_i18n);
    jce_font_close(app->font_main);
    jce_free(app);
}

/* ================================================================== */
/* Debug HUD update (delegates to engine panel)                        */
/* ================================================================== */

static void update_debug_hud(CkApp *app, float dt_ms)
{
    if (!app->engine_hud) return;

    jce_debug_hud_update(app->engine_hud, &(JceDebugHudData){
        .fps          = jce_timer_fps(app->timer),
        .frametime_ms = dt_ms,
        .cpu_cores    = app->sysinfo.cpu_cores,
        .cpu_usage    = app->sysinfo.cpu_usage,
        .ram_used_mb  = app->sysinfo.ram_used_mb,
        .ram_total_mb = app->sysinfo.ram_total_mb,
        .frametime_history = app->ft_history,
        .frametime_history_count = FT_HISTORY,
        .frametime_history_head = app->ft_index,
        .frametime_graph_columns = FT_GRAPH_COLS,
        .extra_status = app->wireframe ? "WIREFRAME" : NULL,
        .shortcut_hints = "F3+V Wireframe   F3+L Language",
    });
}

static void ck_set_paused(CkApp *app, bool paused)
{
    if (!app) return;

    app->paused = paused;
    app->pause_selection = 0;
    jce_touch_hud_set_menu_mode(app->touch_hud, paused);

    if (app->ui && jce_ui_doc_valid(app->doc_pause)) {
        if (paused)
            jce_ui_doc_show(app->ui, app->doc_pause);
        else
            jce_ui_doc_hide(app->ui, app->doc_pause);
    }
}

/* ================================================================== */
/* RmlUI pause menu update                                             */
/* ================================================================== */

static void update_pause_menu_rml(CkApp *app)
{
    if (!app->ui) return;

    /* Update i18n text. */
    if (jce_ui_elem_valid(app->el_pause_title))
        jce_ui_elem_set_text(app->ui, app->el_pause_title,
                             jce_i18n_get(JCE_STR_PAUSED));
    if (jce_ui_elem_valid(app->el_pause_continue))
        jce_ui_elem_set_text(app->ui, app->el_pause_continue,
                             jce_i18n_get(JCE_STR_CONTINUE));
    if (jce_ui_elem_valid(app->el_pause_quit))
        jce_ui_elem_set_text(app->ui, app->el_pause_quit,
                             jce_i18n_get(JCE_STR_QUIT));
    if (jce_ui_elem_valid(app->el_pause_hint))
        jce_ui_elem_set_text(app->ui, app->el_pause_hint,
                             jce_i18n_get(JCE_STR_CONTROLS_HINT));

    /* Highlight selected item. */
    if (jce_ui_elem_valid(app->el_pause_continue))
        jce_ui_elem_set_property(app->ui, app->el_pause_continue,
            "color", app->pause_selection == 0 ? "#64ff64" : "#808080");
    if (jce_ui_elem_valid(app->el_pause_quit))
        jce_ui_elem_set_property(app->ui, app->el_pause_quit,
            "color", app->pause_selection == 1 ? "#ff6464" : "#808080");
}

/* ================================================================== */
/* Input handling                                                      */
/* ================================================================== */

static void record_frametime(CkApp *app, float dt_ms)
{
    app->ft_accum      += dt_ms;
    app->ft_accum_sum  += dt_ms;
    app->ft_accum_count++;

    if (app->ft_accum >= FT_SAMPLE_MS) {
        /* Store average frametime for this interval. */
        app->ft_history[app->ft_index] =
            app->ft_accum_sum / (float)app->ft_accum_count;
        app->ft_index = (app->ft_index + 1) % FT_HISTORY;
        app->ft_accum       = 0.0f;
        app->ft_accum_sum   = 0.0f;
        app->ft_accum_count = 0;
    }
}

static void update_sysinfo(CkApp *app)
{
    double elapsed = jce_timer_elapsed(app->timer);
    static double last_sysinfo = 0.0;
    if (elapsed - last_sysinfo >= 1.0) {
        jce_sysinfo_update(&app->sysinfo);
        last_sysinfo = elapsed;
    }
}

static void handle_input(CkApp *app, float dt_ms)
{
    const JceInput *input = app->svc.input;

    jce_touch_hud_update(app->touch_hud, input, dt_ms);

    /* Desktop mouse capture: ALT to release, auto-release
       when paused. Touch HUD active = mouse stays free. */
    if (!app->touch_hud && !app->touch_native) {
        bool alt_held =
            jce_input_key_down(input, JCE_KEY_LALT) ||
            jce_input_key_down(input, JCE_KEY_RALT);
        bool want_capture = !app->paused && !jce_settings_is_open(app->engine_settings) && !alt_held;
        if (want_capture != app->mouse_captured) {
            jce_window_set_relative_mouse_mode(
                app->svc.window,
                want_capture);
            app->mouse_captured = want_capture;
        }
    }

    /* Touch pause button => open/close settings (same as ESC). */
    if (jce_touch_hud_button(app->touch_hud, JCE_TOUCH_BTN_PAUSE)) {
        if (jce_settings_is_open(app->engine_settings)) {
            jce_settings_close(app->engine_settings);
            if (app->debug_hud && app->engine_hud)
                jce_debug_hud_show(app->engine_hud);
        } else {
            if (app->debug_hud && app->engine_hud)
                jce_debug_hud_hide(app->engine_hud);
            jce_settings_open(app->engine_settings);
        }
    }

    /* ESC / Android Back => settings or pause logic. */
    if (jce_input_key_pressed(input, JCE_KEY_ESCAPE) ||
        jce_input_key_pressed(input, JCE_KEY_AC_BACK)) {
        if (jce_settings_is_open(app->engine_settings)) {
            /* Close settings without applying. */
            jce_settings_close(app->engine_settings);
            /* Restore debug HUD if it was enabled. */
            if (app->debug_hud && app->engine_hud)
                jce_debug_hud_show(app->engine_hud);
        } else if (app->paused) {
            ck_set_paused(app, false);
        } else {
            /* Open settings menu; hide HUD to prevent overlap. */
            if (app->debug_hud && app->engine_hud)
                jce_debug_hud_hide(app->engine_hud);
            jce_settings_open(app->engine_settings);
        }
    }

    if (jce_settings_is_open(app->engine_settings)) {
        if (jce_input_key_pressed(input, JCE_KEY_UP) ||
            jce_input_key_pressed(input, JCE_KEY_W))
            jce_settings_focus_prev(app->engine_settings);
        if (jce_input_key_pressed(input, JCE_KEY_DOWN) ||
            jce_input_key_pressed(input, JCE_KEY_S))
            jce_settings_focus_next(app->engine_settings);
        if (jce_input_key_pressed(input, JCE_KEY_LEFT) ||
            jce_input_key_pressed(input, JCE_KEY_A))
            jce_settings_adjust(app->engine_settings, -1);
        if (jce_input_key_pressed(input, JCE_KEY_RIGHT) ||
            jce_input_key_pressed(input, JCE_KEY_D))
            jce_settings_adjust(app->engine_settings, 1);
        if (jce_input_key_pressed(input, JCE_KEY_RETURN) ||
            jce_input_key_pressed(input, JCE_KEY_SPACE))
            jce_settings_activate(app->engine_settings);
    }

    /* P key => toggle pause (separate from ESC/settings). */
    if (jce_input_key_pressed(input, JCE_KEY_P) && !jce_settings_is_open(app->engine_settings)) {
        ck_set_paused(app, !app->paused);
    }

    /* F11 => fullscreen. */
    if (jce_input_key_pressed(input, JCE_KEY_F11))
        jce_window_toggle_fullscreen(app->svc.window);

    /* F3 debug combos (Minecraft-style: toggle on release if no combo used).
       F3+V => wireframe. */
    {
        bool f3_down = jce_input_key_down(input, JCE_KEY_F3);

        if (f3_down && jce_input_key_pressed(input, JCE_KEY_V)) {
            app->wireframe = !app->wireframe;
            jce_renderer_set_wireframe(app->svc.renderer, app->wireframe);
            app->f3_combo_used = true;
        }
        if (f3_down && jce_input_key_pressed(input, JCE_KEY_L)) {
            JceLang lang = (JceLang)((jce_i18n_get_lang() + 1)
                                     % JCE_LANG_COUNT);
            jce_i18n_set_lang(lang);
            ck_update_font_family(app);
            jce_settings_update_i18n(app->engine_settings);
            app->f3_combo_used = true;
        }

        if (app->f3_was_down && !f3_down) {
            if (!app->f3_combo_used &&
                !jce_settings_is_open(app->engine_settings)) {
                app->debug_hud = !app->debug_hud;
                if (app->engine_hud) {
                    if (app->debug_hud)
                        jce_debug_hud_show(app->engine_hud);
                    else
                        jce_debug_hud_hide(app->engine_hud);
                }
            }
            app->f3_combo_used = false;
        }
        app->f3_was_down = f3_down;
    }

    /* F9 => toggle touch HUD. */
    if (jce_input_key_pressed(input, JCE_KEY_F9)) {
        if (app->touch_native) {
            if (app->touch_hud)
                jce_touch_hud_set_visible(app->touch_hud,
                    !jce_touch_hud_is_visible(app->touch_hud));
        } else if (app->touch_hud) {
            jce_touch_hud_destroy(app->touch_hud);
            app->touch_hud = NULL;
            jce_window_set_relative_mouse_mode(
                app->svc.window, true);
            app->mouse_captured = true;
        } else {
            app->touch_hud = jce_touch_hud_create(
                app->svc.renderer, app->svc.window,
                app->font_main);
            if (app->mouse_captured) {
                jce_window_set_relative_mouse_mode(
                    app->svc.window, false);
                app->mouse_captured = false;
            }
        }
    }
}

/* -- Pause menu logic ---------------------------------------------- */

static void update_pause(CkApp *app, float dt_ms)
{
    const JceInput *input = app->svc.input;

    if (jce_input_key_pressed(input, JCE_KEY_UP) ||
        jce_input_key_pressed(input, JCE_KEY_W))
        app->pause_selection = 0;
    if (jce_input_key_pressed(input, JCE_KEY_DOWN) ||
        jce_input_key_pressed(input, JCE_KEY_S))
        app->pause_selection = 1;
    if (jce_input_key_pressed(input, JCE_KEY_RETURN)) {
        if (app->pause_selection == 0) {
            ck_set_paused(app, false);
        } else {
            app->quit_requested = true;
        }
    }

    /* Touch HUD menu buttons. */
    if (jce_touch_hud_button(app->touch_hud,
                             JCE_TOUCH_BTN_MENU_0)) {
        ck_set_paused(app, false);
    }
    if (jce_touch_hud_button(app->touch_hud,
                             JCE_TOUCH_BTN_MENU_1))
        app->quit_requested = true;
}

/* -- 3D scene ------------------------------------------------------ */

static void update_3d_scene(CkApp *app, float dt_ms)
{
    float dt_sec = dt_ms / 1000.0f;

    /* Gather camera input. */
    JceCameraInput cam_in = {0};

    /* WASD. */
    const JceInput *in = app->svc.input;
    if (jce_input_key_down(in, JCE_KEY_W))
        cam_in.move_forward -= 1;
    if (jce_input_key_down(in, JCE_KEY_S))
        cam_in.move_forward += 1;
    if (jce_input_key_down(in, JCE_KEY_A))
        cam_in.move_right -= 1;
    if (jce_input_key_down(in, JCE_KEY_D))
        cam_in.move_right += 1;

    /* Touch joystick. */
    {
        float tdx = 0, tdz = 0;
        jce_touch_hud_get_move(app->touch_hud, &tdx, &tdz);
        cam_in.move_forward += tdz;
        cam_in.move_right   += tdx;
    }

    /* Vertical: Space/Shift + touch buttons. */
    if (jce_input_key_down(in, JCE_KEY_SPACE))
        cam_in.move_up += 1;
    if (jce_input_key_down(in, JCE_KEY_LSHIFT) ||
        jce_input_key_down(in, JCE_KEY_RSHIFT))
        cam_in.move_up -= 1;
    if (jce_touch_hud_button_down(app->touch_hud,
                                  JCE_TOUCH_BTN_JUMP))
        cam_in.move_up += 1;
    if (jce_touch_hud_button_down(app->touch_hud,
                                  JCE_TOUCH_BTN_CROUCH))
        cam_in.move_up -= 1;

    /* Sprint toggle. */
    cam_in.sprint_toggle =
        jce_input_key_pressed(in, JCE_KEY_LCTRL) ||
        jce_input_key_pressed(in, JCE_KEY_RCTRL);

    /* Mouse look + touch look. */
    if (app->mouse_captured) {
        float mdx = 0, mdy = 0;
        jce_input_mouse_delta(in, &mdx, &mdy);
        cam_in.look_yaw   += mdx * 0.002f;
        cam_in.look_pitch -= mdy * 0.002f;
    }
    {
        float tyaw = 0, tpitch = 0;
        jce_touch_hud_get_look(app->touch_hud,
                               &tyaw, &tpitch);
        cam_in.look_yaw   += tyaw   * JCE_DEG2RAD;
        cam_in.look_pitch += tpitch * JCE_DEG2RAD;
    }

    jce_camctrl_update(app->cam_ctrl, &cam_in, dt_sec);
}

static void draw_3d_scene(CkApp *app, float dt_sec)
{
    LOG_INFO("ck_draw", "begin_frame_3d");
    jce_renderer_begin_frame_3d(app->svc.renderer,
        app->svc.window, app->camera, JCE_VIEW_MAIN_3D);

    LOG_INFO("ck_draw", "scene_renderer_render");
    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    jce_scene_renderer_render(
        app->scene_renderer, app->scene, app->camera,
        JCE_VIEW_MAIN_3D, dt_sec, &cfg);
    LOG_INFO("ck_draw", "scene_renderer_render done");
}

/* -- 2D overlay ---------------------------------------------------- */

static void draw_2d_overlay(CkApp *app)
{
    LOG_INFO("ck_draw", "touch_hud_draw start hud=%p", (void*)app->touch_hud);
    jce_touch_hud_draw(app->touch_hud);
    LOG_INFO("ck_draw", "touch_hud_draw done");
}

static int s_draw_frame_count = 0;

static void ck_app_draw(CkApp *app)
{
    if (!app) return;

    int fc = ++s_draw_frame_count;
    LOG_INFO("ck_draw", "frame=%d start", fc);

#ifdef __ANDROID__
    if (jce_renderer_is_egl_hung()) {
        LOG_INFO("ck_draw", "frame=%d skipped (egl hung)", fc);
        return;
    }
#endif

    if (jce_settings_is_open(app->engine_settings) || app->paused) {
        if (app->ui)
            jce_ui_render(app->ui);
        jce_debug_hud_draw(app->engine_hud);
        return;
    }

    float scene_dt_sec = 0.0f;
    if (app->scene_render_dt_fresh) {
        scene_dt_sec = app->scene_render_dt_sec;
        app->scene_render_dt_fresh = false;
    }

    draw_3d_scene(app, scene_dt_sec);
    draw_2d_overlay(app);

    /* Render RmlUI overlay (debug HUD, etc). */
    if (app->ui)
        jce_ui_render(app->ui);
    jce_debug_hud_draw(app->engine_hud);
}

/* ================================================================== */

void ck_app_update(CkApp *app)
{
    static int s_update_frame = 0;
    int uf = ++s_update_frame;
    LOG_INFO("ck_update", "frame=%d start", uf);

    JCE_PROFILE_ZONE_N("CkApp::Update");
    if (!app) { JCE_PROFILE_ZONE_END; return; }

    LOG_INFO("ck_update", "frame=%d timer", uf);
    bool settings_open_before = jce_settings_is_open(app->engine_settings);
    jce_timer_tick(app->timer);
    float dt_ms = jce_timer_dt_ms(app->timer);
    float dt_sec = dt_ms / 1000.0f;
    app->scene_render_dt_sec = 0.0f;
    app->scene_render_dt_fresh = false;
    record_frametime(app, dt_ms);
    update_sysinfo(app);

    LOG_INFO("ck_update", "frame=%d input", uf);
    handle_input(app, dt_ms);

    LOG_INFO("ck_update", "frame=%d hud", uf);
    /* Update RmlUI element data BEFORE Update() so layout is correct. */
    if (jce_settings_is_open(app->engine_settings)) {
        if (app->debug_hud)
            update_debug_hud(app, dt_ms);
    } else if (app->paused) {
        update_pause(app, dt_ms);
        update_pause_menu_rml(app);
        if (app->debug_hud)
            update_debug_hud(app, dt_ms);
    } else {
        if (app->debug_hud)
            update_debug_hud(app, dt_ms);
    }

    LOG_INFO("ck_update", "frame=%d ui_process_input", uf);
    /* Feed input to RmlUI, compute layout, then render. */
    if (app->ui) {
        if (jce_settings_is_open(app->engine_settings))
            jce_ui_process_pointer_input(app->ui, app->svc.input);
        else
            jce_ui_process_input(app->ui, app->svc.input);
        LOG_INFO("ck_update", "frame=%d ui_update", uf);
        jce_ui_update(app->ui, dt_sec);
        LOG_INFO("ck_update", "frame=%d ui_update done", uf);
    }

    if (settings_open_before &&
        !jce_settings_is_open(app->engine_settings) &&
        app->debug_hud && app->engine_hud) {
        jce_debug_hud_show(app->engine_hud);
    }

    LOG_INFO("ck_update", "frame=%d scene3d", uf);
    if (!jce_settings_is_open(app->engine_settings) && !app->paused) {
        update_3d_scene(app, dt_ms);
        app->scene_render_dt_sec = dt_sec;
        app->scene_render_dt_fresh = true;
    }

    LOG_INFO("ck_update", "frame=%d done", uf);
    JCE_PROFILE_ZONE_END;
}

void ck_app_event(CkApp *app, const JceEvent *event)
{
    (void)app; (void)event;
}

bool ck_app_should_quit(const CkApp *app)
{
    return app ? app->quit_requested : false;
}

/* ================================================================== */
/* JceAppDesc callbacks (IApp pattern)                                 */
/* ================================================================== */

static CkApp *s_app;

static bool demo_init(const JceServices *svc, void *ud)
{
    (void)ud;
    /* ck is the integration-test harness for the JCE engine.  Run the
     * Stage 17–26 smoke tests once at startup; failures are logged but
     * do not block the game from launching. */
    ck_engine_smoke_run();
    s_app = ck_app_create(svc);
    return s_app != NULL;
}

static void demo_exit(void *ud)
{
    (void)ud;
    ck_app_destroy(s_app);
    s_app = NULL;
}

static void demo_update(float dt, void *ud)
{
    (void)dt; (void)ud;
    ck_app_update(s_app);
}

static void demo_draw(const JceServices *svc, void *ud)
{
    (void)svc; (void)ud;
    ck_app_draw(s_app);
}

static void demo_on_resize(uint32_t w, uint32_t h, void *ud)
{
    (void)ud;
    if (s_app && s_app->ui)
        jce_ui_resize(s_app->ui, w, h);
}

static void demo_on_event(const JceEvent *ev, void *ud)
{
    (void)ud;
    ck_app_event(s_app, ev);
}

static bool demo_should_quit(void *ud)
{
    (void)ud;
    return ck_app_should_quit(s_app);
}

JceAppDesc ck_app_get_desc(void)
{
    return (JceAppDesc){
        .name        = "Caged Kingdom",
        .init        = demo_init,
        .exit        = demo_exit,
        .update      = demo_update,
        .draw        = demo_draw,
        .on_event    = demo_on_event,
        .on_resize   = demo_on_resize,
        .should_quit = demo_should_quit,
    };
}
