/*
 * jce_asset_format.h  .jceasset binary container format definition.
 *
 * Unified container for all cooked asset types.
 * Used by both the cooker tool (writer) and runtime loader (reader).
 *
 * Layout:
 *   [JceAssetFileHeader]    32 bytes — magic, version, type, flags
 *   [JceAssetChunkEntry × N] N × 32 bytes — chunk table
 *   [Data blocks]           concatenated chunk data
 *
 * Chunk types allow a single .jceasset to contain multiple data
 * streams (e.g., mesh vertices + indices + material reference).
 *
 * All multi-byte fields are little-endian.
 * Design: pure C, no external dependencies, FFI-safe.
 */

#ifndef JCE_ASSET_FORMAT_H
#define JCE_ASSET_FORMAT_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Magic & Version                                                     */
/* ================================================================== */

#define JCEASSET_MAGIC_0 'J'
#define JCEASSET_MAGIC_1 'C'
#define JCEASSET_MAGIC_2 'E'
#define JCEASSET_MAGIC_3 'A'

#define JCEASSET_VERSION 1u

/* ================================================================== */
/* Asset type tags (matches JceAssetType enum in jce_asset.h)          */
/* ================================================================== */

#define JCEASSET_TYPE_TEXTURE    0
#define JCEASSET_TYPE_MESH       1
#define JCEASSET_TYPE_SOUND      2
#define JCEASSET_TYPE_FONT       3
#define JCEASSET_TYPE_SHADER     4
#define JCEASSET_TYPE_MATERIAL   5
#define JCEASSET_TYPE_MODEL      6
#define JCEASSET_TYPE_ANIMATION  7
#define JCEASSET_TYPE_SCENE      8
#define JCEASSET_TYPE_RAW        9

/* ================================================================== */
/* Compression tags                                                    */
/* ================================================================== */

#define JCEASSET_COMPRESS_NONE   0   /* uncompressed */
#define JCEASSET_COMPRESS_ZSTD   1   /* ZSTD frame */

/* ================================================================== */
/* Chunk type tags                                                     */
/* ================================================================== */

/* Common */
#define JCEASSET_CHUNK_RAW        0x0000  /* opaque binary blob */
#define JCEASSET_CHUNK_METADATA   0x0001  /* JSON UTF-8 metadata */

/* Texture chunks */
#define JCEASSET_CHUNK_TEX_PIXELS 0x0100  /* raw RGBA8 pixel data */
#define JCEASSET_CHUNK_TEX_GPU    0x0101  /* reserved for future GPU formats */
#define JCEASSET_CHUNK_TEX_INFO   0x0102  /* JceAssetTexInfo struct */
#define JCEASSET_CHUNK_TEX_NUMERIC_INFO 0x0103 /* JceAssetNumericInfo */

/* Mesh chunks */
#define JCEASSET_CHUNK_MESH_VERTICES  0x0200  /* vertex buffer */
#define JCEASSET_CHUNK_MESH_INDICES   0x0201  /* index buffer */
#define JCEASSET_CHUNK_MESH_INFO      0x0202  /* JceAssetMeshInfo struct */
/* Auto-LOD index buffer (additive).  One chunk per generated LOD level; the
 * chunk payload begins with a JceAssetMeshLodHeader followed by the level's
 * index buffer (level_index_count × index_stride bytes).  Backward-compatible:
 * the reader looks chunks up by type (jce_asset_find_chunk) and silently
 * ignores unknown types, so an old reader sees the base LOD0 mesh unchanged. */
#define JCEASSET_CHUNK_MESH_LOD_INDICES 0x0203

/* Audio chunks */
#define JCEASSET_CHUNK_AUDIO_PCM      0x0300  /* decoded PCM s16 */
#define JCEASSET_CHUNK_AUDIO_ENCODED  0x0301  /* original encoded (OGG/WAV) */
#define JCEASSET_CHUNK_AUDIO_INFO     0x0302  /* JceAssetAudioInfo struct */

/* Model chunks */
#define JCEASSET_CHUNK_MODEL_NODES    0x0600  /* node hierarchy */
#define JCEASSET_CHUNK_MODEL_MESHES   0x0601  /* mesh data array */
#define JCEASSET_CHUNK_MODEL_MATERIALS 0x0602 /* PBR material array */
#define JCEASSET_CHUNK_MODEL_SKELETON 0x0603  /* joint hierarchy */
#define JCEASSET_CHUNK_MODEL_ANIMS    0x0604  /* animation clips */

