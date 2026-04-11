/*
 * jce_asset_cooker.c  Asset cooking implementation.
 *
 * Converts raw assets into .jceasset binary containers.
 * Each cook function: decode input → build chunks → compress → serialize.
 *
 * Dependencies: SDL3_image (texture decode), miniaudio (audio decode),
 *               ZSTD (compression), XXHash (source hash).
 */

#include "jce_asset_cooker.h"
#include "core/jce_memory.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <zstd.h>
#include <xxhash.h>

#include <string.h>
#include <stdio.h>

#ifndef JCE_NO_AUDIO
#include <miniaudio.h>
#endif

/* ================================================================== */
/* Internal helpers                                                    */
/* ================================================================== */

/* Growable buffer for building .jceasset blobs. */
typedef struct {
	uint8_t *data;
	size_t   size;
	size_t   capacity;
} Buf;

static bool buf_init(Buf *b, size_t cap)
{
	b->data = (uint8_t *)JCE_MALLOC(cap);
	if (!b->data) return false;
	b->size = 0;
	b->capacity = cap;
	return true;
}

static bool buf_grow(Buf *b, size_t needed)
{
	if (b->size + needed <= b->capacity) return true;
	size_t new_cap = b->capacity * 2;
	if (new_cap < b->size + needed) new_cap = b->size + needed;
	uint8_t *tmp = (uint8_t *)JCE_REALLOC(b->data, new_cap);
	if (!tmp) return false;
	b->data = tmp;
	b->capacity = new_cap;
	return true;
}

static bool buf_write(Buf *b, const void *data, size_t len)
{
	if (!buf_grow(b, len)) return false;
	memcpy(b->data + b->size, data, len);
	b->size += len;
	return true;
}

static bool buf_write_zeros(Buf *b, size_t len)
{
	if (!buf_grow(b, len)) return false;
	memset(b->data + b->size, 0, len);
	b->size += len;
	return true;
}

static void buf_free(Buf *b)
{
	JCE_FREE(b->data);
	b->data = NULL;
	b->size = 0;
	b->capacity = 0;
}

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
	int clevel = opts ? opts->compression_level : 3;

	/* Calculate layout sizes. */
	size_t header_size = JCEASSET_HEADER_SIZE;
	size_t toc_size    = (size_t)chunk_count * JCEASSET_CHUNK_ENTRY_SIZE;
	size_t data_start  = header_size + toc_size;

	/* Pre-compress all chunks to get sizes. */
	typedef struct {
		void  *comp_data;
		size_t comp_size;
		bool   is_compressed;
	} CompChunk;

	CompChunk *comp = (CompChunk *)JCE_CALLOC(chunk_count, sizeof(CompChunk));
	if (!comp) {
		snprintf(result.error, sizeof(result.error), "allocation failed");
		return result;
	}

	/* Reusable ZSTD compression context — avoids repeated internal
	   allocation/deallocation when compressing multiple chunks. */
	ZSTD_CCtx *cctx = (clevel > 0) ? ZSTD_createCCtx() : NULL;

	size_t total_data = 0;
	for (uint32_t i = 0; i < chunk_count; i++) {
		if (cctx && chunks[i].raw_size > 64) {
			size_t bound = ZSTD_compressBound(chunks[i].raw_size);
			comp[i].comp_data = JCE_MALLOC(bound);
			if (comp[i].comp_data) {
				size_t csize = ZSTD_compressCCtx(cctx,
				                                  comp[i].comp_data, bound,
				                                  chunks[i].raw_data,
				                                  chunks[i].raw_size,
				                                  clevel);
				if (!ZSTD_isError(csize) && csize < chunks[i].raw_size) {
					comp[i].comp_size = csize;
					comp[i].is_compressed = true;
				} else {
					JCE_FREE(comp[i].comp_data);
					comp[i].comp_data = NULL;
				}
			}
		}

		if (comp[i].is_compressed)
			total_data += comp[i].comp_size;
		else
			total_data += chunks[i].raw_size;
	}

	ZSTD_freeCCtx(cctx);

	/* Build final blob. */
	Buf buf;
	if (!buf_init(&buf, data_start + total_data + 64)) {
		for (uint32_t i = 0; i < chunk_count; i++) JCE_FREE(comp[i].comp_data);
		JCE_FREE(comp);
		snprintf(result.error, sizeof(result.error), "allocation failed");
		return result;
	}

	/* Write header. */
	JceAssetFileHeader hdr = {0};
	hdr.magic[0]    = JCEASSET_MAGIC_0;
	hdr.magic[1]    = JCEASSET_MAGIC_1;
	hdr.magic[2]    = JCEASSET_MAGIC_2;
	hdr.magic[3]    = JCEASSET_MAGIC_3;
	hdr.version     = JCEASSET_VERSION;
	hdr.asset_type  = asset_type;
	hdr.chunk_count = chunk_count;
	hdr.source_hash = source_hash;
	buf_write(&buf, &hdr, sizeof(hdr));

	/* Pad header to JCEASSET_HEADER_SIZE if struct is smaller. */
	if (sizeof(hdr) < JCEASSET_HEADER_SIZE)
		buf_write_zeros(&buf, JCEASSET_HEADER_SIZE - sizeof(hdr));

	/* Write chunk table (fill data_offset after calculating). */
	size_t offset = data_start;

	for (uint32_t i = 0; i < chunk_count; i++) {
		JceAssetChunkEntry entry = {0};
		entry.chunk_type    = chunks[i].chunk_type;
		entry.compression   = comp[i].is_compressed
			? JCEASSET_COMPRESS_ZSTD : JCEASSET_COMPRESS_NONE;
		entry.data_offset   = (uint64_t)offset;
		entry.original_size = (uint64_t)chunks[i].raw_size;

		if (comp[i].is_compressed) {
			entry.compressed_size = (uint64_t)comp[i].comp_size;
			offset += comp[i].comp_size;
		} else {
			entry.compressed_size = (uint64_t)chunks[i].raw_size;
			offset += chunks[i].raw_size;
		}

		buf_write(&buf, &entry, sizeof(entry));
		if (sizeof(entry) < JCEASSET_CHUNK_ENTRY_SIZE)
			buf_write_zeros(&buf, JCEASSET_CHUNK_ENTRY_SIZE - sizeof(entry));
	}

	/* Write data blocks. */
	for (uint32_t i = 0; i < chunk_count; i++) {
		if (comp[i].is_compressed) {
			buf_write(&buf, comp[i].comp_data, comp[i].comp_size);
		} else {
			buf_write(&buf, chunks[i].raw_data, chunks[i].raw_size);
		}
	}

	/* Cleanup compressed buffers. */
	for (uint32_t i = 0; i < chunk_count; i++) JCE_FREE(comp[i].comp_data);
	JCE_FREE(comp);

	result.data    = buf.data;
	result.size    = buf.size;
	result.success = true;
	return result;
}

