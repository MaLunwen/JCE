/*
 * jce_panel_game_view.cpp  Game View panel (runtime viewport).
 * Extracted from jce_editor_panels.cpp.
 */

#include <cstdlib>   /* getenv for the QA autowalk hook below */
#include "core/jce_editor_config.h"
#include "core/jce_editor_renderer_startup.h"
#include "core/jce_editor_viewport_work.h"
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_play_input_owner.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_kpi_game_capture.h"
#include "core/jce_editor_project_state.h"
#include "jce_game_view_focus_policy.h"
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
#include <jce/os/platform/jce_input.h>  /* physical Alt level */
#include <jce/os/platform/jce_input_actions.h>
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

/* Keep persisted aliases consistent with the startup resolver. An unknown or
 * unavailable choice retains its configured preview instead of becoming Auto. */
static int gv_renderer_choice_index(const char *requested,
                                    const char *const *names, int count)
{
    const int backend = jce_editor_renderer_backend_from_name(requested);
    if (backend < 0 || !names) return -1;
    for (int i = 0; i < count; ++i) {
        if (names[i] &&
            jce_editor_renderer_backend_from_name(names[i]) == backend)
            return i;
    }
    return -1;
}

static bool gv_renderer_restart_required(const char *requested,
                                         JceRendererBackend live)
{
    const int backend = jce_editor_renderer_backend_from_name(requested);
    return backend > JCE_BACKEND_AUTO && live != JCE_BACKEND_AUTO &&
           backend != (int)live;
}

/* ── Game View state ──────────────────────────────────────────────── */

static int  s_aspect_idx  = 0;
static bool s_show_stats  = false;
static bool s_show_hints = false;
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
static bool s_alt_held_previous_frame = false;
/* Explicit, per-project opt-in. Auto keeps UI/strategy games pointer-driven. */
static bool s_script_mouse_look = false;
static bool s_script_mouse_look_scope_valid = false;
static char s_script_mouse_look_root[512] = {0};
static bool s_runtime_pointer_camera = false;
static const float kTpBoomLen = 4.5f; /* third-person orbit distance */
static float s_tp_dist = 4.5f;        /* smoothed boom length (collision-shortened) */
static char s_pending_game_exe_path[512] = {0};
static bool s_pending_game_exe_ready = false;

/* ── Gamepad in Play ──────────────────────────────────────────────────
 * The keyboard reaches Play through ImGui, which is what confines it to a
 * focused, clicked-into viewport.  A pad has no cursor and cannot click into
 * anything, so it is read from the ENGINE's action map instead -- the only
 * map in the editor that SDL actually feeds (jce_engine.c calls
 * jce_actions_update on it every frame).  The two are composed by addition,
 * so neither is a mode: whichever device the user moves, moves the character.
 *
 * The value is SIGNED and analog.  "move_back" / "move_left" have no default
 * pad binding on purpose -- one stick axis carries both directions and the
 * sign IS the direction -- so this must never be collapsed to a bool.  That
 * collapse (jce_action_down's `value != 0`) is exactly what made a stick
 * pulled fully BACK read as "move_forward is down". */
static float gv_pad_value(const JceInputActions *map, const char *name)
{
    if (!map) return 0.0f;
    int id = jce_action_find(map, name);
    if (id < 0) return 0.0f;
    return jce_action_value_device(map, id, JCE_DEVICE_GAMEPAD);
}

/* The keyboard a Play SCRIPT may read, which is not the same question as the
 * keyboard the editor's own camera may read.
 *
 * The capture gate is right for a person: without it a Play script would read
 * keys the user is typing into another panel.  It is wrong for a REPLAYED
 * session, and for exactly the reason this file already gives one paragraph
 * down about gamepads -- "a pad has no cursor and cannot click into
 * anything".  Neither does a .jirc file.  With JCE_INPUT_REPLAY set there is
 * no user, no other panel, and nothing to protect; gating on capture there
 * means the shipped runtime replays an input file and the editor silently
 * does not, from the same scene and the same file.
 *
 * And the thing the gate protects against cannot happen during a replay:
 * jce_input_replay_tick() OVERWRITES the engine's input with the file's frame
 * every tick (jce_engine.c:1984), so what Play reads is the recording, not
 * whatever the user is typing into the Inspector.  The keys that worried the
 * gate are not in that object to be read.
 *
 * Measured before it was written: editor Play drove the snake straight into
 * the wall while the shipped runtime turned, on the same 1800-frame input. */
