/* Private process-local XP exclusive SRW port, not a general Win32 shim.
 * Objects are naturally pointer-aligned and never copied while in use.
 * Initialization requires quiescence. Recursive acquisition and forced thread
 * termination/cancellation are unsupported. No allocation or runtime is used.
 */
#ifndef JCE_WINDOWS_XP_SYNC_INTERNAL_H
#define JCE_WINDOWS_XP_SYNC_INTERNAL_H

#if !defined(JCE_WINDOWS_XP_SYNC_PORT) || JCE_WINDOWS_XP_SYNC_PORT != 1
#error "Explicit private XP synchronization opt-in is required"
#endif
#if !defined(_WIN32) || !defined(__clang__) || !defined(__MINGW32__) || defined(_MSC_VER)
#error "XP synchronization requires the LLVM-MinGW GNU target"
#endif
#if defined(_WIN64) && defined(__x86_64__)
#if !defined(_WIN32_WINNT) || !defined(WINVER) || _WIN32_WINNT != 0x0502 || WINVER != 0x0502
#error "x64 XP synchronization requires the real NT 5.2 floor"
#endif
#elif !defined(_WIN64) && defined(__i386__)
#if !defined(_WIN32_WINNT) || !defined(WINVER) || _WIN32_WINNT != 0x0501 || WINVER != 0x0501
#error "x86 XP synchronization requires the real NT 5.1 floor"
#endif
#else
#error "XP synchronization admits only x86 and x64"
#endif

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef char jce_xp_pointer_atomic_must_inline[
    __atomic_always_lock_free(sizeof(PVOID), 0) ? 1 : -1];
typedef char jce_xp_srw_layout[
    sizeof(SRWLOCK) == sizeof(PVOID) && offsetof(SRWLOCK, Ptr) == 0 &&
    sizeof(((PSRWLOCK)0)->Ptr) == sizeof(PVOID) ? 1 : -1];
typedef char jce_xp_srw_alignment[
    __alignof__(SRWLOCK) >= __alignof__(PVOID) ? 1 : -1];
typedef char jce_xp_srw_word_width[sizeof(uintptr_t) == sizeof(PVOID) ? 1 : -1];

/* Frozen prototype algorithms use the declared PVOID object type directly.
 * Never reinterpret SRWLOCK.Ptr as a native-word/integer atomic wrapper.
 * Impossible state fails only this exact current process with exit 91.
 */
static inline void jce_xp_sync_fatal(void) {
    TerminateProcess(GetCurrentProcess(), 91);
    /* Termination of this valid pseudo-handle should not return. */
    for (;;) Sleep(INFINITE);
}
static inline uintptr_t jce_xp_sync_load(PVOID *word) {
    return (uintptr_t)__atomic_load_n(word, __ATOMIC_ACQUIRE);
}
static inline void jce_xp_sync_store(PVOID *word, uintptr_t value) {
    __atomic_store_n(word, (PVOID)value, __ATOMIC_RELEASE);
}
static inline bool jce_xp_sync_cas(PVOID *word, uintptr_t *expected, uintptr_t value) {
    PVOID observed = (PVOID)*expected;
    bool result = __atomic_compare_exchange_n(word, &observed, (PVOID)value, false,
                                              __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    *expected = (uintptr_t)observed;
    return result;
}
static inline void jce_xp_sync_backoff(unsigned *attempts) {
    if (++*attempts < 32) return;
    if (*attempts < 64) SwitchToThread();
    else { Sleep(1); *attempts = 32; }
}

static inline VOID WINAPI jce_xp_InitializeSRWLock(PSRWLOCK lock) {
    jce_xp_sync_store(&lock->Ptr, 0);
}
static inline BOOLEAN WINAPI jce_xp_TryAcquireSRWLockExclusive(PSRWLOCK lock) {
    uintptr_t expected = 0;
    if (jce_xp_sync_cas(&lock->Ptr, &expected, 1)) return TRUE;
    if (expected != 1) jce_xp_sync_fatal();
    return FALSE;
}
static inline VOID WINAPI jce_xp_AcquireSRWLockExclusive(PSRWLOCK lock) {
    unsigned attempts = 0;
    while (!jce_xp_TryAcquireSRWLockExclusive(lock)) jce_xp_sync_backoff(&attempts);
}
static inline VOID WINAPI jce_xp_ReleaseSRWLockExclusive(PSRWLOCK lock) {
    if (jce_xp_sync_load(&lock->Ptr) != 1) jce_xp_sync_fatal();
    jce_xp_sync_store(&lock->Ptr, 0);
}
#endif
