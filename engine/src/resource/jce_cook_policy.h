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
    return strstr(low, "normal") || strstr(low, "_nrm") ||
           strstr(low, "_norm")  || strstr(low, "-normal");
}

/* Auto-select a GPU texture format from the target platform (only when the
 * caller didn't force one). Desktop -> BC (BC5 normals, BC3 colour);
 * mobile/web -> ASTC. Platform AUTO keeps RGBA8 (uncompressed). */
static int jce_cook_auto_texture_format(const char *path, int target_platform)
{
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
