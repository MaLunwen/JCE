/*
 * jce_gpu_vendor.h  GPU vendor PCI IDs and the rules derived from them.
 *
 * bgfx_caps_t::vendorId is the only hardware identity bgfx exposes, and the
 * renderer asks three different questions of it:
 *
 *   1. What do we call this vendor in a log line / the GPU name string?
 *   2. Is this a DISCRETE desktop part (dedicated VRAM on its own bus)?
 *      Gates the VRAM/bandwidth-hungry effects (SSR, TAA, SSAO) and the
 *      integrated-GPU dynamic-resolution path.
 *   3. May this part auto-detect into the HIGH quality tier?
 *
 * (2) and (3) are deliberately NOT the same predicate — Apple Silicon is not
 * discrete but its unified memory is fast enough to run at HIGH, whereas an
 * Intel / ARM / Qualcomm iGPU is not.  They are kept next to each other here
 * precisely so that difference stays intentional instead of drifting apart in
 * copies scattered over several call sites.
 *
 * Header-only (static inline) so no extra translation unit or link edge is
 * needed, and bgfx-free so the constants stay usable from a platform TU.
 */

#ifndef JCE_GPU_VENDOR_H
#define JCE_GPU_VENDOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PCI SIG vendor IDs, as reported by bgfx_caps_t::vendorId.  AMD / Apple /
   Intel / NVIDIA mirror bgfx's own BGFX_PCI_ID_*; ARM and Qualcomm have no
   bgfx equivalent, so all six live here rather than half in each place.  Kept
   as plain (signed-fitting) literals so comparing them against the promoted
   uint16_t vendorId raises no signed/unsigned warning under /W4 or -Wextra. */
#define JCE_GPU_VENDOR_AMD        0x1002
#define JCE_GPU_VENDOR_NVIDIA     0x10DE
#define JCE_GPU_VENDOR_APPLE      0x106B
#define JCE_GPU_VENDOR_ARM        0x13B5
#define JCE_GPU_VENDOR_QUALCOMM   0x5143
#define JCE_GPU_VENDOR_INTEL      0x8086

/* Human-readable vendor name; "Unknown" for anything unrecognised (software
   rasterisers, virtualised adapters, the NoOp backend's zero vendorId). */
static inline const char *jce_gpu_vendor_name(uint16_t vendor_id)
{
    switch (vendor_id) {
    case JCE_GPU_VENDOR_AMD:      return "AMD";
    case JCE_GPU_VENDOR_NVIDIA:   return "NVIDIA";
    case JCE_GPU_VENDOR_INTEL:    return "Intel";
    case JCE_GPU_VENDOR_ARM:      return "ARM";
    case JCE_GPU_VENDOR_APPLE:    return "Apple";
    case JCE_GPU_VENDOR_QUALCOMM: return "Qualcomm";
    default:                      return "Unknown";
    }
}

/* True for a desktop DISCRETE part.  AMD (0x1002) covers both Radeon and the
   APUs — the vendor ID alone cannot disambiguate them, so an APU is treated as
   discrete; that is the historical behaviour and erring towards "discrete"
   here only affects effect gating, never the tier cap. */
static inline bool jce_gpu_vendor_is_discrete(uint16_t vendor_id)
{
    return vendor_id == JCE_GPU_VENDOR_NVIDIA
        || vendor_id == JCE_GPU_VENDOR_AMD;
}

/* True when the part may auto-detect into the HIGH tier.  A superset of
   jce_gpu_vendor_is_discrete(): integrated GPUs have a fraction of a discrete
   part's fill rate and memory bandwidth even when they expose a modern API +
   compute, so they are capped at MEDIUM — but Apple Silicon's unified memory
   is a capable exception and stays uncapped.  (An explicit tier override —
   the editor status-bar picker or JCE_GPU_TIER — bypasses this entirely.) */
static inline bool jce_gpu_vendor_allows_high_tier(uint16_t vendor_id)
{
    return jce_gpu_vendor_is_discrete(vendor_id)
        || vendor_id == JCE_GPU_VENDOR_APPLE;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_GPU_VENDOR_H */
