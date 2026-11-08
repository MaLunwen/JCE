/* Draw-local line state for scene inspection and selection overlays. */
#ifndef JCE_VIEW_WIREFRAME_INTERNAL_H
#define JCE_VIEW_WIREFRAME_INTERNAL_H

#include <bgfx/c99/bgfx.h>

static inline uint64_t jce_view_wireframe_state(bool overlay, uint32_t index_count)
{
    uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                     BGFX_STATE_MSAA | BGFX_STATE_PT_LINES |
                     BGFX_STATE_DEPTH_TEST_LEQUAL;
    if (overlay) {
        if (index_count < 1500000u) state |= BGFX_STATE_LINEAA;
    } else {
        state |= BGFX_STATE_WRITE_Z;
    }
    return state;
}

#endif
