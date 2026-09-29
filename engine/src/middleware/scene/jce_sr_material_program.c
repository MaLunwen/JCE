/*
 * jce_sr_material_program.c  —  the Shader Graph custom-program cache.
 *
 * One question: which bgfx program, if any, does this .mat.json name?  The
 * answer is parsed once per unique material path and remembered, because the
 * alternative is re-reading a JSON file per draw.
 *
 * ITS OWN TRANSLATION UNIT because jce_scene_renderer.c had reached 6825 lines
 * and the file-size ratchet refused to let it grow again -- correctly.  This
 * cache is a self-contained idea with two entry points and one invariant, and
 * it was sitting between the texture cache and a string helper for no reason
 * other than having been written there.
 */

#include "jce_sr_internal.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/renderer/jce_pbr_material.h>

#include <stdint.h>
#include <string.h>

/* ".mat.json", case-insensitively.
 *
 * A private four-line test rather than a shared helper: the renderer's
 * sr_ends_with_ci is file-local to jce_scene_renderer.c, and exporting it to
 * reach one call site would put a general-purpose string function on the
 * internal header for the benefit of a single suffix check. */
static bool mat_json_suffix(const char *s)
{
    static const char kSuffix[] = ".mat.json";
    const size_t n = strlen(s), m = sizeof(kSuffix) - 1;
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != kSuffix[i]) return false;
    }
    return true;
}

/* ── Shader Graph custom-program cache ─────────────────────────────── */

/* Resolve (and cache) the custom shader program declared by a material
 * file's Shader Graph reference.  Returns UINT16_MAX when the material has
 * no custom shader (the common case) or cannot be resolved.  Cached by
 * path so the .mat.json is parsed at most once per unique material. */
void jce_scene_renderer_invalidate_custom_program(JceSceneRenderer *sr,
                                                  const char *material_path)
{
    if (!sr) return;
    for (int i = 0; i < sr->prog_cache_count; i++) {
        if (!sr->prog_cache[i].used) continue;
        if (material_path && material_path[0] &&
            strncmp(sr->prog_cache[i].path, material_path,
                    sizeof(sr->prog_cache[i].path)) != 0)
            continue;
        /* The entry is dropped, not the PROGRAM: the bgfx program is owned by
         * whoever created it (the material loader, or the graph panel's
         * hot-swap, which destroys the old one itself at end of frame).
         * Destroying it here would free a handle the panel is still holding. */
        sr->prog_cache[i].used     = false;
        sr->prog_cache[i].resolved = false;
        sr->prog_cache[i].path[0]  = '\0';
        sr->prog_cache[i].program.idx = UINT16_MAX;
    }
    /* Compact so the linear scan in sr_resolve_custom_program stays short and
     * a freed slot is reusable; the cache is small and this runs on an
     * authoring action, not per frame. */
    int w = 0;
    for (int i = 0; i < sr->prog_cache_count; i++)
        if (sr->prog_cache[i].used) sr->prog_cache[w++] = sr->prog_cache[i];
    sr->prog_cache_count = w;
}

JceShaderHandle sr_resolve_custom_program(JceSceneRenderer *sr,
                                                 const char *material_path)
{
    JceShaderHandle none = { UINT16_MAX };
    if (!sr || !material_path || !material_path[0]) return none;
    if (!mat_json_suffix(material_path)) return none;

    for (int i = 0; i < sr->prog_cache_count; i++) {
        if (sr->prog_cache[i].used &&
            strncmp(sr->prog_cache[i].path, material_path,
                    sizeof(sr->prog_cache[i].path)) == 0)
            return sr->prog_cache[i].program;
    }

    /* Parse the material file; the loader links any persisted graph shader
     * into custom_program for us.  Only the program handle is kept here. */
    JceShaderHandle prog = none;
    if (jce_fs_host_exists_file(material_path)) {
        JcePbrMaterial m;
        char tex_paths[5][256];
        if (jce_pbr_material_load_json(material_path, &m, tex_paths) &&
            m.custom_program != UINT16_MAX)
            prog.idx = m.custom_program;
    }

    if (sr->prog_cache_count < SR_MAT_PROG_CACHE_MAX) {
        int idx = sr->prog_cache_count++;
        snprintf(sr->prog_cache[idx].path, sizeof(sr->prog_cache[idx].path),
                 "%s", material_path);
        sr->prog_cache[idx].program  = prog;
        sr->prog_cache[idx].used     = true;
        sr->prog_cache[idx].resolved = true;
    }
    return prog;
}

/* ── Model cache ──────────────────────────────────────────────────── */