static struct JceInput *gv_script_keyboard(bool kb)
{
    static int s_replay = -1;
    if (s_replay < 0) {
        const char *e = getenv("JCE_INPUT_REPLAY");
        s_replay = (e && e[0]) ? 1 : 0;
    }
    struct JceInput *in = (kb || s_replay) ? jce_editor_engine_input() : NULL;
    /* Said once, because "the runtime was handed no keyboard" and "this code
     * never ran" produce the same silence downstream -- and this function was
     * added to fix exactly that class of bug. */
    static int s_said = 0;
    if (!s_said) {
        s_said = 1;
        LOG_INFO("editor.play",
                 "script keyboard: capture=%d replay=%d -> %s",
                 kb ? 1 : 0, s_replay, in ? "engine input" : "NULL");
    }
    return in;
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
/* A level-triggered Alt sample, never a key-down edge/toggle. The engine
 * input owns physical key state even while ImGui stops receiving pointer
 * events in SDL relative mode. The ImGui fallback covers an absent host. */
static bool gv_alt_held_now()
{
    const JceInput *input = jce_editor_engine_input();
    if (input)
        return jce_input_key_down(input, JCE_KEY_LALT) ||
               jce_input_key_down(input, JCE_KEY_RALT);
    return ImGui::GetCurrentContext() &&
           (ImGui::IsKeyDown(ImGuiKey_LeftAlt) ||
            ImGui::IsKeyDown(ImGuiKey_RightAlt));
}

bool jce_editor_game_view_is_input_active(void)
{
    /* Alt temporarily releases the cursor for editor UI: do not keep
     * claiming shortcuts that now belong to another editor panel. */
    if (ImGui::GetCurrentContext()) {
        const ImGuiIO &io = ImGui::GetIO();
        if (io.AppFocusLost || gv_alt_held_now())
            return false;
    }
    return s_user_wants_capture ||
           jce_editor_game_render_is_mouse_captured() ||
           (s_runtime_pointer_camera &&
            ImGui::IsMouseDown(ImGuiMouseButton_Left));
}

void jce_editor_game_view_release_play_input(void)
{
    /* Stop is a lifecycle transition, not a UI-paint event. When Game View is
     * clipped/hidden its existing Stop-edge does not run. Retire all three
     * owners now, then synthesize button-up for any gameplay-captured press
     * that ImGui never received. It is safe if the physical release arrives
     * later: AddMouseButtonEvent is an edge queue and duplicate up is inert. */
    const bool had_owner = s_user_wants_capture ||
                           jce_editor_game_render_is_mouse_captured();
    s_user_wants_capture = false;
    s_alt_held_previous_frame = false;
    s_runtime_pointer_camera = false;
    jce_editor_game_render_set_mouse_capture(false);
    if (had_owner && ImGui::GetCurrentContext()) {
        ImGuiIO &io = ImGui::GetIO();
        for (int button = 0; button < 5; ++button)
            io.AddMouseButtonEvent(button, false);
    }
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
        s_show_hints = jce_editor_ui_state_load_int("gameview.hints", 0, 0, 1) != 0;
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

    /* Mouse Look is project-scoped. Unlike the older once-per-session viewport
     * preferences, reread this on EVERY project-root switch so a pointer UI
     * game never inherits the previous project's FPS lock preference. */
    if (jce_editor_pstate_active()) {
        const char *root = jce_editor_current_project_root();
        if (root && root[0] &&
            (!s_script_mouse_look_scope_valid ||
             strcmp(s_script_mouse_look_root, root) != 0)) {
            snprintf(s_script_mouse_look_root, sizeof(s_script_mouse_look_root),
                     "%s", root);
            s_script_mouse_look_scope_valid = true;
            s_script_mouse_look =
                jce_editor_pstate_get_int("gameview.script_mouse_look", 0) != 0;
            s_user_wants_capture = false;
            jce_editor_game_render_set_mouse_capture(false);
        }
    } else if (s_script_mouse_look_scope_valid) {
        s_script_mouse_look_scope_valid = false;
        s_script_mouse_look_root[0] = '\0';
        s_script_mouse_look = false;
        s_user_wants_capture = false;
        jce_editor_game_render_set_mouse_capture(false);
    }

    /* Toolbar row */
    const char *aspects[] = { jce_editor_i18n("gameView.aspect.free"), "16:9", "16:10", "4:3", "21:9", "1:1" };
    ImGui::PushItemWidth(80);
    if (ImGui::Combo("##aspect", &s_aspect_idx, aspects, 6))
        jce_editor_pstate_set_int("gameview.aspect", s_aspect_idx);
    ImGui::PopItemWidth();

    /* Maximise, right next to the aspect control: the two together are "how the
     * picture is framed", which is where a user looks for it. The flag lives in
     * the layout TU -- this only asks it to flip.
     *
     * Not bound to F10 any more: on Windows F10 is claimed by the Win32
     * window-menu convention before the app ever sees it, which is why the
     * first attempt appeared to do nothing at all. Shift+F11 sits beside the
     * existing F11 (OS-window fullscreen) and is not claimed by the shell. */
    ImGui::SameLine();
    {
        const bool maxed = jce_editor_game_view_maximized();
        if (ImGui::SmallButton(maxed
                ? jce_editor_i18n("viewport.restore")
                : jce_editor_i18n("viewport.maximize")))
            jce_editor_game_view_set_maximized(!maxed);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("viewport.maximize.tip"));
    }

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

