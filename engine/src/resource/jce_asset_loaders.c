/*
 * jce_asset_loaders.c  Type-dispatch sync loaders, async finalization,
 *                      and per-type payload destruction.
 *
 * Split from jce_asset_manager.c to keep the manager file focused on
 * lifecycle, public API, and the frame pump.
 */

#include "jce_asset_loaders.h"

#include <jce/middleware/audio/jce_audio.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_asset_format.h>

#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <string.h>

#define LOG_TAG "jce_asset"

/* ================================================================== */
/* Per-type payload destruction                                        */
/* ================================================================== */

void destroy_slot_payload(JceAssetManager *mgr, JceAssetSlot *slot)
{
    if (!slot || !slot->data) return;

    /* Try registered external destroyer first. */
    if ((uint32_t)slot->type < JCE_ASSET_TYPE_COUNT &&
        mgr->ext_destroyers[slot->type]) {
        mgr->ext_destroyers[slot->type](slot->data);
    } else {
        switch (slot->type) {
        case JCE_ASSET_TEXTURE: {
            JceTexture *tex = (JceTexture *)slot->data;
            jce_texture_destroy(*tex);
            JCE_FREE(tex);
            break;
        }
        case JCE_ASSET_MESH:
            jce_mesh_destroy((JceMesh *)slot->data);
            break;
        case JCE_ASSET_SOUND:
            /* Sound data is owned by audio subsystem. */
            JCE_FREE(slot->data);
            break;
        case JCE_ASSET_MODEL:
            jce_model_destroy((JceModel *)slot->data);
            break;
        case JCE_ASSET_FONT:
            jce_font_close((JceFont *)slot->data);
            break;
        case JCE_ASSET_SHADER:
        case JCE_ASSET_MATERIAL:
        case JCE_ASSET_ANIMATION:
        case JCE_ASSET_SCENE:
        case JCE_ASSET_RAW:
            JCE_FREE(slot->data);
            break;
        default:
            break;
        }
    }

    slot->data = NULL;
    slot->memory_bytes = 0;
}

/* ================================================================== */
/* Synchronous loading (type dispatch)                                 */
/* ================================================================== */

static void load_texture_sync(JceAssetManager *mgr, JceAssetSlot *slot,
                              const char *path, const JceAssetLoadParams *p)
{
    int sampler = (p) ? p->texture_sampler_mode : 0;
    JceTexture tex = jce_texture_load_ex(mgr->pak, path, sampler);
    if (!jce_texture_valid(tex)) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        /* LOG_WARN instead of ERROR — PAK may still be loading. True failures logged by jce_texture_load_ex. */
        LOG_WARN(LOG_TAG, "texture load failed (may retry): %s", path);
        return;
    }

    JceTexture *heap = JCE_MALLOC(sizeof(JceTexture));
    if (!heap) {
        jce_texture_destroy(tex);
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }
    *heap = tex;

    uint32_t w = 0, h = 0;
    jce_texture_get_size(tex, &w, &h);

    slot->data = heap;
    slot->memory_bytes = (size_t)w * h * 4;
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
}

static void load_mesh_sync(JceAssetManager *mgr, JceAssetSlot *slot,
                           const char *path)
{
    JceMesh *mesh = jce_mesh_load(mgr->pak, path);
    if (!mesh) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        LOG_ERROR(LOG_TAG, "mesh load failed: %s", path);
        return;
    }

    slot->data = mesh;
    slot->memory_bytes = (size_t)jce_mesh_vertex_count(mesh) * 32 +
                         (size_t)jce_mesh_index_count(mesh) * 4;
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
}

static void load_sound_sync(JceAssetManager *mgr, JceAssetSlot *slot,
                            const char *path)
{
    if (!mgr->audio) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        LOG_WARN(LOG_TAG, "no audio subsystem for: %s", path);
        return;
    }

    JceSound snd = jce_audio_load(mgr->audio, mgr->pak, path);
    if (snd == JCE_SOUND_INVALID) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        LOG_ERROR(LOG_TAG, "sound load failed: %s", path);
        return;
    }

    JceSound *heap = JCE_MALLOC(sizeof(JceSound));
    if (!heap) { JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED); return; }
    *heap = snd;

    slot->data = heap;
    slot->memory_bytes = 0; /* audio system owns PCM memory */
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
}

