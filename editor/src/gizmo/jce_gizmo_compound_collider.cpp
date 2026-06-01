/*
 * jce_gizmo_compound_collider.cpp  Compound-collider Scene View overlay.
 *
 * Visual contract:
 *   - Each cooked child shape is drawn as wireframe in its own palette
 *     colour, so a model split into N separated objects shows N distinct
 *     child colliders (never one fat box spanning the gaps).
 *   - Box   → 12 oriented edges.
 *   - Sphere→ 3 great circles.
 *   - Capsule→ two rings + 4 side lines along the local axis.
 *   - Convex hull → bounding box of the hull points (hull face data is
 *     not retained by the cook, so the tight AABB stands in for it).
 *   - Triangle mesh → triangle edges (capped to keep the line count sane).
 *
 * The cook (especially V-HACD) is far too expensive to run per frame, so
 * the model-space wireframe is cached keyed by model path + cook settings
 * and only rebuilt when those change.  Each frame the cached segments are
 * transformed by the owner entity's TRS and emitted through
 * jce_debug_draw_line — reusing the existing debug-line path.
 */

#include "jce_gizmo_compound_collider.h"

extern "C" {
#include <jce/renderer/jce_debug_draw.h>
#include <jce/middleware/physics/jce_collider_cook.h>
#include <jce/resource/jce_model_importer.h>
#include <jce/os/core/jce_math.h>
}

#include "../scene/jce_editor_scene_render.h"

#include <math.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace {

struct Seg {
    jce_vec3 a;
    jce_vec3 b;
    uint32_t abgr;
};

/* Distinct per-child palette (0xAABBGGRR) so separated objects stand
 * apart visually.  Cycles for models with many parts. */
const uint32_t kPalette[] = {
    0xFF00FF00u, /* green   */
    0xFF00FFFFu, /* yellow  */
    0xFFFF8000u, /* blue-ish*/
    0xFFFF00FFu, /* magenta */
    0xFF00A5FFu, /* orange  */
    0xFFFFFF00u, /* cyan    */
    0xFF4040FFu, /* red     */
    0xFF40FF40u, /* lime    */
};
constexpr int kPaletteN = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

constexpr uint32_t kMaxTriSegs = 6000u; /* cap trimesh edge spam */

std::unordered_map<std::string, std::vector<Seg>> s_cache;

inline jce_vec3 v3(float x, float y, float z) { return jce_v3(x, y, z); }

/* model-space child point: rotate local by child quat, add child origin. */
inline jce_vec3 child_pt(const JceCookedChild *c, jce_vec3 local)
{
    jce_quat q = { c->rotation[0], c->rotation[1], c->rotation[2], c->rotation[3] };
    jce_vec3 r = jce_q_rotate(q, local);
    return v3(r.x + c->position[0], r.y + c->position[1], r.z + c->position[2]);
}

void push_box(std::vector<Seg> &out, const JceCookedChild *c, uint32_t col)
{
    const float hx = c->half_extents[0];
    const float hy = c->half_extents[1];
    const float hz = c->half_extents[2];
    jce_vec3 p[8];
    int n = 0;
    for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2)
    for (int sz = -1; sz <= 1; sz += 2)
        p[n++] = child_pt(c, v3(sx * hx, sy * hy, sz * hz));
    /* corner index = (sx<<2)|(sy<<1)|sz with sign mapped -1→0, +1→1 */
    static const int e[12][2] = {
        {0,1},{0,2},{0,4},{1,3},{1,5},{2,3},
        {2,6},{3,7},{4,5},{4,6},{5,7},{6,7}
    };
    for (auto &pair : e)
        out.push_back({ p[pair[0]], p[pair[1]], col });
}

void push_ring(std::vector<Seg> &out, const JceCookedChild *c,
               int ax0, int ax1, float r, uint32_t col, int segs = 16)
{
    jce_vec3 prev{};
    for (int i = 0; i <= segs; ++i) {
        float t = (float)i / (float)segs * 6.2831853f;
        float u = r * cosf(t), v = r * sinf(t);
        float l[3] = { 0, 0, 0 };
        l[ax0] = u; l[ax1] = v;
        jce_vec3 cur = child_pt(c, v3(l[0], l[1], l[2]));
        if (i > 0) out.push_back({ prev, cur, col });
        prev = cur;
    }
}

