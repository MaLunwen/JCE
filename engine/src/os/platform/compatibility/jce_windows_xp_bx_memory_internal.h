/* Private BX optional large-page query, not a large-page allocator port.
 * Zero preserves BX memoryPageSize(PageLarge/PageHuge)'s documented unsupported
 * result. The default-page query and VirtualAlloc behavior remain upstream.
 * No runtime-floor, complete BX/Editor or allocator functionality claim follows.
 */
#ifndef JCE_WINDOWS_XP_BX_MEMORY_INTERNAL_H
#define JCE_WINDOWS_XP_BX_MEMORY_INTERNAL_H
#if !defined(JCE_WINDOWS_XP_BX_MEMORY_PORT) || JCE_WINDOWS_XP_BX_MEMORY_PORT != 1
#error "Explicit private XP BX memory opt-in is required"
#endif
#if !defined(_WIN32) || !defined(__clang__) || !defined(__clang_major__) || __clang_major__ != 22 || !defined(__MINGW32__) || defined(_MSC_VER)
#error "XP BX memory requires the LLVM-MinGW GNU Clang 22 target"
#endif
#if defined(_WIN64) && defined(__x86_64__)
#if !defined(WINVER) || WINVER != 0x0502
#error "x64 XP BX memory requires true NT 5.2 WINVER"
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT != 0x0502 && _WIN32_WINNT != 0x0601)
#error "XP BX memory admits only true XP or reviewed 0601 declarations"
#endif
#elif !defined(_WIN64) && defined(__i386__)
#if !defined(WINVER) || WINVER != 0x0501
#error "x86 XP BX memory requires true NT 5.1 WINVER"
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT != 0x0501 && _WIN32_WINNT != 0x0601)
#error "XP BX memory admits only true XP or reviewed 0601 declarations"
#endif
#else
#error "XP BX memory admits only x86 and x64"
#endif

/* Match BX platform.h/os.cpp's Windows-header intent before forced inclusion.
 * Existing caller selections are preserved. Native declarations precede alias.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
/* Windows defines legacy lowercase pointer keywords. Preserve arbitrary
 * caller choices while preventing those SDK definitions from leaking into BX.
 */
#pragma push_macro("near")
#pragma push_macro("far")
#undef near
#undef far
#include <windows.h>
#include <string.h>
#pragma pop_macro("far")
#pragma pop_macro("near")

/* Typed test ports belong only to the isolated CPU fixture. Borrowing a module
 * never loads/unloads it. Each call probes afresh; no global cache or registry.
 */
#ifndef JCE_XP_BX_MEMORY_GET_MODULE_HANDLE_W
#define JCE_XP_BX_MEMORY_GET_MODULE_HANDLE_W GetModuleHandleW
#endif
#ifndef JCE_XP_BX_MEMORY_GET_PROC_ADDRESS
#define JCE_XP_BX_MEMORY_GET_PROC_ADDRESS GetProcAddress
#endif
static inline SIZE_T WINAPI jce_xp_bx_GetLargePageMinimum(void) {
    typedef SIZE_T (WINAPI *JceXpBxLargePageMinimumFn)(void);
    HMODULE module = JCE_XP_BX_MEMORY_GET_MODULE_HANDLE_W(L"kernel32.dll");
    FARPROC address = module ? JCE_XP_BX_MEMORY_GET_PROC_ADDRESS(module, "GetLargePageMinimum") : NULL;
    JceXpBxLargePageMinimumFn query = NULL;
    if (!address || sizeof query != sizeof address) return 0;
    /* Reuse the native thread-description adapter's function-address copy.
     * Do not cast between incompatible function signatures or invent a size.
     */
    memcpy(&query, &address, sizeof query);
    return query();
}

#define GetLargePageMinimum jce_xp_bx_GetLargePageMinimum
#endif
