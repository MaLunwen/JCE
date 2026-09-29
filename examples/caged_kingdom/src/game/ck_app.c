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
#include "ck_scene_director.h"

#include <jce/api.h>
#include <jce/renderer/jce_image.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_renderer_caps.h>   /* jce_renderer_get_tier (baseline gating) */
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

    JceTexture    tex_demo;
    JceFont      *font_main;
    JceFont      *font_i18n;    /* JCE.ttf with CJK glyphs */

    JceTimer     *timer;

    /* 3D scene advances exactly once per update; resize-only redraws reuse
       the latest state without stepping simulation/animation again. */
    float         scene_render_dt_sec;
    bool          scene_render_dt_fresh;

    /* 3D scene (ECS + engine renderer), owned by the director.
       Direct pointers kept for hot-path call sites; ownership belongs
       to `director`. */
    JceCamera          *camera;
    JceCameraController *cam_ctrl;
    CkSceneDirector    *director;
    JceScene           *scene;
    JceSceneRenderer   *scene_renderer;
    JceOffscreenTarget *post_target;
    JceScene           *camera_binding_scene;
    JceSceneCameraResolveResult camera_binding_result;

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

    if (app->svc.audio)
        jce_audio_set_master_volume(app->svc.audio, v.master);
}

static void on_ck_settings_close_game(JceSettingsPanel *panel, void *ud)
{
    (void)panel;
    CkApp *app = (CkApp *)ud;
    if (app)
        app->quit_requested = true;
}

/* -- Font switching ------------------------------------------------ */

static const char *ck_panel_font_family(void);

