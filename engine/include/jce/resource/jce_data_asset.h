/*
 * jce_data_asset.h  Typed data-only asset (Unity ScriptableObject
 * equivalent).
 *
 * A DataAsset is a .dataasset.json file describing one struct of a
 * registered type.  Each registered type has:
 *   - a string `type_name` (must match the JSON file's "type" field)
 *   - a list of typed field descriptors (name, kind, offset)
 *   - sizeof(blob) so the loader can allocate
 *
 * The loader reads the JSON, allocates the blob, and parses each
 * field per the descriptor.  At runtime, game code queries
 * `jce_data_asset_get(path)` and casts the returned blob to its own
 * struct.  No bgfx, no rendering; pure data.
 *
 * Field kinds supported (data-only, no pointers): BOOL, INT, FLOAT,
 * STRING (fixed-size char[N]), VEC3, VEC4, GUID_REF (link to another
 * asset by 128-bit GUID).
 *
 * Layer: resource (Layer 3) — public.
 */

#ifndef JCE_DATA_ASSET_H
#define JCE_DATA_ASSET_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_DATA_ASSET_NAME_LEN   48
#define JCE_DATA_ASSET_PATH_LEN   192
#define JCE_DATA_ASSET_FIELDS_MAX 32
#define JCE_DATA_ASSET_REG_MAX    64
#define JCE_DATA_ASSET_INST_MAX   256

typedef enum {
    JCE_DA_FIELD_BOOL    = 0,
    JCE_DA_FIELD_INT     = 1,
    JCE_DA_FIELD_FLOAT   = 2,
    JCE_DA_FIELD_STRING  = 3,    /* fixed-size char[field_size] inside blob */
    JCE_DA_FIELD_VEC3    = 4,
    JCE_DA_FIELD_VEC4    = 5,
    JCE_DA_FIELD_GUID    = 6,    /* 128-bit asset reference; field_size = 16 */
} JceDataAssetFieldKind;

typedef struct {
    char                  name[JCE_DATA_ASSET_NAME_LEN];
    JceDataAssetFieldKind kind;
    uint32_t              offset;    /* byte offset within blob */
    uint32_t              field_size; /* for STRING kind, capacity in bytes */
} JceDataAssetField;

typedef struct {
    char              type_name[JCE_DATA_ASSET_NAME_LEN];
    uint32_t          blob_size;
    JceDataAssetField fields[JCE_DATA_ASSET_FIELDS_MAX];
    uint32_t          field_count;
    bool              active;
} JceDataAssetType;

typedef struct {
    char     path[JCE_DATA_ASSET_PATH_LEN];
    char     type_name[JCE_DATA_ASSET_NAME_LEN];
    uint8_t  guid[16];
    void    *blob;
    uint32_t blob_size;
    bool     active;
} JceDataAsset;

/* ── Type registry ───────────────────────────────────────────── */

/* Register a data-asset type.  Returns false on overflow or
 * mismatched field-size invariants. */
JCE_API bool  jce_data_asset_register_type(const char              *type_name,
                                             uint32_t                 blob_size,
                                             const JceDataAssetField *fields,
                                             uint32_t                 field_count);

JCE_API const JceDataAssetType *jce_data_asset_find_type(const char *type_name);

/* ── Instance lifecycle ──────────────────────────────────────── */

/* Load `.dataasset.json` from `path`.  Returns the new asset, or
 * NULL on parse error / unknown type. */
JCE_API JceDataAsset *jce_data_asset_load(const char *path);

/* Save asset to disk (re-emits JSON from blob via the type's
 * field descriptors).  Returns true on success. */
JCE_API bool jce_data_asset_save(const JceDataAsset *asset);

/* Free + un-register a loaded asset (next load_by_path reads disk
 * fresh).  Caller should not retain `asset` pointer afterwards. */
JCE_API void jce_data_asset_release(JceDataAsset *asset);

/* Lookup by path (returns NULL if not loaded). */
JCE_API JceDataAsset *jce_data_asset_find_by_path(const char *path);

JCE_API JceDataAsset *jce_data_asset_find_by_guid(const uint8_t guid[16]);

/* Iterator (NULL when out of range). */
JCE_API uint32_t      jce_data_asset_count(void);
JCE_API JceDataAsset *jce_data_asset_at(uint32_t idx);

JCE_EXTERN_C_END

#endif /* JCE_DATA_ASSET_H */
