/*
 * jce_asset_bundle.c  Bundle reader + writer.
 *
 * Reader keeps the file handle open across calls so seek-reads are
 * cheap.  Writer is a single-shot helper used by the asset cooker.
 *
 * CRC32 is a simple table-driven (poly 0xEDB88320); used for entry-
 * integrity check on read.  Not authenticated — sign at a higher
 * layer if tamper-resistance matters.
 */

#include <jce/resource/jce_asset_bundle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct JceAssetBundle {
    FILE                *fp;
    uint16_t             version;
    uint16_t             flags;
    uint32_t             entry_count;
    uint32_t             blob_offset;
    uint32_t             blob_size;
    JceAssetBundleEntry *entries;
};

#define MAGIC "JCBN"

static uint32_t crc32_table[256];
static bool      crc32_table_init;

static void init_crc32(void)
{
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c >> 1) ^ (0xEDB88320u & -(int32_t)(c & 1));
        crc32_table[i] = c;
    }
    crc32_table_init = true;
}

static uint32_t crc32(const void *buf, size_t n)
{
    init_crc32();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < n; ++i)
        c = crc32_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return ~c;
}

/* ── Reader ─────────────────────────────────────────────────── */

JceAssetBundle *jce_asset_bundle_open(const char *path)
{
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    char magic[4];
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, MAGIC, 4) != 0) {
        fclose(fp);
        return NULL;
    }
    uint16_t version, flags;
    uint32_t entry_count, blob_offset, blob_size;
    if (fread(&version,     2, 1, fp) != 1) goto bad;
    if (fread(&flags,       2, 1, fp) != 1) goto bad;
    if (fread(&entry_count, 4, 1, fp) != 1) goto bad;
    if (fread(&blob_offset, 4, 1, fp) != 1) goto bad;
    if (fread(&blob_size,   4, 1, fp) != 1) goto bad;
    if (entry_count > JCE_ASSET_BUNDLE_MAX_ENTRIES) goto bad;

    JceAssetBundle *b = (JceAssetBundle *)calloc(1, sizeof(*b));
    if (!b) goto bad;
    b->fp          = fp;
    b->version     = version;
    b->flags       = flags;
    b->entry_count = entry_count;
    b->blob_offset = blob_offset;
    b->blob_size   = blob_size;
    b->entries     = (JceAssetBundleEntry *)calloc(entry_count,
                       sizeof(JceAssetBundleEntry));
    if (entry_count > 0 && !b->entries) { free(b); goto bad; }

    if (entry_count > 0 &&
        fread(b->entries, sizeof(JceAssetBundleEntry),
              entry_count, fp) != entry_count) {
        free(b->entries); free(b); goto bad;
    }
    return b;
bad:
    fclose(fp);
    return NULL;
}

void jce_asset_bundle_close(JceAssetBundle *b)
{
    if (!b) return;
    if (b->fp) fclose(b->fp);
    free(b->entries);
    free(b);
}

uint32_t jce_asset_bundle_entry_count(const JceAssetBundle *b)
{
    return b ? b->entry_count : 0;
}

const JceAssetBundleEntry *jce_asset_bundle_entry_at(const JceAssetBundle *b,
                                                      uint32_t idx)
{
    if (!b || idx >= b->entry_count) return NULL;
    return &b->entries[idx];
}

const JceAssetBundleEntry *jce_asset_bundle_find(const JceAssetBundle *b,
                                                  const char *name)
{
    if (!b || !name) return NULL;
    for (uint32_t i = 0; i < b->entry_count; ++i)
        if (strncmp(b->entries[i].name, name,
                     JCE_ASSET_BUNDLE_NAME_LEN) == 0)
            return &b->entries[i];
    return NULL;
}

uint32_t jce_asset_bundle_read(JceAssetBundle *b,
                                 const JceAssetBundleEntry *entry,
                                 void *out_buf, uint32_t cap)
{
    if (!b || !entry || !out_buf || cap < entry->size) return 0;
    if (fseek(b->fp, (long)(b->blob_offset + entry->offset), SEEK_SET) != 0)
        return 0;
    size_t n = fread(out_buf, 1, entry->size, b->fp);
    if (n != entry->size) return 0;
    /* Optional CRC check. */
    uint32_t got = crc32(out_buf, entry->size);
    if (got != entry->crc32) {
        /* Caller may still want the data; we just return 0 to signal
         * corruption. */
        return 0;
    }
    return (uint32_t)n;
}

/* ── Writer ─────────────────────────────────────────────────── */

bool jce_asset_bundle_build(const char *out_path,
                              const JceAssetBundleInputAsset *assets,
                              uint32_t asset_count)
{
    if (!out_path || (!assets && asset_count > 0)) return false;
    if (asset_count > JCE_ASSET_BUNDLE_MAX_ENTRIES) return false;
    FILE *fp = fopen(out_path, "wb");
    if (!fp) return false;

    uint32_t header_size = 4 + 2 + 2 + 4 + 4 + 4;
    uint32_t entry_size  = sizeof(JceAssetBundleEntry);
    uint32_t blob_offset = header_size + asset_count * entry_size;

    /* Compute blob total + per-entry offsets. */
    JceAssetBundleEntry *entries =
        (JceAssetBundleEntry *)calloc(asset_count, sizeof(*entries));
    if (asset_count > 0 && !entries) { fclose(fp); return false; }
    uint32_t blob_size = 0;
    for (uint32_t i = 0; i < asset_count; ++i) {
        strncpy(entries[i].name, assets[i].name,
                 JCE_ASSET_BUNDLE_NAME_LEN - 1);
        entries[i].offset            = blob_size;
        entries[i].size              = assets[i].size;
        entries[i].uncompressed_size = assets[i].size;
        entries[i].crc32             = crc32(assets[i].data, assets[i].size);
        blob_size += assets[i].size;
    }

    /* Header. */
    fwrite(MAGIC,        1, 4, fp);
    uint16_t version = 1, flags = 0;
    fwrite(&version,     2, 1, fp);
    fwrite(&flags,       2, 1, fp);
    fwrite(&asset_count, 4, 1, fp);
    fwrite(&blob_offset, 4, 1, fp);
    fwrite(&blob_size,   4, 1, fp);
    /* Entries. */
    if (asset_count > 0)
        fwrite(entries, sizeof(*entries), asset_count, fp);
    /* Blob. */
    for (uint32_t i = 0; i < asset_count; ++i)
        fwrite(assets[i].data, 1, assets[i].size, fp);

    free(entries);
    fclose(fp);
    return true;
}
