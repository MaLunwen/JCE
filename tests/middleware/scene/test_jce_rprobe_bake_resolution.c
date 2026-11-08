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

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_reflection_probe.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static JceScene *owned_scene;
static JceReflectionProbeBakeHandle owned_bake;
static char owned_directory[1024];
static char working_directory[1024];
static bool owns_directory;
static bool cleanup_failed;

/* This standalone fixture owns the process-wide executor. Draining it joins
 * actual work and pumps completion retirement; public bake DONE alone can
 * precede async terminal publication. Applications must not use this pattern
 * to shut down a shared executor after every bake. */
static bool retire_bake(bool cancel)
{
    if (cancel && owned_bake) jce_reflection_probe_bake_cancel(owned_bake);
    if (!jce_async_default_shutdown(cancel ? JCE_ASYNC_SHUTDOWN_CANCEL_ALL
                                           : JCE_ASYNC_SHUTDOWN_DRAIN, 60000u))
        return false;
    if (owned_bake) {
        JceReflectionProbeBakeProgress progress;
        (void)jce_reflection_probe_bake_poll(owned_bake, &progress);
        owned_bake = 0u;
    }
    return true;
}

/* Bake a probe authored with `resolution` and return the face size the written
 * KTX says it used, or 0 when nothing was written. */
static uint32_t baked_face_size(int resolution, int mode, char *out_path,
                                int cap, uint32_t *out_mips)
{
    if (out_mips) *out_mips = 0u;
    JceScene *s = owned_scene = jce_scene_create();
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
    JceReflectionProbeBakeHandle h = owned_bake =
        jce_scene_reflection_probe_bake(s, e, true, path, (int)sizeof path);
    if (out_path && cap > 0) snprintf(out_path, (size_t)cap, "%s", path);
    if (h == 0u) {
        jce_scene_destroy(owned_scene);
        owned_scene = NULL;
        return 0u;
    }
    if (!retire_bake(false)) return 0u; // tearDown cancels/retries on failure.
    JceReflectionProbeBakeProgress progress;
    const bool known = jce_reflection_probe_bake_poll(h, &progress);
    jce_scene_destroy(owned_scene);
    owned_scene = NULL;
    if (!known || progress.status != JCE_BAKE_STATUS_DONE) return 0u;

    /* The engine rewrites the extension to .ktx; `path` already carries it. */
    FILE *f = fopen(path, "rb");
    if (!f) return 0u;
    uint8_t hdr[64];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n < 60) return 0u;
    uint32_t w = 0;
    memcpy(&w, hdr + 36, sizeof w);
    if (out_mips) memcpy(out_mips, hdr + 56, sizeof *out_mips);
    return w;
}

static void test_the_authored_resolution_is_the_one_baked(void)
{
    /* THE FEATURE, and the two rows that discriminate: before this the
     * Inspector's separate {128,256,512} static decided, so 64 was not even
     * expressible from that panel. */
    uint32_t mips = 0u;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(512, baked_face_size(512, 0, NULL, 0, &mips),
        "512 authored must bake 512 faces");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(10u, mips, "512 must include its 1x1 mip");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(64, baked_face_size(64, 0, NULL, 0, &mips),
        "64 authored must bake 64 faces -- the Inspector's old size static "
        "could not even express this value");
    TEST_ASSERT_EQUAL_UINT32(7u, mips);
}

static void test_zero_keeps_the_engine_default(void)
{
    /* Back-compat row: a zeroed component bakes exactly the file it baked
     * before, because jce_reflection_probe_bake_submit owns the 256 default.
     * It passes under the old code too, which is why it is not the control. */
    uint32_t mips = 0u;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(256, baked_face_size(0, 0, NULL, 0, &mips),
        "resolution 0 must keep the engine's own default");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(9u, mips, "default 256 needs all nine mips");
}

static void test_the_engine_owns_the_ceiling(void)
{
    /* The Inspector still offers 1024.  One clamp, in the engine, rather than
     * each caller inventing one -- the browser had its own `> 512` line. */
    uint32_t mips = 0u;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(512, baked_face_size(1024, 0, NULL, 0, &mips),
        "1024 must clamp to the engine ceiling, not to a caller's");
    TEST_ASSERT_EQUAL_UINT32(10u, mips);
}

static void test_a_custom_probe_refuses_to_bake(void)
{
    /* A Custom probe already has an authored source; baking would overwrite
     * baked_cubemap_path while sr_rprobe_source_path still preferred the
     * custom one, so the work would produce a file nothing reads.  Unity
     * greys the Bake button out for the same reason. */
    char path[256] = {0};
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0,
        baked_face_size(256, JCE_REFLECTION_PROBE_CUSTOM, path, (int)sizeof path, NULL),
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
    (void)baked_face_size(64, 0, a, (int)sizeof a, NULL);
    (void)baked_face_size(64, 0, b, (int)sizeof b, NULL);
    TEST_ASSERT_TRUE_MESSAGE(a[0] != 0, "a bake must hand back its path");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a, b,
        "the same entity must always name the same artefact");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(a, "ReflectionProbes/"),
        "and it must live where both panels already looked");
}

void setUp(void)
{
    TEST_ASSERT_FALSE_MESSAGE(cleanup_failed,
        "prior cleanup did not retire its task/files; do not overwrite owners");
    TEST_ASSERT_NULL(owned_scene);
    TEST_ASSERT_EQUAL_UINT32(0u, owned_bake);
    TEST_ASSERT_FALSE(owns_directory);
    char current[1024];
    TEST_ASSERT_TRUE(jce_fs_host_get_current_dir(current, sizeof(current)));
    TEST_ASSERT_TRUE(jce_path_normalize(working_directory,
                                      sizeof(working_directory), current));
    TEST_ASSERT_TRUE(jce_path_join(owned_directory, sizeof(owned_directory),
                                   working_directory, "ReflectionProbes"));
    /* Never remove or overwrite output left by another invocation. CTest
     * supplies this target's dedicated work directory, separate from r5. */
    TEST_ASSERT_FALSE_MESSAGE(jce_fs_host_exists_dir(owned_directory),
                              "refuse pre-existing unowned probe directory");
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(owned_directory));
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(owned_directory));
    owns_directory = true;
}

void tearDown(void)
{
    const bool retired = retire_bake(true);
    bool removed = !owns_directory;
    if (retired) {
        if (owned_scene) jce_scene_destroy(owned_scene);
        owned_scene = NULL;
        if (owns_directory) {
            char expected[1024];
            const bool owned_path = jce_path_join(expected, sizeof(expected),
                working_directory, "ReflectionProbes") &&
                strcmp(expected, owned_directory) == 0;
            removed = owned_path && jce_fs_host_remove_recursive(owned_directory);
            if (removed) owns_directory = false;
        }
    }
    /* A timeout preserves the live task, scene and files for explicit retry;
     * it is a failure, never permission to free a possible worker borrow. */
    cleanup_failed = !retired || !removed;
    TEST_ASSERT_TRUE_MESSAGE(retired, "probe task did not retire during cleanup");
    TEST_ASSERT_TRUE_MESSAGE(removed, "owned probe output was not removed");
}

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
