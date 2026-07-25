/*
 * jce_mesh_lod_cook.h  Bundle-time automatic LOD generation.
 *
 * The core moved to the public tree: <jce/resource/jce_mesh_lod.h>.  It has
 * an out-of-engine consumer (the editor's LODGroup inspector previews the
 * chain the cook will bake), and a capability reachable from L7 must be
 * declared once in the ABI surface — not hand-mirrored per consumer.
 *
 * This header stays only so the in-tree cook/bake/test call sites keep
 * their existing include; it adds nothing of its own.
 */

#ifndef JCE_MESH_LOD_COOK_H
#define JCE_MESH_LOD_COOK_H

#include <jce/resource/jce_mesh_lod.h>

#endif /* JCE_MESH_LOD_COOK_H */
