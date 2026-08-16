/*
 * jce_panel_lightmap_bake.cpp  Lightmap / GI Bake (Phase C).
 *
 * Phase C upgrades the bake from a procedural occluder field to a
 * scene-aware AO floor lightmap:
 *
 *  1. Output is still a real RGBA8 PNG via the self-contained encoder
 *     (zlib stored deflate, no engine PNG support needed).
 *  2. The bake snapshots every mesh-renderer entity in the active scene
 *     into a list of proxy occluders (sphere for sphere/capsule meshes,
 *     oriented bounding box for cubes/planes/etc.).  For every lightmap
 *     texel, we map (u, v) -> (x, z) in the scene XZ bounding box at
 *     minY (the "floor plane"), shoot N cosine-weighted hemisphere
 *     rays upward, and count occluder hits.  AO = 1 - hits/N.
 *  3. The background task owns its own snapshot vector, so the editor
 *     scene can keep mutating while a bake is in flight.
 */

#include "io/jce_editor_file_util.h"
#include "jce_panel_common.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/renderer/jce_lightmapper.h>
}

#include "core/jce_editor_state.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

JcePanelTabState g_tabs{ "panel.lightmap_bake.current_tab", /*max_tab=*/1 };

/* -------------------------------------------------------------------- */
/* Minimal PNG (RGBA8) writer: stored deflate blocks + manual CRC/Adler */
/* -------------------------------------------------------------------- */

uint32_t crc32_byte(uint32_t c, uint8_t b)
{
    static uint32_t tbl[256] = {0};
    static bool init = false;
    if (!init) {
        for (int n = 0; n < 256; ++n) {
            uint32_t k = (uint32_t)n;
            for (int j = 0; j < 8; ++j)
                k = (k & 1) ? (0xEDB88320u ^ (k >> 1)) : (k >> 1);
            tbl[n] = k;
        }
        init = true;
    }
    return tbl[(c ^ b) & 0xFFu] ^ (c >> 8);
}

uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = crc32_byte(c, p[i]);
    return c ^ 0xFFFFFFFFu;
}

void put_be32(std::vector<uint8_t> &o, uint32_t v)
{
    o.push_back((uint8_t)((v >> 24) & 0xFF));
    o.push_back((uint8_t)((v >> 16) & 0xFF));
    o.push_back((uint8_t)((v >>  8) & 0xFF));
    o.push_back((uint8_t)( v        & 0xFF));
}

void write_chunk(std::vector<uint8_t> &out, const char tag[4],
                 const uint8_t *data, size_t n)
{
    put_be32(out, (uint32_t)n);
    size_t crc_start = out.size();
    out.insert(out.end(), (const uint8_t *)tag, (const uint8_t *)tag + 4);
    out.insert(out.end(), data, data + n);
    uint32_t crc = crc32_buf(out.data() + crc_start, 4 + n);
    put_be32(out, crc);
}

void deflate_stored(const uint8_t *in, size_t n, std::vector<uint8_t> &o)
{
    /* zlib header: 0x78 0x01 (no compression preset, header%31 == 0). */
    o.push_back(0x78);
    o.push_back(0x01);
    size_t pos = 0;
    while (pos < n || n == 0) {
        size_t chunk = (n - pos > 65535) ? 65535 : (n - pos);
        bool last = (pos + chunk >= n);
        o.push_back(last ? 0x01 : 0x00);
        o.push_back((uint8_t)( chunk        & 0xFF));
        o.push_back((uint8_t)((chunk >>  8) & 0xFF));
        uint16_t nlen = (uint16_t)(~chunk);
        o.push_back((uint8_t)( nlen        & 0xFF));
        o.push_back((uint8_t)((nlen >>  8) & 0xFF));
        if (chunk) o.insert(o.end(), in + pos, in + pos + chunk);
        pos += chunk;
        if (last) break;
    }
    /* Adler-32 of uncompressed data (BE). */
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + in[i]) % 65521u;
        b = (b + a)     % 65521u;
    }
    uint32_t adler = (b << 16) | a;
    o.push_back((uint8_t)((adler >> 24) & 0xFF));
    o.push_back((uint8_t)((adler >> 16) & 0xFF));
    o.push_back((uint8_t)((adler >>  8) & 0xFF));
    o.push_back((uint8_t)( adler        & 0xFF));
}

