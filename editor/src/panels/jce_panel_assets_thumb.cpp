/*
 * jce_panel_assets_thumb.cpp  LRU thumbnail cache for the Asset Browser.
 *
 * Decode pipeline (all main-thread, amortised across frames):
 *
 *   request(path)  -> map.insert(PENDING) + queue.push(path)
 *   pump(budget)   -> for at most `budget` pending entries:
 *                       read file, decode RGBA8, downscale, upload to GPU,
 *                       mark READY (or FAILED).
 *
 * Two asset kinds are eligible:
 *   - raw images  (.png .jpg .jpeg .tga .bmp .hdr) -> decode directly
 *   - .mat.json   -> resolve albedo path, decode that
 *
 * Capacity: 256 entries / ~64 MB GPU.  On overflow we evict from the
 * back of an LRU list and destroy the matching texture.
 */

#include "jce_panel_assets_thumb.h"

#include <deque>
#include <list>
#include <string>
#include <unordered_map>

#include "core/jce_editor_alloc.h"
#include "io/jce_editor_file_util.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_str.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_image_decode.h>
}

#define LOG_TAG "thumb"

namespace {

constexpr size_t kMaxEntries     = 256;
constexpr size_t kMaxBytes       = 64u * 1024u * 1024u;
constexpr size_t kMaxSourceBytes = 32u * 1024u * 1024u;
constexpr uint32_t kThumbMax     = 128;

struct Entry {
    JceThumb thumb;
    size_t   bytes;
    std::list<std::string>::iterator lru_it;
};

struct State {
    std::unordered_map<std::string, Entry> map;
    std::list<std::string>                 lru;     /* front = most-recent */
    std::deque<std::string>                pending;
    size_t                                 total_bytes;
};

State *g_state = nullptr;

State &state()
{
    if (!g_state) g_state = new State{};
    return *g_state;
}

bool ext_eq(const char *path, const char *suffix)
{
    if (!path || !suffix) return false;
    const size_t pn = strlen(path);
    const size_t sn = strlen(suffix);
    if (pn < sn) return false;
    return jce_strcasecmp(path + (pn - sn), suffix) == 0;
}

bool is_image_path(const char *path)
{
    return ext_eq(path, ".png") || ext_eq(path, ".jpg")  ||
           ext_eq(path, ".jpeg") || ext_eq(path, ".tga") ||
           ext_eq(path, ".bmp") || ext_eq(path, ".hdr");
}

bool is_material_path(const char *path)
{
    return ext_eq(path, ".mat.json");
}

bool is_matgraph_path(const char *path)
{
    return ext_eq(path, ".matgraph.json");
}

/* Average a w_in x h_in RGBA8 image down to w_out x h_out RGBA8. */
void downscale_rgba(const uint8_t *src, uint32_t w_in, uint32_t h_in,
                    uint8_t *dst, uint32_t w_out, uint32_t h_out)
{
    for (uint32_t y = 0; y < h_out; y++) {
        const uint32_t sy0 = (y * h_in) / h_out;
        const uint32_t sy1 = ((y + 1) * h_in) / h_out;
        const uint32_t ny  = (sy1 > sy0) ? (sy1 - sy0) : 1;
        for (uint32_t x = 0; x < w_out; x++) {
            const uint32_t sx0 = (x * w_in) / w_out;
            const uint32_t sx1 = ((x + 1) * w_in) / w_out;
            const uint32_t nx  = (sx1 > sx0) ? (sx1 - sx0) : 1;

            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t sy = sy0; sy < sy0 + ny; sy++) {
                const uint8_t *row = src + (size_t)sy * w_in * 4;
                for (uint32_t sx = sx0; sx < sx0 + nx; sx++) {
                    const uint8_t *p = row + sx * 4;
                    r += p[0]; g += p[1]; b += p[2]; a += p[3];
                }
            }
            const uint32_t n = nx * ny;
            uint8_t *d = dst + ((size_t)y * w_out + x) * 4;
            d[0] = (uint8_t)(r / n);
            d[1] = (uint8_t)(g / n);
            d[2] = (uint8_t)(b / n);
            d[3] = (uint8_t)(a / n);
        }
    }
}

