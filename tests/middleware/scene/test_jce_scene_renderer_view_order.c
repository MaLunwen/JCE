/*
 * test_jce_scene_renderer_view_order.c
 *
 * Scene renderer view ordering: camera depth, shadow and GPU-compute producers
 * must execute before the color consumer, even when their numeric bgfx view
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
    TEST_ASSERT_TRUE(index_of(&order, 4) >= 0);
    TEST_ASSERT_TRUE(index_of(&order, 4) < index_of(&order, 3));
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

/* base+1 is the shared current-camera depth producer, even when shadow,
 * fog, particle compute and every other optional view are disabled.  Reserving
 * its existing core-band sort slot does not touch it or enable a prepass.
 * The real render owner may submit RP.depth_prepass after this helper runs;
 * an absent slot must not leave that producer sorting after base+0. */
static void assert_depth_before_color(const JceSceneRendererViewOrder *order,
                                      uint16_t base)
{
    const int depth = index_of(order, (uint16_t)(base + 1u));
    const int color = index_of(order, base);
    TEST_ASSERT_TRUE_MESSAGE(depth >= 0, "current-camera depth slot is missing");
    TEST_ASSERT_TRUE_MESSAGE(color >= 0, "scene color slot is missing");
    TEST_ASSERT_TRUE_MESSAGE(depth < order->named_count,
                             "depth producer must be an owned view, not filler");
    TEST_ASSERT_TRUE_MESSAGE(depth < color,
                             "current-camera depth must execute before color readers");
}

/* The bgfx remap window is a finite permutation in this product's 256-view
 * configuration.  Validate before indexing so a malformed owner result fails
 * an ordinary assertion instead of corrupting the fixture's own array. */
static void assert_finite_view_window(const JceSceneRendererViewOrder *order)
{
    bool seen[256] = {false};
    TEST_ASSERT_TRUE(order->count > 0u);
    TEST_ASSERT_TRUE(order->count <= JCE_SCENE_RENDERER_VIEW_ORDER_MAX);
    TEST_ASSERT_TRUE(order->named_count <= order->count);
    TEST_ASSERT_TRUE_MESSAGE((uint32_t)order->first + order->count <= 256u,
                             "view remap window exceeds the hardware view limit");
    for (uint16_t i = 0; i < order->count; ++i) {
        const uint16_t id = order->order[i];
        TEST_ASSERT_TRUE_MESSAGE(id < 256u, "view id exceeds the hardware view limit");
        TEST_ASSERT_TRUE(id >= order->first);
        TEST_ASSERT_TRUE((uint32_t)id < (uint32_t)order->first + order->count);
        TEST_ASSERT_FALSE_MESSAGE(seen[id], "view remap is not a permutation");
        seen[id] = true;
    }
    for (uint16_t i = 0; i < order->count; ++i)
        TEST_ASSERT_TRUE(seen[(uint16_t)(order->first + i)]);
}

static void test_prepass_only_depth_producer_executes_before_color(void)
{
    JceSceneRendererViewOrder order;
    /* The public helper has no depth-request parameter.  This is the actual
     * existing call for a pipeline enabling only depth_prepass, not a fixture
     * flag pretending that production already admits a separate request. */
    TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
        30u, false, 0u, false, false, false, false, false, false, 0u, &order));
    assert_finite_view_window(&order);
    assert_depth_before_color(&order, 30u);
    TEST_ASSERT_EQUAL_UINT16(2u, order.count);
    TEST_ASSERT_EQUAL_UINT16(2u, order.named_count);
    TEST_ASSERT_EQUAL_UINT16(31u, order.order[0]);
    TEST_ASSERT_EQUAL_UINT16(30u, order.order[1]);
}

static void test_depth_producer_precedes_color_at_each_view_base(void)
{
    /* Runtime, Scene, Game, preview and high-end reduced-pass reservations all
     * use this same complete owner.  This test does not initialize a viewport
     * or claim that the reduced reflection path calls this helper. */
    const uint16_t bases[] = {0u, JCE_VIEW_EDITOR_SCENE, 30u, 60u, 80u,
        JCE_VIEW_EDITOR_GAME, 128u, 238u, 254u};
    for (unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
        JceSceneRendererViewOrder order;
        TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
            bases[i], false, 0u, false, false, false, false, false, false, 0u, &order));
        assert_finite_view_window(&order);
        assert_depth_before_color(&order, bases[i]);
    }
}

