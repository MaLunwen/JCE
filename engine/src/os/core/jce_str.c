/*
 * jce_str.c  Cross-platform string helpers.
 */

#include <jce/os/core/jce_str.h>
#include "os/core/jce_memory.h"

#include <stddef.h>
#include <string.h>

#if JCE_PLATFORM_WINDOWS
#  include <string.h>  /* _stricmp */
#else
#  include <strings.h> /* strcasecmp */
#endif

int jce_strcasecmp(const char *a, const char *b)
{
#if JCE_PLATFORM_WINDOWS
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

char *jce_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)JCE_MALLOC(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

const char *jce_platform_name(void)
{
#if JCE_PLATFORM_WINDOWS
    return "Windows";
#elif JCE_PLATFORM_IOS
    return "iOS";
#elif JCE_PLATFORM_MACOS
    return "macOS";
#elif JCE_PLATFORM_ANDROID
    return "Android";
#elif JCE_PLATFORM_WEB
    return "WebAssembly";
#elif JCE_PLATFORM_LINUX
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "Unknown";
#endif
}
