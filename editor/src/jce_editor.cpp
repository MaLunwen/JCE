/*
 * jce_editor.cpp  Editor overlay module implementation.
 *
 * Manages the ImGui lifecycle, SDL3 input integration, and
 * coordinates the bgfx rendering backend.
 */

#include "jce_editor.h"
#include "jce_editor_alloc.h"
#include "jce_imgui_bgfx.h"
#include "jce_editor_layout.h"
#include "jce_editor_panels.h"
#include "jce_editor_style.h"
#include "jce_editor_i18n.h"
#include "jce_editor_state.h"
#include "gizmo/jce_gizmo.h"

#include <imgui.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include <jce/graphics/jce_views.h>
#include <jce/graphics/jce_postfx.h>
#include <jce/platform/jce_window.h>
#include <jce/core/jce_log.h>
#include <jce/core/pak_loader.h>
}

#define LOG_TAG "editor"

/* ── PostFX pipeline global (used by jce_panel_postfx) ─────────────── */

JcePostFXPipeline *g_editor_postfx = NULL;

/* ── Static state ──────────────────────────────────────────────────── */

static struct {
    bool        initialized;
    bool        active;         /* editor overlay visible? */
    uint64_t    last_time;      /* for delta-time computation */
    SDL_Cursor *cursors[ImGuiMouseCursor_COUNT];
    JceWindow  *window;         /* opaque engine window for text input API */
    bool        text_input_active;
    const PakArchive *pak;      /* stored for font rebuild */
    float       font_size;      /* current font size in pixels */
    float       pending_font_size; /* >0 means rebuild next frame */
    FILE       *frame_kpi_file;
    uint32_t    frame_kpi_index;
    uint32_t    frame_kpi_limit;
} s_editor;

/* ── SDL3 key mapping ──────────────────────────────────────────────── */

