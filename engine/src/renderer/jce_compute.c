/*
 * jce_compute.c  Compute dispatch descriptor — data-layer only.
 */

#include <jce/renderer/jce_compute.h>

#include <string.h>

void jce_compute_dispatch_init(JceComputeDispatch *d, const char *shader_name)
{
    if (!d) return;
    memset(d, 0, sizeof(*d));
    if (shader_name)
        strncpy(d->shader_name, shader_name, JCE_COMPUTE_NAME_LEN - 1);
    d->workgroup_x = 1;
    d->workgroup_y = 1;
    d->workgroup_z = 1;
}

void jce_compute_set_workgroup(JceComputeDispatch *d,
                                uint32_t x, uint32_t y, uint32_t z)
{
    if (!d) return;
    d->workgroup_x = x ? x : 1;
    d->workgroup_y = y ? y : 1;
    d->workgroup_z = z ? z : 1;
}

static int find_buffer_slot(JceComputeDispatch *d, const char *name)
{
    int free_slot = -1;
    for (int i = 0; i < JCE_COMPUTE_MAX_BUFFERS; ++i) {
        if (d->buffers[i].active &&
            strncmp(d->buffers[i].name, name, JCE_COMPUTE_NAME_LEN) == 0)
            return i;
        if (!d->buffers[i].active && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

int jce_compute_set_buffer(JceComputeDispatch *d, const char *name,
                            uint32_t handle, uint32_t stride,
                            uint32_t element_count, JceComputeAccess access)
{
    if (!d || !name || !name[0]) return -1;
    int s = find_buffer_slot(d, name);
    if (s < 0) return -1;
    JceComputeBufferBinding *b = &d->buffers[s];
    strncpy(b->name, name, JCE_COMPUTE_NAME_LEN - 1);
    b->name[JCE_COMPUTE_NAME_LEN - 1] = '\0';
    b->handle        = handle;
    b->stride        = stride;
    b->element_count = element_count;
    b->access        = access;
    b->active        = true;
    return s;
}

static int find_tex_slot(JceComputeDispatch *d, const char *name)
{
    int free_slot = -1;
    for (int i = 0; i < JCE_COMPUTE_MAX_TEXTURES; ++i) {
        if (d->textures[i].active &&
            strncmp(d->textures[i].name, name, JCE_COMPUTE_NAME_LEN) == 0)
            return i;
        if (!d->textures[i].active && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

int jce_compute_set_texture(JceComputeDispatch *d, const char *name,
                             uint32_t handle, uint8_t mip,
                             JceComputeAccess access)
{
    if (!d || !name || !name[0]) return -1;
    int s = find_tex_slot(d, name);
    if (s < 0) return -1;
    JceComputeTextureBinding *t = &d->textures[s];
    strncpy(t->name, name, JCE_COMPUTE_NAME_LEN - 1);
    t->name[JCE_COMPUTE_NAME_LEN - 1] = '\0';
    t->handle = handle;
    t->mip    = mip;
    t->access = access;
    t->active = true;
    return s;
}

static int find_const_slot(JceComputeDispatch *d, const char *name)
{
    int free_slot = -1;
    for (int i = 0; i < JCE_COMPUTE_MAX_CONSTS; ++i) {
        if (d->consts[i].active &&
            strncmp(d->consts[i].name, name, JCE_COMPUTE_NAME_LEN) == 0)
            return i;
        if (!d->consts[i].active && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

int jce_compute_set_float(JceComputeDispatch *d, const char *name, float v)
{
    return jce_compute_set_vec4(d, name, v, 0, 0, 0);
}

int jce_compute_set_vec4(JceComputeDispatch *d, const char *name,
                          float x, float y, float z, float w)
{
    if (!d || !name || !name[0]) return -1;
    int s = find_const_slot(d, name);
    if (s < 0) return -1;
    JceComputeConstant *c = &d->consts[s];
    strncpy(c->name, name, JCE_COMPUTE_NAME_LEN - 1);
    c->name[JCE_COMPUTE_NAME_LEN - 1] = '\0';
    c->value[0] = x;
    c->value[1] = y;
    c->value[2] = z;
    c->value[3] = w;
    c->active   = true;
    return s;
}

uint32_t jce_compute_active_buffer_count(const JceComputeDispatch *d)
{
    if (!d) return 0;
    uint32_t n = 0;
    for (int i = 0; i < JCE_COMPUTE_MAX_BUFFERS; ++i)
        if (d->buffers[i].active) n++;
    return n;
}

uint32_t jce_compute_active_texture_count(const JceComputeDispatch *d)
{
    if (!d) return 0;
    uint32_t n = 0;
    for (int i = 0; i < JCE_COMPUTE_MAX_TEXTURES; ++i)
        if (d->textures[i].active) n++;
    return n;
}

uint32_t jce_compute_active_const_count(const JceComputeDispatch *d)
{
    if (!d) return 0;
    uint32_t n = 0;
    for (int i = 0; i < JCE_COMPUTE_MAX_CONSTS; ++i)
        if (d->consts[i].active) n++;
    return n;
}