void push_sphere(std::vector<Seg> &out, const JceCookedChild *c, uint32_t col)
{
    float r = c->half_extents[0];
    push_ring(out, c, 0, 1, r, col);
    push_ring(out, c, 0, 2, r, col);
    push_ring(out, c, 1, 2, r, col);
}

void push_capsule(std::vector<Seg> &out, const JceCookedChild *c, uint32_t col)
{
    float r  = c->half_extents[0];
    float hh = c->half_extents[1];           /* half-height along local Y */
    /* two rings in the XZ plane, offset along Y */
    for (int s = -1; s <= 1; s += 2) {
        jce_vec3 prev{};
        for (int i = 0; i <= 16; ++i) {
            float t = (float)i / 16.0f * 6.2831853f;
            jce_vec3 cur = child_pt(c, v3(r * cosf(t), s * hh, r * sinf(t)));
            if (i > 0) out.push_back({ prev, cur, col });
            prev = cur;
        }
    }
    /* 4 side lines connecting the rings */
    const float a[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
    for (auto &d : a) {
        jce_vec3 lo = child_pt(c, v3(r * d[0], -hh, r * d[1]));
        jce_vec3 hi = child_pt(c, v3(r * d[0],  hh, r * d[1]));
        out.push_back({ lo, hi, col });
    }
}

/* hull / trimesh geometry is baked in model space with identity
 * position/rotation, so vertices are used directly. */
void push_hull_bounds(std::vector<Seg> &out, const JceCookedChild *c, uint32_t col)
{
    if (!c->vertices || c->vertex_count == 0) return;
    float mn[3] = { c->vertices[0], c->vertices[1], c->vertices[2] };
    float mx[3] = { mn[0], mn[1], mn[2] };
    for (uint32_t i = 1; i < c->vertex_count; ++i) {
        const float *v = &c->vertices[i * 3u];
        for (int k = 0; k < 3; ++k) {
            if (v[k] < mn[k]) mn[k] = v[k];
            if (v[k] > mx[k]) mx[k] = v[k];
        }
    }
    jce_vec3 p[8];
    int n = 0;
    for (int sx = 0; sx < 2; ++sx)
    for (int sy = 0; sy < 2; ++sy)
    for (int sz = 0; sz < 2; ++sz)
        p[n++] = v3(sx ? mx[0] : mn[0], sy ? mx[1] : mn[1], sz ? mx[2] : mn[2]);
    static const int e[12][2] = {
        {0,1},{0,2},{0,4},{1,3},{1,5},{2,3},
        {2,6},{3,7},{4,5},{4,6},{5,7},{6,7}
    };
    for (auto &pair : e)
        out.push_back({ p[pair[0]], p[pair[1]], col });
}

void push_trimesh(std::vector<Seg> &out, const JceCookedChild *c, uint32_t col)
{
    if (!c->vertices || !c->indices) {
        push_hull_bounds(out, c, col);
        return;
    }
    uint32_t added = 0;
    for (uint32_t i = 0; i + 2 < c->index_count && added < kMaxTriSegs; i += 3) {
        const float *a = &c->vertices[c->indices[i + 0] * 3u];
        const float *b = &c->vertices[c->indices[i + 1] * 3u];
        const float *d = &c->vertices[c->indices[i + 2] * 3u];
        jce_vec3 va = v3(a[0], a[1], a[2]);
        jce_vec3 vb = v3(b[0], b[1], b[2]);
        jce_vec3 vd = v3(d[0], d[1], d[2]);
        out.push_back({ va, vb, col });
        out.push_back({ vb, vd, col });
        out.push_back({ vd, va, col });
        added += 3;
    }
}

std::string make_key(const JceCompoundColliderComponent *cc)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s|%u|%u|%d|%d|%u|%u|%u",
             cc->model_path, (unsigned)cc->mode, (unsigned)cc->split,
             cc->is_static ? 1 : 0, cc->detect_naming ? 1 : 0,
             cc->vhacd_resolution, cc->vhacd_max_hulls,
             cc->vhacd_max_verts_per_hull);
    return std::string(buf);
}

