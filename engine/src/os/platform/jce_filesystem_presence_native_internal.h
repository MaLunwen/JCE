/* Private OS metadata adapter, compiled in the existing Core FS owner.
 * SDL_GetPathInfo's false/NONE is ambiguous; never parse SDL error text.
 * No read/open, dependency, global state or public platform types. */
#ifndef JCE_FILESYSTEM_PRESENCE_NATIVE_INTERNAL_H
#define JCE_FILESYSTEM_PRESENCE_NATIVE_INTERNAL_H
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#if JCE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif JCE_PLATFORM_LINUX || JCE_PLATFORM_APPLE || JCE_PLATFORM_ANDROID || JCE_PLATFORM_WEB
#include <sys/stat.h>
#include <errno.h>
#endif
static inline JceFsPresence jce_fs_native_probe_read_internal(const char *path)
{
    if (!path || !path[0]) return JCE_FS_PRESENCE_UNKNOWN;
#if JCE_PLATFORM_WINDOWS
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0 || (size_t)count > SIZE_MAX / sizeof(WCHAR)) return JCE_FS_PRESENCE_UNKNOWN;
    WCHAR *wide = jce_malloc((size_t)count * sizeof(*wide));
    if (!wide) return JCE_FS_PRESENCE_UNKNOWN;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count) != count) {
        jce_free(wide);
        return JCE_FS_PRESENCE_UNKNOWN;
    }
    WIN32_FILE_ATTRIBUTE_DATA info;
    const BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &info);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    jce_free(wide);
    if (!ok) return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
        ? JCE_FS_PRESENCE_ABSENT : JCE_FS_PRESENCE_UNKNOWN;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE |
                                FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_REPARSE_POINT))
        return JCE_FS_PRESENCE_UNKNOWN;
    return JCE_FS_PRESENCE_PRESENT;
#elif JCE_PLATFORM_LINUX || JCE_PLATFORM_APPLE || JCE_PLATFORM_ANDROID || JCE_PLATFORM_WEB
    struct stat info;
    if (stat(path, &info) != 0)
        return errno == ENOENT ? JCE_FS_PRESENCE_ABSENT : JCE_FS_PRESENCE_UNKNOWN;
    return S_ISREG(info.st_mode) ? JCE_FS_PRESENCE_PRESENT : JCE_FS_PRESENCE_UNKNOWN;
#else
    (void)path;
    return JCE_FS_PRESENCE_UNKNOWN;
#endif
}
#endif
