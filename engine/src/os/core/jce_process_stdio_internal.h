/* Private stdio policy for the sole SDL process owner. No SDK ABI changes. */
#ifndef JCE_PROCESS_STDIO_INTERNAL_H
#define JCE_PROCESS_STDIO_INTERNAL_H
#include <jce/os/core/jce_defs.h>
#include <SDL3/SDL_process.h>
#include <stdbool.h>
#include "os/platform/jce_process_stdio_native_internal.h"

typedef enum JceProcessStdioReason {
    JCE_PROCESS_STDIO_CAPTURE,
    JCE_PROCESS_STDIO_INHERIT,
    JCE_PROCESS_STDIO_ABSENT,
    JCE_PROCESS_STDIO_INVALID
} JceProcessStdioReason;
typedef struct JceProcessStdioDecision {
    SDL_ProcessIO mode;
    JceProcessStdioReason reason;
} JceProcessStdioDecision;

static inline JceProcessStdioDecision jce_process_stdio_resolve(bool capture,bool stderr_stream)
{
    JceProcessStdioDecision result={SDL_PROCESS_STDIO_APP,JCE_PROCESS_STDIO_CAPTURE};
    if(capture) return result;
    result.mode=SDL_PROCESS_STDIO_INHERITED;result.reason=JCE_PROCESS_STDIO_INHERIT;
#if JCE_PLATFORM_WINDOWS
    const JceProcessStdioHandleState state=jce_process_stdio_native_observe(stderr_stream);
    if(state==JCE_PROCESS_STDIO_HANDLE_ABSENT) {
        result.mode=SDL_PROCESS_STDIO_NULL;result.reason=JCE_PROCESS_STDIO_ABSENT;
    } else if(state==JCE_PROCESS_STDIO_HANDLE_INVALID) {
        result.mode=SDL_PROCESS_STDIO_NULL;result.reason=JCE_PROCESS_STDIO_INVALID;
    }
#else
    (void)stderr_stream; /* Existing POSIX stream inheritance remains unchanged. */
#endif
    return result;
}
static inline const char *jce_process_stdio_reason_name(JceProcessStdioReason reason)
{
    switch(reason) {
    case JCE_PROCESS_STDIO_CAPTURE:return "app";
    case JCE_PROCESS_STDIO_INHERIT:return "inherited";
    case JCE_PROCESS_STDIO_ABSENT:return "discard_absent";
    case JCE_PROCESS_STDIO_INVALID:return "discard_invalid";
    default:return "unknown";
    }
}
#endif
