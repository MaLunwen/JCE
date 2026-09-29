/*
 * jce_asset_cache_texture.cpp  Texture cache + async texture loading.
 */

#include "jce_asset_cache_internal.h"

#include <jce/os/core/jce_filesystem.h>   /* jce_fs_host_get_mtime */

/* ── Texture asset path detection ───────────────────────────────── */

bool looks_like_texture_asset_path(const char *path)
{
    /* Engine-canonical table (jce_asset_format.h) — the editor must agree
     * with the cooker and the runtime about what counts as a texture. */
    return path && path[0] != '\0' && jce_asset_ext_is_texture(path);
}

/* ── Texture cache entry management ─────────────────────────────── */

static uint32_t texture_cache_key_hash(const char *key)
{
    uint32_t h = 2166136261u;               /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

int find_texture_cache_entry(const char *key)
{
    if (!key || key[0] == '\0') return -1;

    const uint32_t h = texture_cache_key_hash(key);
    for (int i = 0; i < s_cache.tex_cache_count; i++) {
        if (s_cache.tex_cache[i].path_hash == h
            && strcmp(s_cache.tex_cache[i].path, key) == 0)
            return i;
    }
    return -1;
}

void reset_texture_cache_entry(TextureCacheEntry *entry)
{
    if (!entry)
        return;

    if (entry->path[0] != '\0') {
        if (s_cache.assets && asset_handle_valid(entry->asset_handle)) {
            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
        }

        if (jce_texture_valid(entry->tex) && !entry->tex_from_asset_manager)
            jce_texture_destroy(entry->tex);
    }

    entry->path[0] = '\0';
    entry->path_hash = 0;
    entry->tex = tex_invalid();
    entry->asset_handle = asset_handle_invalid();
    entry->tex_from_asset_manager = false;
    entry->warned_missing = false;
    entry->requested = false;
    entry->failed = false;
    entry->request_generation = 0;
}

void clear_texture_cache(void)
{
    for (int i = 0; i < s_cache.tex_cache_count; i++)
        reset_texture_cache_entry(&s_cache.tex_cache[i]);
    s_cache.tex_cache_count = 0;
}

/* ── Texture RGBA decoding ──────────────────────────────────────── */

/* Decode the base mip of a cooked .jceasset texture to RGBA8 via the engine
 * (block-compressed data is CPU-decoded there).  The runtime uploads cooked
 * block data straight to the GPU, but this editor cache is RGBA8-based, so
 * when previewing a scene loaded from a bundle/PAK we decode here.  Returns
 * false for non-cooked / malformed input so the caller falls back to raw. */
static bool decode_cooked_texture_rgba(const void *buf, size_t size,
                                       std::vector<uint8_t> *out_rgba,
                                       uint32_t *out_w, uint32_t *out_h)
{
    uint8_t *rgba = nullptr;
    uint32_t w = 0, h = 0;
    if (!jce_texture_decode_cooked_rgba8(buf, size, &rgba, &w, &h) || !rgba)
        return false;
    out_rgba->assign(rgba, rgba + (size_t)w * (size_t)h * 4u);
    jce_free(rgba);
    *out_w = w;
    *out_h = h;
    return true;
}

bool decode_texture_rgba_path(const char *path,
                              std::vector<uint8_t> *out_rgba,
                              uint32_t *out_w,
                              uint32_t *out_h)
{
    if (!out_rgba || !out_w || !out_h || !path_is_file(path))
        return false;

    out_rgba->clear();
    *out_w = 0;
    *out_h = 0;

    size_t img_size = 0;
    void *img_buf = ed_read_file(path, &img_size);
    if (!img_buf) return false;

    /* Cooked .jceasset (e.g. a bundle/PAK preview): the engine CPU-decodes
     * the block data.  Raw PNG/JPG returns false there and falls through to
     * jce_image_decode below. */
    if (decode_cooked_texture_rgba(img_buf, img_size, out_rgba, out_w, out_h)) {
        ED_FREE(img_buf);
        LOG_DEBUG(LOG_TAG, "decoded cooked texture: %s (%ux%u)",
                  path, *out_w, *out_h);
        return true;
    }

    JceImage img;
    bool ok = jce_image_decode(img_buf, img_size, &img);
    ED_FREE(img_buf);
    if (!ok) return false;
    if (img.width == 0 || img.height == 0 || !img.pixels) {
        jce_image_free(&img);
        return false;
    }

    *out_w = img.width;
    *out_h = img.height;

    const size_t total_bytes = (size_t)img.width * (size_t)img.height * 4u;
    out_rgba->resize(total_bytes);
    memcpy(out_rgba->data(), img.pixels, total_bytes);

    jce_image_free(&img);
    LOG_DEBUG(LOG_TAG, "decoded texture: %s (%ux%u)",
              path, *out_w, *out_h);
    return true;
}

/* ── Texture async worker ───────────────────────────────────────── */

/* Runs with the async mutex held.  LIFO — newest request first. */
static bool texture_pop_request_locked(TextureLoadRequest *out)
{
    if (!out || s_tex_async.pending.empty())
        return false;

    *out = std::move(s_tex_async.pending.back());
    s_tex_async.pending.pop_back();
    return true;
}

static void texture_async_worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        TextureLoadRequest req;
        if (!async_loader_worker_take(
                s_tex_async,
                [&req] { return texture_pop_request_locked(&req); })) {
            break;
        }

        TextureLoadResult result = {};
        result.key = req.key;
        result.generation = req.generation;
        if (req.resolve_path) {
            char resolved_path[512];
            const char *material_path = req.material_path.empty()
                                      ? NULL : req.material_path.c_str();
            const char *mesh_path = req.mesh_path.empty()
                                  ? NULL : req.mesh_path.c_str();

            if (resolve_texture_path_for_material(material_path,
                                                  mesh_path,
                                                  resolved_path, sizeof(resolved_path))) {
                result.resolved_path = resolved_path;
                if (!jce_fs_host_get_mtime(resolved_path, &result.resolved_mtime))
                    result.resolved_mtime = 0;
                result.success = decode_texture_rgba_path(resolved_path,
                                                          &result.rgba,
                                                          &result.width,
                                                          &result.height);
            } else {
                result.success = false;
            }
        } else {
            result.resolved_path = req.file_path;
            if (!jce_fs_host_get_mtime(req.file_path.c_str(), &result.resolved_mtime))
                result.resolved_mtime = 0;
            result.success = decode_texture_rgba_path(req.file_path.c_str(),
                                                      &result.rgba,
                                                      &result.width,
                                                      &result.height);
            if (!result.success) {
                LOG_WARN(LOG_TAG, "texture async load FAILED: key='%s' file='%s'",
                         req.key.c_str(), req.file_path.c_str());
            }
        }

        async_loader_publish(s_tex_async, result);
    }
}

