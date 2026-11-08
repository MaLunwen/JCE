/* Exact shared owner release and named-to-variant alias policy. */
#include "unity.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_trace.h>
#ifdef JCE_TEST_SHADER_SET_OWNER
#include JCE_TEST_SHADER_SET_OWNER
#endif
#ifdef JCE_TEST_SHADER_STOCK_OWNER
#include JCE_TEST_SHADER_STOCK_OWNER
#else
#include "renderer/jce_shader_stock_table_internal.h"
#endif
#include <string.h>
static unsigned destroyed[128], refs[128];
static bool check_refs;
void jce_shader_program_destroy(JceShaderHandle h)
{
    TEST_ASSERT_TRUE(h.idx < 128u);
    if (h.idx < 128u) {
        if (check_refs) { TEST_ASSERT_GREATER_THAN_UINT32(0, refs[h.idx]); --refs[h.idx]; }
        ++destroyed[h.idx];
    }
}
void setUp(void)
{
    memset(destroyed, 0, sizeof(destroyed));
    memset(refs, 0, sizeof(refs));
    check_refs = false;
}
void tearDown(void) {}
static void test_named_programs_share_existing_variant_identity(void)
{
    JceShaderSet set;
    memset(&set, 0xff, sizeof(set));
    for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; ++v)
        for (int k = 0; k < JCE_SHADER_KEY_COUNT; ++k)
            set.variant[v][k].idx = (uint16_t)(v*4+k);
    jce_shader_set_alias_stock(&set);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR][0].idx, set.pbr.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_INST][0].idx, set.pbr_inst.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_SKINNED][0].idx, set.pbr_skinned.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_INST_TINT][0].idx, set.pbr_inst_tint.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_INST_FADE][0].idx, set.pbr_inst_fade.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_INST_TEX_ARRAY][0].idx, set.pbr_inst_tex_array.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR][JCE_SHADER_KEY_FWDPLUS].idx, set.pbr_fwdplus.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_INST][JCE_SHADER_KEY_FWDPLUS].idx, set.pbr_inst_fwdplus.idx);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_SKINNED][JCE_SHADER_KEY_FWDPLUS].idx, set.pbr_skinned_fwdplus.idx);
    jce_shader_set_release_owned(&set);
    for (unsigned i = 0; i < 24u; ++i) TEST_ASSERT_EQUAL_UINT32(1, destroyed[i]);
}
static void test_partial_set_covers_sdf_unique_variants_and_zero_handle(void)
{
    JceShaderSet set;
    memset(&set, 0xff, sizeof(set));
    set.color.idx = 0;
    set.text_sdf.idx = 7;
    set.variant[0][0].idx = 7;
    set.variant[4][2].idx = 12;
    set.variant[4][3].idx = 12;
    jce_shader_set_release_owned(&set);
    TEST_ASSERT_EQUAL_UINT32(1, destroyed[0]);
    TEST_ASSERT_EQUAL_UINT32(1, destroyed[7]);
    TEST_ASSERT_EQUAL_UINT32(1, destroyed[12]);
    memset(&set, 0xff, sizeof(set));
    jce_shader_set_release_owned(&set);
    jce_shader_set_release_owned(NULL);
    for (unsigned i = 0; i < 128u; ++i)
        TEST_ASSERT_EQUAL_UINT32(i==0u || i==7u || i==12u, destroyed[i]);
}
static void test_stock_preparation_balances_cached_acquires_without_releasing_borrowed_aliases(void)
{
    JceShaderSet set;
    memset(&set, 0xff, sizeof(set));
    /* bgfx returns one identity with one ref for EVERY successful acquire.
     * These are three independent stock acquires, not three borrowed aliases. */
    JceShaderStockAcquires acquires = {0};
    jce_shader_stock_record_acquire(&acquires, (JceShaderHandle){7});
    jce_shader_stock_record_acquire(&acquires, (JceShaderHandle){7});
    jce_shader_stock_record_acquire(&acquires, (JceShaderHandle){7});
    set.color.idx = 7;
    set.variant[JCE_SHADER_VV_PBR][0].idx = 7;
    set.variant[JCE_SHADER_VV_PBR_INST][0].idx = 7;
    refs[7] = 3;
    check_refs = true;
    TEST_ASSERT_EQUAL_UINT32(2, jce_shader_set_normalize_stock_acquires(&acquires));
    jce_shader_set_alias_stock(&set);
    TEST_ASSERT_EQUAL_UINT16(7, set.pbr.idx);
    TEST_ASSERT_EQUAL_UINT16(7, set.pbr_inst.idx);
    jce_shader_set_release_owned(&set);
    TEST_ASSERT_EQUAL_UINT32(0, refs[7]);
    TEST_ASSERT_EQUAL_UINT32(3, destroyed[7]);
}
static JceShaderHandle fake_stock_pair(void *user, const char *vs, const char *fs)
{
    unsigned *calls = user;
    TEST_ASSERT_NOT_NULL(vs);
    TEST_ASSERT_NOT_NULL(fs);
    ++*calls;
    /* Different named pairs can cache the same binary program identity. */
    ++refs[7];
    return (JceShaderHandle){7};
}
static void test_variant_cells_borrow_one_actual_stock_acquire(void)
{
    JceShaderSet set;
    JceShaderStockAcquires acquires = {0};
    unsigned calls = 0;
    memset(&set, 0xff, sizeof(set));
    check_refs = true;
    jce_shader_stock_load_variants(&set, &acquires, fake_stock_pair, &calls,
        JCE_SHADER_STOCK_PROFILE_FULL);
    /* The production table has 24 cells but only 18 distinct name pairs. */
    TEST_ASSERT_EQUAL_UINT32(18, calls);
    TEST_ASSERT_EQUAL_UINT32(18, acquires.count);
    TEST_ASSERT_EQUAL_UINT32(18, refs[7]);
    TEST_ASSERT_EQUAL_UINT16(set.variant[JCE_SHADER_VV_PBR_INST_TINT][0].idx,
        set.variant[JCE_SHADER_VV_PBR_INST_TINT][JCE_SHADER_KEY_FWDPLUS].idx);
    TEST_ASSERT_EQUAL_UINT32(17, jce_shader_set_normalize_stock_acquires(&acquires));
    jce_shader_set_alias_stock(&set);
    TEST_ASSERT_EQUAL_UINT32(1, refs[7]);
    jce_shader_set_release_owned(&set);
    TEST_ASSERT_EQUAL_UINT32(0, refs[7]);
    TEST_ASSERT_EQUAL_UINT32(18, destroyed[7]);
}
#ifdef JCE_TEST_STOCK_ENTRY_OWNER
#define LOG_TAG "shader-stock-control"
static JceRendererApiInfo stock_api;
static unsigned stock_calls;
static unsigned stock_api_queries;
static bool stock_cache_zero;
static const char *stock_expected_dir;
static const JcePakArchive *stock_expected_pak;
static const char *stock_vs[64], *stock_fs[64];

