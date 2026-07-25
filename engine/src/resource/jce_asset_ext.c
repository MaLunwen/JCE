/*
 * jce_asset_ext.c  Source-extension -> asset-kind classification.
 *
 * The single authority for "what kind of asset is this file?".  See the
 * contract note on jce_asset_type_from_ext() in <jce/resource/jce_asset_format.h>.
 *
 * The table below is the UNION of the five per-consumer lists that existed
 * before this file (cooker dispatch, runtime texture whitelist, editor asset
 * database, editor texture cache, asset-browser thumbnailer).  Where they
 * disagreed, the broader classification wins: a .webp really is a texture and
 * a .dae really is a model, regardless of whether a given build step happens
 * to support it.  Capability ("can I encode this?") is a separate question
 * each consumer answers for itself.
 */

#include <jce/resource/jce_asset_format.h>

#include <stddef.h>

/* ASCII-only case-insensitive compare.  Deliberately self-contained: this TU
 * is linked into the minimal-source-list host cooker as well as the engine, so
 * it must not pull in jce_str/SDL.  Extensions are ASCII by definition, and an
 * explicit A-Z fold avoids tolower()'s locale dependence. */
static int ext_icmp(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
    }
}

typedef struct {
    const char *ext;   /* lowercase, no leading dot */
    int         type;  /* JCEASSET_TYPE_* */
} ExtRow;

/* Kept sorted by kind for readability, not for lookup — the list is short
 * enough that a linear scan is cheaper than any index. */
static const ExtRow k_ext_table[] = {
    /* Textures.  dds/ktx/ktx2 are GPU containers; hdr is radiance HDR;
     * psd/gif/webp come from the editor-side tables. */
    { "png",  JCEASSET_TYPE_TEXTURE },
    { "jpg",  JCEASSET_TYPE_TEXTURE },
    { "jpeg", JCEASSET_TYPE_TEXTURE },
    { "bmp",  JCEASSET_TYPE_TEXTURE },
    { "tga",  JCEASSET_TYPE_TEXTURE },
    { "dds",  JCEASSET_TYPE_TEXTURE },
    { "ktx",  JCEASSET_TYPE_TEXTURE },
    { "ktx2", JCEASSET_TYPE_TEXTURE },
    { "hdr",  JCEASSET_TYPE_TEXTURE },
    { "webp", JCEASSET_TYPE_TEXTURE },
    { "psd",  JCEASSET_TYPE_TEXTURE },
    { "gif",  JCEASSET_TYPE_TEXTURE },

    /* Models.  obj/fbx/gltf/glb have first-party importers; the rest are
     * recognised so the editor and cooker agree on what they ARE. */
    { "obj",  JCEASSET_TYPE_MODEL },
    { "fbx",  JCEASSET_TYPE_MODEL },
    { "gltf", JCEASSET_TYPE_MODEL },
    { "glb",  JCEASSET_TYPE_MODEL },
    { "dae",  JCEASSET_TYPE_MODEL },
    { "stl",  JCEASSET_TYPE_MODEL },
    { "ply",  JCEASSET_TYPE_MODEL },
    { "usd",  JCEASSET_TYPE_MODEL },
    { "usdc", JCEASSET_TYPE_MODEL },
    { "usdz", JCEASSET_TYPE_MODEL },

    /* Audio.  opus is the royalty-free preference; m4a/aac are legacy. */
    { "wav",  JCEASSET_TYPE_SOUND },
    { "ogg",  JCEASSET_TYPE_SOUND },
    { "opus", JCEASSET_TYPE_SOUND },
    { "flac", JCEASSET_TYPE_SOUND },
    { "mp3",  JCEASSET_TYPE_SOUND },
    { "m4a",  JCEASSET_TYPE_SOUND },
    { "aac",  JCEASSET_TYPE_SOUND },

    /* Fonts. */
    { "ttf",  JCEASSET_TYPE_FONT },
    { "otf",  JCEASSET_TYPE_FONT },

    /* Shaders.  '.bin' is a compiled bgfx blob; '.sc' is bgfx source. */
    { "sc",   JCEASSET_TYPE_SHADER },
    { "sh",   JCEASSET_TYPE_SHADER },
    { "sb",   JCEASSET_TYPE_SHADER },
    { "bin",  JCEASSET_TYPE_SHADER },
};

/* Return the extension (no dot) of `path`, or NULL when it has none.
 * A bare extension ("png" or ".png") is accepted as-is. */
static const char *ext_of(const char *path)
{
    if (!path || path[0] == '\0') return NULL;

    const char *dot = NULL;
    for (const char *p = path; *p; ++p) {
        if (*p == '.')                       dot = p;
        else if (*p == '/' || *p == '\\')    dot = NULL; /* dot was in a dir */
    }
    if (dot) return (dot[1] != '\0') ? dot + 1 : NULL;

    /* No dot at all: treat the whole string as a bare extension, but only
     * when it looks like one (no separators) — otherwise it is a plain
     * file name with no extension. */
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') return NULL;
    return path;
}

int jce_asset_type_from_ext(const char *path)
{
    const char *ext = ext_of(path);
    if (!ext) return JCEASSET_TYPE_RAW;

    for (size_t i = 0; i < sizeof(k_ext_table) / sizeof(k_ext_table[0]); ++i) {
        if (ext_icmp(ext, k_ext_table[i].ext) == 0)
            return k_ext_table[i].type;
    }
    return JCEASSET_TYPE_RAW;
}

bool jce_asset_ext_is_texture(const char *path)
{
    return jce_asset_type_from_ext(path) == JCEASSET_TYPE_TEXTURE;
}