/* ── Texture async lifecycle ────────────────────────────────────── */

void texture_async_start(void)
{
    if (s_tex_async.running)
        return;

    if (!async_loader_start(s_tex_async, texture_async_worker_main,
                            "scene_tex_async"))
        LOG_ERROR(LOG_TAG, "failed to start texture decode service");
}

void texture_async_stop(void)
{
    if (!s_tex_async.running)
        return;

    async_loader_stop(s_tex_async);
}

void texture_async_begin_new_generation(void)
{
    /* Decoded pixels live in the result itself, so completions can be
     * dropped right here instead of waiting for the drain to reject them. */
    async_loader_begin_new_generation(s_tex_async, true);
}

uint64_t texture_async_current_generation(void)
{
    return async_loader_generation(s_tex_async);
}

/* Both queue entry points differ only in how the request is built. */
static void texture_async_queue(const char *key,
                                const char *file_path,
                                const char *material_path,
                                const char *mesh_path,
                                bool resolve_path)
{
    async_loader_queue_request(
        s_tex_async,
        [&](TextureLoadRequest &req) { return req.key == key; },
        [&](uint64_t generation, uint32_t order) {
            (void)order;
            TextureLoadRequest req;
            req.key = key;
            req.file_path = file_path ? file_path : "";
            req.material_path = material_path ? material_path : "";
            req.mesh_path = mesh_path ? mesh_path : "";
            req.resolve_path = resolve_path;
            req.generation = generation;
            return req;
        });
}

void texture_async_queue_request(const char *key, const char *file_path)
{
    if (!s_tex_async.running || !key || key[0] == '\0' || !file_path || !file_path[0])
        return;

    texture_async_queue(key, file_path, NULL, NULL, false);
}

void texture_async_queue_resolve_request(const char *key,
                                         const char *material_path,
                                         const char *mesh_path)
{
    if (!s_tex_async.running || !key || key[0] == '\0')
        return;

    texture_async_queue(key, NULL, material_path, mesh_path, true);
}

/* ── Texture finalization ───────────────────────────────────────── */

