/* Stock table loading and actual acquisition accounting, shared by controls. */
#ifndef JCE_SHADER_STOCK_TABLE_INTERNAL_H
#define JCE_SHADER_STOCK_TABLE_INTERNAL_H
#include "jce_shader_set_owned_internal.h"
#include "jce_shader_variants_table.inc"
#include "jce_shader_stock_profiles.inc"
#include "jce_renderer_shader_profile_internal.h"
#include <string.h>

typedef JceShaderHandle (*JceShaderStockLoadPair)(void *, const char *, const char *);

static inline JceShaderStockProfile jce_shader_stock_profile_for_api(
    const JceRendererApiInfo *api, bgfx_renderer_type_t backend)
{
    return jce_renderer_shader_profile_compatibility(api, backend)
        ? JCE_SHADER_STOCK_PROFILE_COMPATIBILITY : JCE_SHADER_STOCK_PROFILE_FULL;
}

static inline bool jce_shader_stock_variant_selected(JceShaderStockProfile profile,
    int variant, int key)
{
    return (unsigned)profile < JCE_SHADER_STOCK_PROFILE_COUNT &&
           (unsigned)variant < JCE_SHADER_VARIANT_COUNT &&
           (unsigned)key < JCE_SHADER_KEY_COUNT &&
           jce_shader_stock_pbr_selected[profile][variant][key] != 0;
}

static inline void jce_shader_stock_load_variants(JceShaderSet *set,
    JceShaderStockAcquires *batch, JceShaderStockLoadPair load, void *user,
    JceShaderStockProfile profile)
{
    struct { const char *vs, *fs; JceShaderHandle handle; } seen[
        JCE_SHADER_VARIANT_COUNT * JCE_SHADER_KEY_COUNT];
    size_t count = 0;
    for (int variant = 0; variant < JCE_SHADER_VARIANT_COUNT; ++variant) {
        const char *vs = jce_shader_vv_name[variant];
        for (int key = 0; key < JCE_SHADER_KEY_COUNT; ++key) {
            if (!jce_shader_stock_variant_selected(profile, variant, key)) continue;
            const char *fs = jce_shader_fs_for[variant][key];
            size_t prior = 0;
            while (prior < count && (strcmp(seen[prior].vs, vs) ||
                                     strcmp(seen[prior].fs, fs))) ++prior;
            if (prior == count) {
                seen[count].vs = vs;
                seen[count].fs = fs;
                seen[count].handle = jce_shader_stock_record_acquire(batch,
                    load(user, vs, fs));
                ++count;
            }
            set->variant[variant][key] = seen[prior].handle;
        }
    }
}
/* Every stock entry route starts empty; skipped cells never acquire handle zero. */
static inline void jce_shader_stock_load(JceShaderSet *set,
    JceShaderStockAcquires *batch, JceShaderStockLoadPair load, void *user,
    JceShaderStockProfile profile)
{
    jce_shader_set_init_invalid(set);
    if ((unsigned)profile >= JCE_SHADER_STOCK_PROFILE_COUNT) return;
    jce_shader_stock_load_variants(set, batch, load, user, profile);
#define JCE_LOAD_STOCK_MEMBER(member_, vs_, fs_, mask_) \
    if ((mask_) & (1u << (unsigned)profile)) { \
        set->member_ = jce_shader_stock_record_acquire(batch, load(user, vs_, fs_)); \
    }
    JCE_SHADER_STOCK_SCALAR_PAIRS(JCE_LOAD_STOCK_MEMBER)
#undef JCE_LOAD_STOCK_MEMBER
}

#endif