const std::vector<Seg> *get_or_build(const JceCompoundColliderComponent *cc)
{
    std::string key = make_key(cc);
    auto it = s_cache.find(key);
    if (it != s_cache.end()) return &it->second;

    std::vector<Seg> segs;

    char host[1024];
    JceModelParts parts;
    memset(&parts, 0, sizeof(parts));
    if (cc->model_path[0] &&
        jce_editor_resolve_asset_path(cc->model_path, host, (int)sizeof(host)) &&
        jce_model_importer_load_parts_file(host, &parts) && parts.count > 0) {

        std::vector<JceColliderPart> in(parts.count);
        for (uint32_t i = 0; i < parts.count; ++i) {
            in[i].name         = parts.parts[i].name;
            in[i].vertices     = parts.parts[i].positions;
            in[i].vertex_count = parts.parts[i].vertex_count;
            in[i].indices      = parts.parts[i].indices;
            in[i].index_count  = parts.parts[i].index_count;
            memcpy(in[i].transform, parts.parts[i].transform, sizeof(float) * 16);
        }

        JceColliderCookConfig cfg = jce_collider_cook_config_default();
        cfg.mode          = (JceColliderMode)cc->mode;
        cfg.split         = (JceColliderSplitMode)cc->split;
        cfg.is_static     = cc->is_static;
        cfg.detect_naming = cc->detect_naming;
        cfg.vhacd_resolution         = cc->vhacd_resolution;
        cfg.vhacd_max_hulls          = cc->vhacd_max_hulls;
        cfg.vhacd_max_verts_per_hull = cc->vhacd_max_verts_per_hull;

        JceCookedCollider cooked;
        memset(&cooked, 0, sizeof(cooked));
        if (jce_collider_cook(in.data(), parts.count, &cfg, &cooked)) {
            for (uint32_t i = 0; i < cooked.child_count; ++i) {
                const JceCookedChild *ch = &cooked.children[i];
                uint32_t col = kPalette[i % kPaletteN];
                switch (ch->shape) {
                case JCE_SHAPE_BOX:           push_box(segs, ch, col);          break;
                case JCE_SHAPE_SPHERE:        push_sphere(segs, ch, col);       break;
                case JCE_SHAPE_CAPSULE:       push_capsule(segs, ch, col);      break;
                case JCE_SHAPE_CONVEX_HULL:   push_hull_bounds(segs, ch, col);  break;
                case JCE_SHAPE_TRIANGLE_MESH: push_trimesh(segs, ch, col);      break;
                default:                      push_hull_bounds(segs, ch, col);  break;
                }
            }
            jce_collider_cooked_free(&cooked);
        }
    }
    if (parts.parts) jce_model_importer_free_parts(&parts);

    auto res = s_cache.emplace(std::move(key), std::move(segs));
    return &res.first->second;
}

} /* anonymous namespace */

extern "C" void jce_gizmo_compound_collider_clear_cache(void)
{
    s_cache.clear();
}

extern "C" void jce_gizmo_compound_collider_draw_from_component(
    JceScene *scene, JceEntity owner, const JceCompoundColliderComponent *cc)
{
    if (!scene || !cc) return;

    const std::vector<Seg> *segs = get_or_build(cc);
    if (!segs || segs->empty()) return;

    JceTransform *t = jce_scene_get_transform(scene, owner);
    jce_vec3 pos   = t ? t->position : v3(0, 0, 0);
    jce_quat rot   = t ? t->rotation : jce_q_identity();
    jce_vec3 scale = t ? t->scale    : v3(1, 1, 1);

    auto xform = [&](jce_vec3 m) -> jce_vec3 {
        jce_vec3 s = v3(m.x * scale.x, m.y * scale.y, m.z * scale.z);
        return jce_v3_add(pos, jce_q_rotate(rot, s));
    };

    for (const Seg &sg : *segs)
        jce_debug_draw_line(xform(sg.a), xform(sg.b), sg.abgr);
}