static AsyncFinalizeAction texture_finalize_result(TextureLoadResult &res,
                                                   uint64_t generation,
                                                   bool budget_left)
{
    if (res.generation != generation)
        return ASYNC_FINALIZE_DROPPED;

    const int idx = find_texture_cache_entry(res.key.c_str());
    if (idx < 0)
        return ASYNC_FINALIZE_DROPPED;

    if (!budget_left)
        return ASYNC_FINALIZE_DEFERRED;

    s_cache.tex_cache[idx].requested = false;

    if (!res.success || res.rgba.empty() || res.width == 0 || res.height == 0) {
        s_cache.tex_cache[idx].failed = true;
        return ASYNC_FINALIZE_DROPPED;
    }

    /* WRAP + mip chain: these are scene MATERIAL textures.  Terrain multiplies
     * its UV by the layer tile scale, so a clamped sampler returns the edge
     * texel for everything past the first tile -- which renders as horizontal
     * streaks across the whole surface rather than as a tiled ground. */
    /* The colour space, read back off the key that requested it -- one
     * source, so the upload and the lookup cannot disagree. */
    const int tex_mode = JCE_TEX_WRAP
        | (asset_cache_key_is_srgb(res.key.c_str()) ? JCE_TEX_SRGB : 0);
    JceTexture tex = jce_texture_from_rgba_ex(res.rgba.data(),
                                              res.width, res.height,
                                              tex_mode);

    if (!jce_texture_valid(tex)) {
        s_cache.tex_cache[idx].failed = true;
        LOG_WARN(LOG_TAG, "texture finalize failed (upload): %s",
                 res.key.c_str());
        return ASYNC_FINALIZE_DROPPED;
    }

    if (jce_texture_valid(s_cache.tex_cache[idx].tex)
        && !s_cache.tex_cache[idx].tex_from_asset_manager) {
        jce_texture_destroy(s_cache.tex_cache[idx].tex);
    }

    if (s_cache.assets && asset_handle_valid(s_cache.tex_cache[idx].asset_handle)) {
        jce_asset_release(s_cache.assets, s_cache.tex_cache[idx].asset_handle);
        s_cache.tex_cache[idx].asset_handle = asset_handle_invalid();
    }

    s_cache.tex_cache[idx].tex = tex;
    s_cache.tex_cache[idx].tex_from_asset_manager = false;
    s_cache.tex_cache[idx].warned_missing = false;
    s_cache.tex_cache[idx].failed = false;
    snprintf(s_cache.tex_cache[idx].file, sizeof(s_cache.tex_cache[idx].file),
             "%s", res.resolved_path.c_str());
    s_cache.tex_cache[idx].file_mtime = res.resolved_mtime;
    return ASYNC_FINALIZE_APPLIED;
}

/* ── External edits ─────────────────────────────────────────────────
 *
 * A designer saves a texture from another program and expects the viewport to
 * show it.  Nothing in this cache noticed: entries are keyed by path and
 * nothing ever removed one, so the old image stayed on screen until the editor
 * was restarted.  (The navmesh beside it has reloaded on mtime for a while --
 * jce_scene_render_draw.cpp:1493 -- so the asymmetry was within one folder.)
 *
 * A ROUND-ROBIN SLICE, not a full scan.  Stat is a syscall; doing 256 of them
 * every frame to catch an edit that happens once a minute is the wrong trade.
 * TEX_POLL_PER_FRAME entries per frame walks the whole cache in about four
 * seconds at 60 Hz, which is faster than alt-tabbing back from an image
 * editor, and costs a handful of stats a frame.
 *
 * Only entries this cache DECODED FROM A FILE are polled: `file` is empty for
 * asset-manager (PAK) textures, whose bytes do not come from a loose file at
 * all, and for entries that never resolved.
 *
 * The entry is not reloaded here -- it is INVALIDATED, and the next
 * asset_cache_get_texture re-requests through the normal async path.  Loading
 * on the main thread to save a frame is how a hot reload becomes a hitch. */
enum { TEX_POLL_PER_FRAME = 4 };

void texture_poll_disk_changes(void)
{
    if (!s_cache.initialized || s_cache.tex_cache_count <= 0)
        return;

    static int s_cursor = 0;
    const int n = s_cache.tex_cache_count;
    for (int step = 0; step < TEX_POLL_PER_FRAME && step < n; ++step) {
        if (s_cursor >= n) s_cursor = 0;
        TextureCacheEntry *e = &s_cache.tex_cache[s_cursor++];
        if (e->file[0] == '\0' || e->requested)
            continue;

        int64_t now = 0;
        if (!jce_fs_host_get_mtime(e->file, &now))
            continue;              /* deleted or unreadable: keep what we have */
        if (now == e->file_mtime)
            continue;

        LOG_INFO(LOG_TAG, "texture changed on disk, reloading: %s", e->file);
        e->file_mtime = now;
        if (jce_texture_valid(e->tex) && !e->tex_from_asset_manager)
            jce_texture_destroy(e->tex);
        e->tex = tex_invalid();
        e->failed = false;         /* a fixed file must get another chance */
        e->warned_missing = false;
        if (s_cache.assets && asset_handle_valid(e->asset_handle)) {
            jce_asset_release(s_cache.assets, e->asset_handle);
            e->asset_handle = asset_handle_invalid();
        }
        e->tex_from_asset_manager = false;
    }
}

