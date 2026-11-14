/* Shared Scene/Game Play overlay. Shapes come from the live physics world;
 * authoring components and visual mesh bounds are never substituted here. */
#ifndef JCE_EDITOR_PHYSICS_OVERLAY_H
#define JCE_EDITOR_PHYSICS_OVERLAY_H

#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/os/core/jce_log.h>

#include <cmath>
#include <array>
#include <stdint.h>
#include <limits>

namespace jce_editor_physics_overlay {

/* Shape inspection is bounded before native triangle reads. This additional
 * retained-line budget also bounds contacts/constraints and renderer output. */
constexpr uint64_t line_budget = 32768;

struct Line { jce_vec3 from, to; uint32_t abgr; };

inline std::array<Line, line_budget> &line_storage()
{
    /* Shared sequential render-thread scratch, never a ~1 MiB Windows stack. */
    static std::array<Line, line_budget> lines;
    return lines;
}

struct Sink {
    jce_mat4 view_projection;
    uint64_t visited = 0;
    uint64_t visible = 0;
    uint64_t stride = 1;
    uint32_t stored = 0;
    bool frustum_culling = true;
};

inline bool camera_center(const jce_mat4 &view, jce_vec3 &center)
{
    const jce_mat4 inverse = jce_m4_inverse(&view);
    const jce_vec4 p = jce_m4_mul_v4(&inverse, jce_v4(0, 0, 0, 1));
    if (!std::isfinite(p.w) || std::fabs(p.w) < 1.e-7f) return false;
    center = jce_v3(p.x / p.w, p.y / p.w, p.z / p.w);
    return std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(center.z);
}

inline bool visible_segment(const jce_mat4 &vp, jce_vec3 from, jce_vec3 to)
{
    const jce_vec4 a = jce_m4_mul_v4(&vp, jce_v4(from.x, from.y, from.z, 1));
    const jce_vec4 b = jce_m4_mul_v4(&vp, jce_v4(to.x, to.y, to.z, 1));
    if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z) ||
        !std::isfinite(a.w) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
        !std::isfinite(b.z) || !std::isfinite(b.w)) return false;
    /* Conservative [-w,w] depth also admits all [0,w] backend positions.
     * Reject only when BOTH endpoints are outside the SAME plane: crossing
     * edges survive even when neither endpoint is inside the viewport. */
    return !((a.x < -a.w && b.x < -b.w) || (a.x > a.w && b.x > b.w) ||
             (a.y < -a.w && b.y < -b.w) || (a.y > a.w && b.y > b.w) ||
             (a.z < -a.w && b.z < -b.w) || (a.z > a.w && b.z > b.w));
}

inline bool query_bounds(const jce_mat4 &vp, jce_vec3 &lo, jce_vec3 &hi)
{
    const jce_mat4 inverse = jce_m4_inverse(&vp);
    lo = jce_v3(INFINITY, INFINITY, INFINITY);
    hi = jce_v3(-INFINITY, -INFINITY, -INFINITY);
    /* [-1,1] encloses both supported NDC depth ranges. The native query uses
     * this conservative AABB; the sink still tests the exact view frustum. */
    for (unsigned corner = 0; corner < 8; ++corner) {
        const jce_vec4 v = jce_m4_mul_v4(&inverse, jce_v4(
            corner & 1 ? 1.f : -1.f, corner & 2 ? 1.f : -1.f,
            corner & 4 ? 1.f : -1.f, 1));
        if (!std::isfinite(v.w) || std::fabs(v.w) < 1.e-7f) return false;
        const jce_vec3 p = jce_v3(v.x / v.w, v.y / v.w, v.z / v.w);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        lo = jce_v3(std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z));
        hi = jce_v3(std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z));
    }
    return true;
}

inline void renderer_sink(jce_vec3 from, jce_vec3 to, uint32_t abgr, void *)
{
    jce_debug_draw_line(from, to, abgr);
}

inline void clipped_sink(jce_vec3 from, jce_vec3 to, uint32_t abgr, void *ud)
{
    Sink &sink = *static_cast<Sink *>(ud);
    ++sink.visited;
    if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(from.z) ||
        !std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z)) return;
    if (sink.frustum_culling && !visible_segment(sink.view_projection, from, to)) return;
    const uint64_t ordinal = sink.visible++;
    if (ordinal % sink.stride != 0) return;
    auto &lines = line_storage();
    if (sink.stored == line_budget) {
        /* Compact the complete retained range, then continue at twice the
         * spacing. Later bodies continue to participate after every growth. */
        for (uint32_t i = 0; i < line_budget / 2; ++i) lines[i] = lines[i * 2];
        sink.stored = uint32_t(line_budget / 2);
        sink.stride *= 2;
        if (ordinal % sink.stride != 0) return;
    }
    lines[sink.stored++] = {from, to, abgr};
}

} // namespace jce_editor_physics_overlay

