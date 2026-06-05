#include <jce/renderer/jce_local_shadow.h>

#include <math.h>

jce_mat4 jce_local_shadow_vp(jce_vec3 pos, jce_vec3 dir,
                             float fov_rad, float near_z, float far_z,
                             bool homogeneous_depth)
{
    if (fov_rad < 0.01f) fov_rad = 0.01f;
    if (fov_rad > 3.10f) fov_rad = 3.10f;
    if (near_z  < 1e-4f) near_z  = 1e-4f;
    if (far_z   <= near_z) far_z = near_z + 1.0f;

    jce_vec3 d = jce_v3_normalize(dir);
    /* Degenerate aim (zero dir) -> default to straight down. */
    if (d.x == 0.0f && d.y == 0.0f && d.z == 0.0f)
        d = jce_v3(0.0f, -1.0f, 0.0f);

    jce_vec3 center = jce_v3_add(pos, d);
    /* Avoid a degenerate up vector when the aim is near-vertical. */
    jce_vec3 up = (fabsf(d.y) > 0.99f) ? jce_v3(0.0f, 0.0f, 1.0f)
                                       : jce_v3(0.0f, 1.0f, 0.0f);

    jce_mat4 view = jce_m4_look_at(pos, center, up);
    jce_mat4 proj = jce_m4_perspective(fov_rad, 1.0f, near_z, far_z,
                                       homogeneous_depth);
    return jce_m4_multiply(&proj, &view);
}

bool jce_local_shadow_atlas_tile(uint32_t slot, uint32_t atlas_size,
                                 uint32_t tiles_per_side,
                                 uint16_t *out_x, uint16_t *out_y,
                                 uint16_t *out_size)
{
    if (tiles_per_side == 0 || atlas_size == 0) return false;
    if (slot >= tiles_per_side * tiles_per_side) return false;

    uint32_t tile = atlas_size / tiles_per_side;
    uint32_t col  = slot % tiles_per_side;
    uint32_t row  = slot / tiles_per_side;

    if (out_x)    *out_x    = (uint16_t)(col * tile);
    if (out_y)    *out_y    = (uint16_t)(row * tile);
    if (out_size) *out_size = (uint16_t)tile;
    return true;
}