static const char *ck_debug_hud_font_family(void)
{
    return ck_panel_font_family();
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

    /* Settings follows locale; debug HUD uses the same default family. */
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

    LOG_INFO("ck_app", "[init] step 7: scene load (director)");
    /* Create scene + scene renderer via the director, which owns both
       and provides the runtime swap path used by level transitions
       (see examples/caged_kingdom/SCENES_DESIGN.md appendix A). */
    app->director = ck_scene_director_create(app->svc.renderer, app->svc.pak,
                                             app->svc.audio);
    if (!app->director) {
        LOG_ERROR("ck_app", "ck_scene_director_create failed");
    } else {
        /* Launch contract precedence matches the generic runtime shell:
         * explicit debugger/editor/CI override, shipping PAK metadata, then
         * the legacy storyline graph.  The app remains project-specific;
         * path parsing and PAK boot metadata stay generic engine APIs. */
        JceRuntimeBootManifest boot = {0};
        char startup_scene[JCE_RUNTIME_BOOT_SCENE_PATH_MAX] = {0};
        bool loaded = false;

        if (jce_args_get_startup_scene(startup_scene,
                                       sizeof(startup_scene))) {
            LOG_INFO("ck_app", "startup scene override: %s", startup_scene);
            loaded = ck_scene_director_load_initial(app->director,
                                                     startup_scene);
        } else if (jce_runtime_boot_manifest_load_pak(app->svc.pak, &boot) &&
                   boot.startup_scene[0]) {
            LOG_INFO("ck_app", "startup scene from runtime boot: %s",
                     boot.startup_scene);
            loaded = ck_scene_director_load_initial(app->director,
                                                     boot.startup_scene);
        }
        if (!loaded)
            loaded = ck_scene_director_load_start(app->director);
        if (!loaded) {
            const char *candidates[] = {
                "scenes/act1_m01_wake.scene.json",
                "scenes/main.scene",
                "scenes/main.scene.json",
            };
            for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i) {
                if (ck_scene_director_load_initial(app->director, candidates[i])) {
                    loaded = true;
                    break;
                }
            }
        }
        if (!loaded) {
            LOG_ERROR("ck_app",
                      "no startup scene found in PAK (quest graph "
                      "+ fallbacks all failed)");
        }
    }
    app->scene          = ck_scene_director_scene(app->director);
    app->scene_renderer = ck_scene_director_renderer(app->director);

    if (app->scene_renderer) {
        char shader_dir[1024];

        if (jce_args_get_shader_dev_dir(shader_dir, sizeof(shader_dir))) {
            jce_scene_renderer_set_project_shader_dir(app->scene_renderer,
                                                       shader_dir);
            LOG_INFO("ck_app", "project shader overlay: %s", shader_dir);
        }
    }

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
                if (sz > 0) {
                    /* Decode via the image service — see jce_window.h: the
                     * platform layer no longer holds a decoder. */
                    int iw = 0, ih = 0;
                    uint8_t *px = jce_image_load_rgba8_from_memory(
                        buf, (uint64_t)sz, &iw, &ih);
                    if (px) {
                        jce_window_set_icon_rgba8(app->svc.window, px, iw, ih);
                        jce_image_free_rgba8(px);
                    }
                }
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
    if (app->post_target) {
        jce_offscreen_target_destroy(app->post_target);
        app->post_target = NULL;
    }

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

    /* Scene renderer + ECS scene (both owned by the director) + camera. */
    ck_scene_director_destroy(app->director);
    app->director       = NULL;
    app->scene          = NULL;
    app->scene_renderer = NULL;
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

    /* After the camera moves, feed its position to the director so
       trigger zones in the active scene can fire transitions.  v1 uses
       the camera as the player proxy; once a real player entity exists
       this will switch to its Transform's position.  Y is ignored
       inside ck_trigger (XZ disc check), so the camera's flight height
       does not affect zone tests. */
    {
        jce_vec3 p = jce_camera_get_position(app->camera);
        const float pos[3] = { p.x, p.y, p.z };
        ck_scene_director_tick(app->director, dt_sec, pos);
        /* The director may have swapped scene/renderer underneath us;
           keep our cached pointers in sync. */
        app->scene          = ck_scene_director_scene(app->director);
        app->scene_renderer = ck_scene_director_renderer(app->director);
    }

    /* A scene-authored primary Camera is the runtime view authority.  Apply it
       after script/simulation updates so editor Play and the shipped app see
       the same pose.  Scenes without one retain the free-fly camera above. */
    if (app->scene) {
        JceSceneCameraResolveResult result =
            jce_scene_camera_apply_primary(app->scene, app->camera, NULL);
        if (app->camera_binding_scene != app->scene ||
            app->camera_binding_result != result) {
            if (result == JCE_SCENE_CAMERA_RESOLVE_OK) {
                LOG_INFO("ck_app", "bound runtime view to scene primary camera");
            } else if (result == JCE_SCENE_CAMERA_RESOLVE_AMBIGUOUS ||
                       result == JCE_SCENE_CAMERA_RESOLVE_INVALID_POSE) {
                LOG_ERROR("ck_app", "scene primary camera rejected (status=%d)",
                          (int)result);
            }
            app->camera_binding_scene = app->scene;
            app->camera_binding_result = result;
        }
    }

    /* Match editor Game View and the stock runtime: a scene-authored active
       VirtualCamera overrides free-fly input after simulation for this frame.
       With no active VCam the camera controller above remains authoritative. */
    if (app->scene) {
        JceVcamOutput output;
        bool has_vcam = false;
        jce_vcam_system_evaluate(app->scene, dt_sec, &output, &has_vcam);
        if (has_vcam) {
            jce_camera_set_position(app->camera,
                jce_v3(output.position[0], output.position[1], output.position[2]));
            jce_camera_look_at(app->camera,
                jce_v3(output.target[0], output.target[1], output.target[2]));
            jce_camera_set_fov(app->camera, output.fov_deg);
        }
    }
}