    JceEditorConfig ren_cfg;
    jce_editor_config_load(&ren_cfg);
    int renderer_idx = gv_renderer_choice_index(ren_cfg.renderer,
                                               renderer_names, renderer_count);
    const char *renderer_preview = renderer_idx >= 0
        ? renderer_names[renderer_idx] : ren_cfg.renderer;
    ImGui::PushItemWidth(110);
    if (ImGui::BeginCombo("##renderer", renderer_preview)) {
        for (int i = 0; i < renderer_count; ++i) {
            const char *picked = renderer_names[i];
            if (!picked) continue;
            const bool selected = (i == renderer_idx);
            if (ImGui::Selectable(picked, selected)) {
                const int picked_backend =
                    jce_editor_renderer_backend_from_name(picked);
                if (picked_backend >= 0 && picked_backend !=
                    jce_editor_renderer_backend_from_name(ren_cfg.renderer)) {
                    snprintf(ren_cfg.renderer, sizeof(ren_cfg.renderer), "%s",
                             picked);
                    jce_editor_config_save(&ren_cfg);
                    LOG_INFO("editor.game_view",
                        "renderer backend selection saved: '%s' restart_required=%d",
                        picked, gv_renderer_restart_required(ren_cfg.renderer, live_bk));
                }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    /* Auto accepts the live backend; unknown requests have no applicable
     * backend. Only a valid different request requires the next launch. */
    if (gv_renderer_restart_required(ren_cfg.renderer, live_bk)) {
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
    char hints_label[96];
    snprintf(hints_label, sizeof(hints_label), "%s###gameview_hints",
             jce_editor_i18n_or("gameView.hud.hintsToggle", "Hints"));
    if (ImGui::Checkbox(hints_label, &s_show_hints))
        jce_editor_ui_state_save_int("gameview.hints", s_show_hints ? 1 : 0);

    ImGui::SameLine();
    char mouse_look_label[128];
    snprintf(mouse_look_label, sizeof(mouse_look_label), "%s##gameview_script_pointer_lock",
             jce_editor_i18n_or("gameView.input.mouseLook", "Mouse Look"));
    if (ImGui::Checkbox(mouse_look_label,
                        &s_script_mouse_look)) {
        jce_editor_pstate_set_int("gameview.script_mouse_look",
                                  s_script_mouse_look ? 1 : 0);
        /* Toggling via toolbar does not instantly trap the cursor under
         * the button: click back into the image to take control. */
        s_user_wants_capture = false;
        jce_editor_game_render_set_mouse_capture(false);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("gameView.input.mouseLookHint"));

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
    if (avail.x <= 0.0f || avail.y <= 0.0f) {
        jce_editor_game_render_observation_policy(
            static_cast<uint32_t>(JceEditorViewportPolicyReason::InvalidLayout));
        jce_editor_viewport_policy_trace("editor.game.policy", JceEditorViewportPolicyReason::InvalidLayout);
        s_user_wants_capture = false;
        s_runtime_pointer_camera = false;
        (void)jce_editor_game_render_set_mouse_capture(false);
        jce_editor_game_render_set_ui_pointer(0.0f, 0.0f, false, false);
        jce_editor_game_input_bridge_publish(jce_editor_game_input_bridge_shared(), nullptr);
        return;
    }

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

    const bool viewport_work = jce_editor_viewport_wants_render(region_start, view_size,
        jce_editor_viewport_capture_requested(jce_editor_kpi_game_capture_global_active()),
        "editor.game.policy");
    /* Suppress GPU work, while still running the existing input/focus/Stop
     * lifecycle below: a clipped panel must release previous held inputs. */

    if (viewport_work) jce_editor_play_publish_viewport(render_vw, render_vh);
    uint32_t canvas_w = 0, canvas_h = 0;
    jce_editor_game_render_resolve_extent(render_vw, render_vh, &canvas_w, &canvas_h);

    /* Feed the ECS-UI (Canvas) graphic raycaster the panel-local pointer,
     * mapped from the displayed image rect into rendered (FBO) pixels.  Only
     * valid while the cursor is over the viewport, we are NOT in FPS capture
     * (where the cursor is pinned/hidden), and PLAY IS RUNNING.
     *
     * The play gate is not cosmetic.  The raycaster does not merely highlight:
     * uc_update_widgets writes its results straight back into the scene's
     * components (jce_ui_canvas.c — `sl->value = v`, `tg->is_on = !tg->is_on`,
     * `dd->selected_index = pressed`), because a UI widget's value IS its
     * component, the same source-of-truth model Unity uses.  Without the gate,
     * moving the mouse across a stopped Game View silently rewrote authored
     * slider / toggle / dropdown values in the scene the user was about to
     * save.  Its two siblings below — the text-input forward and the scroll
     * forward — were already gated; the pointer was the one that was missed.
     *
     * Unity's own split is the same: the Scene View previews UI layout but
     * does not run the widget state machine; only Play does. */
    {
        const ImGuiIO &io = ImGui::GetIO();
        JcePlayState ui_ps = jce_state_get_play_state();
        bool playing = (ui_ps == JCE_PLAY_PLAYING);
        bool fps = jce_editor_game_render_is_mouse_captured();
        float lx = io.MousePos.x - region_start.x;
        float ly = io.MousePos.y - region_start.y;
        const bool native_inside = viewport_work && playing && !fps && view_size.x > 0 && view_size.y > 0 &&
                      lx >= 0 && ly >= 0 &&
                      lx < view_size.x && ly < view_size.y;
        bool inside = native_inside;
        float ui_x = inside ? lx / view_size.x * (float)canvas_w : 0.0f;
        float ui_y = inside ? ly / view_size.y * (float)canvas_h : 0.0f;
        bool down = inside && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        /* Explicit virtual Game input must reach the same Canvas raycaster as
         * the Runtime. Peeking does not drain the next update's input sample;
         * the actual current geometry rejects a stale or covered viewport. */
        JceEditorGameInputViewport ui_viewport{};
        ui_viewport.x = region_start.x; ui_viewport.y = region_start.y;
        ui_viewport.width = view_size.x; ui_viewport.height = view_size.y;
        ui_viewport.render_width = (float)canvas_w;
        ui_viewport.render_height = (float)canvas_h;
        ui_viewport.visible = viewport_work && playing;
        ui_viewport.owner = playing ? JCE_EDITOR_GAME_INPUT_RUNTIME : JCE_EDITOR_GAME_INPUT_NONE;
        JceEditorGameInputSample virtual_pointer{};
        if (jce_editor_game_input_bridge_peek_virtual(
                jce_editor_game_input_bridge_shared(), &ui_viewport, &virtual_pointer)) {
            ui_x = virtual_pointer.x; ui_y = virtual_pointer.y;
            down = (virtual_pointer.buttons & 1u) != 0;
            inside = true;
        }
        jce_editor_game_render_set_ui_pointer(ui_x, ui_y, down, inside);

        /* Forward keyboard / text into the focused ECS-UI InputField while the
         * Game View is hovered (and not in FPS capture) during Play.  The
         * canvas API is fire-and-forget (no-op when nothing focused) and does
         * not consume the events — ImGui still sees them. */
        if (native_inside)
            forward_text_input_to_canvas();
    }

    jce_editor_game_render_observation_request(canvas_w, canvas_h);
    if (!viewport_work) {
        const auto reason = ImGui::GetIO().AppFocusLost
            ? JceEditorViewportPolicyReason::FocusLost : JceEditorViewportPolicyReason::Clipped;
        jce_editor_game_render_observation_policy(static_cast<uint32_t>(reason));
    }
    if (viewport_work) jce_editor_game_render_frame(canvas_w, canvas_h);

    uint16_t tex_idx = viewport_work ? jce_editor_game_render_get_texture() : UINT16_MAX;
    if (tex_idx != UINT16_MAX) {
        jce_editor_kpi_game_capture_global_note_rendered();
        if (const char *capture_path =
                jce_editor_kpi_game_capture_global_due_path()) {
            JceEditorGameScreenshotFrame frame{};
            if (jce_editor_game_render_screenshot(capture_path, &frame)) {
                jce_editor_kpi_game_capture_global_mark_submitted(
                    frame.source_frame, frame.submit_frame);
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
        jce_editor_game_render_observation_image(tex_idx);
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
        play_active && !has_player_controller;
    /* Auto: lock for native CharacterController. Explicit Mouse Look: allow
     * script-driven cameras like CK even with no CharacterController. */
    const bool capture_allowed = !play_active || has_player_controller ||
                                 s_script_mouse_look;

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

    /* Play acquires keyboard ownership. Relative mouse capture is separate:
     * only controller scenes lock the pointer; Alt temporarily releases it. */
    static JcePlayState s_prev_play_state = JCE_PLAY_STOPPED;
    /* Stop edge: when the user presses Stop, immediately release the
     * cursor (Unity-parity behavior).  Without this, s_user_wants_capture
     * stays true and the Game View keeps the cursor locked even though
     * play has ended, blocking the user from clicking other panels. */
    const bool play_just_stopped = play_state == JCE_PLAY_STOPPED &&
                                   s_prev_play_state != JCE_PLAY_STOPPED;
    if (play_just_stopped) jce_editor_game_view_release_play_input();
    /* Play rising edge: give keyboard focus to the Game View. Mouse capture
     * still requires this window to own input and an allowed pointer policy. */
    static bool s_orient_to_player_forward = false;
    const bool play_just_started =
        play_state != JCE_PLAY_STOPPED && s_prev_play_state == JCE_PLAY_STOPPED;
    if (play_just_started) {
        s_orient_to_player_forward = true;
        ImGui::SetWindowFocus();
    }
    s_prev_play_state = play_state;

    if (cam) {
        const ImGuiIO &io = ImGui::GetIO();
        float dt = io.DeltaTime > 0.0f ? io.DeltaTime : (1.0f / 60.0f);

        const bool alt_held = gv_alt_held_now();
        const bool alt_just_released =
            s_alt_held_previous_frame && !alt_held;
        s_alt_held_previous_frame = alt_held;
        const bool view_focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool any_click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool click_inside = hovered && any_click && !alt_held;
        const bool click_elsewhere = any_click && !hovered;
        if (click_inside) ImGui::SetWindowFocus();

        const bool was_captured = jce_editor_game_render_is_mouse_captured();
        const JceGameViewFocusInput focus_in = {
            s_user_wants_capture, play_just_started, play_just_stopped,
            viewport_work, !io.AppFocusLost,
            view_focused || click_inside, was_captured,
            click_inside, click_elsewhere, alt_held,
            ImGui::IsKeyPressed(ImGuiKey_Escape, false), capture_allowed,
            alt_just_released
        };
        const JceGameViewFocusOutput focus =
            jce_game_view_focus_decide(focus_in);
        s_user_wants_capture = focus.wanted;
        /* The capture wrapper checks host-window focus before setting
         * relative mode. Use its returned state, not an unchecked request. */
        bool effective_capture =
            jce_editor_game_render_set_mouse_capture(focus.request_capture);
        const bool game_keyboard = focus.keyboard_owned;
        if (play_active && was_captured != effective_capture) {
            LOG_INFO("editor.game_view",
                     "Game View pointer lock: %s mode=%s alt=%d focus=%d "
                     "viewport=%d keyboard=%d",
                     effective_capture ? "ACQUIRED" : "RELEASED",
                     s_script_mouse_look ? "script_mouse_look" : "auto",
                     alt_held ? 1 : 0, view_focused ? 1 : 0,
                     viewport_work ? 1 : 0, game_keyboard ? 1 : 0);
        }
        if (!was_captured && effective_capture) {
            /* Capture-acquire click's release may be swallowed by SDL's
             * relative mode. Do not leave ImGui thinking MouseLeft is down. */
            for (int button = 0; button < 5; ++button)
                ImGui::GetIO().AddMouseButtonEvent(button, false);
        }

        /* Sample the pad once per frame from the engine's hardware-fed map. */
        const JceInputActions *pad_map =
            play_active ? jce_editor_engine_actions() : nullptr;
        /* Signed pairs: the stick axis is bound to the POSITIVE action only,
         * so pulling back / left comes through as a negative "forward" /
         * "right".  Subtracting the negative action keeps a hand-authored
         * pad bind on move_back / move_left working too. */
        const float pad_fwd    = gv_pad_value(pad_map, "move_forward")
                               - gv_pad_value(pad_map, "move_back");
        const float pad_right  = gv_pad_value(pad_map, "move_right")
                               - gv_pad_value(pad_map, "move_left");
        const float pad_look_x = gv_pad_value(pad_map, "look_x");
        const float pad_look_y = gv_pad_value(pad_map, "look_y");
        const bool  pad_jump   = gv_pad_value(pad_map, "jump")   != 0.0f;
        const bool  pad_sprint = gv_pad_value(pad_map, "sprint") != 0.0f;
        const bool  pad_attack = gv_pad_value(pad_map, "attack") != 0.0f;

        /* Rising edge for jump, tracked here because the engine's own
         * pressed/released edges are computed on the WHOLE action and would
         * fire for the keyboard SPACE this panel deliberately routes through
         * ImGui instead. */
        static bool s_pad_jump_prev = false;
        const bool pad_jump_pressed = pad_jump && !s_pad_jump_prev;
        s_pad_jump_prev = pad_jump;

        /* A pad cannot click into the viewport, so it must not need to.  It
         * ENGAGES on first movement and stays engaged for the rest of the Play
         * session -- staying engaged is what lets a released stick deliver its
         * zero, instead of the runtime holding the last non-zero input.  Until
         * the pad is first touched this is false and the block below behaves
         * exactly as it did before, so a user without a pad sees no change. */
        static bool s_pad_engaged = false;
        if (!play_active) s_pad_engaged = false;
        if (pad_fwd != 0.0f || pad_right != 0.0f ||
            pad_look_x != 0.0f || pad_look_y != 0.0f ||
            pad_jump || pad_sprint || pad_attack)
            s_pad_engaged = true;
        const bool pad_drives = play_active && s_pad_engaged;

        /* QA-only: the headless hooks drive this block without a mouse or a
         * pad.
         *
         * `effective_capture` is only true once the user has HOVERED AND
         * CLICKED the Game View to take the pointer, and `pad_drives` needs a
         * physical stick. Neither can happen in a headless capture, so the
         * whole camera-and-player block was being skipped and both
         * JCE_DBG_AUTOWALK and JCE_DBG_AUTOLOOK were writing into variables
         * nobody read. Two separate measurements came back with a best rigid
         * frame shift of exactly (0,0) before this was found -- the camera had
         * never moved, and each run still produced a full table of residuals
         * that read like a result.
         *
         * A hook that is gated behind a condition it cannot satisfy is worse
         * than a missing hook: the missing one fails loudly. */
        static int s_qa_drives = -1;
        if (s_qa_drives < 0)
            s_qa_drives = (getenv("JCE_DBG_AUTOLOOK") ||
                           getenv("JCE_DBG_AUTOWALK") ||
                           /* JCE_INPUT_REPLAY is the third QA driver of this
                            * view and was missing from this list.  A replayed
                            * session cannot click into the viewport, so
                            * effective_capture is false and pad_drives is
                            * false, and the whole block below -- including
                            * the only call that hands the Play runtime a
                            * keyboard -- was skipped.  The engine dutifully
                            * replayed all 1800 frames into an input object
                            * that Play never read.
                            *
                            * Which is the paragraph above, again: a hook
                            * gated behind a condition it cannot satisfy is
                            * worse than a missing hook.  Measured: the
                            * shipped runtime turned on this file and editor
                            * Play drove straight into the wall. */
                           getenv("JCE_INPUT_REPLAY")) ? 1 : 0;

        if (game_keyboard || pad_drives || s_qa_drives) {
            /* V toggles first/third-person follow camera (Play mode). */
            if (effective_capture && has_player_controller &&
                play_active && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
                s_third_person = !s_third_person;
                jce_editor_pstate_set_int("gameview.third_person",
                                          s_third_person ? 1 : 0);
            }
            /* Use SDL relative-motion accumulator (xrel/yrel) instead of
             * ImGui::IO::MouseDelta — the latter is always zero in
             * relative-mouse-mode because the absolute cursor is pinned. */
            float dx = 0.0f, dy = 0.0f;
            if (effective_capture)
                jce_editor_game_render_consume_mouse_delta(&dx, &dy);
            const float sensitivity = 0.0025f;
            /* Right stick looks.  Mouse delta is a displacement already, the
             * stick is a RATE, so it is the one that needs dt. */
            const float kPadLookRate = 2.5f;   /* rad/s at full deflection */
            float yaw   = dx * sensitivity + pad_look_x * kPadLookRate * dt;
            float pitch = -dy * sensitivity + pad_look_y * kPadLookRate * dt;

            /* QA-only: JCE_DBG_AUTOLOOK=YAW[,PITCH] turns the game camera at a
             * constant rate, in degrees per frame.
             *
             * Turning is the trigger the user names first ("视角位置、角度发生
             * 变化"), and it is a stronger one than walking: a pure rotation
             * re-fits every shadow cascade while the walked distance per frame
             * is a fraction of a metre. Autowalk alone measured a rigid frame
             * shift of exactly (0,0) -- the camera had not moved at all, so
             * there was no flicker for the metric to see and the null result
             * meant nothing.
             *
             * Per FRAME rather than per second, because the capture is taken
             * at a frame number: making the rate depend on dt would put the
             * pose back under the control of how fast the machine happened to
             * run, which is the same defect the day-clock pin exists to remove.
             *
             * Added, not overridden: a QA run has no mouse, so the sum is the
             * autolook value. */
            {
                static int   s_al = -1;
                static float s_aly = 0.0f, s_alp = 0.0f;
                if (s_al < 0) {
                    const char *e = getenv("JCE_DBG_AUTOLOOK");
                    s_al = 0;
                    if (e && e[0]) {
                        const char *c = strchr(e, ',');
                        s_aly = (float)atof(e) * 0.01745329f;
                        s_alp = c ? (float)atof(c + 1) * 0.01745329f : 0.0f;
                        s_al = 1;
                    }
                }
                if (s_al) { yaw += s_aly; pitch += s_alp; }
            }
            /* Script-owned relative camera (CK) consumes pointer delta via
             * Runtime. Never also rotate the editor's fallback camera. */
            if (!(play_active && s_script_mouse_look && !has_player_controller) &&
                (yaw != 0.0f || pitch != 0.0f)) {
                jce_camera_rotate(cam, yaw, pitch);
            }

            float speed_mult = 1.0f;
            if (effective_capture) {
                if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
                    ImGui::IsKeyDown(ImGuiKey_RightCtrl))
                    speed_mult = 5.0f;
                else if (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                         ImGui::IsKeyDown(ImGuiKey_RightShift))
                    speed_mult = 0.4f;
            }

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
                 * fallback when an action is missing or has no key bind.
                 * Keyboard only, and only while the viewport is captured —
                 * `kb` is what keeps WASD from leaking out of an unfocused
                 * Game View when the pad is what engaged this block. */
                const bool kb = effective_capture;
                auto act_down = [kb](const char *name, ImGuiKey fallback) {
                    if (!kb) return false;
                    int keys[4];
                    int n = jce_editor_input_action_keys(name, keys, 4);
                    if (n <= 0) return ImGui::IsKeyDown(fallback);
                    for (int i = 0; i < n; i++)
                        if (ImGui::IsKeyDown((ImGuiKey)keys[i])) return true;
                    return false;
                };
                auto act_pressed = [kb](const char *name, ImGuiKey fallback) {
                    if (!kb) return false;
                    int keys[4];
                    int n = jce_editor_input_action_keys(name, keys, 4);
                    if (n <= 0) return ImGui::IsKeyPressed(fallback, false);
                    for (int i = 0; i < n; i++)
                        if (ImGui::IsKeyPressed((ImGuiKey)keys[i], false)) return true;
                    return false;
                };

                /* Keyboard is a digital ±1 per axis; the pad is the signed
                 * analog value.  They ADD, so either device alone drives and
                 * both together simply agree or cancel — no mode, no toggle. */
                float fwd_amt   = (act_down("move_forward", ImGuiKey_W) ? 1.0f : 0.0f)
                                - (act_down("move_back",    ImGuiKey_S) ? 1.0f : 0.0f)
                                + pad_fwd;
                float right_amt = (act_down("move_right",   ImGuiKey_D) ? 1.0f : 0.0f)
                                - (act_down("move_left",    ImGuiKey_A) ? 1.0f : 0.0f)
                                + pad_right;

                float wx = fwd.x * fwd_amt + right.x * right_amt;
                float wz = fwd.z * fwd_amt + right.z * right_amt;
                /* CLAMP to unit, do not normalize TO unit: a half-deflected
                 * stick must stay half length so rt_drive_character walks at
                 * half speed (it scales by the vector length and only clamps
                 * above 1).  Keyboard is unaffected — its vectors are already
                 * length 1 (one key) or √2 (two keys, clamped to 1 exactly as
                 * the old normalize did). */
                float wlen = sqrtf(wx*wx + wz*wz);
                if (wlen > 1.0f) { wx /= wlen; wz /= wlen; }

                bool jump      = act_pressed("jump", ImGuiKey_Space) || pad_jump_pressed;
                bool jump_held = act_down("jump", ImGuiKey_Space)    || pad_jump;
                /* Hold sprint (Input Manager action; Ctrl fallback) — the
                 * authored CharacterController sprint_mult raises the speed
                 * so the locomotion blend tree / SM crosses into Run. */
                bool sprint =
                    act_down("sprint", ImGuiKey_LeftCtrl) ||
                    (effective_capture && ImGui::IsKeyDown(ImGuiKey_RightCtrl)) ||
                    pad_sprint;
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
                              (effective_capture &&
                               jce_editor_game_render_mouse_button(0)) ||
                              pad_attack;
                /* Direction AND magnitude — move_speed/jump arc still come
                 * from the scene's CharacterController component, but a
                 * part-deflected stick now arrives as a shorter vector. */
                /* QA-only: JCE_DBG_AUTOWALK=X,Z feeds a constant walk vector,
                 * so a headless capture can put the game camera IN MOTION.
                 *
                 * A defect reported as "it happens when the view moves" cannot
                 * be measured from a still frame, and in Play mode the camera
                 * only moves because the player does. Every other headless
                 * camera hook here drives the SCENE view's orbit rig, which is
                 * a different camera on a different code path -- which is why
                 * the one report that mentions Play mode is also the one that
                 * has never been reproduced.
                 *
                 * Overrides rather than adds: a QA run has no other input, and
                 * summing would make the value depend on whether a key happened
                 * to be down. No effect unless the env is set. */
                {
                    static int   s_aw = -1;
                    static float s_awx = 0.0f, s_awz = 0.0f;
                    if (s_aw < 0) {
                        const char *e = getenv("JCE_DBG_AUTOWALK");
                        s_aw = 0;
                        if (e && e[0]) {
                            const char *c = strchr(e, ',');
                            s_awx = (float)atof(e);
                            s_awz = c ? (float)atof(c + 1) : 0.0f;
                            s_aw = 1;
                        }
                    }
                    if (s_aw) { wx = s_awx; wz = s_awz; }
                }
                jce_editor_play_set_player_input(
                    wx, wz, jump, jump_held, sprint, attack,
                    gv_script_keyboard(kb));
                /* Facing + idle/walk/run clip are driven generically by the
                 * engine runtime (rt_drive_character), so it also works in the
                 * shipped game, not just here. */
            } else {
                /* No CharacterController in scene → classic free-fly cam. */
                float speed = 5.0f * speed_mult;
                float step  = speed * dt;
                if (effective_capture && !(play_active && s_script_mouse_look)) {
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
                /* The left stick flies the camera too, so a pad is not dead
                 * in a scene that has no CharacterController.  Analog: the
                 * step is scaled by deflection, not gated on it. */
                if (!play_active || !s_script_mouse_look) {
                    if (pad_fwd   != 0.0f) jce_camera_move_forward(cam, step * pad_fwd);
                    if (pad_right != 0.0f) jce_camera_move_right  (cam, step * pad_right);
                }

                /* AND HAND THE RUNTIME A KEYBOARD ANYWAY.
                 *
                 * jce_editor_play_set_player_input() is the ONLY writer of
                 * JceRuntimeInput::keyboard, and until now it was called only
                 * in the has_player branch above -- so in a scene with no
                 * CharacterController the runtime's keyboard stayed NULL and
                 * jce.is_key_down() answered false for every key, forever, in
                 * editor Play.  Not "no character to move": the BINDING was
                 * dead, for scripts that never wanted a character.
                 *
                 * A grid game is the ordinary case for that: its scripts read
                 * keys and move entities themselves.  The walk vector is zero
                 * here because there is nothing to walk; the keyboard is not. */
                /* A pointer-driven game still receives the keyboard when
                 * relative mouse capture is intentionally disabled. */
                jce_editor_play_set_player_input(
                    0.0f, 0.0f, false, false, false, false,
                    gv_script_keyboard(game_keyboard));
            }

            /* Only the focused Game View claims keyboard input.  A gamepad
             * alone must not swallow editor shortcuts in other panels. */
            if (game_keyboard) {
                ImGui::SetNextFrameWantCaptureKeyboard(true);
                ImGui::SetNextFrameWantCaptureMouse(true);
            }
        }

        if (!game_keyboard && !s_qa_drives && play_active)
            jce_editor_play_set_player_input(
                0.0f, 0.0f, false, false, false, false, nullptr);

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
    input_viewport.render_width = (float)canvas_w;
    input_viewport.render_height = (float)canvas_h;
    input_viewport.visible = viewport_work;
    input_viewport.hovered = hovered;
    input_viewport.captured =
        jce_editor_game_render_is_mouse_captured();
    const ImGuiIO &pointer_io = ImGui::GetIO();
    input_viewport.focused = !pointer_io.AppFocusLost &&
        (s_user_wants_capture || hovered || input_viewport.captured);
    input_viewport.pointer_valid = hovered && input_viewport.focused &&
        !input_viewport.captured;
    input_viewport.pointer_x = pointer_io.MousePos.x;
    input_viewport.pointer_y = pointer_io.MousePos.y;
    /* Mouse Look script mode routes RELATIVE deltas only while the cursor is
     * genuinely locked. Alt/Esc/focus-loss therefore cannot keep orbiting the
     * camera. Auto mode retains existing free-pointer UI semantics. */
    input_viewport.owner = play_active &&
        (!s_script_mouse_look || input_viewport.captured)
        ? JCE_EDITOR_GAME_INPUT_RUNTIME : JCE_EDITOR_GAME_INPUT_NONE;
    jce_editor_game_input_bridge_publish(
        jce_editor_game_input_bridge_shared(), &input_viewport);

    /* HUD overlay: capture state + hint. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (s_show_hints) {
    if (jce_editor_game_render_is_mouse_captured()) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(80, 220, 120, 230),
                    jce_editor_i18n("gameView.hud.flyCaptured"));
    } else if (s_user_wants_capture && capture_allowed) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(255, 220, 120, 230),
                    jce_editor_i18n("gameView.hud.altFree"));
    } else if (hovered && capture_allowed) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 4.0f),
                    IM_COL32(220, 220, 220, 200),
                    jce_editor_i18n("gameView.hud.clickToFly"));
    }
    if (play_active && s_script_mouse_look && hovered &&
        !jce_editor_game_render_is_mouse_captured()) {
        dl->AddText(ImVec2(image_min.x + 6.0f, image_min.y + 22.0f),
                    IM_COL32(220, 240, 255, 220),
                     jce_editor_i18n_or("gameView.hud.mouseLookControls",
                          "Mouse Look: click to focus | Alt: cursor | Esc: release")); // CK_0225_HINT_LOCALIZED
    }

    } /* s_show_hints: stats remains independently controlled. */
    if (s_show_stats) {
        const char *current = jce_renderer_running_backend_name();
        if (!current || !current[0]) current = "?";
        uint32_t target_w = 0, target_h = 0;
        const bool target_known = viewport_work && render_vw && render_vh &&
            jce_editor_game_render_get_committed_extent(&target_w, &target_h);
        char buf[256];
        if (target_known) {
            snprintf(buf, sizeof(buf), "%s: %s | %s %.1f | %s %ux%u | %s %ux%u | X %.1f%% Y %.1f%%",
                     jce_editor_i18n("game.renderer"), current,
                     jce_editor_i18n("gameView.stats.ui_fps"), ImGui::GetIO().Framerate,
                     jce_editor_i18n("gameView.stats.content"), render_vw, render_vh,
                     jce_editor_i18n("gameView.stats.target_3d"), target_w, target_h,
                     100.0 * target_w / render_vw, 100.0 * target_h / render_vh);
        } else {
            snprintf(buf, sizeof(buf), "%s: %s | %s %.1f | %s %ux%u | %s %s",
                     jce_editor_i18n("game.renderer"), current,
                     jce_editor_i18n("gameView.stats.ui_fps"), ImGui::GetIO().Framerate,
                     jce_editor_i18n("gameView.stats.content"), render_vw, render_vh,
                     jce_editor_i18n("gameView.stats.target_3d"),
                     jce_editor_i18n("profiler.value.unavailable"));
        }
        const float stats_y = fmaxf(image_min.y,
            image_max.y - ImGui::CalcTextSize(buf).y - 4.0f);
        dl->AddText(ImVec2(image_min.x + 6.0f, stats_y),
                    IM_COL32(160, 255, 160, 220), buf);
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_game_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW);
    if (!*vis) {
        jce_editor_game_render_observation_policy(
            static_cast<uint32_t>(JceEditorViewportPolicyReason::Hidden));
        jce_editor_viewport_policy_trace("editor.game.policy", JceEditorViewportPolicyReason::Hidden);
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    char title[256];
    snprintf(title, sizeof(title), "%s###game_view", jce_editor_i18n("Game"));
    if (ImGui::Begin(title, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_game_view_content();
    else {
        jce_editor_game_render_observation_policy(
            static_cast<uint32_t>(JceEditorViewportPolicyReason::Collapsed));
        jce_editor_viewport_policy_trace("editor.game.policy", JceEditorViewportPolicyReason::Collapsed);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
