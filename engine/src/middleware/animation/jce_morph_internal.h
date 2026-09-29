/*
 * jce_morph_internal.h  The one seam between the morph evaluator and the GPU
 * resources built from its data.
 *
 * jce_morph.h is PUBLIC and bgfx-free and stays that way; this is not an API
 * a user project has any business calling.  It exists so that
 * jce_morph_data_destroy -- the single point where a JceMorphData dies -- can
 * tell jce_morph_gpu.c to drop the device buffers keyed by that pointer,
 * WITHOUT jce_morph.c knowing that a GPU exists.
 *
 * Why a hook and not a direct call: jce_morph.c is the bgfx-free core, and the
 * whole reason it is bgfx-free is that the import and evaluation path can then
 * be unit-tested with no renderer context at all.  A direct call would pull
 * bgfx into every test binary that touches a blendshape.
 *
 * Why not a call at each destroy SITE, which is what this replaces: those
 * sites are engine/src/renderer/ files, and the renderer may not include
 * middleware (check_layer_dependencies).  They were also two sites today and
 * an unknown number tomorrow, each of which would have to remember -- and a
 * pointer-keyed cache that outlives its key does not fail loudly, it hands the
 * next allocation at that address somebody else's vertices.
 */

#ifndef JCE_MORPH_INTERNAL_H
#define JCE_MORPH_INTERNAL_H

#include <jce/middleware/animation/jce_morph.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*JceMorphDestroyHook)(const JceMorphData *m);

/* Install (or clear, with NULL) the hook jce_morph_data_destroy calls just
 * before it frees.  One hook, process-wide: there is one GPU-side owner. */
void jce_morph_set_destroy_hook(JceMorphDestroyHook hook);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MORPH_INTERNAL_H */
