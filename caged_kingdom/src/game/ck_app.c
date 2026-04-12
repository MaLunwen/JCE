/*
 * ck_app.c  Caged Kingdom demo application logic.
 *
 * All game/demo state and per-frame logic lives here.
 * main.c only calls create/destroy/update/event.
 *
 * This file uses ONLY JCE engine APIs — no direct SDL dependency.
 */

#include "ck_app.h"
#include <jce/api.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#define FT_HISTORY    120
#define FT_GRAPH_COLS  60
#define FT_GRAPH_ROWS   4

struct CkApp {
    JceServices   svc;            /* owned copy of subsystem pointers */
    JceSound      snd_bounce;
    JceSound      snd_music;
    JceVoice      music_voice;

    JceTexture    tex_demo;
    JceFont      *font_main;
    JceFont      *font_i18n;    /* JCE.ttf with CJK glyphs */

    JceTimer     *timer;

    /* 3D scene. */
    JceCamera    *camera;
    JceCameraController *cam_ctrl;
    JceMesh      *cube;
    JceMesh      *chalet;
    JceMesh      *ground;
    JceModel     *model_bagman;
    JceAnimPlayer *anim_player;
    jce_mat4      anim_bones[256];
    uint32_t      anim_num_joints;
    int           anim_clip_index;  /* index of currently playing clip */
    JceTexture    tex_cube;
    JceTexture    tex_ground;
    JceDirLight   sun;
    JceLightEnv  *light_env;
    float         cam_yaw;
    float         cube_angle;

    /* Asset handles for manager-loaded resources. */
    JceAssetHandle h_tex_demo;
    JceAssetHandle h_tex_cube;
    JceAssetHandle h_tex_ground;
    JceAssetHandle h_cube;
    JceAssetHandle h_chalet;

    bool          quit_requested;
    bool          paused;
    int           pause_selection;
    bool          debug_hud;
    bool          wireframe;
    bool          f3_was_down;
    bool          f3_combo_used;
    bool          mouse_captured;
    bool          touch_native;

    /* Frametime graph ring buffer. */
    float         ft_history[FT_HISTORY];
    int           ft_index;

    /* System info (updated once per second). */
    JceSysInfo    sysinfo;

    /* Touch HUD: auto-created on mobile, F9-toggled on desktop. */
    JceTouchHud  *touch_hud;
};

