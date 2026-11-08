/* A borrowed CPU triangle list. Positions are three interleaved floats. */
#ifndef JCE_PICK_GEOMETRY_INTERNAL_H
#define JCE_PICK_GEOMETRY_INTERNAL_H

#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_skinned_mesh.h>

typedef struct JcePickGeometry {
    const void *positions;
    uint32_t stride;
    uint32_t vertex_count;
    const uint32_t *indices;
    uint32_t index_count;
} JcePickGeometry;

bool jce_skinned_mesh_pick_geometry(const JceSkinnedMesh *mesh,
                                    JcePickGeometry *geometry);

/* World ray parameter t is preserved with scaled, mirrored or singular
 * affine transforms. Indexed and sequential triangle lists use CCW fronts.
 * max_t is exclusive; out_t is written only for a hit. */
bool jce_pick_geometry_ray(const JcePickGeometry *geometry,
                           const jce_mat4 *world, bool double_sided,
                           jce_vec3 origin, jce_vec3 direction,
                           float max_t, float *out_t);

#endif
