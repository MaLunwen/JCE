/*
 * jce_cook_policy.h  Shared texture-cook policy heuristics (INTERNAL).
 *
 * One source of truth for "which GPU format does a texture cook to" —
 * previously jce_asset_cooker.c and jce_bundle_pack.c carried mirrored
 * static copies (the bundle_pack one even said "matches jce_asset_cooker.c"
 * in its comment), which is exactly the drift this header prevents: if the
 * two policies ever disagree, a bundle-packed texture cooks differently
 * from a directly-cooked one. NOT installed with the SDK.
 */

#ifndef JCE_COOK_POLICY_H
#define JCE_COOK_POLICY_H

#include <jce/resource/jce_asset_format.h>

#include "jce_asset_cooker.h"   /* JCE_COOK_PLATFORM_* */

#include <stdbool.h>
#include <string.h>

/* Heuristic: does this asset path belong to a 3D colour-grading LUT?
 * LUT strip PNGs are loaded as raw RGBA8 by jce_texture_load_lut_3d; any
 * block-compression (BC3/ASTC) is both lossy and undecodable by the loader.
 * Passthrough-copy (raw PNG bytes) is therefore mandatory for LUTs.
 * We match either a "luts/" directory component or a "_lut" / "-lut" / ".lut"
 * filename pattern (case-insensitive). */