/* Touch an entry: move its path to the front of the LRU list. */
void touch(State &s, const std::string &path)
{
    auto it = s.map.find(path);
    if (it == s.map.end()) return;
    if (it->second.lru_it != s.lru.begin()) {
        s.lru.erase(it->second.lru_it);
        s.lru.push_front(path);
        it->second.lru_it = s.lru.begin();
    }
}

void evict_one(State &s)
{
    if (s.lru.empty()) return;
    const std::string &victim = s.lru.back();
    auto it = s.map.find(victim);
    if (it != s.map.end()) {
        if (jce_texture_valid(it->second.thumb.handle))
            jce_texture_destroy(it->second.thumb.handle);
        s.total_bytes -= it->second.bytes;
        s.map.erase(it);
    }
    s.lru.pop_back();
}

void enforce_caps(State &s)
{
    while (s.map.size() > kMaxEntries || s.total_bytes > kMaxBytes) {
        if (s.lru.empty()) break;
        evict_one(s);
    }
}

/* Resolve the albedo asset path referenced by a .mat.json file.
 * Returns true and fills `out` (host-absolute) on success.  Texture
 * paths in .mat.json are stored relative to the mat file's directory. */
bool resolve_mat_albedo(const char *mat_path, char *out, size_t out_size)
{
    JcePbrMaterial mat;
    char tex_paths[5][256];
    if (!jce_pbr_material_load_json(mat_path, &mat, tex_paths))
        return false;

    const char *albedo = tex_paths[0];
    if (!albedo || !albedo[0]) return false;

    if (jce_path_is_absolute(albedo)) {
        snprintf(out, out_size, "%s", albedo);
        return true;
    }

    char parent[1024];
    if (!jce_path_parent(parent, sizeof(parent), mat_path)) return false;
    return jce_path_join(out, out_size, parent, albedo);
}

/* Decode the on-disk image at `host_path` to a downscaled RGBA8 buffer.
 * Returns malloc'd buffer (caller frees via free()), or NULL on failure. */
uint8_t *decode_and_scale(const char *host_path,
                          uint32_t *out_w, uint32_t *out_h)
{
    size_t src_size = 0;
    void  *src = ed_read_file_capped(host_path, kMaxSourceBytes,
                                     &src_size, NULL);
    if (!src) return NULL;

    JceImage img;
    const bool ok = jce_image_decode(src, src_size, &img);
    ED_FREE(src);
    if (!ok) return NULL;

    uint32_t tw = img.width, th = img.height;
    if (tw > kThumbMax || th > kThumbMax) {
        if (tw >= th) { th = (uint32_t)((uint64_t)th * kThumbMax / tw); tw = kThumbMax; }
        else          { tw = (uint32_t)((uint64_t)tw * kThumbMax / th); th = kThumbMax; }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }

    uint8_t *dst = (uint8_t *)ED_MALLOC((size_t)tw * th * 4);
    if (!dst) { jce_image_free(&img); return NULL; }

    if (tw == img.width && th == img.height) {
        memcpy(dst, img.pixels, (size_t)tw * th * 4);
    } else {
        downscale_rgba(img.pixels, img.width, img.height, dst, tw, th);
    }
    jce_image_free(&img);

    *out_w = tw;
    *out_h = th;
    return dst;
}

/* Resolve the sibling `.mat.json` for a `.matgraph.json` source.  We
 * intentionally do NOT decode the graph itself — the graph editor owns
 * shader pipeline state, and the asset browser only needs a quick visual
 * hint, which the linked mat's albedo provides.  Returns true on success;
 * on miss the caller falls through to FAILED. */