static void load_raw_sync(JceAssetManager *mgr, JceAssetSlot *slot,
                          const char *path)
{
    const JcePakAsset *asset = jce_pak_find(mgr->pak, path);
    if (!asset) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        LOG_ERROR(LOG_TAG, "raw asset not found: %s", path);
        return;
    }

    void *buf = JCE_MALLOC(asset->original_size);
    if (!buf) { JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED); return; }

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        JCE_FREE(buf);
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }

    slot->data = buf;
    slot->memory_bytes = n;
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
}

static void load_model_sync(JceAssetManager *mgr, JceAssetSlot *slot,
                            const char *path)
{
    JceModel *model = jce_model_load_gltf(mgr->pak, path);
    if (!model) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        LOG_ERROR(LOG_TAG, "model load failed: %s", path);
        return;
    }

    slot->data = model;
    slot->memory_bytes = 4096; /* opaque; rough estimate */
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
}

static void load_font_sync(JceAssetManager *mgr, JceAssetSlot *slot,
                            const char *path, const JceAssetLoadParams *p)
{
    float pt_size = (p && p->font_size > 0.0f) ? p->font_size : 24.0f;
    JceFont *font = jce_font_open(mgr->pak, path, pt_size);
    if (!font) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        LOG_ERROR(LOG_TAG, "font load failed: %s", path);
        return;
    }

    slot->data = font;
    slot->memory_bytes = 256 * 1024; /* glyph atlas rough estimate */
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
}

void load_slot_sync(JceAssetManager *mgr,
                    JceAssetSlot *slot,
                    const char *asset_path,
                    JceAssetType type,
                    const JceAssetLoadParams *params)
{
    if (!mgr || !slot || !asset_path) return;

    slot->type = type;
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_LOADING);
    slot->data = NULL;
    slot->memory_bytes = 0;

    /* Try registered external loader first. */
    if ((uint32_t)type < JCE_ASSET_TYPE_COUNT &&
        mgr->ext_loaders[type]) {
        const JcePakAsset *pa = jce_pak_find(mgr->pak, asset_path);
        if (pa) {
            void *decompressed = JCE_MALLOC((size_t)pa->original_size);
            size_t dec_size = decompressed
                ? jce_pak_decompress(pa, decompressed, (size_t)pa->original_size) : 0;
            if (dec_size > 0 && mgr->ext_loaders[type](
                    decompressed, dec_size, params,
                    &slot->data, &slot->memory_bytes)) {
                JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
            } else {
                JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
            }
            JCE_FREE(decompressed);
        } else {
            LOG_ERROR(LOG_TAG, "ext loader: asset not found in PAK: %s",
                      asset_path);
            JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        }
        return;
    }

    /* Built-in dispatch. */
    switch (type) {
    case JCE_ASSET_TEXTURE:   load_texture_sync(mgr, slot, asset_path, params); break;
    case JCE_ASSET_MESH:      load_mesh_sync(mgr, slot, asset_path);            break;
    case JCE_ASSET_MODEL:     load_model_sync(mgr, slot, asset_path);           break;
    case JCE_ASSET_SOUND:     load_sound_sync(mgr, slot, asset_path);           break;
    case JCE_ASSET_FONT:      load_font_sync(mgr, slot, asset_path, params);    break;
    case JCE_ASSET_SHADER:    /* no standalone shader files; load as raw */
    case JCE_ASSET_MATERIAL:  /* material defs embedded in glTF; load as raw */
    case JCE_ASSET_ANIMATION: /* clip data embedded in glTF; load as raw */
    case JCE_ASSET_SCENE:     /* scene JSON, loaded as raw blob */
    case JCE_ASSET_RAW:       load_raw_sync(mgr, slot, asset_path);             break;
    default:
        load_raw_sync(mgr, slot, asset_path);
        break;
    }
}

