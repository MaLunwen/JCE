/*
 * test_jce_scene_renderer_view_order.c
 *
 * Scene renderer view ordering: shadow + GPU-compute producer views must
 * execute before the color consumer view, even when their numeric bgfx view
 * IDs are higher. Assertions favour ordering invariants over exact slot
 * indices so they survive band layout tweaks (only the stable cascade band
 * head and total range count are pinned numerically).
 */

#include "jce_scene_renderer_view_order.h"

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static int index_of(const JceSceneRendererViewOrder *order, uint16_t view_id)
{
    for (uint16_t i = 0; i < order->count; i++) {
        if (order->order[i] == view_id)
            return (int)i;
    }
    return -1;
}

static void test_shadows_execute_before_scene_color_view(void)
{
    JceSceneRendererViewOrder order;
    /* base=3, shadows on, 4 CSM cascades, fog on, no GPU compute views. */
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3, true, 4, true, false, false, false, false, false, /*overlays=*/0, &order));

    TEST_ASSERT_EQUAL_UINT16(3, order.first);
    TEST_ASSERT_EQUAL_UINT16(17, order.count);

    /* Cascade shadow band leads: base+10 then base+11..base+14. */
    TEST_ASSERT_EQUAL_UINT16(13, order.order[0]);
    TEST_ASSERT_EQUAL_UINT16(14, order.order[1]);
    TEST_ASSERT_EQUAL_UINT16(15, order.order[2]);
    TEST_ASSERT_EQUAL_UINT16(16, order.order[3]);
    TEST_ASSERT_EQUAL_UINT16(17, order.order[4]);

    /* Invariant: every shadow producer (cascade band 13.. + local atlas band
     * base+4..base+8) precedes the scene color view (base+0=3). */
    TEST_ASSERT_TRUE(index_of(&order, 13) < index_of(&order, 3));
    TEST_ASSERT_TRUE(index_of(&order, 7)  < index_of(&order, 3));
    /* And the scene color view precedes the fog views (base+16=19 etc.). */
    TEST_ASSERT_TRUE(index_of(&order, 3) < index_of(&order, 18));
    TEST_ASSERT_TRUE(index_of(&order, 3) < index_of(&order, 19));
}

static void test_scene_color_stays_before_fog_when_shadows_are_disabled(void)
{
    JceSceneRendererViewOrder order;
    /* base=3, shadows off, fog on, no GPU compute views. */
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3, false, 4, true, false, false, false, false, false, /*overlays=*/0, &order));

    TEST_ASSERT_EQUAL_UINT16(3, order.first);
    TEST_ASSERT_EQUAL_UINT16(17, order.count);
    TEST_ASSERT_EQUAL_UINT16(3, order.order[0]);
    TEST_ASSERT_TRUE(index_of(&order, 3) < index_of(&order, 18));
    TEST_ASSERT_TRUE(index_of(&order, 18) < index_of(&order, 19));
}

static void test_gpu_compute_view_executes_before_scene_color_view(void)
{
    JceSceneRendererViewOrder order;
    /* base=3, only the GPU-driven cull dispatch (shares the base+9 compute
     * band with GPU particles). It writes buffers the color view consumes, so
     * it must precede base+0. */
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3, false, 0, false, false, true, false, false, false, /*overlays=*/0, &order));

    TEST_ASSERT_EQUAL_UINT16(3, order.first);
    /* The counter reset moved off base+3 -- that slot is SSAO's blur pass, and
     * with both features on the builder sorted the blur ahead of SSAO's own
     * sample.  It is base+JCE_VIEW_SR_GPU_CULL_RESET_OFFSET now, which is a
     * SPARSE id, so the window (and therefore `count`) closes over it. */
    const uint16_t reset_id =
        (uint16_t)(3 + JCE_VIEW_SR_GPU_CULL_RESET_OFFSET);
    TEST_ASSERT_EQUAL_UINT16(reset_id, order.order[0]);           /* reset first  */
    TEST_ASSERT_EQUAL_UINT16(12, order.order[1]);                 /* base+9 cull second  */
    TEST_ASSERT_TRUE(index_of(&order, reset_id) < index_of(&order, 12));
    TEST_ASSERT_TRUE(index_of(&order, 12) < index_of(&order, 3)); /* before scene color  */
    /* The property that matters and had no test: the reset must NOT land on a
     * view another pass owns.  base+2/+3 are SSAO's pair. */
    TEST_ASSERT_TRUE(reset_id != (uint16_t)(3 + JCE_VIEW_SR_SSAO_OFFSET));
    TEST_ASSERT_TRUE(reset_id != (uint16_t)(3 + JCE_VIEW_SR_SSAO_OFFSET + 1));
}

