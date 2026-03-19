/*
 * jce_app.c  Rectangles demo application logic.
 *
 * All game/demo state and per-frame logic lives here.
 * main.c only calls create/destroy/update/event.
 */

#include "jce_app.h"

#include "platform/jce_window.h"
#include "platform/jce_input.h"
#include "platform/jce_touch_hud.h"
#include "audio/jce_audio.h"
#include "renderer/jce_renderer.h"
#include "renderer/jce_primitives.h"
#include "renderer/jce_texture.h"
#include "renderer/jce_text.h"
#include "renderer/jce_camera.h"
#include "renderer/jce_mesh.h"
#include "renderer/jce_material.h"
#include "renderer/jce_lighting.h"
#include "renderer/jce_views.h"
#include "resource/pak_loader.h"
#include "core/jce_config.h"
#include "core/jce_sysinfo.h"
#include "core/jce_timer.h"
#include "core/jce_math.h"

#include <bgfx/c99/bgfx.h>

#define FT_HISTORY    120
#define FT_GRAPH_COLS  60
#define FT_GRAPH_ROWS   4

struct JceApp {
    JceAppContext ctx;        /* owned copy of subsystem pointers */
    JceSound      snd_bounce;
    JceSound      snd_music;
    JceVoice      music_voice;

    JceTexture    tex_demo;      /* Phase 1.1 texture test */
    JceFont      *font_main;     /* Phase 1.2 text test   */

    JceTimer     *timer;         /* Phase 1.4 high-precision timer */

    /* Phase 2: 3D scene. */
    JceCamera    *camera;
    JceMesh      *cube;
    JceMesh      *chalet;
    JceMesh      *ground;
    JceTexture    tex_cube;
    JceTexture    tex_ground;
    JceDirLight   sun;
    float         cam_yaw;
    float         cube_angle;    /* auto-rotate */

    bool          quit_requested;
    bool          paused;
    int           pause_selection; /* 0 = Continue, 1 = Quit */
    bool          debug_hud;
    bool          mouse_captured;  /* SDL relative mouse mode active   */
    bool          sprinting;       /* CTRL toggle: 2x move speed      */
    bool          touch_native;    /* true if touch HUD was auto-created (mobile) */
    float         fov_current;     /* smoothed FOV (degrees)           */
    float         fov_target;      /* target FOV (60 normal, 70 sprint)*/

    uint64_t      last_ticks;
    float         fps_smoothed;

    /* Frametime graph ring buffer. */
    float         ft_history[FT_HISTORY];
    int           ft_index;

    /* System info (updated once per second). */
    JceSysInfo    sysinfo;
    double        sysinfo_last_update;

    /* Touch HUD: auto-created on mobile, F9-toggled on desktop. */
    JceTouchHud  *touch_hud;
};

