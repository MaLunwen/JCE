/*
 * jce_collider_cook.cpp  Per-object compound collider cooker.
 *
 * Turns an array of model parts (one per node / mesh) into a compact
 * cooked collider tree.  Static parts cook to exact triangle meshes,
 * dynamic parts to convex hulls, and concave dynamic parts can be split
 * into multiple convex hulls with VHACD.  Honors the COL_/UCX_/UBX_/...
 * collision-mesh naming conventions.
 *
 * Compiled as C++ because VHACD is a C++ single-header library.  The C
 * surface is declared in jce_collider_cook.h.
 */

#define ENABLE_VHACD_IMPLEMENTATION 1
#include "VHACD.h"

extern "C" {
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
}

#include <vector>
#include <cstring>
#include <cmath>
#include <cctype>

#define LOG_TAG "collider_cook"

namespace {

/* Column-major 4x4 transform of a point: out = M * (x,y,z,1). */
inline void xform_point(const float *m, float x, float y, float z, float out[3])
{
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

/* Recognized collision-mesh name prefixes.  Returns the forced mode and
   sets *matched=true, or returns AUTO with *matched=false. */
JceColliderMode prefix_mode(const char *name, bool *matched)
{
    *matched = false;
    if (!name) return JCE_COLLIDER_MODE_AUTO;

    char p[5] = {0};
    for (int i = 0; i < 4 && name[i]; ++i) p[i] = (char)toupper((unsigned char)name[i]);

    struct { const char *pfx; JceColliderMode mode; } table[] = {
        { "UBX_", JCE_COLLIDER_MODE_BOX },
        { "USP_", JCE_COLLIDER_MODE_SPHERE },
        { "UCP_", JCE_COLLIDER_MODE_CAPSULE },
        { "UCX_", JCE_COLLIDER_MODE_CONVEX_HULL },
        { "TRI_", JCE_COLLIDER_MODE_TRIANGLE_MESH },
        { "COL_", JCE_COLLIDER_MODE_AUTO },   /* generic: use cfg mode */
    };
    for (auto &e : table) {
        if (std::strncmp(p, e.pfx, 4) == 0) { *matched = true; return e.mode; }
    }
    return JCE_COLLIDER_MODE_AUTO;
}

/* Resolve AUTO / illegal combinations to a concrete shape. */
JceColliderMode resolve_mode(JceColliderMode mode, bool is_static)
{
    if (mode == JCE_COLLIDER_MODE_AUTO)
        return is_static ? JCE_COLLIDER_MODE_TRIANGLE_MESH
                         : JCE_COLLIDER_MODE_CONVEX_HULL;
    /* Triangle meshes cannot back a dynamic body — fall back to a hull. */
    if (mode == JCE_COLLIDER_MODE_TRIANGLE_MESH && !is_static)
        return JCE_COLLIDER_MODE_CONVEX_HULL;
    return mode;
}

/* Bake one or more parts' geometry into a single model-space buffer. */
struct Baked {
    std::vector<float>    verts;   /* xyz triplets */
    std::vector<uint32_t> indices;
};

void bake_part(const JceColliderPart &part, Baked &b)
{
    uint32_t base = (uint32_t)(b.verts.size() / 3);
    b.verts.reserve(b.verts.size() + part.vertex_count * 3);
    for (uint32_t v = 0; v < part.vertex_count; ++v) {
        const float *src = &part.vertices[v * 3];
        float w[3];
        xform_point(part.transform, src[0], src[1], src[2], w);
        b.verts.push_back(w[0]);
        b.verts.push_back(w[1]);
        b.verts.push_back(w[2]);
    }
    if (part.indices && part.index_count) {
        b.indices.reserve(b.indices.size() + part.index_count);
        for (uint32_t i = 0; i < part.index_count; ++i)
            b.indices.push_back(part.indices[i] + base);
    }
}

void set_identity(JceCookedChild &c)
{
    c.position[0] = c.position[1] = c.position[2] = 0.0f;
    c.rotation[0] = c.rotation[1] = c.rotation[2] = 0.0f;
    c.rotation[3] = 1.0f;
    c.half_extents[0] = c.half_extents[1] = c.half_extents[2] = 0.0f;
    c.vertices = nullptr; c.vertex_count = 0;
    c.indices  = nullptr; c.index_count  = 0;
}

/* Copy an xyz buffer into a jce_malloc'd array owned by the child. */
bool own_verts(JceCookedChild &c, const float *verts, uint32_t count)
{
    c.vertices = (float *)jce_malloc((size_t)count * 3 * sizeof(float));
    if (!c.vertices) return false;
    std::memcpy(c.vertices, verts, (size_t)count * 3 * sizeof(float));
    c.vertex_count = count;
    return true;
}

bool own_indices(JceCookedChild &c, const uint32_t *idx, uint32_t count)
{
    c.indices = (uint32_t *)jce_malloc((size_t)count * sizeof(uint32_t));
    if (!c.indices) return false;
    std::memcpy(c.indices, idx, (size_t)count * sizeof(uint32_t));
    c.index_count = count;
    return true;
}

/* Emit a box/sphere/capsule child sized to the AABB of `b`. */
void emit_primitive(std::vector<JceCookedChild> &out, JceColliderMode mode,
                    const Baked &b)
{
    if (b.verts.empty()) return;
    float mn[3] = { b.verts[0], b.verts[1], b.verts[2] };
    float mx[3] = { b.verts[0], b.verts[1], b.verts[2] };
    for (size_t i = 0; i < b.verts.size(); i += 3) {
        for (int k = 0; k < 3; ++k) {
            float v = b.verts[i + k];
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    }
    float center[3] = { 0.5f * (mn[0] + mx[0]),
                        0.5f * (mn[1] + mx[1]),
                        0.5f * (mn[2] + mx[2]) };
    float half[3]   = { 0.5f * (mx[0] - mn[0]),
                        0.5f * (mx[1] - mn[1]),
                        0.5f * (mx[2] - mn[2]) };

    JceCookedChild c; set_identity(c);
    c.position[0] = center[0];
    c.position[1] = center[1];
    c.position[2] = center[2];

    if (mode == JCE_COLLIDER_MODE_BOX) {
        c.shape = JCE_SHAPE_BOX;
        c.half_extents[0] = half[0];
        c.half_extents[1] = half[1];
        c.half_extents[2] = half[2];
    } else if (mode == JCE_COLLIDER_MODE_SPHERE) {
        c.shape = JCE_SHAPE_SPHERE;
        float r = half[0];
        if (half[1] > r) r = half[1];
        if (half[2] > r) r = half[2];
        c.half_extents[0] = r;
    } else { /* capsule: align long axis to Y via rotation */
        c.shape = JCE_SHAPE_CAPSULE;
        int axis = 0;
        if (half[1] >= half[0] && half[1] >= half[2]) axis = 1;
        else if (half[2] >= half[0] && half[2] >= half[1]) axis = 2;
        float radius = 0.0f, hh = 0.0f;
        if (axis == 1) { radius = (half[0] > half[2] ? half[0] : half[2]); hh = half[1] - radius; }
        else if (axis == 0) { radius = (half[1] > half[2] ? half[1] : half[2]); hh = half[0] - radius;
                              /* rotate Y→X: -90° about Z */
                              c.rotation[2] = -0.70710678f; c.rotation[3] = 0.70710678f; }
        else { radius = (half[0] > half[1] ? half[0] : half[1]); hh = half[2] - radius;
               /* rotate Y→Z: +90° about X */
               c.rotation[0] = 0.70710678f; c.rotation[3] = 0.70710678f; }
        if (hh < 0.0f) hh = 0.0f;
        c.half_extents[0] = radius;
        c.half_extents[1] = hh;
    }
    out.push_back(c);
}

void emit_convex_hull(std::vector<JceCookedChild> &out, const Baked &b)
{
    if (b.verts.empty()) return;
    JceCookedChild c; set_identity(c);
    c.shape = JCE_SHAPE_CONVEX_HULL;
    if (own_verts(c, b.verts.data(), (uint32_t)(b.verts.size() / 3)))
        out.push_back(c);
}

void emit_triangle_mesh(std::vector<JceCookedChild> &out, const Baked &b)
{
    if (b.verts.empty() || b.indices.size() < 3) return;
    JceCookedChild c; set_identity(c);
    c.shape = JCE_SHAPE_TRIANGLE_MESH;
    if (own_verts(c, b.verts.data(), (uint32_t)(b.verts.size() / 3)) &&
        own_indices(c, b.indices.data(), (uint32_t)b.indices.size()))
        out.push_back(c);
    else { jce_free(c.vertices); jce_free(c.indices); }
}

void emit_convex_decomp(std::vector<JceCookedChild> &out, const Baked &b,
                        const JceColliderCookConfig &cfg)
{
    if (b.verts.empty() || b.indices.size() < 3) {
        emit_convex_hull(out, b); /* nothing to decompose */
        return;
    }

    VHACD::IVHACD *iface = VHACD::CreateVHACD();
    VHACD::IVHACD::Parameters params;
    if (cfg.vhacd_resolution)         params.m_resolution         = cfg.vhacd_resolution;
    if (cfg.vhacd_max_hulls)          params.m_maxConvexHulls      = cfg.vhacd_max_hulls;
    if (cfg.vhacd_max_verts_per_hull) params.m_maxNumVerticesPerCH = cfg.vhacd_max_verts_per_hull;

    bool ok = iface->Compute(b.verts.data(), (uint32_t)(b.verts.size() / 3),
                             b.indices.data(), (uint32_t)(b.indices.size() / 3),
                             params);
    if (!ok) {
        LOG_WARN(LOG_TAG, "VHACD failed; falling back to single convex hull");
        iface->Clean();
        iface->Release();
        emit_convex_hull(out, b);
        return;
    }

    uint32_t n = iface->GetNConvexHulls();
    for (uint32_t h = 0; h < n; ++h) {
        VHACD::IVHACD::ConvexHull ch;
        if (!iface->GetConvexHull(h, ch)) continue;
        if (ch.m_points.empty()) continue;

        std::vector<float> hv;
        hv.reserve(ch.m_points.size() * 3);
        for (const VHACD::Vertex &p : ch.m_points) {
            hv.push_back((float)p.mX);
            hv.push_back((float)p.mY);
            hv.push_back((float)p.mZ);
        }
        JceCookedChild c; set_identity(c);
        c.shape = JCE_SHAPE_CONVEX_HULL;
        if (own_verts(c, hv.data(), (uint32_t)(hv.size() / 3)))
            out.push_back(c);
    }

    iface->Clean();
    iface->Release();

    if (out.empty()) emit_convex_hull(out, b);
}

void emit_unit(std::vector<JceCookedChild> &out, JceColliderMode mode,
               bool is_static, const Baked &b, const JceColliderCookConfig &cfg)
{
    JceColliderMode m = resolve_mode(mode, is_static);
    switch (m) {
    case JCE_COLLIDER_MODE_BOX:
    case JCE_COLLIDER_MODE_SPHERE:
    case JCE_COLLIDER_MODE_CAPSULE:       emit_primitive(out, m, b);        break;
    case JCE_COLLIDER_MODE_CONVEX_HULL:   emit_convex_hull(out, b);         break;
    case JCE_COLLIDER_MODE_CONVEX_DECOMP: emit_convex_decomp(out, b, cfg);  break;
    case JCE_COLLIDER_MODE_TRIANGLE_MESH: emit_triangle_mesh(out, b);       break;
    default:                              emit_convex_hull(out, b);         break;
    }
}

} /* namespace */

extern "C" {

JceColliderCookConfig jce_collider_cook_config_default(void)
{
    JceColliderCookConfig c;
    std::memset(&c, 0, sizeof(c));
    c.mode          = JCE_COLLIDER_MODE_AUTO;
    c.split         = JCE_COLLIDER_SPLIT_BY_PART;
    c.is_static     = true;
    c.detect_naming = true;
    return c;
}

bool jce_collider_cook(const JceColliderPart       *parts,
                       uint32_t                     part_count,
                       const JceColliderCookConfig *cfg_in,
                       JceCookedCollider           *out)
{
    if (!parts || part_count == 0 || !out) return false;

    JceColliderCookConfig cfg = cfg_in ? *cfg_in : jce_collider_cook_config_default();
    std::memset(out, 0, sizeof(*out));

    /* Naming-convention pass: if any part carries a recognized collision
       prefix, only prefixed parts are colliders (the rest are render
       meshes) and each prefix forces that part's shape. */
    bool naming_active = false;
    if (cfg.detect_naming) {
        for (uint32_t i = 0; i < part_count; ++i) {
            bool m = false;
            prefix_mode(parts[i].name, &m);
            if (m) { naming_active = true; break; }
        }
    }

    std::vector<JceCookedChild> children;

    if (naming_active) {
        for (uint32_t i = 0; i < part_count; ++i) {
            bool matched = false;
            JceColliderMode pm = prefix_mode(parts[i].name, &matched);
            if (!matched) continue;                       /* render mesh */
            if (pm == JCE_COLLIDER_MODE_AUTO) pm = cfg.mode; /* COL_ → cfg */
            Baked b; bake_part(parts[i], b);
            emit_unit(children, pm, cfg.is_static, b, cfg);
        }
    } else if (cfg.split == JCE_COLLIDER_SPLIT_WHOLE) {
        Baked b;
        for (uint32_t i = 0; i < part_count; ++i) bake_part(parts[i], b);
        emit_unit(children, cfg.mode, cfg.is_static, b, cfg);
    } else { /* one child (or VHACD group) per part */
        for (uint32_t i = 0; i < part_count; ++i) {
            if (parts[i].vertex_count == 0) continue;
            Baked b; bake_part(parts[i], b);
            emit_unit(children, cfg.mode, cfg.is_static, b, cfg);
        }
    }

    if (children.empty()) return false;

    out->child_count = (uint32_t)children.size();
    out->children = (JceCookedChild *)jce_malloc(children.size() * sizeof(JceCookedChild));
    if (!out->children) {
        for (auto &c : children) { jce_free(c.vertices); jce_free(c.indices); }
        out->child_count = 0;
        return false;
    }
    std::memcpy(out->children, children.data(),
                children.size() * sizeof(JceCookedChild));
    return true;
}

void jce_collider_cooked_free(JceCookedCollider *c)
{
    if (!c || !c->children) { if (c) std::memset(c, 0, sizeof(*c)); return; }
    for (uint32_t i = 0; i < c->child_count; ++i) {
        jce_free(c->children[i].vertices);
        jce_free(c->children[i].indices);
    }
    jce_free(c->children);
    c->children = nullptr;
    c->child_count = 0;
}

} /* extern "C" */
