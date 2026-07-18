/*
 * jce_single_instance.c  Cross-platform process single-instance lock.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_single_instance.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static HANDLE s_single_mutex = NULL;

/* Window-handle rendezvous for second-instance activation: the first
 * instance publishes its HWND into a named shared-memory section; a
 * second instance reads it back to restore/foreground/flash the window
 * instead of showing a modal.  Named alongside the mutex. */
typedef struct SiWndPayload {
    unsigned long long hwnd;   /* HWND widened for fixed layout */
    unsigned long      pid;
} SiWndPayload;

static HANDLE        s_wnd_mapping = NULL;
static SiWndPayload *s_wnd_view    = NULL;
static char          s_safe_name[128];

#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int  s_lock_fd = -1;
static char s_lock_path[260];

#endif

#define LOG_TAG "single_instance"

static void sanitize_name(const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;

    const char *src = (name && name[0]) ? name : "JCE";
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < out_size; i++) {
        const char c = src[i];
        const bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool is_digit = (c >= '0' && c <= '9');
        const bool is_ok_punct = (c == '_' || c == '-' || c == '.');
        out[j++] = (is_alpha || is_digit || is_ok_punct) ? c : '_';
    }

    if (j == 0 && out_size > 1) {
        out[0] = 'J';
        out[1] = '\0';
        return;
    }

    out[j] = '\0';
}

bool jce_single_instance_lock(const char *app_name)
{
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || \
    (defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS)
    /* Android / iOS: the OS enforces a single-instance app model; the
     * sandbox /tmp path may not support flock(), and a second instance
     * is architecturally impossible.
     * WASM: single-tab semantics enforced by the browser. */
    (void)app_name;
    return true;
#elif defined(_WIN32)
    if (s_single_mutex) return true;

    /* Local\ (per logon session), NOT Global\: two users / RDP sessions on
     * one machine should each get their own instance, and a Global mutex
     * owned by another user makes CreateMutex fail with ACCESS_DENIED —
     * the per-user scope is the desktop-app industry norm. */
    char mutex_name[196];
    sanitize_name(app_name, s_safe_name, sizeof(s_safe_name));
    snprintf(mutex_name, sizeof(mutex_name), "Local\\JCE_ENGINE_SINGLE_INSTANCE_%s", s_safe_name);

    HANDLE mtx = CreateMutexA(NULL, TRUE, mutex_name);
    if (!mtx) {
        LOG_ERROR(LOG_TAG, "CreateMutex failed for %s (err=%lu)",
                  mutex_name, (unsigned long)GetLastError());
        return false;
    }

    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS || err == ERROR_ACCESS_DENIED) {
        CloseHandle(mtx);
        LOG_INFO(LOG_TAG, "single-instance lock already held: %s", mutex_name);
        return false;
    }

    s_single_mutex = mtx;
    LOG_INFO(LOG_TAG, "single-instance lock acquired: %s", mutex_name);
    return true;
#else
    if (s_lock_fd >= 0) return true;

    char safe_name[128];
    sanitize_name(app_name, safe_name, sizeof(safe_name));
    snprintf(s_lock_path, sizeof(s_lock_path), "/tmp/jce_engine_single_instance_%s.lock", safe_name);

    int flags = O_CREAT | O_RDWR;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif

    int fd = open(s_lock_path, flags, 0666);
    if (fd < 0) {
        LOG_ERROR(LOG_TAG, "open lock file failed: %s (errno=%d)", s_lock_path, errno);
        s_lock_path[0] = '\0';
        return false;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        LOG_INFO(LOG_TAG, "single-instance lock already held: %s", s_lock_path);
        close(fd);
        s_lock_path[0] = '\0';
        return false;
    }

    s_lock_fd = fd;
    LOG_INFO(LOG_TAG, "single-instance lock acquired: %s", s_lock_path);
    return true;
#endif
}