/* ================================================================== */
/* On-disk structures (packed, little-endian)                          */
/* ================================================================== */

#pragma pack(push, 1)

/*
 * File header — 32 bytes.
 */
typedef struct JceAssetFileHeader {
    uint8_t  magic[4];       /* "JCEA"                             */
    uint32_t version;        /* JCEASSET_VERSION                   */
    uint32_t asset_type;     /* JCEASSET_TYPE_* enum               */
    uint32_t chunk_count;    /* number of chunk entries             */
    uint32_t flags;          /* reserved, must be 0                 */
    uint64_t source_hash;    /* XXH3_64 of original source file    */
    uint32_t _pad;           /* padding to 32 bytes                */
} JceAssetFileHeader;

/*
 * Chunk table entry — 32 bytes.
 */
typedef struct JceAssetChunkEntry {
    uint16_t chunk_type;       /* JCEASSET_CHUNK_* tag               */
    uint16_t compression;      /* JCEASSET_COMPRESS_* tag            */
    uint32_t _reserved;        /* must be 0                          */
    uint64_t data_offset;      /* byte offset from start of file     */
    uint64_t compressed_size;  /* on-disk size (= original if none)  */
    uint64_t original_size;    /* uncompressed size                  */
} JceAssetChunkEntry;

/* ================================================================== */
/* Per-type info structs (stored in INFO chunks, uncompressed)         */
/* ================================================================== */

/*
 * Texture info — stored in JCEASSET_CHUNK_TEX_INFO.
 */
typedef struct JceAssetTexInfo {
    uint32_t width;
    uint32_t height;
    uint32_t format;           /* JCEASSET_TEXFMT_* */
    uint32_t mip_count;        /* 1 = no mipmaps */
    uint32_t flags;            /* bit 0: sRGB, bit 1: premultiplied alpha */
    uint32_t _pad;
} JceAssetTexInfo;

/* GPU texture formats for JceAssetTexInfo::format.
 * Block-compressed formats store ceil(w/4)*ceil(h/4) blocks per mip; the GPU
 * (bgfx) consumes them directly with no runtime decode. Desktop uses BC; mobile
 * uses ASTC/ETC2. The runtime maps these to bgfx formats in jce_texture.c. */
#define JCEASSET_TEXFMT_RGBA8       0   /* Uncompressed RGBA 8-bit          */
#define JCEASSET_TEXFMT_RGB8        1   /* Uncompressed RGB 8-bit           */
#define JCEASSET_TEXFMT_BC1         2   /* DXT1  — opaque RGB, 0.5 byte/px   */
#define JCEASSET_TEXFMT_BC5         3   /* RG    — tangent-space normal maps */
#define JCEASSET_TEXFMT_BC7         4   /* RGBA  — high-quality color, 1 b/px*/
#define JCEASSET_TEXFMT_ASTC_4x4    5   /* mobile RGBA, 1 byte/px            */
#define JCEASSET_TEXFMT_ETC2_RGBA8  6   /* mobile RGBA fallback             */
#define JCEASSET_TEXFMT_BC3         7   /* DXT5 — RGBA color, 1 byte/px      */
#define JCEASSET_TEXFMT_R16F        8   /* one IEEE-754 binary16 channel     */
#define JCEASSET_TEXFMT_RG16F       9   /* two IEEE-754 binary16 channels    */
#define JCEASSET_TEXFMT_RGBA16F    10   /* four IEEE-754 binary16 channels   */
#define JCEASSET_TEXFMT_R32F       11   /* one IEEE-754 binary32 channel     */
#define JCEASSET_TEXFMT_RG32F      12   /* two IEEE-754 binary32 channels    */
#define JCEASSET_TEXFMT_RGBA32F    13   /* four IEEE-754 binary32 channels   */

#define JCEASSET_NUMERIC_FLAG_ALLOW_NONFINITE 0x00000001u

