/* jce_shaders.c
 *
 * Runtime bgfx shader loading from a PAK archive.
 * Decompresses .bin blobs, feeds them to bgfx_create_shader().
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_shaders.h>

#include "os/core/jce_memory.h"
#include "jce_shader_deferred.h"

#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "jce_shaders"

/* -- Embedded engine-shader PAK fallback ----------------------------- *
 * The engine static library bundles all stock shader bins (vs_color,
 * fs_pbr, …) for every backend (dx11, spv, glsl, essl, mtl).  When the
 * user PAK is NULL — or doesn't contain a particular shader — we fall
 * back to this embedded archive so a freshly-scaffolded SDK consumer
 * project still renders without shipping a separate engine_shaders.pak.
 *
 * The symbols `jce_engine_shaders_pak_data[]` /
 * `jce_engine_shaders_pak_size` are emitted at build time by
 * `tools/jce_pak --symbol-prefix jce_engine_shaders_pak`.
 *
 * When the engine is built without the embedded shader pack
 * (`-DJCE_EMBED_ENGINE_SHADERS=OFF`), a 1-byte stub keeps the link
 * happy and the fallback becomes a no-op.
 */
extern const unsigned char jce_engine_shaders_pak[];
extern const size_t        jce_engine_shaders_pak_size;

static const JcePakArchive *embedded_pak(void)
{
    static const JcePakArchive *cached = NULL;
    static int                  tried  = 0;
    if (!tried) {
        tried = 1;
        if (jce_engine_shaders_pak_size > 1) {
            cached = jce_pak_open(jce_engine_shaders_pak,
                                  jce_engine_shaders_pak_size);
            if (!cached) {
                LOG_WARN(LOG_TAG,
                         "embedded engine shader pak failed to open "
                         "(size=%zu)", (size_t)jce_engine_shaders_pak_size);
            }
        }
    }
    return cached;
}

/* Public accessor so other engine subsystems (decals, GPU particles) can use
 * the same embedded-engine-shader fallback as load_single(). */
const JcePakArchive *jce_shaders_embedded_engine_pak(void)
{
    return embedded_pak();
}

/* Map bgfx renderer type to the file suffix produced by shaderc. */
static const char *shader_suffix(bgfx_renderer_type_t type)
{
    switch (type) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12:
        return "dx11";
    case BGFX_RENDERER_TYPE_VULKAN:
        return "spv";
    case BGFX_RENDERER_TYPE_OPENGL:
        return "glsl";
    case BGFX_RENDERER_TYPE_OPENGLES:
        return "essl";
    case BGFX_RENDERER_TYPE_METAL:
        return "mtl";
    default:
        return NULL;
    }
}

/* WebGL must not link unused PBR families at the first frame. Capture the
 * source bytes now (no borrowed PAK/overlay lifetime), and create the GPU
 * program only at submission. Native platforms retain eager loading. */
static void deferred_source_free(JceDeferredSource *src)
{
    if (src->data) {
        if (src->host) jce_fs_buffer_free(src->data);
        else JCE_FREE(src->data);
    }
    memset(src, 0, sizeof *src);
}

static JceDeferredSource deferred_source_read(const JcePakArchive *pak,
    const char *dev, const char *stage, const char *base, bool strict_fs)
{
    JceDeferredSource src = {0};
    const char *suffix = shader_suffix(bgfx_get_renderer_type());
    if (!suffix) return src;
    char path[256], full[1536];
    if (snprintf(path, sizeof path, "shaders/%s_%s_%s.bin", stage, base, suffix)
        >= (int)sizeof path) return src;
    if (dev && dev[0]) {
        if (snprintf(full, sizeof full, "%s/%s", dev, path) >= (int)sizeof full)
            return src;
        if (strict_fs || jce_fs_host_exists_file(full)) {
            uint64_t size = 0;
            src.data = jce_fs_host_read_all(full, &size);
            src.host = true;
            if (!src.data || !size || size > UINT32_MAX) {
                deferred_source_free(&src);
                return src;
            }
            src.size = (uint32_t)size;
            return src;
        }
    }
    if (strict_fs) return src;
    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fallback = embedded_pak();
        asset = fallback ? jce_pak_find(fallback, path) : NULL;
    }
    if (!asset || !asset->original_size || asset->original_size > UINT32_MAX)
        return src;
    src.data = JCE_MALLOC((size_t)asset->original_size);
    if (!src.data) return src;
    src.size = (uint32_t)asset->original_size;
    if (jce_pak_decompress(asset, src.data, src.size) != src.size)
        deferred_source_free(&src);
    return src;
}

