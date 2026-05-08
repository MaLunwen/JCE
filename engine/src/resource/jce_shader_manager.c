/*
 * jce_shader_manager.c  Shader resource cache implementation.
 */

#include "jce_shader_manager.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_file_watcher.h>
#include <jce/renderer/jce_shaders.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "shader_mgr"
#define MAX_SHADERS 64
#define MAX_NAME_LEN 64
#define MAX_DEV_DIR_LEN 256
#define MAX_PATH_LEN 512

typedef struct {
    char            name[MAX_NAME_LEN];
    JceShaderHandle handle;
    int             ref_count;
} ShaderEntry;

struct JceShaderManager {
    const JcePakArchive *pak;
    ShaderEntry       entries[MAX_SHADERS];
    int               count;

    /* Hot-reload state. */
    char              dev_dir[MAX_DEV_DIR_LEN];
    bool              has_dev_dir;
    uint32_t          generation;
    JceFileWatcher   *watcher;          /* not owned */
};

JceShaderManager *jce_shader_manager_create(const JcePakArchive *pak)
{
    JceShaderManager *mgr = (JceShaderManager *)JCE_CALLOC(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->pak = pak;
    return mgr;
}

void jce_shader_manager_destroy(JceShaderManager *mgr)
{
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++) {
        if (jce_shader_valid(mgr->entries[i].handle)) {
            bgfx_program_handle_t p = { mgr->entries[i].handle.idx };
            bgfx_destroy_program(p);
        }
    }
    JCE_FREE(mgr);
}

JceShaderHandle jce_shader_manager_acquire(JceShaderManager *mgr,
                                            const char *name)
{
    if (!mgr || !name) return JCE_INVALID_SHADER;

    /* Check cache. */
    for (int i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, name) == 0) {
            mgr->entries[i].ref_count++;
            return mgr->entries[i].handle;
        }
    }

    /* Load new. */
    if (mgr->count >= MAX_SHADERS) {
        LOG_ERROR(LOG_TAG, "shader cache full");
        return JCE_INVALID_SHADER;
    }

    JceShaderHandle h = mgr->has_dev_dir
        ? shader_load_program_fs(mgr->dev_dir, name)
        : shader_load_program(mgr->pak, name);
    if (!jce_shader_valid(h) && mgr->has_dev_dir) {
        /* Fall back to PAK if the dev dir doesn't have this shader. */
        h = shader_load_program(mgr->pak, name);
    }
    if (!jce_shader_valid(h)) return JCE_INVALID_SHADER;

    ShaderEntry *e = &mgr->entries[mgr->count++];
    snprintf(e->name, MAX_NAME_LEN, "%s", name);
    e->handle    = h;
    e->ref_count = 1;

    LOG_INFO(LOG_TAG, "loaded shader '%s'", name);
    return h;
}

JceShaderHandle jce_shader_manager_acquire_variant(JceShaderManager *mgr,
                                                    const char *base,
                                                    JceShaderKeywordSet set)
{
    if (!mgr || !base) return JCE_INVALID_SHADER;
    if (set == 0) return jce_shader_manager_acquire(mgr, base);

    /* Build canonical variant name and try to load it.  If the variant
     * binary doesn't exist on disk / in the PAK, fall back to the base
     * so the renderer still draws (possibly with the wrong keywords
     * compiled in, but that's the cooker's job to surface). */
    char variant[160];
    uint32_t n = jce_shader_variant_resolve_name(base, set,
                                                 variant, sizeof(variant));
    if (n == 0) return jce_shader_manager_acquire(mgr, base);

    /* First check the cache for the variant; if missing, attempt to
     * load it; on failure, transparently fall through to the base. */
    for (int i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, variant) == 0) {
            mgr->entries[i].ref_count++;
            return mgr->entries[i].handle;
        }
    }

    if (mgr->count < MAX_SHADERS) {
        JceShaderHandle h = mgr->has_dev_dir
            ? shader_load_program_fs(mgr->dev_dir, variant)
            : shader_load_program(mgr->pak, variant);
        if (!jce_shader_valid(h) && mgr->has_dev_dir)
            h = shader_load_program(mgr->pak, variant);
        if (jce_shader_valid(h)) {
            ShaderEntry *e = &mgr->entries[mgr->count++];
            snprintf(e->name, MAX_NAME_LEN, "%s", variant);
            e->handle    = h;
            e->ref_count = 1;
            LOG_INFO(LOG_TAG, "loaded shader variant '%s'", variant);
            return h;
        }
    }

    /* Variant unavailable — fall back to the base.  Logged once per
     * miss to help cooker debugging without spamming. */
    LOG_INFO(LOG_TAG, "variant '%s' not found, falling back to '%s'",
             variant, base);
    return jce_shader_manager_acquire(mgr, base);
}

void jce_shader_manager_release(JceShaderManager *mgr, const char *name)
{
    if (!mgr || !name) return;
    for (int i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, name) == 0) {
            if (--mgr->entries[i].ref_count <= 0) {
                bgfx_program_handle_t p = { mgr->entries[i].handle.idx };
                bgfx_destroy_program(p);
                mgr->entries[i] = mgr->entries[--mgr->count];
                LOG_INFO(LOG_TAG, "released shader '%s'", name);
            }
            return;
        }
    }
}

