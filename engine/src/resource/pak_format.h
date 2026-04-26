/* pak_format.h
 *
 * Binary format definition for JCE PAK archives (version 2).
 *
 * Shared between the host packer tool (jce_pak) and the runtime
 * loader (pak_loader).  Pure C, no external dependencies.
 *
 * Layout:
 *   [JpakHeader]              32 bytes
 *   [JpakTocEntry  count]    count  56 bytes  (sorted by path_hash)
 *   [names section]           concatenated UTF-8 path strings
 *   [data section]            concatenated stored or ZSTD-compressed frames
 *
 * v2 changes vs v1:
 *   - TOC entry adds `flags` (u32)  STORED bit etc.
 *   - TOC entry adds `content_hash` (u64)  XXH3 of original bytes.
 *   - Entries with FLAG_STORED have compressed_size == original_size and
 *     contain raw uncompressed bytes (no ZSTD frame).
 */
#ifndef JCE_PAK_FORMAT_H
#define JCE_PAK_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* -- Magic & Version ------------------------------------------------ */

#define JPAK_MAGIC_0 'J'
#define JPAK_MAGIC_1 'P'
#define JPAK_MAGIC_2 'A'
#define JPAK_MAGIC_3 'K'

#define JPAK_VERSION 2u

/* -- TOC entry flags ------------------------------------------------ */

/* Bytes are stored uncompressed; loader must skip ZSTD_decompress and
 * memcpy compressed_size == original_size bytes directly. */
#define JPAK_FLAG_STORED 0x00000001u

/* -- On-disk structures (all fields little-endian) ------------------ */

#pragma pack(push, 1)

/* File header  32 bytes. */
typedef struct JpakHeader {
    uint8_t  magic[4];      /* "JPAK"                                */
    uint32_t version;       /* JPAK_VERSION                          */
    uint32_t count;         /* number of TOC entries                 */
    uint32_t flags;         /* reserved, must be 0                   */
    uint64_t toc_offset;    /* byte offset of first JpakTocEntry     */
    uint64_t data_offset;   /* byte offset of compressed data region */
} JpakHeader;

/* Table-of-contents entry  56 bytes (v2).
 * The TOC array is sorted by path_hash for O(log n) binary search. */
typedef struct JpakTocEntry {
    uint64_t path_hash;       /* XXH3_64bits of UTF-8 path           */
    uint32_t name_offset;     /* byte offset into names section      */
    uint32_t name_length;     /* path length in bytes (no NUL)       */
    uint64_t data_offset;     /* byte offset from data section start */
    uint64_t compressed_size; /* on-disk size (ZSTD or raw)          */
    uint64_t original_size;   /* uncompressed file size              */
    uint32_t flags;           /* JPAK_FLAG_*                         */
    uint32_t _pad;            /* keep 64-bit alignment for next field*/
    uint64_t content_hash;    /* XXH3_64bits of original bytes       */
} JpakTocEntry;

#pragma pack(pop)

/* -- Compile-time assertions (C99-portable, works on MSVC/Clang/GCC) ------- */

#define JPAK_HEADER_SIZE    32u
#define JPAK_TOC_ENTRY_SIZE 56u

typedef char jpak_sa_header_size    [(sizeof(JpakHeader)   == JPAK_HEADER_SIZE)    ? 1 : -1];
typedef char jpak_sa_toc_entry_size [(sizeof(JpakTocEntry) == JPAK_TOC_ENTRY_SIZE) ? 1 : -1];

/* -- Little-endian read helpers (endian-safe) ----------------------- */

static inline uint32_t jpak_read_le32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0]
         | (uint32_t)b[1] << 8
         | (uint32_t)b[2] << 16
         | (uint32_t)b[3] << 24;
}

static inline uint64_t jpak_read_le64(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint64_t)b[0]
         | (uint64_t)b[1] << 8
         | (uint64_t)b[2] << 16
         | (uint64_t)b[3] << 24
         | (uint64_t)b[4] << 32
         | (uint64_t)b[5] << 40
         | (uint64_t)b[6] << 48
         | (uint64_t)b[7] << 56;
}

#endif /* JCE_PAK_FORMAT_H */