static JceShaderHandle defer_stock(const JcePakArchive *pak, const char *dev,
                                  const char *vs, const char *fs, bool strict_fs)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!JCE_PLATFORM_WEB || !vs || !fs || !caps ||
        caps->limits.maxPrograms >= JCE_DEFERRED_FIRST ||
        (strncmp(fs, "pbr", 3) != 0 && strcmp(fs, "terrain") != 0))
        return JCE_INVALID_SHADER;
    JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
    for (unsigned i = 0; i < JCE_DEFERRED_COUNT; ++i) {
        JceDeferredProgram *p = &state->programs[i];
        if (p->used) continue;
        memset(p, 0, sizeof *p);
        p->vs = deferred_source_read(pak, dev, "vs", vs, strict_fs);
        p->fs = deferred_source_read(pak, dev, "fs", fs, strict_fs);
        if (!p->vs.data || !p->fs.data) {
            deferred_source_free(&p->vs);
            deferred_source_free(&p->fs);
            return JCE_INVALID_SHADER;
        }
        p->used = true; p->program = UINT16_MAX;
        return (JceShaderHandle){ (uint16_t)(JCE_DEFERRED_FIRST + i) };
    }
    return JCE_INVALID_SHADER;
}

uint16_t jce_shader_deferred_resolve(uint16_t idx)
{
    if (!JCE_PLATFORM_WEB || idx < JCE_DEFERRED_FIRST || idx >= JCE_DEFERRED_FIRST + JCE_DEFERRED_COUNT)
        return idx;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || caps->limits.maxPrograms >= JCE_DEFERRED_FIRST) return idx;
    JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
    JceDeferredProgram *p = &state->programs[idx - JCE_DEFERRED_FIRST];
    if (!p->used) return UINT16_MAX;
    if (!p->attempted) {
        p->attempted = true;
        bgfx_shader_handle_t vs = bgfx_create_shader(bgfx_copy(p->vs.data, p->vs.size));
        bgfx_shader_handle_t fs = { UINT16_MAX };
        if (vs.idx != UINT16_MAX)
            fs = bgfx_create_shader(bgfx_copy(p->fs.data, p->fs.size));
        if (vs.idx != UINT16_MAX && fs.idx != UINT16_MAX)
            p->program = bgfx_create_program(vs, fs, true).idx;
        else if (vs.idx != UINT16_MAX)
            bgfx_destroy_shader(vs);
        deferred_source_free(&p->vs);
        deferred_source_free(&p->fs);
    }
    return p->program;
}

void jce_shader_deferred_shutdown(void)
{
    JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
    for (unsigned i = 0; i < JCE_DEFERRED_COUNT; ++i) {
        if (state->programs[i].used)
            jce_shader_program_destroy((JceShaderHandle){ (uint16_t)(JCE_DEFERRED_FIRST + i) });
    }
}

/* Load one shader (.bin) from the PAK, return a bgfx handle.
   Falls back to the engine-embedded shader pak when the caller-supplied
   pak is NULL or the path is missing. */
