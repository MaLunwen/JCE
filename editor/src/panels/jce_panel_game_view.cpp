/*
 * jce_panel_game_view.cpp  Game View panel (runtime viewport).
 * Extracted from jce_editor_panels.cpp.
 */

#include "core/jce_editor_config.h"
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_kpi_game_capture.h"
#include "core/jce_editor_project_state.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "core/jce_editor_state.h"
#include "core/jce_run_manager.h"
#include "dialogs/jce_editor_dialogs.h"
#include "scene/jce_editor_game_render.h"

extern "C" {
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/runtime/jce_game_module.h>
#include <jce/middleware/scene/jce_vcam_system.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/os/platform/jce_keys.h>
#include <jce/os/platform/jce_window_event.h>  /* JCE_KMOD_* */
#include <ctype.h>
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
static bool s_third_person = false;   /* play camera: false=first-person, true=behind-player */
/* Whether the user has clicked into the Game View to drive its camera (FPS
 * fly-cam / WASD free-fly / player-controller).  Persists across frames and
 * stays true even while LeftAlt momentarily frees the cursor (ALT-free-look),
 * so it — not the transient mouse-capture flag — is the authoritative "the
 * Game View owns keyboard input right now" signal.  The editor shortcut gate
 * (jce_editor_layout.cpp) reads it via jce_editor_game_view_is_input_active()
 * so in-game keys (WASD, Ctrl+S, …) never leak into editor commands during
 * Play.  Set on click-in, cleared on ESC / Stop. */
static bool s_user_wants_capture = false;
static bool s_runtime_pointer_camera = false;
static const float kTpBoomLen = 4.5f; /* third-person orbit distance */
static float s_tp_dist = 4.5f;        /* smoothed boom length (collision-shortened) */
static char s_pending_game_exe_path[512] = {0};
static bool s_pending_game_exe_ready = false;

static bool game_view_has_active_vcam(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) return false;

    const int count = jce_state_get_entity_count();
    for (int i = 0; i < count; ++i) {
        const uint32_t id = jce_state_get_entity_id_by_index(i);
        if (!jce_state_entity_enabled(id)) continue;
        const JceEntity entity = jce_state_to_ecs_entity(id);
        if (!entity || !jce_scene_has_virtual_camera(scene, entity)) continue;
        const JceVirtualCameraComponent *vc =
            jce_scene_get_virtual_camera(scene, entity);
        if (vc && vc->active) return true;
    }
    return false;
}

enum {
    JCE_GAME_VIEW_RUN_EDITOR_SIMULATION = 0,
    JCE_GAME_VIEW_RUN_EXTERNAL_GAME = 1,
};

static uint64_t s_run_mode_cfg_gen = 0;

static void ensure_run_mode_loaded(void)
{
    /* Re-read whenever the config generation changes: other writers (the
     * Preferences panel, project profile loads) can rewrite run_mode after
     * startup, and a once-per-session cache would serve the stale value for
     * the rest of the run. */
    uint64_t gen = jce_editor_config_generation();
    if (s_run_mode_loaded && gen == s_run_mode_cfg_gen) return;

    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);
    s_run_mode_idx = cfg.run_mode == JCE_GAME_VIEW_RUN_EXTERNAL_GAME
                         ? JCE_GAME_VIEW_RUN_EXTERNAL_GAME
                         : JCE_GAME_VIEW_RUN_EDITOR_SIMULATION;
    s_run_mode_loaded = true;
    s_run_mode_cfg_gen = gen;
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

static void apply_pending_game_exe_pick(void)
{
    if (!s_pending_game_exe_ready) {
        return;
    }

    s_pending_game_exe_ready = false;
    if (s_pending_game_exe_path[0] == '\0')
        return;

    JceEditorConfig c;
    jce_editor_config_load(&c);
    snprintf(c.game_executable_path,
             sizeof(c.game_executable_path), "%s",
             s_pending_game_exe_path);
    jce_path_parent(c.game_working_directory,
                    sizeof(c.game_working_directory),
                    s_pending_game_exe_path);
    jce_editor_config_save(&c);
    s_pending_game_exe_path[0] = '\0';
}

