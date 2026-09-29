/*
 * test_jce_rprobe_bake_resolution.c — a probe bakes at the resolution its own
 * component authors.
 *
 * JceReflectionProbeComponent.resolution was authored, serialised and shown in
 * an Inspector combo offering {16 .. 1024}, and the engine never read it --
 * because NO engine file built a JceReflectionProbeBakeDesc at all.  The only
 * two builders were editor panels, and they disagreed about the same probe
 * three ways: the browser passed `p->resolution`, the Inspector passed a value
 * out of a static keyed by the component POINTER offering {128, 256, 512}, and
 * they named the artefact "probe_%u.ktx" (entity) versus "probe_%p.ktx"
 * (pointer, not stable across runs).
 *
 * THE OBSERVABLE IS THE FILE, not the desc.  The bake worker is a pure-CPU
 * async task that writes a real KTX1 container, so the face size the bake
 * actually used is readable out of the header -- there is no GPU in this test
 * and no renderer.  Asserting on a desc we filled ourselves would only prove
 * we can fill a struct.
 *
 * KTX1 header: 12-byte identifier, then endianness, then 11 uint32 fields;
 * pixelWidth is the 7th of those (offset 12 + 4*1 + 4*5 = 36).
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/os/core/jce_timer.h>

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_reflection_probe.h>
#include <jce/os/core/jce_async.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bake a probe authored with `resolution` and return the face size the written
 * KTX says it used, or 0 when nothing was written. */
static uint32_t baked_face_size(int resolution, int mode, char *out_path,
                                int cap)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "Probe");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s, e, &tf);

    JceReflectionProbeComponent rp;
    memset(&rp, 0, sizeof rp);
    rp.mode       = mode;
    rp.resolution = resolution;
    rp.intensity  = 1.0f;
    jce_scene_set_reflection_probe(s, e, &rp);

    char path[256] = {0};
    JceReflectionProbeBakeHandle h =
        jce_scene_reflection_probe_bake(s, e, true, path, (int)sizeof path);
    if (out_path && cap > 0) snprintf(out_path, (size_t)cap, "%s", path);
    if (h == 0u) { jce_scene_destroy(s); return 0u; }

    /* Drive it to a terminal state.  The worker is CPU-only, so this is a
     * bounded poll rather than a wait on a GPU.
     *
     * The LAST status is kept rather than the one in scope at the break: poll
     * returns false once the bake is no longer active, and reading `prog`
     * after that reads the memset, not the outcome.  The first version of this
     * loop did exactly that and reported IDLE for three green bakes -- a
     * broken instrument that looked like a broken feature. */
    /* TIME-BOUNDED, not iteration-bounded.  A fixed 200000-iteration spin was
     * enough while the bake was a procedural placeholder that finished
     * instantly; now that steps 2 and 3 run real convolutions the loop
     * completed in well under a second and reported a bake that had not
     * failed but simply had not finished -- and every LATER case then failed
     * with "bake already running", so one instrument produced three
     * indistinguishable red rows about a feature that works. */
    JceReflectionProbeBakeProgress prog;
    JceBakeStatus last = JCE_BAKE_STATUS_IDLE;
    const uint64_t t_start = jce_time_perf_counter();
    for (int i = 0; ; ++i) {
        if ((i & 1023) == 0 &&
            jce_time_perf_to_ms(t_start, jce_time_perf_counter()) > 60000.0)
            break;
        memset(&prog, 0, sizeof prog);
        const bool active = jce_reflection_probe_bake_poll(h, &prog);
        if (prog.status != JCE_BAKE_STATUS_IDLE) last = prog.status;
        if (!active) break;
        if (last == JCE_BAKE_STATUS_DONE ||
            last == JCE_BAKE_STATUS_FAILED ||
            last == JCE_BAKE_STATUS_CANCELLED)
            break;
        {   /* Let queued completions run on this (owner) thread. */
            JceAsyncPumpBudget budget;
            jce_async_pump_budget_init(&budget);
            (void)jce_async_default_pump(&budget);
        }
    }
    jce_scene_destroy(s);
    if (last != JCE_BAKE_STATUS_DONE) return 0u;

    /* The engine rewrites the extension to .ktx; `path` already carries it. */
    FILE *f = fopen(path, "rb");
    if (!f) return 0u;
    uint8_t hdr[64];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n < 40) return 0u;
    uint32_t w = 0;
    memcpy(&w, hdr + 36, sizeof w);
    return w;
}

static void test_the_authored_resolution_is_the_one_baked(void)
{
    /* THE FEATURE, and the two rows that discriminate: before this the
     * Inspector's separate {128,256,512} static decided, so 64 was not even
     * expressible from that panel. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(512, baked_face_size(512, 0, NULL, 0),
        "512 authored must bake 512 faces");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(64, baked_face_size(64, 0, NULL, 0),
        "64 authored must bake 64 faces -- the Inspector's old size static "
        "could not even express this value");
}

static void test_zero_keeps_the_engine_default(void)
{
    /* Back-compat row: a zeroed component bakes exactly the file it baked
     * before, because jce_reflection_probe_bake_submit owns the 256 default.
     * It passes under the old code too, which is why it is not the control. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(256, baked_face_size(0, 0, NULL, 0),
        "resolution 0 must keep the engine's own default");
}

static void test_the_engine_owns_the_ceiling(void)
{
    /* The Inspector still offers 1024.  One clamp, in the engine, rather than
     * each caller inventing one -- the browser had its own `> 512` line. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(512, baked_face_size(1024, 0, NULL, 0),
        "1024 must clamp to the engine ceiling, not to a caller's");
}

static void test_a_custom_probe_refuses_to_bake(void)
{
    /* A Custom probe already has an authored source; baking would overwrite
     * baked_cubemap_path while sr_rprobe_source_path still preferred the
     * custom one, so the work would produce a file nothing reads.  Unity
     * greys the Bake button out for the same reason. */
    char path[256] = {0};
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0,
        baked_face_size(256, JCE_REFLECTION_PROBE_CUSTOM, path, (int)sizeof path),
        "CUSTOM must not submit a bake");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", path,
        "and it must not hand back a path either, or a caller would record "
        "an artefact that was never written");
}

static void test_the_artefact_name_is_keyed_by_entity(void)
{
    /* Two panels used two schemes for one probe, one of them the component
     * POINTER -- so a saved baked_cubemap_path could never be re-baked to the
     * same file.  Same entity, same name, twice. */
    char a[256] = {0}, b[256] = {0};
    (void)baked_face_size(64, 0, a, (int)sizeof a);
    (void)baked_face_size(64, 0, b, (int)sizeof b);
    TEST_ASSERT_TRUE_MESSAGE(a[0] != 0, "a bake must hand back its path");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a, b,
        "the same entity must always name the same artefact");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(a, "ReflectionProbes/"),
        "and it must live where both panels already looked");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_authored_resolution_is_the_one_baked);
    RUN_TEST(test_zero_keeps_the_engine_default);
    RUN_TEST(test_the_engine_owns_the_ceiling);
    RUN_TEST(test_a_custom_probe_refuses_to_bake);
    RUN_TEST(test_the_artefact_name_is_keyed_by_entity);
    return UNITY_END();
}