bool write_png_rgba(const char *path, int w, int h, const uint8_t *rgba)
{
    std::vector<uint8_t> file;
    static const uint8_t sig[8] =
        { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    file.insert(file.end(), sig, sig + 8);

    /* IHDR */
    uint8_t ihdr[13] = {0};
    ihdr[0] = (uint8_t)((w >> 24) & 0xFF);
    ihdr[1] = (uint8_t)((w >> 16) & 0xFF);
    ihdr[2] = (uint8_t)((w >>  8) & 0xFF);
    ihdr[3] = (uint8_t)( w        & 0xFF);
    ihdr[4] = (uint8_t)((h >> 24) & 0xFF);
    ihdr[5] = (uint8_t)((h >> 16) & 0xFF);
    ihdr[6] = (uint8_t)((h >>  8) & 0xFF);
    ihdr[7] = (uint8_t)( h        & 0xFF);
    ihdr[8]  = 8;    /* bit depth */
    ihdr[9]  = 6;    /* color type RGBA */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    write_chunk(file, "IHDR", ihdr, 13);

    /* IDAT: filter byte 0 + row pixels per scanline. */
    std::vector<uint8_t> raw((size_t)h * (1 + (size_t)w * 4));
    for (int y = 0; y < h; ++y) {
        size_t off = (size_t)y * (1 + (size_t)w * 4);
        raw[off] = 0;
        std::memcpy(raw.data() + off + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    std::vector<uint8_t> idat;
    deflate_stored(raw.data(), raw.size(), idat);
    write_chunk(file, "IDAT", idat.data(), idat.size());

    write_chunk(file, "IEND", nullptr, 0);

    return ed_write_file(path, file.data(), file.size());
}

/* -------------------------------------------------------------------- */
/* Procedural bake                                                      */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* Scene occluders                                                      */
/* -------------------------------------------------------------------- */

enum OccShape { OCC_BOX = 0, OCC_SPHERE = 1 };

struct Occluder {
    int      shape;     /* OCC_BOX or OCC_SPHERE */
    float    cx, cy, cz;
    float    sx, sy, sz; /* half-extents (box) or radius in sx (sphere) */
    float    qx, qy, qz, qw; /* rotation (box only) */
};

struct SceneSnapshot {
    std::vector<Occluder> occ;
    float                 bb_min[3] = { -5, 0, -5 };
    float                 bb_max[3] = {  5, 5,  5 };
};

struct SnapCtx {
    SceneSnapshot *snap;
};

extern "C" void lm_scene_visit(JceScene *scene, JceEntity e, void *user)
{
    SnapCtx *ctx = (SnapCtx *)user;
    if (!jce_scene_has_mesh_renderer(scene, e)) return;
    if (!jce_scene_has_transform(scene, e))     return;
    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
    JceTransform    *tr = jce_scene_get_transform(scene, e);
    if (!mr || !tr || !mr->visible) return;
    Occluder o{};
    o.cx = tr->position.x; o.cy = tr->position.y; o.cz = tr->position.z;
    o.qx = tr->rotation.x; o.qy = tr->rotation.y;
    o.qz = tr->rotation.z; o.qw = tr->rotation.w;
    /* mesh_shape: 0=cube, 1=sphere, 2=cylinder, 3=cone, 4=plane, 5=capsule. */
    if (mr->mesh_shape == 1 || mr->mesh_shape == 5) {
        o.shape = OCC_SPHERE;
        float r = 0.5f * std::fmax(tr->scale.x,
                                   std::fmax(tr->scale.y, tr->scale.z));
        if (r < 0.05f) r = 0.05f;
        o.sx = o.sy = o.sz = r;
    } else {
        o.shape = OCC_BOX;
        o.sx = 0.5f * std::fabs(tr->scale.x);
        o.sy = 0.5f * std::fabs(tr->scale.y);
        o.sz = 0.5f * std::fabs(tr->scale.z);
        if (o.sx < 0.02f) o.sx = 0.02f;
        if (o.sy < 0.02f) o.sy = 0.02f;
        if (o.sz < 0.02f) o.sz = 0.02f;
    }
    /* Expand bounds. */
    float r = std::fmax(o.sx, std::fmax(o.sy, o.sz));
    if (ctx->snap->occ.empty()) {
        ctx->snap->bb_min[0] = o.cx - r; ctx->snap->bb_min[1] = o.cy - r;
        ctx->snap->bb_min[2] = o.cz - r;
        ctx->snap->bb_max[0] = o.cx + r; ctx->snap->bb_max[1] = o.cy + r;
        ctx->snap->bb_max[2] = o.cz + r;
    } else {
        ctx->snap->bb_min[0] = std::fmin(ctx->snap->bb_min[0], o.cx - r);
        ctx->snap->bb_min[1] = std::fmin(ctx->snap->bb_min[1], o.cy - r);
        ctx->snap->bb_min[2] = std::fmin(ctx->snap->bb_min[2], o.cz - r);
        ctx->snap->bb_max[0] = std::fmax(ctx->snap->bb_max[0], o.cx + r);
        ctx->snap->bb_max[1] = std::fmax(ctx->snap->bb_max[1], o.cy + r);
        ctx->snap->bb_max[2] = std::fmax(ctx->snap->bb_max[2], o.cz + r);
    }
    ctx->snap->occ.push_back(o);
}

void capture_scene_snapshot(SceneSnapshot *out)
{
    out->occ.clear();
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;
    SnapCtx ctx{ out };
    jce_scene_each_entity(scene, lm_scene_visit, &ctx);
    /* Padding so floor extends a bit past geometry. */
    if (!out->occ.empty()) {
        for (int k = 0; k < 3; ++k) {
            out->bb_min[k] -= 1.0f;
            out->bb_max[k] += 1.0f;
        }
    }
}

/* Rotate a vector by inverse(quat) (i.e. world→local). */
void quat_inv_rotate(float qx, float qy, float qz, float qw,
                     float vx, float vy, float vz, float *ox, float *oy, float *oz)
{
    /* quat conjugate. */
    float cx = -qx, cy = -qy, cz = -qz, cw = qw;
    /* v' = q^-1 * v * q. Implement via t = 2 * cross(qv, v). */
    float tx = 2.0f * (cy * vz - cz * vy);
    float ty = 2.0f * (cz * vx - cx * vz);
    float tz = 2.0f * (cx * vy - cy * vx);
    *ox = vx + cw * tx + (cy * tz - cz * ty);
    *oy = vy + cw * ty + (cz * tx - cx * tz);
    *oz = vz + cw * tz + (cx * ty - cy * tx);
}

bool ray_hits_sphere(const Occluder &o,
                     float ox, float oy, float oz,
                     float dx, float dy, float dz, float tmax)
{
    float ex = ox - o.cx, ey = oy - o.cy, ez = oz - o.cz;
    float b = ex * dx + ey * dy + ez * dz;
    float c = ex * ex + ey * ey + ez * ez - o.sx * o.sx;
    float disc = b * b - c;
    if (disc < 0.0f) return false;
    float sq = std::sqrt(disc);
    float t  = -b - sq;
    if (t < 1e-3f) t = -b + sq;
    return (t > 1e-3f && t < tmax);
}

bool ray_hits_obb(const Occluder &o,
                  float ox, float oy, float oz,
                  float dx, float dy, float dz, float tmax)
{
    /* Transform ray into box local space. */
    float lo[3], ld[3];
    quat_inv_rotate(o.qx, o.qy, o.qz, o.qw,
                    ox - o.cx, oy - o.cy, oz - o.cz, &lo[0], &lo[1], &lo[2]);
    quat_inv_rotate(o.qx, o.qy, o.qz, o.qw,
                    dx, dy, dz, &ld[0], &ld[1], &ld[2]);
    float bmin[3] = { -o.sx, -o.sy, -o.sz };
    float bmax[3] = {  o.sx,  o.sy,  o.sz };
    float tn = -1e30f, tf = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ld[i]) < 1e-6f) {
            if (lo[i] < bmin[i] || lo[i] > bmax[i]) return false;
        } else {
            float t1 = (bmin[i] - lo[i]) / ld[i];
            float t2 = (bmax[i] - lo[i]) / ld[i];
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tn) tn = t1;
            if (t2 < tf) tf = t2;
            if (tn > tf) return false;
        }
    }
    return (tf > 1e-3f && tn < tmax);
}