static bgfx_shader_handle_t load_single(const JcePakArchive *pak, const char *path)
{
    bgfx_shader_handle_t invalid;
    invalid.idx = UINT16_MAX;

    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fb = embedded_pak();
        if (fb && fb != pak) {
            asset = jce_pak_find(fb, path);
            if (asset) pak = fb;
        }
    }
    if (!asset) {
        LOG_ERROR(LOG_TAG, "not found in PAK: %s", path);
        return invalid;
    }

    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return invalid;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", path);
        JCE_FREE(buf);
        return invalid;
    }

    /* bgfx_copy allocates internal memory and copies; we can free buf. */
    const bgfx_memory_t *mem =
        bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);

    return bgfx_create_shader(mem);
}

/* Internal: load a program with explicit VS and FS base names.
   Skinned programs share the fragment shader with their non-skinned
   counterpart (e.g., vs_pbr_skinned + fs_pbr). */
static JceShaderHandle load_program_named(const JcePakArchive *pak,
                                          const char *vs_base,
                                          const char *fs_base)
{
    JceShaderHandle pending = defer_stock(pak, NULL, vs_base, fs_base, false);
    if (jce_shader_valid(pending)) return pending;

    const char *sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) {
        LOG_ERROR(LOG_TAG, "no shader suffix for renderer %s",
                  bgfx_get_renderer_name(bgfx_get_renderer_type()));
        return JCE_INVALID_SHADER;
    }

    char vs_path[256], fs_path[256];
    snprintf(vs_path, sizeof(vs_path), "shaders/vs_%s_%s.bin", vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path), "shaders/fs_%s_%s.bin", fs_base, sfx);

    bgfx_shader_handle_t vsh = load_single(pak, vs_path);
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    bgfx_shader_handle_t fsh = load_single(pak, fs_path);
    if (fsh.idx == UINT16_MAX) {
        bgfx_destroy_shader(vsh);
        return JCE_INVALID_SHADER;
    }

    bgfx_program_handle_t prog = bgfx_create_program(vsh, fsh, true);
    return (JceShaderHandle){ prog.idx };
}

JceShaderHandle shader_load_program(const JcePakArchive *pak, const char *name)
{
    return load_program_named(pak, name, name);
}

JceShaderHandle shader_load_program_named(const JcePakArchive *pak,
                                          const char *vs_base,
                                          const char *fs_base)
{
    return load_program_named(pak, vs_base, fs_base);
}

void jce_shader_program_destroy(JceShaderHandle prog)
{
    if (!jce_shader_valid(prog)) return;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (JCE_PLATFORM_WEB && caps && caps->limits.maxPrograms < JCE_DEFERRED_FIRST &&
        prog.idx >= JCE_DEFERRED_FIRST &&
        prog.idx < JCE_DEFERRED_FIRST + JCE_DEFERRED_COUNT) {
        JceShaderDeferredState *state = jce_renderer_shader_deferred_state();
        JceDeferredProgram *slot = &state->programs[prog.idx - JCE_DEFERRED_FIRST];
        if (!slot->used) return;
        if (slot->program != UINT16_MAX)
            bgfx_destroy_program((bgfx_program_handle_t){ slot->program });
        deferred_source_free(&slot->vs);
        deferred_source_free(&slot->fs);
        memset(slot, 0, sizeof *slot);
        return;
    }
    bgfx_program_handle_t p = { prog.idx };
    bgfx_destroy_program(p);
}

const char *jce_shaders_backend_suffix(void)
{
    return shader_suffix(bgfx_get_renderer_type());
}

/* ================================================================== */
/* Filesystem overlay (hot-reload support)                            */
/* ================================================================== */

static bgfx_shader_handle_t load_single_fs(const char *full_path)
{
    bgfx_shader_handle_t invalid;
    invalid.idx = UINT16_MAX;

    /* Read through the filesystem adapter rather than raw SDL: the dev-dir
     * overlay is an ASSET read, so an active-VFS override (editor bundle
     * preview) must be able to serve the shader bin it shipped. */
    uint64_t sz  = 0;
    void    *buf = jce_fs_host_read_all(full_path, &sz);
    if (!buf) {
        /* The adapter folds open-failure, short read and OOM into NULL. */
        LOG_ERROR(LOG_TAG, "fs open/read failed: %s", full_path);
        return invalid;
    }

    if (sz == 0) {
        jce_fs_buffer_free(buf);
        LOG_ERROR(LOG_TAG, "fs empty: %s", full_path);
        return invalid;
    }

    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)sz);
    jce_fs_buffer_free(buf);
    return bgfx_create_shader(mem);
}

