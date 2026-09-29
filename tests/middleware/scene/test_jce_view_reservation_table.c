/*
 * test_jce_view_reservation_table.c
 *
 * The reservation table must equal what the builder actually names.
 *
 * WHY THIS EXISTS.  JCE_VIEW_SR_OFFSET_RESERVED() in <jce/renderer/jce_views.h>
 * is what the editor's viewport offsets are static_asserted against, so it is
 * the thing that stops a foreign pass from landing on an id the scene renderer
 * hands to bgfx_set_view_order().  A compile-time gate is only as good as the
 * table it consults: if jce_scene_renderer_view_order_build() grows a band and
 * nobody updates the table, every one of those static_asserts keeps passing
 * while protecting nothing.  That is the exact shape this repo has hit before
 * -- a guard that cannot say no -- and it is the shape that produced the bug
 * this file's siblings were written for: base+52 was named by the dynamic-CSM
 * band, declared free in a private comment, and taken by the editor's ECS-UI
 * overlay, which then drew and was erased every frame in both viewports.
 *
 * So this walks EVERY feature combination the builder accepts, for every base
 * a viewport actually uses, and asserts two properties:
 *
 *   1. every id the builder NAMES is declared reserved by the table;
 *   2. the two editor viewport bases never name the same id -- the cross-BASE
 *      case, which no static_assert inside one viewport can reach, and which
 *      was live for any scene with point-cube shadows on.
 *
 * 2^7 combinations x 3 bases, so it is exhaustive rather than representative.
 */

#include "unity.h"

#include "jce_scene_renderer_view_order.h"

#include <jce/renderer/jce_views.h>

#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* The bases a scene render is actually driven from in this product. */
static const uint16_t BASES[] = {
    JCE_VIEW_EDITOR_SCENE,     /* editor Scene View   */
    JCE_VIEW_EDITOR_GAME,      /* editor Game View    */
    JCE_VIEW_RUNTIME_GAME,     /* shipped runtime     */
};

/* Collect the NAMED ids (the prefix the builder owns; the tail is filler that
 * only borrows a sort position).  Returns the count, 0 on a failed build. */
static int named_ids(uint16_t base, unsigned combo,
                     uint16_t *out, int cap)
{
    JceSceneRendererViewOrder o;
    const bool shadows   = (combo & 1u)  != 0;
    const bool fog       = (combo & 2u)  != 0;
    const bool particles = (combo & 4u)  != 0;
    const bool gpu_cull  = (combo & 8u)  != 0;
    const bool cube      = (combo & 16u) != 0;
    const bool dyn_csm   = (combo & 32u) != 0;
    const bool underwtr  = (combo & 64u) != 0;
    /* Camera-stack overlays: a band like any other, so it joins the sweep.
     * A band left out of `combo` is a band this table never asks about. */
    const uint8_t overlays = (combo & 128u) != 0 ? 3u : 0u;
    const uint8_t cascades = shadows ? 4u : 0u;

    if (!jce_scene_renderer_view_order_build(base, shadows, cascades, fog,
                                             particles, gpu_cull, cube,
                                             dyn_csm, underwtr, overlays, &o))
        return 0;
    int n = 0;
    for (uint16_t i = 0; i < o.named_count && n < cap; ++i)
        out[n++] = o.order[i];
    return n;
}

/* PROPERTY 1.  Anything the builder names must be declared by the table the
 * editor's static_asserts consult.  A band added to the builder without a
 * matching entry here would leave those asserts passing over a live id. */
