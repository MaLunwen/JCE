/*
 * jce_cook_atlas.c — `jce_cook --pack-atlas <dir> --atlas-out <base>`.
 *
 * Packs every image in a directory into ONE atlas and writes the pair the
 * engine already reads:
 *
 *     <base>.png     the atlas
 *     <base>.json    Aseprite-shaped frames + meta
 *
 * THE OUTPUT FORMAT IS NOT A CHOICE.  jce_sprite_sheet_load_json already
 * reads Aseprite JSON, and jce_sprite_player already drives it.  Emitting
 * anything else would need a loader, a version, and a second thing to keep in
 * step with the first -- so the runtime half of this feature is code that
 * shipped a long time ago, and this adds none.
 *
 * A SEPARATE TRANSLATION UNIT because tools/jce_cook.c is 1,253 lines and the
 * file-size ratchet is a standing instruction, not a suggestion.  The packing
 * arithmetic lives in engine/src/resource/jce_atlas_pack.c and is unit-tested
 * on its own; what is here is decode, blit, encode and JSON -- the parts that
 * need files and therefore cannot be asserted the same way.
 */
#include "jce_cook_atlas.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_atlas_pack.h>

/* NO SDL_image HERE, and the dependency contract is emphatic about why:
 * contracts/dependency-ownership.yml keeps the image-decode-ldr capability at
 * ONE directory and lists "direct IMG_Load at call sites" as a forbidden
 * alternative, because a second decode path skips the service's protections
 * -- most concretely the 16-bit-greyscale route-around that exists because
 * that input overruns libpng's heap.  The first version of this file included
 * it and the boundary gate refused it.  Decode goes through jce_image, and
 * the atlas is written as a raw RGBA8 blob plus its JSON, which is what the
 * texture pipeline consumes anyway. */
#include <jce/resource/jce_image_decode.h>

/* tools/jce_tex_encode.cpp -- bimg, which this binary already links. */
int jce_tex_write_png(const char *path, const uint8_t *rgba,
                      uint32_t w, uint32_t h);

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "cook-atlas"

enum { ATLAS_MAX_SPRITES = 4096 };

typedef struct {
    char     name[256];       /* file name, used as the frame key */
    JceImage img;             /* RGBA8, via the one decode service */
} AtlasSprite;

static int name_cmp(const void *a, const void *b)
{
    return strcmp(((const AtlasSprite *)a)->name,
                  ((const AtlasSprite *)b)->name);
}

typedef struct {
    char   names[ATLAS_MAX_SPRITES][256];
    size_t count;
} NameList;