typedef struct JceAssetNumericInfo {
    uint32_t struct_size;
    uint32_t row_pitch;
    uint32_t channel_count;
    uint32_t sampler_address;
    uint32_t sampler_filter;
    uint32_t color_space;
    uint32_t flags;
    uint32_t _reserved;
    uint64_t decoded_hash;
} JceAssetNumericInfo;

/* Platform target IDs for the jce_pak --platform flag. */
#define JCEASSET_PLATFORM_DESKTOP   0
#define JCEASSET_PLATFORM_MOBILE    1
#define JCEASSET_PLATFORM_WEB       2
#define JCEASSET_PLATFORM_CONSOLE   3

/*
 * Mesh info — stored in JCEASSET_CHUNK_MESH_INFO.
 */
typedef struct JceAssetMeshInfo {
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t vertex_stride;    /* bytes per vertex */
    uint32_t index_stride;     /* 2 (uint16) or 4 (uint32) */
    uint32_t vertex_format;    /* enum: 0=pos+normal+uv, 1=+tangent, 2=+joints */
    uint32_t _pad;
} JceAssetMeshInfo;

/*
 * Auto-LOD header — prefixes every JCEASSET_CHUNK_MESH_LOD_INDICES chunk.
 * The simplified index buffer for the level follows immediately after this
 * struct in the same chunk (index_count × index_stride bytes).  LOD0 is the
 * base mesh (stored in JCEASSET_CHUNK_MESH_INDICES); LOD chunks carry levels
 * 1..N, each referencing the SAME base vertex buffer.
 */
typedef struct JceAssetMeshLodHeader {
    uint32_t lod_level;        /* 1-based LOD index (1 = first reduced level) */
    uint32_t index_count;      /* number of indices in this level's buffer   */
    uint32_t index_stride;     /* 2 (uint16) or 4 (uint32) — matches base    */
    uint32_t source_mesh;      /* index of the base mesh this LOD belongs to */
} JceAssetMeshLodHeader;

/*
 * Audio info — stored in JCEASSET_CHUNK_AUDIO_INFO.
 */
typedef struct JceAssetAudioInfo {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t total_frames;
    uint32_t format;           /* 0=PCM_S16, future: float32, etc. */
    uint32_t _pad;
} JceAssetAudioInfo;

#pragma pack(pop)

/* ================================================================== */
/* Compile-time assertions                                             */
/* ================================================================== */

#define JCEASSET_HEADER_SIZE      32u
#define JCEASSET_CHUNK_ENTRY_SIZE 32u

typedef char JceAssetHeaderSizeMustBe32[
    sizeof(JceAssetFileHeader) == JCEASSET_HEADER_SIZE ? 1 : -1];
typedef char JceAssetChunkEntrySizeMustBe32[
    sizeof(JceAssetChunkEntry) == JCEASSET_CHUNK_ENTRY_SIZE ? 1 : -1];
typedef char JceAssetNumericInfoSizeMustBe40[
    sizeof(JceAssetNumericInfo) == 40u ? 1 : -1];

/* ================================================================== */
/* Source-extension classification (single authority)                  */
/* ================================================================== */

/*
 * Classify a SOURCE asset by its file extension.
 *
 * This is the one authoritative extension -> asset-kind table in the
 * engine.  Before it existed, the cooker, the runtime texture whitelist,
 * the editor asset database, the editor texture cache and the asset-browser
 * thumbnailer each carried their own list, and they disagreed: a .webp was
 * a texture to the asset browser but cooked as an opaque RAW blob, .gif and
 * .ktx2 were textures to some tables and unknown to others, and .dae/.stl/
 * .usd were models in the browser but never routed to the model importer.
 * The browser would label a file "Texture"/"Model" while the cooker packed
 * it as RAW, so it failed to load at runtime with no obvious cause.
 *
 * `path` may be a full path or a bare extension, with or without the dot.
 * Matching is case-insensitive.  Returns one of the JCEASSET_TYPE_* tags,
 * or JCEASSET_TYPE_RAW when the extension is unknown.
 *
 * NOTE — this answers "what kind of file is this?", NOT "can this build
 * step handle it?".  A consumer that supports only a subset (e.g. the
 * cooker only encodes the formats it has an importer for) must apply its
 * own explicit capability check on top; it must not narrow this table.
 */
JCE_API int jce_asset_type_from_ext(const char *path);

