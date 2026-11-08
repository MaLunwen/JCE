/* Private forced include for reviewed pristine codec consumers only.
 * Admission starts at the true x86 NT 5.1/x64 NT 5.2 floor checked by the
 * shared SRW header. A consumer's later generated config may select a higher
 * declaration floor (dav1d Meson does); no uniform preprocessing-floor or
 * complete codec/Editor/XP execution claim follows from these ten aliases.
 */
#ifndef JCE_WINDOWS_XP_CODECS_INTERNAL_H
#define JCE_WINDOWS_XP_CODECS_INTERNAL_H
#if !defined(JCE_WINDOWS_XP_CODEC_PORT) || JCE_WINDOWS_XP_CODEC_PORT != 1
#error "Explicit private XP codec opt-in is required"
#endif

#include "jce_windows_xp_codec_sync_internal.h"

/* Only the audited missing surface; native declarations and every helper above
 * are parsed before aliases. Existing common/mimalloc headers are unchanged.
 */
#define AcquireSRWLockExclusive jce_xp_AcquireSRWLockExclusive
#define InitializeSRWLock jce_xp_InitializeSRWLock
#define ReleaseSRWLockExclusive jce_xp_ReleaseSRWLockExclusive
#define InitializeConditionVariable jce_xp_codec_InitializeConditionVariable
#define SleepConditionVariableSRW jce_xp_codec_SleepConditionVariableSRW
#define WakeConditionVariable jce_xp_codec_WakeConditionVariable
#define WakeAllConditionVariable jce_xp_codec_WakeAllConditionVariable
#define InitOnceBeginInitialize jce_xp_codec_InitOnceBeginInitialize
#define InitOnceComplete jce_xp_codec_InitOnceComplete
#define GetThreadGroupAffinity jce_xp_codec_GetThreadGroupAffinity
#endif
