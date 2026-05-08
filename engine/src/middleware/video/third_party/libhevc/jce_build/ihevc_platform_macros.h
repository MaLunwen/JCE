/* Generic ihevc_platform_macros.h — used when no platform-specific version applies.
 * Covers: Emscripten/WASM, MSVC ARM64 (Windows on ARM), and any other non-x86/non-GCC target.
 * Provides MSVC-compatible and GCC/Clang-compatible branches for all macros. */
#ifndef _IHEVC_PLATFORM_MACROS_H_
#define _IHEVC_PLATFORM_MACROS_H_

#include "ihevc_typedefs.h"

#define CLIP_U8(x)  CLIP3((x), 0,      255)
#define CLIP_S8(x)  CLIP3((x), -128,   127)
#define CLIP_U10(x) CLIP3((x), 0,      1023)
#define CLIP_S10(x) CLIP3((x), -512,   511)
#define CLIP_U12(x) CLIP3((x), 0,      4095)
#define CLIP_S12(x) CLIP3((x), -2048,  2047)
#define CLIP_U14(x) CLIP3((x), 0,      16383)
#define CLIP_S14(x) CLIP3((x), -8192,  8191)
#define CLIP_U16(x) CLIP3((x), 0,      65535)
#define CLIP_S16(x) CLIP3((x), -32768, 32767)

#define SHL(x,y)    (((y) < 32) ? ((x) << (y)) : 0)
#define SHR(x,y)    (((y) < 32) ? ((x) >> (y)) : 0)

#define SHR_NEG(val,shift)  ((shift>0)?(val>>shift):(val<<(-shift)))
#define SHL_NEG(val,shift)  ((shift<0)?(val>>(-shift)):(val<<shift))

#define ITT_BIG_ENDIAN(x) \
    ((x << 24)) | ((x & 0x0000ff00) << 8) | ((x & 0x00ff0000) >> 8) | ((UWORD32)x >> 24)

#define NOP(nop_cnt) do { } while(0)

#define INLINE inline

#ifdef _MSC_VER
/* ── MSVC (including ARM64/x64 cross-compile) ─────────────────── */
#include <intrin.h>

#define POPCNT_U32(x)   __popcnt(x)

static INLINE UWORD32 CLZ(UWORD32 u4_word)
{
    unsigned long index;
    if (_BitScanReverse(&index, u4_word))
        return 31u - index;
    return 31u;
}

static INLINE UWORD32 CLZNZ(UWORD32 u4_word)
{
    unsigned long index;
    _BitScanReverse(&index, u4_word);
    return 31u - index;
}

static INLINE UWORD32 CTZ(UWORD32 u4_word)
{
    unsigned long index;
    if (_BitScanForward(&index, u4_word))
        return (UWORD32)index;
    return 31u;
}

#define DATA_SYNC()  _ReadWriteBarrier()

#define GET_POS_MSB_32(r, word) \
    do { unsigned long _msb_idx; \
         if (_BitScanReverse(&_msb_idx, (word))) { r = (WORD32)_msb_idx; } \
         else { r = -1; } } while(0)

#define GET_POS_MSB_64(r, word) \
    do { unsigned long _msb_idx; \
         if (_BitScanReverse64(&_msb_idx, (word))) { r = (WORD32)_msb_idx; } \
         else { r = -1; } } while(0)

#define GETRANGE(r, word) \
    do { unsigned long _idx; \
         if (_BitScanReverse(&_idx, (word))) { r = (WORD32)(_idx + 1); } \
         else { r = 1; } } while(0)

#define GETRANGE64(r, llword) \
    do { unsigned long _idx; \
         if (_BitScanReverse64(&_idx, (llword))) { r = (WORD32)(_idx + 1); } \
         else { r = 1; } } while(0)

#define MEM_ALIGN8  __declspec(align(8))
#define MEM_ALIGN16 __declspec(align(16))
#define MEM_ALIGN32 __declspec(align(32))

#else
/* ── GCC / Clang (Emscripten, Linux, macOS, etc.) ────────────── */
#define POPCNT_U32(x)   __builtin_popcount(x)

static INLINE UWORD32 CLZ(UWORD32 u4_word)
{
    return u4_word ? __builtin_clz(u4_word) : 31;
}

static INLINE UWORD32 CLZNZ(UWORD32 u4_word)
{
    return __builtin_clz(u4_word);
}

static INLINE UWORD32 CTZ(UWORD32 u4_word)
{
    return u4_word ? (UWORD32)__builtin_ctz(u4_word) : 31;
}

#define DATA_SYNC()  __sync_synchronize()

#define GET_POS_MSB_32(r, word) \
    do { if(word) { r = 31 - __builtin_clz(word); } else { r = -1; } } while(0)

#define GET_POS_MSB_64(r, word) \
    do { if(word) { r = 63 - __builtin_clzll(word); } else { r = -1; } } while(0)

#define GETRANGE(r, word) \
    do { if(word) { r = 32 - __builtin_clz(word); } else { r = 1; } } while(0)

#define GETRANGE64(r, llword) \
    do { if(llword) { r = 64 - __builtin_clzll(llword); } else { r = 1; } } while(0)

#define MEM_ALIGN8  __attribute__((aligned(8)))
#define MEM_ALIGN16 __attribute__((aligned(16)))
#define MEM_ALIGN32 __attribute__((aligned(32)))

#endif /* _MSC_VER */

#define PREFETCH_ENABLE 0
#define PREFETCH(ptr, type) do { } while(0)
#define PLD(a)

#endif /* _IHEVC_PLATFORM_MACROS_H_ */
