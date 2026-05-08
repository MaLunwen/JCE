/*
 * jce_material_property_block.c  Sparse uniform-override storage.
 *
 * Linear-search each lookup since N <= 8 — branch-predictor handles it
 * fine and a hash table here would be overkill for the typical case
 * (1–3 overrides per renderer instance).
 */

#include <jce/renderer/jce_material_property_block.h>

#include <string.h>

void jce_mpb_clear(JceMaterialPropertyBlock *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
}

static int find_float(JceMaterialPropertyBlock *b, const char *name)
{
    for (uint8_t i = 0; i < b->float_count; ++i)
        if (strncmp(b->floats[i].name, name, JCE_MPB_NAME_MAX) == 0) return (int)i;
    return -1;
}

static int find_texture(JceMaterialPropertyBlock *b, const char *name)
{
    for (uint8_t i = 0; i < b->texture_count; ++i)
        if (strncmp(b->textures[i].name, name, JCE_MPB_NAME_MAX) == 0) return (int)i;
    return -1;
}

static bool put_float(JceMaterialPropertyBlock *b, const char *name,
                      const float v[4], uint8_t components)
{
    if (!b || !name) return false;
    int existing = find_float(b, name);
    JceMpbFloat4 *slot;
    if (existing >= 0) {
        slot = &b->floats[existing];
    } else {
        if (b->float_count >= JCE_MPB_MAX_FLOAT4) return false;
        slot = &b->floats[b->float_count++];
        strncpy(slot->name, name, JCE_MPB_NAME_MAX - 1);
        slot->name[JCE_MPB_NAME_MAX - 1] = '\0';
    }
    memcpy(slot->value, v, sizeof(slot->value));
    slot->components = components;
    return true;
}

bool jce_mpb_set_float(JceMaterialPropertyBlock *b, const char *name, float v)
{
    float buf[4] = { v, 0.0f, 0.0f, 0.0f };
    return put_float(b, name, buf, 1);
}

bool jce_mpb_set_vec3(JceMaterialPropertyBlock *b, const char *name, jce_vec3 v)
{
    float buf[4] = { v.x, v.y, v.z, 0.0f };
    return put_float(b, name, buf, 3);
}

bool jce_mpb_set_vec4(JceMaterialPropertyBlock *b, const char *name, jce_vec4 v)
{
    float buf[4] = { v.x, v.y, v.z, v.w };
    return put_float(b, name, buf, 4);
}

bool jce_mpb_set_color(JceMaterialPropertyBlock *b, const char *name,
                       float r, float g, float bl, float a)
{
    float buf[4] = { r, g, bl, a };
    return put_float(b, name, buf, 4);
}

bool jce_mpb_set_texture(JceMaterialPropertyBlock *b, const char *name,
                         uint16_t handle, uint8_t stage)
{
    if (!b || !name) return false;
    int existing = find_texture(b, name);
    JceMpbTexture *slot;
    if (existing >= 0) {
        slot = &b->textures[existing];
    } else {
        if (b->texture_count >= JCE_MPB_MAX_TEXTURE) return false;
        slot = &b->textures[b->texture_count++];
        strncpy(slot->name, name, JCE_MPB_NAME_MAX - 1);
        slot->name[JCE_MPB_NAME_MAX - 1] = '\0';
    }
    slot->bgfx_handle = handle;
    slot->stage       = stage;
    return true;
}

bool jce_mpb_get_float4(const JceMaterialPropertyBlock *b,
                        const char *name, float out[4])
{
    if (!b || !name || !out) return false;
    for (uint8_t i = 0; i < b->float_count; ++i) {
        if (strncmp(b->floats[i].name, name, JCE_MPB_NAME_MAX) == 0) {
            memcpy(out, b->floats[i].value, sizeof(b->floats[i].value));
            return true;
        }
    }
    return false;
}

bool jce_mpb_is_empty(const JceMaterialPropertyBlock *b)
{
    return !b || (b->float_count == 0 && b->texture_count == 0);
}
