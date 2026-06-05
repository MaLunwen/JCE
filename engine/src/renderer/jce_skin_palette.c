#include <jce/renderer/jce_skin_palette.h>

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
