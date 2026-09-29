/*
 * jce_primitives_selftest.h  Headless check of the 2D quad batcher.
 *
 * Renderer-private, and deliberately free of bgfx: a unit test can include it
 * with only engine/src on the include path, the same shape
 * jce_render_graph.h uses.
 */

#ifndef JCE_PRIMITIVES_SELFTEST_H
#define JCE_PRIMITIVES_SELFTEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Checks the vertex/index generation behind jce_draw_textured_quads_view.
 * Returns true when every case passes; each failure is LOG_ERROR'd with the
 * quad and component that disagreed.  Needs no renderer and no GPU. */
bool jce_primitives_self_test(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PRIMITIVES_SELFTEST_H */