CkApp *ck_app_create(const JceServices *svc)
{
    CkApp *app = (CkApp *)calloc(1, sizeof(*app));
    if (!app) return NULL;
    app->svc = *svc;

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

    /* Load textures via asset manager. */
    if (app->svc.assets) {
        app->h_tex_demo = jce_asset_load(app->svc.assets,
                              "textures/texture.jpg", JCE_ASSET_TEXTURE);
        app->tex_demo = jce_asset_get_texture(app->svc.assets, app->h_tex_demo);

        app->h_tex_cube = jce_asset_load(app->svc.assets,
                              "textures/chalet.jpg", JCE_ASSET_TEXTURE);
        app->tex_cube = jce_asset_get_texture(app->svc.assets, app->h_tex_cube);

        JceAssetLoadParams wrap_params = JCE_ASSET_LOAD_DEFAULT;
        wrap_params.texture_sampler_mode = JCE_TEX_WRAP;
        wrap_params.sync = true;
        app->h_tex_ground = jce_asset_acquire(app->svc.assets,
                                "textures/texture.jpg", JCE_ASSET_TEXTURE,
                                &wrap_params);
        app->tex_ground = jce_asset_get_texture(app->svc.assets, app->h_tex_ground);
    } else {
        /* Fallback: direct loading if no asset manager. */
        app->tex_demo = jce_texture_load(app->svc.pak, "textures/texture.jpg");
        app->tex_cube = jce_texture_load(app->svc.pak, "textures/chalet.jpg");
        app->tex_ground = jce_texture_load_ex(app->svc.pak,
                              "textures/texture.jpg", JCE_TEX_WRAP);
    }

    /* Load demo font (32pt, must run on main thread). */
    app->font_main = jce_font_open(app->svc.pak, "fonts/Caveat.ttf", 32.0f);

    /* Load i18n translations from PAK, then build CJK font atlas. */
    jce_i18n_init(app->svc.pak);
    {
        uint32_t cps[256];
        int cp_count = jce_i18n_collect_codepoints(cps, 256);
        app->font_i18n = jce_font_open_ex(app->svc.pak, "fonts/JCE.ttf",
                                           28.0f, cps, cp_count);
    }

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
    app->ground = jce_mesh_create_plane_ex(10.0f, 10.0f, 10, 5.0f);
    app->sun = jce_dir_light_default();

    /* PBR light environment (matches sun direction). */
    app->light_env = jce_light_env_create();
    jce_light_env_set_ambient(app->light_env, jce_v3(1.0f, 1.0f, 1.0f), 0.15f);
    {
        JceDirLightDesc dl = {0};
        dl.direction = app->sun.direction;
        dl.color     = app->sun.color;
        dl.intensity = JCE_PI;  /* compensate for Lambertian /PI in Cook-Torrance BRDF */
        jce_light_env_add_dir_light(app->light_env, &dl);
    }

    /* Load PSX BagMan GLB model + start animation. */
    app->model_bagman = jce_model_load_gltf(app->svc.pak,
        "models/PSX_BagMan.glb");
    if (app->model_bagman) {
        JceSkeleton *skel = jce_model_get_skeleton(app->model_bagman);
        if (skel) {
            app->anim_player = jce_anim_player_create(skel);
            if (app->anim_player && jce_model_anim_count(app->model_bagman) > 0) {
                /* Prefer "Walk_loop" or "Idle_loop" for clearly visible
                   bone deformation; fall back to first non-zero-duration clip. */
                uint32_t num_clips = jce_model_anim_count(app->model_bagman);
                app->anim_clip_index = -1;
                /* Pass 1: look for Walk_loop or Idle_loop by name. */
                for (uint32_t ci = 0; ci < num_clips; ci++) {
                    JceAnimClip *c = jce_model_get_anim(app->model_bagman, ci);
                    const char *name = jce_anim_clip_name(c);
                    if (name && (strstr(name, "Walk") || strstr(name, "Idle"))) {
                        if (jce_anim_clip_duration(c) > 0.0f) {
                            app->anim_clip_index = (int)ci;
                            break;
                        }
                    }
                }
                /* Pass 2: fallback to first non-zero-duration. */
                if (app->anim_clip_index < 0) {
                    app->anim_clip_index = 0;
                    for (uint32_t ci = 0; ci < num_clips; ci++) {
                        JceAnimClip *c = jce_model_get_anim(app->model_bagman, ci);
                        if (jce_anim_clip_duration(c) > 0.0f) {
                            app->anim_clip_index = (int)ci;
                            break;
                        }
                    }
                }
                JceAnimClip *clip = jce_model_get_anim(
                    app->model_bagman, (uint32_t)app->anim_clip_index);
                jce_anim_player_play(app->anim_player, clip, true, 1.0f);
            }
        }
    }

    /* Set window icon from PAK. */
    {
        const PakAsset *icon = pak_find(app->svc.pak, "JCE_icon.png");
        if (icon) {
            void *buf = malloc((size_t)icon->original_size);
            if (buf) {
                size_t sz = pak_decompress(icon, buf,
                                           (size_t)icon->original_size);
                if (sz > 0)
                    jce_window_set_icon(app->svc.window, buf, sz);
                free(buf);
            }
        }
    }

    app->debug_hud = svc->config ? svc->config->debug_text : true;
    app->timer = jce_timer_create(0.0);

    jce_sysinfo_init(&app->sysinfo);

    /* Touch HUD: auto-create on native touch platforms. */
    {
        bool is_touch_platform = false;
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
        is_touch_platform = true;
#elif defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV)
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

    return app;
}

void ck_app_destroy(CkApp *app)
{
    if (!app) return;
    if (app->mouse_captured)
        jce_window_set_relative_mouse_mode(app->svc.window, false);
    jce_touch_hud_destroy(app->touch_hud);
    jce_timer_destroy(app->timer);
    jce_camctrl_destroy(app->cam_ctrl);
    jce_camera_destroy(app->camera);

    /* Release assets through the manager if available. */
    if (app->svc.assets) {
        /* Release manager-owned assets by handle. */
        jce_asset_release(app->svc.assets, app->h_tex_demo);
        jce_asset_release(app->svc.assets, app->h_tex_cube);
        jce_asset_release(app->svc.assets, app->h_tex_ground);

        /* Meshes: if asset load failed and we created a fallback, destroy it.
           Otherwise the asset manager owns the mesh data. */
        if (jce_asset_get_mesh(app->svc.assets, app->h_cube) == NULL)
            jce_mesh_destroy(app->cube);
        jce_asset_release(app->svc.assets, app->h_cube);

        jce_asset_release(app->svc.assets, app->h_chalet);
    } else {
        jce_mesh_destroy(app->cube);
        jce_mesh_destroy(app->chalet);
        jce_texture_destroy(app->tex_cube);
        jce_texture_destroy(app->tex_ground);
        jce_texture_destroy(app->tex_demo);
    }
    /* Ground is always procedural, not from asset manager. */
    jce_anim_player_destroy(app->anim_player);
    jce_model_destroy(app->model_bagman);
    jce_light_env_destroy(app->light_env);
    jce_mesh_destroy(app->ground);
    jce_font_close(app->font_i18n);
    jce_font_close(app->font_main);
    free(app);
}

/* Return the i18n font for non-English, main font otherwise. */
static JceFont *active_font(CkApp *app)
{
    if (jce_i18n_get_lang() != JCE_LANG_EN && app->font_i18n)
        return app->font_i18n;
    return app->font_main;
}

/* -- Debug HUD (MangoHud style) ------------------------------------- */

static void draw_frametime_graph(CkApp *app, uint16_t tx, uint16_t ty)
{
    const float max_ms = 50.0f;

    for (int row = 0; row < FT_GRAPH_ROWS; row++) {
        float threshold = max_ms * (float)(FT_GRAPH_ROWS - row)
                        / (float)FT_GRAPH_ROWS;

        for (int col = 0; col < FT_GRAPH_COLS; col++) {
            int idx = (app->ft_index - FT_GRAPH_COLS + col + FT_HISTORY)
                    % FT_HISTORY;
            float dt = app->ft_history[idx];

            uint8_t attr = 0x00; /* black = empty */
            if (dt > 0.0f && dt >= threshold) {
                if (dt <= 16.7f)       attr = 0x20; /* green bg */
                else if (dt <= 33.3f)  attr = 0xE0; /* yellow bg */
                else                   attr = 0xC0; /* red bg */
            }
            jce_renderer_dbg_text_attr(
                tx + (uint16_t)col,
                ty + (uint16_t)row, attr, " ");
        }
    }
}

static void draw_debug_hud(CkApp *app, float dt_ms)
{
    float fps_smoothed = jce_timer_fps(app->timer);

    uint32_t win_w, win_h;
    jce_window_get_size(app->svc.window, &win_w, &win_h);

    const char *gpu_name = jce_renderer_get_gpu_name(app->svc.renderer);
    bool vsync = jce_renderer_get_vsync(app->svc.renderer);
    const JceSysInfo *si = &app->sysinfo;

    /* FPS color: green >=55, yellow 30-55, red <30. */
    uint8_t fps_attr;
    if (fps_smoothed >= 55.0f)      fps_attr = 0x0a; /* green */
    else if (fps_smoothed >= 30.0f) fps_attr = 0x0e; /* yellow */
    else                                 fps_attr = 0x0c; /* red */

    /* MangoHud-style layout with colored labels. */
    uint16_t y = 1;

    /* GPU line: purple label + white value */
    jce_renderer_dbg_text(1, y,   0x05, " GPU ");
    jce_renderer_dbg_text(6, y++, 0x0f, " %s", gpu_name);

    /* CPU line: blue label + white value */
    jce_renderer_dbg_text(1, y,   0x09, " CPU ");
    jce_renderer_dbg_text(6, y++, 0x0f, " %d cores  %.0f%%",
                          si->cpu_cores, si->cpu_usage);

    /* RAM line: yellow label + white value */
    jce_renderer_dbg_text(1, y,   0x0e, " RAM ");
    jce_renderer_dbg_text(6, y++, 0x0f, " %d / %d MB",
                          si->ram_used_mb, si->ram_total_mb);

    /* Resolution + VSYNC */
    jce_renderer_dbg_text(1, y++, 0x07, " %ux%u  VSYNC: %s",
                          win_w, win_h, vsync ? "ON" : "OFF");

    /* FPS line: colored value */
    jce_renderer_dbg_text(1, y,   0x07, " FPS ");
    jce_renderer_dbg_text(6, y++, fps_attr, " %.1f", fps_smoothed);

    /* Frame time line */
    jce_renderer_dbg_text(1, y,   0x07, " Frame");
    jce_renderer_dbg_text(7, y++, 0x0f, " %.1f ms", dt_ms);

    /* Wireframe status. */
    if (app->wireframe)
        jce_renderer_dbg_text(1, y++, 0x0c, " WIREFRAME  (F3+V)");

    /* Language. */
    jce_renderer_dbg_text(1, y++, 0x07, " Lang: %s  (F3+L)",
                          jce_i18n_lang_name());

    /* Frametime graph label + text-based bar chart. */
    jce_renderer_dbg_text(1, y++, 0x07, " Frametime");
    draw_frametime_graph(app, 1, y);
}

static void draw_pause_menu(CkApp *app)
{
    if (!app->font_main) return;

    int lw, lh;
    jce_window_get_logical(app->svc.window, &lw, &lh);
    const JceRenderer *r = app->svc.renderer;
    JceFont *font  = active_font(app);
    int sel = app->pause_selection;
    const float shrink = 0.4f;
    float scale = 1.5f * shrink;
    float hint_scale = 1.0f * shrink;

    const uint32_t white = jce_rgba(255, 255, 255, 255);
    const uint32_t green = jce_rgba(100, 255, 100, 255);
    const uint32_t red   = jce_rgba(255, 100, 100, 255);
    const uint32_t gray  = jce_rgba(128, 128, 128, 255);

    float line_h = (float)jce_font_line_height(font) * scale;
    float tw, th;

    const char *s_paused  = jce_i18n_get(JCE_STR_PAUSED);
    const char *s_cont    = jce_i18n_get(JCE_STR_CONTINUE);
    const char *s_quit    = jce_i18n_get(JCE_STR_QUIT);
    const char *s_hint    = jce_i18n_get(JCE_STR_CONTROLS_HINT);

    /* "=== PAUSED ===" centred. */
    jce_text_measure(font, s_paused, &tw, &th);
    float cy = (float)lh * 0.35f;
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * scale) * 0.5f, cy, scale,
        s_paused, white);

    cy += line_h * 2.0f;

    /* "> Continue" */
    jce_text_measure(font, s_cont, &tw, &th);
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * scale) * 0.5f, cy, scale,
        s_cont, sel == 0 ? green : gray);

    cy += line_h * 1.2f;

    /* "> Quit" */
    jce_text_measure(font, s_quit, &tw, &th);
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * scale) * 0.5f, cy, scale,
        s_quit, sel == 1 ? red : gray);

    cy += line_h * 1.5f;

    /* Hint text. */
    jce_text_measure(font, s_hint, &tw, &th);
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * hint_scale) * 0.5f, cy, hint_scale,
        s_hint, gray);
}

