/*
 * jce_asset_cooker.c  Asset cooking implementation.
 *
 * Converts raw assets into .jceasset binary containers.
 * Each cook function: decode input → build chunks → compress → serialize.
 *
 * Dependencies: jce_image via jce_image_decode (texture decode — the cooker
 *               never picks a codec), miniaudio (audio decode),
 *               shared asset writer (compression), XXHash (source hash).
 */

#include "jce_asset_cooker.h"
#include "jce_asset_writer_internal.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_image_decode.h>  /* the one image-decode service */

#include "jce_cook_policy.h"
#include <cjson/cJSON.h>   /* the .import.json colorSpace sidecar */
#include "jce_tex_compress.h"
#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xxhash.h>

#ifndef JCE_NO_AUDIO
/* Audio decode source of truth.
 *
 * Compiled INTO the engine (jce_resource, which links jce_audio — the
 * JCE_BUILDING_ENGINE marker every _jce_add_layer target sets), cook through
 * the audio module's canonical decoder so the cooked PCM is exactly what the
 * runtime would have produced, including the Opus custom backend and the
 * M4A/AAC route a bare ma_decoder knows nothing about (audit
 * A2-AUDIO-DECODE-DRIFT).
 *
 * The standalone jce_cook host tool compiles this same TU from an explicit
 * source list that links neither jce_audio nor Opus/Ogg/fdk-aac, so there it
 * falls back to the lean miniaudio-only decode below: .opus/.m4a sources must
 * be cooked in-process (editor "Build Bundles") until jce_cook links the audio
 * layer. */
#ifdef JCE_BUILDING_ENGINE
#define JCE_COOK_AUDIO_VIA_AUDIO_LAYER 1
#include <jce/middleware/audio/jce_audio.h>
#else
#include <miniaudio.h>
#endif
#endif

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

/* Build a .jceasset blob from pre-built chunks.
   chunks: array of (chunk_type, raw_data, raw_size) tuples.
   Returns malloc'd blob. */

typedef struct {
    uint16_t    chunk_type;
    const void *raw_data;
    size_t      raw_size;
} ChunkInput;

static JceCookResult build_asset(uint32_t asset_type,
                                 uint64_t source_hash,
                                 const ChunkInput *chunks,
                                 uint32_t chunk_count,
                                 const JceCookOptions *opts)
{
    JceCookResult result = {0};
    JceAssetWriteChunk *writer_chunks = JCE_CALLOC(chunk_count, sizeof(*writer_chunks));
    if (!writer_chunks) {
        snprintf(result.error, sizeof(result.error), "allocation failed");
        return result;
    }
    for (uint32_t i = 0; i < chunk_count; ++i) {
        writer_chunks[i].chunk_type = chunks[i].chunk_type;
        writer_chunks[i].raw_data = chunks[i].raw_data;
        writer_chunks[i].raw_size = chunks[i].raw_size;
    }
    JceAssetWriteResult written = jce_asset_writer_build(
        asset_type, source_hash, writer_chunks, chunk_count,
        opts ? opts->compression_level : 3);
    JCE_FREE(writer_chunks);
    result.data = written.data;
    result.size = written.size;
    result.success = written.success;
    memcpy(result.error, written.error, sizeof(result.error));
    return result;
}

/* ================================================================== */
/* Cook: Texture (with optional mipmaps)                               */
/* ================================================================== */

