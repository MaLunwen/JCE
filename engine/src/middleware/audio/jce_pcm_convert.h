/*
 * jce_pcm_convert.h  PCM sizing primitives for cooked-audio ingestion.
 *
 * Cooked .jceasset clips carry a file-controlled byte count and channel
 * count.  When converting 8-bit PCM to the engine's int16 mixing format the
 * output buffer must be sized for, and written to, whole frames only —
 * sizing for whole frames but writing pcm_size samples overflows the heap
 * when the byte count is not a multiple of the channel count (audit R2F8).
 * This is the single source of truth for that sample count.
 *
 * Header-only (static inline) so the loader and its tests share one
 * definition without a link dependency.
 */

#ifndef JCE_PCM_CONVERT_H
#define JCE_PCM_CONVERT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Number of int16 samples produced when converting `src_bytes` of 8-bit PCM
 * at `channels` channels — counting WHOLE FRAMES ONLY.  A trailing partial
 * frame (src_bytes % channels != 0) is dropped, so the caller can both
 * allocate and write exactly this many samples with no overflow.  Returns 0
 * when channels == 0 (no divide) or there is no whole frame.
 */
static inline size_t jce_pcm_u8_to_s16_samples(uint32_t src_bytes,
                                               uint16_t channels)
{
    if (channels == 0) return 0;
    return (size_t)(src_bytes / channels) * channels;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_PCM_CONVERT_H */