/* Encode one Unicode codepoint as UTF-8 into `out` (>= 5 bytes); returns the
 * byte count (0 for control chars we drop). */
static int encode_utf8(unsigned cp, char out[5])
{
    if (cp < 0x20u || cp == 0x7Fu) { out[0] = '\0'; return 0; } /* control */
    if (cp < 0x80u) {
        out[0] = (char)cp; out[1] = '\0'; return 1;
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        out[2] = '\0'; return 2;
    } else if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        out[3] = '\0'; return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    out[4] = '\0'; return 4;
}

/* Forward this frame's ImGui character queue + editing keys into the focused
 * ECS-UI InputField.  ImGui's IO is the Game View panel's natural event source
 * (the same place the pointer is sourced from), so this is correct-by-
 * construction: it does NOT swallow the events — ImGui still processes them —
 * and the canvas API is a no-op when no field is focused. */
static void forward_text_input_to_canvas(void)
{
    ImGuiIO &io = ImGui::GetIO();
    /* Text characters (already filtered to printable by ImGui). */
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        char u8[5];
        if (encode_utf8((unsigned)io.InputQueueCharacters[i], u8) > 0)
            jce_editor_game_render_text_input(u8);
    }
    /* Editing keys.  Map ImGuiKey → JCE scancode for the keys the canvas
     * InputField understands. */
    unsigned short mod = 0;
    if (io.KeyShift) mod |= JCE_KMOD_SHIFT;
    if (io.KeyCtrl)  mod |= JCE_KMOD_CTRL;
    if (io.KeyAlt)   mod |= JCE_KMOD_ALT;
    struct { ImGuiKey ik; int jk; } map[] = {
        { ImGuiKey_Backspace,   JCE_KEY_BACKSPACE },
        { ImGuiKey_Delete,      JCE_KEY_DELETE    },
        { ImGuiKey_LeftArrow,   JCE_KEY_LEFT      },
        { ImGuiKey_RightArrow,  JCE_KEY_RIGHT     },
        { ImGuiKey_Home,        JCE_KEY_HOME      },
        { ImGuiKey_End,         JCE_KEY_END       },
        { ImGuiKey_Enter,       JCE_KEY_RETURN    },
        { ImGuiKey_KeypadEnter, JCE_KEY_KP_ENTER  },
        { ImGuiKey_Escape,      JCE_KEY_ESCAPE    },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (ImGui::IsKeyPressed(map[i].ik, /*repeat*/true))
            jce_editor_game_render_key_edit(map[i].jk, mod);
}

/* Whether the Game View is actively driving game input this frame: the user
 * has clicked into the viewport to take control of the camera / player (FPS
 * fly-cam, WASD free-fly, or CharacterController), OR the OS mouse is currently
 * captured.  True even during a momentary LeftAlt free-look release, where the
 * cursor is freed but the user is still mid-session driving the Game View.
 *
 * The editor shortcut gate consults this (gated by Play state) so editor
 * hotkeys (Ctrl+S / Ctrl+A / …) do not mis-fire while the user is playing in
 * the Game View — Play-consistent, not just the strict mouse-captured case. */