void texture_finalize_completed_loads(void)
{
    async_loader_drain_completed(s_tex_async, TEX_FINALIZE_BUDGET_PER_FRAME,
                                 texture_finalize_result);
}

/* ── Texture cache lookup ───────────────────────────────────────── */

JceTexture asset_cache_get_texture(const char *material_path,
                                   const char *mesh_path)
{
    return asset_cache_get_texture_cs(material_path, mesh_path, false);
}

JceTexture asset_cache_get_texture_cs(const char *material_path,
                                      const char *mesh_path,
                                      bool srgb)
{
    const char *raw = (material_path && material_path[0] != '\0')
                    ? material_path : mesh_path;
    if (!raw || raw[0] == '\0') return tex_invalid();

    /* The colour space is part of the IDENTITY: the same PNG can be one
     * material's albedo (sRGB-encoded colour, decoded by the sampler) and
     * another's mask (linear data), and they need two GPU textures.  Carried
     * in the key rather than beside it so the async queue, the finalize, the
     * mtime poll and the failure lookups all inherit it unchanged. */
    char keybuf[512];
    const char *key = raw;
    if (srgb) {
        snprintf(keybuf, sizeof keybuf, "%c%s", ASSET_CACHE_SRGB_KEY_PREFIX, raw);
        key = keybuf;
    }

    int idx = find_texture_cache_entry(key);
    if (idx < 0) {
        if (s_cache.tex_cache_count >= 256)
            return tex_invalid();

        idx = s_cache.tex_cache_count++;
        reset_texture_cache_entry(&s_cache.tex_cache[idx]);
        snprintf(s_cache.tex_cache[idx].path,
                 sizeof(s_cache.tex_cache[0].path), "%s", key);
        s_cache.tex_cache[idx].path_hash =
            texture_cache_key_hash(s_cache.tex_cache[idx].path);
    }

    TextureCacheEntry *entry = &s_cache.tex_cache[idx];

    if (entry->tex_from_asset_manager
        && s_cache.assets
        && asset_handle_valid(entry->asset_handle)) {
        JceAssetState state = jce_asset_state(s_cache.assets, entry->asset_handle);
        if (state == JCE_ASSET_STATE_READY) {
            JceTexture tex = jce_asset_get_texture(s_cache.assets,
                                                   entry->asset_handle);
            if (jce_texture_valid(tex)) {
                entry->tex = tex;
                entry->warned_missing = false;
                entry->failed = false;
                return tex;
            }

            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
            entry->tex = tex_invalid();
            entry->tex_from_asset_manager = false;
        } else if (state == JCE_ASSET_STATE_FAILED
                || state == JCE_ASSET_STATE_UNLOADED) {
            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
            entry->tex = tex_invalid();
            entry->tex_from_asset_manager = false;
            /* Mark as failed so we don't re-acquire on every frame and
             * spam "not found in PAK" log messages.  Without this, the
             * code below sees an invalid handle and re-issues the load
             * each frame, producing torrents of identical errors. */
            if (state == JCE_ASSET_STATE_FAILED)
                entry->failed = true;
        } else {
            entry->tex = tex_invalid();
            return tex_invalid();
        }
    }

    if (jce_texture_valid(entry->tex)) {
        if (s_cache.assets
            && !entry->tex_from_asset_manager
            && asset_handle_valid(entry->asset_handle)) {
            jce_asset_release(s_cache.assets, entry->asset_handle);
            entry->asset_handle = asset_handle_invalid();
        }
        return entry->tex;
    }
    if (entry->failed)
        return tex_invalid();

    /* The editor resolves scene textures from the project filesystem via
     * resolve_texture_path_for_material (scene-root joins, basename search,
     * material JSON, MTL parsing).  A sync PAK acquire is never correct
     * here: editor scene textures are raw project files, not in the game
     * PAK.  Attempting one would only produce spurious "not found in PAK"
     * warnings before the async resolver finds the real file anyway.
     *
     * Direct filesystem paths (absolute or CWD-relative) are routed to
     * texture_async_queue_request; everything else (asset-style relative
     * paths like "textures/chalet.jpg") goes to the richer resolver. */
    bool is_filesystem_texture = false;
    if (material_path && material_path[0] != '\0'
        && looks_like_texture_asset_path(material_path)) {
        if (path_is_file(material_path))
            is_filesystem_texture = true;
    }

    const uint64_t generation = texture_async_current_generation();
    if (entry->requested
        && entry->request_generation == generation) {
        return tex_invalid();
    }

    entry->requested = true;
    entry->request_generation = generation;

    if (is_filesystem_texture) {
        texture_async_queue_request(key, material_path);
    } else {
        texture_async_queue_resolve_request(key, material_path, mesh_path);
    }
    return tex_invalid();
}