void jce_single_instance_unlock(void)
{
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || \
    (defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS)
    return; /* no-op: lock was not acquired */
#elif defined(_WIN32)
    if (s_wnd_view)    { UnmapViewOfFile(s_wnd_view); s_wnd_view = NULL; }
    if (s_wnd_mapping) { CloseHandle(s_wnd_mapping);  s_wnd_mapping = NULL; }
    if (!s_single_mutex) return;

    ReleaseMutex(s_single_mutex);
    CloseHandle(s_single_mutex);
    s_single_mutex = NULL;
#else
    if (s_lock_fd < 0) return;

    flock(s_lock_fd, LOCK_UN);
    close(s_lock_fd);
    s_lock_fd = -1;
    s_lock_path[0] = '\0';
#endif
}

bool jce_single_instance_is_locked(void)
{
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || \
    (defined(__APPLE__) && defined(TARGET_OS_IOS) && TARGET_OS_IOS)
    return true; /* always considered locked (OS-enforced) */
#elif defined(_WIN32)
    return s_single_mutex != NULL;
#else
    return s_lock_fd >= 0;
#endif
}

#if defined(_WIN32)
static void si_wnd_mapping_name(char *out, size_t out_size)
{
    snprintf(out, out_size, "Local\\JCE_SI_WND_%s",
             s_safe_name[0] ? s_safe_name : "JCE");
}
#endif

void jce_single_instance_publish_window(void *native_window_handle)
{
#if defined(_WIN32)
    if (!s_single_mutex || !native_window_handle) return;

    if (!s_wnd_mapping) {
        char map_name[196];
        si_wnd_mapping_name(map_name, sizeof(map_name));
        s_wnd_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                           PAGE_READWRITE, 0,
                                           (DWORD)sizeof(SiWndPayload),
                                           map_name);
        if (!s_wnd_mapping) {
            LOG_WARN(LOG_TAG, "window mapping create failed (err=%lu)",
                     (unsigned long)GetLastError());
            return;
        }
        s_wnd_view = (SiWndPayload *)MapViewOfFile(
            s_wnd_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SiWndPayload));
        if (!s_wnd_view) {
            CloseHandle(s_wnd_mapping);
            s_wnd_mapping = NULL;
            LOG_WARN(LOG_TAG, "window mapping map failed (err=%lu)",
                     (unsigned long)GetLastError());
            return;
        }
    }

    s_wnd_view->pid  = GetCurrentProcessId();
    s_wnd_view->hwnd = (unsigned long long)(uintptr_t)native_window_handle;
    LOG_DEBUG(LOG_TAG, "published window handle %p for activation",
              native_window_handle);
#else
    /* POSIX second-instance activation would need a per-display protocol
     * (X11 _NET_ACTIVE_WINDOW / Wayland xdg-activation) — not wired yet;
     * the lock alone still guarantees single instance. */
    (void)native_window_handle;
#endif
}

bool jce_single_instance_activate_existing(void)
{
#if defined(_WIN32)
    char map_name[196];
    si_wnd_mapping_name(map_name, sizeof(map_name));

    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, map_name);
    if (!mapping) {
        LOG_INFO(LOG_TAG, "no published window to activate (%s)", map_name);
        return false;
    }
    SiWndPayload *view = (SiWndPayload *)MapViewOfFile(
        mapping, FILE_MAP_READ, 0, 0, sizeof(SiWndPayload));
    HWND hwnd = NULL;
    if (view) {
        hwnd = (HWND)(uintptr_t)view->hwnd;
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);

    if (!hwnd || !IsWindow(hwnd)) {
        LOG_INFO(LOG_TAG, "published window handle is stale");
        return false;
    }

    /* Restore + best-effort foreground + flash-until-focused: Windows
     * denies SetForegroundWindow to non-foreground processes by design;
     * FLASHW_TIMERNOFG keeps the taskbar button flashing until the user
     * brings the window forward, which is exactly the sanctioned
     * "an instance is already running" attention pattern. */
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);

    FLASHWINFO fi;
    fi.cbSize    = sizeof(fi);
    fi.hwnd      = hwnd;
    fi.dwFlags   = FLASHW_ALL | FLASHW_TIMERNOFG;
    fi.uCount    = 0;
    fi.dwTimeout = 0;
    FlashWindowEx(&fi);

    LOG_INFO(LOG_TAG, "activated existing instance window %p", (void *)hwnd);
    return true;
#else
    return false;
#endif
}
