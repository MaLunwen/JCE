/* Executes the production bounded reader against native Bullet indexed meshes.
 * It does not mock Bullet shapes or claim a JCE renderer/FPS test. */
#ifndef JCE_PHYSICS_DEBUG_MESH_OWNER
#define JCE_PHYSICS_DEBUG_MESH_OWNER "../../../engine/src/middleware/physics/jce_physics_debug_mesh.hpp"
#endif
#include JCE_PHYSICS_DEBUG_MESH_OWNER
#include <BulletCollision/CollisionShapes/btTriangleMesh.h>
#include <BulletCollision/CollisionShapes/btTriangleIndexVertexArray.h>
#include <cstdio>
#include <set>
#include <vector>

namespace m = jce_physics_debug_mesh;
static unsigned failures = 0, checks = 0;
static void check(bool condition, const char *name) {
    ++checks; if (!condition) { ++failures; std::fprintf(stderr, "%s\n", name); }
}

template<class Vertex, class Index>
static void layout(PHY_ScalarType vt, PHY_ScalarType it) {
    Vertex vertices[] = {0,0,0, 1,0,0, 0,1,0, 0,0,2};
    Index indices[] = {0,1,2, 1,3,2};
    btIndexedMesh indexed;
    indexed.m_numTriangles = 2; indexed.m_numVertices = 4;
    indexed.m_triangleIndexBase = reinterpret_cast<const unsigned char *>(indices);
    indexed.m_triangleIndexStride = sizeof(Index) * 3;
    indexed.m_vertexBase = reinterpret_cast<const unsigned char *>(vertices);
    indexed.m_vertexStride = sizeof(Vertex) * 3; indexed.m_vertexType = vt;
    btTriangleIndexVertexArray mesh; mesh.addIndexedMesh(indexed, it);
    m::Part part; check(m::inspect(mesh, 0, part), "native scalar layout admitted");
    btTransform parent(btQuaternion(btVector3(0,1,0), btScalar(.7)), btVector3(4,5,6));
    btTransform child(btQuaternion(btVector3(1,0,0), btScalar(-.3)), btVector3(-2,1,3));
    const btTransform pose = parent * child;
    mesh.setScaling(btVector3(2,3,4)); const btVector3 scale = mesh.getScaling() * btVector3(.5,2,1);
    unsigned callbacks = 0; bool invalid = false;
    const uint32_t read = m::sample(mesh, part, 200, pose, scale,
        [&](const btVector3 *tri, int p, uint32_t face) {
            ++callbacks; check(p == 0 && face < 2, "native part/face identity preserved");
            for (unsigned c = 0; c < 3; ++c) {
                const unsigned index = indices[face * 3 + c];
                const btVector3 expected = pose * (btVector3(btScalar(vertices[index * 3]),
                    btScalar(vertices[index * 3 + 1]), btScalar(vertices[index * 3 + 2])) * scale);
                check((tri[c] - expected).length2() < btScalar(1.e-10),
                      "compound pose and mesh/wrapper scale apply exactly once");
            }
        }, invalid);
    check(read == 2 && callbacks == 2 && !invalid, "adequate budget reads every face exactly once");
    indexed.m_triangleIndexStride = sizeof(Index);
    btTriangleIndexVertexArray short_stride; short_stride.addIndexedMesh(indexed, it);
    check(!m::inspect(short_stride, 0, part), "overlapping index strip layout explicitly rejected");
}

int main() {
    layout<float, int32_t>(PHY_FLOAT, PHY_INTEGER);
    layout<double, uint16_t>(PHY_DOUBLE, PHY_SHORT);
    layout<float, uint8_t>(PHY_FLOAT, PHY_UCHAR);
    btTriangleMesh mesh;
    const unsigned count = 10003;
    for (unsigned i = 0; i < count; ++i) mesh.addTriangle(
        btVector3(btScalar(i), 0, 0), btVector3(btScalar(i), 1, 0),
        btVector3(btScalar(i), 0, 1), false);
    m::Part part; check(m::inspect(mesh, 0, part) && part.triangles == count, "native mesh metadata");
    btTransform identity; identity.setIdentity();
    bool invalid = false; std::vector<unsigned> faces;
    const auto emit = [&](const btVector3 *tri, int, uint32_t face) {
        faces.push_back(face); check(tri[0].x() == btScalar(face), "selected coordinates belong to native face");
    };
    check(m::sample(mesh, part, 0, identity, btVector3(1,1,1), emit, invalid) == 0 && faces.empty(),
          "zero geometry budget never visits an index");
    const unsigned quota = 31;
    check(m::sample(mesh, part, quota, identity, btVector3(1,1,1), emit, invalid) == quota,
          "native triangle reads stop at requested budget");
    check(faces.size() == quota && faces[0] == 0 && faces[1] == count - 1,
          "progressive budget anchors head and tail before interior detail");
    check(std::set<unsigned>(faces.begin(), faces.end()).size() == faces.size(),
          "native face reads are unique across the entire mesh");
    std::vector<unsigned> previous;
    for (unsigned next_quota : {2u, 3u, 7u, 16u, 31u, 32u, 64u, 127u, 128u, 257u, count}) {
        faces.clear(); invalid = false;
        check(m::sample(mesh, part, next_quota, identity, btVector3(1,1,1), emit, invalid) == next_quota &&
              faces.size() == next_quota && !invalid, "quota bounds real native triangle reads");
        check(std::set<unsigned>(faces.begin(), faces.end()).size() == faces.size(),
              "progressive native read has no duplicate face");
        check(std::equal(previous.begin(), previous.end(), faces.begin()),
              "changing quota retains prior selected faces as a prefix");
        previous = faces;
    }
    check(std::set<unsigned>(faces.begin(), faces.end()).size() == count,
          "full budget still visits every native face exactly once");
    faces.clear(); m::sample(mesh, part, 32, identity, btVector3(1,1,1), emit, invalid);
    check(std::equal(faces.begin(), faces.end(), previous.begin()),
          "shrinking quota removes detail without selecting replacement faces");
    for (uint32_t total = 0; total <= 1025; ++total) {
        m::FaceSequence sequence(total); std::set<uint32_t> selected;
        for (uint32_t n = 0; n < total; ++n) {
            const uint32_t face = sequence.next();
            check(face < total && selected.insert(face).second,
                  "power-of-two boundary permutation is in range and unique");
        }
        check(sequence.next() == total, "completed sequence refuses further indices");
    }
    for (uint32_t total : {0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        m::FaceSequence sequence(total); std::set<uint32_t> selected;
        for (unsigned n = 0; n < 4096; ++n) {
            const uint32_t face = sequence.next();
            check(face < total && selected.insert(face).second,
                  "wide metadata counts do not overflow the sampling sequence");
        }
        check(sequence.cursor <= 2u * 4096u,
              "bounded prefix examines at most twice its quota of metadata indices");
    }
    faces.clear(); m::sample(mesh, part, 1, identity, btVector3(1,1,1), emit, invalid);
    check(faces.size() == 1 && faces[0] == count/2, "one-face budget samples middle");
    float vertices[] = {0,0,0, 1,0,0, 0,1,0}; int indices[] = {0,1,99};
    btTriangleIndexVertexArray bad(1, indices, 3 * sizeof(int), 3, vertices, 3 * sizeof(float));
    check(m::inspect(bad, 0, part), "malformed-index test reaches native read guard");
    faces.clear(); invalid = false;
    m::sample(bad, part, 1, identity, btVector3(1,1,1), emit, invalid);
    check(invalid && faces.empty(), "out-of-range index rejected before vertex access");
    std::printf("native physics debug mesh: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