static void test_depth_keeps_compute_shadow_and_overlay_dependencies(void)
{
    /* Each optional producer band is exercised independently, then together.
     * Existing exact shadow/fog/overlay controls above remain unchanged. */
    const struct {
        bool shadows;
        bool fog;
        bool particles;
        bool cull;
        bool cube;
        bool dynamic;
        bool underwater;
        uint8_t overlays;
    } cases[] = {
        {false, false, true,  false, false, false, false, 0u},
        {false, false, false, true,  false, false, false, 0u},
        {true,  false, false, false, false, false, false, 0u},
        {false, true,  false, false, false, false, false, 0u},
        {false, false, false, false, true,  false, false, 0u},
        {false, false, false, false, false, true,  false, 0u},
        {false, false, false, false, false, false, true,  0u},
        {false, false, false, false, false, false, false, 3u},
        {true,  true,  true,  true,  true,  true,  true,  3u}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        JceSceneRendererViewOrder order;
        TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
            3u, cases[i].shadows, 4u, cases[i].fog,
            cases[i].particles, cases[i].cull, cases[i].cube,
            cases[i].dynamic, cases[i].underwater, cases[i].overlays, &order));
        assert_finite_view_window(&order);
        assert_depth_before_color(&order, 3u);
        if (cases[i].cull) {
            const int reset = index_of(&order,
                (uint16_t)(3u + JCE_VIEW_SR_GPU_CULL_RESET_OFFSET));
            const int cull = index_of(&order, 12u);
            TEST_ASSERT_TRUE(reset >= 0 && cull >= 0);
            TEST_ASSERT_TRUE(reset < cull);
            TEST_ASSERT_TRUE_MESSAGE(cull < index_of(&order, 4u),
                                     "depth draws must follow visibility compute");
        }
        if (cases[i].shadows) {
            for (uint16_t c = 0; c < 4u; ++c)
                TEST_ASSERT_TRUE(index_of(&order, (uint16_t)(14u + c)) <
                                 index_of(&order, 3u));
        }
        if (cases[i].overlays) {
            const int overlay = index_of(&order, 67u);
            TEST_ASSERT_TRUE(overlay > index_of(&order, 3u));
            TEST_ASSERT_TRUE(overlay < index_of(&order, 21u)); /* SSR reader */
            TEST_ASSERT_TRUE(overlay < index_of(&order, 23u)); /* fullscreen */
            TEST_ASSERT_TRUE(overlay < index_of(&order, 33u)); /* postfx */
        }
    }
}

