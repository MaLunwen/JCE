/* Private codec-only XP synchronization subset, not native API equivalence.
 * Process-local objects are naturally aligned, never copied while in use, and
 * initialized/destroyed only while quiescent. No forced thread termination,
 * cancellation, recursive exclusive acquisition or failed once reset is supported.
 * Each active wait owns one kernel event; no CRT/JCE allocation is used.
 */
#ifndef JCE_WINDOWS_XP_CODEC_SYNC_INTERNAL_H
#define JCE_WINDOWS_XP_CODEC_SYNC_INTERNAL_H
#if !defined(JCE_WINDOWS_XP_CODEC_PORT) || JCE_WINDOWS_XP_CODEC_PORT != 1
#error "Explicit private XP codec opt-in is required"
#endif

/* Only the true WINVER floor and the reviewed upstream 0601 declaration
 * selection are admitted. libaom appends 0601 and its pthread guard requires
 * Vista declarations; dav1d also generates 0601. This is an observed consumer
 * declaration exception, not a runtime floor, ABI or uniform-header claim.
 */
#if !defined(JCE_WINDOWS_XP_SYNC_PORT) || JCE_WINDOWS_XP_SYNC_PORT != 1
#error "Explicit private XP synchronization opt-in is required"
#endif
#if !defined(_WIN32) || !defined(__clang__) || !defined(__MINGW32__) || defined(_MSC_VER)
#error "XP codec synchronization requires the LLVM-MinGW GNU target"
#endif
#if defined(_WIN64) && defined(__x86_64__)
#if !defined(WINVER) || WINVER != 0x0502
#error "JCE_XP_CODEC_WINVER_REFUSED: x64 requires true NT 5.2 WINVER"
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT != 0x0502 && _WIN32_WINNT != 0x0601)
#error "JCE_XP_CODEC_DECLARATION_FLOOR_REFUSED: only true XP or reviewed 0601"
#endif
#elif !defined(_WIN64) && defined(__i386__)
#if !defined(WINVER) || WINVER != 0x0501
#error "JCE_XP_CODEC_WINVER_REFUSED: x86 requires true NT 5.1 WINVER"
#endif
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT != 0x0501 && _WIN32_WINNT != 0x0601)
#error "JCE_XP_CODEC_DECLARATION_FLOOR_REFUSED: only true XP or reviewed 0601"
#endif
#else
#error "XP codec synchronization admits only x86 and x64"
#endif

/* Keep common SRW admission and bytes unchanged. Native headers already parsed
 * by a caller are not retroactively changed. Restore the exact caller macro so
 * unchanged upstream source retains its own reviewed declaration selection.
 */
#pragma push_macro("_WIN32_WINNT")
#undef _WIN32_WINNT
#if defined(_WIN64)
#define _WIN32_WINNT 0x0502
#else
#define _WIN32_WINNT 0x0501
#endif
#include "jce_windows_xp_sync_internal.h"
#pragma pop_macro("_WIN32_WINNT")

typedef char jce_xp_codec_cv_layout[
    sizeof(CONDITION_VARIABLE) == sizeof(PVOID) &&
    offsetof(CONDITION_VARIABLE, Ptr) == 0 &&
    sizeof(((PCONDITION_VARIABLE)0)->Ptr) == sizeof(PVOID) ? 1 : -1];
typedef char jce_xp_codec_cv_alignment[
    __alignof__(CONDITION_VARIABLE) >= __alignof__(PVOID) ? 1 : -1];
typedef char jce_xp_codec_once_layout[
    sizeof(INIT_ONCE) == sizeof(PVOID) && offsetof(INIT_ONCE, Ptr) == 0 &&
    sizeof(((LPINIT_ONCE)0)->Ptr) == sizeof(PVOID) ? 1 : -1];
typedef char jce_xp_codec_once_alignment[
    __alignof__(INIT_ONCE) >= __alignof__(PVOID) ? 1 : -1];