bool jce_editor_game_view_is_input_active(void)
{
    return s_user_wants_capture ||
           jce_editor_game_render_is_mouse_captured() ||
           (s_runtime_pointer_camera &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left));
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_game_view_content(void)
{
    ensure_run_mode_loaded();
    jce_run_manager_poll();
    apply_pending_game_exe_pick();

    /* One-time user-global restore (the session store is always live). */
    static bool s_ui_state_loaded = false;
    if (!s_ui_state_loaded) {
        s_ui_state_loaded = true;
        s_show_stats = jce_editor_ui_state_load_int("gameview.stats", 0, 0, 1) != 0;
    }

    /* Restore per-project view state once the project store is live — it is
     * inert until a project root is known, so the first draw can be too
     * early to read from it. */
    static bool s_pstate_restored = false;
    if (!s_pstate_restored && jce_editor_pstate_active()) {
        s_pstate_restored = true;
        s_aspect_idx = jce_editor_pstate_get_int("gameview.aspect", s_aspect_idx);
        if (s_aspect_idx < 0 || s_aspect_idx > 5) s_aspect_idx = 0;
        s_third_person = jce_editor_pstate_get_int("gameview.third_person",
                                                   s_third_person ? 1 : 0) != 0;
        char mod_name[128];
        if (jce_editor_pstate_get_str("gameview.module", mod_name,
                                      sizeof(mod_name))) {
            /* Re-select the persisted game module by NAME in the same
             * registry the combo lists; an unknown name keeps the default
             * (module sets can differ between builds). */
            int n = jce_game_module_count();
            for (int i = 0; i < n; ++i) {
                const char *mn = jce_game_module_name_at(i);
                if (mn && strcmp(mn, mod_name) == 0) {
                    jce_editor_game_render_set_module(jce_game_module_at(i));
                    break;
                }
            }
        }
    }

    /* Toolbar row */
    const char *aspects[] = { jce_editor_i18n("gameView.aspect.free"), "16:9", "16:10", "4:3", "21:9", "1:1" };
    ImGui::PushItemWidth(80);
    if (ImGui::Combo("##aspect", &s_aspect_idx, aspects, 6))
        jce_editor_pstate_set_int("gameview.aspect", s_aspect_idx);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    /* Renderer backend selector.
     * The combo reflects the user's PERSISTED choice (editor config),
     * NOT the live bgfx backend.  Why: bgfx cannot hot-swap backends,
     * so the chosen value only takes effect at next launch — if we
     * live-synced the combo to the running backend, a user who tries
     * to re-pick the same backend as the live one wouldn't generate
     * a change event (ImGui::Combo returns false when idx is unchanged),
     * leaving the "restart required" hint stuck on screen.  Mirroring
     * the persisted value makes re-selecting the live backend a real
     * write (config := live), which clears the hint cleanly. */
    const char *const *renderer_names = nullptr;
    int renderer_count = jce_editor_renderer_backends(&renderer_names);
    JceRendererBackend live_bk = jce_renderer_get_backend(NULL);
    const char *live_name = jce_renderer_backend_name(live_bk);

    JceEditorConfig ren_cfg;
    jce_editor_config_load(&ren_cfg);
    int renderer_idx = 0;
    for (int i = 0; i < renderer_count; ++i) {
        if (renderer_names[i] && ren_cfg.renderer[0]
                && strcmp(renderer_names[i], ren_cfg.renderer) == 0) {
            renderer_idx = i;
            break;
        }
    }
    ImGui::PushItemWidth(110);
    if (ImGui::Combo("##renderer", &renderer_idx,
                     renderer_names, renderer_count)
            && renderer_idx >= 0 && renderer_idx < renderer_count) {
        const char *picked = renderer_names[renderer_idx];
        if (picked && strcmp(ren_cfg.renderer, picked) != 0) {
            snprintf(ren_cfg.renderer, sizeof(ren_cfg.renderer), "%s",
                     picked);
            jce_editor_config_save(&ren_cfg);
            LOG_INFO("editor.game_view",
                "renderer backend selection changed to '%s' — "
                "restart the editor to apply", picked);
        }
    }
    ImGui::PopItemWidth();

    /* Inline hint: shown whenever the persisted choice differs from
     * the actually-running bgfx backend.  Disappears as soon as the
     * user picks the live backend again (or restarts the editor). */
    if (ren_cfg.renderer[0] && live_name
            && strcmp(ren_cfg.renderer, live_name) != 0) {
        ImGui::SameLine();
        char buf[160];
        snprintf(buf, sizeof(buf),
                 jce_editor_i18n("game.renderer.restartRequired"),
                 ren_cfg.renderer);
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f), "%s", buf);
    }

    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###stats", jce_editor_i18n("game.stats"));
        if (ImGui::Checkbox(_lbl, &s_show_stats))
            jce_editor_ui_state_save_int("gameview.stats", s_show_stats ? 1 : 0);
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    const char *run_modes[] = { jce_editor_i18n("gameView.runMode.editorSim"), jce_editor_i18n("gameView.runMode.externalGame") };
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
                if (ImGui::Selectable(n, sel)) {
                    jce_editor_game_render_set_module(jce_game_module_at(i));
                    /* Persist by NAME — module pointers/indices are not
                     * stable across editor runs. */
                    jce_editor_pstate_set_str("gameview.module", n);
                }
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
                s_pending_game_exe_path[0] = '\0';
                s_pending_game_exe_ready = false;
                open_file_dialog_async(
                    jce_editor_i18n("gameView.selectGameExe"), nullptr,
#if JCE_PLATFORM_WINDOWS
                    jce_editor_i18n_or("fileDialog.filter.executables",
                         "Executables (*.exe);;All Files (*.*)"),
#else
                    jce_editor_i18n_or("fileDialog.filter.allFilesUnix", "All Files (*)"),
#endif
                    s_pending_game_exe_path, sizeof(s_pending_game_exe_path),
                    &s_pending_game_exe_ready,
                    NULL);
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
            if (rs.state == JCE_RUN_BUILDING)
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", jce_editor_i18n("gameView.repackingAssets"));
            else if (rs.state == JCE_RUN_STOPPING)
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

    uint32_t vw = (uint32_t)fmaxf(1.0f, view_size.x);
    uint32_t vh = (uint32_t)fmaxf(1.0f, view_size.y);
    if (vw < 16u) vw = 16u;
    if (vh < 16u) vh = 16u;
    uint32_t render_vw = vw;
    uint32_t render_vh = vh;
    uint16_t qa_width = 0;
    uint16_t qa_height = 0;
    if (jce_editor_kpi_game_capture_global_render_size(
            &qa_width, &qa_height)) {
        render_vw = qa_width;
        render_vh = qa_height;
    }

    /* Feed the ECS-UI (Canvas) graphic raycaster the panel-local pointer,
     * mapped from the displayed image rect into rendered (FBO) pixels.  Only
     * valid while the cursor is over the viewport and we are NOT in FPS
     * capture (where the cursor is pinned/hidden). */
    {
        const ImGuiIO &io = ImGui::GetIO();
        bool fps = jce_editor_game_render_is_mouse_captured();
        float lx = io.MousePos.x - region_start.x;
        float ly = io.MousePos.y - region_start.y;
        bool inside = !fps && view_size.x > 0 && view_size.y > 0 &&
                      lx >= 0 && ly >= 0 &&
                      lx < view_size.x && ly < view_size.y;
        float ui_x = inside ? lx / view_size.x * (float)render_vw : 0.0f;
        float ui_y = inside ? ly / view_size.y * (float)render_vh : 0.0f;
        bool down = inside && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        jce_editor_game_render_set_ui_pointer(ui_x, ui_y, down, inside);

        /* Forward keyboard / text into the focused ECS-UI InputField while the
         * Game View is hovered (and not in FPS capture) during Play.  The
         * canvas API is fire-and-forget (no-op when nothing focused) and does
         * not consume the events — ImGui still sees them. */
        JcePlayState ui_ps = jce_state_get_play_state();
        if (inside && ui_ps == JCE_PLAY_PLAYING)
            forward_text_input_to_canvas();
    }

    jce_editor_game_render_frame(render_vw, render_vh);

    uint16_t tex_idx = jce_editor_game_render_get_texture();
    if (tex_idx != UINT16_MAX) {
        jce_editor_kpi_game_capture_global_note_rendered();
        if (const char *capture_path =
                jce_editor_kpi_game_capture_global_due_path()) {
            if (jce_editor_game_render_screenshot(capture_path)) {
                (void)jce_editor_kpi_game_capture_global_mark_submitted();
                LOG_INFO("editor.game_view",
                         "Game View capture submitted: %s", capture_path);
            }
        }
        /* +1: encode bgfx idx so a valid idx 0 != ImTextureID_Invalid(0)
         * (audit Round-3 P2-B; the imgui_renderer backend decodes -1). */
        ImTextureID tid = (ImTextureID)(uintptr_t)((uint32_t)tex_idx + 1u);
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
    /* s_user_wants_capture is file-scoped (top of this TU) so the editor
       shortcut gate can query it via jce_editor_game_view_is_input_active(). */

    JceCamera *cam = jce_editor_game_render_get_camera();
    JcePlayState play_state = jce_state_get_play_state();
    bool play_active = (play_state == JCE_PLAY_PLAYING ||
                        play_state == JCE_PLAY_PAUSED);

    float player_x = 0.0f, player_y = 0.0f, player_z = 0.0f;
    const bool has_player_controller =
        play_active &&
        jce_editor_play_get_player_position(&player_x, &player_y, &player_z);
    s_runtime_pointer_camera =
        play_active && !has_player_controller && game_view_has_active_vcam();

    /* ── ScrollView wheel channel ───────────────────────────────────────
     * Forward ImGui's mouse-wheel (vertical io.MouseWheel = +up, horizontal
     * io.MouseWheelH = +right) into the ECS-UI ScrollView under the pointer.
     * Runs after the per-frame render (jce_editor_game_render_frame above), so
     * the canvas has resolved this frame's hovered scroll view, and only while
     * the Game View Image is hovered + not FPS-captured during Play.  Fire-and-
     * forget: a no-op when no scroll view is hovered, and it does NOT consume
     * the wheel — ImGui still sees it. */
    if (play_state == JCE_PLAY_PLAYING && hovered &&
        !jce_editor_game_render_is_mouse_captured()) {
        ImGuiIO &io_w = ImGui::GetIO();
        if (io_w.MouseWheel != 0.0f || io_w.MouseWheelH != 0.0f)
            jce_editor_game_render_scroll(io_w.MouseWheelH, io_w.MouseWheel);
    }

    /* Re-arm the third-person boom on each Play start so a collision-
     * shortened length from the previous session doesn't leak in. */
    static bool s_tp_was_play = false;
    if (play_active && !s_tp_was_play) s_tp_dist = kTpBoomLen;
    s_tp_was_play = play_active;

    /* Cursor capture is now user-initiated only (click into the Game
       View → capture; ESC / ALT / Stop → release). Auto-capturing on
       the Play rising edge was Unity-ish for "instant game feel" but
       it also stole the cursor from the Scene viewport, so dragging
       gizmos (TRS) during Play silently failed because the absolute
       mouse position was pinned by SDL relative-mouse mode. Let the
       user opt in to capture by clicking the Game View. */
    static JcePlayState s_prev_play_state = JCE_PLAY_STOPPED;
    /* Stop edge: when the user presses Stop, immediately release the
     * cursor (Unity-parity behavior).  Without this, s_user_wants_capture
     * stays true and the Game View keeps the cursor locked even though
     * play has ended, blocking the user from clicking other panels. */
    if (play_state == JCE_PLAY_STOPPED &&
        s_prev_play_state != JCE_PLAY_STOPPED) {
        s_user_wants_capture = false;
    }
    /* Play rising edge (STOPPED -> PLAYING): orient the game-view camera to the
     * player's forward once (after the first player-snap below) so the user
     * starts looking where the character faces — e.g. at enemies ahead. */
    static bool s_orient_to_player_forward = false;
    if (play_state != JCE_PLAY_STOPPED && s_prev_play_state == JCE_PLAY_STOPPED)
        s_orient_to_player_forward = true;
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
        /* A scene-owned VCam receives pointer data through JceRuntime.  It
         * must not also acquire the editor's free-fly/FPS camera capture. */
        if (s_runtime_pointer_camera)
            s_user_wants_capture = false;

        if (!s_runtime_pointer_camera && hovered &&
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
            /* V toggles first/third-person follow camera (Play mode). */
            if (play_active && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
                s_third_person = !s_third_person;
                jce_editor_pstate_set_int("gameview.third_person",
                                          s_third_person ? 1 : 0);
            }
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

                /* Input Manager actions drive movement (live panel state,
                 * rebinds apply instantly); the hardcoded key is only the
                 * fallback when an action is missing or has no key bind. */
                auto act_down = [](const char *name, ImGuiKey fallback) {
                    int keys[4];
                    int n = jce_editor_input_action_keys(name, keys, 4);
                    if (n <= 0) return ImGui::IsKeyDown(fallback);
                    for (int i = 0; i < n; i++)
                        if (ImGui::IsKeyDown((ImGuiKey)keys[i])) return true;
                    return false;
                };
                auto act_pressed = [](const char *name, ImGuiKey fallback) {
                    int keys[4];
                    int n = jce_editor_input_action_keys(name, keys, 4);
                    if (n <= 0) return ImGui::IsKeyPressed(fallback, false);
                    for (int i = 0; i < n; i++)
                        if (ImGui::IsKeyPressed((ImGuiKey)keys[i], false)) return true;
                    return false;
                };

                float wx = 0.0f, wz = 0.0f;
                if (act_down("move_forward", ImGuiKey_W)) { wx += fwd.x;   wz += fwd.z;   }
                if (act_down("move_back",    ImGuiKey_S)) { wx -= fwd.x;   wz -= fwd.z;   }
                if (act_down("move_right",   ImGuiKey_D)) { wx += right.x; wz += right.z; }
                if (act_down("move_left",    ImGuiKey_A)) { wx -= right.x; wz -= right.z; }
                float wlen = sqrtf(wx*wx + wz*wz);
                if (wlen > 0.0001f) { wx /= wlen; wz /= wlen; }

                bool jump      = act_pressed("jump", ImGuiKey_Space);
                bool jump_held = act_down("jump", ImGuiKey_Space);
                /* Hold sprint (Input Manager action; Ctrl fallback) — the
                 * authored CharacterController sprint_mult raises the speed
                 * so the locomotion blend tree / SM crosses into Run. */
                bool sprint =
                    act_down("sprint", ImGuiKey_LeftCtrl) ||
                    ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                /* Melee/attack — fed as HELD (not edge): the player input is
                 * gathered here but consumed by the runtime step on the next
                 * tick, so a 1-frame edge would be missed.  The script's own
                 * cooldown gates the swing rate.  (J action; left-mouse too.)
                 *
                 * Left-mouse comes from the captured-button tracker, NOT
                 * ImGui::IsKeyDown(ImGuiKey_MouseLeft): during FPS capture the
                 * editor stops forwarding mouse buttons to ImGui, so ImGui's
                 * MouseLeft sticks DOWN forever (the capture-acquire click's
                 * release is swallowed) — which froze attack_pressed true and
                 * let the Lua rising-edge fire exactly once per Play session. */
                bool attack = act_down("attack", ImGuiKey_J) ||
                              jce_editor_game_render_mouse_button(0);
                /* Unit direction only — move_speed/jump arc come from the
                 * scene's CharacterController component. */
                jce_editor_play_set_player_input(wx, wz, jump, jump_held, sprint, attack);
                /* Facing + idle/walk/run clip are driven generically by the
                 * engine runtime (rt_drive_character), so it also works in the
                 * shipped game, not just here. */
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
                if (s_third_person) {
                    /* Orbit behind the player along the mouse-controlled
                     * forward, looking at chest height (V toggles this).
                     * The boom collides with the world (sphere sweep against
                     * the live Play physics) and its length is smoothed, so
                     * the camera neither clips through walls nor pops. */
                    jce_vec3 fwd = jce_camera_get_forward(cam);
                    jce_vec3 pivot = jce_v3(px, py + 1.5f, pz);
                    jce_vec3 back  = jce_v3(-fwd.x, -fwd.y, -fwd.z);
                    float target_dist = kTpBoomLen;
                    if (JcePhysicsWorld *pw = jce_editor_play_get_physics_world()) {
                        JceRaycastResult hit = jce_physics_sweep_sphere(
                            pw, pivot, 0.25f, back, kTpBoomLen,
                            jce_query_filter_default());
                        /* Hits closer than the capsule radius are the player
                         * itself (the pivot sits inside it) — ignore those. */
                        if (hit.hit && hit.distance > 0.6f &&
                            hit.distance < target_dist)
                            target_dist = hit.distance;
                    }
                    /* Snap IN on collision (never clip), recover OUT smoothly. */
                    if (target_dist < s_tp_dist) s_tp_dist = target_dist;
                    else {
                        float k = 1.0f - expf(-8.0f * (dt > 0 ? dt : 0.016f));
                        s_tp_dist += (target_dist - s_tp_dist) * k;
                    }
                    float shk[3] = {0.0f, 0.0f, 0.0f};
                    jce_vcam_system_get_shake_offset(shk);
                    jce_camera_set_position(cam,
                        jce_v3(pivot.x + back.x * s_tp_dist + shk[0],
                               pivot.y + back.y * s_tp_dist + 0.4f + shk[1],
                               pivot.z + back.z * s_tp_dist + shk[2]));
                } else {
                    float shk[3] = {0.0f, 0.0f, 0.0f};
                    jce_vcam_system_get_shake_offset(shk);
                    jce_camera_set_position(cam,
                        jce_v3(px + shk[0], py + 1.6f + shk[1], pz + shk[2]));
                }
                /* One-shot on Play start: aim the camera down the player's
                 * forward so the view begins facing where the character does
                 * (e.g. at the enemies ahead).  General default framing; a
                 * Play-start VCam (evaluated below) still overrides this. */
                if (s_orient_to_player_forward) {
                    float fx, fy, fz;
                    if (jce_editor_play_get_player_forward(&fx, &fy, &fz)) {
                        jce_camera_look_at(cam,
                            jce_v3(px + fx * 10.0f, py + 1.6f + fy * 10.0f,
                                   pz + fz * 10.0f));
                        s_orient_to_player_forward = false;
                    }
                }
            }
        }

    }

    /* Publish ownership for the next platform-event/update phase.  Runtime
     * camera/player input and the editor free-fly camera are exclusive; the
     * core bridge clears held state whenever ownership or hover is lost. */
    JceEditorGameInputViewport input_viewport{};
    input_viewport.x = image_min.x;
    input_viewport.y = image_min.y;
    input_viewport.width = view_size.x;
    input_viewport.height = view_size.y;
    input_viewport.visible = true;
    input_viewport.hovered = hovered;
    input_viewport.captured =
        jce_editor_game_render_is_mouse_captured();
    if (play_active &&
        (s_runtime_pointer_camera || has_player_controller)) {
        input_viewport.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
    } else if (play_active) {
        input_viewport.owner = JCE_EDITOR_GAME_INPUT_FREE_FLY;
    } else {
        input_viewport.owner = JCE_EDITOR_GAME_INPUT_NONE;
    }
    jce_editor_game_input_bridge_publish(
        jce_editor_game_input_bridge_shared(), &input_viewport);

    /* HUD overlay: capture state + hint. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (jce_editor_game_render_is_mouse_captured()) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(80, 220, 120, 230),
                    jce_editor_i18n("gameView.hud.flyCaptured"));
    } else if (s_user_wants_capture) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(255, 220, 120, 230),
                    jce_editor_i18n("gameView.hud.altFree"));
    } else if (hovered && !s_runtime_pointer_camera) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(220, 220, 220, 200),
                    jce_editor_i18n("gameView.hud.clickToFly"));
    }

    if (s_show_stats) {
        const char *current = jce_renderer_get_backend_name(NULL);
        if (!current || !current[0]) current = "?";
        char buf[128];
        snprintf(buf, sizeof(buf), "%s: %s | %.1f fps  %ux%u",
                 jce_editor_i18n("game.renderer"), current,
                 ImGui::GetIO().Framerate, render_vw, render_vh);
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
    if (ImGui::Begin(title, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_game_view_content();
    ImGui::End();
    ImGui::PopStyleVar();
}
