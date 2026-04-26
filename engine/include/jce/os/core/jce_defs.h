/*
 * jce_defs.h  Public ABI and platform/compiler definitions.
 *
 * This header is intentionally dependency-free.  It is safe for C, C++,
 * and foreign-function-interface generators to include before any other
 * JCE header.
 */

#ifndef JCE_DEFS_H
#define JCE_DEFS_H

#include <stddef.h>
#include <stdint.h>

/* -- C ABI ---------------------------------------------------------- */

#ifdef __cplusplus
#define JCE_EXTERN_C_BEGIN extern "C" {
#define JCE_EXTERN_C_END   }
#else
#define JCE_EXTERN_C_BEGIN
#define JCE_EXTERN_C_END
#endif

/* -- Compiler ------------------------------------------------------- */

#if defined(_MSC_VER)
#define JCE_COMPILER_MSVC 1
#else
#define JCE_COMPILER_MSVC 0
#endif

#if defined(__clang__)
#define JCE_COMPILER_CLANG 1
#else
#define JCE_COMPILER_CLANG 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define JCE_COMPILER_GCC 1
#else
#define JCE_COMPILER_GCC 0
#endif

/* -- Platform ------------------------------------------------------- */

#if defined(_WIN32)
#define JCE_PLATFORM_WINDOWS 1
#else
#define JCE_PLATFORM_WINDOWS 0
#endif

#if defined(__APPLE__)
#define JCE_PLATFORM_APPLE 1
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define JCE_PLATFORM_IOS 1
#define JCE_PLATFORM_MACOS 0
#else
#define JCE_PLATFORM_IOS 0
#define JCE_PLATFORM_MACOS 1
#endif
#else
#define JCE_PLATFORM_APPLE 0
#define JCE_PLATFORM_IOS 0
#define JCE_PLATFORM_MACOS 0
#endif

#if defined(__ANDROID__)
#define JCE_PLATFORM_ANDROID 1
#else
#define JCE_PLATFORM_ANDROID 0
#endif

#if defined(__linux__) && !JCE_PLATFORM_ANDROID
#define JCE_PLATFORM_LINUX 1
#else
#define JCE_PLATFORM_LINUX 0
#endif

#if defined(__EMSCRIPTEN__)
#define JCE_PLATFORM_WEB 1
#else
#define JCE_PLATFORM_WEB 0
#endif

/* -- Architecture --------------------------------------------------- */

#if defined(_M_X64) || defined(__x86_64__)
#define JCE_ARCH_X64 1
#else
#define JCE_ARCH_X64 0
#endif

#if defined(_M_IX86) || defined(__i386__)
#define JCE_ARCH_X86 1
#else
#define JCE_ARCH_X86 0
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
#define JCE_ARCH_ARM64 1
#else
#define JCE_ARCH_ARM64 0
#endif

#if defined(_M_ARM) || defined(__arm__)
#define JCE_ARCH_ARM 1
#else
#define JCE_ARCH_ARM 0
#endif

/* -- Symbol visibility / calling convention ------------------------ */

#ifndef JCE_CALL
#if JCE_PLATFORM_WINDOWS
#define JCE_CALL __cdecl
#else
#define JCE_CALL
#endif
#endif

#ifndef JCE_API
#if defined(JCE_SHARED) && JCE_PLATFORM_WINDOWS
#if defined(JCE_BUILDING_ENGINE)
#define JCE_API __declspec(dllexport)
#else
#define JCE_API __declspec(dllimport)
#endif
#elif defined(JCE_SHARED) && (JCE_COMPILER_GCC || JCE_COMPILER_CLANG)
#define JCE_API __attribute__((visibility("default")))
#else
#define JCE_API
#endif
#endif

#ifndef JCE_INLINE
#if JCE_COMPILER_MSVC
#define JCE_INLINE static __inline
#else
#define JCE_INLINE static inline
#endif
#endif

/* -- ABI-stable scalar typedefs ------------------------------------ */
/*
 * Foreign-language bindings (JNI, C#, Python ctypes, Wasm imports, ...)
 * cannot reliably interoperate with `size_t` (LP64 vs LLP64) or the C99
 * `_Bool` (sizeof varies between 1 and 4 bytes across ABIs).  New JCE
 * public APIs SHOULD use these stable aliases so binding generators do
 * not need per-platform plumbing.  Existing APIs that already use
 * `size_t`/`bool` remain valid; migrate opportunistically.
 */
typedef uint64_t JceSize;  /* sizes / counts in the public ABI */
typedef int64_t JceOffset; /* signed offsets / cursor positions */
typedef uint8_t JceBool;   /* boolean values; 0 = false, !=0 = true */

#ifndef JCE_TRUE
#define JCE_TRUE ((JceBool)1)
#define JCE_FALSE ((JceBool)0)
#endif

#endif /* JCE_DEFS_H */