JceApp *jce_app_create(const JceAppContext *ctx)
{
    JceApp *app = (JceApp *)SDL_calloc(1, sizeof(*app));
    if (!app) return NULL;
    app->ctx = *ctx;

    const JcePreloadedAssets *pre = &ctx->preloaded;

    /* Load sounds (use pre-loaded if available). */
    if (app->ctx.audio) {
        app->snd_bounce = (pre->has_preloaded && pre->snd_bounce)
            ? pre->snd_bounce
            : jce_audio_load(app->ctx.audio, app->ctx.pak, "sounds/bounce.wav");
        app->snd_music = (pre->has_preloaded && pre->snd_music)
            ? pre->snd_music
            : jce_audio_load(app->ctx.audio, app->ctx.pak,
                             "sounds/Aria Math - C418.ogg");
        if (app->snd_music != JCE_SOUND_INVALID) {
            float vol = app->ctx.config ? app->ctx.config->music_volume : 0.8f;
            app->music_voice = jce_audio_play(app->ctx.audio, app->snd_music,
                                              true, vol, 1.0f);
        }
    }

    /* Load textures (use pre-loaded if available). */
    app->tex_demo = (pre->has_preloaded && jce_texture_valid(pre->tex_demo))
        ? pre->tex_demo
        : jce_texture_load(app->ctx.pak, "textures/texture.jpg");

    /* Load demo font (32pt)  must run on main thread (SDL_ttf). */
    app->font_main = jce_font_open(app->ctx.pak, "fonts/Caveat.ttf", 32.0f);

    /* 3D scene setup. */
    {
        JceCameraDesc cam_desc = {0};
        cam_desc.mode     = JCE_CAMERA_PERSPECTIVE;
        cam_desc.position = jce_v3(0.0f, 2.0f, 5.0f);
        cam_desc.target   = jce_v3(0.0f, 0.5f, 0.0f);
        cam_desc.fov_deg  = 60.0f;
        cam_desc.near_plane = 0.1f;
        cam_desc.far_plane  = 100.0f;
        app->camera = jce_camera_create(&cam_desc);
    }
    app->cube   = jce_mesh_load(app->ctx.pak, "models/chalet.obj");
    if (!app->cube)
        app->cube = jce_mesh_create_cube(1.0f);  /* fallback */
    app->chalet = jce_mesh_load(app->ctx.pak, "models/sachiel_fab_v2.obj");
    app->ground = jce_mesh_create_plane_ex(10.0f, 10.0f, 10, 5.0f);
    app->tex_cube = (pre->has_preloaded && jce_texture_valid(pre->tex_cube))
        ? pre->tex_cube
        : jce_texture_load(app->ctx.pak, "textures/chalet.jpg");
    app->tex_ground = (pre->has_preloaded && jce_texture_valid(pre->tex_ground))
        ? pre->tex_ground
        : jce_texture_load_ex(app->ctx.pak, "textures/texture.jpg",
                               JCE_TEX_WRAP);
    app->sun = jce_dir_light_default();

    /* Set window icon from PAK. */
    {
        const PakAsset *icon = pak_find(app->ctx.pak, "JCE_icon.png");
        if (icon) {
            void *buf = SDL_malloc((size_t)icon->original_size);
            if (buf) {
                size_t sz = pak_decompress(icon, buf,
                                           (size_t)icon->original_size);
                if (sz > 0)
                    jce_window_set_icon(app->ctx.window, buf, sz);
                SDL_free(buf);
            }
        }
    }

    app->debug_hud = ctx->config ? ctx->config->debug_text : true;
    app->timer     = jce_timer_create(0.0); /* variable timestep for now */

    jce_sysinfo_init(&app->sysinfo);
    app->sysinfo_last_update = 0.0;

    /* Touch HUD: auto-create on native touch platforms only.
       On desktop, F9 toggles it for debugging. */
    {
        bool is_touch_platform = false;
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
        is_touch_platform = true;
#elif defined(__APPLE__)
        {
            int touch_count = 0;
            SDL_TouchID *devs = SDL_GetTouchDevices(&touch_count);
            SDL_free(devs);
            is_touch_platform = (touch_count > 0);
        }
#endif
        if (is_touch_platform) {
            app->touch_hud    = jce_touch_hud_create(ctx->renderer,
                                    ctx->window, app->font_main);
            app->touch_native = true;
        }
    }

    app->fov_current = 60.0f;
    app->fov_target  = 60.0f;

    /* Desktop (no touch HUD): capture mouse for FPS-style look. */
    if (!app->touch_hud) {
        SDL_SetWindowRelativeMouseMode(jce_window_sdl(app->ctx.window), true);
        app->mouse_captured = true;
    }

    return app;
}

void jce_app_destroy(JceApp *app)
{
    if (!app) return;
    if (app->mouse_captured)
        SDL_SetWindowRelativeMouseMode(jce_window_sdl(app->ctx.window), false);
    jce_touch_hud_destroy(app->touch_hud);
    jce_timer_destroy(app->timer);
    jce_camera_destroy(app->camera);
    jce_mesh_destroy(app->cube);
    jce_mesh_destroy(app->chalet);
    jce_mesh_destroy(app->ground);
    jce_texture_destroy(app->tex_cube);
    jce_texture_destroy(app->tex_ground);
    jce_font_close(app->font_main);
    jce_texture_destroy(app->tex_demo);
    SDL_free(app);
}

/* -- Debug HUD (MangoHud style) ------------------------------------- */

static void draw_frametime_graph(JceApp *app, uint16_t tx, uint16_t ty)
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
            bgfx_dbg_text_printf(tx + (uint16_t)col,
                                 ty + (uint16_t)row, attr, " ");
        }
    }
}