/* ================================================================== */
/* Async finalization (main thread)                                    */
/* ================================================================== */

static void finalize_texture_inner(JceAssetManager *mgr, JceAssetSlot *slot,
                                   JceAsyncRequest *req)
{
    if (!req->decoded_data) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }

    JceTexture tex;

    if (req->is_cooked) {
        /* Cooked path: decoded_data = [JceAssetTexInfo | RGBA8 pixels] */
        JceAssetTexInfo *info = (JceAssetTexInfo *)req->decoded_data;
        const void *pixels = (const uint8_t *)req->decoded_data + sizeof(JceAssetTexInfo);

        tex = jce_texture_from_rgba(pixels, info->width, info->height);
        JCE_FREE(req->decoded_data);
        req->decoded_data = NULL;
    } else {
        /* Raw path: decoded_data = SDL_Surface* */
        SDL_Surface *surf = (SDL_Surface *)req->decoded_data;
        tex = jce_texture_load_from_surface(
            surf, req->info.texture_sampler_mode);
        SDL_DestroySurface(surf);
        req->decoded_data = NULL;
    }

    if (!jce_texture_valid(tex)) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }

    JceTexture *heap = JCE_MALLOC(sizeof(JceTexture));
    if (!heap) {
        jce_texture_destroy(tex);
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }
    *heap = tex;

    uint32_t w = 0, h = 0;
    jce_texture_get_size(tex, &w, &h);

    slot->data = heap;
    slot->memory_bytes = (size_t)w * h * 4;
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
    mgr->total_loaded++;
    mgr->total_memory += slot->memory_bytes;
}

void finalize_texture(JceAssetManager *mgr, JceAssetSlot *slot,
                      JceAsyncRequest *req)
{
    JCE_PROFILE_ZONE_N("Asset::FinalizeTexture");
    finalize_texture_inner(mgr, slot, req);
    JCE_PROFILE_ZONE_END;
}

static void finalize_audio_inner(JceAssetManager *mgr, JceAssetSlot *slot,
                                 JceAsyncRequest *req)
{
    if (!mgr->audio || !req->decoded_data) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }

    /* Decoded audio result layout: header + PCM data. */
    typedef struct {
        uint32_t sample_rate;
        uint16_t channels;
        uint16_t bits_per_sample;
        uint32_t pcm_size;
    } AudioResult;

    AudioResult *ar = (AudioResult *)req->decoded_data;
    const void *pcm = (const uint8_t *)ar + sizeof(AudioResult);

    JceSound snd = jce_audio_load_pcm(mgr->audio, pcm, ar->pcm_size,
                                        ar->channels, ar->sample_rate,
                                        ar->bits_per_sample);

    JCE_FREE(req->decoded_data);
    req->decoded_data = NULL;

    if (snd == JCE_SOUND_INVALID) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }

    JceSound *heap = JCE_MALLOC(sizeof(JceSound));
    if (!heap) { JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED); return; }
    *heap = snd;

    slot->data = heap;
    slot->memory_bytes = 0;
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
    mgr->total_loaded++;
}

void finalize_audio(JceAssetManager *mgr, JceAssetSlot *slot,
                    JceAsyncRequest *req)
{
    JCE_PROFILE_ZONE_N("Asset::FinalizeAudio");
    finalize_audio_inner(mgr, slot, req);
    JCE_PROFILE_ZONE_END;
}

void finalize_raw(JceAssetManager *mgr, JceAssetSlot *slot,
                  JceAsyncRequest *req)
{
    if (!req->decoded_data) {
        JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_FAILED);
        return;
    }

    slot->data = req->decoded_data;
    slot->memory_bytes = req->decoded_size;
    req->decoded_data = NULL; /* ownership transferred */
    JCE_SLOT_STATE_SET(slot, JCE_ASSET_STATE_READY);
    mgr->total_loaded++;
    mgr->total_memory += slot->memory_bytes;
}