bool resolve_matgraph_to_mat(const char *graph_path, char *out, size_t out_size)
{
    const size_t pn = strlen(graph_path);
    const size_t ext_len = strlen(".matgraph.json");
    if (pn <= ext_len) return false;
    const size_t base_len = pn - ext_len;
    if (base_len + strlen(".mat.json") + 1 > out_size) return false;
    memcpy(out, graph_path, base_len);
    memcpy(out + base_len, ".mat.json", strlen(".mat.json") + 1);
    return jce_fs_host_exists_file(out);
}

void decode_one(const std::string &path)
{
    State &s = state();
    auto it = s.map.find(path);
    if (it == s.map.end() || it->second.thumb.state != JCE_THUMB_PENDING)
        return;

    char resolved[1024];
    const char *target = path.c_str();
    if (is_matgraph_path(target)) {
        char sibling[1024];
        if (!resolve_matgraph_to_mat(target, sibling, sizeof(sibling))) {
            it->second.thumb.state = JCE_THUMB_FAILED;
            return;
        }
        if (!resolve_mat_albedo(sibling, resolved, sizeof(resolved))) {
            it->second.thumb.state = JCE_THUMB_FAILED;
            return;
        }
        target = resolved;
    } else if (is_material_path(target)) {
        if (!resolve_mat_albedo(target, resolved, sizeof(resolved))) {
            it->second.thumb.state = JCE_THUMB_FAILED;
            return;
        }
        target = resolved;
    }

    uint32_t w = 0, h = 0;
    uint8_t *rgba = decode_and_scale(target, &w, &h);
    if (!rgba) {
        it->second.thumb.state = JCE_THUMB_FAILED;
        return;
    }

    JceTexture tex = jce_texture_from_rgba(rgba, w, h);
    ED_FREE(rgba);
    if (!jce_texture_valid(tex)) {
        it->second.thumb.state = JCE_THUMB_FAILED;
        return;
    }

    it->second.thumb.handle = tex;
    it->second.thumb.w      = (int)w;
    it->second.thumb.h      = (int)h;
    it->second.thumb.state  = JCE_THUMB_READY;
    it->second.bytes        = (size_t)w * h * 4;
    s.total_bytes += it->second.bytes;
    enforce_caps(s);
}

} /* namespace */

/* ── Public API ────────────────────────────────────────────────────── */

bool jce_thumb_is_eligible(const char *path)
{
    return path && (is_image_path(path) || is_material_path(path) ||
                    is_matgraph_path(path));
}

bool jce_thumb_request(const char *abs_path, JceThumb *out)
{
    if (!abs_path || !abs_path[0] || !out) return false;
    if (!jce_thumb_is_eligible(abs_path)) return false;

    State &s = state();
    const std::string key(abs_path);

    auto it = s.map.find(key);
    if (it != s.map.end()) {
        touch(s, key);
        *out = it->second.thumb;
        return true;
    }

    Entry e{};
    e.thumb.handle.idx = UINT16_MAX;
    e.thumb.state      = JCE_THUMB_PENDING;
    s.lru.push_front(key);
    e.lru_it = s.lru.begin();
    s.map.emplace(key, e);
    s.pending.push_back(key);

    *out = e.thumb;
    return true;
}

void jce_thumb_pump(int budget)
{
    if (budget <= 0) return;
    State &s = state();
    int decoded = 0;
    while (decoded < budget && !s.pending.empty()) {
        std::string path = std::move(s.pending.front());
        s.pending.pop_front();
        if (s.map.find(path) == s.map.end()) continue; /* evicted before pump */
        decode_one(path);
        decoded++;
    }
}

void jce_thumb_shutdown(void)
{
    if (!g_state) return;
    for (auto &kv : g_state->map) {
        if (jce_texture_valid(kv.second.thumb.handle))
            jce_texture_destroy(kv.second.thumb.handle);
    }
    delete g_state;
    g_state = nullptr;
}
