#ifndef JCE_TERRAIN_SURFACE_INTERNAL_H
#define JCE_TERRAIN_SURFACE_INTERNAL_H

#include <jce/middleware/scene/jce_terrain.h>
#include <jce/os/core/jce_math.h>

/* Borrowed heightfield plus its prepared world projection. */
typedef struct {
    JceTerrain *terrain;
    jce_mat4 world;
    jce_mat4 inverse;
    jce_mat4 normal_matrix;
    jce_vec3 world_min;
    jce_vec3 world_max;
    float local_min_y;
    float local_max_y;
    bool upright;
} JceTerrainSurface;

bool jce_terrain_surface_init(JceTerrainSurface *out, JceTerrain *terrain,
    const jce_mat4 *world);

/* Project a vertical world ray onto the transformed heightfield. */
bool jce_terrain_surface_sample(const JceTerrainSurface *surface,
    float world_x, float world_z, bool want_normal,
    float *out_world_y, jce_vec3 *out_normal);

/* Sample all cell centres of a square world-space grid, in row-major +Z.
 * Elevations are relative to still_y and must be within [-64, 64] metres.
 * Requires resident monolithic heights and coverage without cut cells.
 * Terrain is borrowed for this call. Discard the scratch bed on failure. */
bool jce_terrain_surface_sample_bed(JceTerrain *terrain, const jce_mat4 *world,
    unsigned side, float cell_size, float min_x, float min_z, float still_y,
    float *bed);

#endif
