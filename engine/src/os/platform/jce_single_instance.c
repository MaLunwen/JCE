/*
 * jce_single_instance.c  Cross-platform process single-instance lock.
 */

#include <jce/os/platform/jce_single_instance.h>

#include <jce/os/core/jce_log.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static HANDLE s_single_mutex = NULL;

#else
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
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
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    /* Android: the OS Activity Manager already enforces a single instance;
     * /tmp is unavailable, so a file-lock would always fail.
     * WASM: no persistent filesystem; single-tab semantics are enforced
     * by the browser. Skip the lock in both cases. */
    (void)app_name;
    return true;
#elif defined(_WIN32)
    if (s_single_mutex) return true;

    char safe_name[128];
    char mutex_name[196];
    sanitize_name(app_name, safe_name, sizeof(safe_name));
    snprintf(mutex_name, sizeof(mutex_name), "Global\\JCE_ENGINE_SINGLE_INSTANCE_%s", safe_name);

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
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    return; /* no-op: lock was not acquired */
#elif defined(_WIN32)
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
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    return true; /* always considered locked (OS-enforced) */
#elif defined(_WIN32)
    return s_single_mutex != NULL;
#else
    return s_lock_fd >= 0;
#endif
}
