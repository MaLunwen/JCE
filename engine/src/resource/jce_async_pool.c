/*
 * jce_async_pool.c  Worker thread pool for async asset loading.
 *
 * Workers wait on a condition variable for incoming requests.
 * Completed requests are moved to a done-list protected by a mutex.
 * Main thread drains the done-list each frame.
 *
 * Worker decoding:
 *   TEXTURE → pak_decompress + IMG_Load_IO → SDL_Surface (RGBA8)
 *   AUDIO   → pak_decompress + miniaudio decode → PCM s16
 *   MESH    → pak_decompress (raw bytes for main-thread GPU upload)
 *   RAW     → pak_decompress (pass-through)
 */

#include "jce_async_pool.h"
#include "../core/jce_memory.h"
#include "jce_asset_reader.h"
#include <jce/resource/pak_loader.h>
#include <jce/resource/jce_asset_format.h>
#include <jce/core/jce_log.h>
#include <jce/core/jce_profiler.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <string.h>
#include <stdio.h>

#ifndef JCE_NO_AUDIO
#include <miniaudio.h>
#endif

#define LOG_TAG "jce_pool"
#define MAX_WORKERS 8

/* ================================================================== */
/* Pool internals                                                      */
/* ================================================================== */

struct JceAsyncPool {
	SDL_Thread *workers[MAX_WORKERS];
	uint32_t    num_workers;

	/* Request queue (FIFO, mutex-protected). */
	SDL_Mutex     *queue_lock;
	SDL_Condition *queue_cond;
	JceAsyncRequest *queue_head;
	JceAsyncRequest *queue_tail;
	SDL_AtomicInt    queue_count;

	/* Completed list (mutex-protected, drained by main thread). */
	SDL_Mutex       *done_lock;
	JceAsyncRequest *done_head;
	JceAsyncRequest *done_tail;

	/* Shutdown flag. */
	SDL_AtomicInt shutdown;
};

/* ================================================================== */
/* Queue helpers (must hold queue_lock)                                */
/* ================================================================== */

static void queue_push(JceAsyncPool *pool, JceAsyncRequest *req)
{
	req->next = NULL;
	if (pool->queue_tail)
		pool->queue_tail->next = req;
	else
		pool->queue_head = req;
	pool->queue_tail = req;
	SDL_AddAtomicInt(&pool->queue_count, 1);
}

static JceAsyncRequest *queue_pop(JceAsyncPool *pool)
{
	JceAsyncRequest *req = pool->queue_head;
	if (!req) return NULL;
	pool->queue_head = req->next;
	if (!pool->queue_head)
		pool->queue_tail = NULL;
	req->next = NULL;
	SDL_AddAtomicInt(&pool->queue_count, -1);
	return req;
}

static void done_push(JceAsyncPool *pool, JceAsyncRequest *req)
{
	SDL_LockMutex(pool->done_lock);
	req->next = NULL;
	if (pool->done_tail)
		pool->done_tail->next = req;
	else
		pool->done_head = req;
	pool->done_tail = req;
	SDL_UnlockMutex(pool->done_lock);
}

/* ================================================================== */
/* Worker: texture decode                                              */
/* ================================================================== */

static SDL_Surface *ensure_rgba8(SDL_Surface *src)
{
	if (!src) return NULL;
	if (src->format == SDL_PIXELFORMAT_RGBA32) return src;
	SDL_Surface *conv = SDL_ConvertSurface(src, SDL_PIXELFORMAT_RGBA32);
	SDL_DestroySurface(src);
	return conv;
}