static bool ext_is(const char *dot, const char *ext)
{
    size_t i = 0;
    for (; dot[i] && ext[i]; ++i) {
        char a = dot[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return false;
    }
    return dot[i] == 0 && ext[i] == 0;
}

static bool is_image_name(const char *n)
{
    const char *dot = strrchr(n, '.');
    if (!dot) return false;
    return ext_is(dot, ".png") || ext_is(dot, ".jpg") ||
           ext_is(dot, ".jpeg") || ext_is(dot, ".bmp") ||
           ext_is(dot, ".tga");
}

static bool collect_name(const char *name, bool is_dir, void *user)
{
    NameList *l = (NameList *)user;
    if (is_dir || !is_image_name(name)) return true;
    if (l->count >= ATLAS_MAX_SPRITES) return false;   /* stop, do not wrap */
    snprintf(l->names[l->count], sizeof l->names[0], "%s", name);
    ++l->count;
    return true;
}

int jce_cook_pack_atlas(const char *dir, const char *out_base,
                        unsigned padding, unsigned max_side)
{
    if (!dir || !dir[0] || !out_base || !out_base[0]) {
        LOG_ERROR(LOG_TAG, "--pack-atlas needs a directory and --atlas-out");
        return 2;
    }

    /* ON THE HEAP, and that is not tidiness: char[4096][256] is 1,048,576
     * bytes, which is EXACTLY the default Windows thread stack.  The first
     * version put it on the stack and the process died before printing a
     * single line -- exit 127, no output, which reads like "the binary is
     * broken" rather than "that array is too big". */
    NameList *list = (NameList *)calloc(1u, sizeof(NameList));
    if (!list) return 1;
    if (!jce_fs_host_list_dir(dir, collect_name, list) || list->count == 0) {
        LOG_ERROR(LOG_TAG, "no images under '%s'", dir);
        free(list);
        return 1;
    }

    AtlasSprite *sprites = (AtlasSprite *)calloc(list->count,
                                                 sizeof(AtlasSprite));
    if (!sprites) { free(list); return 1; }

    size_t n = 0;
    for (size_t i = 0; i < list->count; ++i) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, list->names[i]);
        JceImage img;
        memset(&img, 0, sizeof img);
        if (!jce_image_decode_file(path, &img) || !img.pixels) {
            /* Named, not counted-and-forgotten: a sprite that silently does
             * not reach the atlas renders as nothing later, which looks like
             * an authoring mistake and is not one. */
            LOG_WARN(LOG_TAG, "skipping '%s': could not decode",
                     list->names[i]);
            continue;
        }
        snprintf(sprites[n].name, sizeof sprites[n].name, "%s", list->names[i]);
        sprites[n].img = img;
        ++n;
    }
    free(list);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "no images loaded from '%s'", dir);
        free(sprites);
        return 1;
    }

    /* Sort by NAME before packing.  jce_atlas_pack is deterministic for a
     * given set of ids, and this is what makes the ids themselves stable: a
     * directory listed in a different order must produce the same atlas, or a
     * cook that changed nothing rewrites every UV. */
    qsort(sprites, n, sizeof(AtlasSprite), name_cmp);

    JceAtlasItem      *items = (JceAtlasItem *)calloc(n, sizeof(*items));
    JceAtlasPlacement *pl    = (JceAtlasPlacement *)calloc(n, sizeof(*pl));
    if (!items || !pl) { free(items); free(pl); free(sprites); return 1; }
    for (size_t i = 0; i < n; ++i) {
        items[i].id = (uint32_t)i;
        items[i].w  = sprites[i].img.width;
        items[i].h  = sprites[i].img.height;
    }

    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    if (padding  != UINT_MAX) d.padding    = padding;
    if (max_side != 0u)       d.max_width  = d.max_height = max_side;

    uint32_t aw = 0, ah = 0;
    const size_t placed = jce_atlas_pack(items, n, &d, pl, &aw, &ah);

    if (placed < n) {
        for (size_t i = 0; i < n; ++i)
            if (!pl[i].placed)
                LOG_ERROR(LOG_TAG,
                          "'%s' (%ux%u) did not fit in %ux%u -- raise "
                          "--atlas-max or split the folder",
                          sprites[i].name, items[i].w, items[i].h,
                          d.max_width, d.max_height);
    }
    if (placed == 0) {
        LOG_ERROR(LOG_TAG, "nothing fit; no atlas written");
        goto fail;
    }

    /* CLEARED TO TRANSPARENT, not black.  Bilinear filtering samples half a
     * texel outside a sprite's rectangle, so the padding between sprites is
     * read at every edge -- black there is a dark halo, which is the artefact
     * people report as "the sprite has an outline". */
    const size_t atlas_bytes = (size_t)aw * (size_t)ah * 4u;
    uint8_t     *atlas       = (uint8_t *)calloc(1u, atlas_bytes);
    if (!atlas) goto fail;

    for (size_t i = 0; i < n; ++i) {
        if (!pl[i].placed) continue;
        const JceImage *src = &sprites[i].img;
        for (uint32_t row = 0; row < src->height; ++row) {
            const uint8_t *sp = src->pixels + (size_t)row * src->width * 4u;
            uint8_t       *dp = atlas
                              + ((size_t)(pl[i].y + row) * aw + pl[i].x) * 4u;
            memcpy(dp, sp, (size_t)src->width * 4u);
        }
    }

    char png_path[1024], json_path[1024];
    snprintf(png_path,  sizeof png_path,  "%s.png",  out_base);
    snprintf(json_path, sizeof json_path, "%s.json", out_base);

    /* Through the encoder the cook ALREADY owns.  The first version wrote a
     * raw .rgba blob to dodge the dependency gate -- which traded a contract
     * violation for a broken output, since nothing in the texture path loads
     * a headerless blob.  bimg is linked here for the block encoder, so
     * jce_tex_write_png adds no dependency; it exposes one already paid for. */
    if (jce_tex_write_png(png_path, atlas, aw, ah) != 0) {
        LOG_ERROR(LOG_TAG, "could not write %s", png_path);
        free(atlas);
        goto fail;
    }
    free(atlas);

    const char *png_name = strrchr(png_path, '/');
    png_name = png_name ? png_name + 1 : png_path;

    /* The SHARED writer, not a copy: engine/src/resource/jce_atlas_pack.c
     * owns the format, and the unit test calls the same function -- so a test
     * of "the shape the tool emits" cannot be a test of a hand copy that
     * stays green while the tool drifts. */
    {
        const char **keys = (const char **)calloc(n, sizeof(char *));
        if (!keys) goto fail;
        for (size_t i = 0; i < n; ++i) keys[i] = sprites[i].name;
        const bool ok = jce_atlas_write_aseprite_json(json_path, keys, pl, n,
                                                      aw, ah, png_name);
        free(keys);
        if (!ok) {
            LOG_ERROR(LOG_TAG, "could not write %s", json_path);
            goto fail;
        }
    }

    LOG_INFO(LOG_TAG, "atlas: %zu/%zu sprite(s) -> %ux%u  %s + %s",
             placed, n, aw, ah, png_path, json_path);

    for (size_t i = 0; i < n; ++i) jce_image_free(&sprites[i].img);
    free(items); free(pl); free(sprites);
    return (placed == n) ? 0 : 1;

fail:
    for (size_t i = 0; i < n; ++i) jce_image_free(&sprites[i].img);
    free(items); free(pl); free(sprites);
    return 1;
}
