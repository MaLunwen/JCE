/*
 * msvc_compat.h  GCC builtin shims for MSVC, force-included into libhevc
 *                sources when building with MSVC ARM64.
 *
 * libhevc's pure-C decoder uses `__sync_synchronize`, `__builtin_clz`,
 * `__builtin_ctz` (GCC builtins) and references `ihevcd_init_function_ptr_av8`
 * which we don't compile on MSVC. Provide MSVC equivalents and a stub.
 */

#ifndef JCE_LIBHEVC_MSVC_COMPAT_H
#define JCE_LIBHEVC_MSVC_COMPAT_H

#ifdef _MSC_VER

#include <intrin.h>

/* Atomic full memory barrier */
#define __sync_synchronize() MemoryBarrier()

/* Count leading zeros */
static __inline int __builtin_clz(unsigned int x) {
    unsigned long idx;
    _BitScanReverse(&idx, x);
    return 31 - (int)idx;
}

/* Count leading zeros (long long) */
static __inline int __builtin_clzll(unsigned long long x) {
    unsigned long idx;
#if defined(_M_X64) || defined(_M_ARM64)
    _BitScanReverse64(&idx, x);
    return 63 - (int)idx;
#else
    if (_BitScanReverse(&idx, (unsigned long)(x >> 32))) return 31 - (int)idx;
    _BitScanReverse(&idx, (unsigned long)x);
    return 63 - (int)idx;
#endif
}

/* Count trailing zeros */
static __inline int __builtin_ctz(unsigned int x) {
    unsigned long idx;
    _BitScanForward(&idx, x);
    return (int)idx;
}

/* Stub for the missing av8 (NEON) init: route to the no-NEON path. */
struct _codec_t;
void ihevcd_init_function_ptr_noneon(struct _codec_t *ps_codec);
static __inline void ihevcd_init_function_ptr_av8(struct _codec_t *ps_codec) {
    ihevcd_init_function_ptr_noneon(ps_codec);
}

#endif /* _MSC_VER */

#endif /* JCE_LIBHEVC_MSVC_COMPAT_H */