static void decode_texture_inner(JceAsyncRequest *req)
{
	const PakAsset *asset = pak_find(req->pak, req->path);
	if (!asset) {
		LOG_ERROR(LOG_TAG, "not found: %s", req->path);
		return;
	}

	void *buf = JCE_MALLOC(asset->original_size);
	if (!buf) return;

	size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
	if (n == 0) {
		JCE_FREE(buf);
		LOG_ERROR(LOG_TAG, "decompress failed: %s", req->path);
		return;
	}

	/* ── Cooked path: .jceasset TEX_PIXELS → raw RGBA8 ── */
	if (jce_asset_is_cooked(buf, n)) {
		JceAssetView view;
		if (!jce_asset_open(&view, buf, n)) {
			JCE_FREE(buf);
			return;
		}

		const JceAssetChunkEntry *info_c =
			jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_INFO);
		const JceAssetChunkEntry *pix_c =
			jce_asset_find_chunk(&view, JCEASSET_CHUNK_TEX_PIXELS);

		if (!info_c || !pix_c) {
			JCE_FREE(buf);
			return;
		}

		JceAssetTexInfo tex_info;
		if (jce_asset_chunk_data(&view, info_c,
		                          &tex_info, sizeof(tex_info)) == 0) {
			JCE_FREE(buf);
			return;
		}

		size_t pixel_size = (size_t)pix_c->original_size;
		void *pixels = JCE_MALLOC(pixel_size + sizeof(JceAssetTexInfo));
		if (!pixels) { JCE_FREE(buf); return; }

		/* Store tex_info header before pixel data so finalize can read it. */
		memcpy(pixels, &tex_info, sizeof(tex_info));
		if (jce_asset_chunk_data(&view, pix_c,
		                          (uint8_t *)pixels + sizeof(JceAssetTexInfo),
		                          pixel_size) == 0) {
			JCE_FREE(pixels);
			JCE_FREE(buf);
			return;
		}

		JCE_FREE(buf);
		req->decoded_data = pixels;
		req->decoded_size = pixel_size + sizeof(JceAssetTexInfo);
		req->is_cooked = true;
		req->success = true;
		return;
	}

	/* ── Raw path: PNG/JPG → SDL_Surface ── */
	SDL_IOStream *io = SDL_IOFromConstMem(buf, (size_t)asset->original_size);
	if (!io) {
		JCE_FREE(buf);
		return;
	}

	SDL_Surface *surf = IMG_Load_IO(io, true); /* closes io */
	JCE_FREE(buf);

	if (!surf) {
		LOG_ERROR(LOG_TAG, "IMG_Load_IO failed: %s", req->path);
		return;
	}

	surf = ensure_rgba8(surf);
	if (surf) {
		req->decoded_data = surf;
		req->decoded_size = (size_t)(surf->w * surf->h * 4);
		req->success = true;
	}
}

static void decode_texture(JceAsyncRequest *req)
{
	JCE_PROFILE_ZONE_N("Asset::DecodeTexture");
	decode_texture_inner(req);
	JCE_PROFILE_ZONE_END;
}

/* ================================================================== */
/* Worker: audio decode                                                */
/* ================================================================== */

static void decode_audio_inner(JceAsyncRequest *req)
{
#ifdef JCE_NO_AUDIO
	LOG_WARN(LOG_TAG, "audio disabled: %s", req->path);
	return;
#else
	const PakAsset *asset = pak_find(req->pak, req->path);
	if (!asset) {
		LOG_ERROR(LOG_TAG, "not found: %s", req->path);
		return;
	}

	void *buf = JCE_MALLOC(asset->original_size);
	if (!buf) return;

	size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
	if (n == 0) {
		JCE_FREE(buf);
		return;
	}

	/* Pack result: decoded audio info + PCM data.
	   Same layout for both cooked and raw paths. */
	typedef struct {
		uint32_t sample_rate;
		uint16_t channels;
		uint16_t bits_per_sample;
		uint32_t pcm_size;
	} AudioResult;

	/* ── Cooked path: .jceasset AUDIO_INFO + AUDIO_PCM → direct copy ── */
	if (jce_asset_is_cooked(buf, n)) {
		JceAssetView view;
		if (!jce_asset_open(&view, buf, n)) {
			JCE_FREE(buf);
			return;
		}

		const JceAssetChunkEntry *info_c =
			jce_asset_find_chunk(&view, JCEASSET_CHUNK_AUDIO_INFO);
		const JceAssetChunkEntry *pcm_c =
			jce_asset_find_chunk(&view, JCEASSET_CHUNK_AUDIO_PCM);

		if (!info_c || !pcm_c) {
			JCE_FREE(buf);
			return;
		}

		JceAssetAudioInfo ainfo;
		if (jce_asset_chunk_data(&view, info_c,
		                          &ainfo, sizeof(ainfo)) == 0) {
			JCE_FREE(buf);
			return;
		}

		uint32_t pcm_size = (uint32_t)pcm_c->original_size;
		AudioResult *result = JCE_MALLOC(sizeof(AudioResult) + pcm_size);
		if (!result) { JCE_FREE(buf); return; }

		result->sample_rate     = ainfo.sample_rate;
		result->channels        = ainfo.channels;
		result->bits_per_sample = ainfo.bits_per_sample;
		result->pcm_size        = pcm_size;

		if (jce_asset_chunk_data(&view, pcm_c,
		                          (uint8_t *)result + sizeof(AudioResult),
		                          pcm_size) == 0) {
			JCE_FREE(result);
			JCE_FREE(buf);
			return;
		}

		JCE_FREE(buf);
		req->decoded_data = result;
		req->decoded_size = sizeof(AudioResult) + pcm_size;
		req->is_cooked = true;
		req->success = true;
		return;
	}

	/* ── Raw path: OGG/WAV → miniaudio decode → PCM s16 ── */
	ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
	ma_decoder decoder;

	if (ma_decoder_init_memory(buf, n, &cfg, &decoder) != MA_SUCCESS) {
		LOG_ERROR(LOG_TAG, "audio decode failed: %s", req->path);
		JCE_FREE(buf);
		return;
	}

	ma_uint64 total_frames = 0;
	ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

	ma_uint32 channels    = decoder.outputChannels;
	ma_uint32 sample_rate = decoder.outputSampleRate;
	void *pcm = NULL;

	if (total_frames == 0) {
		/* Unknown length — decode in growing chunks. */
		size_t alloc_frames = 256 * 1024;
		size_t used_frames  = 0;
		pcm = JCE_MALLOC(alloc_frames * channels * sizeof(int16_t));
		if (!pcm) { ma_decoder_uninit(&decoder); JCE_FREE(buf); return; }

		for (;;) {
			if (used_frames + 4096 > alloc_frames) {
				alloc_frames *= 2;
				void *tmp = JCE_REALLOC(pcm,
					alloc_frames * channels * sizeof(int16_t));
				if (!tmp) {
					JCE_FREE(pcm);
					ma_decoder_uninit(&decoder);
					JCE_FREE(buf);
					return;
				}
				pcm = tmp;
			}
			ma_uint64 read = 0;
			ma_decoder_read_pcm_frames(&decoder,
				(int16_t *)pcm + used_frames * channels, 4096, &read);
			if (read == 0) break;
			used_frames += (size_t)read;
		}
		total_frames = (ma_uint64)used_frames;
	} else {
		pcm = JCE_MALLOC((size_t)(total_frames * channels * sizeof(int16_t)));
		if (!pcm) { ma_decoder_uninit(&decoder); JCE_FREE(buf); return; }

		ma_uint64 frames_read = 0;
		ma_decoder_read_pcm_frames(&decoder, pcm, total_frames, &frames_read);
		total_frames = frames_read;
	}

	ma_decoder_uninit(&decoder);
	JCE_FREE(buf);

	uint32_t pcm_size = (uint32_t)(total_frames * channels * sizeof(int16_t));

	AudioResult *result = JCE_MALLOC(sizeof(AudioResult) + pcm_size);
	if (!result) { JCE_FREE(pcm); return; }

	result->sample_rate     = sample_rate;
	result->channels        = (uint16_t)channels;
	result->bits_per_sample = 16;
	result->pcm_size        = pcm_size;
	memcpy((uint8_t *)result + sizeof(AudioResult), pcm, pcm_size);
	JCE_FREE(pcm);

	req->decoded_data = result;
	req->decoded_size = sizeof(AudioResult) + pcm_size;
	req->success = true;
#endif
}

