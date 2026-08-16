/*
 * jce_asset.h  Unified asset system — public API.
 *
 * Provides:
 *   - Generational handles (32-bit, FFI-safe)
 *   - Centralized asset manager with reference counting
 *   - Async loading via internal thread pool
 *   - Hash-map registry for O(1) lookup (supports >5000 assets)
 *   - Type-safe accessors for all engine resource types
 *
 * The manager owns all loaded assets; consumers hold handles.
 * Handles detect use-after-free via generation counters.
 *
 * Layer: Resource (Layer 2).
 */

#ifndef JCE_ASSET_H
#define JCE_ASSET_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Generational Handle                                                 */
/* ================================================================== */

/*
 * 32-bit handle: bits [15:0] = slot index, bits [31:16] = generation.
 *
 * Properties:
 *   - Fits in a single register on all platforms (32-bit ARM included)
 *   - Trivially bindable from any language (just a uint32_t)
 *   - Generation wraps after 65535 reuses per slot — safe for games
 *   - Index 0xFFFF reserved as invalid sentinel
 *   - O(1) lookup: slots[handle.index]
 */
typedef struct JceAssetHandle {
    uint16_t index;
    uint16_t generation;
} JceAssetHandle;

#define JCE_ASSET_HANDLE_INVALID ((JceAssetHandle){ 0xFFFF, 0 })

static inline bool jce_asset_handle_valid(JceAssetHandle h)
{
    return h.index != 0xFFFF;
}

static inline bool jce_asset_handle_eq(JceAssetHandle a, JceAssetHandle b)
{
    return a.index == b.index && a.generation == b.generation;
}

/* Pack/unpack for FFI: treat handle as opaque uint32_t. */
static inline uint32_t jce_asset_handle_pack(JceAssetHandle h)
{
    return ((uint32_t)h.generation << 16) | (uint32_t)h.index;
}

static inline JceAssetHandle jce_asset_handle_unpack(uint32_t packed)
{
    JceAssetHandle h;
    h.index      = (uint16_t)(packed & 0xFFFF);
    h.generation = (uint16_t)(packed >> 16);
    return h;
}

/* ================================================================== */
/* Asset types                                                         */
/* ================================================================== */

typedef enum JceAssetType {
    JCE_ASSET_TEXTURE    = 0,
    JCE_ASSET_MESH       = 1,
    JCE_ASSET_SOUND      = 2,
    JCE_ASSET_FONT       = 3,
    JCE_ASSET_SHADER     = 4,
    JCE_ASSET_MATERIAL   = 5,
    JCE_ASSET_MODEL      = 6,   /* composite glTF model */
    JCE_ASSET_ANIMATION  = 7,
    JCE_ASSET_SCENE      = 8,
    JCE_ASSET_RAW        = 9,   /* opaque binary blob */

    JCE_ASSET_TYPE_COUNT
} JceAssetType;

/* ================================================================== */
/* Asset loading state                                                 */
/* ================================================================== */

typedef enum JceAssetState {
    JCE_ASSET_STATE_UNLOADED  = 0,
    JCE_ASSET_STATE_QUEUED    = 1,  /* waiting in async queue */
    JCE_ASSET_STATE_LOADING   = 2,  /* worker thread active */
    JCE_ASSET_STATE_LOADED    = 3,  /* CPU-side ready, awaiting finalize */
    JCE_ASSET_STATE_READY     = 4,  /* GPU-uploaded, fully usable */
    JCE_ASSET_STATE_FAILED    = 5
} JceAssetState;

/* ================================================================== */
/* Asset load parameters                                               */
/* ================================================================== */

/* Optional extra parameters for loading specific asset types. */
typedef struct JceAssetLoadParams {
    /* Texture sampler mode: 0=default(clamp), JCE_TEX_WRAP, JCE_TEX_MIRROR */
    int texture_sampler_mode;

    /* Font point size (only for JCE_ASSET_FONT). */
    float font_size;

    /* Optional codepoints array for font atlas (NULL = ASCII only). */
    const uint32_t *font_codepoints;
    int             font_codepoint_count;

    /* Force synchronous load even when async is available. */
    bool sync;
} JceAssetLoadParams;

#define JCE_ASSET_LOAD_DEFAULT ((JceAssetLoadParams){0})

/* ================================================================== */
/* Manager — opaque handle                                             */
/* ================================================================== */

typedef struct JceAssetManager JceAssetManager;

/* Forward declarations for subsystems. */
typedef struct JcePakArchive   JcePakArchive;
typedef struct JceFileSystem JceFileSystem;
typedef struct JceAudio     JceAudio;

/* ================================================================== */
/* Manager configuration                                               */
/* ================================================================== */

