#include "stock_payload.h"
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_archive.h>
#include <string.h>
static const uint8_t stock_magic[16] = "JCE_STOCK_APP_V2";
static uint16_t read16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}
static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t read64(const uint8_t *p)
{
    return (uint64_t)read32(p) | (uint64_t)read32(p + 4) << 32;
}
static bool stock_pe(const uint8_t *p, size_t size)
{
    uint32_t offset;
    if (size < 64 || p[0] != 'M' || p[1] != 'Z') return false;
    offset = read32(p + 60);
    if (offset > size || size - offset < 94) return false;
    /* PE32+, AMD64, GUI subsystem. No platform headers or unaligned loads. */
    return memcmp(p + offset, "PE\0\0", 4) == 0 &&
        p[offset + 4] == 0x64 && p[offset + 5] == 0x86 &&
        read16(p + offset + 20) >= 70 &&
        read16(p + offset + 20) <= size - offset - 24 &&
        p[offset + 24] == 0x0b &&
        p[offset + 25] == 0x02 && p[offset + 92] == 2 &&
        p[offset + 93] == 0;
}
bool jce_stock_payload_read(const void *image, size_t size, JceStockPayload *out)
{
    const uint8_t *bytes = (const uint8_t *)image;
    const uint8_t *footer;
    uint64_t host, bundle;
    if (!out) return false;
    out->archive = NULL;
    out->archive_size = 0;
    if (!bytes || size < JCE_STOCK_FOOTER_SIZE ||
        (uint64_t)size > JCE_STOCK_IMAGE_LIMIT) return false;
    footer = bytes + size - JCE_STOCK_FOOTER_SIZE;
    if (memcmp(footer, stock_magic, sizeof(stock_magic)) != 0 ||
        read32(footer + 16) != 2 ||
        read32(footer + 20) != JCE_STOCK_FOOTER_SIZE ||
        read64(footer + 56) != jce_fnv1a64(footer, 56)) return false;
    host = read64(footer + 24);
    bundle = read64(footer + 32);
    if (host == 0 || host > JCE_STOCK_HOST_LIMIT || bundle == 0 ||
        bundle > JCE_STOCK_ARCHIVE_LIMIT ||
        host > (uint64_t)size - JCE_STOCK_FOOTER_SIZE ||
        bundle != (uint64_t)size - JCE_STOCK_FOOTER_SIZE - host ||
        !stock_pe(bytes, (size_t)host)) return false;
    if (host >= JCE_STOCK_FOOTER_SIZE &&
        (memcmp(bytes + (size_t)host - JCE_STOCK_FOOTER_SIZE,
                stock_magic, sizeof(stock_magic)) == 0 ||
         memcmp(bytes + (size_t)host - JCE_STOCK_FOOTER_SIZE,
                "JCE_STOCK_APP_V1", 16) == 0)) return false;
    if (read64(footer + 40) != jce_fnv1a64(bytes, (size_t)host) ||
        read64(footer + 48) != jce_fnv1a64(bytes + (size_t)host,
                                         (size_t)bundle)) return false;
    out->archive = bytes + (size_t)host;
    out->archive_size = (size_t)bundle;
    return true;
}

/* Fixed-size header budget only; parsing, keyed lookup and cryptography stay
 * with the public archive owner. A one-entry wrapper has one 32-byte index
 * and no dictionaries, so malformed sizes cannot request a giant index first. */
bool jce_stock_payload_decrypt(const JceStockPayload *payload,
                               uint8_t **out, size_t *out_size)
{
    JceArchive *archive = NULL;
    const JceArchiveEntry *entry;
    uint8_t *bytes = NULL;
    const uint8_t *header;
    size_t size;
    bool valid = false;
    if (out) *out = NULL;
    if (out_size) *out_size = 0;
    if (!out || !out_size || !payload || !payload->archive ||
        payload->archive_size < 64 ||
        payload->archive_size > JCE_STOCK_ARCHIVE_LIMIT) return false;
    header = payload->archive;
    if (memcmp(header, "JPAK", 4) != 0 || read32(header + 4) != 1 ||
        read32(header + 12) != 1 || read64(header + 32) != 32 ||
        read16(header + 56) != 0) return false;
    archive = jce_archive_open(header, payload->archive_size);
    if (!archive || !jce_archive_is_secure(archive) ||
        !jce_archive_is_authenticated(archive) ||
        jce_archive_auth_status(archive) != JCE_ARCHIVE_AUTH_VALID ||
        !jce_archive_verify_header(archive)) goto done;
    if (jce_archive_count(archive) != 1) goto done;
    entry = jce_archive_find(archive, JCE_STOCK_BUNDLE_PATH);
    if (!entry || (entry->entry_flags &
            (JCE_ARCHIVE_ENTRY_ENCRYPTED | JCE_ARCHIVE_ENTRY_AUTHENTICATED)) !=
            (JCE_ARCHIVE_ENTRY_ENCRYPTED | JCE_ARCHIVE_ENTRY_AUTHENTICATED) ||
        entry->original_size == 0 ||
        entry->original_size > JCE_STOCK_BUNDLE_LIMIT) goto done;
    size = (size_t)entry->original_size;
    bytes = (uint8_t *)jce_malloc(size);
    if (!bytes || jce_archive_read(archive, entry, bytes, size) != size ||
        !jce_archive_verify_entry(entry, bytes, size)) goto done;
    *out = bytes;
    *out_size = size;
    bytes = NULL;
    valid = true;
done:
    jce_free(bytes);
    jce_archive_close(archive);
    return valid;
}