static void test_last_legal_view_windows_preserve_depth_and_permutation(void)
{
    const struct {
        uint16_t base;
        bool fog;
        bool cull;
        bool cube;
        uint8_t overlays;
    } cases[] = {
        {254u, false, false, false, 0u}, /* base+1 = 255 */
        {239u, true,  false, false, 0u}, /* base+16 = 255 */
        {192u, false, true,  false, 0u}, /* base+63 = 255 */
        {139u, false, false, true,  0u}, /* base+116 = 255 */
        {189u, false, false, false, 3u}  /* base+66 = 255 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        JceSceneRendererViewOrder order;
        TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(
            cases[i].base, false, 0u, cases[i].fog, false,
            cases[i].cull, cases[i].cube, false, false, cases[i].overlays, &order));
        assert_finite_view_window(&order);
        assert_depth_before_color(&order, cases[i].base);
        TEST_ASSERT_TRUE(index_of(&order, 255u) >= 0);
        TEST_ASSERT_EQUAL_UINT16(256u - cases[i].base, order.count);
    }
}

static void test_depth_slot_beyond_last_hardware_view_is_rejected(void)
{
    JceSceneRendererViewOrder order;
    TEST_ASSERT_FALSE_MESSAGE(jce_scene_renderer_view_order_build(
        255u, false, 0u, false, false, false, false, false, false, 0u, &order),
        "a color-only window cannot omit a potentially submitted depth producer");
}

static void test_base_outside_hardware_view_window_is_rejected(void)
{
    JceSceneRendererViewOrder order;
    TEST_ASSERT_FALSE(jce_scene_renderer_view_order_build(
        256u, false, 0u, false, false, false, false, false, false, 0u, &order));
}

static void test_optional_bands_cannot_extend_past_hardware_view_window(void)
{
    JceSceneRendererViewOrder order;
    TEST_ASSERT_FALSE(jce_scene_renderer_view_order_build(
        193u, false, 0u, false, false, true, false, false, false, 0u, &order));
    TEST_ASSERT_FALSE(jce_scene_renderer_view_order_build(
        140u, false, 0u, false, false, false, true, false, false, 0u, &order));
    TEST_ASSERT_FALSE(jce_scene_renderer_view_order_build(
        190u, false, 0u, false, false, false, false, false, false, 3u, &order));
    TEST_ASSERT_FALSE(jce_scene_renderer_view_order_build(
        240u, false, 0u, true, false, false, false, false, false, 0u, &order));
}

static void test_caustic_atlas_is_owned_before_water_colour(void)
{
    const uint16_t bases[] = {JCE_VIEW_EDITOR_SCENE, JCE_VIEW_RUNTIME_GAME, JCE_VIEW_EDITOR_GAME};
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; ++i) {
        const uint16_t base = bases[i];
        const uint16_t caustic = (uint16_t)(base + JCE_VIEW_SR_WATER_CAUSTIC_OFFSET);
        JceSceneRendererViewOrder order, off;
        TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build_with_caustic(base,
            false, 0u, false, false, false, false, false, false, 0u, true, &order));
        assert_finite_view_window(&order);
        const int producer = index_of(&order, caustic);
        TEST_ASSERT_TRUE(producer >= 0 && producer < order.named_count);
        TEST_ASSERT_TRUE(producer < index_of(&order, base));
        TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build(base,
            false, 0u, false, false, false, false, false, false, 0u, &off));
        TEST_ASSERT_EQUAL_INT(-1, index_of(&off, caustic));
    }
    JceSceneRendererViewOrder out;
    TEST_ASSERT_FALSE(jce_scene_renderer_view_order_build_with_caustic(JCE_VIEW_PLANAR_REFLECTION_BASE,
        false, 0u, false, false, false, false, false, false, 0u, true, &out));
    TEST_ASSERT_EQUAL_UINT16(0u, out.count);
}

static void test_cloud_temporal_runs_before_each_primary_sky(void)
{
    const uint16_t bases[] = {JCE_VIEW_EDITOR_SCENE, JCE_VIEW_EDITOR_GAME,
                             JCE_VIEW_RUNTIME_GAME};
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; ++i) {
        const uint16_t base = bases[i];
        const uint16_t cloud = (uint16_t)(base + JCE_VIEW_SR_CLOUD_TEMPORAL_OFFSET);
        JceSceneRendererViewOrder order, off;
        TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build_with_cloud_temporal(base,
            false, 0u, false, false, false, false, false, false, 0u,
            false, false, false, false, true, &order));
        assert_finite_view_window(&order);
        TEST_ASSERT_TRUE(index_of(&order, cloud) >= 0);
        TEST_ASSERT_TRUE(index_of(&order, cloud) < index_of(&order, base));
        TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build_with_cloud_temporal(base,
            false, 0u, false, false, false, false, false, false, 0u,
            false, false, false, false, false, &off));
        TEST_ASSERT_EQUAL_INT(-1, index_of(&off, cloud));
    }
    JceSceneRendererViewOrder offscreen;
    TEST_ASSERT_FALSE(jce_scene_renderer_view_order_build_with_cloud_temporal(
        JCE_VIEW_PLANAR_REFLECTION_BASE, false, 0u, false, false, false, false,
        false, false, 0u, false, false, false, false, true, &offscreen));
}

static void test_cloud_opaque_precedes_transparency_and_preserves_optics_source(void)
{
    const uint16_t bases[] = {JCE_VIEW_EDITOR_SCENE, JCE_VIEW_EDITOR_GAME,
                             JCE_VIEW_RUNTIME_GAME};
    for (unsigned i = 0; i < sizeof bases / sizeof bases[0]; ++i) {
        const uint16_t base = bases[i];
        for (unsigned optics = 0; optics < 2; ++optics) {
            JceSceneRendererViewOrder order;
            TEST_ASSERT_TRUE(jce_scene_renderer_view_order_build_with_cloud_temporal(base,
                false, 0u, false, false, false, false, false, false, 0u,
                false, optics != 0u, false, false, true, &order));
            assert_finite_view_window(&order);
            const int composite = index_of(&order,
                (uint16_t)(base + JCE_VIEW_SR_CLOUD_OPAQUE_OFFSET));
            const int transparent = index_of(&order,
                (uint16_t)(base + JCE_VIEW_SR_TRANSPARENCY_OFFSET));
            TEST_ASSERT_TRUE(composite > index_of(&order, base));
            TEST_ASSERT_TRUE(transparent > composite);
            TEST_ASSERT_TRUE(composite < order.named_count);
            TEST_ASSERT_TRUE(transparent < order.named_count);
            if (optics) {
                const int trace = index_of(&order,
                    (uint16_t)(base + JCE_VIEW_SR_WATER_TRACE_OFFSET));
                TEST_ASSERT_TRUE(trace > index_of(&order, base));
                TEST_ASSERT_TRUE(trace < composite);
            }
        }
    }
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
    RUN_TEST(test_prepass_only_depth_producer_executes_before_color);
    RUN_TEST(test_depth_producer_precedes_color_at_each_view_base);
    RUN_TEST(test_depth_keeps_compute_shadow_and_overlay_dependencies);
    RUN_TEST(test_last_legal_view_windows_preserve_depth_and_permutation);
    RUN_TEST(test_depth_slot_beyond_last_hardware_view_is_rejected);
    RUN_TEST(test_base_outside_hardware_view_window_is_rejected);
    RUN_TEST(test_optional_bands_cannot_extend_past_hardware_view_window);
    RUN_TEST(test_caustic_atlas_is_owned_before_water_colour);
    RUN_TEST(test_cloud_temporal_runs_before_each_primary_sky);
    RUN_TEST(test_cloud_opaque_precedes_transparency_and_preserves_optics_source);
    return UNITY_END();
}