/* Convenience predicate for the very common texture test. */
JCE_API bool jce_asset_ext_is_texture(const char *path);

/* ================================================================== */
/* Script-source classification (single authority)                     */
/* ================================================================== */

/*
 * WHAT FORM a script file is in.  This is the only thing about a script
 * that changes how the pipeline TREATS the bytes, which is why it is an
 * enum and the language is a string.
 */
#define JCEASSET_SCRIPT_FORM_NONE      0 /* not a script file at all      */
#define JCEASSET_SCRIPT_FORM_SOURCE    1 /* UTF-8 text a VM compiles      */
#define JCEASSET_SCRIPT_FORM_BYTECODE  2 /* already-compiled binary blob  */
/*
 * REFERENCE — the path NAMES the script, it does not CONTAIN it.  The code
 * was compiled before the process started and lives in a native module; what
 * the extension identifies is a class inside that module, and the bytes at
 * the path (if a file exists there at all) are never executed.
 *
 * It is a third value rather than a reuse of SOURCE because the one consumer
 * that branches on `form` is the archive cooker's compression class
 * (engine/src/resource/jce_archive_cook.c), whose SOURCE arm means "put these
 * bytes in the shared TEXT dictionary because a VM will compile them".  For a
 * reference that sentence is false in both halves, and a form that lied about
 * it would be a comment nothing enforces wearing an enum's clothes.
 * *Enforced by:* tests/os/resource/test_jce_asset_ext.c ::
 * test_a_reference_form_script_is_neither_source_nor_bytecode.
 */
#define JCEASSET_SCRIPT_FORM_REFERENCE 3 /* the path names code elsewhere */

/*
 * The scripting LANGUAGE whose files carry this extension, or NULL.
 *
 * This is the one authoritative script-extension table in the engine, and
 * it exists for the same reason as jce_asset_type_from_ext() above: before
 * it, ".lua" was spelled into the bundle contract twice, into the archive
 * compressor's text class once, and into the editor's asset database once
 * more — so a .py attached to an entity was labelled "binary" in the
 * bundle manifest and dropped outright by the editor's publication policy,
 * a failure that only appears in a PACKAGED build because the editor keeps
 * loading it from loose files.
 *
 * WHAT THIS IS NOT: it is not "which VM can run this".  That question is
 * answered per-process by the VM registry
 * (jce_script_vm_language_for_path, <jce/middleware/script/jce_script_vm.h>)
 * and its answer depends on which backends this executable linked.  The two
 * are deliberately different:
 *
 *   - this table must NOT depend on build options, because an offline
 *     cooker with no Python linked still has to pack turret.py into the
 *     bundle that a Python-enabled runtime will load;
 *   - the registry must NOT be a static list, because a backend claims its
 *     own extension from its own register() and no engine file learns
 *     about a sixth language.
 *
 * A consumer that needs "can this build actually run it" asks the registry
 * ON TOP of this call and reports the two conditions separately — an
 * unlinked backend and an unknown language have different fixes.
 * *Was enforced by* (no longer checked — tools/audit/ was removed):
 * check_script_language_catalog.py, which fails
 * when a backend under scripting/ claims an extension this table does not
 * know, or claims it for a different language.
 *
 * `path` may be a full path or a bare extension, with or without the dot;
 * matching is case-insensitive.  The returned pointer is a string literal.
 */
JCE_API const char *jce_asset_script_language_from_ext(const char *path);

/*
 * The payload-contract token a file with this extension carries in a bundle
 * manifest's JCE_BUNDLE_KEY_REPRESENTATION field ("lua.source",
 * "python.source", "java.class", ...), or NULL when it is not a script.
 * Descriptive only — see the note on jce_asset_script_form_from_ext() for
 * the part of this that is actually load-bearing.
 */
JCE_API const char *jce_asset_script_representation_from_ext(const char *path);

/*
 * JCEASSET_SCRIPT_FORM_* for this extension, JCEASSET_SCRIPT_FORM_NONE when
 * it is not a script.  The archive cooker keys its compression class off
 * this: SOURCE joins the shared text dictionary, BYTECODE does not.
 */
JCE_API int jce_asset_script_form_from_ext(const char *path);

JCE_EXTERN_C_END

#endif /* JCE_ASSET_FORMAT_H */
