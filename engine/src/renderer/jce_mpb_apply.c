/*
 * jce_mpb_apply.c  Flatten material property block to uniform list.
 *
 * Floats / vec3 / vec4 all land in the same vec4 slot — caller's
 * shader binding code reads `components` if it cares; the apply
 * layer treats every entry uniformly.  Texture entries carry the
 * bgfx handle in value[0] (cast through uint as float bits).
 */

#include <jce/renderer/jce_mpb_apply.h>

#include <string.h>

uint32_t jce_mpb_apply_flatten(const JceMaterialPropertyBlock *b,
                                JceMpbUniformList *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!b) return 0;

    /* Floats / vec*. */
    for (uint8_t i = 0; i < b->float_count &&
                          out->count < JCE_MPB_APPLY_MAX_UNIFORMS; ++i) {
        JceMpbUniformEntry *e = &out->entries[out->count++];
        strncpy(e->name, b->floats[i].name, JCE_MPB_APPLY_NAME_LEN - 1);
        e->name[JCE_MPB_APPLY_NAME_LEN - 1] = '\0';
        e->kind = JCE_MPB_UNIFORM_VEC4;
        memcpy(e->value, b->floats[i].value, 4 * sizeof(float));
    }
    /* Textures — handle stored as raw uint16 in a float slot. */
    for (uint8_t i = 0; i < b->texture_count &&
                          out->count < JCE_MPB_APPLY_MAX_UNIFORMS; ++i) {
        JceMpbUniformEntry *e = &out->entries[out->count++];
        strncpy(e->name, b->textures[i].name, JCE_MPB_APPLY_NAME_LEN - 1);
        e->name[JCE_MPB_APPLY_NAME_LEN - 1] = '\0';
        e->kind = JCE_MPB_UNIFORM_TEXTURE;
        /* Pack handle in value[0] (as float bits) and stage in value[1]. */
        union { float f; uint32_t u; } pun;
        pun.u = b->textures[i].bgfx_handle;
        e->value[0] = pun.f;
        e->value[1] = (float)b->textures[i].stage;
    }
    return out->count;
}