static bool jce_cook_path_is_lut(const char *path)
{
    if (!path) return false;
    char low[1024];
    size_t n = 0;
    for (; path[n] && n < sizeof(low) - 1; n++) {
        char c = path[n];
        low[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[n] = '\0';
    /* Match "luts/" as a path segment (works for any depth). */
    if (strstr(low, "luts/") || strstr(low, "luts\\"))
        return true;
    /* Match filename suffixes / infixes: _lut, -lut, .lut before the
     * extension dot.  Find the last '/' or '\\', then search the basename. */
    const char *base = low;
    for (const char *p = low; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    return strstr(base, "_lut") || strstr(base, "-lut") ||
           strstr(base, ".lut");
}

/* Heuristic: does this texture path look like a tangent-space normal map?
 * Normal maps must use BC5 (RG), not BC3 — BC3's chroma subsampling wrecks
 * them. */
static bool jce_cook_path_is_normal_map(const char *path)
{
    if (!path) return false;
    char low[1024];
    size_t n = 0;
    for (; path[n] && n < sizeof(low) - 1; n++) {
        char c = path[n];
        low[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[n] = '\0';
    /* BASENAME only, like the LUT test above.
     *
     * This used to search the whole path, so a colour texture living in a
     * directory called `normals/` was cooked to BC5 -- a two-channel format --
     * and lost its blue channel entirely.  That is the more dangerous of the
     * two failure directions: a false NEGATIVE (a normal map that does not
     * match) merely gets BC3, which the comment above notes wrecks it, while a
     * false POSITIVE destroys a colour map outright.
     *
     * Measured before changing it: across 1935 textures in this repository the
     * two spellings classify identically, so this removes a landmine rather
     * than changing any current output. */
    const char *base = low;
    for (const char *q = low; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    return strstr(base, "normal") || strstr(base, "_nrm") ||
           strstr(base, "_norm")  || strstr(base, "-normal");
}

/* Decode an `.import.json` "colorSpace" value.
 *
 * Returns true when the string is one this project defines, and only then
 * writes *out_srgb.  An unrecognised value is NOT an error and NOT a default:
 * it leaves the caller's existing answer alone, so a future third colour space
 * cannot silently be read as one of these two.
 *
 * Split out as pure string logic so the cook path and its test share ONE
 * implementation.  The alternative -- the comparison living inline in
 * cook_asset -- is untestable without building a bundle, which is how a key
 * that is written correctly and read with a typo ships green. */
static bool jce_cook_colour_space_parse(const char *value, bool *out_srgb)
{
    if (!value || !out_srgb) return false;
    /* Case-insensitive: the value is authored by tools and by humans. */
    char low[16];
    size_t n = 0;
    for (; value[n] && n < sizeof(low) - 1; n++) {
        char c = value[n];
        low[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[n] = '\0';
    if (strcmp(low, "srgb") == 0)   { *out_srgb = true;  return true; }
    if (strcmp(low, "linear") == 0) { *out_srgb = false; return true; }
    return false;
}

/* Is this texture sRGB-ENCODED COLOUR, as opposed to linear data?
 *
 * It decides the space mipmaps are averaged in (jce_tex_generate_mip_ex), and
 * it lives here, beside the format choice, because it is the same judgement
 * from the same evidence -- and because this file is already where the project
 * accepts a filename heuristic for a decision with WORSE consequences: picking
 * BC5 for a colour map destroys it, while averaging a linear map in sRGB
 * merely shifts values that were correct.
 *
 * It is a heuristic and it is wrong in a knowable way: 114 of this
 * repository's textures are GLB-embedded and named `<stem>_tex<N>.png`, which
 * carries no semantic at all and cannot be classified by ANY naming scheme.
 * The real fix is to plumb the semantic the importers already have --
 * JceModelMaterialInfo carries five named slots, JcePbrMaterial five typed
 * handles -- down into JceCookOptions, which today has no field for it.  Until
 * that exists this is the honest best available, and it errs toward sRGB
 * because colour maps outnumber linear ones and the colour case is the one
 * with the measured defect.
 */
static bool jce_cook_path_is_srgb(const char *path)
{
    /* UNKNOWN ANSWERS FALSE.  This polarity is the whole safety of the change.
     *
     * The two errors are not symmetric.  Calling a colour map linear leaves it
     * averaged exactly as it has been averaged since the cooker was written --
     * mildly wrong, unchanged, no regression.  Calling a LINEAR map sRGB
     * gamma-decodes data that was correct, and corrupts normals, roughness and
     * masks with no diagnostic.  So sRGB is asserted only on a POSITIVE
     * signal, never as a fallback, and the honest consequence is stated
     * plainly: most of this project's textures still get the old behaviour,
     * because most of them are named `<stem>_tex<N>.png` and say nothing.
     * That is the gap the semantic plumbing has to close; it is not something
     * a longer list of suffixes can fix. */
    if (!path) return false;                               /* unknown */
    if (jce_cook_path_is_lut(path))        return false;   /* raw strips */
    if (jce_cook_path_is_normal_map(path)) return false;   /* tangent space */

    char low[1024];
    size_t n = 0;
    for (; path[n] && n < sizeof(low) - 1; n++) {
        char c = path[n];
        low[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[n] = '\0';
    const char *base = low;
    for (const char *q = low; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;

    /* Linear-data conventions, checked FIRST so they beat a colour word that
     * happens to appear in the same name (`rock_colour_roughness.png`). */
    static const char *kLinear[] = {
        "_orm", "-orm", "_arm", "-arm",
        "_mr", "-mr", "_metallicroughness", "_metalroughness", "_metallic",
        "_roughness", "_rough", "_metalness", "_gloss", "_spec",
        "_ao", "-ao", "_occlusion", "_ambientocclusion",
        "_height", "_disp", "_displacement", "_bump",
        "_mask", "_opacity", "_data", "_splat",
    };
    for (size_t i = 0; i < sizeof(kLinear) / sizeof(kLinear[0]); ++i)
        if (strstr(base, kLinear[i])) return false;

    /* POSITIVE colour signals.  Only these turn the sRGB path on. */
    static const char *kColour[] = {
        "albedo", "basecolor", "basecolour", "base_color", "base_colour",
        "diffuse", "_col.", "_col_", "_color", "_colour", "-color", "-colour",
        "emissive", "emission",
    };
    for (size_t i = 0; i < sizeof(kColour) / sizeof(kColour[0]); ++i)
        if (strstr(base, kColour[i])) return true;
    return false;
}

/* Auto-select a GPU texture format from the target platform (only when the
 * caller didn't force one). Desktop -> BC (BC5 normals, BC3 colour);
 * mobile/web -> ASTC. Platform AUTO keeps RGBA8 (uncompressed).
 *
 * LUTs are always RGBA8: they are decoded by jce_texture_load_lut_3d as
 * raw pixel strips and block-compression destroys their precision. */
static int jce_cook_auto_texture_format(const char *path, int target_platform)
{
    /* LUTs must never be block-compressed — passthrough as RGBA8. */
    if (jce_cook_path_is_lut(path)) return JCEASSET_TEXFMT_RGBA8;

    switch (target_platform) {
    case JCE_COOK_PLATFORM_WINDOWS:
    case JCE_COOK_PLATFORM_LINUX:
    case JCE_COOK_PLATFORM_MACOS:
        return jce_cook_path_is_normal_map(path) ? JCEASSET_TEXFMT_BC5
                                                 : JCEASSET_TEXFMT_BC3;
    case JCE_COOK_PLATFORM_ANDROID:
    case JCE_COOK_PLATFORM_IOS:
    case JCE_COOK_PLATFORM_WEB:
        return JCEASSET_TEXFMT_ASTC_4x4;
    default:
        return JCEASSET_TEXFMT_RGBA8;   /* AUTO -> uncompressed */
    }
}

#endif /* JCE_COOK_POLICY_H */