static JceShaderHandle load_program_fs_named(const char *dev_dir,
                                             const char *vs_base,
                                             const char *fs_base)
{
    if (!dev_dir || !vs_base || !fs_base) return JCE_INVALID_SHADER;
    JceShaderHandle pending = defer_stock(NULL, dev_dir, vs_base, fs_base, true);
    if (jce_shader_valid(pending)) return pending;

    const char *sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) return JCE_INVALID_SHADER;

    char vs_path[512], fs_path[512];
    snprintf(vs_path, sizeof(vs_path),
             "%s/shaders/vs_%s_%s.bin", dev_dir, vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path),
             "%s/shaders/fs_%s_%s.bin", dev_dir, fs_base, sfx);

    bgfx_shader_handle_t vsh = load_single_fs(vs_path);
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    bgfx_shader_handle_t fsh = load_single_fs(fs_path);
    if (fsh.idx == UINT16_MAX) {
        bgfx_destroy_shader(vsh);
        return JCE_INVALID_SHADER;
    }

    bgfx_program_handle_t prog = bgfx_create_program(vsh, fsh, true);
    return (JceShaderHandle){ prog.idx };
}

JceShaderHandle shader_load_program_fs(const char *dev_dir, const char *name)
{
    return load_program_fs_named(dev_dir, name, name);
}

JceShaderHandle shader_load_program_fs_named(const char *dev_dir,
                                             const char *vs_base,
                                             const char *fs_base)
{
    return load_program_fs_named(dev_dir, vs_base, fs_base);
}

JceShaderHandle shader_load_program_overlay_named(
    const char *dev_dir, const JcePakArchive *pak,
    const char *vs_base, const char *fs_base)
{
    bgfx_shader_handle_t vsh, fsh;
    bgfx_program_handle_t program;
    const char *sfx;
    char vs_path[512], fs_path[512];
    bool vs_overlay, fs_overlay;

    if (!vs_base || !fs_base) return JCE_INVALID_SHADER;
    JceShaderHandle pending = defer_stock(pak, dev_dir, vs_base, fs_base, false);
    if (jce_shader_valid(pending)) return pending;
    sfx = shader_suffix(bgfx_get_renderer_type());
    if (!sfx) return JCE_INVALID_SHADER;

    snprintf(vs_path, sizeof(vs_path),
             "%s/shaders/vs_%s_%s.bin",
             dev_dir ? dev_dir : "", vs_base, sfx);
    snprintf(fs_path, sizeof(fs_path),
             "%s/shaders/fs_%s_%s.bin",
             dev_dir ? dev_dir : "", fs_base, sfx);
    vs_overlay = dev_dir && dev_dir[0] && jce_fs_host_exists_file(vs_path);
    fs_overlay = dev_dir && dev_dir[0] && jce_fs_host_exists_file(fs_path);

    vsh = vs_overlay ? load_single_fs(vs_path) : (bgfx_shader_handle_t){ UINT16_MAX };
    if (!vs_overlay) {
        char pak_path[256];
        snprintf(pak_path, sizeof(pak_path),
                 "shaders/vs_%s_%s.bin", vs_base, sfx);
        vsh = load_single(pak, pak_path);
    }
    if (vsh.idx == UINT16_MAX) return JCE_INVALID_SHADER;

    fsh = fs_overlay ? load_single_fs(fs_path) : (bgfx_shader_handle_t){ UINT16_MAX };
    if (!fs_overlay) {
        char pak_path[256];
        snprintf(pak_path, sizeof(pak_path),
                 "shaders/fs_%s_%s.bin", fs_base, sfx);
        fsh = load_single(pak, pak_path);
    }
    if (fsh.idx == UINT16_MAX) {
        bgfx_destroy_shader(vsh);
        return JCE_INVALID_SHADER;
    }

    program = bgfx_create_program(vsh, fsh, true);
    return (JceShaderHandle){ program.idx };
}

