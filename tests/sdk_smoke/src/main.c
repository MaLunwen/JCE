/*
 * sdk_smoke main.c  Plain-C99 consumer of the installed JCE SDK.
 *
 * Exercises the native-interop contract end to end:
 *   1. jce_api_version() major must match the headers we compiled against.
 *   2. The PAK that jce_add_pak() cooked+packed+embedded must open through
 *      the engine (JceServices.pak) and contain our assets.
 *   3. The packed texture must be COOKED (.jceasset "JCEA" magic), not the
 *      raw PNG bytes — the historical "stub/raw PAK" regression.
 *   4. The engine must boot headless (runner sets JCE_BACKEND=noop), run a
 *      frame, and shut down cleanly.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int   s_frames = 0;
static float s_elapsed = 0.0f;
static bool  s_checks_passed = false;

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
    if (jce_audio_file_open(NULL) != NULL) {
        printf("JCE_SMOKE: FAIL installed audio file validation\n");
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
    return d;
}

JCE_MAIN(smoke_get_desc)
