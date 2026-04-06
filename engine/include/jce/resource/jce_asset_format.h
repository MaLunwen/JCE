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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
#define JCEASSET_CHUNK_TEX_GPU    0x0101  /* GPU-compressed (BC/ASTC/KTX2) */
#define JCEASSET_CHUNK_TEX_INFO   0x0102  /* JceAssetTexInfo struct */

/* Mesh chunks */
#define JCEASSET_CHUNK_MESH_VERTICES  0x0200  /* vertex buffer */
#define JCEASSET_CHUNK_MESH_INDICES   0x0201  /* index buffer */
#define JCEASSET_CHUNK_MESH_INFO      0x0202  /* JceAssetMeshInfo struct */

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
	uint32_t format;           /* 0=RGBA8, future: BC7, ASTC, etc. */
	uint32_t mip_count;        /* 1 = no mipmaps */
	uint32_t flags;            /* bit 0: sRGB, bit 1: premultiplied alpha */
	uint32_t _pad;
} JceAssetTexInfo;

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

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_FORMAT_H */