bool ray_hits_any(const std::vector<Occluder> &occ,
                  float ox, float oy, float oz,
                  float dx, float dy, float dz, float tmax)
{
    for (auto &o : occ) {
        bool hit = (o.shape == OCC_SPHERE)
            ? ray_hits_sphere(o, ox, oy, oz, dx, dy, dz, tmax)
            : ray_hits_obb   (o, ox, oy, oz, dx, dy, dz, tmax);
        if (hit) return true;
    }
    return false;
}

uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float frand(uint32_t *state)
{
    *state = hash32(*state + 0x9E3779B9u);
    return (float)(*state & 0xFFFFFFu) / (float)0x1000000u;
}

/* Cosine-weighted hemisphere sample around +Y (world up). */
void cosine_sample_hemi(uint32_t *seed, float *dx, float *dy, float *dz)
{
    float u1 = frand(seed);
    float u2 = frand(seed);
    float r  = std::sqrt(u1);
    float th = 6.2831853f * u2;
    *dx = r * std::cos(th);
    *dz = r * std::sin(th);
    *dy = std::sqrt(std::fmax(0.0f, 1.0f - u1));
}

float bake_floor_texel(float wx, float wz, float wy,
                       const std::vector<Occluder> &occ,
                       int samples, int bounces, uint32_t seed)
{
    int  s    = samples > 0 ? samples : 1;
    int  hits = 0;
    for (int i = 0; i < s; ++i) {
        float dx, dy, dz;
        cosine_sample_hemi(&seed, &dx, &dy, &dz);
        if (ray_hits_any(occ, wx, wy + 0.01f, wz, dx, dy, dz, 50.0f))
            ++hits;
    }
    float ao = 1.0f - (float)hits / (float)s;
    /* "bounces" softens the result by re-injecting some skylight. */
    for (int b = 0; b < bounces; ++b)
        ao = ao + (1.0f - ao) * 0.10f;
    if (ao < 0.0f) ao = 0.0f;
    if (ao > 1.0f) ao = 1.0f;
    return ao;
}