JceRendererApiInfo jce_renderer_get_api_info(void)
{
    ++stock_api_queries;
    return stock_api;
}
bgfx_renderer_type_t bgfx_get_renderer_type(void)
{
    return stock_api.backend == JCE_BACKEND_OPENGL ? BGFX_RENDERER_TYPE_OPENGL
         : stock_api.backend == JCE_BACKEND_OPENGLES ? BGFX_RENDERER_TYPE_OPENGLES
         : BGFX_RENDERER_TYPE_NOOP;
}
const char *jce_renderer_backend_name(JceRendererBackend backend)
{
    (void)backend;
    return "counted CPU backend input";
}
JceShaderHandle shader_load_program_overlay_named(const char *directory,
    const JcePakArchive *pak, const char *vs, const char *fs)
{
    TEST_ASSERT_EQUAL_STRING(stock_expected_dir, directory);
    TEST_ASSERT_EQUAL_PTR(stock_expected_pak, pak);
    TEST_ASSERT_LESS_THAN_UINT32(64, stock_calls);
    stock_vs[stock_calls] = vs;
    stock_fs[stock_calls] = fs;
    const uint16_t handle = stock_cache_zero ? 0 : (uint16_t)stock_calls;
    ++stock_calls;
    ++refs[handle];
    return (JceShaderHandle){handle};
}
#include JCE_TEST_STOCK_ENTRY_OWNER

