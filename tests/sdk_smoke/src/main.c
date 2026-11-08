/*
 * sdk_smoke main.c  Plain-C99 consumer of the installed JCE SDK.
 *
 * Exercises the native-interop contract end to end:
 *   1. jce_api_version() major must match the headers we compiled against.
 *   2. The PAK that jce_add_pak() cooked+packed+embedded must open through
 *      the engine (JceServices.pak) and contain our assets.
 *   3. The packed texture must be COOKED (.jceasset "JCEA" magic), not the
 *      raw PNG bytes — the historical "stub/raw PAK" regression.
 *   4. The native descriptor requests headless boot; the runner also selects
 *      JCE_BACKEND=noop. Update ticks run and the engine shuts down cleanly.
 *
 * Success contract for the runner (scripts/jce.py smoke): print exactly one
 * "JCE_SMOKE: OK ..." line and exit 0. Any "JCE_SMOKE: FAIL ..." line (or a
 * missing OK marker) fails the gate — the process exit code alone is not
 * trusted because an init failure aborts startup through the engine's own
 * error path.
 */

#include <jce/api.h>
/* Deliberately NOT in the api.h umbrella: JCE_MAIN() must appear in exactly
 * one TU per app, and the version header is the bindings' explicit ABI
 * handshake — include both the way a real consumer would. */
#include <jce/application/jce_main.h>
#include <jce/jce_version.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/ui/jce_imgui_renderer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool jce_smoke_water_spectrum(void);

static int   s_frames = 0;
static float s_elapsed = 0.0f;
static bool  s_checks_passed = false;

/* Pin the public enum without relying on C11 assertions or C++ headers. */
typedef char JceSmokeApiTierValues[
    JCE_GRAPHICS_API_TIER_STABLE == 0 &&
    JCE_GRAPHICS_API_TIER_MODERN == 1 &&
    JCE_GRAPHICS_API_TIER_CURRENT == 2 &&
    JCE_GRAPHICS_API_TIER_COUNT == 3 &&
    JCE_GRAPHICS_API_TIER_COMPATIBILITY == 4 ? 1 : -1];

/* Link the installed archive as well as compiling its public inline policy. */
static bool check_graphics_api_policy(void)
{
    static const JceRendererBackend backends[] = {
        JCE_BACKEND_AUTO, JCE_BACKEND_D3D11, JCE_BACKEND_D3D12,
        JCE_BACKEND_VULKAN, JCE_BACKEND_OPENGL, JCE_BACKEND_OPENGLES,
        JCE_BACKEND_METAL, JCE_BACKEND_NOOP,
        (JceRendererBackend)-1, (JceRendererBackend)99
    };
    static const bool compatibility[] = {
        false, false, false, false, true, true, false, false, false, false
    };
    static const JceGraphicsApiTier tiers[] = {
        JCE_GRAPHICS_API_TIER_STABLE, JCE_GRAPHICS_API_TIER_MODERN,
        JCE_GRAPHICS_API_TIER_CURRENT, JCE_GRAPHICS_API_TIER_COMPATIBILITY,
        (JceGraphicsApiTier)-1, JCE_GRAPHICS_API_TIER_COUNT
    };
    static const struct {
        JceRendererBackend backend;
        JceGraphicsApiTier tier;
        uint16_t major, minor;
    } minimums[] = {
        {JCE_BACKEND_OPENGL, JCE_GRAPHICS_API_TIER_COMPATIBILITY, 2, 1},
        {JCE_BACKEND_OPENGLES, JCE_GRAPHICS_API_TIER_COMPATIBILITY, 2, 0},
        {JCE_BACKEND_VULKAN, JCE_GRAPHICS_API_TIER_COMPATIBILITY, 1, 0},
        {JCE_BACKEND_AUTO, JCE_GRAPHICS_API_TIER_COMPATIBILITY, 0, 0},
        {JCE_BACKEND_NOOP, JCE_GRAPHICS_API_TIER_COMPATIBILITY, 0, 0},
        {JCE_BACKEND_OPENGL, (JceGraphicsApiTier)-1, 0, 0},
        {JCE_BACKEND_OPENGLES, JCE_GRAPHICS_API_TIER_COUNT, 0, 0}
    };
    for (size_t t = 0; t < sizeof tiers / sizeof tiers[0]; ++t) {
        for (size_t b = 0; b < sizeof backends / sizeof backends[0]; ++b) {
            const bool expected =
                tiers[t] == JCE_GRAPHICS_API_TIER_COMPATIBILITY && compatibility[b];
            if (jce_graphics_api_is_compatibility_gl_es(tiers[t], backends[b])
                    != expected) {
                printf("JCE_SMOKE: FAIL installed graphics profile policy\n");
                return false;
            }
        }
    }
    if (strcmp(jce_graphics_api_tier_name(
            JCE_GRAPHICS_API_TIER_COMPATIBILITY), "compatibility") != 0 ||
        strcmp(jce_graphics_api_tier_name(JCE_GRAPHICS_API_TIER_COUNT), "unknown") != 0) {
        printf("JCE_SMOKE: FAIL installed graphics tier name\n");
        return false;
    }
    for (size_t i = 0; i < sizeof minimums / sizeof minimums[0]; ++i) {
        const JceGraphicsApiVersion version = jce_renderer_api_tier_minimum(
            minimums[i].backend, minimums[i].tier);
        if (version.major != minimums[i].major ||
            version.minor != minimums[i].minor || version.patch != 0) {
            printf("JCE_SMOKE: FAIL installed graphics API floor\n");
            return false;
        }
    }
    return true;
}