JceCookResult jce_cook_texture(const void *input, size_t input_size,
                               const JceCookOptions *opts)
{
    JceCookResult result = {0};
    SDL_Surface *surf = NULL;

    /* Decode through the ONE image service (jce_image, reached here via the
     * JceImage adapter).  Two reasons this call site must not pick a codec:
     *
     *   1. Cooked pixels must be exactly what the runtime would have decoded
     *      from the same source, and the runtime decodes through this service.
     *   2. A 16-bit GRAYSCALE PNG (IHDR bitdepth=16, colortype=0) heap-overruns
     *      the bundled SDL3_image/libpng path — STATUS_HEAP_CORRUPTION, found
     *      cooking a DCC-exported height map.  This file used to carry its own
     *      copy of the header sniff that routes that class to stb_image; the
     *      sniff now lives in the service, so there is one implementation of
     *      it instead of two that can silently drift apart.
     *
     * The service always yields tightly-packed RGBA8, so the surface below is
     * RGBA8 by construction and needs no convert step.  It exists only because
     * the rest of this function drives SDL blits (the max-dimension downscale). */
    JceImage img;
    if (!jce_image_decode(input, input_size, &img) || !img.pixels) {
        snprintf(result.error, sizeof(result.error), "image decode failed");
        return result;
    }
    surf = SDL_CreateSurface((int)img.width, (int)img.height,
                             SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        jce_image_free(&img);
        snprintf(result.error, sizeof(result.error),
                 "SDL_CreateSurface failed");
        return result;
    }
    for (uint32_t y = 0; y < img.height; y++)
        memcpy((uint8_t *)surf->pixels + (size_t)y * surf->pitch,
               img.pixels + (size_t)y * img.width * 4u,
               (size_t)img.width * 4u);
    jce_image_free(&img);

    /* Downscale if exceeding max texture dimension cap. */
    int max_dim = opts ? opts->max_texture_size : 0;
    if (max_dim > 0 && (surf->w > max_dim || surf->h > max_dim)) {
        float scale = (float)max_dim / (float)(surf->w > surf->h ? surf->w : surf->h);
        int nw = (int)(surf->w * scale);
        int nh = (int)(surf->h * scale);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        SDL_Surface *scaled = SDL_CreateSurface(nw, nh, SDL_PIXELFORMAT_RGBA32);
        if (scaled) {
            SDL_BlitSurfaceScaled(surf, NULL, scaled, NULL, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(surf);
            surf = scaled;
        }
    }

    uint32_t base_w = (uint32_t)surf->w;
    uint32_t base_h = (uint32_t)surf->h;

    /* Target GPU format. Block-compress (BC7/BC5/.../ASTC/ETC2) only when the
       caller explicitly requests it via opts->texture_format; 0 == RGBA8 keeps
       the default cook behavior unchanged. The RGBA8 mip pyramid is generated
       first either way, then re-encoded below if a block format was requested. */
    int target_format = opts ? opts->texture_format : JCEASSET_TEXFMT_RGBA8;
    if (target_format != JCEASSET_TEXFMT_RGBA8 &&
        !jce_tex_format_is_block(target_format)) {
        target_format = JCEASSET_TEXFMT_RGBA8;
    }

    /* Determine mip count. */
    uint32_t mip_count = 1;
    if (opts && opts->generate_mipmaps) {
        mip_count = jce_tex_mip_count(base_w, base_h);
    }

    /* Calculate total output size for all mip levels (RGBA8 = 4 bpp). */
    size_t total_mip_size = 0;
    for (uint32_t m = 0; m < mip_count; m++) {
        uint32_t mw, mh;
        jce_tex_mip_dimensions(base_w, base_h, m, &mw, &mh);
        total_mip_size += (size_t)mw * mh * 4;
    }

    /* Allocate output buffer for all mips. */
    uint8_t *mip_data = (uint8_t *)JCE_MALLOC(total_mip_size);
    if (!mip_data) {
        SDL_DestroySurface(surf);
        snprintf(result.error, sizeof(result.error), "allocation failed");
        return result;
    }

    /* Allocate mip offset array (stored after TEX_INFO struct). */
    uint32_t *mip_offsets = (uint32_t *)JCE_MALLOC(mip_count * sizeof(uint32_t));
    if (!mip_offsets) {
        JCE_FREE(mip_data);
        SDL_DestroySurface(surf);
        snprintf(result.error, sizeof(result.error), "allocation failed");
        return result;
    }

    /* Generate each mip level into the contiguous mip_data pyramid.
     *
     * Mip generation reads the PREVIOUS level as its source while writing
     * the next, so a single realloc'd scratch buffer is unsafe: a realloc
     * that relocates frees the old block while `current_mip` still points
     * at it, and jce_tex_generate_mip then reads freed memory.  In release
     * builds the shrinking realloc usually stays in place, so the bug only
     * fired intermittently (and the crash surfaced INSIDE generate_mip,
     * masking the real cause here).  Ping-pong between two fixed buffers
     * sized to the largest sub-base level (mip 1); every later level is
     * smaller, so capacity never grows and `current_mip` always refers to
     * the buffer NOT being written this step. */
    uint8_t *current_mip = (uint8_t *)surf->pixels;   /* level 0 = source */
    uint8_t *scratch[2] = { NULL, NULL };
    int scratch_idx = 0;
    if (mip_count > 1) {
        uint32_t m1w, m1h;
        jce_tex_mip_dimensions(base_w, base_h, 1, &m1w, &m1h);
        size_t scratch_cap = (size_t)m1w * m1h * 4;
        scratch[0] = (uint8_t *)JCE_MALLOC(scratch_cap);
        scratch[1] = (uint8_t *)JCE_MALLOC(scratch_cap);
        if (!scratch[0] || !scratch[1]) {
            JCE_FREE(scratch[0]);
            JCE_FREE(scratch[1]);
            JCE_FREE(mip_offsets);
            JCE_FREE(mip_data);
            SDL_DestroySurface(surf);
            snprintf(result.error, sizeof(result.error), "mip allocation failed");
            return result;
        }
    }
    /* Resolved by the path-aware caller (jce_cook_file / the bundle packer);
     * false when nobody knew, which is the historical behaviour.
     *
     * BC5 vetoes it unconditionally.  BC5 is the two-channel normal-map format
     * and this file already back-infers `is_normal` from it further down, so a
     * name or an authored sidecar claiming sRGB next to a normal map cannot
     * win: the format is a harder signal than either, and this is the one
     * combination that would corrupt data rather than merely leave it as it
     * was. */
    const bool tex_is_srgb = opts && opts->texture_srgb &&
                             target_format != JCEASSET_TEXFMT_BC5;

    uint32_t current_w = base_w, current_h = base_h;
    size_t mip_offset = 0;

    for (uint32_t m = 0; m < mip_count; m++) {
        mip_offsets[m] = (uint32_t)mip_offset;

        /* Copy RGBA8 pixel data for this mip level. */
        size_t mip_size = (size_t)current_w * current_h * 4;
        memcpy(mip_data + mip_offset, current_mip, mip_size);
        mip_offset += mip_size;

        /* Generate next mip level if needed. */
        if (m + 1 < mip_count) {
            uint32_t next_w, next_h;
            uint8_t *dst = scratch[scratch_idx];
            /* Average in the space the texels are ENCODED in.
             *
             * A box filter over raw sRGB bytes is exactly right on flat
             * regions and increasingly wrong as local contrast rises, so the
             * defect hid in plain sight for as long as the test content was
             * flat: measured over this repository's textures, worst per-texel
             * error 73 levels, median texture's worst 27, and on a leaf atlas
             * a SIGNED mean shift of -14.5 luma / +16 R-B by mip 7.  Distant
             * foliage was being rendered darker and warmer than it should be
             * by an amount that grows with mip level -- with distance.
             *
             * The renderer decodes sRGB manually in the shader (pow 2.2 in
             * fs_pbr_body.sh / fs_terrain.sc), never via a sampler flag, so
             * the stored mips must themselves be sRGB-encoded: decode,
             * average, re-encode is exactly right for this pipeline. */
            jce_tex_generate_mip_ex(current_mip, current_w, current_h,
                                    dst, &next_w, &next_h, tex_is_srgb);
            current_mip = dst;
            scratch_idx ^= 1;   /* next level writes the other buffer */
            current_w = next_w;
            current_h = next_h;
        }
    }

    JCE_FREE(scratch[0]);
    JCE_FREE(scratch[1]);

    /* Default output = the RGBA8 mip pyramid just generated. */
    uint8_t  *final_data    = mip_data;
    uint32_t *final_offsets = mip_offsets;
    size_t    final_total   = mip_offset;
    uint8_t  *enc_data      = NULL;   /* block-compressed buffer, if used */
    uint32_t *enc_offsets   = NULL;

    /* GPU block compression: re-encode each RGBA8 mip into the target block
       format (BC7/BC5/.../ASTC/ETC2). On any failure fall back to RGBA8. */
    if (jce_tex_format_is_block(target_format)) {
        enc_offsets = (uint32_t *)JCE_MALLOC(mip_count * sizeof(uint32_t));
        size_t enc_total = 0;
        for (uint32_t m = 0; m < mip_count; m++) {
            uint32_t mw, mh;
            jce_tex_mip_dimensions(base_w, base_h, m, &mw, &mh);
            enc_total += jce_tex_encoded_size(mw, mh, target_format);
        }
        enc_data = (uint8_t *)JCE_MALLOC(enc_total > 0 ? enc_total : 1);

        bool enc_ok = (enc_offsets && enc_data && enc_total > 0);
        size_t eo = 0;
        for (uint32_t m = 0; enc_ok && m < mip_count; m++) {
            uint32_t mw, mh;
            jce_tex_mip_dimensions(base_w, base_h, m, &mw, &mh);
            uint32_t es = jce_tex_encoded_size(mw, mh, target_format);
            enc_offsets[m] = (uint32_t)eo;
            /* BC5 IS the normal-map format; pick the matching quality family
             * so range-fit is applied per-channel (degrades normals far less
             * than range-fit chroma). */
            int is_normal = (target_format == JCEASSET_TEXFMT_BC5);
            int quality   = opts ? opts->encode_quality : 0;
            if (!jce_tex_encode(mip_data + mip_offsets[m], mw, mh,
                                target_format, is_normal, quality,
                                enc_data + eo, es)) {
                enc_ok = false;
            }
            eo += es;
        }

        if (enc_ok) {
            final_data    = enc_data;
            final_offsets = enc_offsets;
            final_total   = eo;
        } else {
            if (opts && opts->verbose)
                printf("[cook] block encode failed; storing RGBA8\n");
            JCE_FREE(enc_data);    enc_data = NULL;
            JCE_FREE(enc_offsets); enc_offsets = NULL;
            target_format = JCEASSET_TEXFMT_RGBA8;
        }
    }

    /* Build info chunk (extended with mip offsets). */
    size_t info_size = sizeof(JceAssetTexInfo) + mip_count * sizeof(uint32_t);
    uint8_t *info_buf = (uint8_t *)JCE_MALLOC(info_size);
    if (!info_buf) {
        JCE_FREE(enc_data);
        JCE_FREE(enc_offsets);
        JCE_FREE(mip_offsets);
        JCE_FREE(mip_data);
        SDL_DestroySurface(surf);
        snprintf(result.error, sizeof(result.error), "allocation failed");
        return result;
    }

    JceAssetTexInfo *info = (JceAssetTexInfo *)info_buf;
    info->width     = base_w;
    info->height    = base_h;
    info->format    = (uint32_t)target_format;
    info->mip_count = mip_count;
    /* Bit 0 = sRGB.  It used to be hard-coded to 1 for EVERY cooked texture,
     * normal and roughness maps included, with an audit note saying that was
     * only harmless because the bit has no readers.  It now carries the same
     * value the mip filter above was given, so the bit and the pixels agree.
     *
     * That is an improvement, not a resolution.  The value is still derived
     * from a FILENAME heuristic, not from the texture's semantic: the
     * importers know (JceModelMaterialInfo has five named slots,
     * JcePbrMaterial five typed handles) and JceCookOptions still has no field
     * to carry it.  114 textures in this repository are GLB-embedded and named
     * `<stem>_tex<N>.png`, which no naming scheme can classify.
     *
     * So: still do not build anything on this bit that would be WRONG when the
     * heuristic is.  The next person to want a consumer should plumb the
     * semantic first; the hook is JceCookOptions::texture_srgb, and widening
     * it to a real usage enum is the intended path. */
    info->flags     = tex_is_srgb ? 1u : 0u;
    info->_pad      = 0;

    /* Copy mip offsets after the info struct. */
    memcpy(info_buf + sizeof(JceAssetTexInfo), final_offsets,
           mip_count * sizeof(uint32_t));

    uint64_t source_hash = XXH3_64bits(input, input_size);

    ChunkInput chunks[2];
    chunks[0].chunk_type = JCEASSET_CHUNK_TEX_INFO;
    chunks[0].raw_data   = info_buf;
    chunks[0].raw_size   = info_size;
    chunks[1].chunk_type = JCEASSET_CHUNK_TEX_PIXELS;
    chunks[1].raw_data   = final_data;
    chunks[1].raw_size   = final_total;

    result = build_asset(JCEASSET_TYPE_TEXTURE, source_hash, chunks, 2, opts);

    JCE_FREE(info_buf);
    JCE_FREE(enc_data);
    JCE_FREE(enc_offsets);
    JCE_FREE(mip_offsets);
    JCE_FREE(mip_data);
    SDL_DestroySurface(surf);
    return result;
}

/* ================================================================== */
/* Cook: Audio                                                         */
/* ================================================================== */

JceCookResult jce_cook_audio(const void *input, size_t input_size,
                             const JceCookOptions *opts)
{
    JceCookResult result = {0};

#ifdef JCE_NO_AUDIO
    snprintf(result.error, sizeof(result.error), "audio disabled");
    return result;
#else
    /* Decoded PCM plus its format.  The buffer is borrowed here because the
     * two backends below own it differently — see the release step at the end. */
    const void *pcm         = NULL;
    size_t      pcm_size    = 0;
    uint32_t    sample_rate = 0;
    uint16_t    channels    = 0;
    uint16_t    bits        = 16;

#ifdef JCE_COOK_AUDIO_VIA_AUDIO_LAYER
    /* Canonical decode — same code the runtime loads clips with. */
    JceAudioCpu *cpu = jce_audio_decode_cpu_memory(input, input_size, NULL);
    if (!cpu) {
        snprintf(result.error, sizeof(result.error), "audio decode failed");
        return result;
    }
    uint32_t cpu_pcm_bytes = 0;
    if (!jce_audio_cpu_get_pcm(cpu, &pcm, &cpu_pcm_bytes, &channels,
                               &sample_rate, &bits)) {
        jce_audio_cpu_free(cpu);
        snprintf(result.error, sizeof(result.error),
                 "audio decoded to no samples");
        return result;
    }
    pcm_size = (size_t)cpu_pcm_bytes;
#else
    /* Decode via miniaudio. */
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoder decoder;

    if (ma_decoder_init_memory(input, input_size, &cfg, &decoder) != MA_SUCCESS) {
        snprintf(result.error, sizeof(result.error), "miniaudio decode failed");
        return result;
    }

    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    channels    = (uint16_t)decoder.outputChannels;
    sample_rate = decoder.outputSampleRate;
    void *raw_pcm = NULL;

    if (total_frames == 0) {
        /* Unknown length — decode in chunks. */
        size_t alloc = 256 * 1024;
        size_t used  = 0;
        raw_pcm = JCE_MALLOC(alloc * channels * sizeof(int16_t));
        if (!raw_pcm) {
            ma_decoder_uninit(&decoder);
            snprintf(result.error, sizeof(result.error), "allocation failed");
            return result;
        }
        for (;;) {
            if (used + 4096 > alloc) {
                alloc *= 2;
                void *tmp = JCE_REALLOC(raw_pcm,
                    alloc * channels * sizeof(int16_t));
                if (!tmp) {
                    JCE_FREE(raw_pcm);
                    ma_decoder_uninit(&decoder);
                    snprintf(result.error, sizeof(result.error), "realloc failed");
                    return result;
                }
                raw_pcm = tmp;
            }
            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(&decoder,
                (int16_t *)raw_pcm + used * channels, 4096, &read);
            if (read == 0) break;
            used += (size_t)read;
        }
        total_frames = (ma_uint64)used;
    } else {
        raw_pcm = JCE_MALLOC((size_t)(total_frames * channels * sizeof(int16_t)));
        if (!raw_pcm) {
            ma_decoder_uninit(&decoder);
            snprintf(result.error, sizeof(result.error), "allocation failed");
            return result;
        }
        ma_uint64 read = 0;
        ma_decoder_read_pcm_frames(&decoder, raw_pcm, total_frames, &read);
        total_frames = read;
    }

    ma_decoder_uninit(&decoder);

    pcm      = raw_pcm;
    pcm_size = (size_t)(total_frames * channels * sizeof(int16_t));
#endif

    /* Derive the frame count from the byte count so both backends agree even
     * on a short read.  `bits` is 16 for every encoded source; only a cooked
     * pass-through can carry 8. */
    uint32_t bytes_per_frame = (uint32_t)channels * (uint32_t)(bits / 8u);
    if (bytes_per_frame == 0) {
#ifdef JCE_COOK_AUDIO_VIA_AUDIO_LAYER
        jce_audio_cpu_free(cpu);
#else
        JCE_FREE(raw_pcm);
#endif
        snprintf(result.error, sizeof(result.error), "invalid audio format");
        return result;
    }

    /* Build info chunk. */
    JceAssetAudioInfo info = {0};
    info.sample_rate     = sample_rate;
    info.channels        = channels;
    info.bits_per_sample = bits;
    info.total_frames    = (uint64_t)(pcm_size / bytes_per_frame);
    info.format          = 0; /* PCM_S16 */

    uint64_t source_hash = XXH3_64bits(input, input_size);

    ChunkInput chunks[2];
    chunks[0].chunk_type = JCEASSET_CHUNK_AUDIO_INFO;
    chunks[0].raw_data   = &info;
    chunks[0].raw_size   = sizeof(info);
    chunks[1].chunk_type = JCEASSET_CHUNK_AUDIO_PCM;
    chunks[1].raw_data   = pcm;
    chunks[1].raw_size   = pcm_size;

    result = build_asset(JCEASSET_TYPE_SOUND, source_hash, chunks, 2, opts);

    /* Whoever allocated frees: the JceAudioCpu owns its buffer, the lean
     * miniaudio path JCE_MALLOC'd one directly. */
#ifdef JCE_COOK_AUDIO_VIA_AUDIO_LAYER
    jce_audio_cpu_free(cpu);
#else
    JCE_FREE(raw_pcm);
#endif
    return result;
#endif
}

/* ================================================================== */
/* Cook: Raw                                                           */
/* ================================================================== */

JceCookResult jce_cook_raw(const void *input, size_t input_size,
                           const JceCookOptions *opts)
{
    uint64_t source_hash = XXH3_64bits(input, input_size);

    ChunkInput chunks[1];
    chunks[0].chunk_type = JCEASSET_CHUNK_RAW;
    chunks[0].raw_data   = input;
    chunks[0].raw_size   = input_size;

    return build_asset(JCEASSET_TYPE_RAW, source_hash, chunks, 1, opts);
}

/* ================================================================== */
/* Cook: file dispatch                                                 */
/* ================================================================== */

/*
 * Extensions this cooker actually has an encoder for.  The canonical table
 * (jce_asset_type_from_ext) answers "what kind of file is this?" across the
 * whole engine; it deliberately recognises more formats than any single build
 * step can process.  This predicate is the cooker's own capability check, kept
 * explicit so widening the shared table never silently changes what gets
 * cooked: an unsupported source still falls through to RAW exactly as before.
 */
static bool cook_can_encode(const char *ext, int type)
{
    switch (type) {
    case JCEASSET_TYPE_TEXTURE:
        return SDL_strcasecmp(ext, "png") == 0 ||
               SDL_strcasecmp(ext, "jpg") == 0 ||
               SDL_strcasecmp(ext, "jpeg") == 0 ||
               SDL_strcasecmp(ext, "bmp") == 0 ||
               SDL_strcasecmp(ext, "tga") == 0;
    case JCEASSET_TYPE_MODEL:
        return SDL_strcasecmp(ext, "obj") == 0 ||
               SDL_strcasecmp(ext, "fbx") == 0 ||
               SDL_strcasecmp(ext, "gltf") == 0 ||
               SDL_strcasecmp(ext, "glb") == 0;
    case JCEASSET_TYPE_SHADER:
        /* '.sh'/'.sb' are recognised engine-wide but were never cooked. */
        return SDL_strcasecmp(ext, "sc") == 0 ||
               SDL_strcasecmp(ext, "bin") == 0;
    case JCEASSET_TYPE_SOUND:
    case JCEASSET_TYPE_FONT:
        return true;   /* every canonical audio/font extension is handled */
    default:
        return false;
    }
}

int jce_cook_detect_type(const char *path)
{
    if (!path) return -1;
    const char *dot = strrchr(path, '.');
    if (!dot) return JCEASSET_TYPE_RAW;

    int type = jce_asset_type_from_ext(path);
    return cook_can_encode(dot + 1, type) ? type : JCEASSET_TYPE_RAW;
}

/* Texture-format policy (normal-map heuristic + per-platform auto format)
 * lives in jce_cook_policy.h — shared with jce_bundle_pack.c so the two
 * cook paths can never drift apart. */

JceCookResult jce_cook_file(const char *input_path,
                            const JceCookOptions *opts)
{
    JceCookResult result = {0};
    if (!input_path) {
        snprintf(result.error, sizeof(result.error), "null input path");
        return result;
    }

    /* Read file. */
    uint64_t nread = 0;
    void *data = jce_fs_host_read_all(input_path, &nread);
    if (!data) {
        snprintf(result.error, sizeof(result.error),
                 "cannot open: %s", input_path);
        return result;
    }

    if (nread == 0) {
        JCE_FREE(data);
        snprintf(result.error, sizeof(result.error),
                 "empty file: %s", input_path);
        return result;
    }

    /* Dispatch by type. */
    int type = jce_cook_detect_type(input_path);

    /* LUT strip PNGs must ship as verbatim PNG bytes.  jce_texture_load_lut_3d
     * calls jce_texture_decode_cpu, which runs the image service on the raw
     * bytes; a .jceasset wrapper (any chunk layout) is opaque to that raw path,
     * and block-compression destroys the LUT's per-channel precision.
     * Synthesise a successful result pointing at the original file bytes so
     * the cooked output is exactly the source PNG. */
    if (type == JCEASSET_TYPE_TEXTURE && jce_cook_path_is_lut(input_path)) {
        result.data    = (uint8_t *)data;   /* transfer ownership */
        result.size    = (size_t)nread;
        result.success = true;
        return result;   /* caller owns data; skip the JCE_FREE below */
    }

    /* Resolve auto texture format (by platform + normal-map name) when the
       caller didn't force a specific format. cook_texture honors the result. */
    JceCookOptions local;
    const JceCookOptions *use_opts = opts;
    if (type == JCEASSET_TYPE_TEXTURE && opts) {
        local = *opts;
        /* Format auto-selection stays gated on "the caller didn't force one";
         * colour space is resolved for EVERY texture, because a forced format
         * says nothing about whether the texels are sRGB. */
        if (opts->texture_format == JCEASSET_TEXFMT_RGBA8)
            local.texture_format =
                jce_cook_auto_texture_format(input_path, opts->platform);
        local.texture_srgb = jce_cook_path_is_srgb(input_path);

        /* `<asset>.import.json` "colorSpace", which outranks the filename
         * guess above because it was written by something that knew.
         *
         * Read HERE as well as in the bundle packer so the two cook entry
         * points do not disagree.  They already did: the packer honoured the
         * sidecar and this path ignored it, so the same texture cooked two
         * different ways depending on which door it came through, and the CLI
         * -- the one a person reaches for to check a single file -- was the
         * door that lied.  Only "colorSpace" is read here; the older keys stay
         * the packer's business, so this cannot change any existing CLI cook
         * except for textures an importer has explicitly tagged. */
        {
            char side[1024];
            int  n = snprintf(side, sizeof side, "%s.import.json", input_path);
            if (n > 0 && (size_t)n < sizeof side) {
                uint64_t side_len = 0;
                void *side_buf = jce_fs_host_read_all(side, &side_len);
                if (side_buf) {
                    cJSON *sj = cJSON_ParseWithLength((const char *)side_buf,
                                                      (size_t)side_len);
                    if (cJSON_IsObject(sj)) {
                        const cJSON *cs =
                            cJSON_GetObjectItemCaseSensitive(sj, "colorSpace");
                        if (cJSON_IsString(cs))
                            (void)jce_cook_colour_space_parse(cs->valuestring,
                                                              &local.texture_srgb);
                    }
                    if (sj) cJSON_Delete(sj);
                    jce_fs_buffer_free(side_buf);
                }
            }
        }
        use_opts = &local;
    }

    switch (type) {
    case JCEASSET_TYPE_TEXTURE:
        result = jce_cook_texture(data, nread, use_opts);
        break;
    case JCEASSET_TYPE_SOUND:
        result = jce_cook_audio(data, nread, opts);
        break;
    default:
        /* For models, fonts, shaders — pass through as raw for now.
           Full mesh cooking (vertex quantization, etc.) is a future phase. */
        result = jce_cook_raw(data, nread, opts);
        break;
    }

    JCE_FREE(data);
    return result;
}

/* ================================================================== */
/* Utility                                                             */
/* ================================================================== */

void jce_cook_result_free(JceCookResult *result)
{
    if (!result) return;
    JCE_FREE(result->data);
    result->data = NULL;
    result->size = 0;
}

bool jce_cook_write(const JceCookResult *result, const char *output_path)
{
    if (!result || !result->success || !result->data || !output_path)
        return false;

    return jce_fs_host_write_all(output_path, result->data, result->size);
}
