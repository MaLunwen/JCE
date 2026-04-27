/*
 * jce_compat.h  Cross-platform / cross-architecture compatibility baseline.
 *
 * Declares the minimum OS, compiler, language, and SIMD requirements that
 * every JCE build is guaranteed to satisfy.  Foreign code generators and
 * downstream applications can include this header to assert at compile-
 * time that their environment matches the engine's expectations.
 *
 * Compatibility matrix (as of v0.7.x):
 *   Platform   | Min Version       | Min Arch     | Required SIMD
 *   -----------|-------------------|--------------|--------------
 *   Windows    | 7   (NT 6.1)      | x86_64       | SSE2
 *   macOS      | 10.13 (High Sierra)| x86_64/arm64 | SSE2 / NEON
 *   iOS        | 12.0              | arm64        | NEON
 *   Linux      | glibc 2.28        | x86_64/arm64 | SSE2 / NEON
 *   Android    | API 21 (Lollipop) | arm64-v8a    | NEON
 *   Web (wasm) | wasm32 + simd128  | —            | wasm-SIMD
 *
 * Language baseline: C11 + C++17.
 *
 * This header is intentionally dependency-light and safe for FFI tools.
 */

#ifndef JCE_COMPAT_H
#define JCE_COMPAT_H

#include <jce/os/core/jce_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Language baseline ----------------------------------------------- */

#if defined(__cplusplus)
#if __cplusplus < 201703L && !defined(_MSC_VER)
#error "JCE requires C++17 or later"
#endif
#elif defined(__STDC_VERSION__)
#if __STDC_VERSION__ < 199901L
#error "JCE requires C99 or later"
#endif
#endif

/* -- OS minimum versions -------------------------------------------- */

#if JCE_PLATFORM_WINDOWS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7 */
#elif _WIN32_WINNT < 0x0601
#error "JCE requires _WIN32_WINNT >= 0x0601 (Windows 7)"
#endif
#define JCE_MIN_WIN32_WINNT 0x0601
#endif

#if JCE_PLATFORM_IOS
#include <Availability.h>
#if defined(__IPHONE_OS_VERSION_MIN_REQUIRED) && \
    __IPHONE_OS_VERSION_MIN_REQUIRED < 120000
#error "JCE requires iOS 12.0 or later"
#endif
#define JCE_MIN_IOS_VERSION 120000
#endif

#if JCE_PLATFORM_MACOS
#include <Availability.h>
#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && \
    __MAC_OS_X_VERSION_MIN_REQUIRED < 101300
#error "JCE requires macOS 10.13 (High Sierra) or later"
#endif
#define JCE_MIN_MACOS_VERSION 101300
#endif

#if JCE_PLATFORM_ANDROID
#include <android/api-level.h>
#if defined(__ANDROID_API__) && __ANDROID_API__ < 21
#error "JCE requires Android API level 21 (Lollipop) or later"
#endif
#define JCE_MIN_ANDROID_API 21
#endif

/* -- SIMD baseline guarantees --------------------------------------- */
/*
 * On x86_64 we always have SSE2 (architectural).  On arm64 we always
 * have NEON.  On 32-bit ARM, NEON is required by JCE; on 32-bit x86,
 * SSE2 is required.  These macros let inline helpers select the fast
 * path without runtime dispatch.
 */

#if JCE_ARCH_X64 || JCE_ARCH_X86
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define JCE_HAS_SSE2 1
#else
#define JCE_HAS_SSE2 0
#if JCE_ARCH_X64
#error "JCE requires SSE2 on x86 (always available on x86_64)"
#endif
#endif
#else
#define JCE_HAS_SSE2 0
#endif

#if defined(__SSE4_2__) || (defined(_MSC_VER) && defined(_M_X64))
#define JCE_HAS_SSE42 1
#else
#define JCE_HAS_SSE42 0
#endif

#if defined(__AVX2__)
#define JCE_HAS_AVX2 1
#else
#define JCE_HAS_AVX2 0
#endif

#if JCE_ARCH_ARM64 || JCE_ARCH_ARM
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || JCE_ARCH_ARM64
#define JCE_HAS_NEON 1
#else
#define JCE_HAS_NEON 0
#if JCE_ARCH_ARM64
#error "JCE requires NEON on arm64 (always available)"
#endif
#endif
#else
#define JCE_HAS_NEON 0
#endif

#if defined(__wasm_simd128__)
#define JCE_HAS_WASM_SIMD 1
#else
#define JCE_HAS_WASM_SIMD 0
#endif

/* -- Pointer width assertion ---------------------------------------- */

#if defined(__SIZEOF_POINTER__)
#if __SIZEOF_POINTER__ != 8 && !JCE_ARCH_ARM && !JCE_ARCH_X86 && !JCE_PLATFORM_WEB
#error "JCE expects 64-bit pointers on desktop/server platforms"
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* JCE_COMPAT_H */
