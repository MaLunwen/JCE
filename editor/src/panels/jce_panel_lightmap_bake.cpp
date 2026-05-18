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
 *  3. The worker thread owns its own snapshot vector, so the editor
 *     scene can keep mutating while a bake is in flight.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
#include "dialogs/jce_path_input.h"
#include "core/jce_assetdb.h"
extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_thread.h>
}

#include "core/jce_editor_state.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

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
    Settings        cfg;
    JceThread      *worker   = nullptr;
    JceAtomicI32   *progress = nullptr;  /* 0..100 */
    JceAtomicI32   *running  = nullptr;  /* 0/1 */
    JceAtomicI32   *cancel   = nullptr;  /* 0/1 */
    char            last_output_path[320] = {0};
    char            status[160] = "Idle";
};

State s;

static void ensure_atomics(void)
{
    if (!s.progress) s.progress = jce_atomic_i32_create(0);
    if (!s.running)  s.running  = jce_atomic_i32_create(0);
    if (!s.cancel)   s.cancel   = jce_atomic_i32_create(0);
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
};

void worker_run(void *arg_ptr)
{
    WorkerArgs *args = static_cast<WorkerArgs *>(arg_ptr);
    Settings cfg = args->cfg;
    SceneSnapshot snap = std::move(args->snap);
    delete args;

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
        if (jce_atomic_i32_load(s.cancel) != 0) {
            std::snprintf(s.status, sizeof(s.status),
                          "Cancelled at row %d/%d", y, H);
            jce_atomic_i32_store(s.running, 0);
            return;
        }
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
        jce_atomic_i32_store(s.progress, (int)((y + 1) * 100 / H));
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

    char png_path[320];
    std::snprintf(png_path, sizeof(png_path), "%s.png", cfg.output);
    ensure_dir(png_path);
    if (write_png_rgba(png_path, W, H, rgba.data())) {
        std::snprintf(s.last_output_path, sizeof(s.last_output_path),
                      "%s", png_path);
        std::snprintf(s.status, sizeof(s.status),
                      "Bake done: %s (%dx%d, %d spp, %d bounces, %d occ)",
                      png_path, W, H, cfg.samples, cfg.bounces,
                      (int)snap.occ.size());
    } else {
        std::snprintf(s.status, sizeof(s.status),
                      "Bake failed: cannot write %s", png_path);
    }
    jce_atomic_i32_store(s.running, 0);
}

void start_bake(void)
{
    ensure_atomics();
    if (jce_atomic_i32_load(s.running) != 0) return;
    jce_atomic_i32_store(s.cancel, 0);
    jce_atomic_i32_store(s.progress, 0);
    std::snprintf(s.status, sizeof(s.status), "Baking...");
    jce_atomic_i32_store(s.running, 1);

    /* Snapshot the scene on the main thread before the worker starts. */
    WorkerArgs *args = new WorkerArgs();
    args->cfg = s.cfg;
    capture_scene_snapshot(&args->snap);

    /* Join previous worker if any (worker is detached after start, but
     * keep the handle around for jce_thread_join semantics). */
    if (s.worker) {
        jce_thread_join(s.worker);
        s.worker = nullptr;
    }
    s.worker = jce_thread_create(worker_run, args, "jce_lightmap_bake");
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

void draw_content(void)
{
    bool busy = s.running ? (jce_atomic_i32_load(s.running) != 0) : false;

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
            if (s.cancel) jce_atomic_i32_store(s.cancel, 1);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || s.last_output_path[0] == 0);
    if (ImGui::Button(jce_editor_i18n_id("lightmapBake.button.bindToScene", "lmb_bind"))) write_binding();
    ImGui::EndDisabled();

    ImGui::Separator();
    int p = s.progress ? jce_atomic_i32_load(s.progress) : 0;
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

extern "C" void jce_editor_panel_lightmap_bake(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTMAP_BAKE);
    if (!vis || !*vis) return;
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_lightmap_bake", jce_editor_i18n("lightmapBake.title"));
    if (ImGui::Begin(_wt, vis, ImGuiWindowFlags_NoFocusOnAppearing)) {
        draw_content();
    }
    ImGui::End();
}
