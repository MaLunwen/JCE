/*
 * jce_asset_registry.h  Hash-map registry for asset path → slot lookup.
 *
 * Open-addressing hash table with linear probing.
 * Capacity auto-grows by 2× when load factor exceeds 70%.
 * Supports >5000 assets with O(1) average lookup.
 *
 * Internal module — not part of the public API.
 */

#ifndef JCE_ASSET_REGISTRY_H
#define JCE_ASSET_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registry entry stored in the hash map. */
typedef struct JceRegistryEntry {
	uint64_t path_hash;      /* XXH3_64bits of virtual path */
	uint16_t slot_index;     /* index into asset slot array */
	bool     occupied;       /* true if this bucket is in use */
} JceRegistryEntry;

/* Open-addressing hash table. */
typedef struct JceAssetRegistry {
	JceRegistryEntry *buckets;
	uint32_t          capacity;     /* always power of 2 */
	uint32_t          count;        /* number of active entries */
} JceAssetRegistry;

/* Create registry with initial capacity (rounded up to power of 2).
   init_capacity 0 → default (256). */
bool jce_registry_create(JceAssetRegistry *reg, uint32_t init_capacity);

/* Destroy and free bucket memory. */
void jce_registry_destroy(JceAssetRegistry *reg);

/* Insert or find existing entry.
   Returns slot_index if path_hash already exists.
   Returns UINT16_MAX if insertion fails (table full and grow failed). */
uint16_t jce_registry_insert(JceAssetRegistry *reg,
                             uint64_t path_hash,
                             uint16_t slot_index);

/* Look up slot index by path hash.
   Returns UINT16_MAX if not found. */
uint16_t jce_registry_find(const JceAssetRegistry *reg, uint64_t path_hash);

/* Remove entry by path hash. Returns true if found and removed. */
bool jce_registry_remove(JceAssetRegistry *reg, uint64_t path_hash);

/* Clear all entries without freeing bucket memory. */
void jce_registry_clear(JceAssetRegistry *reg);

#ifdef __cplusplus
}
#endif

#endif /* JCE_ASSET_REGISTRY_H */
