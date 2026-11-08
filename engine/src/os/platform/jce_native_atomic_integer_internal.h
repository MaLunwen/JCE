/* Private, allocation-free fixed-width RMW and native-word publication.
 * Signed add returns the POST value modulo 2^32/2^64, including negative deltas.
 * Callers provide live, writable int32_t/int64_t objects aligned to 4/8 bytes;
 * do not mix atomic access with concurrent ordinary reads/writes or packed data.
 * The x86 64-bit path requires CMPXCHG8B (the selected XP i686 CPU baseline).
 * These operations are lock-free, not wait-free. No OS API or lock fallback is
 * admitted. Actual compiler code generation/imports remain target validation.
 */
#ifndef JCE_NATIVE_ATOMIC_INTEGER_INTERNAL_H
#define JCE_NATIVE_ATOMIC_INTEGER_INTERNAL_H

#include "jce_native_atomic_word_internal.h"
#include <stdint.h>

/* Avoid an out-of-range unsigned-to-signed cast and ordinary signed overflow. */
JCE_INLINE int32_t jce_native_atomic_i32_from_bits(uint32_t bits)
{
    return bits <= (uint32_t)INT32_MAX ? (int32_t)bits :
        INT32_MIN + (int32_t)(bits - (UINT32_C(1) << 31));
}

JCE_INLINE int64_t jce_native_atomic_i64_from_bits(uint64_t bits)
{
    return bits <= (uint64_t)INT64_MAX ? (int64_t)bits :
        INT64_MIN + (int64_t)(bits - (UINT64_C(1) << 63));
}

typedef char JceNativeInteger32HasExactWidth[sizeof(int32_t) == 4 ? 1 : -1];
typedef char JceNativeInteger64HasExactWidth[sizeof(int64_t) == 8 ? 1 : -1];

#if JCE_COMPILER_MSVC
#if !(JCE_ARCH_X86 || JCE_ARCH_X64 || JCE_ARCH_ARM || JCE_ARCH_ARM64)
#error "JCE native integer MSVC architecture needs an explicit lock-free adapter"
#endif
#define JCE_NATIVE_ATOMIC_I32_ALIGN __declspec(align(4))
#define JCE_NATIVE_ATOMIC_I64_ALIGN __declspec(align(8))
typedef char JceNativeIntegerMsvcLongIs32[sizeof(long) == 4 ? 1 : -1];
typedef char JceNativeIntegerMsvcLongLongIs64[sizeof(__int64) == 8 ? 1 : -1];
#pragma intrinsic(_InterlockedExchangeAdd)
#pragma intrinsic(_InterlockedCompareExchange64)

JCE_INLINE int32_t jce_native_atomic_i32_add(int32_t *value, int32_t delta)
{
    long previous = _InterlockedExchangeAdd((volatile long *)value, (long)delta);
    uint32_t post = (uint32_t)previous + (uint32_t)delta;
    return jce_native_atomic_i32_from_bits(post);
}

JCE_INLINE int64_t jce_native_atomic_i64_add(int64_t *value, int64_t delta)
{
    volatile __int64 *target = (volatile __int64 *)value;
    __int64 observed = _InterlockedCompareExchange64(target, 0, 0);
    for (;;) {
        uint64_t post = (uint64_t)observed + (uint64_t)delta;
        __int64 desired = (__int64)jce_native_atomic_i64_from_bits(post);
        __int64 previous = _InterlockedCompareExchange64(target, desired, observed);
        if (previous == observed) return (int64_t)desired;
        observed = previous;
    }
}

/* Existing MSVC interlocked word operations supply a full barrier. */
JCE_INLINE size_t jce_native_atomic_word_load_acquire(const JceNativeAtomicWord *word)
{ return jce_native_atomic_word_load(word); }
JCE_INLINE void jce_native_atomic_word_store_release(JceNativeAtomicWord *word, size_t value)
{ jce_native_atomic_word_store(word, value); }
JCE_INLINE bool jce_native_atomic_word_compare_exchange_acq_rel(JceNativeAtomicWord *word,
    size_t expected, size_t desired)
{ return jce_native_atomic_word_compare_exchange(word, expected, desired); }

#elif JCE_COMPILER_GCC || JCE_COMPILER_CLANG
#define JCE_NATIVE_ATOMIC_I32_ALIGN __attribute__((aligned(4)))
#define JCE_NATIVE_ATOMIC_I64_ALIGN __attribute__((aligned(8)))
/* A signed object may be viewed through these explicitly alias-safe unsigned
 * types. Atomic unsigned addition supplies defined modulo arithmetic. */
typedef uint32_t JceNativeAtomicI32Bits __attribute__((__may_alias__, aligned(4)));
typedef uint64_t JceNativeAtomicI64Bits __attribute__((__may_alias__, aligned(8)));
typedef char JceNativeInteger32MustBeLockFree[
    __atomic_always_lock_free(sizeof(JceNativeAtomicI32Bits), 0) ? 1 : -1];
typedef char JceNativeInteger64MustBeLockFree[
    __atomic_always_lock_free(sizeof(JceNativeAtomicI64Bits), 0) ? 1 : -1];

JCE_INLINE int32_t jce_native_atomic_i32_add(int32_t *value, int32_t delta)
{
    uint32_t post = __atomic_add_fetch((JceNativeAtomicI32Bits *)value,
        (uint32_t)delta, __ATOMIC_SEQ_CST);
    return jce_native_atomic_i32_from_bits(post);
}

JCE_INLINE int64_t jce_native_atomic_i64_add(int64_t *value, int64_t delta)
{
    uint64_t post = __atomic_add_fetch((JceNativeAtomicI64Bits *)value,
        (uint64_t)delta, __ATOMIC_SEQ_CST);
    return jce_native_atomic_i64_from_bits(post);
}

JCE_INLINE size_t jce_native_atomic_word_load_acquire(const JceNativeAtomicWord *word)
{ return __atomic_load_n(&word->value, __ATOMIC_ACQUIRE); }
JCE_INLINE void jce_native_atomic_word_store_release(JceNativeAtomicWord *word, size_t value)
{ __atomic_store_n(&word->value, value, __ATOMIC_RELEASE); }
JCE_INLINE bool jce_native_atomic_word_compare_exchange_acq_rel(JceNativeAtomicWord *word,
    size_t expected, size_t desired)
{ return __atomic_compare_exchange_n(&word->value, &expected, desired, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
#else
#error "JCE native integer atomics require a supported lock-free compiler adapter"
#endif

#endif
