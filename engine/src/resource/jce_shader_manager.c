/*
 * jce_shader_manager.c  Shader resource cache implementation.
 */

#include "jce_shader_manager.h"
#include <jce/renderer/jce_shaders.h>
#include <jce/os/core/jce_log.h>

#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <string.h>
#include "os/core/jce_memory.h"

#define LOG_TAG "shader_mgr"
#define MAX_SHADERS 64
#define MAX_NAME_LEN 64

typedef struct {
    char            name[MAX_NAME_LEN];
    JceShaderHandle handle;
    int             ref_count;
} ShaderEntry;

struct JceShaderManager {
    const JcePakArchive *pak;
    ShaderEntry       entries[MAX_SHADERS];
    int               count;
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

    JceShaderHandle h = shader_load_program(mgr->pak, name);
    if (!jce_shader_valid(h)) return JCE_INVALID_SHADER;

    ShaderEntry *e = &mgr->entries[mgr->count++];
    snprintf(e->name, MAX_NAME_LEN, "%s", name);
    e->handle    = h;
    e->ref_count = 1;

    LOG_INFO(LOG_TAG, "loaded shader '%s'", name);
    return h;
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
