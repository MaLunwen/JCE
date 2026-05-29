/*
 * jce_terrain.h -- Terrain runtime (heightmap + splat + chunked LOD).
 *
 * A terrain owns a regular WxH height grid and a parallel WxH RGBA8
 * splat map (4 texture layer weights, packed 0..255).  The grid maps
 * to a world-space rectangle [origin, origin + size] on the XZ plane
 * with vertex Y = sample_height(x, z) * max_height + base_y.
 *
 * Chunked LOD: the grid is divided into fixed-size square chunks
 * (terrain_chunk_size verts per side).  Per-chunk meshes are built
 * on demand at a caller-chosen LOD (0 = full res, 1 = every other
 * vertex, 2 = every 4th, ...).
 *
 * Sculpt and splat-paint apply Gaussian-falloff brushes onto the
 * heightmap / splat directly; the editor panel calls these per
 * mouse-drag, the runtime is identical for both edit and play modes.
 *
 * IO: a .terrain side-car binary file ('JCTR' magic, V1 layout,
 * little-endian) holds the bulk arrays; a .terrain.json companion
 * holds metadata (world size, chunk size, layer texture paths) so
 * the asset DB and importer can stay JSON-friendly.
 *
 * Layer: Scene / Terrain (Layer 3) — public.
 */

#ifndef JCE_TERRAIN_PUBLIC_H
#define JCE_TERRAIN_PUBLIC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceTerrain JceTerrain;
struct JcePakArchive;

/* Vertex layout of generated chunk meshes:  (pos.xyz, normal.xyz, uv.xy) */
typedef struct {
    float px, py, pz;
    float nx, ny, nz;
    float u,  v;
} JceTerrainVertex;

typedef enum {
    JCE_TERRAIN_SCULPT_RAISE   = 0,
    JCE_TERRAIN_SCULPT_LOWER   = 1,
    JCE_TERRAIN_SCULPT_SMOOTH  = 2,
    JCE_TERRAIN_SCULPT_FLATTEN = 3
} JceTerrainSculptMode;

/* -- Lifecycle --------------------------------------------------- */

/* Allocate a flat terrain.  width/height are vertex counts (must be
 * >= 2 and ideally `(N * chunk_size) + 1` so an integer number of
 * chunks tile the grid). */
JceTerrain *jce_terrain_create(int width, int height,
                               float world_size_x, float world_size_z,
                               float max_height, int chunk_size);

JCE_API JceTerrain *jce_terrain_load_file(const char *meta_json_path);

/* Same as jce_terrain_load_file but reads the meta JSON and its side-car .bin
 * from a PAK archive (e.g. an embedded engine PAK with bundle overlays).
 * `meta_vpath` is the asset path inside the PAK (e.g.
 * "terrains/sample.terrain.json").  Returns NULL if the meta is not in the
 * PAK (caller may then fall back to jce_terrain_load_file).  When the meta
 * is present but the .bin is missing, an empty terrain is returned. */
JCE_API JceTerrain *jce_terrain_load_from_pak(const struct JcePakArchive *pak,
                                              const char *meta_vpath);
bool        jce_terrain_save_file(const JceTerrain *t,
                                  const char *meta_json_path);

JCE_API void        jce_terrain_free(JceTerrain *t);

/* -- Introspection ---------------------------------------------- */

JCE_API int   jce_terrain_width      (const JceTerrain *t);
JCE_API int   jce_terrain_height     (const JceTerrain *t);
JCE_API float jce_terrain_world_size_x(const JceTerrain *t);
JCE_API float jce_terrain_world_size_z(const JceTerrain *t);
JCE_API float jce_terrain_max_height (const JceTerrain *t);
JCE_API int   jce_terrain_chunk_size (const JceTerrain *t);
JCE_API int   jce_terrain_chunk_count_x(const JceTerrain *t);
JCE_API int   jce_terrain_chunk_count_z(const JceTerrain *t);

/* Direct read access to the raw arrays (heights are normalized 0..1
 * before multiplying by max_height; splat is 8-bit per channel). */
JCE_API const float    *jce_terrain_heights(const JceTerrain *t);
JCE_API const uint32_t *jce_terrain_splat  (const JceTerrain *t);

/* -- Sampling --------------------------------------------------- */

/* Bilinearly samples the heightmap at world XZ; returns 0 outside. */
JCE_API float jce_terrain_sample_height(const JceTerrain *t, float wx, float wz);

/* Bilinearly samples 4-layer splat weights (sum == 1.0). */
void  jce_terrain_sample_splat (const JceTerrain *t, float wx, float wz,
                                float out_w[4]);

/* Vertical raycast (downward only).  Walks a stepwise refinement
 * along the world-XZ projection of the ray; good enough for editor
 * picking and casual physics queries.  Returns true if a hit occurs
 * within [0, max_dist] and writes the hit point + height. */
bool jce_terrain_raycast(const JceTerrain *t,
                         const float origin[3], const float dir[3],
                         float max_dist,
                         float out_hit[3]);

/* -- Mesh generation ------------------------------------------- */

/* Returns the number of vertices and (triangle-list) indices needed
 * for `chunk(cx,cz)` at the given LOD.  Cheaper than a real build
 * call; lets the caller size buffers up front. */
void jce_terrain_chunk_mesh_size(const JceTerrain *t, int cx, int cz, int lod,
                                 int *out_vertex_count, int *out_index_count);

/* Builds the chunk mesh into the caller-allocated buffers.  Returns
 * the actual vertex / index counts written.  Caller is responsible
 * for buffer capacity (use jce_terrain_chunk_mesh_size first). */
void jce_terrain_chunk_build_mesh(const JceTerrain *t, int cx, int cz, int lod,
                                  JceTerrainVertex *out_verts, int v_cap,
                                  uint32_t *out_indices,       int i_cap,
                                  int *out_vertex_count,
                                  int *out_index_count);

/* -- Authoring brushes ----------------------------------------- */

/* Apply a Gaussian-falloff brush at world XZ.  `strength` is per
 * second so the editor panel must scale by frame dt for stable
 * feel.  Radius is in world units. */
void jce_terrain_sculpt_apply(JceTerrain *t,
                              JceTerrainSculptMode mode,
                              float wx, float wz,
                              float radius_world, float strength,
                              float dt);

/* Paint splat layer `layer` (0..3) up at expense of the others. */
void jce_terrain_splat_paint(JceTerrain *t, int layer,
                             float wx, float wz,
                             float radius_world, float strength,
                             float dt);

JCE_EXTERN_C_END

#endif /* JCE_TERRAIN_PUBLIC_H */
