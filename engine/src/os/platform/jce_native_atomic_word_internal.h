/* Private allocation-free native-word atomics for foundational owners.
 * Static zero initialization needs no runtime, SDL, heap or shutdown owner.
 * These counters provide at least relaxed ordering, not object publication.
 * Unknown/non-lock-free targets must be ported explicitly, never use plain RMW. */
#ifndef JCE_NATIVE_ATOMIC_WORD_INTERNAL_H
#define JCE_NATIVE_ATOMIC_WORD_INTERNAL_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stddef.h>

#if JCE_COMPILER_MSVC
#include <intrin.h>
#if JCE_ARCH_X64 || JCE_ARCH_ARM64
typedef struct __declspec(align(8)) { volatile __int64 value; } JceNativeAtomicWord;
JCE_INLINE size_t jce_native_atomic_word_load(const JceNativeAtomicWord *word)
{ return (size_t)_InterlockedCompareExchange64((volatile __int64 *)&word->value, 0, 0); }
JCE_INLINE void jce_native_atomic_word_store(JceNativeAtomicWord *word, size_t value)
{ (void)_InterlockedExchange64(&word->value, (__int64)value); }
JCE_INLINE size_t jce_native_atomic_word_add(JceNativeAtomicWord *word, size_t value)
{ return (size_t)_InterlockedExchangeAdd64(&word->value, (__int64)value); }
JCE_INLINE bool jce_native_atomic_word_compare_exchange(JceNativeAtomicWord *word,
    size_t expected, size_t desired)
{ return (size_t)_InterlockedCompareExchange64(&word->value, (__int64)desired,
                                              (__int64)expected) == expected; }
#elif JCE_ARCH_X86 || JCE_ARCH_ARM
typedef struct __declspec(align(4)) { volatile long value; } JceNativeAtomicWord;
JCE_INLINE size_t jce_native_atomic_word_load(const JceNativeAtomicWord *word)
{ return (size_t)_InterlockedCompareExchange((volatile long *)&word->value, 0, 0); }
JCE_INLINE void jce_native_atomic_word_store(JceNativeAtomicWord *word, size_t value)
{ (void)_InterlockedExchange(&word->value, (long)value); }
JCE_INLINE size_t jce_native_atomic_word_add(JceNativeAtomicWord *word, size_t value)
{ return (size_t)_InterlockedExchangeAdd(&word->value, (long)value); }
JCE_INLINE bool jce_native_atomic_word_compare_exchange(JceNativeAtomicWord *word,
    size_t expected, size_t desired)
{ return (size_t)_InterlockedCompareExchange(&word->value, (long)desired,
                                            (long)expected) == expected; }
#else
#error "JCE native-word atomic MSVC architecture needs an explicit adapter"
#endif
#elif JCE_COMPILER_GCC || JCE_COMPILER_CLANG
typedef struct { size_t value; } JceNativeAtomicWord;
typedef char JceNativeWordMustBeLockFree[
    __atomic_always_lock_free(sizeof(size_t), 0) ? 1 : -1];
JCE_INLINE size_t jce_native_atomic_word_load(const JceNativeAtomicWord *word)
{ return __atomic_load_n(&word->value, __ATOMIC_RELAXED); }
JCE_INLINE void jce_native_atomic_word_store(JceNativeAtomicWord *word, size_t value)
{ __atomic_store_n(&word->value, value, __ATOMIC_RELAXED); }
JCE_INLINE size_t jce_native_atomic_word_add(JceNativeAtomicWord *word, size_t value)
{ return __atomic_fetch_add(&word->value, value, __ATOMIC_RELAXED); }
JCE_INLINE bool jce_native_atomic_word_compare_exchange(JceNativeAtomicWord *word,
    size_t expected, size_t desired)
{ return __atomic_compare_exchange_n(&word->value, &expected, desired, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED); }
#else
#error "JCE native-word atomics require a supported compiler adapter"
#endif

typedef char JceNativeWordMatchesSizeT[
    sizeof(((JceNativeAtomicWord *)0)->value) == sizeof(size_t) ? 1 : -1];
#endif
