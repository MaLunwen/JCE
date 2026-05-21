/*
 * jce_editor.cpp  Editor overlay module implementation.
 *
 * Manages the ImGui lifecycle, SDL3 input integration, and
 * coordinates the bgfx rendering backend.
 */

#include "jce_editor.h"

#include <cstdlib>
#include <cmath>
#include <cfloat>

#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_timer.h>

#include "gizmo/jce_gizmo.h"
#include "jce_editor_alloc.h"
#include "jce_editor_config.h"
#include "jce_editor_i18n.h"
#include "ui/jce_editor_layout.h"
#include "ui/jce_editor_panels.h"
#include "jce_editor_state.h"
#include "ui/jce_editor_style.h"
#include "jce_project_settings.h"
#include <jce/ui/jce_imgui_renderer.h>
#include "jce_build_manager.h"
#include "jce_run_manager.h"
#include "panels/jce_panel_assets_thumb.h"
#include "scene/jce_editor_game_render.h"

extern "C" void jce_reflect_register_builtin(void);
extern "C" void jce_hotkeys_init(void);
extern "C" void jce_workspace_init(void);
extern "C" void jce_editor_prefs_load_and_apply(void);

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/platform/jce_clipboard.h>
#include <jce/os/platform/jce_cursor.h>
#include <jce/os/platform/jce_window.h>
#include <jce/renderer/jce_views.h>
}

#define LOG_TAG "editor"

/* ── Static state ──────────────────────────────────────────────────── */

static struct {
    bool        initialized;
    bool        active;         /* editor overlay visible? */
    uint64_t    last_time;      /* for delta-time computation */
    JceCursor  *cursors[ImGuiMouseCursor_COUNT];
    JceWindow  *window;         /* opaque engine window for text input API */
    bool        text_input_active;
    const JcePakArchive *pak;      /* stored for font rebuild */
    float       font_size;      /* current font size in pixels */

    char        frame_kpi_path[1024];
    uint32_t    frame_kpi_index;
    uint32_t    frame_kpi_limit;
} s_editor;

/* ── Key mapping (JCE → ImGui) ─────────────────────────────────────── */