static void draw_3d_scene(CkApp *app, float dt_sec)
{
    JceSceneRenderConfig cfg = jce_scene_render_config_default();
    /* Tier-gate expensive features for the 512MB / no-GPU baseline: skip the
       multi-cascade shadow passes on LOW-tier (old integrated) GPUs.  LOW-tier
       postfx is tonemap-only (single pass, cheap) so it stays enabled. */
    cfg.draw_shadows = (jce_renderer_get_tier() >= JCE_GPU_TIER_MEDIUM);
    JcePostFXPipeline *postfx =
        jce_scene_renderer_get_postfx(app->scene_renderer);
    bool has_fullscreen =
        jce_scene_renderer_has_fullscreen_effect(app->scene);
    bool has_postfx = false;
    uint32_t width = 0, height = 0;

    if (postfx) {
        for (int i = 0; i < JCE_POSTFX_COUNT; ++i) {
            if (jce_postfx_is_enabled(postfx, (JcePostFXType)i)) {
                has_postfx = true;
                break;
            }
        }
    }
    if (app->svc.window)
        jce_window_get_size(app->svc.window, &width, &height);

    if ((has_fullscreen || has_postfx) && postfx && width && height) {
        if (!app->post_target) {
            app->post_target = jce_offscreen_target_create(
                app->svc.renderer, JCE_VIEW_RUNTIME_GAME);
            jce_postfx_set_view_base(postfx, 100);
        }
        if (app->post_target) {
            const float aspect = (float)width / (float)height;
            jce_mat4 view = jce_camera_view(app->camera);
            jce_mat4 proj = jce_camera_proj(app->camera, aspect,
                                            jce_gfx_caps().homogeneous_depth);
            if (jce_offscreen_target_prepare(app->post_target, width, height,
                    view.raw[0], proj.raw[0], 0x000000ffu,
                    "CagedKingdomScene")) {
                const uint16_t base = jce_offscreen_target_get_view_id(
                    app->post_target);
                cfg.scene_frame_buffer = jce_offscreen_target_get_frame_buffer(
                    app->post_target);
                cfg.viewport_width = width;
                cfg.viewport_height = height;
                cfg.scene_depth_tex_handle =
                    jce_offscreen_target_get_depth_texture(app->post_target);
                cfg.ssr_color_tex_handle =
                    jce_offscreen_target_get_color_texture(app->post_target);
                cfg.gi_color_tex_handle = cfg.ssr_color_tex_handle;
                cfg.fog_enabled = jce_scene_fog_params_from_scene(
                    app->scene, &cfg.fog);
                if (cfg.fog_enabled) {
                    cfg.fog_rt_width = (int)width;
                    cfg.fog_rt_height = (int)height;
                }

                jce_scene_renderer_render(app->scene_renderer, app->scene,
                    app->camera, base, dt_sec, &cfg);
                if (cfg.fog_enabled)
                    jce_scene_renderer_composite_fog(app->scene_renderer,
                        (uint16_t)(base + 16), cfg.scene_frame_buffer);
                jce_scene_renderer_composite_ssr(app->scene_renderer,
                    (uint16_t)(base + 19), cfg.scene_frame_buffer);

                JceTextureHandle color = {
                    jce_offscreen_target_get_color_texture(app->post_target) };
                JceTextureHandle depth = {
                    jce_offscreen_target_get_depth_texture(app->post_target) };
                if (has_fullscreen && jce_gfx_texture_valid(color)) {
                    color = jce_scene_renderer_apply_fullscreen_effects(
                        app->scene_renderer, app->scene, app->camera,
                        color, depth, width, height, 50, 0,
                        JCE_FULLSCREEN_EFFECT_HDR_BEFORE_POSTFX, dt_sec);
                }
                if (jce_gfx_texture_valid(color)) {
                    jce_postfx_resize(postfx, width, height);
                    jce_postfx_apply(postfx, color, depth);
                    if (jce_gfx_texture_valid(jce_postfx_get_output(postfx))) {
                        jce_postfx_present(postfx, width, height);
                        LOG_TRACE("ck_draw", "offscreen scene presented");
                        return;
                    }
                }
            }
        }
    }

    cfg = jce_scene_render_config_default();
    cfg.draw_shadows = (jce_renderer_get_tier() >= JCE_GPU_TIER_MEDIUM);
    cfg.viewport_width = width;
    cfg.viewport_height = height;
    LOG_TRACE("ck_draw", "begin_frame_3d");
    jce_renderer_begin_frame_3d(app->svc.renderer,
        app->svc.window, app->camera, JCE_VIEW_MAIN_3D);
    jce_scene_renderer_render(app->scene_renderer, app->scene, app->camera,
        JCE_VIEW_MAIN_3D, dt_sec, &cfg);
    LOG_TRACE("ck_draw", "scene_renderer_render done");
}