typedef struct JceAssetManagerConfig {
    /* Maximum number of concurrent assets (determines slot pool size).
       0 = default (8192). Must be <= 65534 (0xFFFE). */
    uint32_t max_assets;

    /* Number of async worker threads. 0 = default (2).
       Workers handle decompression + decoding off main thread. */
    uint32_t worker_threads;

    /* PAK archive for reading cooked assets (required). */
    JcePakArchive *pak;

    /* Optional VFS for loose-file override (developer mode). */
    JceFileSystem *fs;

    /* Optional audio subsystem for sound asset loading. */
    JceAudio *audio;
} JceAssetManagerConfig;

/* ================================================================== */
/* Manager lifecycle                                                   */
/* ================================================================== */

/* Create the asset manager. Returns NULL on failure. */
JCE_API JceAssetManager *jce_asset_manager_create(const JceAssetManagerConfig *cfg);

/* Destroy the manager and release all loaded assets. */
JCE_API void jce_asset_manager_destroy(JceAssetManager *mgr);

/* ================================================================== */
/* Loading & Unloading                                                 */
/* ================================================================== */

/*
 * Acquire an asset by virtual path.
 *
 * If already loaded, bumps reference count and returns existing handle.
 * Otherwise, loads from PAK (or VFS if mounted).
 *
 * Default is async: returns handle immediately in QUEUED state.
 * Use jce_asset_state() to poll, or set params.sync = true.
 *
 * Returns JCE_ASSET_HANDLE_INVALID on failure.
 */
JCE_API JceAssetHandle jce_asset_acquire(JceAssetManager *mgr,
                                 const char *asset_path,
                                 JceAssetType type,
                                 const JceAssetLoadParams *params);

/* Convenience: synchronous acquire with default params. */
JCE_API JceAssetHandle jce_asset_load(JceAssetManager *mgr,
                              const char *asset_path,
                              JceAssetType type);

/* Release an asset (decrement ref count; freed at zero). */
JCE_API void jce_asset_release(JceAssetManager *mgr, JceAssetHandle handle);

/* Force reload an asset (for hot-reload in editor).
 *
 * Returns the resulting state, which a file-watcher driving this MUST branch
 * on — the outcomes need different responses and a plain success/failure flag
 * cannot express them:
 *
 *   JCE_ASSET_STATE_READY     reloaded; the new payload is live.
 *   JCE_ASSET_STATE_LOADING   DEFERRED, not failed — a load for this slot is
 *                             already in flight.  Retry once it settles;
 *                             treating this as failure abandons an asset that
 *                             was merely busy.
 *   JCE_ASSET_STATE_FAILED    the reload attempt failed AND THE PREVIOUS
 *                             PAYLOAD IS STILL LIVE.  Nothing was lost, so a
 *                             watcher should report and stop retrying rather
 *                             than spin on a file that no longer parses.
 *   JCE_ASSET_STATE_UNLOADED  invalid handle, or the slot has no source path
 *                             (it was never loaded from one) — a caller bug;
 *                             retrying cannot help.
 *
 * This reuses the asset domain's existing state vocabulary on purpose: the
 * distinction that matters here is "busy vs broken vs bad-call", which a bool
 * collapses, and a new bespoke enum would just add a 19th unrelated error
 * type to the ABI (see contracts/language-driver-abi.md §5). */
JCE_API JceAssetState jce_asset_reload(JceAssetManager *mgr,
                                       JceAssetHandle handle);

/* ================================================================== */
/* Query                                                               */
/* ================================================================== */

/* Get the current loading state of an asset. */
JCE_API JceAssetState jce_asset_state(const JceAssetManager *mgr,
                              JceAssetHandle handle);

/* Is the asset fully ready for use? */
static inline bool jce_asset_ready(const JceAssetManager *mgr,
                                   JceAssetHandle handle)
{
    return jce_asset_state(mgr, handle) == JCE_ASSET_STATE_READY;
}

/* Get reference count (0 if invalid/freed). */
JCE_API uint32_t jce_asset_ref_count(const JceAssetManager *mgr,
                             JceAssetHandle handle);

/* Get the asset type stored in a slot. */
JCE_API JceAssetType jce_asset_type(const JceAssetManager *mgr,
                            JceAssetHandle handle);

/* Get total number of loaded assets. */
JCE_API uint32_t jce_asset_count(const JceAssetManager *mgr);

/* ================================================================== */
/* Type-safe data retrieval                                            */
/* ================================================================== */

/* Generic: get raw data pointer. Returns NULL if not ready. */
JCE_API void *jce_asset_data(const JceAssetManager *mgr, JceAssetHandle handle);

/* Forward-declare typed accessors' return types. */
#include <jce/renderer/jce_texture_types.h>

typedef struct JceMesh         JceMesh;
typedef struct JceModel        JceModel;
typedef struct JceFont         JceFont;

/* Texture: returns JCE_TEXTURE_INVALID if not a ready texture. */
JCE_API JceTexture jce_asset_get_texture(const JceAssetManager *mgr,
                                 JceAssetHandle handle);