/* false means there is no live world; it is NOT permission to draw component
 * boxes as if they were admitted physics bodies. Both views use this owner.
 * The view's collider toggle owns wireframe; Physics Debugger owns other bits.
 * Public flags and the persistent renderer sink are restored before return. */
inline bool jce_editor_physics_overlay_draw(
    JcePhysicsWorld *world, const jce_mat4 &view, const jce_mat4 &projection,
    uint16_t view_id, const JceRenderer *renderer, bool wireframe,
    bool &budget_warned, float radius = 0.0f, bool frustum_culling = true,
    JceCharacterHandle keep_character = JceCharacterHandle{UINT32_MAX},
    const jce_vec3 *center_override = nullptr)
{
    if (!world || !renderer) return false;
    const uint32_t saved = jce_physics_debug_get_flags();
    const uint32_t requested = (saved & ~uint32_t(JCE_PHYS_DBG_WIREFRAME)) |
        (wireframe ? uint32_t(JCE_PHYS_DBG_WIREFRAME) : 0u);
    if (requested == 0) return true;

    jce_editor_physics_overlay::Sink sink;
    sink.view_projection = jce_m4_multiply(&projection, &view);
    sink.frustum_culling = frustum_culling;
    const uint32_t shape_flags = requested & (JCE_PHYS_DBG_WIREFRAME |
        JCE_PHYS_DBG_AABB | JCE_PHYS_DBG_NORMALS);
    JcePhysicsDebugQuery query{};
    query.struct_size = sizeof(query); query.flags = shape_flags;
    query.max_objects = 2048; query.max_shapes = 2048;
    query.max_mesh_parts = 2048; query.max_triangles = 8192;
    query.max_lines = uint32_t(jce_editor_physics_overlay::line_budget);
    query.range_radius = radius;
    query.keep_character = keep_character;
    query.range_flags = jce_character_valid(keep_character) ? uint32_t(JCE_PHYS_QUERY_KEEP_CHARACTER) : 0u;
    const float extent = std::numeric_limits<float>::max();
    query.bounds_min = jce_v3(-extent, -extent, -extent);
    query.bounds_max = jce_v3(extent, extent, extent);
    bool bounds_ok = jce_editor_physics_overlay::camera_center(view, query.range_center);
    if (center_override && std::isfinite(center_override->x) &&
        std::isfinite(center_override->y) && std::isfinite(center_override->z))
        query.range_center = *center_override;
    if (frustum_culling)
        bounds_ok = bounds_ok && jce_editor_physics_overlay::query_bounds(
            sink.view_projection, query.bounds_min, query.bounds_max);
    JcePhysicsDebugQueryStats stats{}; stats.struct_size = sizeof(stats);
    const bool query_ok = !shape_flags ||
        (bounds_ok &&
         jce_physics_debug_query_shapes(world, &query,
             jce_editor_physics_overlay::clipped_sink, &sink, &stats));
    /* The legacy route is used only for contacts/joints. With neither Bullet
     * wireframe nor AABB enabled it cannot re-enter the whole-mesh draw walk. */
    const uint32_t other_flags = requested & (JCE_PHYS_DBG_CONTACTS | JCE_PHYS_DBG_CONSTRAINTS);
    if (other_flags) {
        jce_physics_debug_set_flags(other_flags);
        jce_physics_debug_set_line_sink(jce_editor_physics_overlay::clipped_sink, &sink);
        jce_physics_debug_flush(world);
    }
    for (uint32_t i = 0; i < sink.stored; ++i) {
        const auto &line = jce_editor_physics_overlay::line_storage()[i];
        jce_debug_draw_line(line.from, line.to, line.abgr);
    }
    /* Never retain the stack Sink in the process-wide physics bridge. */
    jce_physics_debug_set_line_sink(jce_editor_physics_overlay::renderer_sink, nullptr);
    jce_physics_debug_set_flags(saved);
    jce_debug_draw_flush(view_id, renderer);
    if ((!query_ok || stats.status != JCE_PHYS_QUERY_COMPLETE || sink.stride > 1) && !budget_warned) {
        budget_warned = true;
        jce_log_write(JCE_LOG_LEVEL_WARN, "physics_overlay", __FILE__, __LINE__,
            "Physics wireframe is PARTIAL (query_ok=%u status=0x%x, "
            "objects=%u shapes=%u parts=%u triangles=%u/%llu lines=%u). "
            "Sampling or unsupported geometry can omit edges; an unshown edge "
            "does not mean missing collision. Simulation is unchanged.",
            unsigned(query_ok), unsigned(stats.status), stats.objects_visited,
            stats.shapes_visited, stats.mesh_parts_visited, stats.triangles_read,
            (unsigned long long)stats.admitted_mesh_triangles, stats.lines_emitted);
    }
    return true;
}

#endif
