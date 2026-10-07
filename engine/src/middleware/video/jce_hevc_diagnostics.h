/* Exact bounded opt-in controls; malformed values preserve defaults. */
#ifndef JCE_HEVC_DIAGNOSTICS_H
#define JCE_HEVC_DIAGNOSTICS_H
#include <stdint.h>
#include <stdbool.h>
static inline uint32_t jce_hevc_diagnostic_cores(const char *value, uint32_t fallback)
{
    return value && value[0] >= '1' && value[0] <= '8' && value[1] == '\0'
        ? (uint32_t)(value[0] - '0') : fallback;
}
static inline int jce_hevc_diagnostic_keep_threads(const char *value, int fallback)
{
    if (value && (value[0] == '0' || value[0] == '1') && value[1] == '\0')
        return value[0] == '1';
    return fallback;
}
static inline bool jce_hevc_runtime_sse42(bool sse41, bool sse42, const char *generic)
{
    return sse41 && sse42 && !jce_hevc_diagnostic_keep_threads(generic, 0);
}
#endif