/* Typed fault/observation ports belong only to the isolated CPU fixture. */
#ifndef JCE_XP_CODEC_SYNC_CREATE_EVENT
#define JCE_XP_CODEC_SYNC_CREATE_EVENT() CreateEventW(NULL, FALSE, FALSE, NULL)
#endif
#ifndef JCE_XP_CODEC_SYNC_SET_EVENT
#define JCE_XP_CODEC_SYNC_SET_EVENT(h) SetEvent(h)
#endif
#ifndef JCE_XP_CODEC_SYNC_WAIT_EVENT
#define JCE_XP_CODEC_SYNC_WAIT_EVENT(h) WaitForSingleObject(h, INFINITE)
#endif
#ifndef JCE_XP_CODEC_SYNC_CLOSE_EVENT
#define JCE_XP_CODEC_SYNC_CLOSE_EVENT(h) CloseHandle(h)
#endif
#ifndef JCE_XP_CODEC_SYNC_BEFORE_WAIT
#define JCE_XP_CODEC_SYNC_BEFORE_WAIT() ((void)0)
#endif

static inline BOOL jce_xp_codec_sync_error(DWORD error) {
    SetLastError(error);
    return FALSE;
}
typedef struct jce_xp_codec_sync_waiter {
    struct jce_xp_codec_sync_waiter *next;
    HANDLE event;
    BOOL queued;
} jce_xp_codec_sync_waiter;
typedef char jce_xp_codec_waiter_tag_alignment[
    __alignof__(jce_xp_codec_sync_waiter) >= 2 ? 1 : -1];

/* The low bit guards the intrusive LIFO queue. All waiter fields, signal and
 * removal hold this guard; no waker retains a stack pointer after unlocking.
 * No FIFO/fairness promise is made. Only the declared PVOID is atomically used.
 */
static inline jce_xp_codec_sync_waiter *jce_xp_codec_sync_queue_lock(
        PCONDITION_VARIABLE cv) {
    unsigned attempts = 0;
    for (;;) {
        uintptr_t value = jce_xp_sync_load(&cv->Ptr);
        if (!(value & 1) && jce_xp_sync_cas(&cv->Ptr, &value, value | 1))
            return (jce_xp_codec_sync_waiter *)(value & ~(uintptr_t)1);
        jce_xp_sync_backoff(&attempts);
    }
}
static inline void jce_xp_codec_sync_queue_unlock(
        PCONDITION_VARIABLE cv, jce_xp_codec_sync_waiter *head) {
    jce_xp_sync_store(&cv->Ptr, (uintptr_t)head);
}
static inline VOID WINAPI jce_xp_codec_InitializeConditionVariable(
        PCONDITION_VARIABLE cv) {
    jce_xp_sync_store(&cv->Ptr, 0);
}
static inline WINBOOL WINAPI jce_xp_codec_SleepConditionVariableSRW(
        PCONDITION_VARIABLE cv, PSRWLOCK lock, DWORD milliseconds, ULONG flags) {
    jce_xp_codec_sync_waiter waiter, *head, **cursor;
    DWORD result;
    if (!cv || !lock || (flags & ~CONDITION_VARIABLE_LOCKMODE_SHARED))
        return jce_xp_codec_sync_error(ERROR_INVALID_PARAMETER);
    if (flags || milliseconds != INFINITE)
        return jce_xp_codec_sync_error(ERROR_NOT_SUPPORTED);
    /* Reviewed codecs ignore wait failures. A supported wait must never return
     * a false success or spin in their predicate loop with its lock held.
     */
    waiter.event = JCE_XP_CODEC_SYNC_CREATE_EVENT();
    if (!waiter.event) jce_xp_sync_fatal();
    waiter.queued = TRUE;
    head = jce_xp_codec_sync_queue_lock(cv);
    waiter.next = head;
    head = &waiter;
    jce_xp_ReleaseSRWLockExclusive(lock);
    jce_xp_codec_sync_queue_unlock(cv, head);
    JCE_XP_CODEC_SYNC_BEFORE_WAIT();
    result = JCE_XP_CODEC_SYNC_WAIT_EVENT(waiter.event);
    head = jce_xp_codec_sync_queue_lock(cv);
    if (waiter.queued) {
        cursor = &head;
        while (*cursor && *cursor != &waiter) cursor = &(*cursor)->next;
        if (!*cursor) jce_xp_sync_fatal();
        *cursor = waiter.next;
        waiter.queued = FALSE;
    }
    jce_xp_codec_sync_queue_unlock(cv, head);
    if (!JCE_XP_CODEC_SYNC_CLOSE_EVENT(waiter.event)) jce_xp_sync_fatal();
    if (result != WAIT_OBJECT_0) jce_xp_sync_fatal();
    jce_xp_AcquireSRWLockExclusive(lock);
    return TRUE; /* Predicate loops must permit spurious successful wakes. */
}
static inline void jce_xp_codec_sync_wake(PCONDITION_VARIABLE cv, BOOL all) {
    jce_xp_codec_sync_waiter *head = jce_xp_codec_sync_queue_lock(cv);
    do {
        jce_xp_codec_sync_waiter *waiter = head;
        if (!waiter) break;
        head = waiter->next;
        waiter->queued = FALSE;
        if (!JCE_XP_CODEC_SYNC_SET_EVENT(waiter->event)) jce_xp_sync_fatal();
    } while (all);
    jce_xp_codec_sync_queue_unlock(cv, head);
}
static inline VOID WINAPI jce_xp_codec_WakeConditionVariable(
        PCONDITION_VARIABLE cv) {
    jce_xp_codec_sync_wake(cv, FALSE);
}
static inline VOID WINAPI jce_xp_codec_WakeAllConditionVariable(
        PCONDITION_VARIABLE cv) {
    jce_xp_codec_sync_wake(cv, TRUE);
}