/* roadmap #18 shadow extension: the per-cascade GPU shadow cull dispatch (base+9)
 * + its counter reset MUST precede the CSM cascade DRAW views
 * (base+11+c), or a cascade depth draw would consume the cull's visible/indirect
 * buffers before the compute filled them (garbage shadows).  Pins that ordering. */
static void test_gpu_cull_precedes_csm_cascade_draw_views(void)
{
    JceSceneRendererViewOrder order;
    /* base=3, shadows on, 4 cascades, fog on, GPU cull on. */
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3, true, 4, true, false, true, false, false, false, /*overlays=*/0, &order));

    const int reset = index_of(&order,
        (uint16_t)(3 + JCE_VIEW_SR_GPU_CULL_RESET_OFFSET));  /* counter reset */
    const int cull  = index_of(&order, 12);  /* base+9 cull/compact/build */
    TEST_ASSERT_TRUE(reset >= 0 && cull >= 0);
    TEST_ASSERT_TRUE(reset < cull);                       /* reset before cull   */
    /* cull before every cascade draw view base+11+c (=14..17). */
    for (uint16_t cv = 14; cv <= 17; cv++)
        TEST_ASSERT_TRUE(cull < index_of(&order, cv));
    /* and the producer band still precedes the color view (base+0=3). */
    TEST_ASSERT_TRUE(index_of(&order, 14) < index_of(&order, 3));
}


/* The remap window must be a permutation, but only part of it is OURS: the
 * builder appends filler ids purely so no live view is left holding a borrowed
 * sort key.  jce_view_bands_claim reads the named prefix to declare ownership,
 * so the split has to survive refactoring -- when it did not, the scene
 * renderer claimed postfx's band and the overlap guard fired every frame. */
static void test_named_count_excludes_the_window_filler(void)
{
    JceSceneRendererViewOrder o;
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3u, true, 4u, true, false, false, false, /*dyn_csm=*/true, false, /*overlays=*/0, &o));

    /* The dynamic atlas sits far above the contiguous range, so closing the
     * window necessarily adds filler. */
    TEST_ASSERT_TRUE(o.named_count < o.count);
    TEST_ASSERT_TRUE(o.named_count > 0u);

    /* Every named id is one this renderer actually draws into: the contiguous
     * head, or the dynamic-atlas band.  Nothing in between. */
    for (uint16_t i = 0; i < o.named_count; i++) {
        const uint16_t rel = (uint16_t)(o.order[i] - o.first);
        /* base+17 is the underwater slot; base+18/19 are SSR.
         * postfx re-bases at base+20, which is where the head
         * band must stop. */
        const bool head = rel <= 17u;
        const bool dyn  = rel >= JCE_VIEW_DYN_CSM_OFFSET &&
                          rel <= (uint16_t)(JCE_VIEW_DYN_CSM_OFFSET + 4u);
        TEST_ASSERT_TRUE_MESSAGE(head || dyn, "named view outside owned bands");
    }
}

/* postfx re-bases to base+20 and claims 21 ids; the scene renderer must not
 * declare any of them, or the ownership guard is worthless. */
static void test_named_views_never_touch_the_postfx_band(void)
{
    JceSceneRendererViewOrder o;
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3u, true, 4u, true, true, true, false, /*dyn_csm=*/true, false, /*overlays=*/0, &o));

    for (uint16_t i = 0; i < o.named_count; i++) {
        const uint16_t rel = (uint16_t)(o.order[i] - o.first);
        TEST_ASSERT_TRUE_MESSAGE(rel < 20u || rel > 40u,
                                 "named view lands inside the postfx band");
    }
    /* ...while the full array still spans its whole window, as bgfx needs. */
    uint16_t highest = o.first;
    for (uint16_t i = 0; i < o.count; i++)
        if (o.order[i] > highest) highest = o.order[i];
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(highest - o.first + 1u), o.count);
}

/* ── Underwater absorption owns base+17, and only when asked ───────────
 *
 * The slot matters more than it looks.  base+10..14 is the shadow band
 * (base+10 simple, base+11+c per cascade), so the obvious-looking base+14 is
 * CASCADE 3 -- a fullscreen quad written there overwrites a shadow map, and in
 * this renderer a view-slot collision has previously produced a black screen
 * rather than anything resembling a slot conflict. */