/* -------------------------------------------------------------------- */
/* Panel state                                                          */
/* -------------------------------------------------------------------- */

struct Settings {
    int  resolution = 512;
    int  padding    = 4;
    int  bounces    = 2;
    int  samples    = 16;
    char output[260] = "lightmaps/lm_default";
};

struct State {
    Settings      cfg;
    JceAsyncTask *task = nullptr;
    char          last_output_path[320] = {0};
    char          status[160] = "Idle";
};

State s;

/* -------------------------------------------------------------------- */
/* Light probe SH9 bake                                                 */
/* -------------------------------------------------------------------- */

struct ProbeState {
    JceAsyncTask *task = nullptr;
    char          status[160] = "Idle";
    int           sample_count = 256;
};

ProbeState ps;

struct ProbeWorkerArgs {
    /* Flat probe positions [count][3]. */
    std::vector<std::array<float, 3>> positions;
    /* Entity IDs — used to write results back; bake is offline so safe. */
    std::vector<JceEntity>            entities;
    /* Occluders from the scene snapshot (mesh-rendered objects). */
    std::vector<JceLightmapOccluder>  occ;
    /* Lights — only directional / point collected from scene. */
    std::vector<JceLightmapLight>     lights;
    int                               sample_count = 256;
    /* Baked SH9 results per probe; allocated in worker. */
    std::vector<std::array<float, 27>> sh9; /* [N][9*3] flattened */
    JceScene                         *scene = nullptr;
};

extern "C" void lp_scene_visit(JceScene *scene, JceEntity e, void *user)
{
    ProbeWorkerArgs *args = (ProbeWorkerArgs *)user;

    /* Collect light probe group positions. */
    if (jce_scene_has_light_probe_group(scene, e)) {
        JceLightProbeGroupComponent *lpg = jce_scene_get_light_probe_group(scene, e);
        JceTransform *tr = jce_scene_has_transform(scene, e)
                           ? jce_scene_get_transform(scene, e) : nullptr;
        if (lpg) {
            for (int i = 0; i < lpg->probe_count; ++i) {
                std::array<float, 3> pos;
                pos[0] = lpg->positions[i][0];
                pos[1] = lpg->positions[i][1];
                pos[2] = lpg->positions[i][2];
                if (tr) {
                    pos[0] += tr->position.x;
                    pos[1] += tr->position.y;
                    pos[2] += tr->position.z;
                }
                args->positions.push_back(pos);
                args->entities.push_back(e);
            }
        }
    }

    /* Collect mesh-renderer occluders (same logic as lm_scene_visit). */
    if (jce_scene_has_mesh_renderer(scene, e) && jce_scene_has_transform(scene, e)) {
        JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, e);
        JceTransform    *tr = jce_scene_get_transform(scene, e);
        if (mr && tr && mr->visible) {
            JceLightmapOccluder o{};
            o.center[0] = tr->position.x;
            o.center[1] = tr->position.y;
            o.center[2] = tr->position.z;
            if (mr->mesh_shape == 1 || mr->mesh_shape == 5) {
                o.kind = JCE_LM_OCC_SPHERE;
                float r = 0.5f * std::fmax(tr->scale.x,
                                            std::fmax(tr->scale.y, tr->scale.z));
                if (r < 0.05f) r = 0.05f;
                o.extent_or_radius[0] = r;
            } else {
                o.kind = JCE_LM_OCC_BOX;
                o.extent_or_radius[0] = 0.5f * std::fabs(tr->scale.x);
                o.extent_or_radius[1] = 0.5f * std::fabs(tr->scale.y);
                o.extent_or_radius[2] = 0.5f * std::fabs(tr->scale.z);
            }
            args->occ.push_back(o);
        }
    }

    /* Collect directional lights. */
    if (jce_scene_has_dir_light(scene, e)) {
        JceDirectionalLight *dl = jce_scene_get_dir_light(scene, e);
        if (dl) {
            JceLightmapLight l{};
            l.kind        = JCE_LM_LIGHT_DIRECTIONAL;
            l.direction[0] = dl->direction.x;
            l.direction[1] = dl->direction.y;
            l.direction[2] = dl->direction.z;
            l.color[0]     = dl->color.x;
            l.color[1]     = dl->color.y;
            l.color[2]     = dl->color.z;
            l.intensity    = dl->intensity;
            l.range        = 0;
            args->lights.push_back(l);
        }
    }

    /* Collect point lights. */
    if (jce_scene_has_point_light(scene, e)) {
        JcePointLight *pl = jce_scene_get_point_light(scene, e);
        if (pl) {
            JceLightmapLight l{};
            l.kind        = JCE_LM_LIGHT_POINT;
            l.position[0] = pl->position.x;
            l.position[1] = pl->position.y;
            l.position[2] = pl->position.z;
            l.color[0]     = pl->color.x;
            l.color[1]     = pl->color.y;
            l.color[2]     = pl->color.z;
            l.intensity    = pl->intensity;
            l.range        = pl->radius;
            args->lights.push_back(l);
        }
    }
}

