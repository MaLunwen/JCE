/* Private current-executable adapter, compiled only by its small Core owner.
 * Include before libc headers: strict C99 needs POSIX.1-2001 for readlink.
 * Preserve an explicitly selected feature level, as the TCP adapter does. */
#ifndef JCE_EXECUTABLE_PATH_NATIVE_INTERNAL_H
#define JCE_EXECUTABLE_PATH_NATIVE_INTERNAL_H
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && \
    !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#define _POSIX_C_SOURCE 200112L
#endif

#include <jce/os/core/jce_alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if JCE_PLATFORM_WEB || JCE_PLATFORM_MOBILE || JCE_PLATFORM_TVOS
/* Deliberately no native-image query on these hosts. */
#elif JCE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
/* XP cannot request WC_ERR_INVALID_CHARS. Validate UTF-16 before conversion
 * so flags=0 cannot admit an unpaired surrogate as invalid UTF-8. */
static inline bool jce_fs_executable_path_utf16_valid_internal(const WCHAR *wide, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        const uint32_t unit = wide[i];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (++i >= size || wide[i] < 0xDC00 || wide[i] > 0xDFFF)
                return false;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;
        }
    }
    return true;
}
#endif
#elif JCE_PLATFORM_LINUX
#include <unistd.h>

/* Linux filenames need not be UTF-8. Refuse invalid encoding rather than
 * returning bytes that violate the public UTF-8 contract. No directories
 * are searched. */
static inline bool jce_fs_executable_path_utf8_internal(const char *path, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const unsigned char first = (unsigned char)path[offset++];
        if (first < 0x80) continue;
        uint32_t code, minimum;
        unsigned remaining;
        if (first >= 0xC2 && first <= 0xDF) {
            code = first & 0x1F; minimum = 0x80; remaining = 1;
        } else if (first >= 0xE0 && first <= 0xEF) {
            code = first & 0x0F; minimum = 0x800; remaining = 2;
        } else if (first >= 0xF0 && first <= 0xF4) {
            code = first & 0x07; minimum = 0x10000; remaining = 3;
        } else return false;
        if (remaining > size - offset) return false;
        while (remaining--) {
            const unsigned char next = (unsigned char)path[offset++];
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
            return false;
    }
    return true;
}
#endif

static inline bool jce_fs_native_get_executable_path_internal(char *out, uint32_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
#if JCE_PLATFORM_WEB || JCE_PLATFORM_MOBILE || JCE_PLATFORM_TVOS
    return false;
#elif JCE_PLATFORM_WINDOWS
    /* n == capacity can mean truncation, even when older Windows omits NUL.
     * Keep every attempt bounded by the Windows maximum extended-path size. */
    for (uint32_t capacity = 256; capacity <= 32768; capacity *= 2) {
        WCHAR *wide = jce_malloc((size_t)capacity * sizeof(*wide));
        if (!wide) return false;
        const DWORD size = GetModuleFileNameW(NULL, wide, (DWORD)capacity);
        if (size == 0) { jce_free(wide); return false; }
        if (size >= capacity) { jce_free(wide); continue; }
        wide[size] = L'\0';
#if defined(JCE_WINXP_LEGACY) && JCE_WINXP_LEGACY
        if (!jce_fs_executable_path_utf16_valid_internal(wide, size)) {
            jce_free(wide);
            return false;
        }
        const DWORD conversion_flags = 0;
#else
        const DWORD conversion_flags = WC_ERR_INVALID_CHARS;
#endif
        const int bytes = WideCharToMultiByte(CP_UTF8, conversion_flags,
                                             wide, -1, NULL, 0, NULL, NULL);
        if (bytes <= 0 || (uint32_t)bytes > out_size) {
            jce_free(wide);
            return false;
        }
        const int copied = WideCharToMultiByte(CP_UTF8, conversion_flags,
                                              wide, -1, out, bytes, NULL, NULL);
        jce_free(wide);
        if (copied == bytes) return true;
        out[0] = '\0';
        return false;
    }
    return false;
#elif JCE_PLATFORM_LINUX
    for (size_t capacity = 256; capacity <= 32768; capacity *= 2) {
        char *path = jce_malloc(capacity + 1);
        if (!path) return false;
        const ssize_t count = readlink("/proc/self/exe", path, capacity);
        if (count <= 0) { jce_free(path); return false; }
        const size_t size = (size_t)count;
        if (size >= capacity) { jce_free(path); continue; }
        path[size] = '\0';
        if (path[0] != '/' || size + 1 > out_size ||
            !jce_fs_executable_path_utf8_internal(path, size)) {
            jce_free(path);
            return false;
        }
        memcpy(out, path, size + 1);
        jce_free(path);
        return true;
    }
    return false;
#else
    /* macOS is not implemented here. Mobile and Web must not accidentally
     * report a runtime/host interpreter as a native application executable. */
    return false;
#endif
}
#endif
