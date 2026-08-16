/*
 * jce_glb_write.h  Minimal binary glTF (.glb) writer.
 *
 * The engine only READS glTF (cgltf); this writes a single-mesh, single-material
 * .glb that the runtime model loader can consume — used by the engine HLOD bake
 * (Direction A2) to emit per-cell proxy meshes.  Faithful C port of the proven
 * writer in tools/worldgen/gen_hlod.py (whose output already loads at
 * runtime): three concatenated blocks (POSITION | NORMAL | indices), a JSON
 * chunk, a BIN chunk.
 */

#ifndef JCE_GLB_WRITE_H
#define JCE_GLB_WRITE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Write `host_path` as a .glb containing ONE mesh / ONE primitive:
 *   positions : tightly-packed float[3] * vertex_count  (required)
 *   normals   : tightly-packed float[3] * vertex_count  (required)
 *   indices   : uint32 * index_count (triangle list, multiple of 3)
 *   base_color: RGBA pbr baseColorFactor (double-sided, metallic 0, rough 1)
 * Returns false on bad args or write failure. */
JCE_API bool jce_glb_write_mesh(const char     *host_path,
                                const float    *positions,
                                const float    *normals,
                                uint32_t        vertex_count,
                                const uint32_t *indices,
                                uint32_t        index_count,
                                const float     base_color[4]);

JCE_EXTERN_C_END

#endif /* JCE_GLB_WRITE_H */