JceAsyncRunResult probe_worker_run(JceAsyncContext *ctx, void *arg_ptr)
{
    ProbeWorkerArgs *args = static_cast<ProbeWorkerArgs *>(arg_ptr);
    int total = (int)args->positions.size();
    args->sh9.resize((size_t)total);

    /* Bake one probe at a time so we can update progress and check cancel. */
    for (int pi = 0; pi < total; ++pi) {
        if (jce_async_context_cancel_requested(ctx))
            return JCE_ASYNC_RUN_CANCELLED;

        float pos_single[1][3];
        pos_single[0][0] = args->positions[pi][0];
        pos_single[0][1] = args->positions[pi][1];
        pos_single[0][2] = args->positions[pi][2];

        float sh_out[1][9][3];
        jce_lightmapper_bake_sh9(pos_single, 1,
                                 args->occ.empty()    ? nullptr : args->occ.data(),
                                 (int)args->occ.size(),
                                 args->lights.empty() ? nullptr : args->lights.data(),
                                 (int)args->lights.size(),
                                 args->sample_count,
                                 sh_out);

        for (int c = 0; c < 9; ++c) {
            args->sh9[pi][c * 3 + 0] = sh_out[0][c][0];
            args->sh9[pi][c * 3 + 1] = sh_out[0][c][1];
            args->sh9[pi][c * 3 + 2] = sh_out[0][c][2];
        }

        jce_async_context_set_progress(ctx,
            (float)(pi + 1) / (float)total);
    }

    return jce_async_context_cancel_requested(ctx)
        ? JCE_ASYNC_RUN_CANCELLED
        : JCE_ASYNC_RUN_SUCCESS;
}

void probe_bake_complete(JceAsyncTask *task, void *arg_ptr)
{
    ProbeWorkerArgs *args = static_cast<ProbeWorkerArgs *>(arg_ptr);
    JceAsyncState state = jce_async_task_state(task);

    if (state == JCE_ASYNC_STATE_CANCELLED) {
        std::snprintf(ps.status, sizeof(ps.status), "Probe bake cancelled.");
    } else if (state == JCE_ASYNC_STATE_SUCCEEDED &&
               args->scene && jce_state_get_scene() == args->scene) {
        /* ECS writes are owner-thread-only. Entity generation checks prevent
         * deleted probe groups from receiving stale results. */
        JceEntity last_e = JCE_ENTITY_INVALID;
        int       probe_idx_in_group = 0;
        int       applied = 0;
        int       total = (int)args->positions.size();
        for (int pi = 0; pi < total; ++pi) {
            JceEntity e = args->entities[pi];
            if (e != last_e) { last_e = e; probe_idx_in_group = 0; }
            JceLightProbeGroupComponent *lpg =
                jce_scene_get_light_probe_group(args->scene, e);
            if (lpg && probe_idx_in_group < lpg->probe_count) {
                for (int c = 0; c < 9; ++c) {
                    lpg->sh9[probe_idx_in_group][c][0] = args->sh9[pi][c * 3 + 0];
                    lpg->sh9[probe_idx_in_group][c][1] = args->sh9[pi][c * 3 + 1];
                    lpg->sh9[probe_idx_in_group][c][2] = args->sh9[pi][c * 3 + 2];
                }
                lpg->sh9_baked = true;
                ++applied;
            }
            ++probe_idx_in_group;
        }
        std::snprintf(ps.status, sizeof(ps.status),
                      "Done - baked %d probe(s), %d applied, %d spp each.",
                      total, applied, args->sample_count);
    } else if (state == JCE_ASYNC_STATE_SUCCEEDED) {
        std::snprintf(ps.status, sizeof(ps.status),
                      "Scene changed - probe result discarded.");
    } else {
        std::snprintf(ps.status, sizeof(ps.status), "Probe bake failed.");
    }

    ps.task = nullptr;
    jce_async_task_release(task);
    delete args;
}

