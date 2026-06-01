/* jce_archive_format.h
 *
 * On-disk binary layout for the JCE Archive format (JPAK, format_version 1).
 *
 * This is the richer parallel archive format defined by the Archive &
 * Compression specification.  It coexists with the legacy PAK v2 format
 * (jce_pak_format.h): both share the "JPAK" magic but are disambiguated by
 * the version field — the v2 loader rejects version != 2, this loader
 * rejects version != 1, so neither ever mis-reads the other's files.
 *
 * Fields are little-endian and are read/written at the explicit byte
 * offsets below; structs are NEVER serialized directly (padding differs
 * across compilers/platforms — spec §10.2).  Use the read/write helpers.
 *
 * Layout (spec §3):
 *   [Header]            64 bytes
 *   [Dictionary table]  dict_count * 16 bytes
 *   [Data region]       aligned compressed/stored bytes
 *   [Index region]      entry_count * 32 bytes, sorted by path_hash
 *   [Debug path table]  optional, present iff HAS_DEBUG_PATHS
 *
 * Internal header shared by the reader, the writer, and host tooling.
 * Pure C99, no external dependencies.
 */
#ifndef JCE_ARCHIVE_FORMAT_H
#define JCE_ARCHIVE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/* -- Magic & version ------------------------------------------------ */

#define JARC_MAGIC_0 'J'
#define JARC_MAGIC_1 'P'
#define JARC_MAGIC_2 'A'
#define JARC_MAGIC_3 'K'

#define JARC_FORMAT_VERSION 1u

/* -- Fixed sizes ---------------------------------------------------- */

#define JARC_HEADER_SIZE      64u
#define JARC_DICT_ENTRY_SIZE  16u
#define JARC_INDEX_ENTRY_SIZE 32u

/* -- Header field offsets (spec §4.2) ------------------------------- */

#define JARC_OFF_MAGIC                0u  /* 4  bytes */
#define JARC_OFF_FORMAT_VERSION       4u  /* u32 */
#define JARC_OFF_FLAGS                8u  /* u32 */
#define JARC_OFF_ENTRY_COUNT         12u  /* u32 */
#define JARC_OFF_INDEX_OFFSET        16u  /* u64 */
#define JARC_OFF_INDEX_STORED_SIZE   24u  /* u64 */
#define JARC_OFF_INDEX_ORIGINAL_SIZE 32u  /* u64 */
#define JARC_OFF_DATA_CONTENT_HASH   40u  /* u64 */
#define JARC_OFF_INDEX_CONTENT_HASH  48u  /* u64 */
#define JARC_OFF_DICT_COUNT          56u  /* u16 */
#define JARC_OFF_DEFAULT_COMPRESSION 58u  /* u8  */
#define JARC_OFF_ALIGNMENT_LOG2      59u  /* u8  */
#define JARC_OFF_RESERVED            60u  /* u32 */

/* -- Dictionary table entry field offsets (spec §4.4) --------------- */

#define JARC_DOFF_DATA_OFFSET 0u  /* u64 */
#define JARC_DOFF_SIZE        8u  /* u32 */
#define JARC_DOFF_TAG        12u  /* u32 */

/* -- Index entry field offsets (spec §4.5) -------------------------- */

#define JARC_EOFF_PATH_HASH     0u  /* u64 */
#define JARC_EOFF_DATA_OFFSET   8u  /* u64 */
#define JARC_EOFF_STORED_SIZE  16u  /* u32 */
#define JARC_EOFF_ORIG_SIZE    20u  /* u32 */
#define JARC_EOFF_COMPRESSION  24u  /* u8  */
#define JARC_EOFF_ENTRY_FLAGS  25u  /* u8  */
#define JARC_EOFF_DICT_ID      26u  /* u16 */
#define JARC_EOFF_CONTENT_CRC  28u  /* u32 */

/* -- Header flags (spec §4.3) --------------------------------------- */

#define JARC_FLAG_INDEX_COMPRESSED (1u << 0)
#define JARC_FLAG_HAS_DEBUG_PATHS  (1u << 1)
#define JARC_FLAG_ENCRYPTED        (1u << 2)
#define JARC_FLAG_MMAP_FRIENDLY    (1u << 3)

/* -- Compression algorithm ids (spec §4.6) -------------------------- */

#define JARC_COMP_NONE      0u
#define JARC_COMP_ZSTD      1u
#define JARC_COMP_ZSTD_DICT 2u
#define JARC_COMP_LZ4       3u

/* -- Per-entry flags (spec §4.7) ------------------------------------ */

#define JARC_ENTRY_PAGE_ALIGNED (1u << 0)
#define JARC_ENTRY_ENCRYPTED    (1u << 1)

/* Sentinel meaning "no dictionary" in an entry's dict_id. */
#define JARC_DICT_ID_NONE 0xFFFFu

/* -- Little-endian read helpers (endian-safe) ----------------------- */

static inline uint16_t jarc_rd16(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8);
}

static inline uint32_t jarc_rd32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0]
         | (uint32_t)b[1] << 8
         | (uint32_t)b[2] << 16
         | (uint32_t)b[3] << 24;
}

static inline uint64_t jarc_rd64(const void *p) {
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

/* -- Little-endian write helpers ------------------------------------ */

static inline void jarc_wr16(void *p, uint16_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void jarc_wr32(void *p, uint32_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void jarc_wr64(void *p, uint64_t v) {
    uint8_t *b = (uint8_t *)p;
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    b[4] = (uint8_t)((v >> 32) & 0xFF);
    b[5] = (uint8_t)((v >> 40) & 0xFF);
    b[6] = (uint8_t)((v >> 48) & 0xFF);
    b[7] = (uint8_t)((v >> 56) & 0xFF);
}

#endif /* JCE_ARCHIVE_FORMAT_H */
