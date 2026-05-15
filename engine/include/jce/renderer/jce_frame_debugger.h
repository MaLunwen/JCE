/*
 * jce_frame_debugger.h  Per-pass GPU timestamp ring.
 *
 * Mirrors Unity Frame Debugger / RenderDoc-lite: every render pass
 * pushes a (name, gpu_start_ns, gpu_end_ns) record into a ring,
 * which the editor's Frame Debugger panel reads each frame to draw
 * a Gantt-style horizontal bar chart.
 *
 * Ring-buffer storage so we keep the last N frames of data.  GPU
 * timestamps come from the renderer's bgfx::frame() encoder block in
 * B17.5 (wire-up batch); this module is the pure data carrier so
 * the panel can render with no bgfx coupling.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_FRAME_DEBUGGER_H
#define JCE_FRAME_DEBUGGER_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_FRAMEDBG_PASSES_PER_FRAME 64
#define JCE_FRAMEDBG_FRAMES_KEPT      4
#define JCE_FRAMEDBG_NAME_LEN         32

typedef struct {
    char     name[JCE_FRAMEDBG_NAME_LEN];
    uint64_t gpu_start_ns;
    uint64_t gpu_end_ns;
    uint64_t cpu_start_ns;
    uint64_t cpu_end_ns;
    /* Free-form payload: draw call count, vertex count, etc. */
    uint32_t draw_calls;
    uint32_t vertices;
    bool     active;
} JceFrameDbgPass;

typedef struct {
    uint32_t        frame_index;
    JceFrameDbgPass passes[JCE_FRAMEDBG_PASSES_PER_FRAME];
    uint32_t        pass_count;
    uint64_t        gpu_total_ns;
    bool            active;
} JceFrameDbgFrame;

/* Push a pass into the current frame.  Returns false when the
 * frame's pass cap is reached. */
JCE_API bool jce_framedbg_push_pass(const JceFrameDbgPass *pass);

/* Advance to the next frame.  Old frame contents become inspectable
 * via jce_framedbg_get_frame(0..FRAMES_KEPT-1) with 0 = newest. */
JCE_API void jce_framedbg_next_frame(void);

JCE_API const JceFrameDbgFrame *jce_framedbg_get_frame(uint32_t age);

/* Reset ring (e.g. when the user resets play). */
JCE_API void jce_framedbg_reset(void);

JCE_API uint32_t jce_framedbg_current_frame_index(void);

JCE_EXTERN_C_END

#endif /* JCE_FRAME_DEBUGGER_H */
