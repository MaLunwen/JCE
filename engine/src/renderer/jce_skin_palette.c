#include <jce/renderer/jce_skin_palette.h>

bool jce_skin_bone_world(const jce_mat4 *root,
                         const jce_mat4 *palette,
                         uint32_t palette_count,
                         const jce_mat4 *inverse_bind,
                         uint32_t bone_index,
                         jce_mat4 *out_world)
{
    if (!inverse_bind || !out_world)
        return false;

    /* bind = inverseBind^-1.  Needed in both branches: it IS the bone's
     * global transform at rest, and it is what cancels the inverseBind the
     * palette carries. */
    const jce_mat4 bind = jce_m4_inverse(inverse_bind);

    jce_mat4 global;
    if (palette && bone_index < palette_count) {
        /* palette[i] = global * inverseBind  =>  global = palette[i] * bind.
         * Same order as jce_skeleton.h:92 states it. */
        global = jce_m4_multiply(&palette[bone_index], &bind);
    } else {
        /* No palette, or a bone the palette does not reach: the bind pose is
         * the honest answer, not a failure.  skin_palette_count == 0 is this
         * renderer's way of saying "nothing is playing, draw the bind pose". */
        global = bind;
    }

    *out_world = root ? jce_m4_multiply(root, &global) : global;
    return true;
}

uint32_t jce_skin_build_world_palette(const jce_mat4 *root,
                                      const jce_mat4 *joints,
                                      uint32_t num_joints,
                                      jce_mat4 *out,
                                      uint32_t out_cap)
{
    if (!joints || !out || num_joints == 0 || out_cap == 0)
        return 0;

    uint32_t n = num_joints < out_cap ? num_joints : out_cap;

    if (root) {
        for (uint32_t i = 0; i < n; i++)
            out[i] = jce_m4_multiply(root, &joints[i]);
    } else {
        for (uint32_t i = 0; i < n; i++)
            out[i] = joints[i];
    }
    return n;
}