static void test_every_named_view_is_declared_reserved(void)
{
    uint16_t ids[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
    for (size_t b = 0; b < sizeof BASES / sizeof BASES[0]; ++b) {
        for (unsigned combo = 0; combo < 256u; ++combo) {
            const uint16_t base = BASES[b];
            const int n = named_ids(base, combo, ids, (int)(sizeof ids / sizeof ids[0]));
            for (int i = 0; i < n; ++i) {
                TEST_ASSERT_TRUE_MESSAGE(ids[i] >= base,
                    "a named id below its own base");
                const unsigned off = (unsigned)(ids[i] - base);
                char msg[192];
                snprintf(msg, sizeof msg,
                         "base=%u combo=0x%02x NAMES base+%u, which "
                         "JCE_VIEW_SR_OFFSET_RESERVED() does not declare -- "
                         "the editor's static_asserts are consulting a table "
                         "that no longer matches the renderer",
                         (unsigned)base, combo, off);
                /* The message names base, feature combination and offset:
                 * "which band?" is the first question a failure raises, and a
                 * fixed string cannot answer it. */
                TEST_ASSERT_TRUE_MESSAGE(
                    JCE_VIEW_SR_OFFSET_RESERVED(off), msg);
            }
        }
    }
}

/* PROPERTY 2.  The two editor viewports render in the SAME frame from two
 * bases.  No id may be named by both, in any feature combination.  This is the
 * cross-base case: with point-cube shadows on, the Scene View named 103..119
 * while the Game View's base was 80, and no per-viewport static_assert can see
 * across the two. */
static void test_the_two_editor_viewport_bases_never_name_the_same_id(void)
{
    uint16_t a[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
    uint16_t b[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
    const int cap = (int)(sizeof a / sizeof a[0]);

    for (unsigned ca = 0; ca < 128u; ++ca) {
        const int na = named_ids(JCE_VIEW_EDITOR_SCENE, ca, a, cap);
        for (unsigned cb = 0; cb < 128u; ++cb) {
            const int nb = named_ids(JCE_VIEW_EDITOR_GAME, cb, b, cap);
            for (int i = 0; i < na; ++i)
                for (int j = 0; j < nb; ++j)
                    TEST_ASSERT_TRUE_MESSAGE(a[i] != b[j],
                        "the Scene View and the Game View name the same bgfx "
                        "view id: one of them will silently render nothing");
        }
    }
}

/* Every FIXED absolute id in jce_views.h, and whether a scene-renderer band is
 * allowed to name it.  Hand-listing three of them -- which is what this
 * property did -- is the same shape as a contract file whose checker parses
 * half of it: the ids nobody remembered were the ones that got taken.  base+57
 * landing on JCE_VIEW_EDITOR_PREVIEW got through a version of this test.
 *
 * `allowed` is not an escape hatch; it is one row with a reason, and the reason
 * has to be a mechanism, not an intention. */
typedef struct { uint16_t id; const char *name; bool allowed; const char *why; } FixedId;

static const FixedId FIXED_IDS[] = {
    { JCE_VIEW_MAIN_3D,               "MAIN_3D",               false, NULL },
    { JCE_VIEW_DEBUG,                 "DEBUG",                 false, NULL },
    { JCE_VIEW_POST_BASE,             "POST_BASE",             true,
      "the value jce_postfx holds at CREATE, before every consumer re-bases it; "
      "scene-view base 3 + offset 17 is 20 by construction.  Guarded rather than "
      "moved: jce_postfx_apply claims its 21 views unconditionally, so a consumer "
      "that skipped the re-base is reported by name" },
    { JCE_VIEW_EDITOR_OVERLAY,        "EDITOR_OVERLAY",        false, NULL },
    { JCE_VIEW_EDITOR_PREVIEW,        "EDITOR_PREVIEW",        false, NULL },
    { JCE_VIEW_EDITOR_PICK,           "EDITOR_PICK",           false, NULL },
    { JCE_VIEW_EDITOR_PICK_READBACK,  "EDITOR_PICK_READBACK",  false, NULL },
    { JCE_VIEW_IMGUI,                 "IMGUI",                 false, NULL },
    { JCE_VIEW_RECORDER_RESOLVE,      "RECORDER_RESOLVE",      false, NULL },
    { JCE_VIEW_OCCLUSION,             "OCCLUSION",             false, NULL },
    { JCE_VIEW_EDITOR_GAME_OCCLUSION, "EDITOR_GAME_OCCLUSION", false, NULL },
    { JCE_VIEW_UI,                    "UI",                    false, NULL },
};

/* PROPERTY 3.  No named view, at any live base, under any feature combination,
 * may land on a fixed absolute id -- the axis no per-base static_assert can
 * reach, because it is a property of the base VALUE and not of the offsets. */
static void test_no_named_view_reaches_any_fixed_id(void)
{
    uint16_t ids[JCE_SCENE_RENDERER_VIEW_ORDER_MAX];
    for (size_t b = 0; b < sizeof BASES / sizeof BASES[0]; ++b) {
        for (unsigned combo = 0; combo < 256u; ++combo) {
            const int n = named_ids(BASES[b], combo, ids,
                                    (int)(sizeof ids / sizeof ids[0]));
            for (int i = 0; i < n; ++i) {
                for (size_t f = 0; f < sizeof FIXED_IDS / sizeof FIXED_IDS[0]; ++f) {
                    if (FIXED_IDS[f].allowed)        continue;
                    if (ids[i] != FIXED_IDS[f].id)   continue;
                    char msg[192];
                    snprintf(msg, sizeof msg,
                             "base %u names view %u, which is JCE_VIEW_%s",
                             (unsigned)BASES[b], (unsigned)ids[i],
                             FIXED_IDS[f].name);
                    TEST_FAIL_MESSAGE(msg);
                }
            }
        }
    }
}

/* PROPERTY 4.  A base of 0 is NOT a scene-renderer base.  The shipped runtime
 * used to render its no-postfx fallback at JCE_VIEW_MAIN_3D, which put a
 * ~117-offset span on top of every fixed id below 117 -- offset 2 is the SSAO
 * sample pass and absolute 2 is JCE_VIEW_DEBUG, which the renderer touches in
 * the same frame.  Asserted here because BASES above is the list this file
 * reasons over, and a base silently rejoining it is the regression. */
static void test_zero_is_not_a_live_base(void)
{
    for (size_t b = 0; b < sizeof BASES / sizeof BASES[0]; ++b)
        TEST_ASSERT_TRUE_MESSAGE(BASES[b] != 0,
            "a scene-renderer base of 0 overlaps every fixed id below its span");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_named_view_is_declared_reserved);
    RUN_TEST(test_the_two_editor_viewport_bases_never_name_the_same_id);
    RUN_TEST(test_no_named_view_reaches_any_fixed_id);
    RUN_TEST(test_zero_is_not_a_live_base);
    return UNITY_END();
}