static void decode_audio(JceAsyncRequest *req)
{
	JCE_PROFILE_ZONE_N("Asset::DecodeAudio");
	decode_audio_inner(req);
	JCE_PROFILE_ZONE_END;
}

/* ================================================================== */
/* Worker: raw/mesh decompress                                         */
/* ================================================================== */

static void decode_raw(JceAsyncRequest *req)
{
	const PakAsset *asset = pak_find(req->pak, req->path);
	if (!asset) {
		LOG_ERROR(LOG_TAG, "not found: %s", req->path);
		return;
	}

	void *buf = JCE_MALLOC(asset->original_size);
	if (!buf) return;

	size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
	if (n == 0) {
		JCE_FREE(buf);
		return;
	}

	req->decoded_data = buf;
	req->decoded_size = n;
	req->success = true;
}

/* ================================================================== */
/* Worker thread entry point                                           */
/* ================================================================== */

static int worker_func(void *data)
{
	JceAsyncPool *pool = (JceAsyncPool *)data;

	for (;;) {
		SDL_LockMutex(pool->queue_lock);

		/* Wait for a request or shutdown. */
		while (!pool->queue_head &&
		       SDL_GetAtomicInt(&pool->shutdown) == 0) {
			SDL_WaitCondition(pool->queue_cond, pool->queue_lock);
		}

		if (SDL_GetAtomicInt(&pool->shutdown) != 0 &&
		    !pool->queue_head) {
			SDL_UnlockMutex(pool->queue_lock);
			break;
		}

		JceAsyncRequest *req = queue_pop(pool);
		SDL_UnlockMutex(pool->queue_lock);

		if (!req) continue;

		/* Dispatch by type. */
		switch (req->type) {
		case JCE_ASYNC_TEXTURE: decode_texture(req); break;
		case JCE_ASYNC_AUDIO:   decode_audio(req);   break;
		case JCE_ASYNC_MESH:    decode_raw(req);      break;
		case JCE_ASYNC_MODEL:   decode_raw(req);      break;
		case JCE_ASYNC_RAW:     decode_raw(req);      break;
		case JCE_ASYNC_FONT:    decode_raw(req);      break;
		}

		SDL_SetAtomicInt(&req->done, 1);
		done_push(pool, req);
	}

	return 0;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

JceAsyncPool *jce_pool_create(uint32_t num_workers)
{
	JceAsyncPool *pool = JCE_NEW(JceAsyncPool);
	if (!pool) return NULL;

	if (num_workers == 0) {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
		num_workers = 2;
#else
		num_workers = 3;
#endif
	}
	if (num_workers > MAX_WORKERS)
		num_workers = MAX_WORKERS;

	pool->queue_lock = SDL_CreateMutex();
	pool->queue_cond = SDL_CreateCondition();
	pool->done_lock  = SDL_CreateMutex();

	if (!pool->queue_lock || !pool->queue_cond || !pool->done_lock) {
		jce_pool_destroy(pool);
		return NULL;
	}

	pool->num_workers = num_workers;
	for (uint32_t i = 0; i < num_workers; i++) {
		char name[32];
		snprintf(name, sizeof(name), "jce_worker_%u", i);
		pool->workers[i] = SDL_CreateThread(worker_func, name, pool);
		if (!pool->workers[i]) {
			LOG_ERROR(LOG_TAG, "failed to create worker thread %u", i);
		}
	}

	LOG_INFO(LOG_TAG, "async pool: %u workers", num_workers);
	return pool;
}

void jce_pool_destroy(JceAsyncPool *pool)
{
	if (!pool) return;

	/* Signal shutdown. */
	SDL_SetAtomicInt(&pool->shutdown, 1);
	if (pool->queue_cond)
		SDL_BroadcastCondition(pool->queue_cond);

	/* Join all workers. */
	for (uint32_t i = 0; i < pool->num_workers; i++) {
		if (pool->workers[i])
			SDL_WaitThread(pool->workers[i], NULL);
	}

	/* Free remaining queued requests. */
	while (pool->queue_head) {
		JceAsyncRequest *req = pool->queue_head;
		pool->queue_head = req->next;
		if (req->decoded_data) JCE_FREE(req->decoded_data);
		JCE_FREE(req);
	}

	/* Free remaining done requests. */
	while (pool->done_head) {
		JceAsyncRequest *req = pool->done_head;
		pool->done_head = req->next;
		if (req->decoded_data) JCE_FREE(req->decoded_data);
		JCE_FREE(req);
	}

	if (pool->queue_cond) SDL_DestroyCondition(pool->queue_cond);
	if (pool->queue_lock) SDL_DestroyMutex(pool->queue_lock);
	if (pool->done_lock)  SDL_DestroyMutex(pool->done_lock);
	JCE_FREE(pool);
}

JceAsyncRequest *jce_pool_submit(JceAsyncPool *pool,
                                 JceAsyncRequestType type,
                                 uint16_t slot_index,
                                 const char *path,
                                 PakArchive *pak,
                                 JceFileSystem *fs,
                                 const JceAsyncLoadInfo *info)
{
	if (!pool || !path) return NULL;

	JceAsyncRequest *req = JCE_NEW(JceAsyncRequest);
	if (!req) return NULL;

	req->type       = type;
	req->slot_index = slot_index;
	req->pak        = pak;
	req->fs         = fs;
	snprintf(req->path, sizeof(req->path), "%s", path);

	if (info)
		req->info = *info;

	SDL_LockMutex(pool->queue_lock);
	queue_push(pool, req);
	SDL_SignalCondition(pool->queue_cond);
	SDL_UnlockMutex(pool->queue_lock);

	return req;
}

JceAsyncRequest *jce_pool_drain(JceAsyncPool *pool, uint32_t max_count)
{
	if (!pool) return NULL;

	SDL_LockMutex(pool->done_lock);

	JceAsyncRequest *result = NULL;

	if (max_count == 0 || max_count >= UINT32_MAX) {
		/* Drain all. */
		result = pool->done_head;
		pool->done_head = NULL;
		pool->done_tail = NULL;
	} else {
		/* Drain up to max_count. */
		JceAsyncRequest *head = pool->done_head;
		JceAsyncRequest *tail = NULL;
		uint32_t count = 0;

		JceAsyncRequest *cur = head;
		while (cur && count < max_count) {
			tail = cur;
			cur = cur->next;
			count++;
		}

		if (tail) {
			result = head;
			tail->next = NULL;
			pool->done_head = cur;
			if (!cur) pool->done_tail = NULL;
		}
	}

	SDL_UnlockMutex(pool->done_lock);
	return result;
}

void jce_pool_free_request(JceAsyncRequest *req)
{
	if (!req) return;
	/* Note: decoded_data ownership is transferred to the asset manager
	   during finalization. Only free here if it wasn't claimed. */
	if (req->decoded_data)
		JCE_FREE(req->decoded_data);
	JCE_FREE(req);
}

uint32_t jce_pool_pending_count(const JceAsyncPool *pool)
{
	if (!pool) return 0;
	return (uint32_t)SDL_GetAtomicInt(
		(SDL_AtomicInt *)&pool->queue_count);
}
