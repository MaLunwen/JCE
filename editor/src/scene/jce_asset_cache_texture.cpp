/*
 * jce_asset_cache_texture.cpp  Texture cache + async texture loading.
 */

#include "jce_asset_cache_internal.h"

/* ── Texture asset path detection ───────────────────────────────── */

bool looks_like_texture_asset_path(const char *path)
{
    if (!path || path[0] == '\0')
        return false;

    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
        || ext == ".bmp" || ext == ".tga" || ext == ".dds"
        || ext == ".ktx" || ext == ".ktx2";
}

/* ── Texture cache entry management ─────────────────────────────── */

int find_texture_cache_entry(const char *key)
{
    if (!key || key[0] == '\0') return -1;

    for (int i = 0; i < s_cache.tex_cache_count; i++) {
        if (strcmp(s_cache.tex_cache[i].path, key) == 0)
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

bool decode_texture_rgba_path(const fs::path &path,
                              std::vector<uint8_t> *out_rgba,
                              uint32_t *out_w,
                              uint32_t *out_h)
{
    if (!out_rgba || !out_w || !out_h || !path_is_file(path))
        return false;

    out_rgba->clear();
    *out_w = 0;
    *out_h = 0;

    SDL_IOStream *io = SDL_IOFromFile(path.string().c_str(), "rb");
    if (!io) return false;

    SDL_Surface *surf = IMG_Load_IO(io, true);
    if (!surf) return false;

    if (surf->format != SDL_PIXELFORMAT_RGBA32) {
        SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(surf);
        surf = conv;
    }
    if (!surf || surf->w <= 0 || surf->h <= 0 || !surf->pixels) {
        if (surf) SDL_DestroySurface(surf);
        return false;
    }

    *out_w = (uint32_t)surf->w;
    *out_h = (uint32_t)surf->h;

    const size_t row_bytes = (size_t)(*out_w) * 4;
    const size_t total_bytes = row_bytes * (size_t)(*out_h);
    out_rgba->resize(total_bytes);

    const uint8_t *src = (const uint8_t *)surf->pixels;
    uint8_t *dst = out_rgba->data();
    for (uint32_t y = 0; y < *out_h; y++) {
        memcpy(dst + (size_t)y * row_bytes,
               src + (size_t)y * (size_t)surf->pitch,
               row_bytes);
    }

    SDL_DestroySurface(surf);
    LOG_DEBUG(LOG_TAG, "decoded texture: %s (%ux%u)",
              path.string().c_str(), *out_w, *out_h);
    return true;
}

/* ── Texture async worker ───────────────────────────────────────── */

static void texture_async_worker_main(void)
{
    for (;;) {
        TextureLoadRequest req;
        {
            std::unique_lock<std::mutex> lock(s_tex_async.mutex);
            s_tex_async.cv.wait(lock, [] {
                return s_tex_async.stop || !s_tex_async.pending.empty();
            });

            if (s_tex_async.stop && s_tex_async.pending.empty())
                break;

            if (s_tex_async.pending.empty())
                continue;

            req = std::move(s_tex_async.pending.back());
            s_tex_async.pending.pop_back();
        }

        TextureLoadResult result = {};
        result.key = req.key;
        result.generation = req.generation;
        result.success = decode_texture_rgba_path(req.file_path,
                                                  &result.rgba,
                                                  &result.width,
                                                  &result.height);

        std::lock_guard<std::mutex> lock(s_tex_async.mutex);
        s_tex_async.completed.push_back(std::move(result));
    }
}

/* ── Texture async lifecycle ────────────────────────────────────── */

void texture_async_start(void)
{
    if (s_tex_async.running)
        return;

    s_tex_async.generation = 1;
    s_tex_async.stop = false;
    s_tex_async.pending.clear();
    s_tex_async.completed.clear();

    s_tex_async.worker = std::thread(texture_async_worker_main);
    s_tex_async.running = true;
}

void texture_async_stop(void)
{
    if (!s_tex_async.running)
        return;

    {
        std::lock_guard<std::mutex> lock(s_tex_async.mutex);
        s_tex_async.stop = true;
    }
    s_tex_async.cv.notify_all();

    if (s_tex_async.worker.joinable())
        s_tex_async.worker.join();

    s_tex_async.pending.clear();
    s_tex_async.completed.clear();
    s_tex_async.running = false;
}

void texture_async_begin_new_generation(void)
{
    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    s_tex_async.generation++;
    s_tex_async.pending.clear();
    s_tex_async.completed.clear();
}

uint64_t texture_async_current_generation(void)
{
    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    return s_tex_async.generation;
}

void texture_async_queue_request(const char *key, const fs::path &file_path)
{
    if (!s_tex_async.running || !key || key[0] == '\0' || file_path.empty())
        return;

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(s_tex_async.mutex);
        for (TextureLoadRequest &req : s_tex_async.pending) {
            if (req.generation == s_tex_async.generation && req.key == key) {
                inserted = true;
                break;
            }
        }

        if (!inserted) {
            TextureLoadRequest req;
            req.key = key;
            req.file_path = file_path.string();
            req.generation = s_tex_async.generation;
            s_tex_async.pending.push_back(std::move(req));
            inserted = true;
        }
    }

    if (inserted)
        s_tex_async.cv.notify_one();
}

static void texture_async_take_completed(std::vector<TextureLoadResult> *out)
{
    if (!out) return;

    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    out->swap(s_tex_async.completed);
}

static void texture_async_push_back_completed(std::vector<TextureLoadResult> *results)
{
    if (!results || results->empty()) return;

    std::lock_guard<std::mutex> lock(s_tex_async.mutex);
    for (TextureLoadResult &res : *results)
        s_tex_async.completed.push_back(std::move(res));
    results->clear();
}

/* ── Texture finalization ───────────────────────────────────────── */

void texture_finalize_completed_loads(void)
{
    std::vector<TextureLoadResult> completed;
    texture_async_take_completed(&completed);
    if (completed.empty())
        return;

    const uint64_t generation = texture_async_current_generation();
    uint32_t finalized = 0;
    std::vector<TextureLoadResult> deferred;
    deferred.reserve(completed.size());

    for (TextureLoadResult &res : completed) {
        if (res.generation != generation)
            continue;

        const int idx = find_texture_cache_entry(res.key.c_str());
        if (idx < 0)
            continue;

        if (finalized >= TEX_FINALIZE_BUDGET_PER_FRAME) {
            deferred.push_back(std::move(res));
            continue;
        }

        s_cache.tex_cache[idx].requested = false;

        if (!res.success || res.rgba.empty() || res.width == 0 || res.height == 0) {
            s_cache.tex_cache[idx].failed = true;
            continue;
        }

        SDL_Surface *surf = SDL_CreateSurface((int)res.width,
                                              (int)res.height,
                                              SDL_PIXELFORMAT_RGBA32);
        if (!surf || !surf->pixels) {
            if (surf) SDL_DestroySurface(surf);
            s_cache.tex_cache[idx].failed = true;
            LOG_WARN(LOG_TAG, "texture finalize failed (surface): %s",
                     res.key.c_str());
            continue;
        }

        const size_t row_bytes = (size_t)res.width * 4;
        const uint8_t *src = res.rgba.data();
        uint8_t *dst = (uint8_t *)surf->pixels;
        for (uint32_t y = 0; y < res.height; y++) {
            memcpy(dst + (size_t)y * (size_t)surf->pitch,
                   src + (size_t)y * row_bytes,
                   row_bytes);
        }

        JceTexture tex = jce_texture_load_from_surface(surf, JCE_TEX_WRAP);
        SDL_DestroySurface(surf);

        if (!jce_texture_valid(tex)) {
            s_cache.tex_cache[idx].failed = true;
            LOG_WARN(LOG_TAG, "texture finalize failed (upload): %s",
                     res.key.c_str());
            continue;
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
        finalized++;
    }

    if (!deferred.empty())
        texture_async_push_back_completed(&deferred);
}

/* ── Texture cache lookup ───────────────────────────────────────── */

JceTexture asset_cache_get_texture(const char *material_path,
                                   const char *mesh_path)
{
    const char *key = (material_path && material_path[0] != '\0')
                    ? material_path : mesh_path;
    if (!key || key[0] == '\0') return tex_invalid();

    int idx = find_texture_cache_entry(key);
    if (idx < 0) {
        if (s_cache.tex_cache_count >= 256)
            return tex_invalid();

        idx = s_cache.tex_cache_count++;
        reset_texture_cache_entry(&s_cache.tex_cache[idx]);
        snprintf(s_cache.tex_cache[idx].path,
                 sizeof(s_cache.tex_cache[0].path), "%s", key);
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

    /* If the path points directly to an existing file on disk, bypass the
     * PAK asset manager entirely and go straight to the async file-based
     * texture loader.  The editor always works with real filesystem paths
     * (never packed assets), so this avoids noisy "not found in PAK"
     * errors for every drag-dropped model texture. */
    bool is_filesystem_texture = false;
    if (material_path && material_path[0] != '\0'
        && looks_like_texture_asset_path(material_path)) {
        fs::path direct(material_path);
        if (path_is_file(direct))
            is_filesystem_texture = true;
    }

    if (s_cache.assets && material_path && looks_like_texture_asset_path(material_path)
        && !is_filesystem_texture) {
        if (!asset_handle_valid(entry->asset_handle)) {
            JceAssetLoadParams params = asset_load_params_default();
            params.texture_sampler_mode = JCE_TEX_WRAP;
            params.sync = true;
            entry->asset_handle =
                jce_asset_acquire(s_cache.assets,
                                  material_path,
                                  JCE_ASSET_TEXTURE,
                                  &params);
        }

        if (asset_handle_valid(entry->asset_handle)) {
            JceAssetState state = jce_asset_state(s_cache.assets,
                                                  entry->asset_handle);
            if (state == JCE_ASSET_STATE_READY) {
                JceTexture tex = jce_asset_get_texture(s_cache.assets,
                                                       entry->asset_handle);
                if (jce_texture_valid(tex)) {
                    entry->tex = tex;
                    entry->tex_from_asset_manager = true;
                    entry->warned_missing = false;
                    entry->failed = false;
                    return tex;
                }
            } else if (state == JCE_ASSET_STATE_FAILED
                    || state == JCE_ASSET_STATE_UNLOADED) {
                jce_asset_release(s_cache.assets, entry->asset_handle);
                entry->asset_handle = asset_handle_invalid();
            } else {
                return tex_invalid();
            }
        }
    }

    const uint64_t generation = texture_async_current_generation();
    if (entry->requested
        && entry->request_generation == generation) {
        return tex_invalid();
    }

    fs::path resolved_path;
    if (!resolve_texture_path_for_material(material_path, mesh_path, &resolved_path)) {
        entry->failed = true;
        LOG_WARN(LOG_TAG,
                 "tex cache: MISS (no texture found) key='%s' mat='%s' mesh='%s'",
                 key, material_path ? material_path : "<null>",
                 mesh_path ? mesh_path : "<null>");
        return tex_invalid();
    }

    entry->requested = true;
    entry->request_generation = generation;

    texture_async_queue_request(key, resolved_path);
    return tex_invalid();
}