#include "renderer/jce_shader_variants_table.inc"

/* Defined below, beside the other overlay loaders: the table loader has
 * to sit above jce_shaders_load_all and load_overlay below it, and one
 * declaration is cheaper than moving either. */
static JceShaderHandle load_overlay(const char *dev_dir,
                                    const JcePakArchive *pak,
                                    const char *vs_base,
                                    const char *fs_base);

/* LOAD THE WHOLE PBR PROGRAM TABLE, deduplicated by (vs, fs).
 *
 * Six vertex variants times four keyword keys is 24 cells, but far fewer
 * distinct programs: three variants share the base fragment family, and a
 * family that does not carry an axis maps the same name for both states of
 * that bit.  Creating a bgfx program per duplicate would pay for every one of
 * them at load and hold them all until shutdown, so identical pairs share a
 * handle -- and the destroy side already frees by handle, not by cell.
 *
 * `dev_dir` NULL is the shipped path (pak only); non-NULL is the editor's
 * Reload Shaders, which prefers files on disk.  ONE implementation for both,
 * because a table that is right in the editor and stale in the build is the
 * defect this whole file exists to prevent. */
static void load_variant_table(const char *dev_dir, const JcePakArchive *pak,
                               JceShaderSet *set)
{
    struct { const char *vs, *fs; JceShaderHandle h; } seen[
        JCE_SHADER_VARIANT_COUNT * JCE_SHADER_KEY_COUNT];
    int nseen = 0;

    for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; v++) {
        const char *vs = jce_shader_vv_name[v];
        for (int k = 0; k < JCE_SHADER_KEY_COUNT; k++) {
            const char *fs = jce_shader_fs_for[v][k];
            JceShaderHandle h = JCE_INVALID_SHADER;
            int hit = -1;
            for (int i = 0; i < nseen; i++)
                if (strcmp(seen[i].vs, vs) == 0 && strcmp(seen[i].fs, fs) == 0) {
                    hit = i; break;
                }
            if (hit >= 0) {
                h = seen[hit].h;
            } else {
                h = load_overlay(dev_dir, pak, vs, fs);
                seen[nseen].vs = vs;
                seen[nseen].fs = fs;
                seen[nseen].h = h;
                nseen++;
            }
            set->variant[v][k] = h;
        }
    }
}

