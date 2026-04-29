/*
 * jce_asset_cache_texture.cpp  Texture cache + async texture loading.
 */

#include "jce_asset_cache_internal.h"

/* ── Texture asset path detection ───────────────────────────────── */

bool looks_like_texture_asset_path(const char *path)
{
    if (!path || path[0] == '\0')
        return false;

    char ext_buf[32];
    if (!jce_path_extension(ext_buf, sizeof(ext_buf), path))
        return false;
        
    std::string ext(ext_buf);
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

static void texture_async_worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        TextureLoadRequest req;
        {
            jce_mutex_lock(s_tex_async.mutex);
            while (!(s_tex_async.stop || !s_tex_async.pending.empty()))
                jce_cond_wait(s_tex_async.cv, s_tex_async.mutex);

            if (s_tex_async.stop && s_tex_async.pending.empty()) {
                jce_mutex_unlock(s_tex_async.mutex);
                break;
            }

            if (s_tex_async.pending.empty()) {
                jce_mutex_unlock(s_tex_async.mutex);
                continue;
            }

            req = std::move(s_tex_async.pending.back());
            s_tex_async.pending.pop_back();
            jce_mutex_unlock(s_tex_async.mutex);
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
                result.success = decode_texture_rgba_path(resolved_path,
                                                          &result.rgba,
                                                          &result.width,
                                                          &result.height);
            } else {
                result.success = false;
            }
        } else {
            result.success = decode_texture_rgba_path(req.file_path.c_str(),
                                                      &result.rgba,
                                                      &result.width,
                                                      &result.height);
        }

        JceMutexGuard lock(s_tex_async.mutex);
        s_tex_async.completed.push_back(std::move(result));
    }
}

/* ── Texture async lifecycle ────────────────────────────────────── */

void texture_async_start(void)
{
    if (s_tex_async.running)
        return;

    if (!s_tex_async.mutex) s_tex_async.mutex = jce_mutex_create();
    if (!s_tex_async.cv)    s_tex_async.cv    = jce_cond_create();

    s_tex_async.generation = 1;
    s_tex_async.stop = false;
    s_tex_async.pending.clear();
    s_tex_async.completed.clear();

    s_tex_async.worker = jce_thread_create(texture_async_worker_main, NULL,
                                           "scene_tex_async");
    s_tex_async.running = true;
}

void texture_async_stop(void)
{
    if (!s_tex_async.running)
        return;

    {
        JceMutexGuard lock(s_tex_async.mutex);
        s_tex_async.stop = true;
    }
    jce_cond_broadcast(s_tex_async.cv);

    if (s_tex_async.worker) {
        jce_thread_join(s_tex_async.worker);
        s_tex_async.worker = NULL;
    }

    s_tex_async.pending.clear();
    s_tex_async.completed.clear();
    s_tex_async.running = false;

    if (s_tex_async.cv)    { jce_cond_destroy(s_tex_async.cv);    s_tex_async.cv = NULL; }
    if (s_tex_async.mutex) { jce_mutex_destroy(s_tex_async.mutex); s_tex_async.mutex = NULL; }
}

void texture_async_begin_new_generation(void)
{
    JceMutexGuard lock(s_tex_async.mutex);
    s_tex_async.generation++;
    s_tex_async.pending.clear();
    s_tex_async.completed.clear();
}

uint64_t texture_async_current_generation(void)
{
    JceMutexGuard lock(s_tex_async.mutex);
    return s_tex_async.generation;
}

void texture_async_queue_request(const char *key, const char *file_path)
{
    if (!s_tex_async.running || !key || key[0] == '\0' || !file_path || !file_path[0])
        return;

    bool inserted = false;
    {
        JceMutexGuard lock(s_tex_async.mutex);
        for (TextureLoadRequest &req : s_tex_async.pending) {
            if (req.generation == s_tex_async.generation && req.key == key) {
                inserted = true;
                break;
            }
        }

        if (!inserted) {
            TextureLoadRequest req;
            req.key = key;
            req.file_path = file_path;
            req.material_path.clear();
            req.mesh_path.clear();
            req.resolve_path = false;
            req.generation = s_tex_async.generation;
            s_tex_async.pending.push_back(std::move(req));
            inserted = true;
        }
    }

    if (inserted)
        jce_cond_signal(s_tex_async.cv);
}

void texture_async_queue_resolve_request(const char *key,
                                         const char *material_path,
                                         const char *mesh_path)
{
    if (!s_tex_async.running || !key || key[0] == '\0')
        return;

    bool inserted = false;
    {
        JceMutexGuard lock(s_tex_async.mutex);
        for (TextureLoadRequest &req : s_tex_async.pending) {
            if (req.generation == s_tex_async.generation && req.key == key) {
                inserted = true;
                break;
            }
        }

        if (!inserted) {
            TextureLoadRequest req;
            req.key = key;
            req.file_path.clear();
            req.material_path = material_path ? material_path : "";
            req.mesh_path = mesh_path ? mesh_path : "";
            req.resolve_path = true;
            req.generation = s_tex_async.generation;
            s_tex_async.pending.push_back(std::move(req));
            inserted = true;
        }
    }

    if (inserted)
        jce_cond_signal(s_tex_async.cv);
}

static void texture_async_take_completed(std::vector<TextureLoadResult> *out)
{
    if (!out) return;

    JceMutexGuard lock(s_tex_async.mutex);
    out->swap(s_tex_async.completed);
}

static void texture_async_push_back_completed(std::vector<TextureLoadResult> *results)
{
    if (!results || results->empty()) return;

    JceMutexGuard lock(s_tex_async.mutex);
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

        JceTexture tex = jce_texture_from_rgba(res.rgba.data(),
                                               res.width, res.height);

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
        if (path_is_file(material_path))
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

    entry->requested = true;
    entry->request_generation = generation;

    if (is_filesystem_texture) {
        texture_async_queue_request(key, material_path);
    } else {
        texture_async_queue_resolve_request(key, material_path, mesh_path);
    }
    return tex_invalid();
}