/* -- Input handling ------------------------------------------------ */

static void record_frametime(CkApp *app, float dt_ms)
{
    app->ft_history[app->ft_index] = dt_ms;
    app->ft_index = (app->ft_index + 1) % FT_HISTORY;
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
        bool want_capture = !app->paused && !alt_held;
        if (want_capture != app->mouse_captured) {
            jce_window_set_relative_mouse_mode(
                app->svc.window,
                want_capture);
            app->mouse_captured = want_capture;
        }
    }

    /* ESC / Android Back / touch Pause => toggle pause. */
    if (jce_input_key_pressed(input, JCE_KEY_ESCAPE) ||
        jce_input_key_pressed(input, JCE_KEY_AC_BACK) ||
        jce_touch_hud_button(app->touch_hud,
                             JCE_TOUCH_BTN_PAUSE)) {
        app->paused = !app->paused;
        app->pause_selection = 0;
        jce_touch_hud_set_menu_mode(app->touch_hud,
                                    app->paused);
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
            app->f3_combo_used = true;
        }

        if (app->f3_was_down && !f3_down) {
            if (!app->f3_combo_used)
                app->debug_hud = !app->debug_hud;
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
            app->paused = false;
            jce_touch_hud_set_menu_mode(app->touch_hud,
                                        false);
        } else {
            app->quit_requested = true;
        }
    }

    /* Touch HUD menu buttons. */
    if (jce_touch_hud_button(app->touch_hud,
                             JCE_TOUCH_BTN_MENU_0)) {
        app->paused = false;
        jce_touch_hud_set_menu_mode(app->touch_hud, false);
    }
    if (jce_touch_hud_button(app->touch_hud,
                             JCE_TOUCH_BTN_MENU_1))
        app->quit_requested = true;

    draw_pause_menu(app);
    if (app->debug_hud)
        draw_debug_hud(app, dt_ms);
}