static void draw_debug_hud(JceApp *app, float dt_ms)
{
    /* Smooth FPS with exponential moving average. */
    if (dt_ms > 0.0f) {
        float instant_fps = 1000.0f / dt_ms;
        app->fps_smoothed = app->fps_smoothed * 0.95f + instant_fps * 0.05f;
    }

    uint32_t win_w, win_h;
    jce_window_get_size(app->ctx.window, &win_w, &win_h);

    const char *gpu_name = jce_renderer_get_gpu_name(app->ctx.renderer);
    bool vsync = jce_renderer_get_vsync(app->ctx.renderer);
    const JceSysInfo *si = &app->sysinfo;

    /* FPS color: green >=55, yellow 30-55, red <30. */
    uint8_t fps_attr;
    if (app->fps_smoothed >= 55.0f)      fps_attr = 0x0a; /* green */
    else if (app->fps_smoothed >= 30.0f) fps_attr = 0x0e; /* yellow */
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
    jce_renderer_dbg_text(6, y++, fps_attr, " %.1f", app->fps_smoothed);

    /* Frame time line */
    jce_renderer_dbg_text(1, y,   0x07, " Frame");
    jce_renderer_dbg_text(7, y++, 0x0f, " %.1f ms", dt_ms);

    /* Frametime graph label + text-based bar chart. */
    jce_renderer_dbg_text(1, y++, 0x07, " Frametime");
    draw_frametime_graph(app, 1, y);
    y += FT_GRAPH_ROWS;
}

static void draw_pause_menu(JceApp *app)
{
    if (!app->font_main) return;

    int lw, lh;
    jce_window_get_logical(app->ctx.window, &lw, &lh);
    JceRenderer *r = app->ctx.renderer;
    JceFont *font  = app->font_main;
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

    /* "=== PAUSED ===" centred. */
    jce_text_measure(font, "=== PAUSED ===", &tw, &th);
    float cy = (float)lh * 0.35f;
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * scale) * 0.5f, cy, scale,
        "=== PAUSED ===", white);

    cy += line_h * 2.0f;

    /* "> Continue" */
    jce_text_measure(font, "> Continue", &tw, &th);
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * scale) * 0.5f, cy, scale,
        "> Continue", sel == 0 ? green : gray);

    cy += line_h * 1.2f;

    /* "> Quit" */
    jce_text_measure(font, "> Quit", &tw, &th);
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * scale) * 0.5f, cy, scale,
        "> Quit", sel == 1 ? red : gray);

    cy += line_h * 1.5f;

    /* Hint text. */
    jce_text_measure(font, "Up/Down + Enter", &tw, &th);
    jce_text_draw_scaled(r, font,
        ((float)lw - tw * hint_scale) * 0.5f, cy, hint_scale,
        "Up/Down + Enter", gray);
}

