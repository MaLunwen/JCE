/* Read-only bounded traversal of the LIVE Bullet quantized BVH. This neither
 * rebuilds an acceleration structure nor asks Bullet to read every triangle. */
#ifndef JCE_PHYSICS_DEBUG_RANGE_HPP
#define JCE_PHYSICS_DEBUG_RANGE_HPP
#include <jce/middleware/physics/jce_physics_debug.h>
#include <BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btOptimizedBvh.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace jce_physics_debug_range {
constexpr uint32_t candidate_capacity = 16384, node_budget = 131072;
struct Face { uint32_t triangle; uint16_t leaf, part; };
struct Candidates {
    std::array<Face, candidate_capacity> faces;
    uint32_t count = 0, node_visits = 0;
};

inline bool valid_bounds(const btVector3 &lo, const btVector3 &hi) {
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(double(lo[axis])) || !std::isfinite(double(hi[axis])) || lo[axis] > hi[axis])
            return false;
    return true;
}

inline bool overlaps(const JcePhysicsDebugQuery &q, const btVector3 &lo,
                     const btVector3 &hi)
{
    const float qlo[] = {q.bounds_min.x, q.bounds_min.y, q.bounds_min.z};
    const float qhi[] = {q.bounds_max.x, q.bounds_max.y, q.bounds_max.z};
    const float center[] = {q.range_center.x, q.range_center.y, q.range_center.z};
    double distance2 = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(double(lo[axis])) || !std::isfinite(double(hi[axis])) ||
            lo[axis] > hi[axis] || lo[axis] > qhi[axis] || hi[axis] < qlo[axis]) return false;
        const double d = std::max(double(lo[axis]) - center[axis],
                                  std::max(double(center[axis]) - hi[axis], 0.0));
        distance2 += d * d;
    }
    return q.range_radius == 0 || distance2 <= double(q.range_radius) * q.range_radius;
}

inline void collect(Candidates &out, const btBvhTriangleMeshShape &shape,
                    uint32_t leaf, const btTransform &pose, const btVector3 &wrapper_scale,
                    const JcePhysicsDebugQuery &q, JcePhysicsDebugQueryStats &stats)
{
    /* Bullet 3.25 exposes these accessors without const overloads. We bind the
     * returned array as const and never mutate it or retain it after the call. */
    auto *bvh = const_cast<btBvhTriangleMeshShape &>(shape).getOptimizedBvh();
    if (!bvh || !bvh->isQuantized()) {
        ++stats.unsupported_shapes; stats.status |= JCE_PHYS_QUERY_UNSUPPORTED; return;
    }
    const auto &nodes = bvh->getQuantizedNodeArray();
    if (nodes.size() == 0) return;
    const int end = nodes[0].isLeafNode() ? 1 : nodes[0].getEscapeIndex();
    if (end <= 0 || end > nodes.size()) { stats.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; return; }
    const btMatrix3x3 absolute_basis = pose.getBasis().absolute();
    const btVector3 absolute_scale = wrapper_scale.absolute();
    for (int index = 0; index < end;) {
        if (out.node_visits == node_budget || out.count == candidate_capacity) {
            stats.status |= JCE_PHYS_QUERY_LIMIT; return;
        }
        ++out.node_visits;
        const auto &node = nodes[index];
        const bool is_leaf = node.isLeafNode();
        const int skip = is_leaf ? 1 : node.getEscapeIndex();
        if (skip <= 0 || skip > end - index) { stats.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; return; }
        /* BVH coordinates already include mesh scaling. Only the optional
         * scaled-shape wrapper belongs here; raw vertices use the full scale
         * separately in the existing bounded indexed reader. */
        const btVector3 lo = bvh->unQuantize(node.m_quantizedAabbMin);
        const btVector3 hi = bvh->unQuantize(node.m_quantizedAabbMax);
        const btVector3 center = pose * (((lo + hi) * btScalar(0.5)) * wrapper_scale);
        const btVector3 extent = absolute_basis * (((hi - lo) * btScalar(0.5)) * absolute_scale);
        if (!valid_bounds(lo, hi) || !valid_bounds(center - extent, center + extent)) {
            stats.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; return;
        }
        const bool hit = overlaps(q, center - extent, center + extent);
        if (hit && is_leaf) {
            const int part = node.getPartId(), triangle = node.getTriangleIndex();
            if (part < 0 || part > UINT16_MAX || triangle < 0 || leaf > UINT16_MAX) {
                stats.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; return;
            }
            out.faces[out.count++] = {uint32_t(triangle), uint16_t(leaf), uint16_t(part)};
        }
        index += hit || is_leaf ? 1 : skip;
    }
}

inline bool before(const Face &a, const Face &b) {
    return a.leaf != b.leaf ? a.leaf < b.leaf :
           a.part != b.part ? a.part < b.part : a.triangle < b.triangle;
}
} // namespace jce_physics_debug_range
#endif