static ImGuiKey jce_key_to_imgui_key(JceKey sc)
{
    switch (sc) {
    case JCE_KEY_TAB:          return ImGuiKey_Tab;
    case JCE_KEY_LEFT:         return ImGuiKey_LeftArrow;
    case JCE_KEY_RIGHT:        return ImGuiKey_RightArrow;
    case JCE_KEY_UP:           return ImGuiKey_UpArrow;
    case JCE_KEY_DOWN:         return ImGuiKey_DownArrow;
    case JCE_KEY_PAGEUP:       return ImGuiKey_PageUp;
    case JCE_KEY_PAGEDOWN:     return ImGuiKey_PageDown;
    case JCE_KEY_HOME:         return ImGuiKey_Home;
    case JCE_KEY_END:          return ImGuiKey_End;
    case JCE_KEY_INSERT:       return ImGuiKey_Insert;
    case JCE_KEY_DELETE:       return ImGuiKey_Delete;
    case JCE_KEY_BACKSPACE:    return ImGuiKey_Backspace;
    case JCE_KEY_SPACE:        return ImGuiKey_Space;
    case JCE_KEY_RETURN:       return ImGuiKey_Enter;
    case JCE_KEY_ESCAPE:       return ImGuiKey_Escape;
    case JCE_KEY_APOSTROPHE:   return ImGuiKey_Apostrophe;
    case JCE_KEY_COMMA:        return ImGuiKey_Comma;
    case JCE_KEY_MINUS:        return ImGuiKey_Minus;
    case JCE_KEY_PERIOD:       return ImGuiKey_Period;
    case JCE_KEY_SLASH:        return ImGuiKey_Slash;
    case JCE_KEY_SEMICOLON:    return ImGuiKey_Semicolon;
    case JCE_KEY_EQUALS:       return ImGuiKey_Equal;
    case JCE_KEY_LEFTBRACKET:  return ImGuiKey_LeftBracket;
    case JCE_KEY_BACKSLASH:    return ImGuiKey_Backslash;
    case JCE_KEY_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case JCE_KEY_GRAVE:        return ImGuiKey_GraveAccent;
    case JCE_KEY_CAPSLOCK:     return ImGuiKey_CapsLock;
    case JCE_KEY_SCROLLLOCK:   return ImGuiKey_ScrollLock;
    case JCE_KEY_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case JCE_KEY_PRINTSCREEN:  return ImGuiKey_PrintScreen;
    case JCE_KEY_PAUSE:        return ImGuiKey_Pause;
    case JCE_KEY_LCTRL:        return ImGuiKey_LeftCtrl;
    case JCE_KEY_LSHIFT:       return ImGuiKey_LeftShift;
    case JCE_KEY_LALT:         return ImGuiKey_LeftAlt;
    case JCE_KEY_LGUI:         return ImGuiKey_LeftSuper;
    case JCE_KEY_RCTRL:        return ImGuiKey_RightCtrl;
    case JCE_KEY_RSHIFT:       return ImGuiKey_RightShift;
    case JCE_KEY_RALT:         return ImGuiKey_RightAlt;
    case JCE_KEY_RGUI:         return ImGuiKey_RightSuper;
    case JCE_KEY_KP_0:         return ImGuiKey_Keypad0;
    case JCE_KEY_KP_1:         return ImGuiKey_Keypad1;
    case JCE_KEY_KP_2:         return ImGuiKey_Keypad2;
    case JCE_KEY_KP_3:         return ImGuiKey_Keypad3;
    case JCE_KEY_KP_4:         return ImGuiKey_Keypad4;
    case JCE_KEY_KP_5:         return ImGuiKey_Keypad5;
    case JCE_KEY_KP_6:         return ImGuiKey_Keypad6;
    case JCE_KEY_KP_7:         return ImGuiKey_Keypad7;
    case JCE_KEY_KP_8:         return ImGuiKey_Keypad8;
    case JCE_KEY_KP_9:         return ImGuiKey_Keypad9;
    case JCE_KEY_KP_PERIOD:    return ImGuiKey_KeypadDecimal;
    case JCE_KEY_KP_DIVIDE:    return ImGuiKey_KeypadDivide;
    case JCE_KEY_KP_MULTIPLY:  return ImGuiKey_KeypadMultiply;
    case JCE_KEY_KP_MINUS:     return ImGuiKey_KeypadSubtract;
    case JCE_KEY_KP_PLUS:      return ImGuiKey_KeypadAdd;
    case JCE_KEY_KP_ENTER:     return ImGuiKey_KeypadEnter;
    case JCE_KEY_KP_EQUALS:    return ImGuiKey_KeypadEqual;
    case JCE_KEY_A: return ImGuiKey_A; case JCE_KEY_B: return ImGuiKey_B;
    case JCE_KEY_C: return ImGuiKey_C; case JCE_KEY_D: return ImGuiKey_D;
    case JCE_KEY_E: return ImGuiKey_E; case JCE_KEY_F: return ImGuiKey_F;
    case JCE_KEY_G: return ImGuiKey_G; case JCE_KEY_H: return ImGuiKey_H;
    case JCE_KEY_I: return ImGuiKey_I; case JCE_KEY_J: return ImGuiKey_J;
    case JCE_KEY_K: return ImGuiKey_K; case JCE_KEY_L: return ImGuiKey_L;
    case JCE_KEY_M: return ImGuiKey_M; case JCE_KEY_N: return ImGuiKey_N;
    case JCE_KEY_O: return ImGuiKey_O; case JCE_KEY_P: return ImGuiKey_P;
    case JCE_KEY_Q: return ImGuiKey_Q; case JCE_KEY_R: return ImGuiKey_R;
    case JCE_KEY_S: return ImGuiKey_S; case JCE_KEY_T: return ImGuiKey_T;
    case JCE_KEY_U: return ImGuiKey_U; case JCE_KEY_V: return ImGuiKey_V;
    case JCE_KEY_W: return ImGuiKey_W; case JCE_KEY_X: return ImGuiKey_X;
    case JCE_KEY_Y: return ImGuiKey_Y; case JCE_KEY_Z: return ImGuiKey_Z;
    case JCE_KEY_0: return ImGuiKey_0; case JCE_KEY_1: return ImGuiKey_1;
    case JCE_KEY_2: return ImGuiKey_2; case JCE_KEY_3: return ImGuiKey_3;
    case JCE_KEY_4: return ImGuiKey_4; case JCE_KEY_5: return ImGuiKey_5;
    case JCE_KEY_6: return ImGuiKey_6; case JCE_KEY_7: return ImGuiKey_7;
    case JCE_KEY_8: return ImGuiKey_8; case JCE_KEY_9: return ImGuiKey_9;
    case JCE_KEY_F1:  return ImGuiKey_F1;  case JCE_KEY_F2:  return ImGuiKey_F2;
    case JCE_KEY_F3:  return ImGuiKey_F3;  case JCE_KEY_F4:  return ImGuiKey_F4;
    case JCE_KEY_F5:  return ImGuiKey_F5;  case JCE_KEY_F6:  return ImGuiKey_F6;
    case JCE_KEY_F7:  return ImGuiKey_F7;  case JCE_KEY_F8:  return ImGuiKey_F8;
    case JCE_KEY_F9:  return ImGuiKey_F9;  case JCE_KEY_F10: return ImGuiKey_F10;
    case JCE_KEY_F11: return ImGuiKey_F11; case JCE_KEY_F12: return ImGuiKey_F12;
    default: return ImGuiKey_None;
    }
}

