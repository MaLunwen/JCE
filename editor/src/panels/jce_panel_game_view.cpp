/*
 * jce_panel_game_view.cpp  Game View panel (runtime viewport).
 * Extracted from jce_editor_panels.cpp.
 */

#include "core/jce_editor_config.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_run_manager.h"
#include "scene/jce_editor_game_render.h"

extern "C" {
#include <jce/os/core/jce_defs.h>
#include <jce/os/platform/jce_host_dialog.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/runtime/jce_game_module.h>
#include <jce/middleware/scene/jce_vcam_system.h>
}

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* Renderer backend list comes from jce_editor_panels.cpp via the
   editor's public accessor (populated lazily from
   jce_renderer_caps_list_backends()). */

/* ── Game View state ──────────────────────────────────────────────── */

static int  s_aspect_idx  = 0;
static bool s_show_stats  = false;
static int  s_run_mode_idx = 0;
static bool s_run_mode_loaded = false;

enum {
    JCE_GAME_VIEW_RUN_EDITOR_SIMULATION = 0,
    JCE_GAME_VIEW_RUN_EXTERNAL_GAME = 1,
};

static void ensure_run_mode_loaded(void)
{
    if (s_run_mode_loaded) return;

    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    s_run_mode_idx = cfg.run_mode == JCE_GAME_VIEW_RUN_EXTERNAL_GAME
                         ? JCE_GAME_VIEW_RUN_EXTERNAL_GAME
                         : JCE_GAME_VIEW_RUN_EDITOR_SIMULATION;
    s_run_mode_loaded = true;
}

static void persist_run_mode(void)
{
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    cfg.run_mode = s_run_mode_idx;
    jce_editor_config_save(&cfg);
}

