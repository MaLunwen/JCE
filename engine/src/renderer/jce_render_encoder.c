#include "renderer/jce_render_encoder.h"
#if defined(_MSC_VER)
__declspec(thread) bgfx_encoder_t *jce_tls_encoder = NULL;
#else
__thread bgfx_encoder_t *jce_tls_encoder = NULL;
#endif

/* Per-frame transform-matrix pressure counter (see header).  Owned here;
 * reset + peak-tracked + overflow-warned in jce_renderer_end_frame. */
uint32_t jce_dbg_xform_matrices = 0;