/* Require the installed clone symbol and complete orthographic pose rather
 * than accepting a newer header paired with an older archive. */
static bool check_camera_clone(void)
{
    const JceCameraDesc desc = {
        .mode = JCE_CAMERA_ORTHO,
        .position = {2.0f, 3.0f, 4.0f},
        .target = {0.0f, 0.0f, 0.0f},
        .up = {0.0f, 1.0f, 0.0f},
        .fov_deg = 57.0f,
        .near_plane = 0.2f,
        .far_plane = 731.0f,
        .ortho_w = 0.0f,
        .ortho_h = 5.5f
    };
    JceCamera *source = jce_camera_create(&desc);
    JceCamera *copy = NULL;
    bool ok = false;
    if (source && jce_camera_set_pose(source, desc.position,
            jce_v3(0.25f, -0.4f, -0.9f), jce_v3(0.4f, 1.0f, 0.2f))) {
        const jce_mat4 view = jce_camera_view(source);
        const jce_mat4 projection = jce_camera_proj(source, 1.7f, false);
        copy = jce_camera_clone(source);
        if (copy && copy != source) {
            jce_camera_set_position(source, jce_v3(91.0f, 82.0f, 73.0f));
            jce_camera_destroy(source);
            source = NULL;
            const jce_mat4 copy_view = jce_camera_view(copy);
            const jce_mat4 copy_projection = jce_camera_proj(copy, 1.7f, false);
            ok = jce_camera_get_mode(copy) == JCE_CAMERA_ORTHO
                && memcmp(&view, &copy_view, sizeof view) == 0
                && memcmp(&projection, &copy_projection, sizeof projection) == 0;
        }
    }
    if (copy && copy != source) jce_camera_destroy(copy);
    if (source) jce_camera_destroy(source);
    if (!ok) printf("JCE_SMOKE: FAIL installed camera clone state/ownership\n");
    return ok;
}

/* Require the new installed symbol even when this host allocates no target. */
static bool check_offscreen_extent(void)
{
    uint32_t width = 101, height = 103;
    if (jce_offscreen_target_get_extent(NULL, &width, &height) || width || height) {
        printf("JCE_SMOKE: FAIL installed offscreen extent absent-owner contract\n");
        return false;
    }
    return true;
}

/* Require the installed C99 exclusive-copy symbol and its ownership reset. */
static bool check_exclusive_copy_symbol(void)
{
    bool created = true;
    if (jce_fs_host_copy_file_new(NULL, NULL, &created) || created ||
        jce_fs_host_copy_file_new(NULL, NULL, NULL)) {
        printf("JCE_SMOKE: FAIL installed exclusive copy invalid-input contract\n");
        return false;
    }
    return true;
}

/* Exercise the installed clock, including Web imports, without assuming
 * every device grants sub-millisecond precision. The observed minimum is
 * evidence for a separate resolution qualification, not a unit guarantee. */