/* -- 2D overlay ---------------------------------------------------- */

static void draw_2d_overlay(CkApp *app)
{
    LOG_TRACE("ck_draw", "touch_hud_draw start hud=%p", (void*)app->touch_hud);
    jce_touch_hud_draw(app->touch_hud);
    LOG_TRACE("ck_draw", "touch_hud_draw done");
}

static int s_draw_frame_count = 0;

static void ck_app_draw(CkApp *app)
{
    if (!app) return;

    int fc = ++s_draw_frame_count;
    LOG_TRACE("ck_draw", "frame=%d start", fc);

    if (jce_renderer_is_egl_hung()) {
        LOG_INFO("ck_draw", "frame=%d skipped (egl hung)", fc);
        return;
    }

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
    LOG_TRACE("ck_update", "frame=%d start", uf);

    JCE_PROFILE_ZONE_N("CkApp::Update");
    if (!app) { JCE_PROFILE_ZONE_END; return; }

    LOG_TRACE("ck_update", "frame=%d timer", uf);
    bool settings_open_before = jce_settings_is_open(app->engine_settings);
    jce_timer_tick(app->timer);
    float dt_ms = jce_timer_dt_ms(app->timer);
    float dt_sec = dt_ms / 1000.0f;
    app->scene_render_dt_sec = 0.0f;
    app->scene_render_dt_fresh = false;
    record_frametime(app, dt_ms);
    update_sysinfo(app);

    LOG_TRACE("ck_update", "frame=%d input", uf);
    handle_input(app, dt_ms);

    LOG_TRACE("ck_update", "frame=%d hud", uf);
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

    LOG_TRACE("ck_update", "frame=%d ui_process_input", uf);
    /* Feed input to RmlUI, compute layout, then render. */
    if (app->ui) {
        if (jce_settings_is_open(app->engine_settings))
            jce_ui_process_pointer_input(app->ui, app->svc.input);
        else
            jce_ui_process_input(app->ui, app->svc.input);
        LOG_TRACE("ck_update", "frame=%d ui_update", uf);
        jce_ui_update(app->ui, dt_sec);
        LOG_TRACE("ck_update", "frame=%d ui_update done", uf);
    }

    if (settings_open_before &&
        !jce_settings_is_open(app->engine_settings) &&
        app->debug_hud && app->engine_hud) {
        jce_debug_hud_show(app->engine_hud);
    }

    LOG_TRACE("ck_update", "frame=%d scene3d", uf);
    if (!jce_settings_is_open(app->engine_settings) && !app->paused) {
        update_3d_scene(app, dt_ms);
        app->scene_render_dt_sec = dt_sec;
        app->scene_render_dt_fresh = true;
    }

    LOG_TRACE("ck_update", "frame=%d done", uf);
    JCE_PROFILE_ZONE_END;
}

void ck_app_event(CkApp *app, const JceEvent *event)
{
    if (!app || !event) return;

    /* Honour OS-level quit signals (window X button, SDL_EVENT_QUIT,
     * SDL_EVENT_WINDOW_CLOSE_REQUESTED, Alt-F4).  Without this the
     * engine's should_quit callback returns false and the window
     * becomes unclosable. */
    if (event->type == JCE_EVENT_QUIT ||
        event->type == JCE_EVENT_WINDOW_CLOSE) {
        app->quit_requested = true;
    }
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
