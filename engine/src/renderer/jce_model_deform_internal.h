/*
 * Private model draws with a prepared per-instance vertex source.
 */
#ifndef JCE_MODEL_DEFORM_INTERNAL_H
#define JCE_MODEL_DEFORM_INTERNAL_H

#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>

/* On the CPU-skin tier the callback supplies post-skin vertices for skinned
 * primitives and morphed vertices for rigid primitives. A NULL callback lets
 * a standalone draw prepare CPU skinning from its palette or the rest pose. */
void jce_model_draw_deformed(const JceModel *model, const JceRenderer *renderer,
                             uint16_t view, const jce_mat4 *transform,
                             const jce_mat4 *palette, uint32_t joint_count,
                             JceModelMorphVbCb vb_cb, void *vb_user);

void jce_model_draw_shadow_deformed(const JceModel *model,
                                    const JceRenderer *renderer, uint16_t view,
                                    const jce_mat4 *transform,
                                    const jce_mat4 *palette, uint32_t joint_count,
                                    JceModelMorphVbCb vb_cb, void *vb_user);

/* Share the selection pass's posed geometry. prepare binds color for each draw. */
void jce_model_submit_view_wireframe(
    const JceModel *model, const JceRenderer *renderer, uint16_t view,
    const jce_mat4 *transform, const jce_mat4 *palette, uint32_t joint_count,
    JceModelMorphVbCb vb_cb, void *vb_user, bool overlay,
    void (*prepare)(void *user, bool pbr), void *prepare_user);

void jce_mesh_submit_view_wireframe(const JceMesh *mesh, const JceRenderer *r,
                                    uint16_t view, bool overlay);

/* A pick draw discards bindings after each primitive. Rebind its ID via
 * prepare immediately before every primitive submit. */
void jce_model_submit_pick_id_deformed(
    const JceModel *model, const JceRenderer *renderer, uint16_t view,
    const jce_mat4 *transform, const jce_mat4 *palette, uint32_t joint_count,
    JceShaderHandle static_program, JceShaderHandle skinned_program,
    JceModelMorphVbCb vb_cb, void *vb_user,
    void (*prepare)(void *), void *prepare_user);

#endif
