/* Private explicit terrain (8 floats) -> mesh (12 floats) owned conversion.
 * Public JceTerrainVertex ABI is unchanged; no temporary buffer is borrowed. */
#ifndef JCE_SR_TERRAIN_VERTICES_H
#define JCE_SR_TERRAIN_VERTICES_H
#include <stddef.h>
#include <stdint.h>
#include <jce/middleware/scene/jce_terrain.h>
#include <jce/renderer/jce_mesh.h>

typedef struct {
    void *(*allocate)(size_t bytes, void *user);
    void (*release)(void *ptr, void *user);
    void *user;
} SrTerrainVertexAllocator;
typedef struct {
    JceMeshVertex *vertices;
    SrTerrainVertexAllocator allocator;
} SrTerrainVertexOwner;
typedef enum {
    SR_TERRAIN_VERTICES_OK,
    SR_TERRAIN_VERTICES_INVALID,
    SR_TERRAIN_VERTICES_OOM
} SrTerrainVertexResult;
SrTerrainVertexResult sr_terrain_vertices_create(SrTerrainVertexOwner *owner,
    const JceTerrainVertex *source, uint32_t count,
    const SrTerrainVertexAllocator *allocator);
void sr_terrain_vertices_destroy(SrTerrainVertexOwner *owner);
#endif
