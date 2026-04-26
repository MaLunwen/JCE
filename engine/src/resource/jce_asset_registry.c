/*
 * jce_asset_registry.c  Open-addressing hash map for asset lookup.
 *
 * Linear probing with power-of-2 capacity.
 * Grows at 70% load factor. Tombstone-free: uses backward-shift
 * deletion for correct probe chain maintenance.
 */

#include "jce_asset_registry.h"
#include "os/core/jce_memory.h"

#include <string.h>

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static uint32_t next_pow2(uint32_t v)
{
	if (v == 0) return 1;
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return v + 1;
}

/* Fibonacci hashing: good distribution for sequential/similar hashes. */
static uint32_t bucket_index(uint64_t path_hash, uint32_t mask)
{
	/* Multiply by golden-ratio constant, take upper bits. */
	uint64_t h = path_hash * UINT64_C(0x9E3779B97F4A7C15);
	return (uint32_t)(h >> 32) & mask;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

bool jce_registry_create(JceAssetRegistry *reg, uint32_t init_capacity)
{
	if (!reg) return false;

	uint32_t cap = (init_capacity > 0) ? next_pow2(init_capacity) : 256;
	if (cap < 16) cap = 16;

	reg->buckets = JCE_NEW_ARRAY(JceRegistryEntry, cap);
	if (!reg->buckets) return false;

	reg->capacity = cap;
	reg->count    = 0;
	return true;
}

void jce_registry_destroy(JceAssetRegistry *reg)
{
	if (!reg) return;
	JCE_FREE(reg->buckets);
	reg->buckets  = NULL;
	reg->capacity = 0;
	reg->count    = 0;
}

/* ================================================================== */
/* Internal: grow table                                                */
/* ================================================================== */

static bool registry_grow(JceAssetRegistry *reg)
{
	uint32_t old_cap = reg->capacity;
	uint32_t new_cap = old_cap * 2;
	if (new_cap < old_cap) return false; /* overflow */

	JceRegistryEntry *old_buckets = reg->buckets;
	JceRegistryEntry *new_buckets = JCE_NEW_ARRAY(JceRegistryEntry, new_cap);
	if (!new_buckets) return false;

	uint32_t new_mask = new_cap - 1;

	/* Re-insert all occupied entries. */
	for (uint32_t i = 0; i < old_cap; i++) {
		if (!old_buckets[i].occupied) continue;

		uint32_t idx = bucket_index(old_buckets[i].path_hash, new_mask);
		while (new_buckets[idx].occupied)
			idx = (idx + 1) & new_mask;

		new_buckets[idx] = old_buckets[i];
	}

	JCE_FREE(old_buckets);
	reg->buckets  = new_buckets;
	reg->capacity = new_cap;
	return true;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

uint16_t jce_registry_insert(JceAssetRegistry *reg,
                             uint64_t path_hash,
                             uint16_t slot_index)
{
	if (!reg || !reg->buckets) return UINT16_MAX;

	uint32_t mask = reg->capacity - 1;
	uint32_t idx  = bucket_index(path_hash, mask);

	/* Probe for existing entry or empty slot. */
	for (uint32_t i = 0; i < reg->capacity; i++) {
		uint32_t probe = (idx + i) & mask;
		JceRegistryEntry *e = &reg->buckets[probe];

		if (!e->occupied) {
			/* Empty slot — insert here. */
			/* Check load factor first (70%). */
			if ((reg->count + 1) * 10 > reg->capacity * 7) {
				if (!registry_grow(reg))
					return UINT16_MAX;
				/* Recursive insert into grown table. */
				return jce_registry_insert(reg, path_hash, slot_index);
			}

			e->path_hash  = path_hash;
			e->slot_index = slot_index;
			e->occupied   = true;
			reg->count++;
			return slot_index;
		}

		if (e->path_hash == path_hash) {
			/* Already exists — return existing slot. */
			return e->slot_index;
		}
	}

	return UINT16_MAX; /* should not reach if load factor is maintained */
}

uint16_t jce_registry_find(const JceAssetRegistry *reg, uint64_t path_hash)
{
	if (!reg || !reg->buckets || reg->count == 0) return UINT16_MAX;

	uint32_t mask = reg->capacity - 1;
	uint32_t idx  = bucket_index(path_hash, mask);

	for (uint32_t i = 0; i < reg->capacity; i++) {
		uint32_t probe = (idx + i) & mask;
		const JceRegistryEntry *e = &reg->buckets[probe];

		if (!e->occupied)
			return UINT16_MAX; /* empty slot = not found */

		if (e->path_hash == path_hash)
			return e->slot_index;
	}

	return UINT16_MAX;
}

bool jce_registry_remove(JceAssetRegistry *reg, uint64_t path_hash)
{
	if (!reg || !reg->buckets || reg->count == 0) return false;

	uint32_t mask = reg->capacity - 1;
	uint32_t idx  = bucket_index(path_hash, mask);

	/* Find the entry. */
	uint32_t found = UINT32_MAX;
	for (uint32_t i = 0; i < reg->capacity; i++) {
		uint32_t probe = (idx + i) & mask;
		JceRegistryEntry *e = &reg->buckets[probe];

		if (!e->occupied)
			return false;

		if (e->path_hash == path_hash) {
			found = probe;
			break;
		}
	}

	if (found == UINT32_MAX) return false;

	/* Backward-shift deletion: move subsequent entries back to fill
	   the gap, maintaining probe chain integrity (no tombstones). */
	reg->buckets[found].occupied = false;
	reg->count--;

	uint32_t hole = found;
	for (uint32_t i = 1; i < reg->capacity; i++) {
		uint32_t next = (hole + i) & mask;
		JceRegistryEntry *e = &reg->buckets[next];

		if (!e->occupied) break;

		/* Would this entry's ideal position be at or before the hole? */
		uint32_t ideal = bucket_index(e->path_hash, mask);
		/* Check if 'ideal' is in the range (hole, next] wrapped. */
		bool needs_shift;
		if (hole < next)
			needs_shift = (ideal <= hole) || (ideal > next);
		else
			needs_shift = (ideal <= hole) && (ideal > next);

		if (needs_shift) {
			reg->buckets[hole] = *e;
			e->occupied = false;
			hole = next;
		}
	}

	return true;
}

void jce_registry_clear(JceAssetRegistry *reg)
{
	if (!reg || !reg->buckets) return;
	memset(reg->buckets, 0, reg->capacity * sizeof(JceRegistryEntry));
	reg->count = 0;
}
