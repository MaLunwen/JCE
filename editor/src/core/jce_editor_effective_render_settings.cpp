/* See the header for why this exists and why it is not next to the path
 * resolver it calls. */

#include "core/jce_editor_effective_render_settings.h"

#include "core/jce_project_settings.h"
#include "scene/jce_scene_content_context.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>

namespace {

bool editor_load_isolated_render_settings(const std::string &path,
                                          JceRenderSettings *out)
{
    /* This content address belongs to the currently admitted isolated VFS.
     * The host convenience reader also supports alternate reader callbacks;
     * an absent callback must never turn an isolated miss into a CWD read. */
    JceFileSystem *fs = jce_fs_get_active();
    if (!fs || jce_fs_get_active_policy() != JCE_FS_ACTIVE_ISOLATED)
        return false;
    JceFile *file = jce_fs_open(fs, path.c_str());
    if (!file)
        return false;
    const uint64_t size = jce_fs_size(file);
    if (!size || size > (1u << 20)) {
        jce_fs_close(file);
        return false;
    }
    /* Bound this caller's copy before allocation/read. A PAK-backed open
     * already eagerly decoded its entry; this is not a total VFS memory cap. */
    char *bytes = static_cast<char *>(jce_malloc(static_cast<size_t>(size)));
    const bool read = bytes && jce_fs_read(file, bytes, size) == size;
    jce_fs_close(file);
    const bool loaded = read &&
        jce_render_settings_load_json_mem(bytes, static_cast<size_t>(size), out);
    jce_free(bytes);
    return loaded;
}

bool editor_load_render_settings(bool isolated, const std::string &path,
                                  JceRenderSettings *out)
{
    if (isolated)
        return editor_load_isolated_render_settings(path, out);
    return jce_render_settings_load_json(path.c_str(), out);
}

} // namespace

int jce_editor_quality_msaa(int level_anti_aliasing, int project_default_msaa)
{
    const int aa = (level_anti_aliasing == JCE_PS_AA_USE_PROJECT_DEFAULT)
                       ? project_default_msaa
                       : level_anti_aliasing;
    /* Only the sample counts a swapchain can be asked for.  A stray 3 from a
     * hand-edited project file must not reach bgfx, and 1 and 0 both mean
     * "no multisampling" downstream. */
    switch (aa) {
        case 2: case 4: case 8: case 16: return aa;
        default:                         return 0;
    }
}

bool jce_editor_effective_render_settings(bool isolated,
                                          const char *project_root,
                                          JceRenderSettings *out,
                                          std::string *out_source_path,
                                          const char *source_assets,
                                          const char *cooked_assets,
                                          const char *loose_asset_root)
{
    if (!out)
        return false;

    const JceEditorSceneContentPaths paths =
        jce_editor_scene_content_paths(isolated, project_root, source_assets,
                                       cooked_assets, loose_asset_root);

    /* 1. The authored file carries the Look Profile and the grass gate. */
    std::string used;
    bool loaded = false;
    if (!paths.render_settings_primary.empty()) {
        used = paths.render_settings_primary;
        loaded = editor_load_render_settings(isolated, used, out);
    }
    if (!loaded && !paths.render_settings_fallback.empty()) {
        used = paths.render_settings_fallback;
        loaded = editor_load_render_settings(isolated, used, out);
    }
    if (!loaded)
        used.clear();

    /* 2. The ACTIVE QUALITY LEVEL wins for the seven fields it owns.  This is
     *    the half the viewport was missing: the packager applied it, the
     *    editor did not, so a designer only saw their quality level after a
     *    build. */
    const bool loose = !isolated && (!project_root || !project_root[0]) &&
                       loose_asset_root && loose_asset_root[0];
    JceProjectSettings ps_local;
    /* A standalone scene has no project quality owner. Do not overwrite its
     * authored settings with the previous project's cache or CWD defaults. */
    const JceProjectSettings *ps = loose ? nullptr : jce_project_settings_current();
    if (!loose && !ps && jce_project_settings_load(&ps_local))
        ps = &ps_local;
    if (ps) {
        int ql = ps->quality.current_level;
        if (ql < 0) ql = 0;
        if (ql >= JCE_PS_MAX_QUALITY_LEVELS) ql = JCE_PS_MAX_QUALITY_LEVELS - 1;
        const JceProjectQualityLevel *lvl = &ps->quality.levels[ql];
        static const int kShadowRes[4] = { 512, 1024, 2048, 4096 };
        int sr = lvl->shadow_resolution;
        if (sr < 0) sr = 0;
        if (sr > 3) sr = 3;
        out->shadow_quality  = lvl->shadow_quality;
        out->shadow_map_size = kShadowRes[sr];
        out->shadow_cascades = lvl->shadow_cascades;
        out->shadow_distance = lvl->shadow_distance;
        out->lod_bias        = lvl->lod_bias;
        out->vsync           = lvl->vsync_count > 0 ? 1 : 0;
        /* THE LEVEL'S MSAA, not the project's.  This line read
         * ps->graphics.default_msaa unconditionally while every other line in
         * this block took the level's value, so the per-level Anti Aliasing
         * combo could not change anything -- in the viewport or in the
         * shipped game, since this one composition feeds both. */
        /* THE LEVEL'S MSAA, not the project's.  This line read
         * ps->graphics.default_msaa unconditionally while every other line in
         * this block took the level's value, so the per-level Anti Aliasing
         * combo could not change anything -- in the viewport or in the
         * shipped game, since this one composition feeds both. */
        out->msaa            = jce_editor_quality_msaa(lvl->anti_aliasing,
                                                       ps->graphics.default_msaa);
        /* Texture quality is the LEVEL's; anisotropic is project-wide
         * Graphics.  Both were applied to the VIEWPORT (jce_panel_project_
         * settings and jce_editor_scene_rendering_defaults both call the two
         * jce_texture_set_* consumers) and neither was exported, so the
         * authored value stopped at the editor window. */
        out->texture_quality = lvl->texture_quality;
        out->anisotropic     = (int)ps->graphics.anisotropic_textures;
        /* -1 (Unity's "uncapped") and 0 both mean uncapped here. */
        out->target_framerate = lvl->target_framerate > 0
                                    ? lvl->target_framerate : 0;
        out->pixel_light_count = lvl->pixel_light_count > 0
                                     ? lvl->pixel_light_count : 0;
        /* Soft particles: a per-level checkbox that had no depth fade in
         * either particle path to reach.  The level carries a boolean, so the
         * distance is the engine's default -- a second authored number would
         * be one more field nobody sets. */
        out->soft_particles = lvl->soft_particles ? 1 : 0;
    }

    if (out_source_path)
        *out_source_path = used;
    return loaded || ps != nullptr;
}
