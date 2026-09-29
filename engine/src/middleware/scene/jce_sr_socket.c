/*
 * jce_sr_socket.c  Bone attachments: put an entity where a bone is.
 *
 * A weapon in a hand, a jetpack on a spine, a torch held by an NPC.  Unity
 * does this by PARENTING to a bone, because in Unity a bone IS a Transform in
 * the hierarchy.  Here bones live in the skinning palette and are not scene
 * entities, so JceBoneAttachmentComponent names the bone and this pass writes
 * the attached entity's world pose once per frame.
 *
 * WHY IT LIVES IN THE RENDERER RATHER THAN THE RUNTIME.  The pose it reads is
 * produced by sr_update_skinned_anims, which runs inside the render pass; a
 * runtime pass would necessarily read LAST frame's palette and the weapon
 * would trail the hand by one frame during fast motion -- visible exactly when
 * someone is swinging it.  This runs immediately after the pose eval and
 * before anything draws, so the attachment and the hand are the same frame.
 *
 * THE MATH IS NOT HERE.  jce_skin_bone_world() recovers a bone's world matrix
 * from the palette, and its tests are pure and run without a graphics device
 * -- which this file cannot be, since standing up a JceSceneRenderer needs
 * one.  Keeping the derivation there is what makes the feature testable at
 * all; this file is the wiring.
 *
 * Layer: L4 (middleware/scene).
 */

#include "jce_sr_internal.h"

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_skin_palette.h>

#include <string.h>

/* A zeroed quaternion is not a rotation.  {0,0,0,0} is what memset and a
 * scene file without the keys both produce, and the only reading under which
 * an attachment with no authored rotation points the way the bone does is
 * IDENTITY -- so that is what it means.  Anything else is normalised, because
 * an author typing components by hand will not produce a unit quaternion and
 * a non-unit one scales the attachment. */
static jce_quat socket_rotation(const float q[4])
{
    const float len_sq = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (len_sq < 1e-12f)
        return jce_q_identity();
    return jce_q_normalize(jce_v4(q[0], q[1], q[2], q[3]));
}

void sr_update_bone_attachments(JceSceneRenderer *sr, JceScene *scene,
                                EntityList *list)
{
    if (!sr || !scene || !list) return;

    /* Counts only attachments on entities that THEMSELVES have a parent --
     * see the note at the end of the loop for why roots cost nothing. */
    int wrote_parented = 0;
    for (int i = 0; i < list->count; i++) {
        const JceEntity e = list->entities[i];
        if (!entity_enabled(scene, e)) continue;
        if (!jce_scene_has_bone_attachment(scene, e)) continue;

        JceBoneAttachmentComponent *a = jce_scene_get_bone_attachment(scene, e);
        if (!a || a->target == 0 || a->bone[0] == '\0') continue;
        if (a->target == (uint64_t)e) continue;   /* self-attach: no frame */

        /* The TARGET's animation instance, which is keyed by entity and
         * survives the target being culled this frame.  That is deliberate: a
         * weapon held by someone who walked off screen keeps the pose it had
         * rather than snapping to the origin.  It goes stale, and a pose that
         * stopped updating is a far smaller lie than one that teleported. */
        SrAnimInstance *ai = sr_find_anim_instance(sr, (uint32_t)a->target);
        if (!ai || !ai->model) continue;

        const JceSkeleton *skel = jce_model_get_skeleton(ai->model);
        if (!skel) continue;

        const int bone = jce_skeleton_find_joint(skel, a->bone);
        if (bone < 0) continue;   /* a name the skeleton does not have */

        /* Not cached.  jce_skeleton_find_joint is a linear strcmp over at most
         * JCE_MAX_BONES entries and an attachment count is single digits --
         * weapons and props, not crowds.  A resolved-index cache would have to
         * live in a SERIALISED struct and be invalidated when the model
         * changes, which is a correctness surface bought with a cost nobody
         * has measured. */
        const jce_mat4 inv_bind = jce_skeleton_get_inverse_bind(skel,
                                                               (uint32_t)bone);
        const jce_mat4 root = jce_scene_get_world_matrix(scene,
                                                         (JceEntity)a->target);

        jce_mat4 bone_world;
        if (!jce_skin_bone_world(&root, ai->skin_palette,
                                 ai->skin_palette_count, &inv_bind,
                                 (uint32_t)bone, &bone_world))
            continue;

        /* offset is BONE space: bone_world * TRS(offset, rot, 1). */
        const jce_mat4 local = jce_m4_from_trs(
            jce_v3(a->offset[0], a->offset[1], a->offset[2]),
            socket_rotation(a->rotation_offset),
            jce_v3(1.0f, 1.0f, 1.0f));
        const jce_mat4 want = jce_m4_multiply(&bone_world, &local);

        jce_vec3 wp;
        jce_quat wr;
        jce_m4_decompose(&want, &wp, &wr, NULL);

        /* Solve + write IN PLACE, the same shape the physics write-back uses:
         * jce_scene_set_world_pose would go through jce_scene_set_transform
         * and bump this entity's subtree world-cache generation every frame,
         * which is what naming the entity afterwards exists to avoid. */
        JceTransform *tf = jce_scene_get_transform(scene, e);
        if (!tf) continue;
        jce_scene_solve_local_pose(scene, e, wp, wr, &tf->position,
                                   &tf->rotation);
        jce_scene_notify_physics_writeback_entity(scene, e);
        if (jce_scene_get_parent(scene, e) != JCE_ENTITY_INVALID)
            wrote_parented++;
    }

    /* DROP THIS FRAME'S WORLD-MATRIX MEMO -- once, and only for the case that
     * needs it.
     *
     * The writes above are IN PLACE, and notify_physics_writeback_entity
     * ring-pushes the subtree for the renderer's incremental repair; it does
     * NOT touch scene_world_matrix_memo.  That memo is populated EARLIER IN
     * THE SAME FRAME by the cull walk that built `list`.  So an attached
     * entity would be drawn from a matrix computed before this pass ran:
     * right pose, one frame late, visible only while it is moving -- which is
     * the entire situation a bone attachment exists for, and invisible to
     * anyone testing it on a still character.
     *
     * ONLY FOR PARENTED ENTITIES, because the memo does not hold the others:
     * scene_world_matrix_memo returns a root's local matrix early and never
     * caches it, so a root re-reads the Transform this pass just wrote.  A
     * weapon parented to nothing -- the common shape -- therefore costs
     * nothing here, and a scene full of attachments does not drop its memo
     * every frame for a staleness it cannot have.
     *
     * begin_render_world_cache rather than invalidate_world_cache: the former
     * drops the intra-frame memo WITHOUT bumping the structural epoch, so the
     * renderer's cross-frame static world-matrix/AABB cache survives.  Bumping
     * the epoch would throw that away every frame any character holds
     * anything, which is a far larger bill than the one being avoided. */
    if (wrote_parented > 0)
        jce_scene_begin_render_world_cache(scene);
}
