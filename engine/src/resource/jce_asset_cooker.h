/*
 * jce_asset_cooker.h  Asset cooking library.
 *
 * Converts raw assets (PNG, OBJ, WAV, etc.) into optimized .jceasset
 * binary containers ready for fast runtime loading.
 *
 * Used by:
 *   - jce_cook CLI tool (build-time batch cooking)
 *   - Editor (import-time asset cooking)
 *
 * Cooking pipeline per type:
 *   TEXTURE: PNG/JPG → decode → RGBA8 pixels → .jceasset (TEX_INFO + TEX_PIXELS)
 *   MESH:    OBJ/FBX → parse → vertices+indices → .jceasset (MESH_INFO + MESH_*)
 *   AUDIO:   WAV/OGG → decode → PCM s16 → .jceasset (AUDIO_INFO + AUDIO_PCM)
 *   RAW:     any → pass-through → .jceasset (RAW chunk)
 *
 * All chunks are optionally ZSTD-compressed.
 *
 * Thread safety: each cook_* function is self-contained and thread-safe.
 */

#ifndef JCE_ASSET_COOKER_H
#define JCE_ASSET_COOKER_H

#include <jce/resource/jce_asset_format.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Target platform enumeration                                         */
/* ================================================================== */

typedef enum JceCookPlatform {
    JCE_COOK_PLATFORM_WINDOWS = 0,
    JCE_COOK_PLATFORM_LINUX   = 1,
    JCE_COOK_PLATFORM_MACOS   = 2,
    JCE_COOK_PLATFORM_ANDROID = 3,
    JCE_COOK_PLATFORM_IOS     = 4,
    JCE_COOK_PLATFORM_WEB     = 5,
    JCE_COOK_PLATFORM_AUTO    = 6,
    JCE_COOK_PLATFORM_COUNT
} JceCookPlatform;

/* ================================================================== */
/* Cook options                                                        */
/* ================================================================== */

typedef struct JceCookOptions {
    /* ZSTD compression level (0=none, 1-19=normal, 20-22=ultra).
       Default: 3. */
    int compression_level;

    /* For textures: force specific GPU format (JCEASSET_TEXFMT_*).
       0 = auto-select based on platform. */
    int texture_format;

    /* For textures: generate mipmaps. Default: false. */
    bool generate_mipmaps;

    /* For textures: max dimension (width or height) cap.
       0 = no cap. Textures exceeding this are downscaled preserving
       aspect ratio. Reduces PAK size and runtime memory. */
    int max_texture_size;

    /* Verbose logging. */
    bool verbose;

    /* Target platform for auto-format selection. Default: AUTO. */
    JceCookPlatform platform;

    /* Force power-of-two dimensions (pad if needed). Default: false. */
    bool force_power_of_two;

    /* Block-encode quality / speed tradeoff for BC/ASTC textures:
       0 = DEFAULT (squish cluster-fit — best quality, slowest; the cost
                    behind multi-second 2K texture cooks),
       1 = FAST    (squish range-fit — ~5-7x faster, modest quality drop;
                    the build-bundles default so iteration stays snappy),
       2 = HIGHEST (iterative cluster-fit — slowest, for a final ship pass).
       0 keeps the historical behaviour, so existing callers are unaffected. */
    int encode_quality;
} JceCookOptions;

#define JCE_COOK_ENCODE_DEFAULT 0
#define JCE_COOK_ENCODE_FAST    1
#define JCE_COOK_ENCODE_HIGHEST 2

#define JCE_COOK_DEFAULT                                                                           \
    ((JceCookOptions){.compression_level = 3, .platform = JCE_COOK_PLATFORM_AUTO})

/* ================================================================== */
/* Cook result                                                         */
/* ================================================================== */

typedef struct JceCookResult {
    void  *data;        /* malloc'd .jceasset blob */
    size_t size;        /* total size in bytes */
    bool   success;
    char   error[256];  /* error message if !success */
} JceCookResult;

/* Free the result data. */
void jce_cook_result_free(JceCookResult *result);

/* ================================================================== */
/* Cook functions (one per asset type)                                 */
/* ================================================================== */

/*
 * Cook a texture from raw image data in memory.
 * input: PNG/JPG/BMP/TGA encoded bytes.
 * output: .jceasset with TEX_INFO + TEX_PIXELS chunks.
 */
JceCookResult jce_cook_texture(const void *input, size_t input_size,
                               const JceCookOptions *opts);

/*
 * Cook audio from raw encoded data in memory.
 * input: WAV/OGG/FLAC encoded bytes.
 * output: .jceasset with AUDIO_INFO + AUDIO_PCM chunks.
 */
JceCookResult jce_cook_audio(const void *input, size_t input_size,
                             const JceCookOptions *opts);

/*
 * Cook a raw binary blob (pass-through with optional compression).
 * output: .jceasset with single RAW chunk.
 */
JceCookResult jce_cook_raw(const void *input, size_t input_size,
                           const JceCookOptions *opts);

/*
 * Cook from file path (auto-detects type from extension).
 * Reads file, dispatches to type-specific cooker.
 */
JceCookResult jce_cook_file(const char *input_path,
                            const JceCookOptions *opts);

/* ================================================================== */
/* Utility                                                             */
/* ================================================================== */

/* Detect asset type from file extension. Returns -1 if unknown. */
int jce_cook_detect_type(const char *path);

/* Write a cook result to disk. Returns true on success. */
bool jce_cook_write(const JceCookResult *result, const char *output_path);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_COOKER_H */
