/* Bounded inspection of LIVE Bullet shapes. No render mesh/model uploads,
 * cross-frame shape pointers, whole-mesh debug traversal, or simulation edits. */
#include "jce_physics_debug_query.hpp"
#include "jce_physics_debug_mesh.hpp"
#include "jce_physics_debug_range.hpp"
#include <BulletCollision/CollisionDispatch/btCollisionWorld.h>
#include <BulletCollision/CollisionShapes/btBoxShape.h>
#include <BulletCollision/CollisionShapes/btCapsuleShape.h>
#include <BulletCollision/CollisionShapes/btCompoundShape.h>
#include <BulletCollision/CollisionShapes/btSphereShape.h>
#include <BulletCollision/CollisionShapes/btStaticPlaneShape.h>
#include <BulletCollision/CollisionShapes/btPolyhedralConvexShape.h>
#include <BulletCollision/CollisionShapes/btConvexPolyhedron.h>
#include <BulletCollision/CollisionShapes/btTriangleMeshShape.h>
#include <BulletCollision/CollisionShapes/btScaledBvhTriangleMeshShape.h>
#include <LinearMath/btIDebugDraw.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
namespace mesh = jce_physics_debug_mesh;
namespace range = jce_physics_debug_range;
constexpr uint32_t leaf_capacity = 512, part_capacity = 2048;
constexpr uint32_t shape_flags = JCE_PHYS_DBG_WIREFRAME | JCE_PHYS_DBG_AABB |
                                 JCE_PHYS_DBG_NORMALS;

struct Leaf {
    const btCollisionShape *shape = nullptr;
    const btStridingMeshInterface *mesh = nullptr;
    btTransform pose;
    btVector3 scale, lo, hi;
};
struct Part { mesh::Part part; uint32_t leaf = 0, quota = 0, first = 0, candidates = 0; };

struct Drawer final : btIDebugDraw {
    const JcePhysicsDebugQuery &q;
    JcePhysicsDebugQueryStats &s;
    jce_debug_line_fn sink;
    void *ud;
    Drawer(const JcePhysicsDebugQuery &query, JcePhysicsDebugQueryStats &stats,
           jce_debug_line_fn fn, void *data) : q(query), s(stats), sink(fn), ud(data) {}
    void drawLine(const btVector3 &a, const btVector3 &b, const btVector3 &c) override {
        if (s.lines_emitted >= q.max_lines) { s.status |= JCE_PHYS_QUERY_LIMIT; return; }
        for (unsigned i = 0; i < 3; ++i) if (!std::isfinite(double(a[i])) ||
            !std::isfinite(double(b[i]))) { s.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; return; }
        auto channel = [](btScalar n) { return uint32_t(std::max(btScalar(0),
            std::min(btScalar(1), n)) * btScalar(255)); };
        const uint32_t color = 0xff000000u | channel(c.x()) |
            (channel(c.y()) << 8) | (channel(c.z()) << 16);
        ++s.lines_emitted;
        sink(jce_v3(float(a.x()), float(a.y()), float(a.z())),
             jce_v3(float(b.x()), float(b.y()), float(b.z())), color, ud);
    }
    void drawContactPoint(const btVector3 &, const btVector3 &, btScalar, int,
                          const btVector3 &) override {}
    void reportErrorWarning(const char *) override {}
    void draw3dText(const btVector3 &, const char *) override {}
    void setDebugMode(int) override {}
    int getDebugMode() const override { return DBG_NoDebug; }
};

