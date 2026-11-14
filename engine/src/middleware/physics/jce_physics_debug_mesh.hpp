/* Private bounded reader of the instantiated Bullet mesh, not render assets.
 * Mesh/BVH selection belongs to the query; unselected faces are never read. */
#ifndef JCE_PHYSICS_DEBUG_MESH_HPP
#define JCE_PHYSICS_DEBUG_MESH_HPP

#include <BulletCollision/CollisionShapes/btStridingMeshInterface.h>
#include <LinearMath/btTransform.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace jce_physics_debug_mesh {

struct Part { int index = 0; uint32_t triangles = 0; };

struct View {
    const unsigned char *vertices = nullptr, *indices = nullptr;
    int vertex_count = 0, vertex_stride = 0, index_stride = 0, triangles = 0;
    PHY_ScalarType vertex_type = PHY_FLOAT, index_type = PHY_INTEGER;
    const btStridingMeshInterface &mesh;
    int part;
    View(const btStridingMeshInterface &m, int p) : mesh(m), part(p) {
        mesh.getLockedReadOnlyVertexIndexBase(&vertices, vertex_count, vertex_type,
            vertex_stride, &indices, index_stride, triangles, index_type, part);
    }
    ~View() { mesh.unLockReadOnlyVertexBase(part); }
    View(const View &) = delete;
    View &operator=(const View &) = delete;
    bool valid() const {
        /* This query accepts independent indexed triangles; overlapping
         * triangle-strip layouts are rejected by this reader's layout guard. */
        const int width = vertex_type == PHY_FLOAT ? sizeof(float) :
                          vertex_type == PHY_DOUBLE ? sizeof(double) : 0;
        const int iw = index_type == PHY_INTEGER ? sizeof(int32_t) :
                       index_type == PHY_SHORT ? sizeof(uint16_t) :
                       index_type == PHY_UCHAR ? sizeof(uint8_t) : 0;
        return width && iw && vertex_count >= 0 && triangles >= 0 &&
               vertex_stride >= 3 * width && index_stride >= 3 * iw &&
               (triangles == 0 || (vertices && indices && vertex_count > 0));
    }
};

template<class T> inline T read(const unsigned char *p) {
    T value; std::memcpy(&value, p, sizeof(value)); return value;
}

inline bool inspect(const btStridingMeshInterface &mesh, int index, Part &part) {
    View view(mesh, index);
    part = {index, view.triangles > 0 ? uint32_t(view.triangles) : 0u};
    return view.valid();
}

/* Inclusive endpoints prevent a face budget from becoming an index prefix.
 * A one-face budget selects the middle; no random state changes between frames. */
inline uint64_t ordinal(uint64_t i, uint64_t count, uint64_t quota) {
    return quota <= 1 ? count / 2 : i * (count - 1) / (quota - 1);
}

/* A quota change must only add/remove detail, not replace the whole sample.
 * Anchor both ends, then visit a bit-reversed power-of-two domain. Rejecting
 * indices outside count preserves a permutation without reading geometry.
 * The domain is smaller than 2*count; no cross-frame state or allocation.
 * Emission order is progressive, not ascending. Singleton samples below keep
 * their historical middle-face choice; prefix stability starts at quota 2. */
struct FaceSequence {
    uint32_t count, emitted = 0;
    uint64_t cursor = 0, domain = 1;
    unsigned bits = 0;
    explicit FaceSequence(uint32_t n) : count(n) {
        while (domain < count) { domain <<= 1; ++bits; }
    }
    static uint32_t reverse(uint32_t value) {
        value = ((value & 0x55555555u) << 1) | ((value >> 1) & 0x55555555u);
        value = ((value & 0x33333333u) << 2) | ((value >> 2) & 0x33333333u);
        value = ((value & 0x0f0f0f0fu) << 4) | ((value >> 4) & 0x0f0f0f0fu);
        value = ((value & 0x00ff00ffu) << 8) | ((value >> 8) & 0x00ff00ffu);
        return (value << 16) | (value >> 16);
    }
    uint32_t next() {
        if (emitted >= count) return count;
        const uint32_t at = emitted++;
        if (at == 0) return 0;
        if (at == 1) return count - 1;
        while (cursor < domain) {
            const uint32_t face = reverse(uint32_t(cursor++)) >> (32u - bits);
            if (face && face < count - 1) return face;
        }
        return count; /* defensive exhaustion: caller refuses before a read */
    }
};

inline bool mesh_triangle(const View &v, uint32_t face, const btTransform &pose,
                          const btVector3 &scale, btVector3 (&out)[3])
{
    if (face >= uint32_t(v.triangles)) return false;
    const unsigned iw = v.index_type == PHY_INTEGER ? sizeof(int32_t) :
                        v.index_type == PHY_SHORT ? sizeof(uint16_t) : 1;
    const auto *ip = v.indices + size_t(face) * size_t(v.index_stride);
    bool valid = true;
    for (unsigned corner = 0; corner < 3; ++corner) {
        const auto *p = ip + corner * iw;
        const int64_t index = v.index_type == PHY_INTEGER ? read<int32_t>(p) :
                              v.index_type == PHY_SHORT ? read<uint16_t>(p) : *p;
        if (index < 0 || index >= v.vertex_count) { valid = false; break; }
        const auto *vp = v.vertices + size_t(index) * size_t(v.vertex_stride);
        btVector3 vertex(0, 0, 0);
        for (unsigned axis = 0; axis < 3; ++axis) {
            const double value = v.vertex_type == PHY_FLOAT ?
                double(read<float>(vp + axis * sizeof(float))) :
                read<double>(vp + axis * sizeof(double));
            if (!std::isfinite(value)) { valid = false; break; }
            vertex[axis] = btScalar(value);
        }
        if (!valid) break;
        out[corner] = pose * (vertex * scale);
    }
    return valid;
}

template<class Emit>
uint32_t sample(const btStridingMeshInterface &mesh, const Part &part,
                uint32_t quota, const btTransform &pose, const btVector3 &scale,
                Emit emit, bool &invalid)
{
    if (!quota || !part.triangles) return 0;
    View v(mesh, part.index);
    if (!v.valid() || uint32_t(v.triangles) != part.triangles) {
        invalid = true; return 0;
    }
    quota = std::min(quota, part.triangles);
    FaceSequence sequence(part.triangles);
    for (uint32_t n = 0; n < quota; ++n) {
        const uint64_t face = quota == 1
            ? part.triangles / 2u : sequence.next();
        if (face >= part.triangles) { invalid = true; return n; }
        btVector3 triangle[3];
        const bool valid = mesh_triangle(v, uint32_t(face), pose, scale, triangle);
        if (valid) emit(triangle, part.index, uint32_t(face));
        else invalid = true;
    }
    return quota;
}

} // namespace jce_physics_debug_mesh
#endif
