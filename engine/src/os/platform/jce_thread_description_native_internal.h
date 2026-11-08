/* Private optional Windows thread-name adapter. Include after the native
 * Windows declarations, or a typed test port; no Windows type enters the ABI. */
#ifndef JCE_THREAD_DESCRIPTION_NATIVE_INTERNAL_H
#define JCE_THREAD_DESCRIPTION_NATIVE_INTERNAL_H

#include <stddef.h>
#include <string.h>

static inline HRESULT jce_native_get_thread_description_internal(
    HANDLE thread, PWSTR *description)
{
    typedef HRESULT (WINAPI *JceGetThreadDescriptionFn)(HANDLE, PWSTR *);
    if (!description) return E_POINTER;
    *description = NULL;

    /* Windows 10 1607 exposes this only through KernelBase at runtime.
     * Older Windows may expose neither. Both module handles are borrowed. */
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    FARPROC address = module ? GetProcAddress(module, "GetThreadDescription") : NULL;
    if (!address) {
        module = GetModuleHandleW(L"Kernel32.dll");
        address = module ? GetProcAddress(module, "GetThreadDescription") : NULL;
    }
    if (!address) return E_NOTIMPL;

    /* Windows native function pointers have the same representation. Copy
     * the address without a cast between incompatible function signatures. */
    JceGetThreadDescriptionFn get_description = NULL;
    if (sizeof get_description != sizeof address) return E_NOTIMPL;
    memcpy(&get_description, &address, sizeof get_description);
    /* The caller owns any returned allocation, including on API failure. */
    return get_description(thread, description);
}

#endif