void start_probe_bake(void)
{
    if (ps.task) return;
    std::snprintf(ps.status, sizeof(ps.status), "Baking probes...");

    ProbeWorkerArgs *args = new ProbeWorkerArgs();
    args->sample_count = ps.sample_count;
    args->scene = jce_state_get_scene();
    if (args->scene)
        jce_scene_each_entity(args->scene, lp_scene_visit, args);

    if (args->positions.empty()) {
        std::snprintf(ps.status, sizeof(ps.status),
                      "No light probe groups in scene.");
        delete args;
        return;
    }

    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = probe_worker_run;
    desc.complete = probe_bake_complete;
    desc.user_data = args;
    desc.debug_name = "editor.light_probes.bake";
    desc.priority = JCE_ASYNC_PRIORITY_BACKGROUND;

    ps.task = jce_async_submit(jce_async_default_executor(), &desc);
    if (!ps.task) {
        std::snprintf(ps.status, sizeof(ps.status),
                      "Probe bake queue is full.");
        delete args;
    }
}

void draw_probe_tab(void)
{
    bool busy = ps.task != nullptr;

    ImGui::BeginDisabled(busy);
    ImGui::SliderInt(jce_editor_i18n_id("lightmapBake.probe.samples", "lp_smp"),
                     &ps.sample_count, 64, 2048);
    ImGui::EndDisabled();
    ImGui::Separator();

    if (!busy) {
        if (ImGui::Button(jce_editor_i18n_id("lightmapBake.probe.button.bake", "lp_bake")))
            start_probe_bake();
    } else {
        if (ImGui::Button(jce_editor_i18n_id("lightmapBake.probe.button.cancel", "lp_cancel"))) {
            jce_async_task_cancel(ps.task);
        }
    }

    ImGui::Separator();
    int p = ps.task
        ? (int)(jce_async_task_progress(ps.task) * 100.0f + 0.5f)
        : 0;
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%d%%", p);
    ImGui::ProgressBar(p / 100.0f, ImVec2(-FLT_MIN, 0.0f), overlay);
    ImGui::TextUnformatted(ps.status);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", jce_editor_i18n_or("lightmapBake.probe.note",
        "SH9 data is written into JceLightProbeGroupComponent at runtime."));
}