/* ================================================================== */
/* Cook: Texture                                                       */
/* ================================================================== */

JceCookResult jce_cook_texture(const void *input, size_t input_size,
                               const JceCookOptions *opts)
{
	JceCookResult result = {0};

	/* Decode image via SDL3_image. */
	SDL_IOStream *io = SDL_IOFromConstMem(input, input_size);
	if (!io) {
		snprintf(result.error, sizeof(result.error), "SDL_IOFromConstMem failed");
		return result;
	}

	SDL_Surface *surf = IMG_Load_IO(io, true);
	if (!surf) {
		snprintf(result.error, sizeof(result.error), "IMG_Load_IO failed: %s",
		         SDL_GetError());
		return result;
	}

	/* Ensure RGBA8. */
	if (surf->format != SDL_PIXELFORMAT_RGBA32) {
		SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
		SDL_DestroySurface(surf);
		surf = conv;
		if (!surf) {
			snprintf(result.error, sizeof(result.error), "RGBA conversion failed");
			return result;
		}
	}

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

	/* Build info chunk. */
	JceAssetTexInfo info = {0};
	info.width     = (uint32_t)surf->w;
	info.height    = (uint32_t)surf->h;
	info.format    = 0; /* RGBA8 */
	info.mip_count = 1;
	info.flags     = 1; /* sRGB */

	/* Pixel data. */
	size_t pixel_size = (size_t)surf->w * (size_t)surf->h * 4;

	uint64_t source_hash = XXH3_64bits(input, input_size);

	ChunkInput chunks[2];
	chunks[0].chunk_type = JCEASSET_CHUNK_TEX_INFO;
	chunks[0].raw_data   = &info;
	chunks[0].raw_size   = sizeof(info);
	chunks[1].chunk_type = JCEASSET_CHUNK_TEX_PIXELS;
	chunks[1].raw_data   = surf->pixels;
	chunks[1].raw_size   = pixel_size;

	result = build_asset(JCEASSET_TYPE_TEXTURE, source_hash, chunks, 2, opts);

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
	/* Decode via miniaudio. */
	ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
	ma_decoder decoder;

	if (ma_decoder_init_memory(input, input_size, &cfg, &decoder) != MA_SUCCESS) {
		snprintf(result.error, sizeof(result.error), "miniaudio decode failed");
		return result;
	}

	ma_uint64 total_frames = 0;
	ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

	ma_uint32 channels    = decoder.outputChannels;
	ma_uint32 sample_rate = decoder.outputSampleRate;
	void *pcm = NULL;

	if (total_frames == 0) {
		/* Unknown length — decode in chunks. */
		size_t alloc = 256 * 1024;
		size_t used  = 0;
		pcm = JCE_MALLOC(alloc * channels * sizeof(int16_t));
		if (!pcm) {
			ma_decoder_uninit(&decoder);
			snprintf(result.error, sizeof(result.error), "allocation failed");
			return result;
		}
		for (;;) {
			if (used + 4096 > alloc) {
				alloc *= 2;
				void *tmp = JCE_REALLOC(pcm, alloc * channels * sizeof(int16_t));
				if (!tmp) {
					JCE_FREE(pcm);
					ma_decoder_uninit(&decoder);
					snprintf(result.error, sizeof(result.error), "realloc failed");
					return result;
				}
				pcm = tmp;
			}
			ma_uint64 read = 0;
			ma_decoder_read_pcm_frames(&decoder,
				(int16_t *)pcm + used * channels, 4096, &read);
			if (read == 0) break;
			used += (size_t)read;
		}
		total_frames = (ma_uint64)used;
	} else {
		pcm = JCE_MALLOC((size_t)(total_frames * channels * sizeof(int16_t)));
		if (!pcm) {
			ma_decoder_uninit(&decoder);
			snprintf(result.error, sizeof(result.error), "allocation failed");
			return result;
		}
		ma_uint64 read = 0;
		ma_decoder_read_pcm_frames(&decoder, pcm, total_frames, &read);
		total_frames = read;
	}

	ma_decoder_uninit(&decoder);

	/* Build info chunk. */
	JceAssetAudioInfo info = {0};
	info.sample_rate     = sample_rate;
	info.channels        = (uint16_t)channels;
	info.bits_per_sample = 16;
	info.total_frames    = total_frames;
	info.format          = 0; /* PCM_S16 */

	size_t pcm_size = (size_t)(total_frames * channels * sizeof(int16_t));
	uint64_t source_hash = XXH3_64bits(input, input_size);

	ChunkInput chunks[2];
	chunks[0].chunk_type = JCEASSET_CHUNK_AUDIO_INFO;
	chunks[0].raw_data   = &info;
	chunks[0].raw_size   = sizeof(info);
	chunks[1].chunk_type = JCEASSET_CHUNK_AUDIO_PCM;
	chunks[1].raw_data   = pcm;
	chunks[1].raw_size   = pcm_size;

	result = build_asset(JCEASSET_TYPE_SOUND, source_hash, chunks, 2, opts);

	JCE_FREE(pcm);
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

int jce_cook_detect_type(const char *path)
{
	if (!path) return -1;
	const char *dot = strrchr(path, '.');
	if (!dot) return JCEASSET_TYPE_RAW;

	dot++; /* skip the dot */

	/* Texture extensions. */
	if (SDL_strcasecmp(dot, "png") == 0 ||
	    SDL_strcasecmp(dot, "jpg") == 0 ||
	    SDL_strcasecmp(dot, "jpeg") == 0 ||
	    SDL_strcasecmp(dot, "bmp") == 0 ||
	    SDL_strcasecmp(dot, "tga") == 0)
		return JCEASSET_TYPE_TEXTURE;

	/* Audio extensions. */
	if (SDL_strcasecmp(dot, "wav") == 0 ||
	    SDL_strcasecmp(dot, "ogg") == 0 ||
	    SDL_strcasecmp(dot, "flac") == 0 ||
	    SDL_strcasecmp(dot, "mp3") == 0)
		return JCEASSET_TYPE_SOUND;

	/* Mesh/model extensions. */
	if (SDL_strcasecmp(dot, "obj") == 0 ||
	    SDL_strcasecmp(dot, "fbx") == 0 ||
	    SDL_strcasecmp(dot, "gltf") == 0 ||
	    SDL_strcasecmp(dot, "glb") == 0)
		return JCEASSET_TYPE_MODEL;

	/* Font extensions. */
	if (SDL_strcasecmp(dot, "ttf") == 0 ||
	    SDL_strcasecmp(dot, "otf") == 0)
		return JCEASSET_TYPE_FONT;

	/* Shader extensions. */
	if (SDL_strcasecmp(dot, "sc") == 0 ||
	    SDL_strcasecmp(dot, "bin") == 0)
		return JCEASSET_TYPE_SHADER;

	return JCEASSET_TYPE_RAW;
}

JceCookResult jce_cook_file(const char *input_path,
                            const JceCookOptions *opts)
{
	JceCookResult result = {0};
	if (!input_path) {
		snprintf(result.error, sizeof(result.error), "null input path");
		return result;
	}

	/* Read file. */
	FILE *fp = fopen(input_path, "rb");
	if (!fp) {
		snprintf(result.error, sizeof(result.error),
		         "cannot open: %s", input_path);
		return result;
	}

	fseek(fp, 0, SEEK_END);
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (file_size <= 0) {
		fclose(fp);
		snprintf(result.error, sizeof(result.error),
		         "empty file: %s", input_path);
		return result;
	}

	void *data = JCE_MALLOC((size_t)file_size);
	if (!data) {
		fclose(fp);
		snprintf(result.error, sizeof(result.error), "allocation failed");
		return result;
	}

	size_t nread = fread(data, 1, (size_t)file_size, fp);
	fclose(fp);

	if (nread != (size_t)file_size) {
		JCE_FREE(data);
		snprintf(result.error, sizeof(result.error),
		         "read error: %s", input_path);
		return result;
	}

	/* Dispatch by type. */
	int type = jce_cook_detect_type(input_path);
	switch (type) {
	case JCEASSET_TYPE_TEXTURE:
		result = jce_cook_texture(data, nread, opts);
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

	FILE *fp = fopen(output_path, "wb");
	if (!fp) return false;

	size_t written = fwrite(result->data, 1, result->size, fp);
	fclose(fp);
	return written == result->size;
}