static void update_key_modifiers(uint16_t mods)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,  (mods & JCE_KMOD_CTRL)  != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & JCE_KMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt,   (mods & JCE_KMOD_ALT)   != 0);
    io.AddKeyEvent(ImGuiMod_Super, (mods & JCE_KMOD_GUI)   != 0);
}

/* ── Clipboard (JCE <-> ImGui) ────────────────────────────────────── */

static const char *clipboard_get(void *)
{
    return jce_clipboard_get_text();
}

static void clipboard_set(void *, const char *text)
{
    jce_clipboard_set_text(text);
}

/* ── Cursor mapping ────────────────────────────────────────────────── */

static void create_cursors(void)
{
    s_editor.cursors[ImGuiMouseCursor_Arrow]      = jce_cursor_create_system(JCE_CURSOR_ARROW);
    s_editor.cursors[ImGuiMouseCursor_TextInput]  = jce_cursor_create_system(JCE_CURSOR_TEXT);
    s_editor.cursors[ImGuiMouseCursor_ResizeAll]  = jce_cursor_create_system(JCE_CURSOR_MOVE);
    s_editor.cursors[ImGuiMouseCursor_ResizeNS]   = jce_cursor_create_system(JCE_CURSOR_NS_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_ResizeEW]   = jce_cursor_create_system(JCE_CURSOR_EW_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_ResizeNESW] = jce_cursor_create_system(JCE_CURSOR_NESW_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_ResizeNWSE] = jce_cursor_create_system(JCE_CURSOR_NWSE_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_Hand]       = jce_cursor_create_system(JCE_CURSOR_HAND);
    s_editor.cursors[ImGuiMouseCursor_NotAllowed] = jce_cursor_create_system(JCE_CURSOR_NOT_ALLOWED);
}

