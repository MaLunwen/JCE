/* Private optional DbgHelp refresh adapter. Include after native Windows
 * declarations or a typed test port; no Windows type enters the public ABI. */
#ifndef JCE_DBGHELP_NATIVE_INTERNAL_H
#define JCE_DBGHELP_NATIVE_INTERNAL_H

#include <stddef.h>
#include <string.h>

static inline BOOL jce_native_sym_refresh_module_list_internal(HANDLE process)
{
    typedef BOOL (WINAPI *JceSymRefreshModuleListFn)(HANDLE);
    /* SymInitialize is already linked by the caller. Borrow its loaded module;
     * absence must not load a second DbgHelp or invent a successful refresh. */
    HMODULE module = GetModuleHandleW(L"dbghelp.dll");
    FARPROC address = module ? GetProcAddress(module, "SymRefreshModuleList") : NULL;
    JceSymRefreshModuleListFn refresh = NULL;
    if (!address || sizeof refresh != sizeof address) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return FALSE;
    }

    /* Match the existing thread-description adapter's function-address copy.
     * WINAPI preserves the native x86 calling convention as well as x64. */
    memcpy(&refresh, &address, sizeof refresh);
    return refresh(process);
}

#endif
