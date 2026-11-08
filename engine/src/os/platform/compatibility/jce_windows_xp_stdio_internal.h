/* Private consumer-only XP standard-FILE port for reviewed Flecs/miniz calls.
 * Not a public CRT replacement or secureCRT emulation: ordinary fopen sharing,
 * no invalid-parameter handler, Unicode stream/BOM extensions or secure modes.
 * Admits C byte modes r/w/a, optionally b and/or + in either standard order.
 * NULL/empty arguments and other modes return EINVAL, set errno, and clear a
 * valid output slot. Invalid reopen arguments leave the old stream untouched.
 * Valid reopen delegates to system _wfreopen, including consumption on failure;
 * callers must provide a live FILE and never use it after failed valid reopen.
 * FILE objects remain system-MSVCRT objects. No JCE allocator, SDL or state.
 */
#ifndef JCE_WINDOWS_XP_STDIO_INTERNAL_H
#define JCE_WINDOWS_XP_STDIO_INTERNAL_H
#if !defined(JCE_WINDOWS_XP_STDIO_PORT) || JCE_WINDOWS_XP_STDIO_PORT != 1
#error "Explicit private XP stdio opt-in is required"
#endif
#if !defined(_WIN32) || !defined(__clang__) || !defined(__MINGW32__) || defined(_MSC_VER) || defined(_UCRT)
#error "XP stdio requires the LLVM-MinGW GNU system-MSVCRT target"
#endif
#if defined(_WIN64) && defined(__x86_64__)
#if !defined(_WIN32_WINNT) || !defined(WINVER) || _WIN32_WINNT != 0x0502 || WINVER != 0x0502
#error "x64 XP stdio requires the real NT 5.2 floor"
#endif
#elif !defined(_WIN64) && defined(__i386__)
#if !defined(_WIN32_WINNT) || !defined(WINVER) || _WIN32_WINNT != 0x0501 || WINVER != 0x0501
#error "x86 XP stdio requires the real NT 5.1 floor"
#endif
#else
#error "XP stdio admits only x86 and x64"
#endif

/* Native declarations must precede consumer aliases. */
#include <stdio.h>
#include <wchar.h>
#include <errno.h>
#ifdef _UCRT
#error "XP stdio does not admit UCRT headers"
#endif

/* Typed private CPU-fixture ports; production defaults are real system CRT. */
#ifndef JCE_XP_STDIO_FOPEN
#define JCE_XP_STDIO_FOPEN fopen
#endif
#ifndef JCE_XP_STDIO_WFOPEN
#define JCE_XP_STDIO_WFOPEN _wfopen
#endif
#ifndef JCE_XP_STDIO_WFREOPEN
#define JCE_XP_STDIO_WFREOPEN _wfreopen
#endif

static inline int jce_xp_stdio_mode(const char *mode) {
    if (!mode || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) return 0;
    if (!mode[1]) return 1;
    if (mode[1] == 'b') return !mode[2] || (mode[2] == '+' && !mode[3]);
    if (mode[1] == '+') return !mode[2] || (mode[2] == 'b' && !mode[3]);
    return 0;
}
static inline int jce_xp_stdio_wmode(const wchar_t *mode) {
    if (!mode || (mode[0] != L'r' && mode[0] != L'w' && mode[0] != L'a')) return 0;
    if (!mode[1]) return 1;
    if (mode[1] == L'b') return !mode[2] || (mode[2] == L'+' && !mode[3]);
    if (mode[1] == L'+') return !mode[2] || (mode[2] == L'b' && !mode[3]);
    return 0;
}
static inline errno_t jce_xp_stdio_invalid(void) { errno = EINVAL; return EINVAL; }
static inline int jce_xp_stdio_output(FILE **result) {
    if (!result) return 0;
    *result = NULL; /* Required failure/invalid-argument output initialization. */
    return 1;
}
static inline errno_t jce_xp_stdio_finish(FILE **result, FILE *opened, int previous_errno) {
    if (opened) { *result = opened; errno = previous_errno; return 0; }
    /* Never return success for a NULL stream or recycle stale caller errno. */
    if (!errno) errno = EIO;
    return errno;
}
static inline errno_t jce_xp_fopen_s(FILE **result, const char *filename, const char *mode) {
    FILE *opened; int previous_errno;
    if (!jce_xp_stdio_output(result)) return jce_xp_stdio_invalid();
    if (!filename || !filename[0] || !jce_xp_stdio_mode(mode)) return jce_xp_stdio_invalid();
    previous_errno = errno; errno = 0;
    opened = JCE_XP_STDIO_FOPEN(filename, mode);
    return jce_xp_stdio_finish(result, opened, previous_errno);
}
static inline errno_t jce_xp_wfopen_s(FILE **result, const wchar_t *filename, const wchar_t *mode) {
    FILE *opened; int previous_errno;
    if (!jce_xp_stdio_output(result)) return jce_xp_stdio_invalid();
    if (!filename || !filename[0] || !jce_xp_stdio_wmode(mode)) return jce_xp_stdio_invalid();
    previous_errno = errno; errno = 0;
    opened = JCE_XP_STDIO_WFOPEN(filename, mode);
    return jce_xp_stdio_finish(result, opened, previous_errno);
}
static inline errno_t jce_xp_wfreopen_s(FILE **result, const wchar_t *filename, const wchar_t *mode, FILE *stream) {
    FILE *opened; int previous_errno;
    if (!jce_xp_stdio_output(result)) return jce_xp_stdio_invalid();
    if (!filename || !filename[0] || !jce_xp_stdio_wmode(mode) || !stream) return jce_xp_stdio_invalid();
    previous_errno = errno; errno = 0;
    opened = JCE_XP_STDIO_WFREOPEN(filename, mode, stream);
    /* A failed valid _wfreopen has already consumed stream. Never fclose it. */
    return jce_xp_stdio_finish(result, opened, previous_errno);
}

/* Consumer-private names only. Never define global CRT/linker symbols. */
#define fopen_s jce_xp_fopen_s
#define _wfopen_s jce_xp_wfopen_s
#define _wfreopen_s jce_xp_wfreopen_s
#endif
