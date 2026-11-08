/* Production resource-resolution owner with a one-level font-chain port.
 * Complementary face coverage is controlled input, not a rasterizer claim. */
#include "jce_ui_canvas_resources.h"
#include "jce_ui_canvas_font_cache.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/platform/jce_host_font.h>
#include "unity.h"
#include <string.h>

struct JceFont {
    unsigned coverage;
    JceFont *fallback[JCE_FONT_MAX_FALLBACKS];
    unsigned count;
};
static JceFont primary, first, second, stock;
static unsigned leaf_lookups, accepted, closes;
static unsigned char canvas_token;
static UCFontCache cache;
static bool fail_open;
static unsigned opened;
static bool system_installed, missing_symbols, failed_symbol_leaf;
static bool stock_available;
static unsigned stock_lookups;
static unsigned stock_pak_opens;
static unsigned role_calls[3];

bool jce_host_default_font(JceHostFontRole role, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    if ((unsigned)role >= 3) return false;
    ++role_calls[role];
    if (!system_installed || (role == JCE_HOST_FONT_UI_SYMBOLS && missing_symbols)) return false;
    const char *path = role == JCE_HOST_FONT_UI_LATIN ? "/system/latin.ttf"
        : role == JCE_HOST_FONT_UI_CJK ? "fonts/F1.ttf" : "fonts/F2.ttf";
    if (!out || strlen(path) + 1 > cap) return false;
    strcpy(out, path); return true;
}

