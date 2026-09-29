/*
 * test_jce_sequencer_easing_events.c  (FEATURE 8.4 — deterministic slices)
 *
 * Exercises the REAL jce_sequencer eval + event/camera-cut path:
 *   - per-key easing remaps the bracket fraction through jce_ease before the
 *     lerp (easeInQuad at the segment midpoint yields 0.25, not 0.5), while a
 *     key with no "ease" stays linear (backward-compatible);
 *   - color tracks ease too;
 *   - EVENT keys fire the registered sink exactly once, in (t_prev, t_now],
 *     with the authored handler name + key time, and NOT outside the range;
 *   - a CAMERA-CUT track parses and the active-camera seam (vcam priority
 *     raise) is invoked at the key crossing via the scene-sequencer handler.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_sequencer.h>
#include <jce/middleware/scene/jce_sequencer.h>
#include <jce/os/core/jce_easing.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── Per-key easing through the real evaluator ──────────────────────── */

static void test_eased_float_remaps_fraction(void)
{
    /* One float track, two keys 0..2 -> 0..10, segment eased easeInQuad. */
    static const char *json =
        "{\"duration\":2.0,\"fps\":30,\"loop\":false,\"tracks\":["
        "{\"name\":\"eased\",\"type\":0,"
         "\"keys\":[{\"t\":0,\"v\":0,\"ease\":\"easeInQuad\"},"
                   "{\"t\":2,\"v\":10}]}"
        "]}";
    JceSequencer *seq = jce_sequencer_load_text(json, strlen(json));
    TEST_ASSERT_NOT_NULL(seq);
    TEST_ASSERT_EQUAL_INT(1, jce_sequencer_track_count(seq));

    /* The starting key carries the easing for its outgoing segment. */
    TEST_ASSERT_EQUAL_INT(JCE_EASE_QUAD_IN,
                          (int)jce_sequencer_track_key_ease(seq, 0, 0));

    /* Midpoint: linear fraction 0.5 -> quad-in 0.25 -> value 2.5 (not 5.0). */
    float mid = jce_sequencer_track_eval_float(seq, 0, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.5f, mid);
    TEST_ASSERT_TRUE(mid < 5.0f - 1e-3f);   /* definitely not the linear value */

    /* Endpoints unaffected by easing (ease(0)=0, ease(1)=1). */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f,  jce_sequencer_track_eval_float(seq, 0, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, jce_sequencer_track_eval_float(seq, 0, 2.0f));

    /* Quarter point: u=0.25 -> 0.0625 -> 0.625. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.625f,
                             jce_sequencer_track_eval_float(seq, 0, 0.5f));

    jce_sequencer_free(seq);
}

static void test_linear_default_unchanged(void)
{
    /* No "ease" authored anywhere -> identical to the historical linear lerp. */
    static const char *json =
        "{\"duration\":2.0,\"fps\":30,\"loop\":false,\"tracks\":["
        "{\"name\":\"lin\",\"type\":0,"
         "\"keys\":[{\"t\":0,\"v\":0},{\"t\":2,\"v\":10}]}"
        "]}";
    JceSequencer *seq = jce_sequencer_load_text(json, strlen(json));
    TEST_ASSERT_NOT_NULL(seq);

    TEST_ASSERT_EQUAL_INT(JCE_EASE_LINEAR,
                          (int)jce_sequencer_track_key_ease(seq, 0, 0));

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.5f, jce_sequencer_track_eval_float(seq, 0, 0.5f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, jce_sequencer_track_eval_float(seq, 0, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 7.5f, jce_sequencer_track_eval_float(seq, 0, 1.5f));

    jce_sequencer_free(seq);
}

static void test_numeric_and_canonical_ease_spellings(void)
{
    /* "ease": <int ordinal>, the canonical "QuadInOut" id, and the swapped
     * "easeInOutQuad" authoring spelling all resolve to the same type. */
    static const char *json =
        "{\"duration\":3.0,\"tracks\":["
        "{\"name\":\"a\",\"type\":0,\"keys\":[{\"t\":0,\"v\":0,\"ease\":3},{\"t\":1,\"v\":1}]},"
        "{\"name\":\"b\",\"type\":0,\"keys\":[{\"t\":0,\"v\":0,\"ease\":\"QuadInOut\"},{\"t\":1,\"v\":1}]},"
        "{\"name\":\"c\",\"type\":0,\"keys\":[{\"t\":0,\"v\":0,\"ease\":\"easeInOutQuad\"},{\"t\":1,\"v\":1}]}"
        "]}";
    JceSequencer *seq = jce_sequencer_load_text(json, strlen(json));
    TEST_ASSERT_NOT_NULL(seq);
    TEST_ASSERT_EQUAL_INT(JCE_EASE_QUAD_INOUT, (int)jce_sequencer_track_key_ease(seq, 0, 0));
    TEST_ASSERT_EQUAL_INT(JCE_EASE_QUAD_INOUT, (int)jce_sequencer_track_key_ease(seq, 1, 0));
    TEST_ASSERT_EQUAL_INT(JCE_EASE_QUAD_INOUT, (int)jce_sequencer_track_key_ease(seq, 2, 0));
    jce_sequencer_free(seq);
}

static void test_eased_color_remaps_fraction(void)
{
    /* Color track 0..2: rgb (0,0,0) -> (1,1,1), eased easeInQuad. */
    static const char *json =
        "{\"duration\":2.0,\"tracks\":["
        "{\"name\":\"col\",\"type\":2,"
         "\"keys\":[{\"t\":0,\"rgb\":[0,0,0],\"ease\":\"QuadIn\"},"
                   "{\"t\":2,\"rgb\":[1,1,1]}]}"
        "]}";
    JceSequencer *seq = jce_sequencer_load_text(json, strlen(json));
    TEST_ASSERT_NOT_NULL(seq);
    float rgb[3] = { -1, -1, -1 };
    jce_sequencer_track_eval_color(seq, 0, 1.0f, rgb);  /* u=0.5 -> 0.25 */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, rgb[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, rgb[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, rgb[2]);
    jce_sequencer_free(seq);
}

/* ── EVENT key dispatch through the real fire path ──────────────────── */

typedef struct {
    int      count;
    char     last_name[64];
    float    last_time;
    uint64_t last_entity;
} EventCapture;

static void event_sink(const char *name, float time, uint64_t entity, void *user)
{
    EventCapture *c = (EventCapture *)user;
    c->count++;
    strncpy(c->last_name, name ? name : "", sizeof(c->last_name) - 1);
    c->last_name[sizeof(c->last_name) - 1] = '\0';
    c->last_time   = time;
    c->last_entity = entity;
}

static void test_event_fires_once_in_range(void)
{
    static const char *json =
        "{\"duration\":5.0,\"tracks\":["
        "{\"name\":\"ev\",\"type\":1,"
         "\"keys\":[{\"t\":1.5,\"name\":\"onBeat\",\"entity\":99},"
                   "{\"t\":3.0,\"name\":\"onDrop\"}]}"
        "]}";
    JceSequencer *seq = jce_sequencer_load_text(json, strlen(json));
    TEST_ASSERT_NOT_NULL(seq);

    /* count-only API also recognises EVENT tracks. */
    TEST_ASSERT_EQUAL_INT(1, jce_sequencer_track_events_in_range(seq, 0, 1.0f, 2.0f));

    /* (1.0, 2.0] crosses only the t=1.5 "onBeat" key, exactly once. */
    EventCapture cap;
    memset(&cap, 0, sizeof cap);
    int fired = jce_sequencer_track_fire_events_in_range(seq, 0, 1.0f, 2.0f,
                                                         event_sink, &cap);
    TEST_ASSERT_EQUAL_INT(1, fired);
    TEST_ASSERT_EQUAL_INT(1, cap.count);
    TEST_ASSERT_EQUAL_STRING("onBeat", cap.last_name);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.5f, cap.last_time);
    TEST_ASSERT_EQUAL_UINT64(99, cap.last_entity);

    /* (3.5, 4.0] contains no key -> the sink must NOT be invoked. */
    memset(&cap, 0, sizeof cap);
    fired = jce_sequencer_track_fire_events_in_range(seq, 0, 3.5f, 4.0f,
                                                     event_sink, &cap);
    TEST_ASSERT_EQUAL_INT(0, fired);
    TEST_ASSERT_EQUAL_INT(0, cap.count);

    /* Half-open boundary: a key exactly at t_now fires; exactly at t_prev does
     * not.  (1.5, 3.0] -> only "onDrop" (the t=1.5 key is excluded). */
    memset(&cap, 0, sizeof cap);
    fired = jce_sequencer_track_fire_events_in_range(seq, 0, 1.5f, 3.0f,
                                                     event_sink, &cap);
    TEST_ASSERT_EQUAL_INT(1, fired);
    TEST_ASSERT_EQUAL_STRING("onDrop", cap.last_name);

    /* A property track yields nothing through the event fire path. */
    jce_sequencer_free(seq);
}

/* ── CAMERA-CUT track parse + active-camera seam ────────────────────── */

static void test_camera_cut_parses(void)
{
    static const char *json =
        "{\"duration\":4.0,\"tracks\":["
        "{\"name\":\"cuts\",\"type\":3,"
         "\"keys\":[{\"t\":0.5,\"camera\":11},{\"t\":2.5,\"camera\":22}]}"
        "]}";
    JceSequencer *seq = jce_sequencer_load_text(json, strlen(json));
    TEST_ASSERT_NOT_NULL(seq);
    TEST_ASSERT_EQUAL_INT(JCE_SEQ_TRACK_CAMERA_CUT,
                          (int)jce_sequencer_track_type(seq, 0));
    TEST_ASSERT_EQUAL_INT(2, jce_sequencer_track_key_count(seq, 0));
    /* "camera" alias populates the key entity. */
    TEST_ASSERT_EQUAL_UINT64(11, jce_sequencer_track_key_entity(seq, 0, 0));
    TEST_ASSERT_EQUAL_UINT64(22, jce_sequencer_track_key_entity(seq, 0, 1));

    /* Camera-cut keys are crossing-counted like events. */
    TEST_ASSERT_EQUAL_INT(1, jce_sequencer_track_events_in_range(seq, 0, 0.0f, 1.0f));

    jce_sequencer_free(seq);
}

/* The driver-level seam: a CAMERA-CUT key crossing must call the registered
 * camera-cut handler with the target entity.  We drive the REAL
 * jce_scene_sequencer_update by attaching a SequencePlayer to an entity and
 * loading a .seq via jce_scene_sequencer's own loader path. */

typedef struct {
    int       count;
    JceEntity last_target;
    float     last_time;
} CutCapture;

static CutCapture g_cut_cap;

static void cut_observer(JceScene *s, JceEntity target, float time, void *user)
{
    (void)s; (void)user;
    g_cut_cap.count++;
    g_cut_cap.last_target = target;
    g_cut_cap.last_time   = time;
}

static void test_camera_cut_seam_invoked_at_key(void)
{
    /* Build a scene with a vcam target + a holder running an inline .seq via a
     * temp file (jce_scene_sequencer opens seq_path on first update). */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity cam = jce_scene_create_entity(s, "CineCam");
    JceVirtualCameraComponent vc;
    memset(&vc, 0, sizeof vc);
    vc.priority = 0;
    vc.active   = false;
    vc.fov_deg  = 60.0f;
    jce_scene_set_virtual_camera(s, cam, &vc);

    /* Another, higher-priority vcam so the cut must raise above it. */
    JceEntity other = jce_scene_create_entity(s, "OtherCam");
    JceVirtualCameraComponent ov;
    memset(&ov, 0, sizeof ov);
    ov.priority = 50;
    ov.active   = true;
    jce_scene_set_virtual_camera(s, other, &ov);

    /* Write a temp .seq.json with a single camera-cut key at t=0.1 targeting
     * `cam` (per-key entity = the real runtime entity id). */
    char path[256];
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(path, sizeof path, "%s/jce_test_cut_%llu.seq.json",
             tmp, (unsigned long long)cam);
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fprintf(f,
        "{\"duration\":1.0,\"fps\":30,\"loop\":false,\"tracks\":["
        "{\"name\":\"cuts\",\"type\":3,\"keys\":[{\"t\":0.1,\"camera\":%llu}]}"
        "]}", (unsigned long long)cam);
    fclose(f);

    JceEntity holder = jce_scene_create_entity(s, "Director");
    JceSequencePlayerComponent sp;
    memset(&sp, 0, sizeof sp);
    strncpy(sp.seq_path, path, sizeof(sp.seq_path) - 1);
    sp.play_on_awake = true;
    sp.speed         = 1.0f;
    jce_scene_set_sequence_player(s, holder, &sp);

    /* Register the camera-cut observer (mirrors what the runtime does). */
    memset(&g_cut_cap, 0, sizeof g_cut_cap);
    jce_scene_sequencer_set_camera_cut_handler(cut_observer, NULL);

    /* First update opens the seq at t=0 and advances by 0.2s, crossing the
     * t=0.1 key -> the seam must fire once with `cam`. */
    jce_scene_sequencer_update(s, 0.2f);

    TEST_ASSERT_EQUAL_INT(1, g_cut_cap.count);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)cam, (uint64_t)g_cut_cap.last_target);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.1f, g_cut_cap.last_time);

    /* Active-camera seam: the cut raised `cam` above the other vcam (50). */
    JceVirtualCameraComponent *cv = jce_scene_get_virtual_camera(s, cam);
    TEST_ASSERT_NOT_NULL(cv);
    TEST_ASSERT_TRUE(cv->active);
    TEST_ASSERT_TRUE(cv->priority > 50);

    /* A second update past the end crosses no further key -> no extra fire. */
    jce_scene_sequencer_update(s, 0.2f);
    TEST_ASSERT_EQUAL_INT(1, g_cut_cap.count);

    jce_scene_sequencer_set_camera_cut_handler(NULL, NULL);
    jce_scene_destroy(s);
    remove(path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_eased_float_remaps_fraction);
    RUN_TEST(test_linear_default_unchanged);
    RUN_TEST(test_numeric_and_canonical_ease_spellings);
    RUN_TEST(test_eased_color_remaps_fraction);
    RUN_TEST(test_event_fires_once_in_range);
    RUN_TEST(test_camera_cut_parses);
    RUN_TEST(test_camera_cut_seam_invoked_at_key);
    return UNITY_END();
}