void jce_app_update(JceApp *app)
{
    if (!app) return;

    /* Frame timing. */
    uint64_t now = SDL_GetTicks();
    float dt_ms = (float)(now - app->last_ticks);
    app->last_ticks = now;

    /* Record frametime in ring buffer. */
    app->ft_history[app->ft_index] = dt_ms;
    app->ft_index = (app->ft_index + 1) % FT_HISTORY;

    /* Update system info once per second. */
    if (now - (uint64_t)app->sysinfo_last_update >= 1000) {
        jce_sysinfo_update(&app->sysinfo);
        app->sysinfo_last_update = (double)now;
    }

    /* -- Input handling -------------------------------------------- */

    const JceInput *input = app->ctx.input;

    /* Update touch HUD (processes finger zones). */
    jce_touch_hud_update(app->touch_hud, input, dt_ms);

    /* Desktop mouse capture: ALT to release, auto-release when paused.
       When touch HUD is active (F9 debug), mouse stays free. */
    if (!app->touch_hud && !app->touch_native) {
        bool alt_held = jce_input_key_down(input, SDL_SCANCODE_LALT) ||
                        jce_input_key_down(input, SDL_SCANCODE_RALT);
        bool want_capture = !app->paused && !alt_held;
        if (want_capture != app->mouse_captured) {
            SDL_SetWindowRelativeMouseMode(
                jce_window_sdl(app->ctx.window), want_capture);
            app->mouse_captured = want_capture;
        }
    }

    /* ESC / Android Back / touch Pause  toggle pause menu. */
    if (jce_input_key_pressed(input, SDL_SCANCODE_ESCAPE) ||
        jce_input_key_pressed(input, SDL_SCANCODE_AC_BACK) ||
        jce_touch_hud_button(app->touch_hud, JCE_TOUCH_BTN_PAUSE)) {
        app->paused = !app->paused;
        app->pause_selection = 0;
        jce_touch_hud_set_menu_mode(app->touch_hud, app->paused);
    }

    /* F11  toggle fullscreen (works even while paused). */
    if (jce_input_key_pressed(input, SDL_SCANCODE_F11))
        jce_window_toggle_fullscreen(app->ctx.window);

    /* F3  toggle debug HUD (works even while paused). */
    if (jce_input_key_pressed(input, SDL_SCANCODE_F3))
        app->debug_hud = !app->debug_hud;

    /* F9  toggle touch HUD on desktop (for debugging mobile controls).
       On native touch platforms this is a no-op (always shown). */
    if (jce_input_key_pressed(input, SDL_SCANCODE_F9) && !app->touch_native) {
        if (app->touch_hud) {
            /* Disable touch HUD, re-enable mouse look. */
            jce_touch_hud_destroy(app->touch_hud);
            app->touch_hud = NULL;
            SDL_SetWindowRelativeMouseMode(
                jce_window_sdl(app->ctx.window), true);
            app->mouse_captured = true;
        } else {
            /* Enable touch HUD, disable mouse look. */
            app->touch_hud = jce_touch_hud_create(
                app->ctx.renderer, app->ctx.window, app->font_main);
            if (app->mouse_captured) {
                SDL_SetWindowRelativeMouseMode(
                    jce_window_sdl(app->ctx.window), false);
                app->mouse_captured = false;
            }
        }
    }

    /* -- Pause menu ------------------------------------------------ */

    if (app->paused) {
        if (jce_input_key_pressed(input, SDL_SCANCODE_UP) ||
            jce_input_key_pressed(input, SDL_SCANCODE_W))
            app->pause_selection = 0;
        if (jce_input_key_pressed(input, SDL_SCANCODE_DOWN) ||
            jce_input_key_pressed(input, SDL_SCANCODE_S))
            app->pause_selection = 1;
        if (jce_input_key_pressed(input, SDL_SCANCODE_RETURN)) {
            if (app->pause_selection == 0) {
                app->paused = false;       /* Continue */
                jce_touch_hud_set_menu_mode(app->touch_hud, false);
            } else {
                app->quit_requested = true; /* Quit */
            }
        }

        /* Touch HUD menu buttons (unified: works on mobile + F9 desktop). */
        if (jce_touch_hud_button(app->touch_hud, JCE_TOUCH_BTN_MENU_0)) {
            app->paused = false;       /* Continue */
            jce_touch_hud_set_menu_mode(app->touch_hud, false);
        }
        if (jce_touch_hud_button(app->touch_hud, JCE_TOUCH_BTN_MENU_1))
            app->quit_requested = true; /* Quit */

        draw_pause_menu(app);
        if (app->debug_hud)
            draw_debug_hud(app, dt_ms);
        return; /* Skip game logic while paused. */
    }

    /* -- Debug HUD ------------------------------------------------- */

    if (app->debug_hud)
        draw_debug_hud(app, dt_ms);

    /* -- 3D scene (Phase 2 demo) ---------------------------------- */

    {
        float dt_sec = dt_ms / 1000.0f;

        /* Auto-rotate cube. */
        app->cube_angle += dt_sec * 1.0f;

        /* CTRL toggle: sprint (2x speed). */
        if (jce_input_key_pressed(app->ctx.input, SDL_SCANCODE_LCTRL) ||
            jce_input_key_pressed(app->ctx.input, SDL_SCANCODE_RCTRL))
            app->sprinting = !app->sprinting;

        /* WASD + touch joystick: camera movement. */
        float base_speed = 3.0f * dt_sec;
        float move_speed = app->sprinting ? base_speed * 2.0f : base_speed;
        float kb_fwd = 0, kb_right = 0;
        if (jce_input_key_down(app->ctx.input, SDL_SCANCODE_W)) kb_fwd  -= 1;
        if (jce_input_key_down(app->ctx.input, SDL_SCANCODE_S)) kb_fwd  += 1;
        if (jce_input_key_down(app->ctx.input, SDL_SCANCODE_A)) kb_right -= 1;
        if (jce_input_key_down(app->ctx.input, SDL_SCANCODE_D)) kb_right += 1;

        float touch_dx = 0, touch_dz = 0;
        jce_touch_hud_get_move(app->touch_hud, &touch_dx, &touch_dz);

        float move_fwd   = kb_fwd   + touch_dz;  /* touch Y = forward/back */
        float move_right = kb_right + touch_dx;   /* touch X = strafe       */

        /* Minecraft: sprint cancels when not moving forward.
           move_fwd < 0 = forward (W subtracts 1), so >= 0 = stopped/backward. */
        if (app->sprinting && move_fwd >= 0)
            app->sprinting = false;

        if (move_fwd   != 0) jce_camera_move_forward(app->camera, -move_fwd * move_speed);
        if (move_right != 0) jce_camera_move_right(app->camera, move_right * move_speed);

        /* Space = ascend, Shift = descend. */
        if (jce_input_key_down(app->ctx.input, SDL_SCANCODE_SPACE))
            jce_camera_move_up(app->camera, move_speed);
        if (jce_input_key_down(app->ctx.input, SDL_SCANCODE_LSHIFT) ||
            jce_input_key_down(app->ctx.input, SDL_SCANCODE_RSHIFT))
            jce_camera_move_up(app->camera, -move_speed);

        /* Touch buttons: vertical movement (mobile). */
        if (jce_touch_hud_button_down(app->touch_hud, JCE_TOUCH_BTN_JUMP))
            jce_camera_move_up(app->camera, move_speed);
        if (jce_touch_hud_button_down(app->touch_hud, JCE_TOUCH_BTN_CROUCH))
            jce_camera_move_up(app->camera, -move_speed);

        /* Mouse look (desktop, when captured) + touch look. */
        float kb_yaw = 0, kb_pitch = 0;
        if (app->mouse_captured) {
            float mdx = 0, mdy = 0;
            jce_input_mouse_delta(app->ctx.input, &mdx, &mdy);
            float sensitivity = 0.002f;  /* rad/pixel */
            kb_yaw   += mdx * sensitivity;
            kb_pitch -= mdy * sensitivity;
        }

        float touch_yaw = 0, touch_pitch = 0;
        jce_touch_hud_get_look(app->touch_hud, &touch_yaw, &touch_pitch);
        /* Touch look returns degrees; camera_rotate takes radians. */
        float deg2rad = 3.14159265f / 180.0f;
        float total_yaw   = kb_yaw   + touch_yaw   * deg2rad;
        float total_pitch = kb_pitch + touch_pitch * deg2rad;
        if (total_yaw != 0 || total_pitch != 0)
            jce_camera_rotate(app->camera, total_yaw, total_pitch);

        /* FOV sprint effect: smooth 60° ↔ 70° transition. */
        app->fov_target = app->sprinting ? 70.0f : 60.0f;
        app->fov_current += (app->fov_target - app->fov_current)
                          * fminf(dt_sec * 8.0f, 1.0f);
        jce_camera_set_fov(app->camera, app->fov_current);

        /* Set 3D camera on view 0. */
        jce_renderer_begin_frame_3d(app->ctx.renderer, app->ctx.window,
                                     app->camera, JCE_VIEW_MAIN_3D);

        bgfx_program_handle_t mesh_prog =
            jce_renderer_get_program_mesh(app->ctx.renderer);

        if (mesh_prog.idx != UINT16_MAX) {
            /* Apply lighting. */
            jce_lighting_apply(app->ctx.renderer, &app->sun);

            /* Bind cube texture. */
            if (jce_texture_valid(app->tex_cube)) {
                bgfx_texture_handle_t th = { app->tex_cube.idx };
                bgfx_set_texture(0,
                    jce_renderer_get_tex_uniform(app->ctx.renderer),
                    th, UINT32_MAX);
            }

            /* Draw rotating cube at Y=0.5. */
            {
                jce_mat4 rot = jce_q_to_mat4(
                    jce_q_from_axis_angle(jce_v3(0, 1, 0), app->cube_angle));
                jce_mat4 trans = jce_m4_translate(jce_v3(0, 0.5f, 0));
                jce_mat4 model = jce_m4_multiply(&trans, &rot);
                bgfx_set_transform(model.m, 1);
                jce_mesh_submit(app->cube, app->ctx.renderer,
                                JCE_VIEW_MAIN_3D);
            }

            /* Draw chalet offset to the right. */
            if (app->chalet) {
                jce_lighting_apply(app->ctx.renderer, &app->sun);

                if (jce_texture_valid(app->tex_cube)) {
                    bgfx_texture_handle_t th = { app->tex_cube.idx };
                    bgfx_set_texture(0,
                        jce_renderer_get_tex_uniform(app->ctx.renderer),
                        th, UINT32_MAX);
                }

                jce_mat4 model = jce_m4_translate(jce_v3(3.0f, 0.5f, 0.0f));
                bgfx_set_transform(model.m, 1);
                jce_mesh_submit(app->chalet, app->ctx.renderer,
                                JCE_VIEW_MAIN_3D);
            }

            /* Re-apply lighting for ground (uniforms consumed per-draw). */
            jce_lighting_apply(app->ctx.renderer, &app->sun);

            /* Bind ground texture (falls back to cube texture). */
            {
                JceTexture gt = jce_texture_valid(app->tex_ground)
                              ? app->tex_ground : app->tex_cube;
                if (jce_texture_valid(gt)) {
                    bgfx_texture_handle_t th = { gt.idx };
                    bgfx_set_texture(0,
                        jce_renderer_get_tex_uniform(app->ctx.renderer),
                        th, UINT32_MAX);
                }
            }

            /* Draw ground plane at Y=0. */
            {
                jce_mat4 model = jce_m4_identity();
                bgfx_set_transform(model.m, 1);
                jce_mesh_submit(app->ground, app->ctx.renderer,
                                JCE_VIEW_MAIN_3D);
            }
        }
    }

    /* -- Rectangles animation (ported from SDL3 example) ----------- */

    int logical_w, logical_h;
    jce_window_get_logical(app->ctx.window, &logical_w, &logical_h);

    const float direction = ((now % 2000) >= 1000) ? 1.0f : -1.0f;
    const float scale = ((float)((int)(now % 1000) - 500) / 500.0f)
                        * direction;

    const uint32_t red   = jce_rgba(255,   0,   0, 255);
    const uint32_t green = jce_rgba(  0, 255,   0, 255);
    const uint32_t blue  = jce_rgba(  0,   0, 255, 255);
    const uint32_t white = jce_rgba(255, 255, 255, 255);

    /* 1) Single red outlined rectangle. */
    {
        float s = 100.0f + 100.0f * scale;
        jce_draw_rect_outline(app->ctx.renderer, 100, 100, s, s, red);
    }

    /* 2) Three green outlined rectangles (centered). */
    {
        float rects[3 * 4];
        for (int i = 0; i < 3; i++) {
            float s = (float)(i + 1) * 50.0f;
            float sz = s + s * scale;
            rects[i*4+0] = ((float)logical_w - sz) / 2.0f;
            rects[i*4+1] = ((float)logical_h - sz) / 2.0f;
            rects[i*4+2] = sz;
            rects[i*4+3] = sz;
        }
        jce_draw_rect_outlines(app->ctx.renderer, rects, 3, green);
    }

    /* 3) One blue filled rectangle. */
    {
        float w = 100.0f + 100.0f * scale;
        float h =  50.0f +  50.0f * scale;
        jce_draw_filled_rect(app->ctx.renderer, 400, 50, w, h, blue);
    }

    /* 4) 16 white filled rectangles along the bottom. */
    {
        float rects[16 * 4];
        const float bar_w = (float)logical_w / 16.0f;
        for (int i = 0; i < 16; i++) {
            float h = (float)(i + 1) * 8.0f;
            rects[i*4+0] = (float)i * bar_w;
            rects[i*4+1] = (float)logical_h - h;
            rects[i*4+2] = bar_w;
            rects[i*4+3] = h;
        }
        jce_draw_filled_rects(app->ctx.renderer, rects, 16, white);
    }

    /* 5) Textured rectangle demo (Phase 1.1). */
    if (jce_texture_valid(app->tex_demo)) {
        uint32_t tw, th;
        jce_texture_get_size(app->tex_demo, &tw, &th);
        float draw_w = (float)tw * 0.5f;
        float draw_h = (float)th * 0.5f;
        float tx = (float)logical_w - draw_w - 20.0f;
        float ty = 20.0f;
        jce_draw_textured_rect(app->ctx.renderer, tx, ty, draw_w, draw_h,
                               app->tex_demo, jce_rgba(255, 255, 255, 255),
                               NULL);
    }

    /* 6) Text rendering demo (Phase 1.2). */
    if (app->font_main) {
        const uint32_t yellow = jce_rgba(255, 220, 50, 255);
        jce_text_draw(app->ctx.renderer, app->font_main,
                      20.0f, (float)logical_h - 60.0f,
                      "JCE - Text Rendering!", yellow);
    }

    /* 7) Touch HUD overlay (F9 on desktop, always on mobile). */
    jce_touch_hud_draw(app->touch_hud);
}

void jce_app_event(JceApp *app, const SDL_Event *event)
{
    (void)app; (void)event;
}

bool jce_app_should_quit(const JceApp *app)
{
    return app ? app->quit_requested : false;
}