JceShaderSet jce_shaders_load_all(const JcePakArchive *pak)
{
    JCE_PROFILE_ZONE_N("Shaders::LoadAll");
    JceShaderSet set;
    memset(&set, 0, sizeof set);
    load_variant_table(NULL, pak, &set);
    set.color    = shader_load_program(pak, "color");
    set.textured = shader_load_program(pak, "textured");
    /* vs_textured + fs_text_sdf: same vertex stage, different edge. */
    set.text_sdf = load_program_named(pak, "textured", "text_sdf");
    set.mesh     = shader_load_program(pak, "mesh");
    set.pbr            = shader_load_program(pak, "pbr");
    /* Instanced PBR variant: vs_pbr_inst + fs_pbr (fragment unchanged). */
    set.pbr_inst       = load_program_named(pak, "pbr_inst",       "pbr");
    /* Per-instance-tint instanced variant (large-world-opt P1 #7):
       vs_pbr_inst_tint + fs_pbr_tint.  Optional; absent in older paks. */
    set.pbr_inst_tint  = load_program_named(pak, "pbr_inst_tint",  "pbr_tint");
    /* Texture-diverse instanced variant (texture-array batcher):
       vs_pbr_inst_tex_array + fs_pbr_inst_tex_array.  Optional; absent in older
       paks and on essl1/GLES2 (excluded from the array-sampler allowlist). */
    set.pbr_inst_tex_array = load_program_named(pak, "pbr_inst_tex_array", "pbr_inst_tex_array");
    /* LOD cross-fade instanced variant (千万 ②): vs_pbr_inst_fade smuggles the
       band-transition coverage in i_data3.w → fs_pbr_fade screen-door dithers.
       Optional; absent → the LOD-in-cull draw falls back to pbr_inst (hard
       band switches, still correct). */
    set.pbr_inst_fade  = load_program_named(pak, "pbr_inst_fade",  "pbr_fade");
    /* Skinned variants share the fragment shader with their non-skinned
       counterpart: vs_pbr_skinned + fs_pbr, vs_shadow_skinned + fs_shadow. */
    set.pbr_skinned    = load_program_named(pak, "pbr_skinned",    "pbr");
    /* Forward+ fragment variant (vs_pbr* + fs_pbr_fwdplus).  Same vertex
     * shaders; only the fragment program differs.  Optional: a pak built
     * before the variant existed simply won't have the bins → the handle stays
     * invalid and the renderer falls back to the non-variant program. */
    set.pbr_fwdplus         = load_program_named(pak, "pbr",         "pbr_fwdplus");
    set.pbr_inst_fwdplus    = load_program_named(pak, "pbr_inst",    "pbr_fwdplus");
    set.pbr_skinned_fwdplus = load_program_named(pak, "pbr_skinned", "pbr_fwdplus");
    /* Toon character (stylized-slice §5.6): vs_pbr_skinned + fs_pbr_toon. */
    set.pbr_toon       = load_program_named(pak, "pbr_skinned", "pbr_toon");
    /* Inverted-hull skinned outline: vs_pbr_skinned_outline + fs_outline. */
    set.outline_skinned = load_program_named(pak, "pbr_skinned_outline", "outline");
    set.shadow         = shader_load_program(pak, "shadow");
    /* Instanced shadow variant: vs_shadow_inst + fs_shadow (fragment unchanged). */
    set.shadow_inst    = load_program_named(pak, "shadow_inst",    "shadow");
    set.shadow_skinned = load_program_named(pak, "shadow_skinned", "shadow");
    set.terrain        = shader_load_program(pak, "terrain");

    if (!jce_shader_valid(set.color))
        LOG_ERROR(LOG_TAG, "failed to load 'color' shader");
    if (!jce_shader_valid(set.textured))
        LOG_WARN(LOG_TAG, "'textured' shader unavailable");
    if (!jce_shader_valid(set.mesh))
        LOG_WARN(LOG_TAG, "'mesh' shader unavailable");
    if (!jce_shader_valid(set.pbr))
        LOG_WARN(LOG_TAG, "'pbr' shader unavailable");
    if (!jce_shader_valid(set.pbr_inst))
        LOG_WARN(LOG_TAG, "'pbr_inst' shader unavailable (GPU instancing disabled)");
    if (!jce_shader_valid(set.pbr_inst_tint))
        LOG_WARN(LOG_TAG, "'pbr_inst_tint' shader unavailable (per-instance tint batching disabled)");
    if (!jce_shader_valid(set.pbr_inst_tex_array))
        LOG_WARN(LOG_TAG, "'pbr_inst_tex_array' shader unavailable (texture-diverse instancing disabled)");
    if (!jce_shader_valid(set.pbr_inst_fade))
        LOG_WARN(LOG_TAG, "'pbr_inst_fade' shader unavailable (LOD cross-fade dither disabled)");
    if (!jce_shader_valid(set.pbr_skinned))
        LOG_WARN(LOG_TAG, "'pbr_skinned' shader unavailable");
    if (!jce_shader_valid(set.pbr_fwdplus))
        LOG_WARN(LOG_TAG, "'pbr_fwdplus' shader unavailable (Forward+ disabled)");
    if (!jce_shader_valid(set.pbr_toon))
        LOG_WARN(LOG_TAG, "'pbr_toon' shader unavailable (toon character disabled)");
    if (!jce_shader_valid(set.outline_skinned))
        LOG_WARN(LOG_TAG, "'outline_skinned' shader unavailable (character outline disabled)");
    if (!jce_shader_valid(set.shadow))
        LOG_WARN(LOG_TAG, "'shadow' shader unavailable");
    if (!jce_shader_valid(set.shadow_inst))
        LOG_WARN(LOG_TAG, "'shadow_inst' shader unavailable (shadow instancing disabled)");
    if (!jce_shader_valid(set.shadow_skinned))
        LOG_WARN(LOG_TAG, "'shadow_skinned' shader unavailable");
    if (!jce_shader_valid(set.terrain))
        LOG_WARN(LOG_TAG, "'terrain' shader unavailable");

    JCE_PROFILE_ZONE_END;
    return set;
}

