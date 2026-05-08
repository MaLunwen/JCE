/*
 * jce_asset_labels.h  Asset addressables / label system.
 *
 * Mirrors Unity's Addressables labels: each asset can carry up to 64
 * arbitrary string labels (e.g. "ui", "level1", "boss_room") and game
 * code can `find_by_label("ui")` to enumerate every asset tagged with
 * that label — useful for bulk preload, scene-specific streaming
 * groups, and build-time slicing.
 *
 * Labels are project-wide and registered lazily by name; the symbolic
 * name is mapped to a single bit in a 64-bit mask stored on each
 * asset.  jce_asset_label_bit("ui") returns the bit (registering on
 * first call).  Up to 64 distinct labels per project; running out
 * returns 0.
 *
 * Layer: resource (Layer 4) — public.
 */

#ifndef JCE_ASSET_LABELS_H
#define JCE_ASSET_LABELS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_ASSET_LABEL_MAX 64u

/* Resolve a symbolic label to its bitmask, registering on first use.
 * Returns 0 if the registry is full. */
JCE_API uint64_t jce_asset_label_bit(const char *name);

/* Reverse — returns NULL if `bit_index` (0..63) is unregistered. */
JCE_API const char *jce_asset_label_name(uint32_t bit_index);

/* Number of registered labels. */
JCE_API uint32_t jce_asset_label_count(void);

/* Helper: build a mask from N names in one call (NULL-terminated). */
JCE_API uint64_t jce_asset_label_mask_from_names(const char *const *names);

JCE_EXTERN_C_END

#endif /* JCE_ASSET_LABELS_H */