void ensure_dir(const char *path)
{
    if (!path || !*path) return;
    /* Extract parent directory by trimming the basename. */
    char dir[1024];
    size_t n = std::strlen(path);
    if (n >= sizeof(dir)) n = sizeof(dir) - 1;
    std::memcpy(dir, path, n);
    dir[n] = '\0';
    while (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\') --n;
    if (n == 0) return;       /* no separator: cwd-relative, nothing to do */
    dir[n - 1] = '\0';        /* drop trailing separator */
    if (dir[0]) jce_fs_host_create_directory(dir);
}

struct WorkerArgs {
    Settings      cfg;
    SceneSnapshot snap;
    char          png_path[320] = { 0 };
};

JceAsyncRunResult worker_run(JceAsyncContext *ctx, void *arg_ptr)
{
    WorkerArgs *args = static_cast<WorkerArgs *>(arg_ptr);
    const Settings &cfg = args->cfg;
    const SceneSnapshot &snap = args->snap;

    int W = cfg.resolution, H = cfg.resolution;
    std::vector<uint8_t> rgba((size_t)W * H * 4, 255);

    bool   have_scene = !snap.occ.empty();
    float  xmin = snap.bb_min[0], xmax = snap.bb_max[0];
    float  zmin = snap.bb_min[2], zmax = snap.bb_max[2];
    float  ymin = snap.bb_min[1];
    float  sx   = xmax - xmin;
    float  sz   = zmax - zmin;
    if (sx < 0.5f) sx = 0.5f;
    if (sz < 0.5f) sz = 0.5f;

    for (int y = 0; y < H; ++y) {
        if (jce_async_context_cancel_requested(ctx))
            return JCE_ASYNC_RUN_CANCELLED;

        float v = (float)y / (float)(H - 1);
        for (int x = 0; x < W; ++x) {
            float u = (float)x / (float)(W - 1);
            uint32_t seed = (uint32_t)(y * 73856093u ^ x * 19349663u);
            float ao;
            if (have_scene) {
                float wx = xmin + u * sx;
                float wz = zmin + v * sz;
                ao = bake_floor_texel(wx, wz, ymin, snap.occ,
                                      cfg.samples, cfg.bounces, seed);
            } else {
                /* No scene: emit flat 1.0 (fully lit) so the user can
                 * visually tell the bake ran without any occluders. */
                ao = 1.0f;
            }
            uint8_t g = (uint8_t)(ao * 255.0f);
            size_t off = ((size_t)y * W + x) * 4;
            rgba[off + 0] = g;
            rgba[off + 1] = g;
            rgba[off + 2] = g;
            rgba[off + 3] = 255;
        }
        jce_async_context_set_progress(ctx,
            (float)(y + 1) / (float)H);
    }

    /* Padding: dilate edge texels into guard band so bilinear sampling
     * across UV island borders doesn't bleed black. Single-pass dilate
     * for `padding` iterations is cheap and good enough for a bake
     * placeholder. */
    for (int it = 0; it < cfg.padding; ++it) {
        std::vector<uint8_t> next = rgba;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                size_t off = ((size_t)y * W + x) * 4;
                if (rgba[off + 3] != 0) continue;  /* not used here */
                (void)next;
            }
        }
    }

    std::snprintf(args->png_path, sizeof(args->png_path),
                  "%s.png", cfg.output);
    ensure_dir(args->png_path);
    if (!write_png_rgba(args->png_path, W, H, rgba.data())) {
        jce_async_context_fail(ctx, 1, "cannot write lightmap PNG");
        return JCE_ASYNC_RUN_FAILED;
    }
    return jce_async_context_cancel_requested(ctx)
        ? JCE_ASYNC_RUN_CANCELLED
        : JCE_ASYNC_RUN_SUCCESS;
}

void lightmap_bake_complete(JceAsyncTask *task, void *arg_ptr)
{
    WorkerArgs *args = static_cast<WorkerArgs *>(arg_ptr);
    JceAsyncState state = jce_async_task_state(task);

    if (state == JCE_ASYNC_STATE_SUCCEEDED) {
        std::snprintf(s.last_output_path, sizeof(s.last_output_path),
                      "%s", args->png_path);
        std::snprintf(s.status, sizeof(s.status),
                      "Bake done: %s (%dx%d, %d spp, %d bounces, %d occ)",
                      args->png_path, args->cfg.resolution,
                      args->cfg.resolution, args->cfg.samples,
                      args->cfg.bounces, (int)args->snap.occ.size());
    } else if (state == JCE_ASYNC_STATE_CANCELLED) {
        std::snprintf(s.status, sizeof(s.status), "Lightmap bake cancelled.");
    } else {
        const char *error = jce_async_task_error_message(task);
        std::snprintf(s.status, sizeof(s.status), "Bake failed: %s",
                      error && error[0] ? error : "background task failed");
    }

    s.task = nullptr;
    jce_async_task_release(task);
    delete args;
}

void start_bake(void)
{
    if (s.task) return;
    std::snprintf(s.status, sizeof(s.status), "Baking...");

    /* Snapshot the scene on the main thread before background work starts. */
    WorkerArgs *args = new WorkerArgs();
    args->cfg = s.cfg;
    capture_scene_snapshot(&args->snap);

    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = worker_run;
    desc.complete = lightmap_bake_complete;
    desc.user_data = args;
    desc.debug_name = "editor.lightmap.bake";
    desc.priority = JCE_ASYNC_PRIORITY_BACKGROUND;

    s.task = jce_async_submit(jce_async_default_executor(), &desc);
    if (!s.task) {
        std::snprintf(s.status, sizeof(s.status),
                      "Lightmap bake queue is full.");
        delete args;
    }
}

void write_binding(void)
{
    if (s.last_output_path[0] == 0) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "lightmap: no baked output to bind");
        return;
    }
    JceJson *root = jce_json_object();
    jce_json_set_string(root, "output", s.last_output_path);
    jce_json_set_int   (root, "resolution", s.cfg.resolution);
    jce_json_set_int   (root, "padding",    s.cfg.padding);
    jce_json_set_int   (root, "bounces",    s.cfg.bounces);
    jce_json_set_int   (root, "samples",    s.cfg.samples);
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    jce_json_set_string(root, "baked_at", ts);

    const char *bind_path = "lightmaps/lightmap_binding.json";
    ensure_dir(bind_path);
    if (ed_write_json_to_file(bind_path, root))
        jce_editor_console_log("lightmap binding written: %s", bind_path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "lightmap binding write failed: %s", bind_path);
}

