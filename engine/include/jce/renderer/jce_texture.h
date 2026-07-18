/*
 * jce_texture.h  Cross-platform texture loading and management.
 *
 * Loads images from PAK archive memory buffers into GPU textures.
 * Supports PNG, JPG, BMP, and other common image formats.
 */

#ifndef JCE_TEXTURE_H
#define JCE_TEXTURE_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/resource/jce_asset_format.h>  /* JceAssetTexInfo */

JCE_EXTERN_C_BEGIN

typedef struct JcePakArchive  JcePakArchive;

/* Load a texture from a PAK asset path (e.g. "textures/chalet.jpg").
   Returns JCE_TEXTURE_INVALID on failure. Uses clamp sampling. */
JCE_API JceTexture jce_texture_load(const JcePakArchive *pak, const char *asset_path);

/* Load a texture with explicit sampler flags (JCE_TEX_CLAMP/WRAP/MIRROR). */
JceTexture jce_texture_load_ex(const JcePakArchive *pak, const char *asset_path,
                                int sampler_mode);

/* Load a 3D colour-grading LUT from a horizontal PNG strip: N tiles of NxN
   laid left-to-right (image = N*N wide, N tall), tile z holding blue index z.
   Reordered into an N x N x N RGBA8 3D texture (CLAMP sampling). Returns
   JCE_TEXTURE_INVALID if the backend lacks TEXTURE_3D support or on decode
   failure. */
JCE_API JceTexture jce_texture_load_lut_3d(const JcePakArchive *pak,
                                           const char *asset_path);

/* Load a 3D LUT from a loose host-filesystem path (absolute or CWD-relative).
   Same PNG-strip format and N×N×N output as jce_texture_load_lut_3d; uses
   jce_fs_host_read_all instead of a PAK archive.  Intended for editor mode
   where assets are loose files on disk rather than cooked into a PAK.
   Returns JCE_TEXTURE_INVALID if the file cannot be read, the backend lacks
   TEXTURE_3D support, or the image is not the expected N*N x N shape. */
JCE_API JceTexture jce_texture_load_lut_3d_host(const char *host_path);

/* Create a GPU texture from a pre-decoded RGBA8 surface (SDL_Surface*).
   The opaque pointer must be a valid SDL_Surface*.
   Caller retains ownership of the surface. */
JCE_API JceTexture jce_texture_load_from_surface(const void *surface, int sampler_mode);

/* Load a texture from raw pixel data (RGBA8, top-left origin).
   Caller retains ownership of data. */
JceTexture jce_texture_from_rgba(const void *data,
                                  uint32_t width, uint32_t height);

/* Create a GPU texture from a cooked .jceasset pixel payload, honoring the
   block-compressed format (BC1/BC3/BC5/BC7/ASTC4x4/ETC2A, default RGBA8) and
   the mip chain recorded in `info`. `pixels` points at the full TEX_PIXELS
   payload (all mips concatenated when info->mip_count > 1); `pixel_bytes` is
   its size. Used by the async texture finalize path so it matches the
   synchronous cooked loader. Caller retains ownership of `info`/`pixels`. */
JCE_API JceTexture jce_texture_from_cooked(const JceAssetTexInfo *info,
                                           const void *pixels,
                                           size_t pixel_bytes,
                                           int sampler_mode);

/* ================================================================== */
/* Worker-decode + render-thread-upload split                          */
/* ================================================================== */
/*
 * jce_texture_load_ex() does PAK decompress + image decode + bgfx_create
 * in one call — fine on the render thread, but the decode is the slow,
 * variable-latency part and bgfx resource creation must stay on the
 * render thread.  This pair splits the two so a worker can decode while
 * the render thread only does the cheap GPU upload:
 *
 *   worker:        JceTextureCpu *c = jce_texture_decode_cpu(pak,path,mode);
 *   render thread: JceTexture t = jce_texture_upload_cpu(c);   // consumes c
 *
 * decode_cpu touches no bgfx/GPU state (PAK decompress + SDL_image /
 * cooked parse only), so it is safe to call from any thread.  upload_cpu
 * does the bgfx_create on the calling thread and frees `c`.  On a cancel
 * path, jce_texture_cpu_free() releases a decoded result without upload.
 */
typedef struct JceTextureCpu JceTextureCpu;

/* Worker-safe: decode `asset_path` from `pak` to a CPU result.  Returns
 * NULL on failure (asset missing, bad format, decode error). */
JCE_API JceTextureCpu *jce_texture_decode_cpu(const JcePakArchive *pak,
                                              const char *asset_path,
                                              int sampler_mode);

/* Worker-safe: decode an in-memory encoded image (PNG/JPG/… bytes, e.g. a
 * glTF embedded buffer-view) to a CPU result.  Returns NULL on failure. */
JCE_API JceTextureCpu *jce_texture_decode_cpu_mem(const void *encoded,
                                                  size_t size,
                                                  int sampler_mode);

/* Decode the BASE MIP of a cooked .jceasset texture (block-compressed or
 * RGBA8) to a freshly-allocated RGBA8 buffer.  For consumers that want CPU
 * RGBA8 rather than a GPU block upload — e.g. the editor previewing a scene
 * loaded from a bundle/PAK, whose texture cache is RGBA8-based.  On success
 * *out_rgba8 points at a jce_malloc'd buffer the caller must jce_free.
 * Returns false (and leaves *out_rgba8 NULL) for non-cooked / malformed
 * input, so callers can fall back to a raw image decode. */
JCE_API bool jce_texture_decode_cooked_rgba8(const void *encoded, size_t size,
                                             uint8_t **out_rgba8,
                                             uint32_t *out_w, uint32_t *out_h);