void jce_font_set_sdf_spread(int spread) { (void)spread; }
JceFont *jce_font_open_ex(const JcePakArchive *pak, const char *path, float px,
                         const uint32_t *points, int count)
{
    (void)px; (void)points; (void)count;
    if (strcmp(path, "fonts/JCE-CJK-Fallback.ttf") == 0) {
        TEST_ASSERT_NOT_NULL(pak);
        ++stock_pak_opens;
        return &stock;
    }
    return NULL;
}
JceFont *jce_font_open_file_ex(const char *path, float px,
                              const uint32_t *points, int count)
{
    (void)path; (void)px; (void)points; (void)count;
    return NULL;
}
JceFont *jce_font_open_mem_ex(const void *bytes, size_t size, const char *label,
                             float px, const uint32_t *points, int count)
{
    (void)bytes; (void)size; (void)label; (void)px; (void)points; (void)count;
    return NULL;
}
JceFont *uc_get_font_leaf(JceUICanvas *owner, const char *path, int px, bool sdf)
{
    TEST_ASSERT_EQUAL_PTR(&canvas_token, owner);
    TEST_ASSERT_EQUAL_INT(24, px);
    TEST_ASSERT_FALSE(sdf);
    if (strcmp(path, "@stock-cjk") == 0) {
        ++stock_lookups;
        return stock_available ? &stock : NULL;
    }
    ++leaf_lookups;
    if (strcmp(path, "fonts/F1.ttf") == 0) return &first;
    if (strcmp(path, "fonts/F2.ttf") == 0) return failed_symbol_leaf ? NULL : &second;
    TEST_FAIL_MESSAGE("unexpected fallback path");
    return NULL;
}
bool jce_font_add_fallback(JceFont *font, JceFont *fallback)
{
    /* Independent contract guard: the renderer refuses nested fallback lists.
     * The actual resources owner must supply both faces as leaves. */
    if (!font || !fallback || font == fallback || fallback->count ||
        font->count >= JCE_FONT_MAX_FALLBACKS) return false;
    for (unsigned i = 0; i < font->count; ++i)
        if (font->fallback[i] == fallback) return true;
    font->fallback[font->count++] = fallback;
    ++accepted;
    return true;
}
void jce_font_close(JceFont *font)
{
    TEST_ASSERT_EQUAL_PTR(&primary, font);
    ++closes;
}
static JceFont *open_cached(void *user, const char *path, int px, bool sdf)
{
    (void)user; (void)path; (void)px; (void)sdf;
    ++opened;
    return fail_open ? NULL : &primary;
}
static unsigned flat_coverage(const JceFont *font)
{
    unsigned coverage = font->coverage;
    for (unsigned i = 0; i < font->count; ++i) {
        TEST_ASSERT_EQUAL_UINT(0, font->fallback[i]->count);
        coverage |= font->fallback[i]->coverage;
    }
    return coverage;
}
void setUp(void)
{
    memset(&primary, 0, sizeof primary);
    memset(&first, 0, sizeof first);
    memset(&second, 0, sizeof second);
    memset(&stock, 0, sizeof stock);
    memset(&cache, 0, sizeof cache);
    primary.coverage = 1;
    first.coverage = 2;
    second.coverage = 4;
    stock.coverage = 6;
    leaf_lookups = accepted = closes = opened = 0;
    fail_open = false;
    system_installed = missing_symbols = failed_symbol_leaf = false;
    stock_available = false;
    stock_lookups = stock_pak_opens = 0;
    memset(role_calls, 0, sizeof role_calls);
    jce_ui_canvas_set_default_font(NULL);
    jce_ui_canvas_set_font_fallbacks(NULL);
    jce_ui_canvas_invalidate_resources();
}
void tearDown(void)
{
    uc_font_cache_clear(&cache);
    jce_ui_canvas_set_font_fallbacks(NULL);
    jce_ui_canvas_set_default_font(NULL);
}
static void two_complementary_fallbacks_are_flat_and_both_accepted(void)
{
    jce_ui_canvas_set_font_fallbacks("fonts/F1.ttf;fonts/F2.ttf");
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary,
                         "fonts/Main.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(2, leaf_lookups);
    TEST_ASSERT_EQUAL_UINT(2, accepted);
    TEST_ASSERT_EQUAL_UINT(2, primary.count);
    TEST_ASSERT_EQUAL_UINT(7, flat_coverage(&primary));
    TEST_ASSERT_EQUAL_PTR(&first, primary.fallback[0]);
    TEST_ASSERT_EQUAL_PTR(&second, primary.fallback[1]);
}
static void the_primary_path_is_not_added_to_its_own_fallback_list(void)
{
    jce_ui_canvas_set_font_fallbacks("fonts/F1.ttf;fonts/F2.ttf");
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &first,
                         "fonts/F1.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(1, leaf_lookups);
    TEST_ASSERT_EQUAL_UINT(1, first.count);
    TEST_ASSERT_EQUAL_PTR(&second, first.fallback[0]);
    TEST_ASSERT_FALSE(jce_font_add_fallback(&first, &first));
    TEST_ASSERT_FALSE(jce_font_add_fallback(&primary, &first));
}
static void explicit_invalidation_retries_negative_results_and_closes_positive_once(void)
{
    unsigned generation = uc_resource_generation();
    TEST_ASSERT_TRUE(uc_font_cache_bind(&cache, NULL, 0, generation));
    fail_open = true;
    TEST_ASSERT_NULL(uc_font_cache_get(&cache, "fonts/new.ttf", 24, false, false,
                                       open_cached, NULL, NULL));
    fail_open = false;
    TEST_ASSERT_NULL(uc_font_cache_get(&cache, "fonts/new.ttf", 24, false, false,
                                       open_cached, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(1, opened);
    jce_ui_canvas_invalidate_resources();
    TEST_ASSERT_NOT_EQUAL_UINT(generation, uc_resource_generation());
    TEST_ASSERT_TRUE(uc_font_cache_bind(&cache, NULL, 0, uc_resource_generation()));
    TEST_ASSERT_EQUAL_PTR(&primary, uc_font_cache_get(&cache, "fonts/new.ttf",
                          24, false, false, open_cached, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(2, opened);
    jce_ui_canvas_invalidate_resources();
    TEST_ASSERT_TRUE(uc_font_cache_bind(&cache, NULL, 0, uc_resource_generation()));
    TEST_ASSERT_EQUAL_UINT(1, closes);
    TEST_ASSERT_FALSE(uc_font_cache_bind(&cache, NULL, 0, uc_resource_generation()));
    TEST_ASSERT_EQUAL_UINT(1, closes);
}
static void default_chain_attaches_cjk_then_symbols_and_resolves_once_per_epoch(void)
{
    system_installed = true;
    char path[1024]; TEST_ASSERT_TRUE(uc_resource_system_ui_font(path, sizeof path));
    TEST_ASSERT_EQUAL_STRING("/system/latin.ttf", path);
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary, path, 24, false);
    TEST_ASSERT_EQUAL_UINT(2, primary.count); TEST_ASSERT_EQUAL_UINT(7, flat_coverage(&primary));
    TEST_ASSERT_EQUAL_PTR(&first, primary.fallback[0]); TEST_ASSERT_EQUAL_PTR(&second, primary.fallback[1]);
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT(1, role_calls[i]);
    TEST_ASSERT_TRUE(uc_resource_system_ui_font(path, sizeof path));
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT(1, role_calls[i]);
    jce_ui_canvas_invalidate_resources(); TEST_ASSERT_TRUE(uc_resource_system_ui_font(path, sizeof path));
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT(2, role_calls[i]);
}
static void symbols_unavailable_or_failed_leaf_keeps_primary_and_cjk_usable(void)
{
    system_installed = true; missing_symbols = true;
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary, "/system/latin.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(1, primary.count); TEST_ASSERT_EQUAL_UINT(3, flat_coverage(&primary));
    TEST_ASSERT_EQUAL_UINT(1, leaf_lookups); TEST_ASSERT_EQUAL_UINT(1, role_calls[JCE_HOST_FONT_UI_SYMBOLS]);
    memset(&primary, 0, sizeof primary); primary.coverage = 1;
    missing_symbols = false; failed_symbol_leaf = true;
    jce_ui_canvas_invalidate_resources();
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary, "/system/latin.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(1, primary.count); TEST_ASSERT_EQUAL_UINT(3, flat_coverage(&primary));
    TEST_ASSERT_EQUAL_UINT(3, leaf_lookups); TEST_ASSERT_EQUAL_UINT(2, role_calls[JCE_HOST_FONT_UI_SYMBOLS]);
}
static void explicit_font_policy_keeps_its_own_chain(void)
{
    system_installed = true;
    jce_ui_canvas_set_default_font("fonts/Custom.ttf");
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary, "fonts/Custom.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(0, primary.count); TEST_ASSERT_EQUAL_UINT(0, leaf_lookups);
    for (unsigned i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT(0, role_calls[i]);
    jce_ui_canvas_set_default_font(NULL);
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary, "fonts/ExplicitComponent.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(0, primary.count); TEST_ASSERT_EQUAL_UINT(0, leaf_lookups);
    jce_ui_canvas_set_font_fallbacks("fonts/F1.ttf");
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary, "/system/latin.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(1, primary.count); TEST_ASSERT_EQUAL_PTR(&first, primary.fallback[0]);
    TEST_ASSERT_EQUAL_UINT(3, flat_coverage(&primary));
}

static void stock_fallback_fills_missing_roles_after_installed_faces(void)
{
    system_installed = true;
    stock_available = true;
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary,
                         "/system/latin.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(0, stock_lookups);

    memset(&primary, 0, sizeof primary);
    primary.coverage = 1;
    missing_symbols = true;
    jce_ui_canvas_invalidate_resources();
    uc_attach_fallbacks((JceUICanvas *)&canvas_token, &primary,
                         "/system/latin.ttf", 24, false);
    TEST_ASSERT_EQUAL_UINT(1, stock_lookups);
    TEST_ASSERT_EQUAL_UINT(2, primary.count);
    TEST_ASSERT_EQUAL_PTR(&first, primary.fallback[0]);
    TEST_ASSERT_EQUAL_PTR(&stock, primary.fallback[1]);
    TEST_ASSERT_EQUAL_UINT(7, flat_coverage(&primary));
    TEST_ASSERT_NULL(uc_open_font_path(NULL, "@stock-cjk", 24, false));
    TEST_ASSERT_EQUAL_UINT(0, stock_pak_opens);
    TEST_ASSERT_EQUAL_PTR(&stock, uc_open_font_path(
        (const JcePakArchive *)&canvas_token, "@stock-cjk", 24, false));
    TEST_ASSERT_EQUAL_UINT(1, stock_pak_opens);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(two_complementary_fallbacks_are_flat_and_both_accepted);
    RUN_TEST(the_primary_path_is_not_added_to_its_own_fallback_list);
    RUN_TEST(explicit_invalidation_retries_negative_results_and_closes_positive_once);
    RUN_TEST(default_chain_attaches_cjk_then_symbols_and_resolves_once_per_epoch);
    RUN_TEST(symbols_unavailable_or_failed_leaf_keeps_primary_and_cjk_usable);
    RUN_TEST(explicit_font_policy_keeps_its_own_chain);
    RUN_TEST(stock_fallback_fills_missing_roles_after_installed_faces);
    return UNITY_END();
}