static ImGuiKey sdl_scancode_to_imgui_key(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_TAB:          return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT:         return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT:        return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP:           return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN:         return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP:       return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN:     return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME:         return ImGuiKey_Home;
    case SDL_SCANCODE_END:          return ImGuiKey_End;
    case SDL_SCANCODE_INSERT:       return ImGuiKey_Insert;
    case SDL_SCANCODE_DELETE:        return ImGuiKey_Delete;
    case SDL_SCANCODE_BACKSPACE:    return ImGuiKey_Backspace;
    case SDL_SCANCODE_SPACE:        return ImGuiKey_Space;
    case SDL_SCANCODE_RETURN:       return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE:       return ImGuiKey_Escape;
    case SDL_SCANCODE_APOSTROPHE:   return ImGuiKey_Apostrophe;
    case SDL_SCANCODE_COMMA:        return ImGuiKey_Comma;
    case SDL_SCANCODE_MINUS:        return ImGuiKey_Minus;
    case SDL_SCANCODE_PERIOD:       return ImGuiKey_Period;
    case SDL_SCANCODE_SLASH:        return ImGuiKey_Slash;
    case SDL_SCANCODE_SEMICOLON:    return ImGuiKey_Semicolon;
    case SDL_SCANCODE_EQUALS:       return ImGuiKey_Equal;
    case SDL_SCANCODE_LEFTBRACKET:  return ImGuiKey_LeftBracket;
    case SDL_SCANCODE_BACKSLASH:    return ImGuiKey_Backslash;
    case SDL_SCANCODE_RIGHTBRACKET: return ImGuiKey_RightBracket;
    case SDL_SCANCODE_GRAVE:        return ImGuiKey_GraveAccent;
    case SDL_SCANCODE_CAPSLOCK:     return ImGuiKey_CapsLock;
    case SDL_SCANCODE_SCROLLLOCK:   return ImGuiKey_ScrollLock;
    case SDL_SCANCODE_NUMLOCKCLEAR: return ImGuiKey_NumLock;
    case SDL_SCANCODE_PRINTSCREEN:  return ImGuiKey_PrintScreen;
    case SDL_SCANCODE_PAUSE:        return ImGuiKey_Pause;
    case SDL_SCANCODE_LCTRL:        return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_LSHIFT:       return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LALT:         return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_LGUI:         return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RCTRL:        return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_RSHIFT:       return ImGuiKey_RightShift;
    case SDL_SCANCODE_RALT:         return ImGuiKey_RightAlt;
    case SDL_SCANCODE_RGUI:         return ImGuiKey_RightSuper;
    case SDL_SCANCODE_KP_0:         return ImGuiKey_Keypad0;
    case SDL_SCANCODE_KP_1:         return ImGuiKey_Keypad1;
    case SDL_SCANCODE_KP_2:         return ImGuiKey_Keypad2;
    case SDL_SCANCODE_KP_3:         return ImGuiKey_Keypad3;
    case SDL_SCANCODE_KP_4:         return ImGuiKey_Keypad4;
    case SDL_SCANCODE_KP_5:         return ImGuiKey_Keypad5;
    case SDL_SCANCODE_KP_6:         return ImGuiKey_Keypad6;
    case SDL_SCANCODE_KP_7:         return ImGuiKey_Keypad7;
    case SDL_SCANCODE_KP_8:         return ImGuiKey_Keypad8;
    case SDL_SCANCODE_KP_9:         return ImGuiKey_Keypad9;
    case SDL_SCANCODE_KP_PERIOD:    return ImGuiKey_KeypadDecimal;
    case SDL_SCANCODE_KP_DIVIDE:    return ImGuiKey_KeypadDivide;
    case SDL_SCANCODE_KP_MULTIPLY:  return ImGuiKey_KeypadMultiply;
    case SDL_SCANCODE_KP_MINUS:     return ImGuiKey_KeypadSubtract;
    case SDL_SCANCODE_KP_PLUS:      return ImGuiKey_KeypadAdd;
    case SDL_SCANCODE_KP_ENTER:     return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_KP_EQUALS:    return ImGuiKey_KeypadEqual;
    case SDL_SCANCODE_A: return ImGuiKey_A; case SDL_SCANCODE_B: return ImGuiKey_B;
    case SDL_SCANCODE_C: return ImGuiKey_C; case SDL_SCANCODE_D: return ImGuiKey_D;
    case SDL_SCANCODE_E: return ImGuiKey_E; case SDL_SCANCODE_F: return ImGuiKey_F;
    case SDL_SCANCODE_G: return ImGuiKey_G; case SDL_SCANCODE_H: return ImGuiKey_H;
    case SDL_SCANCODE_I: return ImGuiKey_I; case SDL_SCANCODE_J: return ImGuiKey_J;
    case SDL_SCANCODE_K: return ImGuiKey_K; case SDL_SCANCODE_L: return ImGuiKey_L;
    case SDL_SCANCODE_M: return ImGuiKey_M; case SDL_SCANCODE_N: return ImGuiKey_N;
    case SDL_SCANCODE_O: return ImGuiKey_O; case SDL_SCANCODE_P: return ImGuiKey_P;
    case SDL_SCANCODE_Q: return ImGuiKey_Q; case SDL_SCANCODE_R: return ImGuiKey_R;
    case SDL_SCANCODE_S: return ImGuiKey_S; case SDL_SCANCODE_T: return ImGuiKey_T;
    case SDL_SCANCODE_U: return ImGuiKey_U; case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_W: return ImGuiKey_W; case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Y: return ImGuiKey_Y; case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_0: return ImGuiKey_0; case SDL_SCANCODE_1: return ImGuiKey_1;
    case SDL_SCANCODE_2: return ImGuiKey_2; case SDL_SCANCODE_3: return ImGuiKey_3;
    case SDL_SCANCODE_4: return ImGuiKey_4; case SDL_SCANCODE_5: return ImGuiKey_5;
    case SDL_SCANCODE_6: return ImGuiKey_6; case SDL_SCANCODE_7: return ImGuiKey_7;
    case SDL_SCANCODE_8: return ImGuiKey_8; case SDL_SCANCODE_9: return ImGuiKey_9;
    case SDL_SCANCODE_F1:  return ImGuiKey_F1;  case SDL_SCANCODE_F2:  return ImGuiKey_F2;
    case SDL_SCANCODE_F3:  return ImGuiKey_F3;  case SDL_SCANCODE_F4:  return ImGuiKey_F4;
    case SDL_SCANCODE_F5:  return ImGuiKey_F5;  case SDL_SCANCODE_F6:  return ImGuiKey_F6;
    case SDL_SCANCODE_F7:  return ImGuiKey_F7;  case SDL_SCANCODE_F8:  return ImGuiKey_F8;
    case SDL_SCANCODE_F9:  return ImGuiKey_F9;  case SDL_SCANCODE_F10: return ImGuiKey_F10;
    case SDL_SCANCODE_F11: return ImGuiKey_F11; case SDL_SCANCODE_F12: return ImGuiKey_F12;
    default: return ImGuiKey_None;
    }
}

