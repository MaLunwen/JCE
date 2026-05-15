/*
 * jce_frame_debugger.c  Per-pass GPU timing ring.
 */

#include <jce/renderer/jce_frame_debugger.h>

#include <string.h>

static JceFrameDbgFrame s_frames[JCE_FRAMEDBG_FRAMES_KEPT];
static uint32_t         s_head;            /* index of newest frame */
static uint32_t         s_frame_counter;

bool jce_framedbg_push_pass(const JceFrameDbgPass *pass)
{
    if (!pass) return false;
    JceFrameDbgFrame *f = &s_frames[s_head % JCE_FRAMEDBG_FRAMES_KEPT];
    if (!f->active) {
        memset(f, 0, sizeof(*f));
        f->frame_index = s_frame_counter;
        f->active = true;
    }
    if (f->pass_count >= JCE_FRAMEDBG_PASSES_PER_FRAME) return false;
    JceFrameDbgPass *p = &f->passes[f->pass_count++];
    *p = *pass;
    p->active = true;
    if (pass->gpu_end_ns > pass->gpu_start_ns)
        f->gpu_total_ns += pass->gpu_end_ns - pass->gpu_start_ns;
    return true;
}

void jce_framedbg_next_frame(void)
{
    s_frame_counter++;
    s_head = (s_head + 1) % JCE_FRAMEDBG_FRAMES_KEPT;
    JceFrameDbgFrame *f = &s_frames[s_head];
    memset(f, 0, sizeof(*f));
    f->frame_index = s_frame_counter;
    f->active = true;
}

const JceFrameDbgFrame *jce_framedbg_get_frame(uint32_t age)
{
    if (age >= JCE_FRAMEDBG_FRAMES_KEPT) return NULL;
    uint32_t idx = (s_head + JCE_FRAMEDBG_FRAMES_KEPT - age) %
                    JCE_FRAMEDBG_FRAMES_KEPT;
    return s_frames[idx].active ? &s_frames[idx] : NULL;
}

void jce_framedbg_reset(void)
{
    memset(s_frames, 0, sizeof(s_frames));
    s_head = 0;
    s_frame_counter = 0;
}

uint32_t jce_framedbg_current_frame_index(void)
{
    return s_frame_counter;
}
