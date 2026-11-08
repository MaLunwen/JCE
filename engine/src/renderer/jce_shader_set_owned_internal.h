/* One shader-set owner: named ABI handles alias the variant table. */
#ifndef JCE_SHADER_SET_OWNED_INTERNAL_H
#define JCE_SHADER_SET_OWNED_INTERNAL_H
#include <jce/renderer/jce_shaders.h>
#include <stddef.h>
#include <string.h>

/* Set member, renderer member. Includes optional SDF and every named program. */
#define JCE_SHADER_SET_OWNED_FIELDS(X) \
    X(color, program) \
    X(textured, program_textured) \
    X(mesh, program_mesh) \
    X(pbr, program_pbr) \
    X(pbr_inst, program_pbr_inst) \
    X(pbr_inst_tint, program_pbr_inst_tint) \
    X(pbr_inst_tex_array, program_pbr_inst_tex_array) \
    X(pbr_inst_fade, program_pbr_inst_fade) \
    X(pbr_skinned, program_pbr_skinned) \
    X(pbr_fwdplus, program_pbr_fwdplus) \
    X(pbr_inst_fwdplus, program_pbr_inst_fwdplus) \
    X(pbr_skinned_fwdplus, program_pbr_skinned_fwdplus) \
    X(pbr_toon, program_pbr_toon) \
    X(outline_skinned, program_outline_skinned) \
    X(shadow, program_shadow) \
    X(shadow_inst, program_shadow_inst) \
    X(shadow_skinned, program_shadow_skinned) \
    X(terrain, program_terrain) \
    X(text_sdf, program_text_sdf)

enum {
    JCE_SHADER_SET_NAMED_COUNT = 0
#define JCE_COUNT_SHADER_MEMBER(member_, renderer_) + 1
        JCE_SHADER_SET_OWNED_FIELDS(JCE_COUNT_SHADER_MEMBER)
#undef JCE_COUNT_SHADER_MEMBER
};

static inline void jce_shader_set_init_invalid(JceShaderSet *set)
{
    if (!set) return;
    memset(set, 0, sizeof(*set));
#define JCE_INIT_SHADER_MEMBER(member_, renderer_) set->member_ = JCE_INVALID_SHADER;
    JCE_SHADER_SET_OWNED_FIELDS(JCE_INIT_SHADER_MEMBER)
#undef JCE_INIT_SHADER_MEMBER
    for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; ++v)
        for (int k = 0; k < JCE_SHADER_KEY_COUNT; ++k)
            set->variant[v][k] = JCE_INVALID_SHADER;
}

/* Only successful loader calls acquire; table/scalar aliases merely borrow. */
typedef struct JceShaderStockAcquires {
    JceShaderHandle handles[JCE_SHADER_SET_NAMED_COUNT + JCE_SHADER_VARIANT_COUNT * JCE_SHADER_KEY_COUNT];
    size_t count;
} JceShaderStockAcquires;

static inline JceShaderHandle jce_shader_stock_record_acquire(
    JceShaderStockAcquires *batch, JceShaderHandle handle)
{
    if (jce_shader_valid(handle)) batch->handles[batch->count++] = handle;
    return handle;
}

static inline unsigned jce_shader_set_normalize_stock_acquires(
    const JceShaderStockAcquires *batch)
{
    unsigned duplicates = 0;
    if (!batch) return 0;
    for (size_t i = 0; i < batch->count; ++i) {
        for (size_t prior = 0; prior < i; ++prior) {
            if (batch->handles[prior].idx == batch->handles[i].idx) {
                jce_shader_program_destroy(batch->handles[i]);
                ++duplicates;
                break;
            }
        }
    }
    return duplicates;
}

static inline void jce_shader_set_alias_stock(JceShaderSet *set)
{
    set->pbr = set->variant[JCE_SHADER_VV_PBR][0u];
    set->pbr_inst = set->variant[JCE_SHADER_VV_PBR_INST][0u];
    set->pbr_skinned = set->variant[JCE_SHADER_VV_PBR_SKINNED][0u];
    set->pbr_inst_tint = set->variant[JCE_SHADER_VV_PBR_INST_TINT][0u];
    set->pbr_inst_fade = set->variant[JCE_SHADER_VV_PBR_INST_FADE][0u];
    set->pbr_inst_tex_array = set->variant[JCE_SHADER_VV_PBR_INST_TEX_ARRAY][0u];
    set->pbr_fwdplus = set->variant[JCE_SHADER_VV_PBR][JCE_SHADER_KEY_FWDPLUS];
    set->pbr_inst_fwdplus = set->variant[JCE_SHADER_VV_PBR_INST][JCE_SHADER_KEY_FWDPLUS];
    set->pbr_skinned_fwdplus = set->variant[JCE_SHADER_VV_PBR_SKINNED][JCE_SHADER_KEY_FWDPLUS];
}

static inline void jce_shader_set_release_owned(const JceShaderSet *set)
{
    JceShaderHandle handles[JCE_SHADER_SET_NAMED_COUNT + JCE_SHADER_VARIANT_COUNT * JCE_SHADER_KEY_COUNT];
    size_t count = 0;
    if (!set) return;
#define JCE_APPEND_PROGRAM(member_, renderer_) handles[count++] = set->member_;
    JCE_SHADER_SET_OWNED_FIELDS(JCE_APPEND_PROGRAM)
#undef JCE_APPEND_PROGRAM
    for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; ++v)
        for (int k = 0; k < JCE_SHADER_KEY_COUNT; ++k)
            handles[count++] = set->variant[v][k];
    for (size_t i = 0; i < count; ++i) {
        bool seen = false;
        if (!jce_shader_valid(handles[i])) continue;
        for (size_t previous = 0; previous < i; ++previous)
            if (handles[previous].idx == handles[i].idx) { seen = true; break; }
        if (!seen) jce_shader_program_destroy(handles[i]);
    }
}
#endif