struct Query {
    const JcePhysicsDebugQuery &q;
    JcePhysicsDebugQueryStats &s;
    std::array<Leaf, leaf_capacity> leaves;
    std::array<Part, part_capacity> parts;
    range::Candidates nearby;
    uint32_t leaf_count = 0, part_count = 0;
    bool full() const { return s.shapes_visited >= q.max_shapes || leaf_count == leaf_capacity; }
    void gather(const btCollisionShape *shape, const btTransform &pose,
                unsigned depth = 0, bool keep = false) {
        if (!shape) return;
        if (full() || depth >= 32) { s.status |= JCE_PHYS_QUERY_LIMIT; return; }
        ++s.shapes_visited;
        btVector3 lo, hi; shape->getAabb(pose, lo, hi);
        if (!range::valid_bounds(lo, hi)) { s.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; return; }
        if (!keep && !range::overlaps(q, lo, hi)) return;
        if (shape->isCompound()) {
            const auto *compound = static_cast<const btCompoundShape *>(shape);
            for (int child = 0; child < compound->getNumChildShapes(); ++child) {
                if (full()) { s.status |= JCE_PHYS_QUERY_LIMIT; break; }
                gather(compound->getChildShape(child), pose * compound->getChildTransform(child), depth + 1, keep);
            }
            return;
        }
        Leaf &leaf = leaves[leaf_count++];
        leaf.shape = shape; leaf.pose = pose; leaf.lo = lo; leaf.hi = hi;
        leaf.scale = btVector3(1, 1, 1);
        if (shape->getShapeType() == TRIANGLE_MESH_SHAPE_PROXYTYPE)
            leaf.mesh = static_cast<const btTriangleMeshShape *>(shape)->getMeshInterface();
        else if (shape->getShapeType() == SCALED_TRIANGLE_MESH_SHAPE_PROXYTYPE) {
            const auto *scaled = static_cast<const btScaledBvhTriangleMeshShape *>(shape);
            leaf.mesh = scaled->getChildShape()->getMeshInterface();
            leaf.scale = scaled->getLocalScaling();
        }
        ++s.shapes_admitted;
        if (!leaf.mesh || !(q.flags & (JCE_PHYS_DBG_WIREFRAME | JCE_PHYS_DBG_NORMALS))) return;
        const btVector3 wrapper_scale = leaf.scale;
        leaf.scale *= leaf.mesh->getScaling();
        if (q.range_radius > 0) {
            const auto *bvh_shape = shape->getShapeType() == TRIANGLE_MESH_SHAPE_PROXYTYPE
                ? static_cast<const btBvhTriangleMeshShape *>(shape)
                : static_cast<const btScaledBvhTriangleMeshShape *>(shape)->getChildShape();
            range::collect(nearby, *bvh_shape, leaf_count - 1, pose, wrapper_scale, q, s);
            return;
        }
        const uint32_t total = uint32_t(std::max(0, leaf.mesh->getNumSubParts()));
        const uint32_t available = q.max_mesh_parts - s.mesh_parts_visited;
        const uint32_t admitted = std::min(total, available);
        if (admitted != total) s.status |= JCE_PHYS_QUERY_LIMIT;
        /* A huge mesh cannot use unbounded metadata scans. Even this subset
         * covers all part indices, rather than silently dropping the tail. */
        for (uint32_t i = 0; i < admitted; ++i) {
            Part &part = parts[part_count];
            ++s.mesh_parts_visited;
            if (!mesh::inspect(*leaf.mesh, int(mesh::ordinal(i, total, admitted)), part.part)) {
                s.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; continue;
            }
            if (!part.part.triangles) continue;
            part.leaf = leaf_count - 1; part.candidates = part.part.triangles; ++part_count;
            s.admitted_mesh_triangles += part.part.triangles;
        }
    }
    void prepare_nearby_parts() {
        if (q.range_radius == 0) return;
        std::sort(nearby.faces.begin(), nearby.faces.begin() + nearby.count, range::before);
        for (uint32_t first = 0; first < nearby.count;) {
            const auto face = nearby.faces[first];
            uint32_t end = first + 1;
            while (end < nearby.count && nearby.faces[end].leaf == face.leaf &&
                   nearby.faces[end].part == face.part) ++end;
            if (s.mesh_parts_visited == q.max_mesh_parts) { s.status |= JCE_PHYS_QUERY_LIMIT; break; }
            ++s.mesh_parts_visited;
            Part &part = parts[part_count];
            const Leaf &leaf = leaves[face.leaf];
            if (face.part >= leaf.mesh->getNumSubParts() ||
                !mesh::inspect(*leaf.mesh, face.part, part.part)) {
                s.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY;
            } else {
                part.leaf = face.leaf; part.first = first; part.candidates = end - first;
                s.admitted_mesh_triangles += part.candidates; ++part_count;
            }
            first = end;
        }
    }

};