/* -- 3D scene ------------------------------------------------------ */

static void draw_mesh_lit(CkApp *app, const JceMesh *mesh,
                          JceTexture tex,
                          const jce_mat4 *model)
{
    jce_lighting_apply(app->svc.renderer, &app->sun);
    jce_renderer_bind_texture(app->svc.renderer, 0, tex);
    jce_renderer_set_transform(model->raw[0]);
    jce_mesh_submit(mesh, app->svc.renderer,
                    JCE_VIEW_MAIN_3D);
}

static void draw_3d_scene(CkApp *app, float dt_ms)
{
    float dt_sec = dt_ms / 1000.0f;
    app->cube_angle += dt_sec * 1.0f;

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

    /* Begin 3D frame. */
    jce_renderer_begin_frame_3d(app->svc.renderer,
        app->svc.window, app->camera, JCE_VIEW_MAIN_3D);

    JceShaderHandle mesh_sh =
        jce_renderer_get_program_mesh(app->svc.renderer);
    if (!jce_shader_valid(mesh_sh))
        return;

    /* Ground plane at Y=0. */
    {
        JceTexture gt = jce_texture_valid(app->tex_ground)
                      ? app->tex_ground : app->tex_cube;
        jce_mat4 model = jce_m4_identity();
        draw_mesh_lit(app, app->ground, gt, &model);
    }

    /* PSX BagMan character (PBR). */
    if (app->model_bagman) {
        /* Advance animation; cycle to the next clip when the current one ends. */
        if (app->anim_player) {
            uint32_t num_clips = jce_model_anim_count(app->model_bagman);
            if (num_clips > 0 && !jce_anim_player_is_playing(app->anim_player)) {
                /* Advance to next clip, skipping zero-duration rest-pose clips. */
                for (uint32_t tries = 0; tries < num_clips; tries++) {
                    app->anim_clip_index =
                        (app->anim_clip_index + 1) % (int)num_clips;
                    JceAnimClip *next = jce_model_get_anim(
                        app->model_bagman, (uint32_t)app->anim_clip_index);
                    if (jce_anim_clip_duration(next) > 0.0f) {
                        jce_anim_player_play(app->anim_player, next, true, 1.0f);
                        break;
                    }
                }
            }
            app->anim_num_joints = jce_anim_player_update(
                app->anim_player, dt_sec, app->anim_bones, 256);
        }

        /* Set PBR light uniforms (camera pos for specular). */
        jce_light_env_set_camera_pos(app->light_env,
            jce_camera_get_position(app->camera));
        jce_light_env_apply(app->light_env, app->svc.renderer);

        jce_mat4 model_t = jce_m4_translate(jce_v3(0.0f, 0.0f, -2.0f));

        jce_model_draw(app->model_bagman, app->svc.renderer,
                       JCE_VIEW_MAIN_3D, &model_t,
                       app->anim_num_joints > 0 ? app->anim_bones : NULL,
                       app->anim_num_joints);
    }
}

