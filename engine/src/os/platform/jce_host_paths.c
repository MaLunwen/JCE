/*
 * jce_host_paths.c  Host OS well-known paths and drive enumeration.
 *
 * Implementation notes:
 *   - User folders are resolved via SDL3's SDL_GetUserFolder, which
 *     internally calls SHGetKnownFolderPath on Windows, XDG user-dirs
 *     on Linux, and NSFileManager URLsForDirectory on Apple platforms.
 *   - Drive enumeration is Windows-only; SDL3 does not expose an
 *     equivalent helper, so the Windows backend uses
 *     GetLogicalDriveStringsA directly here (allowed: this TU lives in
 *     engine/src/os/platform/ which is the only place permitted to
 *     branch on platform macros).
 */

#include <jce/os/platform/jce_host_paths.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/* ------------------------------------------------------------------ */
/* User folders                                                        */
/* ------------------------------------------------------------------ */

bool jce_host_get_user_folder(JceUserFolder kind, char *dst, size_t cap)
{
    if (!dst || cap == 0) return false;
    dst[0] = '\0';

    SDL_Folder sdl_kind;
    switch (kind) {
    case JCE_USER_FOLDER_HOME:      sdl_kind = SDL_FOLDER_HOME;      break;
    case JCE_USER_FOLDER_DESKTOP:   sdl_kind = SDL_FOLDER_DESKTOP;   break;
    case JCE_USER_FOLDER_DOCUMENTS: sdl_kind = SDL_FOLDER_DOCUMENTS; break;
    case JCE_USER_FOLDER_DOWNLOADS: sdl_kind = SDL_FOLDER_DOWNLOADS; break;
    default:                        return false;
    }

    const char *p = SDL_GetUserFolder(sdl_kind);
    if (!p || !p[0]) return false;

    size_t len = strlen(p);
    if (len + 1 > cap) return false;
    memcpy(dst, p, len + 1);
    return true;
}

/* ------------------------------------------------------------------ */
/* Drive enumeration                                                   */
/* ------------------------------------------------------------------ */

int jce_host_list_drives(char dst[][8], int max_drives)
{
    if (!dst || max_drives <= 0) return 0;

#if defined(_WIN32)
    /* GetLogicalDriveStringsA fills a buffer with a sequence of
       NUL-terminated drive root strings ("C:\\\0D:\\\0\0").  We need
       at most 4 * 26 + 1 = 105 bytes for every possible drive. */
    char buf[256];
    DWORD n = GetLogicalDriveStringsA((DWORD)sizeof(buf) - 1, buf);
    if (n == 0 || n >= sizeof(buf)) return 0;

    int count = 0;
    const char *p = buf;
    while (*p && count < max_drives) {
        size_t len = strlen(p);
        if (len + 1 <= 8) {
            memcpy(dst[count], p, len + 1);
            ++count;
        }
        p += len + 1;
    }
    return count;
#else
    /* POSIX: a single filesystem root.  Caller fans this out into
       favourites / Home / Documents via jce_host_get_user_folder. */
    dst[0][0] = '/';
    dst[0][1] = '\0';
    return 1;
#endif
}