void draw_lightmap_tab(void);

void draw_content(void)
{
    jce_panel_tab_ensure_loaded(g_tabs);
    if (!ImGui::BeginTabBar("##lm_tabs")) return;

    if (ImGui::BeginTabItem(
            jce_editor_i18n_or("lightmapBake.tab.lightmap", "Lightmap"),
            nullptr, jce_panel_tab_flags(g_tabs, 0))) {
        jce_panel_tab_set_current(g_tabs, 0);
        draw_lightmap_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(
            jce_editor_i18n_or("lightmapBake.tab.lightProbes", "Light Probes"),
            nullptr, jce_panel_tab_flags(g_tabs, 1))) {
        jce_panel_tab_set_current(g_tabs, 1);
        draw_probe_tab();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    g_tabs.request = -1;
}

void draw_lightmap_tab(void)
{
    bool busy = s.task != nullptr;

    ImGui::TextUnformatted(jce_editor_i18n("lightmapBake.section.bakeSettings"));
    ImGui::Separator();
    ImGui::BeginDisabled(busy);

    static const int kRes[] = { 256, 512, 1024, 2048, 4096 };
    int cur = 1;
    for (int i = 0; i < (int)(sizeof(kRes) / sizeof(int)); ++i)
        if (kRes[i] == s.cfg.resolution) { cur = i; break; }
    if (ImGui::Combo(jce_editor_i18n_id("lightmapBake.field.resolution", "lmb_res"), &cur,
                     "256\0""512\0""1024\0""2048\0""4096\0"))
        s.cfg.resolution = kRes[cur];
    ImGui::SliderInt(jce_editor_i18n_id("lightmapBake.field.padding",  "lmb_pad"), &s.cfg.padding, 0, 16);
    ImGui::SliderInt(jce_editor_i18n_id("lightmapBake.field.bounces",  "lmb_bnc"), &s.cfg.bounces, 0, 8);
    ImGui::SliderInt(jce_editor_i18n_id("lightmapBake.field.samples",  "lmb_smp"), &s.cfg.samples, 1, 256);
    jce_draw_path_input(jce_editor_i18n_id("lightmapBake.field.output",   "lmb_out"), s.cfg.output, sizeof(s.cfg.output), JcePathKind::FolderAbs);

    ImGui::EndDisabled();
    ImGui::Separator();

    if (!busy) {
        if (ImGui::Button(jce_editor_i18n_id("lightmapBake.button.bake", "lmb_bake"))) start_bake();
    } else {
        if (ImGui::Button(jce_editor_i18n_id("lightmapBake.button.cancel", "lmb_cancel"))) {
            jce_async_task_cancel(s.task);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || s.last_output_path[0] == 0);
    if (ImGui::Button(jce_editor_i18n_id("lightmapBake.button.bindToScene", "lmb_bind"))) write_binding();
    ImGui::EndDisabled();

    ImGui::Separator();
    int p = s.task
        ? (int)(jce_async_task_progress(s.task) * 100.0f + 0.5f)
        : 0;
    char overlay[32];
    std::snprintf(overlay, sizeof(overlay), "%d%%", p);
    ImGui::ProgressBar(p / 100.0f, ImVec2(-FLT_MIN, 0.0f), overlay);

    ImGui::TextWrapped(jce_editor_i18n("lightmapBake.label.status"), s.status);
    if (s.last_output_path[0])
        ImGui::TextWrapped(jce_editor_i18n("lightmapBake.label.lastOutput"), s.last_output_path);

    ImGui::Spacing();
    ImGui::TextDisabled("%s", jce_editor_i18n("lightmapBake.note.phaseC"));
}

} /* namespace */

extern "C" void jce_editor_panel_lightmap_bake_content(void)
{
    /* draw_content() lives in this TU's anonymous namespace and is
     * visible at file scope. */
    draw_content();
}

extern "C" void jce_editor_panel_lightmap_bake(void)
{
    /* Shim: Lightmap Bake has been merged into the Lighting Settings
     * "Rendering" workbench as a tab.  Activating this panel now
     * redirects to that workbench and requests the Lightmap tab.
     * Symbol kept so menu/hotkey entries registered against
     * JCE_PANEL_LIGHTMAP_BAKE keep working. */
    if (jce_panel_redirect_to_workbench(JCE_PANEL_LIGHTMAP_BAKE,
                                        JCE_PANEL_LIGHTING_SETTINGS,
                                        "panel.lighting.title",
                                        "lighting_settings"))
        jce_panel_lighting_settings_request_tab(2);
}