static void update_cursor(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return;

    ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
    if (cursor == ImGuiMouseCursor_None || io.MouseDrawCursor) {
        jce_cursor_show(false);
    } else {
        JceCursor *c = s_editor.cursors[cursor]
            ? s_editor.cursors[cursor]
            : s_editor.cursors[ImGuiMouseCursor_Arrow];
        jce_cursor_set(c);
        jce_cursor_show(true);
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_editor_init(const JcePakArchive *pak, JceWindow *window)
{
    if (s_editor.initialized) return true;

    /* Create ImGui context.
     * Route ImGui's IM_ALLOC/IM_FREE through the editor allocator (mimalloc-
     * backed, Tracy-tracked) BEFORE CreateContext so the context itself and
     * every subsequent ImGui allocation (including FontDataOwnedByAtlas
     * buffers freed by ImGui) flow through ED_MALLOC/ED_FREE. */
    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(
        [](size_t sz, void * /*ud*/) -> void * { return ED_MALLOC(sz); },
        [](void *p, void * /*ud*/) { ED_FREE(p); },
        nullptr);
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDragClickToInputText = true;  /* single-click on DragFloat enters text-input mode */

    /* Ensure .jce config dir exists, then let ImGui persist layout/docking state there. */
    jce_editor_config_ensure_dir();
    io.IniFilename = ".jce/imgui.ini";

    /* Clipboard. */
    io.SetClipboardTextFn = clipboard_set;
    io.GetClipboardTextFn = clipboard_get;

    /* Display size (updated each frame). */
    uint32_t w, h;
    jce_window_get_size(window, &w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    /* Apply JCE dark theme and load custom font. */
    jce_editor_setup_style();

    /* Load editor config once; reuse for theme, font, and scene restore. */
    JceEditorConfig ecfg = {};
    const bool have_ecfg = jce_editor_config_load(&ecfg);

    /* Apply persisted theme from editor config. setup_style applies the
       default dark theme; this overrides if the user previously chose
       Light/SSMS so the saved preference takes effect at startup. */
    if (have_ecfg) {
        int t = JCE_THEME_DARK;
        if      (jce_strcasecmp(ecfg.theme, "Light") == 0) t = JCE_THEME_LIGHT;
        else if (jce_strcasecmp(ecfg.theme, "SSMS")  == 0) t = JCE_THEME_SSMS;
        else if (jce_strcasecmp(ecfg.theme, "Blue")  == 0) t = JCE_THEME_SSMS;
        jce_editor_apply_theme(t);
    }

    /* Cursors. */
    create_cursors();

    /* bgfx renderer backend. */
    if (!jce_imgui_renderer_init(pak, JCE_VIEW_IMGUI)) {
        ImGui::DestroyContext();
        return false;
    }

    /* Load custom font (after bgfx backend is ready). */
    {
        float fs = (ecfg.font_size >= 12 && ecfg.font_size <= 48)
                       ? (float)ecfg.font_size : 14.0f;
        jce_editor_load_fonts(pak, fs,
                              ecfg.font_en_path, ecfg.font_zh_path);
        /* Use ImGui 1.92 FontScaleMain (not legacy FontGlobalScale)
         * so the value composes correctly with style.FontSizeBase. */
        ImGui::GetStyle().FontScaleMain = (ecfg.ui_scale > 0.1f) ? ecfg.ui_scale : 1.0f;
        ImGui::GetIO().FontGlobalScale = 1.0f;
        s_editor.pak       = pak;
        s_editor.font_size = fs;
    }

    /* i18n. */
    jce_editor_i18n_init(pak);

    /* Initialize editor state and panels. */
    jce_editor_state_init();
    jce_editor_panels_init();
    jce_run_manager_init();
    jce_build_manager_init();
    jce_reflect_register_builtin();
    jce_hotkeys_init();
    jce_workspace_init();
    jce_editor_prefs_load_and_apply();
    jce_gizmo_init();

    /* Pre-warm project settings cache so panels can use
     * jce_project_settings_current() without a per-panel disk read. */
    {
        JceProjectSettings ps;
        jce_project_settings_load(&ps);
        (void)ps; /* result already cached inside jce_project_settings_load */
    }

    /* Set window icon from embedded PAK. */
    {
        const JcePakAsset *icon = jce_pak_find(pak, "JCE_icon.png");
        if (icon) {
            void *buf = ED_MALLOC((size_t)icon->original_size);
            if (buf) {
                size_t sz = jce_pak_decompress(icon, buf,
                                           (size_t)icon->original_size);
                if (sz > 0)
                    jce_window_set_icon(window, buf, sz);
                ED_FREE(buf);
            }
        } else {
            LOG_WARN(LOG_TAG, "JCE_icon.png not found in PAK");
        }
    }

    s_editor.last_time   = jce_time_perf_counter();
    s_editor.window      = window;
    s_editor.text_input_active = false;
    s_editor.frame_kpi_path[0] = '\0';
    s_editor.frame_kpi_index = 0;
    s_editor.frame_kpi_limit = 0;

    const char *frame_kpi_path = getenv("JCE_KPI_FRAME_LOG");
    if (frame_kpi_path && frame_kpi_path[0]) {
        /* Truncate then write CSV header. */
        static const char hdr[] = "frame_index,frame_ms\n";
        if (jce_fs_host_write_all(frame_kpi_path, hdr, sizeof(hdr) - 1)) {
            jce_strlcpy(s_editor.frame_kpi_path, frame_kpi_path,
                        sizeof(s_editor.frame_kpi_path));
            const char *frame_count = getenv("JCE_KPI_FRAME_COUNT");
            if (frame_count && frame_count[0]) {
                const int parsed = atoi(frame_count);
                if (parsed > 0) s_editor.frame_kpi_limit = (uint32_t)parsed;
            }
            LOG_INFO(LOG_TAG, "frame KPI capture enabled -> %s", frame_kpi_path);
        } else {
            LOG_WARN(LOG_TAG, "failed to open frame KPI log: %s", frame_kpi_path);
        }
    }

    s_editor.active      = true;
    s_editor.initialized = true;

    /* Auto-restore last opened scene. */
    if (have_ecfg
        && ecfg.last_scene_path[0] != '\0'
        && jce_fs_host_exists_file(ecfg.last_scene_path)) {
        (void)jce_state_load_scene_file(ecfg.last_scene_path);
    }

    return true;
}

void jce_editor_shutdown(void)
{
    if (!s_editor.initialized) return;

    if (s_editor.text_input_active && s_editor.window) {
        if (!jce_window_stop_text_input(s_editor.window)) {
            LOG_WARN(LOG_TAG, "jce_window_stop_text_input failed during shutdown");
        }
        s_editor.text_input_active = false;
    }

    jce_gizmo_shutdown();
    jce_build_manager_shutdown();
    jce_run_manager_shutdown();
    jce_editor_panels_shutdown();
    jce_thumb_shutdown();
    jce_editor_state_shutdown();
    jce_editor_i18n_shutdown();
    jce_imgui_renderer_shutdown();

    s_editor.frame_kpi_path[0] = '\0';

    for (int i = 0; i < ImGuiMouseCursor_COUNT; i++) {
        if (s_editor.cursors[i]) {
            jce_cursor_destroy(s_editor.cursors[i]);
            s_editor.cursors[i] = NULL;
        }
    }

    ImGui::DestroyContext();
    memset(&s_editor, 0, sizeof(s_editor));

    LOG_INFO(LOG_TAG, "editor shutdown");
}

bool jce_editor_process_event(const JceEvent *event)
{
    if (!s_editor.initialized) return false;

    ImGuiIO &io = ImGui::GetIO();

    if (!s_editor.active) return false;

    /* When the Game View has captured the cursor (FPS-look mode using SDL
     * relative-mouse-mode), the OS still emits absolute MOUSE_MOTION events
     * pinned to the warp point.  Forwarding them to ImGui makes its virtual
     * cursor hover/click whatever panel sits behind that warp point — the
     * user sees other panels react while playing.  Gate ImGui pointer
     * events with the capture flag (Unity ImGUIEvents-during-Play model)
     * and still forward xrel/yrel to the FPS accumulator. */
    const bool game_capture = jce_editor_game_render_is_mouse_captured();

    switch (event->type) {
    case JCE_EVENT_MOUSE_MOTION:
        if (game_capture) {
            /* Tell ImGui there is no mouse — official sentinel. */
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        } else {
            io.AddMousePosEvent(event->motion.x, event->motion.y);
        }
        /* Always forward relative deltas: when SDL relative-mouse-mode is
         * on, the absolute position is pinned and ImGui's MouseDelta is
         * zero, so we need xrel/yrel to drive FPS look. */
        jce_editor_game_render_push_mouse_delta(event->motion.xrel,
                                                 event->motion.yrel);
        break;

    case JCE_EVENT_MOUSE_WHEEL: {
        if (game_capture) break; /* wheel belongs to the game while captured */
        /* Distinguish touchpad from mouse-wheel by checking whether wheel.x
           is fractional. Windows Precision Touchpad reports values in the
           0.05..0.95 range per event; classic mouse wheels (incl. tilt)
           report integer ±1.0. Only the touchpad case is flipped — the
           mouse wheel is handled elsewhere via invert_scroll_zoom. */
        float wx = event->wheel.x;
        float frac = fabsf(wx) - floorf(fabsf(wx));
        bool is_touchpad_h = (wx != 0.0f) && (frac > 0.01f && frac < 0.99f);
        if (is_touchpad_h && jce_editor_pref_touchpad_h_invert) {
            wx = -wx;
        }
        io.AddMouseWheelEvent(wx, event->wheel.y);
        break;
    }

    case JCE_EVENT_MOUSE_BUTTON_DOWN:
    case JCE_EVENT_MOUSE_BUTTON_UP: {
        if (game_capture) break; /* clicks belong to the game while captured */
        int btn = -1;
        switch (event->button.button) {
        case JCE_MOUSE_BUTTON_LEFT:   btn = 0; break;
        case JCE_MOUSE_BUTTON_RIGHT:  btn = 1; break;
        case JCE_MOUSE_BUTTON_MIDDLE: btn = 2; break;
        case JCE_MOUSE_BUTTON_X1:     btn = 3; break;
        case JCE_MOUSE_BUTTON_X2:     btn = 4; break;
        default: break;
        }
        if (btn >= 0)
            io.AddMouseButtonEvent(btn,
                event->type == JCE_EVENT_MOUSE_BUTTON_DOWN);
        break;
    }

    case JCE_EVENT_TEXT_INPUT:
        io.AddInputCharactersUTF8(event->text.text);
        break;

    case JCE_EVENT_KEY_DOWN:
    case JCE_EVENT_KEY_UP: {
        update_key_modifiers(event->key.mod);
        ImGuiKey key = jce_key_to_imgui_key(event->key.scancode);
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, event->type == JCE_EVENT_KEY_DOWN);
        break;
    }

    case JCE_EVENT_WINDOW_FOCUS_GAINED:
        io.AddFocusEvent(true);
        break;
    case JCE_EVENT_WINDOW_FOCUS_LOST:
        io.AddFocusEvent(false);
        break;

    default:
        break;
    }

    /* If ImGui wants keyboard/mouse, signal that the event is consumed. */
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}

void jce_editor_update(JceWindow *window)
{
    if (!s_editor.initialized || !s_editor.active) return;

    ImGuiIO &io = ImGui::GetIO();

    /* Update display size. */
    uint32_t w, h;
    jce_window_get_size(window, &w, &h);
    io.DisplaySize = ImVec2((float)w, (float)h);

    /* Delta time. */
    uint64_t now  = jce_time_perf_counter();
    uint64_t freq = jce_time_perf_freq();
    float dt = (float)((double)(now - s_editor.last_time) / (double)freq);
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    io.DeltaTime = dt;
    s_editor.last_time = now;

    if (s_editor.frame_kpi_path[0]) {
        if (s_editor.frame_kpi_limit == 0 ||
            s_editor.frame_kpi_index < s_editor.frame_kpi_limit) {
            const double frame_ms = (double)dt * 1000.0;
            char kpi_line[64];
            int kpi_len = snprintf(kpi_line, sizeof(kpi_line), "%u,%.3f\n",
                                   s_editor.frame_kpi_index, frame_ms);
            if (kpi_len > 0)
                jce_fs_host_append(s_editor.frame_kpi_path, kpi_line, (size_t)kpi_len);
            s_editor.frame_kpi_index++;
        }
    }

    /* Setup bgfx view. */
    jce_imgui_renderer_setup_view((uint16_t)w, (uint16_t)h);

    /* Sync OS window title with scene name + dirty marker. Only push to
       SDL when the composed string actually changes — avoids per-frame
       allocation / windowing churn. */
    {
        static char s_last_title[256] = {0};
        char        title[256];
        const char *spath = jce_state_get_current_scene_path();
        const bool  dirty = jce_state_is_scene_modified();

        const char *base = (spath && spath[0]) ? spath : NULL;
        const char *name = base;
        if (base) {
            const char *slash = strrchr(base, '/');
            const char *bslash = strrchr(base, '\\');
            if (bslash && (!slash || bslash > slash)) slash = bslash;
            if (slash && slash[1]) name = slash + 1;
        }

        if (name) {
            snprintf(title, sizeof(title), "JCE Editor %s %s%s",
                     "\xe2\x80\x94", name, dirty ? " *" : "");
        } else {
            snprintf(title, sizeof(title), "JCE Editor%s",
                     dirty ? " *" : "");
        }

        if (strcmp(title, s_last_title) != 0) {
            jce_window_set_title(window, title);
            snprintf(s_last_title, sizeof(s_last_title), "%s", title);
        }
    }

    /* Apply any pending font reload BEFORE starting the next frame. */
    jce_editor_apply_pending_font_reload();

    /* Begin ImGui frame. */
    ImGui::NewFrame();

    /* Horizontal scroll direction is decided per-event in the wheel
       handler above based on touchpad vs mouse-wheel detection
       (jce_editor_pref_touchpad_h_invert). No global flip here. */

    /* Draw editor panels. */
    jce_editor_layout_draw();

    /* Render and submit to bgfx. */
    ImGui::Render();
    jce_imgui_renderer_draw();

    /* Decode a small slice of pending asset thumbnails this frame.
       Budget chosen so a folder of ~100 images warms in ~2 s without a
       perceptible spike on the editor's old-hardware target. */
    jce_thumb_pump(2);

    /* The platform window does not emit text-input events unless text
       input is started.  Mirror ImGui's WantTextInput here. */
    if (s_editor.window) {
        if (io.WantTextInput && !s_editor.text_input_active) {
            if (jce_window_start_text_input(s_editor.window)) {
                s_editor.text_input_active = true;
            } else {
                LOG_WARN(LOG_TAG, "jce_window_start_text_input failed");
            }
        } else if (!io.WantTextInput && s_editor.text_input_active) {
            if (jce_window_stop_text_input(s_editor.window)) {
                s_editor.text_input_active = false;
            } else {
                LOG_WARN(LOG_TAG, "jce_window_stop_text_input failed");
            }
        }
    }

    /* Update mouse cursor. */
    update_cursor();
}

bool jce_editor_is_active(void)
{
    return s_editor.active;
}

void jce_editor_toggle(void)
{
    s_editor.active = !s_editor.active;
    if (!s_editor.active && s_editor.text_input_active && s_editor.window) {
        if (jce_window_stop_text_input(s_editor.window)) {
            s_editor.text_input_active = false;
        } else {
            LOG_WARN(LOG_TAG, "jce_window_stop_text_input failed while hiding editor");
        }
    }
    LOG_INFO(LOG_TAG, "editor %s", s_editor.active ? "shown" : "hidden");
}

void jce_editor_toggle_fullscreen(void)
{
    if (!s_editor.initialized || !s_editor.window)
        return;

    jce_window_toggle_fullscreen(s_editor.window);
}

float jce_editor_get_font_size(void)
{
    return s_editor.font_size;
}

bool jce_editor_set_font_size(float size)
{
    if (size < 12.0f) size = 12.0f;
    if (size > 48.0f) size = 48.0f;
    s_editor.font_size = size;
    return true;
}

const JcePakArchive *jce_editor_get_pak(void)
{
    return s_editor.pak;
}
