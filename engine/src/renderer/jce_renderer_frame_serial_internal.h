/* API frame serial ordering, not a GPU fence or presentation observation. */
#ifndef JCE_RENDERER_FRAME_SERIAL_INTERNAL_H
#define JCE_RENDERER_FRAME_SERIAL_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>
typedef enum JceRendererFrameOrder {
    JCE_RENDERER_FRAME_BEFORE=-1,
    JCE_RENDERER_FRAME_EQUAL=0,
    JCE_RENDERER_FRAME_AFTER=1,
    JCE_RENDERER_FRAME_AMBIGUOUS=2
} JceRendererFrameOrder;
/* Related observations must be less than 2^31 advances apart in one backend
 * lifetime. Exactly half the uint32 range cannot establish an ordering.
 * Zero and UINT32_MAX are ordinary frame serials, not invalid sentinels. */
static inline JceRendererFrameOrder jce_renderer_frame_order(uint32_t now,uint32_t target)
{
    uint32_t distance=(uint32_t)(now-target);
    if(!distance) return JCE_RENDERER_FRAME_EQUAL;
    if(distance==UINT32_C(0x80000000)) return JCE_RENDERER_FRAME_AMBIGUOUS;
    return distance<UINT32_C(0x80000000) ? JCE_RENDERER_FRAME_AFTER : JCE_RENDERER_FRAME_BEFORE;
}
static inline bool jce_renderer_frame_ready(uint32_t now,uint32_t target)
{
    JceRendererFrameOrder order=jce_renderer_frame_order(now,target);
    return order==JCE_RENDERER_FRAME_EQUAL || order==JCE_RENDERER_FRAME_AFTER;
}
#endif