/* Begin(0) and Complete(0,NULL) only. Duplicate completion truthfully fails
 * without altering completed state. ERROR_GEN_FAILURE is private policy, not
 * a claim of native wrong-state LastError equivalence. Acquire/release once
 * publication uses the shared pointer-typed atomics and no global registry.
 */
static inline WINBOOL WINAPI jce_xp_codec_InitOnceBeginInitialize(
        LPINIT_ONCE once, DWORD flags, PBOOL pending, LPVOID *context) {
    unsigned attempts = 0;
    if (!once || !pending || (flags & ~(INIT_ONCE_CHECK_ONLY | INIT_ONCE_ASYNC)))
        return jce_xp_codec_sync_error(ERROR_INVALID_PARAMETER);
    if (flags == (INIT_ONCE_CHECK_ONLY | INIT_ONCE_ASYNC))
        return jce_xp_codec_sync_error(ERROR_INVALID_PARAMETER);
    if (flags) return jce_xp_codec_sync_error(ERROR_NOT_SUPPORTED);
    for (;;) {
        uintptr_t value = jce_xp_sync_load(&once->Ptr);
        if (value == 0 && jce_xp_sync_cas(&once->Ptr, &value, 1)) {
            *pending = TRUE;
            if (context) *context = NULL;
            return TRUE;
        }
        if (value == 2) {
            *pending = FALSE;
            if (context) *context = NULL;
            return TRUE;
        }
        if (value > 2) return jce_xp_codec_sync_error(ERROR_INVALID_DATA);
        jce_xp_sync_backoff(&attempts);
    }
}
static inline WINBOOL WINAPI jce_xp_codec_InitOnceComplete(
        LPINIT_ONCE once, DWORD flags, LPVOID context) {
    uintptr_t expected = 1;
    if (!once || (flags & ~(INIT_ONCE_ASYNC | INIT_ONCE_INIT_FAILED)) ||
        ((uintptr_t)context & 3))
        return jce_xp_codec_sync_error(ERROR_INVALID_PARAMETER);
    if (flags == (INIT_ONCE_ASYNC | INIT_ONCE_INIT_FAILED))
        return jce_xp_codec_sync_error(ERROR_INVALID_PARAMETER);
    if (flags || context) return jce_xp_codec_sync_error(ERROR_NOT_SUPPORTED);
    if (!jce_xp_sync_cas(&once->Ptr, &expected, 2))
        return jce_xp_codec_sync_error(ERROR_GEN_FAILURE);
    return TRUE;
}
static inline WINBOOL WINAPI jce_xp_codec_GetThreadGroupAffinity(
        HANDLE thread, PGROUP_AFFINITY affinity) {
    (void)thread;
    (void)affinity;
    /* Preserve dav1d's unavailable -> one-processor default, not a fake mask.
     * Explicit multiworker selection needs separate actual decoder validation.
     */
    return jce_xp_codec_sync_error(ERROR_CALL_NOT_IMPLEMENTED);
}
#endif