/* Render-thread: upload a decoded result to a GPU texture and free `c`
 * (also frees `c` when it is NULL/failed).  Returns JCE_TEXTURE_INVALID
 * on failure. */
JCE_API JceTexture jce_texture_upload_cpu(JceTextureCpu *c);

/* Free a decoded result without uploading (cancellation). */
JCE_API void jce_texture_cpu_free(JceTextureCpu *c);

/* Update an existing RGBA8 texture in-place.
   Returns false when the handle is invalid, dimensions mismatch, or upload fails. */
bool       jce_texture_update_rgba(JceTexture tex, const void *data,
                                   uint32_t width, uint32_t height);

/* Update an existing RGBA8 texture in-place (zero-copy variant).
   bgfx takes a reference to `data`; caller guarantees data remains valid
   until bgfx_frame() is called (end of the current render frame).
   Returns false when the handle is invalid or dimensions mismatch. */
bool       jce_texture_update_rgba_ref(JceTexture tex, const void *data,
                                       uint32_t width, uint32_t height);
JCE_API void       jce_texture_get_size(JceTexture tex, uint32_t *w, uint32_t *h);
/* Source mip count (>=1) of a registry-loaded texture; 1 if not tracked. Used by
 * the texture-array batcher to build the albedo array with a matching mip chain. */
JCE_API uint32_t   jce_texture_get_mips(JceTexture tex);

/* Destroy a texture. */
JCE_API void       jce_texture_destroy(JceTexture tex);

/* ================================================================== */
/* Runtime mip streaming (P3-A.2)                                      */
/* ================================================================== */
/*
 * CONTRACT (read before relying on this for memory reclaim).
 *
 * These calls express a *desired* mip residency.  The effective top-mip is
 * the maximum of:
 *   - the global bias       (jce_texture_set_global_mip_bias),
 *   - the per-texture bias  (jce_texture_set_mip_bias),
 *   - the requested floor   (jce_texture_request_mip_residency),
 *   - the caps floor        (derived from jce_renderer_get_tier(); see
 *                            engine/src/renderer/jce_texture.c for the
 *                            Low/Mid/High → floor matrix),
 * clamped so the smallest 4x4 mip-tail is always resident.
 *
 * Whether the desire is *physically realised* depends on having a CPU copy
 * of the texture's mip-0 to re-upload from:
 *
 *   - Source-backed textures (a retained RGBA8 mip-0): an effective top-mip
 *     change destroys and recreates the GPU texture at the smaller size, so
 *     VRAM is genuinely reclaimed and jce_texture_get_size /
 *     jce_texture_get_resident_top_mip report the new, smaller resident level.
 *
 *   - Default textures (NO retained copy): to honour the low-memory baseline
 *     (single-core, 512 MB, no discrete GPU) the engine does NOT keep a
 *     redundant CPU mirror of every texture.  For these, a bias/residency
 *     request is recorded as intent but the resident GPU texture is left
 *     unchanged — and crucially the queries keep reporting the TRUE resident
 *     level/size.  They never claim a shrink that did not happen (audit F29;
 *     the previous build silently advanced the reported size while leaving the
 *     GPU at full resolution, corrupting size-derived math and the streaming
 *     memory accounting).
 *
 * Roadmap: full streaming for default textures will arrive with an asset
 * format that stores the explicit mip chain on disk, so high mips can be
 * streamed in/out from the PAK on demand instead of from a RAM mirror —
 * keeping the reclaim path zero-extra-RAM, as the baseline requires.
 *
 * Per-texture mip bias.  0 = full quality, +1 = drop top mip, +2 = drop two
 * top mips, etc.  Negative values are clamped to 0.
 */
JCE_API void   jce_texture_set_mip_bias(JceTextureId tex, int8_t bias);
JCE_API int8_t jce_texture_get_mip_bias(JceTextureId tex);

/*
 * Request a given top-mip residency (e.g. top_mip = 0 to upload the full
 * chain).  Honored on the next streaming tick; may be denied / clamped
 * upward under HARD streaming pressure or by the caps floor.  See the
 * CONTRACT note above: only source-backed textures are physically resized.
 */
JCE_API void    jce_texture_request_mip_residency(JceTextureId tex, uint8_t top_mip);
JCE_API uint8_t jce_texture_get_resident_top_mip(JceTextureId tex);

/*
 * Global mip bias.  Added on top of per-texture bias and the caps floor.
 * Driven automatically by the streaming pressure hook
 * (OK -> 0, SOFT -> 1, HARD -> 2) but also callable directly by tools
 * (editor quality slider, headless tests, etc.).  Affects source-backed
 * textures immediately; a truthful no-op for the rest (see CONTRACT above).
 */
JCE_API void   jce_texture_set_global_mip_bias(int8_t bias);
JCE_API int8_t jce_texture_get_global_mip_bias(void);

/*
 * Arm/disarm "streaming uploads".  While armed, NEW uncompressed-RGBA8 texture
 * uploads (the streamed-texture path: PAK PNG/JPG and cooked RGBA8) retain a CPU
 * mip-0 copy AND opt into streaming_tracked, so the streaming-pressure global
 * mip-bias hook can PHYSICALLY shrink them under memory pressure (without a
 * retained source the demote is a truthful no-op — see the CONTRACT above).
 * Off by default so editor/UI/one-off textures pay zero extra RAM.  The runtime
 * scene renderer arms it around streamed model/texture uploads.  Render-thread
 * only (uploads run there).
 */
JCE_API void   jce_texture_set_streaming_uploads(bool on);

JCE_EXTERN_C_END

#endif /* JCE_TEXTURE_H */