/* -- 2D overlay ---------------------------------------------------- */

static void draw_2d_overlay(CkApp *app)
{
    /* Touch HUD overlay (on mobile / F9-toggled). */
    jce_touch_hud_draw(app->touch_hud);
}

/* ================================================================== */

void ck_app_update(CkApp *app)
{
    JCE_PROFILE_ZONE_N("CkApp::Update");
    if (!app) { JCE_PROFILE_ZONE_END; return; }

    jce_timer_tick(app->timer);
    float dt_ms = jce_timer_dt_ms(app->timer);
    record_frametime(app, dt_ms);
    update_sysinfo(app);

    handle_input(app, dt_ms);

    if (app->paused) {
        update_pause(app, dt_ms);
        return;
    }

    if (app->debug_hud)
        draw_debug_hud(app, dt_ms);

    draw_3d_scene(app, dt_ms);
    draw_2d_overlay(app);
    JCE_PROFILE_ZONE_END;
}

void ck_app_event(CkApp *app, const void *event)
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
    /* Drawing is done inside ck_app_update for now. */
}

static void demo_on_event(const void *ev, void *ud)
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
        .name        = "JCE Demo",
        .init        = demo_init,
        .exit        = demo_exit,
        .update      = demo_update,
        .draw        = demo_draw,
        .on_event    = demo_on_event,
        .should_quit = demo_should_quit,
    };
}