static void test_underwater_view_is_owned_and_gated(void)
{
    JceSceneRendererViewOrder off, on;

    /* Same configuration, underwater off then on. */
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3, true, 4, true, false, false, false, false, false, /*overlays=*/0, &off));
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3, true, 4, true, false, false, false, false, true, /*overlays=*/0, &on));

    /* Gated: a scene with no absorbing water must not pay for the slot. */
    TEST_ASSERT_TRUE(on.count > off.count);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(off.count + 1u), on.count);

    /* And the slot claimed is base+17 -- never inside the shadow band. */
    TEST_ASSERT_TRUE(index_of(&on, (uint16_t)(3 + 17)) >= 0);
    TEST_ASSERT_TRUE(index_of(&off, (uint16_t)(3 + 17)) < 0);

    /* It must come AFTER every cascade: absorbing the frame before the
     * shadows are drawn would tint a buffer that is then overwritten. */
    const int uw = index_of(&on, (uint16_t)(3 + 17));
    for (uint16_t c = 0; c < 4u; c++) {
        const int casc = index_of(&on, (uint16_t)(3 + 11 + c));
        TEST_ASSERT_TRUE(casc >= 0);
        TEST_ASSERT_TRUE(uw > casc);
    }

    /* And after the scene colour view it is absorbing. */
    const int color = index_of(&on, 3);
    TEST_ASSERT_TRUE(color >= 0);
    TEST_ASSERT_TRUE(uw > color);
}

/* Camera-stack overlays draw INTO the scene colour, so they must sort after it
 * -- and before every consumer of that colour.  Ascending id order would put
 * base+64..66 after SSR (base+18), after the project fullscreen chain
 * (base+20..25), after SSGI's composite (base+27) and after the PostFX chain
 * (base+30): an overlay composited onto a tone-mapped image. */
static void test_camera_overlays_sort_after_colour_and_before_its_readers(void)
{
    JceSceneRendererViewOrder o;
    /* underwater ON, so base+1..17 are ALL named: the claim below is about
     * every view in the base camera's contiguous run, and asserting it against
     * a build that leaves base+17 out would be asserting about the window
     * filler instead. */
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3u, true, 4u, true, false, false, false, false, /*underwater=*/true,
        /*overlays=*/3, &o));

    const int colour = index_of(&o, 3);
    TEST_ASSERT_TRUE(colour >= 0);

    for (uint16_t i = 0; i < 3u; i++) {
        const int ov = index_of(&o, (uint16_t)(3 + 64 + i));
        TEST_ASSERT_TRUE(ov >= 0);
        TEST_ASSERT_TRUE(ov > colour);
        /* After the base camera's WHOLE run, not merely after its colour
         * view: base+16 and base+17 -- the fog composite and the underwater
         * absorption -- DRAW INTO that colour, and an overlay ordered ahead of
         * them would come back fogged by the base camera's atmosphere. */
        for (uint16_t b = 1u; b <= 17u; b++)
            TEST_ASSERT_TRUE(ov > index_of(&o, (uint16_t)(3 + b)));
        /* Every id that reads the scene colour comes later.  base+18 is SSR,
         * base+20 the first fullscreen stage, base+27 SSGI's composite,
         * base+30 the head of the PostFX chain. */
        TEST_ASSERT_TRUE(ov < index_of(&o, (uint16_t)(3 + 18)));
        TEST_ASSERT_TRUE(ov < index_of(&o, (uint16_t)(3 + 20)));
        TEST_ASSERT_TRUE(ov < index_of(&o, (uint16_t)(3 + 27)));
        TEST_ASSERT_TRUE(ov < index_of(&o, (uint16_t)(3 + 30)));
    }
}

/* Zero overlays must leave the order exactly as it was: a project with one
 * camera pays nothing for a feature it never authored. */
static void test_no_overlays_is_the_order_this_build_had_before(void)
{
    JceSceneRendererViewOrder none, three;
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3u, true, 4u, true, false, false, false, false, false, 0, &none));
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        3u, true, 4u, true, false, false, false, false, false, 3, &three));
    /* The three overlay ids are the ONLY difference: same first, three more
     * named views, and every previously named view in the same position. */
    TEST_ASSERT_EQUAL_UINT16(none.first, three.first);
    TEST_ASSERT_EQUAL_UINT16(none.named_count + 3u, three.named_count);
    TEST_ASSERT_TRUE(index_of(&none, (uint16_t)(3 + 64)) < 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_underwater_view_is_owned_and_gated);
    RUN_TEST(test_shadows_execute_before_scene_color_view);
    RUN_TEST(test_scene_color_stays_before_fog_when_shadows_are_disabled);
    RUN_TEST(test_gpu_compute_view_executes_before_scene_color_view);
    RUN_TEST(test_gpu_cull_precedes_csm_cascade_draw_views);
    RUN_TEST(test_named_count_excludes_the_window_filler);
    RUN_TEST(test_named_views_never_touch_the_postfx_band);
    RUN_TEST(test_camera_overlays_sort_after_colour_and_before_its_readers);
    RUN_TEST(test_no_overlays_is_the_order_this_build_had_before);
    return UNITY_END();
}
