/* NDK compatibility for FDK's optional AOSP security-event reporting. */
#ifndef JCE_FDK_ANDROID_LOG_COMPAT_H
#define JCE_FDK_ANDROID_LOG_COMPAT_H
#include <android/log.h>
static inline int android_errorWriteLog(int tag, const char *message)
{
    return __android_log_print(ANDROID_LOG_ERROR, "fdk-aac",
        "security detection %s (tag 0x%x)", message, (unsigned)tag);
}
#endif