static bool check_clock(void)
{
    const uint64_t frequency = jce_time_perf_freq();
    uint64_t previous = jce_time_perf_counter();
    uint64_t minimum = 0;
    unsigned positive = 0;
    if (!frequency) {
        printf("JCE_CLOCK: FAIL zero frequency\n");
        return false;
    }
    for (unsigned i = 0; i < 65536; ++i) {
        const uint64_t current = jce_time_perf_counter();
        if (current < previous || jce_time_perf_freq() != frequency) {
            printf("JCE_CLOCK: FAIL non-monotonic counter or changed frequency\n");
            return false;
        }
        const uint64_t delta = current - previous;
        const double expected = (double)delta / (double)frequency;
        const double seconds = jce_time_perf_to_seconds(previous, current);
        const double milliseconds = jce_time_perf_to_ms(previous, current);
        const double tolerance = 1e-12 * (expected + 1.0);
        const double expected_ms = seconds * 1000.0;
        if (!(seconds >= expected - tolerance
              && seconds <= expected + tolerance
              && milliseconds >= expected_ms - tolerance * 1000.0
              && milliseconds <= expected_ms + tolerance * 1000.0)) {
            printf("JCE_CLOCK: FAIL inconsistent conversion\n");
            return false;
        }
        if (delta) {
            ++positive;
            if (!minimum || delta < minimum) minimum = delta;
        }
        previous = current;
    }
    printf("JCE_CLOCK: OK frequency=%llu samples=65536 positive=%u "
           "min_ticks=%llu min_ms=%.9f\n",
           (unsigned long long)frequency, positive,
           (unsigned long long)minimum,
           (double)minimum * 1000.0 / (double)frequency);
    fflush(stdout);
    return true;
}

static bool check_cooked_asset(const JcePakArchive *pak, const char *path)
{
    const JcePakAsset *a = jce_pak_find(pak, path);
    if (!a) {
        printf("JCE_SMOKE: FAIL '%s' missing from embedded pak\n", path);
        return false;
    }
    if (a->original_size < 4) {
        printf("JCE_SMOKE: FAIL '%s' implausibly small (%u bytes)\n",
               path, (unsigned)a->original_size);
        return false;
    }
    unsigned char *buf = (unsigned char *)malloc(a->original_size);
    if (!buf) return false;
    size_t got = jce_pak_decompress(a, buf, a->original_size);
    bool cooked = got >= 4 && memcmp(buf, "JCEA", 4) == 0;
    bool raw_png = got >= 4 && buf[0] == 0x89 && buf[1] == 'P';
    free(buf);
    if (!cooked) {
        printf("JCE_SMOKE: FAIL '%s' is %s, expected cooked .jceasset "
               "(JCEA magic)\n", path, raw_png ? "a RAW png" : "not cooked");
        return false;
    }
    return true;
}

/* The introspection ABI, exercised THROUGH THE INSTALLED SDK.
 *
 * It lives here rather than in a tool of its own for one reason: this is the
 * only consumer in the tree that compiles against the packaged headers and
 * links the packaged library, so it is the only place that can show
 * <jce/api_introspect.h> is actually reachable from an SDK.  A tool built
 * in-tree would prove the code compiles, which is a different and much weaker
 * statement -- and this repository has shipped a header that worked in-tree
 * and broke every SDK consumer on the first line of api.h.
 *
 * Opt-in through the environment, so the default smoke path -- the ABI
 * regression gate -- prints exactly what it always printed.
 *
 *   JCE_SMOKE_INTROSPECT=components   the component schema
 *   JCE_SMOKE_INTROSPECT=stats        a statistics snapshot
 *
 * The JSON is bracketed by markers because stdout also carries the engine's
 * own startup log, and a caller that tried to find JSON by looking for a
 * leading brace would pick up whatever else happened to start with one. */
static void smoke_introspect(void)
{
    const char *what = getenv("JCE_SMOKE_INTROSPECT");
    size_t need;
    char *buf;

    if (!what || !what[0])
        return;

    if (strcmp(what, "components") == 0)
        need = jce_introspect_components_json(NULL, 0);
    else if (strcmp(what, "stats") == 0)
        need = jce_introspect_stats_json(NULL, NULL, 0);
    else {
        printf("JCE_INTROSPECT: FAIL unknown subject '%s' "
               "(components|stats)\n", what);
        return;
    }

    buf = (char *)malloc(need + 1u);
    if (!buf) {
        printf("JCE_INTROSPECT: FAIL out of memory for %u bytes\n",
               (unsigned)need);
        return;
    }
    if (strcmp(what, "components") == 0)
        jce_introspect_components_json(buf, need + 1u);
    else
        jce_introspect_stats_json(NULL, buf, need + 1u);

    printf("JCE_INTROSPECT_BEGIN %s\n%s\nJCE_INTROSPECT_END\n", what, buf);
    fflush(stdout);
    free(buf);
}