static void start_external_game(void)
{
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);

    JceRunConfig run_cfg;
    memset(&run_cfg, 0, sizeof(run_cfg));
    snprintf(run_cfg.executable_path, sizeof(run_cfg.executable_path), "%s",
             cfg.game_executable_path);
    snprintf(run_cfg.working_directory, sizeof(run_cfg.working_directory), "%s",
             cfg.game_working_directory);
    run_cfg.capture_stdout = true;
    run_cfg.capture_stderr = true;

    jce_run_manager_start(&run_cfg);
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_game_view_content(void)
{
    ensure_run_mode_loaded();
    jce_run_manager_poll();

    /* Toolbar row */
    const char *aspects[] = { "Free", "16:9", "16:10", "4:3", "21:9", "1:1" };
    ImGui::PushItemWidth(80);
    ImGui::Combo("##aspect", &s_aspect_idx, aspects, 6);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    static int s_renderer_idx = 0;
    const char *const *renderer_names = nullptr;
    int renderer_count = jce_editor_renderer_backends(&renderer_names);
    ImGui::PushItemWidth(80);
    ImGui::Combo("##renderer", &s_renderer_idx, renderer_names, renderer_count);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###stats", jce_editor_i18n("game.stats"));
        ImGui::Checkbox(_lbl, &s_show_stats);
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    const char *run_modes[] = { "Editor Simulation", "External Game" };
    bool external_running = jce_run_manager_is_running();
    ImGui::PushItemWidth(150);
    if (external_running) ImGui::BeginDisabled();
    if (ImGui::Combo("##runMode", &s_run_mode_idx, run_modes, 2))
        persist_run_mode();
    if (external_running) ImGui::EndDisabled();
    ImGui::PopItemWidth();

    /* B6 — Game module dropdown (only for Editor Simulation; external
     * mode runs the standalone exe which embeds its own module). */
    if (s_run_mode_idx == JCE_GAME_VIEW_RUN_EDITOR_SIMULATION) {
        ImGui::SameLine();
        int mod_count = jce_game_module_count();
        const JceGameModule *current = jce_editor_game_render_get_module();
        int cur_idx = 0;
        for (int i = 0; i < mod_count; ++i) {
            if (jce_game_module_at(i) == current) { cur_idx = i; break; }
        }
        ImGui::PushItemWidth(180);
        if (ImGui::BeginCombo("##gameModule",
                              jce_game_module_name_at(cur_idx))) {
            for (int i = 0; i < mod_count; ++i) {
                const char *n = jce_game_module_name_at(i);
                if (!n) continue;
                bool sel = (i == cur_idx);
                if (ImGui::Selectable(n, sel))
                    jce_editor_game_render_set_module(jce_game_module_at(i));
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
    }

    ImGui::SameLine();

    if (s_run_mode_idx == JCE_GAME_VIEW_RUN_EXTERNAL_GAME) {
        JceRunStatus rs;
        jce_run_manager_get_status(&rs);
        if (!rs.running) {
            if (ImGui::SmallButton(">")) start_external_game();
            ImGui::SameLine();
            if (ImGui::SmallButton("...##pickExe")) {
                jce_host_dialog_pick_file(
                    jce_editor_i18n("gameView.selectGameExe"), nullptr,
#if JCE_PLATFORM_WINDOWS
                    "Executables (*.exe);;All Files (*.*)",
#else
                    "All Files (*)",
#endif
                    [](void *, JceDialogResult result, const char *path) {
                        if (result != JCE_DIALOG_OK || !path) return;
                        JceEditorConfig c;
                        jce_editor_config_load(&c);
                        snprintf(c.game_executable_path,
                                 sizeof(c.game_executable_path), "%s", path);
                        const char *slash = strrchr(path, '/');
                        const char *bslash = strrchr(path, '\\');
                        const char *sep = (slash && bslash) ? (slash > bslash ? slash : bslash)
                                                            : (slash ? slash : bslash);
                        if (sep) {
                            size_t n = (size_t) (sep - path);
                            if (n >= sizeof(c.game_working_directory))
                                n = sizeof(c.game_working_directory) - 1;
                            memcpy(c.game_working_directory, path, n);
                            c.game_working_directory[n] = '\0';
                        }
                        jce_editor_config_save(&c);
                    },
                    nullptr);
            }
            if (ImGui::IsItemHovered()) {
                JceEditorConfig c;
                jce_editor_config_load(&c);
                ImGui::SetTooltip("%s\n%s",
                                  jce_editor_i18n("gameView.gameExePath"),
                                  c.game_executable_path[0] ? c.game_executable_path
                                                            : jce_editor_i18n("gameView.notSet"));
            }
            if (rs.last_error[0]) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                                   rs.last_error);
            }
        } else {
            if (ImGui::SmallButton("[]")) jce_run_manager_request_stop();
            ImGui::SameLine();
            if (rs.state == JCE_RUN_STOPPING)
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", jce_editor_i18n("gameView.stoppingExternal"));
            else
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", jce_editor_i18n("gameView.externalRunning"));
        }
    } else {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_STOPPED) {
            if (ImGui::SmallButton(">"))  jce_state_play();
        } else {
            if (ImGui::SmallButton("||")) jce_state_pause();
            ImGui::SameLine();
            if (ImGui::SmallButton("[]")) jce_state_stop();
            if (ps == JCE_PLAY_PAUSED) {
                ImGui::SameLine();
                if (ImGui::SmallButton(">|"))
                    jce_state_step(1.0f / 60.0f);
            }
            ImGui::SameLine();
            if (ps == JCE_PLAY_PLAYING)
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", jce_editor_i18n("gameView.playing"));
            else
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", jce_editor_i18n("gameView.paused"));
        }
    }

    ImGui::Separator();

    /* ── Embedded game viewport ──────────────────────────────────── */

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 16.0f) avail.x = 16.0f;
    if (avail.y < 16.0f) avail.y = 16.0f;

    /* Apply optional aspect-ratio letterboxing. */
    static const float aspect_ratios[] = {
        0.0f,            /* Free  */
        16.0f / 9.0f,
        16.0f / 10.0f,
        4.0f / 3.0f,
        21.0f / 9.0f,
        1.0f,
    };
    ImVec2 view_size = avail;
    float ar = aspect_ratios[s_aspect_idx >= 0 && s_aspect_idx < 6 ? s_aspect_idx : 0];
    if (ar > 0.0f) {
        float by_w = avail.x / ar;
        if (by_w <= avail.y) {
            view_size = ImVec2(avail.x, by_w);
        } else {
            view_size = ImVec2(avail.y * ar, avail.y);
        }
    }

    ImVec2 region_start = ImGui::GetCursorScreenPos();
    if (ar > 0.0f) {
        float pad_x = (avail.x - view_size.x) * 0.5f;
        float pad_y = (avail.y - view_size.y) * 0.5f;
        ImGui::Dummy(ImVec2(pad_x, pad_y));
        ImGui::SameLine();
        region_start = ImGui::GetCursorScreenPos();
    }

    uint32_t vw = (uint32_t)view_size.x;
    uint32_t vh = (uint32_t)view_size.y;
    jce_editor_game_render_frame(vw, vh);

    uint16_t tex_idx = jce_editor_game_render_get_texture();
    if (tex_idx != UINT16_MAX) {
        ImTextureID tid = (ImTextureID)(uintptr_t)tex_idx;
        ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
        if (jce_renderer_origin_bottom_left()) {
            uv0 = ImVec2(0.0f, 1.0f);
            uv1 = ImVec2(1.0f, 0.0f);
        }
        ImGui::Image(tid, view_size, uv0, uv1);
    } else {
        ImGui::Dummy(view_size);
    }

    bool   hovered   = ImGui::IsItemHovered();
    ImVec2 image_min = region_start;
    ImVec2 image_max = ImVec2(region_start.x + view_size.x,
                              region_start.y + view_size.y);

    /* ── FPS fly-cam input (CryEngine-style) ──────────────────────
     *   Click on the viewport     → capture cursor (FPS lock)
     *   Hold LeftAlt              → temporarily release while held
     *   ESC                       → fully release capture
     *   Mouse motion (captured)   → look (yaw / pitch)
     *   WASD                      → move horizontal
     *   Space / LeftShift         → move up / down
     *   LeftCtrl                  → x5 boost
     */
    static bool s_user_wants_capture = false;

    JceCamera *cam = jce_editor_game_render_get_camera();
    JcePlayState play_state = jce_state_get_play_state();
    bool play_active = (play_state == JCE_PLAY_PLAYING ||
                        play_state == JCE_PLAY_PAUSED);

    /* Auto-capture the cursor the moment the user presses Play, so the
     * Game View behaves like an actual game window — no extra click
     * required.  Only on the rising edge so that releasing capture
     * (ALT / ESC) while play is still running stays released. */
    static JcePlayState s_prev_play_state = JCE_PLAY_STOPPED;
    if (play_state == JCE_PLAY_PLAYING &&
        s_prev_play_state != JCE_PLAY_PLAYING) {
        s_user_wants_capture = true;
        ImGui::SetWindowFocus();
    }
    /* Stop edge: when the user presses Stop, immediately release the
     * cursor (Unity-parity behavior).  Without this, s_user_wants_capture
     * stays true and the Game View keeps the cursor locked even though
     * play has ended, blocking the user from clicking other panels. */
    if (play_state == JCE_PLAY_STOPPED &&
        s_prev_play_state != JCE_PLAY_STOPPED) {
        s_user_wants_capture = false;
    }
    s_prev_play_state = play_state;

    if (cam) {
        const ImGuiIO &io = ImGui::GetIO();
        float dt = io.DeltaTime > 0.0f ? io.DeltaTime : (1.0f / 60.0f);

        bool alt_held =
            ImGui::IsKeyDown(ImGuiKey_LeftAlt) ||
            ImGui::IsKeyDown(ImGuiKey_RightAlt);

        /* Capture is available any time the Game View is hovered+clicked
         * (CryEngine-style). When Play is active and a CharacterController
         * exists, capture also drives the player; otherwise it's a pure
         * free-fly camera. */
        if (hovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !alt_held) {
            s_user_wants_capture = true;
            ImGui::SetWindowFocus();
        }
        if (s_user_wants_capture && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_user_wants_capture = false;
        }

        bool effective_capture =
            s_user_wants_capture && !alt_held;
        bool was_captured = jce_editor_game_render_is_mouse_captured();
        jce_editor_game_render_set_mouse_capture(effective_capture);

        if (effective_capture) {
            /* Use SDL relative-motion accumulator (xrel/yrel) instead of
             * ImGui::IO::MouseDelta — the latter is always zero in
             * relative-mouse-mode because the absolute cursor is pinned. */
            float dx = 0.0f, dy = 0.0f;
            jce_editor_game_render_consume_mouse_delta(&dx, &dy);
            const float sensitivity = 0.0025f;
            if (dx != 0.0f || dy != 0.0f) {
                jce_camera_rotate(cam,
                                   dx * sensitivity,
                                  -dy * sensitivity);
            }

            float speed_mult = 1.0f;
            if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                ImGui::IsKeyDown(ImGuiKey_RightCtrl))
                speed_mult = 5.0f;
            else if (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                     ImGui::IsKeyDown(ImGuiKey_RightShift))
                speed_mult = 0.4f;

            float px, py, pz;
            bool has_player =
                play_active &&
                jce_editor_play_get_player_position(&px, &py, &pz);

            if (has_player) {
                /* Drive the scene's CharacterController.  Walk direction
                 * is computed from the camera's current forward / right
                 * so look direction defines move direction (FPS feel). */
                jce_vec3 fwd   = jce_camera_get_forward(cam);
                jce_vec3 right = jce_camera_get_right(cam);
                fwd.y = 0.0f; right.y = 0.0f;
                float fl = sqrtf(fwd.x*fwd.x + fwd.z*fwd.z);
                float rl = sqrtf(right.x*right.x + right.z*right.z);
                if (fl > 0.0001f) { fwd.x   /= fl; fwd.z   /= fl; }
                if (rl > 0.0001f) { right.x /= rl; right.z /= rl; }

                float wx = 0.0f, wz = 0.0f;
                const float walk_speed = 4.0f;
                if (ImGui::IsKeyDown(ImGuiKey_W)) { wx += fwd.x;   wz += fwd.z;   }
                if (ImGui::IsKeyDown(ImGuiKey_S)) { wx -= fwd.x;   wz -= fwd.z;   }
                if (ImGui::IsKeyDown(ImGuiKey_D)) { wx += right.x; wz += right.z; }
                if (ImGui::IsKeyDown(ImGuiKey_A)) { wx -= right.x; wz -= right.z; }
                float wlen = sqrtf(wx*wx + wz*wz);
                if (wlen > 0.0001f) { wx /= wlen; wz /= wlen; }

                bool jump = ImGui::IsKeyPressed(ImGuiKey_Space, false);
                jce_editor_play_set_player_input(wx * walk_speed,
                                                  wz * walk_speed,
                                                  jump,
                                                  speed_mult);
            } else {
                /* No CharacterController in scene → classic free-fly cam. */
                float speed = 5.0f * speed_mult;
                float step  = speed * dt;
                if (ImGui::IsKeyDown(ImGuiKey_W)) jce_camera_move_forward(cam,  step);
                if (ImGui::IsKeyDown(ImGuiKey_S)) jce_camera_move_forward(cam, -step);
                if (ImGui::IsKeyDown(ImGuiKey_D)) jce_camera_move_right  (cam,  step);
                if (ImGui::IsKeyDown(ImGuiKey_A)) jce_camera_move_right  (cam, -step);
                if (ImGui::IsKeyDown(ImGuiKey_Space))
                    jce_camera_move_up(cam,  step);
                if (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                    ImGui::IsKeyDown(ImGuiKey_RightShift))
                    jce_camera_move_up(cam, -step);
            }

            ImGui::SetNextFrameWantCaptureKeyboard(true);
            ImGui::SetNextFrameWantCaptureMouse(true);
        }

        /* On release edge: re-park the cursor at the centre of the
         * viewport so the user finds it where they last looked, not
         * stuck at the edge of the screen where SDL parked it before
         * relative mode engaged. */
        if (was_captured && !effective_capture) {
            int cx = (int)((image_min.x + image_max.x) * 0.5f);
            int cy = (int)((image_min.y + image_max.y) * 0.5f);
            jce_editor_game_render_warp_cursor(cx, cy);
        }

        /* If there's a player character in Play mode, snap the game
         * view camera to its eye position every frame (regardless of
         * capture state) so the user always sees through the player. */
        if (play_active) {
            float px, py, pz;
            if (jce_editor_play_get_player_position(&px, &py, &pz)) {
                jce_vec3 eye = jce_v3(px, py + 1.6f, pz);
                jce_camera_set_position(cam, eye);
            }
        }

        /* Cinemachine-style VCam override: if any active VCam exists in
         * the scene, it wins over both player-snap and free-fly. Active
         * only in Play mode so designers can keep editing freely. */
        if (play_active) {
            JceScene *scene = jce_state_get_scene();
            if (scene) {
                JceVcamOutput vout;
                bool has_vcam = false;
                jce_vcam_system_evaluate(scene, dt, &vout, &has_vcam);
                if (has_vcam) {
                    jce_camera_set_position(cam,
                        jce_v3(vout.position[0], vout.position[1], vout.position[2]));
                    jce_camera_look_at(cam,
                        jce_v3(vout.target[0], vout.target[1], vout.target[2]));
                    jce_camera_set_fov(cam, vout.fov_deg);
                }
            }
        } else {
            /* Reset damping when not playing so the next Play snaps. */
            jce_vcam_system_reset();
        }
    }

    /* HUD overlay: capture state + hint. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (jce_editor_game_render_is_mouse_captured()) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(80, 220, 120, 230),
                    "[FPS] WASD move | Space up | Shift down | Ctrl x5  "
                    "(hold ALT to free cursor, ESC to exit)");
    } else if (s_user_wants_capture) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(255, 220, 120, 230),
                    "[ALT held - cursor free]  release ALT to re-capture");
    } else if (hovered) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(220, 220, 220, 200),
                    "Click to enter FPS fly-cam (WASD/Space/Shift, "
                    "Ctrl=boost, ALT=free, ESC=exit)");
    }

    if (s_show_stats) {
        const char *const *names = nullptr;
        int n = jce_editor_renderer_backends(&names);
        const char *current = (names && s_renderer_idx >= 0 && s_renderer_idx < n)
                                  ? names[s_renderer_idx] : "?";
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s | %.1f fps  %ux%u",
                 jce_editor_i18n("game.renderer"), current,
                 ImGui::GetIO().Framerate, vw, vh);
        dl->AddText(ImVec2(image_min.x + 6.0f, image_max.y - 18.0f),
                    IM_COL32(160, 255, 160, 220), buf);
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_game_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW);
    if (!*vis) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    char title[256];
    snprintf(title, sizeof(title), "%s###game_view", jce_editor_i18n("Game"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_game_view_content();
    ImGui::End();
    ImGui::PopStyleVar();
}