/* Mesh: returns NULL if not a ready mesh. */
JCE_API JceMesh *jce_asset_get_mesh(const JceAssetManager *mgr,
                            JceAssetHandle handle);

/* Model: returns NULL if not a ready model. */
JCE_API JceModel *jce_asset_get_model(const JceAssetManager *mgr,
                              JceAssetHandle handle);

/* Sound handle — reuse existing audio type definition. */
#include <jce/os/core/jce_asset_types.h>

JCE_API JceSound jce_asset_get_sound(const JceAssetManager *mgr,
                             JceAssetHandle handle);

/* Font: returns NULL if not a ready font. */
JCE_API JceFont *jce_asset_get_font(const JceAssetManager *mgr,
                            JceAssetHandle handle);

/* Raw blob: returns pointer and sets *out_size. NULL if not ready. */
JCE_API const void *jce_asset_get_raw(const JceAssetManager *mgr,
                              JceAssetHandle handle,
                              size_t *out_size);

/* ================================================================== */
/* Frame pump (must call once per frame from main thread)              */
/* ================================================================== */

/*
 * Finalize completed async loads on the main thread.
 *
 * This is where GPU resources are created (textures uploaded to VRAM,
 * vertex/index buffers allocated, etc.).
 *
 * max_finalize_ms: time budget per frame in milliseconds.
 *   0 = finalize all pending. Typical: 2-4 ms.
 *
 * Returns number of assets finalized this frame.
 */
JCE_API uint32_t jce_asset_manager_update(JceAssetManager *mgr,
                                  float max_finalize_ms);

/* ================================================================== */
/* Statistics                                                          */
/* ================================================================== */

typedef struct JceAssetStats {
    uint32_t total_loaded;
    uint32_t total_slots_used;
    uint32_t total_slots_capacity;
    uint32_t pending_loads;
    uint32_t failed_loads;
    uint64_t total_memory_bytes;  /* approximate */
} JceAssetStats;

JCE_API void jce_asset_manager_stats(const JceAssetManager *mgr,
                             JceAssetStats *out);

/* ================================================================== */
/* Load-failure callback                                                */
/* ================================================================== */

/* Reasons an asset load can fail. */
typedef enum JceAssetErrorCode {
    JCE_ASSET_ERR_NOT_FOUND = 1,   /* path not in PAK / VFS */
    JCE_ASSET_ERR_IO,              /* read failed mid-stream */
    JCE_ASSET_ERR_BAD_FORMAT,      /* parser rejected payload */
    JCE_ASSET_ERR_OUT_OF_MEMORY,
    JCE_ASSET_ERR_UNSUPPORTED,     /* no loader registered for type */
    JCE_ASSET_ERR_INTERNAL         /* loader returned false w/o detail */
} JceAssetErrorCode;

typedef struct JceAssetErrorInfo {
    JceAssetHandle    handle;      /* slot whose load failed */
    JceAssetType      type;
    JceAssetErrorCode code;
    const char       *path;        /* asset path (manager-owned, valid for callback duration) */
    const char       *detail;      /* short human message, may be NULL */
} JceAssetErrorInfo;

typedef void (*jce_asset_error_fn)(const JceAssetErrorInfo *info, void *user);

/* Install (or clear with NULL) a global error handler.
   Replaces any previous handler.  Called on the main thread from
   jce_asset_manager_update() for async loads, or inline for sync loads. */
JCE_API void jce_asset_set_error_handler(JceAssetManager *mgr,
                                         jce_asset_error_fn fn,
                                         void *user);

/* ================================================================== */
/* Pluggable loader registration                                       */
/* ================================================================== */

/*
 * External modules can register custom loaders for any asset type.
 * Registered loaders take precedence over the built-in switch.
 *
 * load_fn:
 *   Called on the loading thread (or main thread for sync types).
 *   `data`    — PAK file contents (raw bytes, owned by caller).
 *   `size`    — byte count.
 *   `params`  — optional load params (may be NULL).
 *   `out`     — write the loaded asset pointer here.
 *   `out_mem` — write approximate memory footprint here.
 *   Return true on success.
 *
 * destroy_fn:
 *   Called when the asset is released (ref_count → 0).
 *   `data` — the pointer previously written to *out.
 */
typedef bool (*jce_asset_load_fn)(const void *data, size_t size,
                                  const JceAssetLoadParams *params,
                                  void **out, size_t *out_mem);
typedef void (*jce_asset_destroy_fn)(void *data);

/* Register a loader for a given asset type.
   Returns false if type is out of range or load_fn is NULL. */
JCE_API bool jce_asset_register_loader(JceAssetManager *mgr,
                               JceAssetType type,
                               jce_asset_load_fn load_fn,
                               jce_asset_destroy_fn destroy_fn);

JCE_EXTERN_C_END

#endif /* JCE_ASSET_H */
