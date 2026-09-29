/*
 * jce_cook_atlas.h — the --pack-atlas subcommand, in its own translation unit.
 *
 * Separate from tools/jce_cook.c because that file is 1,253 lines and the
 * file-size ratchet is a standing instruction; the packing arithmetic itself
 * is in engine/src/resource/jce_atlas_pack.c and unit-tested there.
 */
#ifndef JCE_COOK_ATLAS_H
#define JCE_COOK_ATLAS_H

/* Pack every image directly under `dir` into `<out_base>.png` +
 * `<out_base>.json` (Aseprite-shaped, which is what the engine already
 * reads).  `padding` UINT_MAX means the default; `max_side` 0 means default.
 * Returns 0 when every sprite was placed, 1 otherwise -- a sprite that did
 * not fit is NAMED, because a silently missing one renders as nothing. */
int jce_cook_pack_atlas(const char *dir, const char *out_base,
                        unsigned padding, unsigned max_side);

#endif /* JCE_COOK_ATLAS_H */
