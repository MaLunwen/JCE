/* Triangle prefix and wireframe suffix share one immutable index buffer. */
#ifndef JCE_MESH_INDEX_DATA_H
#define JCE_MESH_INDEX_DATA_H

#include "os/core/jce_memory.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int jce_mesh_edge_compare(const void *lhs, const void *rhs)
{
    const uint32_t *a = lhs, *b = rhs;
    if (a[0] != b[0]) return a[0] < b[0] ? -1 : 1;
    if (a[1] != b[1]) return a[1] < b[1] ? -1 : 1;
    return 0;
}

/* Optional wire topology: NULL leaves the original triangle upload usable.
 * The caller owns the result and uploads only count + *wire_count indices.
 * No pointer is retained after upload. Keep each existing producer's edge
 * convention: procedural meshes deduplicate, PBR/skinned meshes retain pairs. */
static uint32_t *jce_mesh_pack_indices(const uint32_t *indices, uint32_t count,
                                      bool unique_edges, uint32_t *wire_count)
{
    if (!wire_count) return NULL;
    *wire_count = 0;
    const uint32_t triangles = count / 3u;
    const uint32_t max_indices = UINT32_MAX / (uint32_t)sizeof(uint32_t);
    if (!indices || !triangles || count > max_indices ||
        triangles > (max_indices - count) / 6u)
        return NULL;

    uint32_t *packed = JCE_MALLOC((size_t)(count + triangles * 6u) * sizeof(*packed));
    if (!packed) return NULL;
    memcpy(packed, indices, (size_t)count * sizeof(*packed));
    uint32_t *wire = packed + count;
    uint32_t pairs = 0;
    for (uint32_t t = 0; t < triangles; ++t) {
        for (uint32_t edge = 0; edge < 3u; ++edge) {
            uint32_t a = indices[t * 3u + edge];
            uint32_t b = indices[t * 3u + (edge + 1u) % 3u];
            if (unique_edges && a == b) continue;
            if (unique_edges && a > b) {
                const uint32_t swap = a; a = b; b = swap;
            }
            wire[pairs * 2u] = a; wire[pairs * 2u + 1u] = b;
            ++pairs;
        }
    }
    if (!pairs) { JCE_FREE(packed); return NULL; }
    if (unique_edges) {
        qsort(wire, pairs, 2u * sizeof(*wire), jce_mesh_edge_compare);
        uint32_t used = 1;
        for (uint32_t pair = 1; pair < pairs; ++pair) {
            const uint32_t a = wire[pair * 2u], b = wire[pair * 2u + 1u];
            if (a == wire[(used - 1u) * 2u] && b == wire[(used - 1u) * 2u + 1u])
                continue;
            wire[used * 2u] = a; wire[used * 2u + 1u] = b;
            ++used;
        }
        pairs = used;
    }
    *wire_count = pairs * 2u;
    return packed;
}

#endif
