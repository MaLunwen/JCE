/*
 * jce_window_modal_loop.c  Modal-loop tick hook implementation.
 *
 * Windows implementation uses SDL_SetWindowsMessageHook to detect
 * WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE and pumps the user callback via
 * a Win32 SetTimer (~60 fps) that fires from inside the modal loop.
 *
 * Other platforms get a no-op stub — the OS doesn't have a comparable
 * stop-the-world modal loop and the normal frame loop suffices.
 *
 * Layer: OS / Platform.
 */

#include <jce/os/platform/jce_window_modal_loop.h>

#include <SDL3/SDL.h>

static JceModalTickFn g_cb;
static void          *g_user;

#ifdef SDL_PLATFORM_WINDOWS

#include <windows.h>

#define JCE_MODAL_TIMER_ID   1
#define JCE_MODAL_TIMER_MS  16   /* ~60 fps */

static void CALLBACK jce_modal_timer_proc(HWND hwnd, UINT msg,
                                          UINT_PTR id, DWORD time)
{
    (void)hwnd; (void)msg; (void)id; (void)time;
    if (g_cb) g_cb(g_user);
}

static bool SDLCALL jce_win32_msg_hook(void *userdata, MSG *msg)
{
    (void)userdata;

    if (msg->message == WM_ENTERSIZEMOVE) {
        SetTimer(msg->hwnd, JCE_MODAL_TIMER_ID,
                 JCE_MODAL_TIMER_MS, jce_modal_timer_proc);
    } else if (msg->message == WM_EXITSIZEMOVE) {
        KillTimer(msg->hwnd, JCE_MODAL_TIMER_ID);
    }

    return true;   /* let SDL process the message */
}

void jce_window_install_modal_tick(JceModalTickFn cb, void *user)
{
    g_cb   = cb;
    g_user = user;
    SDL_SetWindowsMessageHook(jce_win32_msg_hook, NULL);
}

void jce_window_uninstall_modal_tick(void)
{
    SDL_SetWindowsMessageHook(NULL, NULL);
    g_cb   = NULL;
    g_user = NULL;
}

#else /* non-Windows */

void jce_window_install_modal_tick(JceModalTickFn cb, void *user)
{
    (void)cb; (void)user;
    /* No modal loop on this platform — the regular frame loop handles
       all window events without needing a tick hook. */
}

void jce_window_uninstall_modal_tick(void)
{
    /* Nothing to do. */
}

#endif /* SDL_PLATFORM_WINDOWS */
