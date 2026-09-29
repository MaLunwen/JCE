/*
 * jce_atlas_pack.h — pack loose sprites into one atlas rectangle.
 *
 * WHY THIS EXISTS.  Sprite atlases in this engine come from an Aseprite JSON
 * that an artist exported: jce_sprite_sheet_load_json reads that format and
 * nothing in the cook produces one.  So a project with two hundred loose PNGs
 * either ships two hundred textures — two hundred binds, two hundred draws,
 * no batching — or the artist hand-packs them in another program and
 * re-exports every time a sprite changes.
 *
 * DELIBERATELY PURE.  No image data, no file I/O, no allocation: it takes
 * sizes and writes placements.  That is what lets every property below be
 * asserted headlessly — overlap, containment, padding, determinism — instead
 * of being inspected in a picture, where a one-pixel overlap looks exactly
 * like correct output.  The caller does the decoding and the blitting, which
 * is also what keeps this usable from the cook, from a tool, and from a test.
 *
 * THE OUTPUT IS THE FORMAT THE ENGINE ALREADY READS.  A packer that invented
 * its own layout would need a loader, a version, and a second thing to keep
 * in step with the first.  Emitting Aseprite-shaped JSON means the runtime
 * side of this feature is code that already shipped.
 */
#ifndef JCE_ATLAS_PACK_H
#define JCE_ATLAS_PACK_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* One sprite going in.  `id` is carried through untouched so the caller can
 * match a placement back to its file without a parallel array. */
typedef struct {
    uint32_t id;
    uint32_t w;
    uint32_t h;
} JceAtlasItem;

/* Where it landed.  `placed` is false for an item that did not fit; the pack
 * reports that rather than dropping it, because a silently missing sprite
 * renders as nothing and looks like an authoring mistake. */
typedef struct {
    uint32_t id;
    uint32_t x, y;
    uint32_t w, h;
    bool     placed;
} JceAtlasPlacement;

typedef struct {
    uint32_t max_width;      /* 0 => 4096 */
    uint32_t max_height;     /* 0 => 4096 */
    /* Transparent pixels left between neighbours AND at the atlas edge.
     * Bilinear filtering samples half a texel outside a sprite's rectangle,
     * so with 0 padding a sprite bleeds its neighbour's colour along the seam
     * — the artefact that makes an atlas look like a rendering bug. */
    uint32_t padding;
    /* Round the atlas up to a power of two.  Off by default: it costs memory,
     * and every backend this engine targets samples NPOT textures. */
    bool     power_of_two;
} JceAtlasPackDesc;

JCE_API JceAtlasPackDesc jce_atlas_pack_desc_default(void);

/*
 * Place `count` items into one rectangle.
 *
 * Returns the number PLACED, and writes every item's placement (placed or
 * not) into `out` in the caller's original order.  `out_w` / `out_h` receive
 * the used extent, which is what the caller should allocate — not max_width.
 *
 * DETERMINISTIC.  The same items in any order give the same atlas: the sort
 * is by height, then width, then id, and every tie is broken by id.  An atlas
 * that reshuffles when a directory listing changes order would rewrite every
 * sprite's UVs on a cook that changed nothing, and this tree gates
 * reproducible builds.
 *
 * An item wider or taller than the atlas can never be placed; it comes back
 * with placed=false rather than clipped.
 */
JCE_API size_t jce_atlas_pack(const JceAtlasItem      *items,
                              size_t                   count,
                              const JceAtlasPackDesc  *desc,
                              JceAtlasPlacement       *out,
                              uint32_t                *out_w,
                              uint32_t                *out_h);

/*
 * Write the placements as Aseprite-shaped JSON -- the format
 * jce_sprite_sheet_load_json already reads.
 *
 * PUBLIC so the cook and the test call the SAME writer.  A test that
 * hand-copies "the shape the tool emits" is a test of the hand copy: it stays
 * green while the tool drifts, which is the failure this whole audit is about.
 *
 * `names[i]` is the frame key for item i (Aseprite uses the source file name,
 * and keeping that means a designer looks for the name they gave the file).
 * Unplaced items are omitted -- they have no rectangle to name.
 * Returns false only on a write error.
 */
JCE_API bool jce_atlas_write_aseprite_json(const char              *path,
                                           const char *const       *names,
                                           const JceAtlasPlacement *placements,
                                           size_t                   count,
                                           uint32_t                 atlas_w,
                                           uint32_t                 atlas_h,
                                           const char              *image_name);

JCE_EXTERN_C_END

#endif /* JCE_ATLAS_PACK_H */