static void stock_reset(bool compatibility, bool filesystem, bool cache_zero)
{
    memset(refs, 0, sizeof(refs));
    memset(destroyed, 0, sizeof(destroyed));
    memset(&stock_api, 0, sizeof(stock_api));
    stock_api.build_tier = compatibility ? JCE_GRAPHICS_API_TIER_COMPATIBILITY
                                         : JCE_GRAPHICS_API_TIER_STABLE;
    stock_api.backend = JCE_BACKEND_OPENGL;
    stock_calls = stock_api_queries = 0;
    stock_cache_zero = cache_zero;
    stock_expected_dir = filesystem ? "owned shader overlay" : NULL;
    stock_expected_pak = NULL;
    check_refs = true;
}
static JceShaderSet stock_load_entry(bool filesystem)
{
    return filesystem ? jce_shaders_load_all_fs(stock_expected_dir, stock_expected_pak)
                      : jce_shaders_load_all(stock_expected_pak);
}
static void test_empty_invalid_profile_does_not_destroy_borrowed_zero(void)
{
    JceShaderSet set;
    JceShaderStockAcquires acquires = {0};
    stock_reset(false, false, false);
    jce_shader_stock_load(&set, &acquires, fake_stock_pair, &stock_calls,
                         JCE_SHADER_STOCK_PROFILE_COUNT);
    TEST_ASSERT_EQUAL_UINT32(0, stock_calls);
    TEST_ASSERT_EQUAL_UINT32(0, acquires.count);
#define ASSERT_INVALID(member_, renderer_) TEST_ASSERT_FALSE(jce_shader_valid(set.member_));
    JCE_SHADER_SET_OWNED_FIELDS(ASSERT_INVALID)
#undef ASSERT_INVALID
    for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; ++v)
        for (int k = 0; k < JCE_SHADER_KEY_COUNT; ++k)
            TEST_ASSERT_FALSE(jce_shader_valid(set.variant[v][k]));
    jce_shader_set_release_owned(&set);
    TEST_ASSERT_EQUAL_UINT32(0, destroyed[0]);
    jce_shader_set_init_invalid(NULL);
}
static void test_profile_requires_compiled_compatibility_and_actual_gl_family(void)
{
    static const JceGraphicsApiTier tiers[] = {
        JCE_GRAPHICS_API_TIER_STABLE, JCE_GRAPHICS_API_TIER_MODERN,
        JCE_GRAPHICS_API_TIER_CURRENT, JCE_GRAPHICS_API_TIER_COUNT,
        JCE_GRAPHICS_API_TIER_COMPATIBILITY
    };
    for (size_t i = 0; i < sizeof tiers / sizeof tiers[0]; ++i) {
        const JceGraphicsApiTier tier = tiers[i];
        for (int backend = 0; backend < BGFX_RENDERER_TYPE_COUNT; ++backend) {
            const bool compatibility = tier == JCE_GRAPHICS_API_TIER_COMPATIBILITY &&
                (backend == BGFX_RENDERER_TYPE_OPENGL || backend == BGFX_RENDERER_TYPE_OPENGLES);
            JceRendererApiInfo info = {0};
            info.build_tier = tier;
            TEST_ASSERT_EQUAL_INT(compatibility ? JCE_SHADER_STOCK_PROFILE_COMPATIBILITY
                                                : JCE_SHADER_STOCK_PROFILE_FULL,
                jce_shader_stock_profile_for_api(&info, (bgfx_renderer_type_t)backend));
        }
    }
}
static void test_both_entries_preserve_full_actual_request_roster_and_zero_owner(void)
{
    static const char *const expected[28][2] = {
        {"pbr", "pbr"}, {"pbr", "pbr_fwdplus"}, {"pbr", "pbr_noshadow"}, {"pbr", "pbr_fwdplus_noshadow"},
        {"pbr_inst", "pbr"}, {"pbr_inst", "pbr_fwdplus"}, {"pbr_inst", "pbr_noshadow"}, {"pbr_inst", "pbr_fwdplus_noshadow"},
        {"pbr_skinned", "pbr"}, {"pbr_skinned", "pbr_fwdplus"}, {"pbr_skinned", "pbr_noshadow"}, {"pbr_skinned", "pbr_fwdplus_noshadow"},
        {"pbr_inst_tint", "pbr_tint"}, {"pbr_inst_tint", "pbr_tint_noshadow"},
        {"pbr_inst_fade", "pbr_fade"}, {"pbr_inst_fade", "pbr_fade_noshadow"},
        {"pbr_inst_tex_array", "pbr_inst_tex_array"}, {"pbr_inst_tex_array", "pbr_inst_tex_array_noshadow"},
        {"color", "color"}, {"textured", "textured"}, {"textured", "text_sdf"}, {"mesh", "mesh"},
        {"pbr_skinned", "pbr_toon"}, {"pbr_skinned_outline", "outline"}, {"shadow", "shadow"},
        {"shadow_inst", "shadow"}, {"shadow_skinned", "shadow"}, {"terrain", "terrain"}
    };
    for (int filesystem = 0; filesystem < 2; ++filesystem) {
        stock_reset(false, filesystem != 0, false);
        JceShaderSet set = stock_load_entry(filesystem != 0);
        TEST_ASSERT_EQUAL_UINT32(28, stock_calls);
        TEST_ASSERT_EQUAL_UINT32(1, stock_api_queries);
        for (unsigned i = 0; i < 28; ++i) {
            TEST_ASSERT_EQUAL_STRING(expected[i][0], stock_vs[i]);
            TEST_ASSERT_EQUAL_STRING(expected[i][1], stock_fs[i]);
        }
        TEST_ASSERT_TRUE(jce_shader_valid(set.pbr));
        TEST_ASSERT_EQUAL_UINT16(0, set.pbr.idx);
        jce_shader_set_release_owned(&set);
        for (unsigned i = 0; i < 128; ++i) {
            TEST_ASSERT_EQUAL_UINT32(0, refs[i]);
            TEST_ASSERT_EQUAL_UINT32(i < 28 ? 1 : 0, destroyed[i]);
        }
    }
}
static void test_both_compatibility_entries_skip_unsupported_and_balance_cached_zero(void)
{
    static const char *const expected[7][2] = {
        {"pbr", "pbr"}, {"pbr", "pbr_noshadow"}, {"color", "color"},
        {"textured", "textured"}, {"textured", "text_sdf"}, {"mesh", "mesh"}, {"shadow", "shadow"}
    };
    for (int filesystem = 0; filesystem < 2; ++filesystem) {
        stock_reset(true, filesystem != 0, true);
        JceShaderSet set = stock_load_entry(filesystem != 0);
        TEST_ASSERT_EQUAL_UINT32(7, stock_calls);
        TEST_ASSERT_EQUAL_UINT32(1, stock_api_queries);
        for (unsigned i = 0; i < 7; ++i) {
            TEST_ASSERT_EQUAL_STRING(expected[i][0], stock_vs[i]);
            TEST_ASSERT_EQUAL_STRING(expected[i][1], stock_fs[i]);
        }
        TEST_ASSERT_TRUE(jce_shader_valid(set.pbr));
        TEST_ASSERT_TRUE(jce_shader_valid(set.variant[JCE_SHADER_VV_PBR][JCE_SHADER_KEY_NOSHADOW]));
        TEST_ASSERT_FALSE(jce_shader_valid(set.pbr_fwdplus));
        TEST_ASSERT_FALSE(jce_shader_valid(set.pbr_inst));
        TEST_ASSERT_FALSE(jce_shader_valid(set.pbr_skinned));
        TEST_ASSERT_FALSE(jce_shader_valid(set.pbr_inst_tex_array));
        TEST_ASSERT_FALSE(jce_shader_valid(set.pbr_toon));
        TEST_ASSERT_FALSE(jce_shader_valid(set.outline_skinned));
        TEST_ASSERT_FALSE(jce_shader_valid(set.terrain));
        for (int v = 0; v < JCE_SHADER_VARIANT_COUNT; ++v)
            for (int k = 0; k < JCE_SHADER_KEY_COUNT; ++k)
                if (v != JCE_SHADER_VV_PBR || (k != 0 && k != JCE_SHADER_KEY_NOSHADOW))
                    TEST_ASSERT_FALSE(jce_shader_valid(set.variant[v][k]));
        TEST_ASSERT_EQUAL_UINT32(1, refs[0]);
        TEST_ASSERT_EQUAL_UINT32(6, destroyed[0]);
        jce_shader_set_release_owned(&set);
        TEST_ASSERT_EQUAL_UINT32(0, refs[0]);
        TEST_ASSERT_EQUAL_UINT32(7, destroyed[0]);
        for (unsigned i = 1; i < 128; ++i) TEST_ASSERT_EQUAL_UINT32(0, destroyed[i]);
    }
}
#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_named_programs_share_existing_variant_identity);
    RUN_TEST(test_partial_set_covers_sdf_unique_variants_and_zero_handle);
    RUN_TEST(test_stock_preparation_balances_cached_acquires_without_releasing_borrowed_aliases);
    RUN_TEST(test_variant_cells_borrow_one_actual_stock_acquire);
#ifdef JCE_TEST_STOCK_ENTRY_OWNER
    RUN_TEST(test_empty_invalid_profile_does_not_destroy_borrowed_zero);
    RUN_TEST(test_profile_requires_compiled_compatibility_and_actual_gl_family);
    RUN_TEST(test_both_entries_preserve_full_actual_request_roster_and_zero_owner);
    RUN_TEST(test_both_compatibility_entries_skip_unsupported_and_balance_cached_zero);
#endif
    return UNITY_END();
}
