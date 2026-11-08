/* Private native observation compiled in the existing Core process owner.
 * Header-only seam: no platform-library dependency, new ABI or global state. */
#ifndef JCE_PROCESS_STDIO_NATIVE_INTERNAL_H
#define JCE_PROCESS_STDIO_NATIVE_INTERNAL_H
#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#if JCE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
typedef enum JceProcessStdioHandleState {
    JCE_PROCESS_STDIO_HANDLE_VALID,
    JCE_PROCESS_STDIO_HANDLE_ABSENT,
    JCE_PROCESS_STDIO_HANDLE_INVALID
} JceProcessStdioHandleState;
static inline JceProcessStdioHandleState jce_process_stdio_native_observe(bool stderr_stream)
{
#if JCE_PLATFORM_WINDOWS
    HANDLE handle=GetStdHandle(stderr_stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if(!handle || handle==INVALID_HANDLE_VALUE)return JCE_PROCESS_STDIO_HANDLE_ABSENT;
    DWORD flags=0;
    return GetHandleInformation(handle,&flags) ? JCE_PROCESS_STDIO_HANDLE_VALID : JCE_PROCESS_STDIO_HANDLE_INVALID;
#else
    (void)stderr_stream;return JCE_PROCESS_STDIO_HANDLE_VALID;
#endif
}
#endif