static void update_key_modifiers(SDL_Keymod mods)
{
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl,  (mods & SDL_KMOD_CTRL)  != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & SDL_KMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt,   (mods & SDL_KMOD_ALT)   != 0);
    io.AddKeyEvent(ImGuiMod_Super, (mods & SDL_KMOD_GUI)   != 0);
}

/* ── Clipboard (SDL3 <-> ImGui) ────────────────────────────────────── */

static const char *clipboard_get(void *)
{
    return SDL_GetClipboardText();
}

static void clipboard_set(void *, const char *text)
{
    SDL_SetClipboardText(text);
}

/* ── Cursor mapping ────────────────────────────────────────────────── */

static void create_cursors(void)
{
    s_editor.cursors[ImGuiMouseCursor_Arrow]      = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    s_editor.cursors[ImGuiMouseCursor_TextInput]   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    s_editor.cursors[ImGuiMouseCursor_ResizeAll]   = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    s_editor.cursors[ImGuiMouseCursor_ResizeNS]    = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_ResizeEW]    = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_ResizeNESW]  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NESW_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_ResizeNWSE]  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
    s_editor.cursors[ImGuiMouseCursor_Hand]        = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    s_editor.cursors[ImGuiMouseCursor_NotAllowed]  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
}

static void update_cursor(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
        return;

    ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
    if (cursor == ImGuiMouseCursor_None || io.MouseDrawCursor) {
        SDL_HideCursor();
    } else {
        SDL_Cursor *c = s_editor.cursors[cursor]
            ? s_editor.cursors[cursor]
            : s_editor.cursors[ImGuiMouseCursor_Arrow];
        SDL_SetCursor(c);
        SDL_ShowCursor();
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_editor_init(const PakArchive *pak, JceWindow *window)
{
    if (s_editor.initialized) return true;

    /* Create ImGui context. */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    /* Let ImGui persist layout/docking state to imgui.ini. */
    io.IniFilename = "imgui.ini";

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

    /* Cursors. */
    create_cursors();

    /* bgfx renderer backend. */
    if (!jce_imgui_bgfx_init(pak, JCE_VIEW_IMGUI)) {
        ImGui::DestroyContext();
        return false;
    }

    /* Load custom font (after bgfx backend is ready). */
    jce_editor_load_fonts(pak, 24.0f);
    s_editor.pak       = pak;
    s_editor.font_size = 24.0f;

    /* i18n. */
    jce_editor_i18n_init(pak);

    /* Initialize editor state and panels. */
    jce_editor_state_init();
    jce_editor_panels_init();
    jce_gizmo_init();

    /* Set window icon from embedded PAK. */
    {
        const PakAsset *icon = pak_find(pak, "JCE_icon.png");
        if (icon) {
            void *buf = ED_MALLOC((size_t)icon->original_size);
            if (buf) {
                size_t sz = pak_decompress(icon, buf,
                                           (size_t)icon->original_size);
                if (sz > 0)
                    jce_window_set_icon(window, buf, sz);
                ED_FREE(buf);
            }
        } else {
            LOG_WARN(LOG_TAG, "JCE_icon.png not found in PAK");
        }
    }

    s_editor.last_time   = SDL_GetPerformanceCounter();
    s_editor.window      = window;
    s_editor.text_input_active = false;
    s_editor.frame_kpi_file = NULL;
    s_editor.frame_kpi_index = 0;
    s_editor.frame_kpi_limit = 0;

    const char *frame_kpi_path = SDL_getenv("JCE_KPI_FRAME_LOG");
    if (frame_kpi_path && frame_kpi_path[0]) {
        s_editor.frame_kpi_file = fopen(frame_kpi_path, "w");
        if (s_editor.frame_kpi_file) {
            const char *frame_count = SDL_getenv("JCE_KPI_FRAME_COUNT");
            if (frame_count && frame_count[0]) {
                const int parsed = SDL_atoi(frame_count);
                if (parsed > 0) {
                    s_editor.frame_kpi_limit = (uint32_t)parsed;
                }
            }
            fprintf(s_editor.frame_kpi_file, "frame_index,frame_ms\n");
            fflush(s_editor.frame_kpi_file);
            LOG_INFO(LOG_TAG, "frame KPI capture enabled -> %s", frame_kpi_path);
        } else {
            LOG_WARN(LOG_TAG, "failed to open frame KPI log: %s", frame_kpi_path);
        }
    }

    s_editor.active      = true;
    s_editor.initialized = true;

    LOG_SUCCESS(LOG_TAG, "editor initialized (F1 to toggle)");
    return true;
}

void jce_editor_shutdown(void)
{
    if (!s_editor.initialized) return;

    if (s_editor.text_input_active && s_editor.window) {
        if (!jce_window_stop_text_input(s_editor.window)) {
            LOG_WARN(LOG_TAG, "jce_window_stop_text_input failed during shutdown: %s",
                     SDL_GetError());
        }
        s_editor.text_input_active = false;
    }

    jce_gizmo_shutdown();
    jce_editor_panels_shutdown();
    jce_editor_state_shutdown();
    jce_editor_i18n_shutdown();
    jce_imgui_bgfx_shutdown();

    if (s_editor.frame_kpi_file) {
        fflush(s_editor.frame_kpi_file);
        fclose(s_editor.frame_kpi_file);
        s_editor.frame_kpi_file = NULL;
    }

    for (int i = 0; i < ImGuiMouseCursor_COUNT; i++) {
        if (s_editor.cursors[i]) {
            SDL_DestroyCursor(s_editor.cursors[i]);
            s_editor.cursors[i] = NULL;
        }
    }

    ImGui::DestroyContext();
    memset(&s_editor, 0, sizeof(s_editor));

    LOG_INFO(LOG_TAG, "editor shutdown");
}

bool jce_editor_process_event(const SDL_Event *event)
{
    if (!s_editor.initialized) return false;

    ImGuiIO &io = ImGui::GetIO();

    /* F1 toggles editor regardless of ImGui focus. */
    if (event->type == SDL_EVENT_KEY_DOWN &&
        event->key.scancode == SDL_SCANCODE_F1 &&
        !event->key.repeat) {
        jce_editor_toggle();
        return true;
    }

    if (!s_editor.active) return false;

    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
        io.AddMousePosEvent(event->motion.x, event->motion.y);
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        io.AddMouseWheelEvent(event->wheel.x, event->wheel.y);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int btn = -1;
        if (event->button.button == SDL_BUTTON_LEFT)   btn = 0;
        if (event->button.button == SDL_BUTTON_RIGHT)  btn = 1;
        if (event->button.button == SDL_BUTTON_MIDDLE) btn = 2;
        if (event->button.button == SDL_BUTTON_X1)     btn = 3;
        if (event->button.button == SDL_BUTTON_X2)     btn = 4;
        if (btn >= 0)
            io.AddMouseButtonEvent(btn,
                event->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    }

    case SDL_EVENT_TEXT_INPUT:
        io.AddInputCharactersUTF8(event->text.text);
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        update_key_modifiers(event->key.mod);
        ImGuiKey key = sdl_scancode_to_imgui_key(event->key.scancode);
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, event->type == SDL_EVENT_KEY_DOWN);
        break;
    }

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        io.AddFocusEvent(true);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
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
    uint64_t now  = SDL_GetPerformanceCounter();
    uint64_t freq = SDL_GetPerformanceFrequency();
    float dt = (float)((double)(now - s_editor.last_time) / (double)freq);
    if (dt <= 0.0f) dt = 1.0f / 60.0f;
    io.DeltaTime = dt;
    s_editor.last_time = now;

    if (s_editor.frame_kpi_file) {
        if (s_editor.frame_kpi_limit == 0 ||
            s_editor.frame_kpi_index < s_editor.frame_kpi_limit) {
            const double frame_ms = (double)dt * 1000.0;
            fprintf(s_editor.frame_kpi_file, "%u,%.3f\n",
                    s_editor.frame_kpi_index, frame_ms);
            s_editor.frame_kpi_index++;
            if ((s_editor.frame_kpi_index % 60u) == 0u) {
                fflush(s_editor.frame_kpi_file);
            }
        }
    }

    /* Setup bgfx view. */
    jce_imgui_bgfx_setup_view((uint16_t)w, (uint16_t)h);

    /* Deferred font rebuild — must happen before NewFrame(). */
    if (s_editor.pending_font_size > 0.0f) {
        float new_size = s_editor.pending_font_size;
        s_editor.pending_font_size = 0.0f;

        ImGui::GetIO().Fonts->Clear();
        if (jce_editor_load_fonts(s_editor.pak, new_size)) {
            s_editor.font_size = new_size;
            LOG_INFO(LOG_TAG, "font size changed to %.0f px", new_size);
        } else {
            /* Fallback: reload previous size. */
            jce_editor_load_fonts(s_editor.pak, s_editor.font_size);
            LOG_WARN(LOG_TAG, "font size change failed, reverted to %.0f px",
                     s_editor.font_size);
        }
    }

    /* Begin ImGui frame. */
    ImGui::NewFrame();

    /* Draw editor panels. */
    jce_editor_layout_draw();

    /* Render and submit to bgfx. */
    ImGui::Render();
    jce_imgui_bgfx_render_draw_data();

    /* SDL3 does not emit SDL_EVENT_TEXT_INPUT unless text input is started.
       Mirror Java behavior: toggle it based on ImGui's WantTextInput. */
    if (s_editor.window) {
        if (io.WantTextInput && !s_editor.text_input_active) {
            if (jce_window_start_text_input(s_editor.window)) {
                s_editor.text_input_active = true;
            } else {
                LOG_WARN(LOG_TAG, "jce_window_start_text_input failed: %s", SDL_GetError());
            }
        } else if (!io.WantTextInput && s_editor.text_input_active) {
            if (jce_window_stop_text_input(s_editor.window)) {
                s_editor.text_input_active = false;
            } else {
                LOG_WARN(LOG_TAG, "jce_window_stop_text_input failed: %s", SDL_GetError());
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
            LOG_WARN(LOG_TAG, "jce_window_stop_text_input failed while hiding editor: %s",
                     SDL_GetError());
        }
    }
    LOG_INFO(LOG_TAG, "editor %s", s_editor.active ? "shown" : "hidden");
}

float jce_editor_get_font_size(void)
{
    return s_editor.font_size;
}

bool jce_editor_set_font_size(float size)
{
    if (size < 12.0f) size = 12.0f;
    if (size > 48.0f) size = 48.0f;
    if (!s_editor.pak) return false;

    /* Skip if already at this size and no pending change. */
    if (size == s_editor.font_size && s_editor.pending_font_size <= 0.0f)
        return true;

    /* Defer the actual rebuild to the start of the next frame,
       before ImGui::NewFrame(). Rebuilding mid-frame causes
       ACCESS_VIOLATION since the font atlas is in use. */
    s_editor.pending_font_size = size;
    LOG_INFO(LOG_TAG, "font size change to %.0f px scheduled", size);
    return true;
}
