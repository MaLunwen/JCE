/*
 * jce_os.h  Platform detection and portability macros.
 *
 * Centralises all platform/compiler detection so the rest of the
 * engine never needs raw #ifdef _WIN32 / __ANDROID__ / etc.
 *
 * Layer: Foundation (Layer 1 — no engine dependencies).
 */

#ifndef JCE_OS_H
#define JCE_OS_H

/* ================================================================== */
/* Platform detection                                                  */
/* ================================================================== */

#if defined(__EMSCRIPTEN__)
  #define JCE_PLATFORM_WEB     1
#elif defined(__ANDROID__)
  #define JCE_PLATFORM_ANDROID 1
#elif defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IOS || TARGET_OS_TV
    #define JCE_PLATFORM_IOS   1
  #else
    #define JCE_PLATFORM_MACOS 1
  #endif
#elif defined(_WIN32)
  #define JCE_PLATFORM_WINDOWS 1
#elif defined(__linux__)
  #define JCE_PLATFORM_LINUX   1
#else
  #error "Unsupported platform"
#endif

/* Convenience groupings. */
#if defined(JCE_PLATFORM_ANDROID) || defined(JCE_PLATFORM_IOS)
  #define JCE_PLATFORM_MOBILE  1
#endif

#if defined(JCE_PLATFORM_WINDOWS) || defined(JCE_PLATFORM_MACOS) || \
    defined(JCE_PLATFORM_LINUX)
  #define JCE_PLATFORM_DESKTOP 1
#endif

/* ================================================================== */
/* Architecture detection                                              */
/* ================================================================== */

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
  #define JCE_ARCH_X64  1
#elif defined(__i386__) || defined(_M_IX86)
  #define JCE_ARCH_X86  1
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define JCE_ARCH_ARM64 1
#elif defined(__arm__) || defined(_M_ARM)
  #define JCE_ARCH_ARM   1
#elif defined(__wasm__)
  #define JCE_ARCH_WASM  1
#endif

/* ================================================================== */
/* Compiler detection                                                  */
/* ================================================================== */

#if defined(_MSC_VER)
  #define JCE_COMPILER_MSVC  1
#elif defined(__clang__)
  #define JCE_COMPILER_CLANG 1
#elif defined(__GNUC__)
  #define JCE_COMPILER_GCC   1
#endif

/* ================================================================== */
/* Export / import macros (shared library boundaries)                   */
/* ================================================================== */

#if defined(JCE_COMPILER_MSVC)
  #define JCE_EXPORT __declspec(dllexport)
  #define JCE_IMPORT __declspec(dllimport)
#elif defined(JCE_COMPILER_GCC) || defined(JCE_COMPILER_CLANG)
  #define JCE_EXPORT __attribute__((visibility("default")))
  #define JCE_IMPORT
#else
  #define JCE_EXPORT
  #define JCE_IMPORT
#endif

#ifdef JCE_BUILD_SHARED
  #define JCE_API JCE_EXPORT
#elif defined(JCE_USE_SHARED)
  #define JCE_API JCE_IMPORT
#else
  #define JCE_API
#endif

/* ================================================================== */
/* Portability helpers                                                 */
/* ================================================================== */

#if defined(JCE_COMPILER_MSVC)
  #define JCE_INLINE       __forceinline
  #define JCE_THREAD_LOCAL __declspec(thread)
#elif defined(JCE_COMPILER_GCC) || defined(JCE_COMPILER_CLANG)
  #define JCE_INLINE       static inline __attribute__((always_inline))
  #define JCE_THREAD_LOCAL __thread
#else
  #define JCE_INLINE       static inline
  #define JCE_THREAD_LOCAL
#endif

#define JCE_UNUSED(x) ((void)(x))

/* C11 _Static_assert fallback for C99. */
#if __STDC_VERSION__ >= 201112L
  #define JCE_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
  #define JCE_STATIC_ASSERT(cond, msg) \
      typedef char jce_static_assert_##__LINE__[(cond) ? 1 : -1]
#endif

/* Array element count. */
#define JCE_COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif /* JCE_OS_H */
