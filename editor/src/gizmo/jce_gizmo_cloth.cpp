/*
 * jce_gizmo_cloth.cpp  P3-C.4 follow-up  Cloth-gizmo Scene View overlay.
 *
 * Visual contract:
 *   - res_u × res_v grid of cloth particles rendered as wireframe
 *     (horizontal + vertical neighbour lines).
 *   - Pinned vertices marked with a small "X" crosshair.
 *
 * Uses jce_cloth_get_positions() to read the live solver state, so
 * the wireframe animates with the simulation when physics is on; in
 * builds where cloth simulation is disabled (no PhysX), the grid is
 * still drawn from the initial positions, which is exactly what the
 * editor needs to author corners / resolution.
 */

#include "jce_gizmo_cloth.h"

extern "C" {
#include <jce/renderer/jce_debug_draw.h>
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/os/core/jce_math.h>
}

#include <stdint.h>
#include <vector>

namespace {

/* Colour palette (0xAABBGGRR). */
constexpr uint32_t COL_GRID   = 0xFFFFFF00u;  /* cyan   */
constexpr uint32_t COL_PINNED = 0xFF0000FFu;  /* red    */

constexpr float PIN_SIZE = 0.04f;

inline jce_vec3 grid_lerp(const JceClothComponent *cl, float u, float v)
{
    jce_vec3 a, b, r;
    a.x = (1.0f - u) * cl->corner_00.x + u * cl->corner_10.x;
    a.y = (1.0f - u) * cl->corner_00.y + u * cl->corner_10.y;
    a.z = (1.0f - u) * cl->corner_00.z + u * cl->corner_10.z;
    b.x = (1.0f - u) * cl->corner_01.x + u * cl->corner_11.x;
    b.y = (1.0f - u) * cl->corner_01.y + u * cl->corner_11.y;
    b.z = (1.0f - u) * cl->corner_01.z + u * cl->corner_11.z;
    r.x = (1.0f - v) * a.x + v * b.x;
    r.y = (1.0f - v) * a.y + v * b.y;
    r.z = (1.0f - v) * a.z + v * b.z;
    return r;
}

} /* anonymous namespace */

extern "C" void jce_gizmo_cloth_draw_from_component(const JceClothComponent *cl)
{
    if (!cl) return;

    uint32_t ru = cl->res_u < 2 ? 2 : cl->res_u;
    uint32_t rv = cl->res_v < 2 ? 2 : cl->res_v;
    uint32_t n  = ru * rv;
    if (n == 0) return;

    /* Pull live solver positions if the runtime cloth exists; otherwise
     * synthesise them from the authored corner grid so the gizmo is
     * useful at edit time before play.
     *
     * Gizmo drawing is main-thread only, so reuse static scratch buffers
     * that grow on demand instead of malloc/free-ing twice every frame. */
    static std::vector<float>    s_flat;
    static std::vector<jce_vec3> s_pos;
    if (s_flat.size() < (size_t)n * 3u) s_flat.resize((size_t)n * 3u);
    if (s_pos.size()  < (size_t)n)      s_pos.resize((size_t)n);
    float    *flat = s_flat.data();
    jce_vec3 *pos  = s_pos.data();

    bool got_live = false;
    if (cl->handle != 0) {
        if (jce_cloth_get_positions((JceClothHandle)cl->handle, flat, n * 3u)) {
            for (uint32_t k = 0; k < n; ++k) {
                pos[k].x = flat[k * 3u + 0];
                pos[k].y = flat[k * 3u + 1];
                pos[k].z = flat[k * 3u + 2];
            }
            got_live = true;
        }
    }
    if (!got_live) {
        for (uint32_t j = 0; j < rv; ++j) {
            float v = (rv > 1) ? (float)j / (float)(rv - 1) : 0.0f;
            for (uint32_t i = 0; i < ru; ++i) {
                float u = (ru > 1) ? (float)i / (float)(ru - 1) : 0.0f;
                pos[j * ru + i] = grid_lerp(cl, u, v);
            }
        }
    }

    /* Horizontal edges. */
    for (uint32_t j = 0; j < rv; ++j) {
        for (uint32_t i = 0; i + 1 < ru; ++i) {
            jce_debug_draw_line(pos[j * ru + i],
                                pos[j * ru + i + 1], COL_GRID);
        }
    }
    /* Vertical edges. */
    for (uint32_t j = 0; j + 1 < rv; ++j) {
        for (uint32_t i = 0; i < ru; ++i) {
            jce_debug_draw_line(pos[j * ru + i],
                                pos[(j + 1) * ru + i], COL_GRID);
        }
    }

    /* Pinned-vertex markers (small "X"). */
    for (uint32_t k = 0; k < cl->pinned_count; ++k) {
        uint32_t idx = cl->pinned_indices[k];
        if (idx >= n) continue;
        jce_vec3 p = pos[idx];
        jce_vec3 a = { p.x - PIN_SIZE, p.y, p.z - PIN_SIZE };
        jce_vec3 b = { p.x + PIN_SIZE, p.y, p.z + PIN_SIZE };
        jce_vec3 c = { p.x - PIN_SIZE, p.y, p.z + PIN_SIZE };
        jce_vec3 d = { p.x + PIN_SIZE, p.y, p.z - PIN_SIZE };
        jce_debug_draw_line(a, b, COL_PINNED);
        jce_debug_draw_line(c, d, COL_PINNED);
    }
}
