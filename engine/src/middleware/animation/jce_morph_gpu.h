/*
 * jce_morph_gpu.h  Blendshape (morph target) deformation on the GPU.
 *
 * jce_morph.h owns the maths and the CPU evaluator and holds NO GPU
 * resources, deliberately, so the import + eval path is unit-testable without
 * a renderer.  This module is the other half: the compute program, the
 * per-JceMorphData device buffers, and the dispatch.
 *
 * WHY IT EXISTS.  The CPU path memcpy's a primitive's FULL interleaved vertex
 * array into a transient buffer and rewrites position/normal on the main
 * thread every time a weight moves -- per instance, per frame, for a face rig
 * that changes weights every frame by definition.  On the baseline this
 * engine targets (one core) that is the animation budget spent on memory
 * traffic.  Here the base vertices and the deltas live on the device once, are
 * SHARED by every instance of the model, and a weight change costs one
 * dispatch and four floats of uniform.
 *
 * THE CPU PATH IS NOT REPLACED, IT IS THE FALLBACK.  Compute needs GL 4.3 /
 * ES 3.1, and the default graphics tier floor is GL 3.1 / GLES 3.0 (`stable`
 * in conan/hooks/hook_bgfx_wasm_fix.py; `modern` is 4.3/3.1 and `current` is
 * 4.6/3.2), so on the default tier the answer is no.  An earlier version of
 * this comment said the floor was profile 120 "by design"; it never was --
 * that number is the shaderc SOURCE profile, which bgfx rewrites at load.
 * jce_morph_gpu_available() answers from bgfx caps, not from either number,
 * and the caller keeps its CPU branch for when the answer is no.
 * That also means the two evaluators must agree: cs_morph_deform.sc is written
 * against jce_morph_apply's formula line for line, including the detail that a
 * normal is renormalised ONLY when the data carries normal deltas.
 *
 * Layer: Animation (Layer 3) -- PRIVATE, beside jce_morph.c.  It holds GPU
 * resources, which reads like a renderer concern, and it cannot live there:
 * its cache key is JceMorphData, a middleware type, and the renderer may not
 * include middleware.  The layer rule is right and the module is what moved.
 */

#ifndef JCE_MORPH_GPU_H
#define JCE_MORPH_GPU_H

#include <jce/middleware/animation/jce_morph.h>
#include <jce/renderer/jce_skinned_mesh.h>
#include <jce/resource/jce_pak_loader.h>

#include <bgfx/c99/bgfx.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Can this backend run the deform at all?  Answers from BGFX_CAPS_COMPUTE, so
 * it is false on the desktop GL 120 profile and on any renderer bgfx brought
 * up without compute.  Cheap and cached; call it per frame if convenient. */
bool jce_morph_gpu_available(void);

/* The flags a caller must create the OUTPUT dynamic vertex buffer with when
 * it intends to feed it to jce_morph_gpu_deform.
 *
 * A COMPUTE_WRITE buffer is a D3D11 USAGE_DEFAULT resource with a UAV and no
 * CPU access, so it is NOT updatable with bgfx_update_dynamic_vertex_buffer.
 * The choice of path therefore has to be made BEFORE the buffer is created,
 * which is why this is a macro the caller reads rather than something this
 * module does on its behalf. */
#define JCE_MORPH_GPU_OUT_FLAGS                                             \
    (BGFX_BUFFER_COMPUTE_WRITE | BGFX_BUFFER_COMPUTE_FORMAT_32X1 |          \
     BGFX_BUFFER_COMPUTE_TYPE_FLOAT)

/* Build (or find) the device buffers for one primitive, and load the program
 * if it is not loaded yet.
 *
 * SEPARATE FROM THE DISPATCH ON PURPOSE.  The caller has to know whether the
 * GPU path will run BEFORE it creates the output buffer, because the flags
 * differ and a COMPUTE_WRITE buffer cannot be written from the CPU: deciding
 * after the buffer exists would leave a primitive whose upload silently does
 * nothing.  So this answers the question first, and jce_morph_gpu_deform then
 * only has to dispatch.
 *
 * Returns false for every reason the GPU path might not be usable -- no
 * compute, no shader blob, a layout that is not 4-byte addressable, the cache
 * full -- and the caller runs jce_morph_apply for that primitive instead. */
bool jce_morph_gpu_prepare(const JcePakArchive *pak,
                           const JceMorphData *md,
                           const JceSkinnedMesh *sm,
                           uint32_t num_verts);

/* Deform one primitive into `out`.
 *
 *   md        the primitive's morph deltas (also the CACHE KEY for the device
 *             buffers this builds from it)
 *   sm        the primitive's retained base vertices + layout
 *   num_verts the bound mesh's vertex count; the caller has already checked it
 *             equals jce_morph_vertex_count(md)
 *   weights   the resolved per-instance weight vector
 *   out       a dynamic vertex buffer created with JCE_MORPH_GPU_OUT_FLAGS and
 *             the same layout as the static one
 *
 * Returns false when the program, the buffers or the caps are not there -- the
 * caller must then run jce_morph_apply, which is why nothing is logged per
 * call: a backend without compute would log once per primitive per frame. */
bool jce_morph_gpu_deform(const JceMorphData *md,
                          const JceSkinnedMesh *sm,
                          uint32_t num_verts,
                          const float *weights, uint32_t num_weights,
                          bgfx_dynamic_vertex_buffer_handle_t out);

/* Drop the device buffers built from `md`.
 *
 * NOT CALLED BY HAND.  jce_morph_gpu.c registers it with the destroy hook in
 * jce_morph_internal.h, so jce_morph_data_destroy invokes it wherever a
 * JceMorphData dies -- today the model destroy and the glTF loader's error
 * path, tomorrow whatever else.  That matters more than tidiness: the cache is
 * keyed by POINTER, so an entry that outlives its key hands the next
 * allocation landing on the same address somebody else's vertices, silently
 * and only sometimes.  A rule that has to be remembered at each new destroy
 * site is a rule that will be missed at one of them.  Exposed anyway because
 * the hook needs a function to point at. */
void jce_morph_gpu_release(const JceMorphData *md);

/* Free the program, the uniforms and every remaining entry.  Renderer
 * shutdown; safe to call when nothing was ever built. */
void jce_morph_gpu_shutdown(void);

/* How many cache entries are live, for logging and for the tests.  */
uint32_t jce_morph_gpu_entry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_MORPH_GPU_H */
