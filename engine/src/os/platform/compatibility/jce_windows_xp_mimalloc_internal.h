/* Private forced include for pristine mimalloc in the explicit XP lane.
 * Refusing optional NUMA capabilities when the real current-CPU export is
 * absent preserves mimalloc's own node-0/count-1 fallback. This conservative
 * package policy is not native API equivalence or an invented CPU identity.
 */
#ifndef JCE_WINDOWS_XP_MIMALLOC_INTERNAL_H
#define JCE_WINDOWS_XP_MIMALLOC_INTERNAL_H
#if !defined(JCE_WINDOWS_XP_MIMALLOC_PORT) || JCE_WINDOWS_XP_MIMALLOC_PORT != 1
#error "Explicit private XP mimalloc opt-in is required"
#endif

#include "jce_windows_xp_sync_internal.h"
#include <string.h>

/* Typed test ports may be supplied only by the isolated CPU fixture. Native
 * definitions are consumed before the aliases below; helpers never recurse.
 */
#ifndef JCE_XP_MIMALLOC_GET_PROC_ADDRESS
#define JCE_XP_MIMALLOC_GET_PROC_ADDRESS GetProcAddress
#endif
#ifndef JCE_XP_MIMALLOC_GET_MODULE_HANDLE_W
#define JCE_XP_MIMALLOC_GET_MODULE_HANDLE_W GetModuleHandleW
#endif

static inline FARPROC jce_xp_mimalloc_current_processor_export(void) {
    HMODULE kernel32 = JCE_XP_MIMALLOC_GET_MODULE_HANDLE_W(L"kernel32.dll");
    return kernel32 ? JCE_XP_MIMALLOC_GET_PROC_ADDRESS(kernel32, "GetCurrentProcessorNumber") : NULL;
}
static inline bool jce_xp_mimalloc_is_numa_probe(LPCSTR name) {
    /* GetProcAddress accepts 16-bit ordinals in the low pointer value. */
    if ((uintptr_t)name <= UINT16_MAX) return false;
    return strcmp(name, "GetNumaProcessorNodeEx") == 0 ||
           strcmp(name, "GetNumaNodeProcessorMaskEx") == 0 ||
           strcmp(name, "GetNumaProcessorNode") == 0 ||
           strcmp(name, "GetNumaNodeProcessorMask") == 0 ||
           strcmp(name, "GetNumaHighestNodeNumber") == 0;
}
static inline FARPROC WINAPI jce_xp_mimalloc_GetProcAddress(HMODULE module, LPCSTR name) {
    if (jce_xp_mimalloc_is_numa_probe(name) && !jce_xp_mimalloc_current_processor_export())
        return NULL;
    return JCE_XP_MIMALLOC_GET_PROC_ADDRESS(module, name);
}
static inline DWORD WINAPI jce_xp_mimalloc_GetCurrentProcessorNumber(void) {
    typedef DWORD (WINAPI *JceXpCurrentProcessor)(void);
    JceXpCurrentProcessor query =
        (JceXpCurrentProcessor)(void (*)(void))jce_xp_mimalloc_current_processor_export();
    if (!query) jce_xp_sync_fatal();
    return query();
}

/* Consumer-private aliases follow every Windows and native helper definition. */
#define InitializeSRWLock jce_xp_InitializeSRWLock
#define TryAcquireSRWLockExclusive jce_xp_TryAcquireSRWLockExclusive
#define AcquireSRWLockExclusive jce_xp_AcquireSRWLockExclusive
#define ReleaseSRWLockExclusive jce_xp_ReleaseSRWLockExclusive
#define GetCurrentProcessorNumber jce_xp_mimalloc_GetCurrentProcessorNumber
#define GetProcAddress jce_xp_mimalloc_GetProcAddress
#endif
