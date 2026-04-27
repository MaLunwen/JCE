/*
 * jce_str.c  Cross-platform string helpers.
 */

#include <jce/os/core/jce_str.h>

#include <stddef.h>
#include <string.h>

#if defined(_WIN32)
#  include <string.h>  /* _stricmp */
#else
#  include <strings.h> /* strcasecmp */
#endif

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#endif

int jce_strcasecmp(const char *a, const char *b)
{
#if defined(_WIN32)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

size_t jce_strlcpy(char *dst, const char *src, size_t n)
{
    size_t src_len = 0;
    while (src[src_len] != '\0') src_len++;

    if (n != 0) {
        size_t copy = (src_len < n - 1) ? src_len : (n - 1);
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return src_len;
}

const char *jce_platform_name(void)
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
#  if TARGET_OS_IOS
    return "iOS";
#  else
    return "macOS";
#  endif
#elif defined(__ANDROID__)
    return "Android";
#elif defined(__EMSCRIPTEN__)
    return "Emscripten";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "Unknown";
#endif
}
