/* Private CPU-root shadow instancing, shared by scene foliage and models. */
#ifndef JCE_MODEL_INSTANCING_H
#define JCE_MODEL_INSTANCING_H
#include <jce/renderer/jce_model.h>
/* True only when every eligible non-skinned node/primitive root slice was
 * admitted and submitted by CPU calls. False on shader/upload unavailability;
 * this is not GPU completion. level is a reduced-LOD index (0=first reduced
 * IB), matching resident scatter depth; UINT32_MAX chooses each primitive's
 * coarsest available LOD. Without reduced LOD it uses base triangles, independent
 * of color wireframe. Restores the caller's model LOD state. Unsupported skinned
 * primitives remain excluded. */
bool jce_model_draw_shadow_instanced_cpu_lod(const JceModel *, const JceRenderer *,
                                             uint16_t view_id, const jce_mat4 *roots,
                                             uint32_t count, uint32_t level);
#endif