/* ================================================================== */
/* FS-overlay variant of jce_shaders_load_all                         */
/* ================================================================== */

/* Try FS first, fall back to PAK on a per-shader basis. */
static JceShaderHandle load_overlay(const char *dev_dir,
                                    const JcePakArchive *pak,
                                    const char *vs, const char *fs)
{
    JceShaderHandle pending = defer_stock(pak, dev_dir, vs, fs, false);
    if (jce_shader_valid(pending)) return pending;

    JceShaderHandle h = JCE_INVALID_SHADER;
    if (dev_dir && dev_dir[0])
        h = load_program_fs_named(dev_dir, vs, fs);
    if (!jce_shader_valid(h))
        h = load_program_named(pak, vs, fs);
    return h;
}

JceShaderSet jce_shaders_load_all_fs(const char *dev_dir,
                                     const JcePakArchive *pak)
{
    JCE_PROFILE_ZONE_N("Shaders::LoadAllFS");
    JceShaderSet set;
    memset(&set, 0, sizeof set);
    load_variant_table(dev_dir, pak, &set);
    set.color          = load_overlay(dev_dir, pak, "color",          "color");
    set.textured       = load_overlay(dev_dir, pak, "textured",       "textured");
    set.text_sdf       = load_overlay(dev_dir, pak, "textured",       "text_sdf");
    set.mesh           = load_overlay(dev_dir, pak, "mesh",           "mesh");
    set.pbr            = load_overlay(dev_dir, pak, "pbr",            "pbr");
    set.pbr_inst       = load_overlay(dev_dir, pak, "pbr_inst",       "pbr");
    set.pbr_inst_tint  = load_overlay(dev_dir, pak, "pbr_inst_tint",  "pbr_tint");
    set.pbr_inst_tex_array = load_overlay(dev_dir, pak, "pbr_inst_tex_array", "pbr_inst_tex_array");
    set.pbr_inst_fade  = load_overlay(dev_dir, pak, "pbr_inst_fade",  "pbr_fade");
    set.pbr_skinned    = load_overlay(dev_dir, pak, "pbr_skinned",    "pbr");
    set.pbr_fwdplus         = load_overlay(dev_dir, pak, "pbr",         "pbr_fwdplus");
    set.pbr_inst_fwdplus    = load_overlay(dev_dir, pak, "pbr_inst",    "pbr_fwdplus");
    set.pbr_skinned_fwdplus = load_overlay(dev_dir, pak, "pbr_skinned", "pbr_fwdplus");
    set.pbr_toon        = load_overlay(dev_dir, pak, "pbr_skinned",         "pbr_toon");
    set.outline_skinned = load_overlay(dev_dir, pak, "pbr_skinned_outline", "outline");
    set.shadow         = load_overlay(dev_dir, pak, "shadow",         "shadow");
    set.shadow_inst    = load_overlay(dev_dir, pak, "shadow_inst",    "shadow");
    set.shadow_skinned = load_overlay(dev_dir, pak, "shadow_skinned", "shadow");
    set.terrain        = load_overlay(dev_dir, pak, "terrain",        "terrain");
    JCE_PROFILE_ZONE_END;
    return set;
}