void draw_primitive(const Leaf &leaf, Drawer &d)
{
    const btVector3 color(0.2f, 0.9f, 0.35f);
    const auto *shape = leaf.shape;
    bool reported = false;
    const auto unsupported = [&]() {
        if (!reported) { ++d.s.unsupported_shapes; reported = true; }
        d.s.status |= JCE_PHYS_QUERY_UNSUPPORTED;
    };
    if (d.q.flags & JCE_PHYS_DBG_AABB) d.drawAabb(leaf.lo, leaf.hi, btVector3(1, 0.65f, 0.2f));
    if (!leaf.mesh && (d.q.flags & JCE_PHYS_DBG_NORMALS)) {
        /* This bounded version supplies triangle normals only. Never report
         * complete coverage for a requested primitive/hull normal overlay. */
        unsupported();
    }
    if (!(d.q.flags & JCE_PHYS_DBG_WIREFRAME) || leaf.mesh) return;
    if (d.s.lines_emitted >= d.q.max_lines) { d.s.status |= JCE_PHYS_QUERY_LIMIT; return; }
    switch (shape->getShapeType()) {
        case BOX_SHAPE_PROXYTYPE: {
            const btVector3 half = static_cast<const btBoxShape *>(shape)->getHalfExtentsWithMargin();
            d.drawBox(-half, half, leaf.pose, color); break;
        }
        case SPHERE_SHAPE_PROXYTYPE:
            d.drawSphere(shape->getMargin(), leaf.pose, color); break;
        case CAPSULE_SHAPE_PROXYTYPE: {
            const auto *capsule = static_cast<const btCapsuleShape *>(shape);
            d.drawCapsule(capsule->getRadius(), capsule->getHalfHeight(),
                          capsule->getUpAxis(), leaf.pose, color); break;
        }
        case STATIC_PLANE_PROXYTYPE: {
            const auto *plane = static_cast<const btStaticPlaneShape *>(shape);
            d.drawPlane(plane->getPlaneNormal(), plane->getPlaneConstant(), leaf.pose, color); break;
        }
        default:
            if (shape->isPolyhedral()) {
                const auto *features = static_cast<const btPolyhedralConvexShape *>(shape)->getConvexPolyhedron();
                /* btConvexHullShape::getEdge can join input points in their
                 * original order. That is not a hull wireframe. Never invent
                 * those edges or build a convex hull on this render path. */
                if (!features) { unsupported(); break; }
                const uint32_t total = uint32_t(features->m_faces.size());
                uint32_t available = d.q.max_lines - d.s.lines_emitted;
                const uint32_t faces = std::min(total, available / 3);
                if (faces != total) d.s.status |= JCE_PHYS_QUERY_SAMPLED;
                for (uint32_t f = 0; f < faces; ++f) {
                    const auto &face = features->m_faces[int(mesh::ordinal(f, total, faces))];
                    const uint32_t edges = uint32_t(face.m_indices.size());
                    const uint32_t quota = std::min(edges, available / (faces - f));
                    if (quota != edges) d.s.status |= JCE_PHYS_QUERY_SAMPLED;
                    available -= quota;
                    for (uint32_t i = 0; i < quota; ++i) {
                        const uint32_t edge = uint32_t(mesh::ordinal(i, edges, quota));
                        const int a = face.m_indices[int(edge)], b = face.m_indices[int((edge + 1) % edges)];
                        if (a < 0 || b < 0 || a >= features->m_vertices.size() || b >= features->m_vertices.size()) {
                            d.s.status |= JCE_PHYS_QUERY_INVALID_GEOMETRY; continue;
                        }
                        d.drawLine(leaf.pose * features->m_vertices[a], leaf.pose * features->m_vertices[b], color);
                    }
                }
            } else unsupported();
    }
}