static bool smoke_init(const JceServices *svc, void *ud)
{
    (void)ud;

    uint32_t v = jce_api_version();
    if ((v >> 24) != (uint32_t)JCE_VERSION_MAJOR) {
        printf("JCE_SMOKE: FAIL api major mismatch: lib=0x%08x hdr=%d\n",
               v, JCE_VERSION_MAJOR);
        return false;
    }

    if (!svc || !svc->pak) {
        printf("JCE_SMOKE: FAIL engine booted without the embedded pak\n");
        return false;
    }
    uint32_t count = jce_pak_count(svc->pak);
    if (count == 0) {
        printf("JCE_SMOKE: FAIL embedded pak is empty\n");
        return false;
    }

    if (!check_cooked_asset(svc->pak, "smoke.png")) return false;
    if (!check_cooked_asset(svc->pak, "smoke.wav")) return false;
    if (!check_graphics_api_policy()) return false;
    if (!jce_smoke_water_spectrum()) return false;
    if (!check_camera_clone()) return false;
    if (!check_offscreen_extent()) return false;
    if (!check_exclusive_copy_symbol()) return false;
    /* Installed host-only diagnostic symbols: init is outside app.draw and
     * this headless consumer has no initialized ImGui renderer. */
    if (jce_engine_frame_observation_enabled() ||
        jce_engine_report_frame_observations(NULL, 0, 0)) {
        printf("JCE_SMOKE: FAIL installed frame observation accepted init report\n");
        return false;
    }
    JceImGuiDrawObservation ui_observation = {0};
    if (!jce_imgui_renderer_draw_observed(0, &ui_observation, sizeof ui_observation) ||
        ui_observation.struct_size != sizeof ui_observation ||
        ui_observation.flags || ui_observation.queued_commands ||
        ui_observation.main_submits || ui_observation.recording_submits) {
        printf("JCE_SMOKE: FAIL installed unavailable ImGui observation\n");
        return false;
    }

    /* Require the new video symbol from the installed SDK, not source headers. */
    if (jce_video_set_preview_max_dimension(JCE_VIDEO_INVALID, 1280u)
        || jce_video_is_ready_to_play(JCE_VIDEO_INVALID)) {
        printf("JCE_SMOKE: FAIL video preview accepted an invalid handle\n");
        return false;
    }

    /* Reach the installed streaming surface through api.h alone. */
    JceReadSource *source = jce_read_source_open_memory("abc", 3u, true);
    char bytes[2];
    if (!source || jce_read_source_read_at(source, 1u, bytes, sizeof(bytes)) != 2u
        || memcmp(bytes, "bc", 2u) != 0) {
        jce_read_source_close(source);
        printf("JCE_SMOKE: FAIL installed streaming reader\n");
        return false;
    }
    jce_read_source_close(source);
    if (jce_audio_stream_is_quiescent(NULL, JCE_VOICE_INVALID)) {
        printf("JCE_SMOKE: FAIL installed audio NULL quiescence\n");
        return false;
    }
    /* Link the installed endpoint-gain getter without requiring a device. */
    if (jce_audio_get_master_volume(NULL) != 0.0f) {
        printf("JCE_SMOKE: FAIL installed audio NULL endpoint gain\n");
        return false;
    }
    if (jce_audio_file_open(NULL) != NULL) {
        printf("JCE_SMOKE: FAIL installed audio file validation\n");
        return false;
    }

    if (!check_clock()) {
        printf("JCE_SMOKE: FAIL installed clock\n");
        return false;
    }

    printf("JCE_SMOKE: OK version=%s pak_assets=%u\n",
           jce_api_version_string(), (unsigned)count);
    fflush(stdout);
    s_checks_passed = true;

    smoke_introspect();
    return true;
}

static void smoke_update(float dt, void *ud)
{
    (void)ud;
    s_frames++;
    s_elapsed += dt;
}

static bool smoke_should_quit(void *ud)
{
    (void)ud;
    /* Quit on WALL time, not frames: under the noop renderer frames are
       sub-millisecond, and async startup work (worker pool spin-up, asset
       indexing, audio device init) must settle before teardown. */
    return s_elapsed >= 0.75f && s_frames >= 60;
}

static JceAppDesc smoke_get_desc(void)
{
    JceAppDesc d;
    memset(&d, 0, sizeof d);
    d.name          = "JceSdkSmoke";
    d.init          = smoke_init;
    d.update        = smoke_update;
    d.should_quit   = smoke_should_quit;
    d.window_width  = 320;
    d.window_height = 200;
#if !JCE_PLATFORM_WEB
    /* Noop alone still creates a native window; this consumer needs no draw. */
    d.headless      = true;
#endif
    return d;
}

JCE_MAIN(smoke_get_desc)