int jce_shader_manager_count(const JceShaderManager *mgr)
{
    return mgr ? mgr->count : 0;
}

/* ============================================================ */
/* Hot-reload                                                    */
/* ============================================================ */

void jce_shader_manager_set_dev_dir(JceShaderManager *mgr, const char *dev_dir)
{
    if (!mgr) return;
    if (dev_dir && dev_dir[0]) {
        snprintf(mgr->dev_dir, sizeof(mgr->dev_dir), "%s", dev_dir);
        mgr->has_dev_dir = true;
        LOG_INFO(LOG_TAG, "dev dir set: %s", mgr->dev_dir);
    } else {
        mgr->dev_dir[0]  = '\0';
        mgr->has_dev_dir = false;
    }
}

uint32_t jce_shader_manager_generation(const JceShaderManager *mgr)
{
    return mgr ? mgr->generation : 0;
}

bool jce_shader_manager_reload(JceShaderManager *mgr, const char *name)
{
    if (!mgr || !name) return false;

    for (int i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, name) != 0) continue;

        JceShaderHandle nh = mgr->has_dev_dir
            ? shader_load_program_fs(mgr->dev_dir, name)
            : shader_load_program(mgr->pak, name);

        if (!jce_shader_valid(nh)) {
            LOG_ERROR(LOG_TAG, "reload failed for '%s'", name);
            return false;
        }

        /* Destroy old program; bgfx tolerates destruction of in-flight
           handles (it defers to the end of the current frame). */
        bgfx_program_handle_t old = { mgr->entries[i].handle.idx };
        bgfx_destroy_program(old);

        mgr->entries[i].handle = nh;
        mgr->generation++;
        LOG_INFO(LOG_TAG, "reloaded shader '%s' (gen=%u)", name, mgr->generation);
        return true;
    }
    return false;
}

/* Watcher callback: extracts shader base name from the path and
   triggers reload.  Path is expected to look like
       <dev_dir>/shaders/(vs|fs)_<base>_<backend>.bin
   We strip prefix/suffix to recover <base>.  Note: shaders that
   share an FS shader (e.g., pbr_skinned uses fs_pbr) will be
   reloaded under their FS base name; this re-creates the program
   correctly because acquire/reload re-runs load_program_named with
   the same vs/fs split. */
static void shader_watcher_cb(const char *path, void *user_data)
{
    JceShaderManager *mgr = (JceShaderManager *)user_data;
    if (!mgr || !path) return;

    /* Find last '/' or '\\'. */
    const char *fname = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') fname = p + 1;
    }
    /* Expect "vs_<base>_<sfx>.bin" or "fs_<base>_<sfx>.bin". */
    if ((fname[0] != 'v' && fname[0] != 'f') || fname[1] != 's' || fname[2] != '_') {
        return;
    }
    char base[MAX_NAME_LEN];
    snprintf(base, sizeof(base), "%s", fname + 3);
    /* Strip "_<sfx>.bin" — find last '_'. */
    char *last_us = NULL;
    for (char *p = base; *p; p++) if (*p == '_') last_us = p;
    if (last_us) *last_us = '\0';

    jce_shader_manager_reload(mgr, base);
}

int jce_shader_manager_attach_watcher(JceShaderManager *mgr,
                                       JceFileWatcher *watcher)
{
    if (!mgr || !watcher) return 0;
    if (!mgr->has_dev_dir) {
        LOG_INFO(LOG_TAG, "attach_watcher: no dev_dir set, nothing to watch");
        return 0;
    }
    mgr->watcher = watcher;

    /* Backend suffix selection mirrors jce_shaders.c:shader_suffix(). */
    const char *sfx = NULL;
    bgfx_renderer_type_t rt = bgfx_get_renderer_type();
    switch (rt) {
    case BGFX_RENDERER_TYPE_DIRECT3D11:
    case BGFX_RENDERER_TYPE_DIRECT3D12: sfx = "dx11"; break;
    case BGFX_RENDERER_TYPE_VULKAN:     sfx = "spv";  break;
    case BGFX_RENDERER_TYPE_OPENGL:     sfx = "glsl"; break;
    case BGFX_RENDERER_TYPE_OPENGLES:   sfx = "essl"; break;
    case BGFX_RENDERER_TYPE_METAL:      sfx = "mtl";  break;
    default: return 0;
    }

    int registered = 0;
    char path[MAX_PATH_LEN];
    for (int i = 0; i < mgr->count; i++) {
        const char *n = mgr->entries[i].name;

        snprintf(path, sizeof(path),
                 "%s/shaders/vs_%s_%s.bin", mgr->dev_dir, n, sfx);
        if (jce_file_watcher_add(mgr->watcher, path, shader_watcher_cb, mgr))
            registered++;

        snprintf(path, sizeof(path),
                 "%s/shaders/fs_%s_%s.bin", mgr->dev_dir, n, sfx);
        if (jce_file_watcher_add(mgr->watcher, path, shader_watcher_cb, mgr))
            registered++;
    }

    LOG_INFO(LOG_TAG, "attached watcher: %d files registered", registered);
    return registered;
}