bool valid(const JcePhysicsDebugQuery &q) {
    if (q.struct_size != sizeof(q) || (q.flags & ~shape_flags) ||
        (q.range_flags & ~uint32_t(JCE_PHYS_QUERY_KEEP_CHARACTER)) ||
        !std::isfinite(q.range_radius) || q.range_radius < 0 ||
        !std::isfinite(q.range_center.x) || !std::isfinite(q.range_center.y) ||
        !std::isfinite(q.range_center.z) ||
        !q.max_objects || q.max_objects > 8192 || !q.max_shapes || q.max_shapes > 4096 ||
        !q.max_mesh_parts || q.max_mesh_parts > part_capacity ||
        !q.max_triangles || q.max_triangles > 65536 || !q.max_lines || q.max_lines > 262144) return false;
    const float lo[] = {q.bounds_min.x, q.bounds_min.y, q.bounds_min.z};
    const float hi[] = {q.bounds_max.x, q.bounds_max.y, q.bounds_max.z};
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(lo[i]) || !std::isfinite(hi[i]) || lo[i] > hi[i]) return false;
    return true;
}
} // namespace

bool jce_bullet_debug_query_native(btCollisionWorld *world,
    const JcePhysicsDebugQuery *query, jce_debug_line_fn sink, void *userdata,
    JcePhysicsDebugQueryStats *stats, const btCollisionObject *keep_character)
{
    if (!world || !query || !sink || !stats || stats->struct_size != sizeof(*stats)) return false;
    /* Old binaries really allocate only their declared prefix. Do not inspect
     * any optional member until a bounded byte copy has normalized it. */
    if (query->struct_size != JCE_PHYSICS_DEBUG_QUERY_V1_SIZE &&
        query->struct_size != sizeof(*query)) return false;
    JcePhysicsDebugQuery normalized{};
    std::memcpy(&normalized, query, query->struct_size);
    normalized.struct_size = sizeof(normalized);
    if (!valid(normalized)) return false;
    query = &normalized;
    *stats = {}; stats->struct_size = sizeof(*stats);
    if (!query->flags) return true;
    Query work{*query, *stats, {}, {}, {}};
    const auto &objects = world->getCollisionObjectArray();
    const uint32_t total = uint32_t(objects.size());
    /* The bridge resolves only the requested live player handle. Process that
     * exact object first so nearby terrain cannot consume its shape/line share. */
    if (!(query->range_flags & JCE_PHYS_QUERY_KEEP_CHARACTER)) keep_character = nullptr;
    if (keep_character) {
        const int at = keep_character->getWorldArrayIndex();
        if (at < 0 || uint32_t(at) >= total || objects[at] != keep_character)
            keep_character = nullptr;
    }
    if (keep_character) {
        ++stats->objects_visited;
        work.gather(keep_character->getCollisionShape(), keep_character->getWorldTransform(), 0, true);
    }
    const uint32_t others = total - (keep_character ? 1u : 0u);
    const uint32_t count = std::min(others, query->max_objects - stats->objects_visited);
    if (count != others) stats->status |= JCE_PHYS_QUERY_LIMIT;
    for (uint32_t i = 0; i < count; ++i) {
        if (work.full()) { stats->status |= JCE_PHYS_QUERY_LIMIT; break; }
        uint32_t at = uint32_t(mesh::ordinal(i, others, count));
        if (keep_character && at >= uint32_t(keep_character->getWorldArrayIndex())) ++at;
        const auto *object = objects[int(at)];
        ++stats->objects_visited;
        work.gather(object->getCollisionShape(), object->getWorldTransform());
    }
    work.prepare_nearby_parts();
    Drawer drawer(*query, *stats, sink, userdata);
    for (uint32_t i = 0; i < work.leaf_count; ++i) {
        if (stats->lines_emitted >= query->max_lines) { stats->status |= JCE_PHYS_QUERY_LIMIT; break; }
        draw_primitive(work.leaves[i], drawer);
    }
    if (!work.part_count) return true;
    const uint32_t lines_per_triangle = (query->flags & JCE_PHYS_DBG_WIREFRAME ? 3 : 0) +
                                        (query->flags & JCE_PHYS_DBG_NORMALS ? 1 : 0);
    const uint32_t budget = std::min(query->max_triangles,
        (query->max_lines - stats->lines_emitted) / lines_per_triangle);
    const uint32_t base = std::min(16u, budget / work.part_count);
    uint32_t remaining = budget; uint64_t unmet = 0;
    for (uint32_t i = 0; i < work.part_count; ++i) {
        Part &p = work.parts[i]; p.quota = std::min(base, p.candidates);
        remaining -= p.quota; unmet += p.candidates - p.quota;
    }
    /* Give every admitted part a small share before distributing the balance.
     * Budget arithmetic touches metadata only, never unselected vertices. */
    for (uint32_t i = 0; i < work.part_count; ++i) {
        Part &p = work.parts[i]; const uint32_t need = p.candidates - p.quota;
        const uint32_t extra = unmet ? uint32_t(uint64_t(remaining) * need / unmet) : 0;
        p.quota += std::min(need, extra); remaining -= std::min(need, extra); unmet -= need;
        if (p.quota != p.candidates) stats->status |= JCE_PHYS_QUERY_SAMPLED;
        if (!p.quota) continue;
        const Leaf &leaf = work.leaves[p.leaf]; bool invalid = false;
        const auto emit = [&](const btVector3 *t, int, uint32_t) {
                if (query->range_radius > 0) {
                    /* Quantized BVH leaves conservatively expand their bounds.
                     * Recheck only selected local faces against actual vertices;
                     * distant world faces never entered this candidate budget. */
                    btVector3 lo = t[0], hi = t[0];
                    for (unsigned i = 1; i < 3; ++i) { lo.setMin(t[i]); hi.setMax(t[i]); }
                    if (!range::valid_bounds(lo, hi)) { invalid = true; return; }
                    if (!range::overlaps(*query, lo, hi)) return;
                }
                if (query->flags & JCE_PHYS_DBG_WIREFRAME)
                    for (unsigned edge = 0; edge < 3; ++edge) drawer.drawLine(t[edge], t[(edge + 1) % 3], btVector3(0.2f, 0.9f, 0.35f));
                if (query->flags & JCE_PHYS_DBG_NORMALS) {
                    btVector3 normal = (t[1] - t[0]).cross(t[2] - t[0]);
                    if (normal.length2() > SIMD_EPSILON) {
                        const btVector3 center = (t[0] + t[1] + t[2]) / btScalar(3);
                        drawer.drawLine(center, center + normal.normalized() * btScalar(0.3), btVector3(0.3f, 0.6f, 1));
                    }
                }
            };
        if (query->range_radius == 0) {
            stats->triangles_read += mesh::sample(*leaf.mesh, p.part, p.quota,
                leaf.pose, leaf.scale, emit, invalid);
        } else {
            mesh::View view(*leaf.mesh, p.part.index);
            if (!view.valid() || uint32_t(view.triangles) != p.part.triangles) invalid = true;
            else {
                mesh::FaceSequence sequence(p.candidates);
                for (uint32_t n = 0; n < p.quota; ++n) {
                    const uint32_t ordinal = p.quota == 1 ? p.candidates / 2 : sequence.next();
                    const uint32_t face = work.nearby.faces[p.first + ordinal].triangle;
                    btVector3 triangle[3]; ++stats->triangles_read;
                    if (mesh::mesh_triangle(view, face, leaf.pose, leaf.scale, triangle))
                        emit(triangle, p.part.index, face);
                    else invalid = true;
                }
            }
        }
        if (invalid) stats->status |= JCE_PHYS_QUERY_INVALID_GEOMETRY;
    }
    return true;
}
