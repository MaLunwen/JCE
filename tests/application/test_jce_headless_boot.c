/*
 * test_jce_headless_boot.c — integration test for the headless / dedicated-
 * server boot path (dependency/ABI audit §6 B3 → engine headless capability).
 *
 * Boots the FULL engine headless — exactly as a dedicated server would — and
 * verifies the contract:
 *   1. jce_engine_create() succeeds with desc.headless = true, on a process
 *      with no window (init'd with SDL_INIT_EVENTS only, NullRHI renderer).
 *   2. Inside the app's init, the services show a HEADLESS renderer
 *      (jce_renderer_is_headless), NO window, and NO audio — a server touches
 *      none of them.
 *   3. jce_engine_iterate() ticks the app's update() and keeps running.
 *   4. jce_engine_destroy() tears down cleanly.
 *
 * This is the reference verification that a game gets a working dedicated
 * server purely by setting desc.headless = true — no example project needed.
 *
 * Pure C99 consumer of the public engine + renderer ABI.
 */

#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/application/jce_runtime.h>
#include <jce/middleware/net/jce_net.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/renderer/jce_renderer.h>

#include "unity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Embedded-PAK blob symbols.  The ENGINE declares these extern and the
 * EXECUTABLE supplies them (see engine/src/application/jce_embedded_assets.h);
 * a project with no cooked assets links a 1-byte stub — exactly what
 * engine/templates/empty/CMakeLists.txt generates for a fresh project.  A
 * headless server needs no embedded assets, so the stub is the right shape. */
const unsigned char assets_pak_data[1] = { 0 };
const size_t        assets_pak_data_size = 0;

/* Captured by the app callbacks so the test can assert on the boot contract. */
static bool s_init_called;
static bool s_renderer_is_headless;
static bool s_window_is_null;
static bool s_audio_is_null;
static uint32_t s_update_count;
static bool s_exit_called;

void setUp(void)
{
    s_init_called          = false;
    s_renderer_is_headless = false;
    s_window_is_null       = false;
    s_audio_is_null        = false;
    s_update_count         = 0;
    s_exit_called          = false;
}

void tearDown(void) {}

static bool hb_init(const JceServices *svc, void *user_data)
{
    (void)user_data;
    s_init_called          = true;
    /* Headless boot: NullRHI renderer, no window, no audio. */
    s_renderer_is_headless = jce_renderer_is_headless(svc->renderer);
    s_window_is_null       = (svc->window == NULL);
    s_audio_is_null        = (svc->audio  == NULL);
    return true;
}

static void hb_update(float dt, void *user_data)
{
    (void)dt;
    (void)user_data;
    s_update_count++;
}

static void hb_exit(void *user_data)
{
    (void)user_data;
    s_exit_called = true;
}

/* Boot the engine headless, tick it, and tear it down — the full server path. */
void test_headless_engine_boots_ticks_and_shuts_down(void)
{
    JceAppDesc desc = (JceAppDesc){0};
    desc.name     = "headless-boot-test";
    desc.headless = true;         /* the entire point: no window / GPU / audio */
    desc.init     = hb_init;
    desc.update   = hb_update;
    desc.exit     = hb_exit;

    jce_engine_set_app_desc(&desc);

    char  arg0[] = "test_jce_headless_boot";
    char *argv[] = { arg0, NULL };
    JceEngine *e = jce_engine_create(1, argv);

    /* Booted headless on a windowless process. */
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "headless jce_engine_create returned NULL");
    TEST_ASSERT_TRUE_MESSAGE(s_init_called, "app init was not called");
    TEST_ASSERT_TRUE_MESSAGE(s_renderer_is_headless,
                             "renderer is not the headless NullRHI");
    TEST_ASSERT_TRUE_MESSAGE(s_window_is_null, "a window was created headless");
    TEST_ASSERT_TRUE_MESSAGE(s_audio_is_null, "audio was created headless");

    /* Tick a few frames: each returns CONTINUE and drives update(). */
    for (int i = 0; i < 5; ++i) {
        JceAppResult r = jce_engine_iterate(e);
        TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_APP_CONTINUE, r,
                                      "headless iterate did not CONTINUE");
    }
    TEST_ASSERT_TRUE_MESSAGE(s_update_count >= 5u,
                             "app update() was not ticked headless");

    jce_engine_destroy(e);
    TEST_ASSERT_TRUE_MESSAGE(s_exit_called, "app exit was not called on destroy");
}

/* ================================================================== */
/* Dedicated-server simulation stack, headless                         */
/* ================================================================== */
/*
 * The engine booting headless is only half the contract: a dedicated server
 * must also RUN the simulation with no GPU.  This exercises the exact stack a
 * server drives — scene (ECS) + physics via jce_runtime + an ENet host — while
 * the renderer is the NullRHI, and asserts the sim actually advances.
 */

#define HB_ENTITY_COUNT 8u

static JceScene   *s_scene;
static JceRuntime *s_runtime;
static JceNetHost *s_host;
static JceEntity   s_bodies[HB_ENTITY_COUNT];
static float       s_start_y;

static bool hb_sim_init(const JceServices *svc, void *user_data)
{
    (void)user_data;
    if (!hb_init(svc, user_data)) return false;

    s_scene = jce_scene_create();
    if (!s_scene) return false;

    /* Dynamic rigid bodies lifted above the origin so gravity has room to
     * move them — the observable proof that physics really steps headless. */
    s_start_y = 25.0f;
    for (uint32_t i = 0; i < HB_ENTITY_COUNT; ++i) {
        JceEntity e = jce_scene_create_entity(s_scene, "hb_body");
        JceTransform t;
        t.position = (jce_vec3){ (float)i * 2.0f, s_start_y, 0.0f };
        t.rotation = (jce_quat){ 0.0f, 0.0f, 0.0f, 1.0f };
        t.scale    = (jce_vec3){ 1.0f, 1.0f, 1.0f };
        jce_scene_set_transform(s_scene, e, &t);

        JceRigidBodyComponent rb = (JceRigidBodyComponent){0};
        rb.body_type     = JCE_BODY_DYNAMIC;
        rb.shape_type    = JCE_SHAPE_BOX;
        rb.mass          = 1.0f;
        rb.friction      = 0.5f;
        rb.use_gravity   = true;
        rb.gravity_scale = 1.0f;
        jce_scene_set_rigidbody(s_scene, e, &rb);

        s_bodies[i] = e;
    }

    /* Runtime = the server's simulation driver (scene + physics + script).
     * No renderer, no audio: a dedicated server needs neither. */
    JceRuntimeDesc rd = (JceRuntimeDesc){0};
    rd.scene          = s_scene;
    rd.pak            = NULL;
    rd.audio          = NULL;
    rd.enable_physics = true;
    s_runtime = jce_runtime_create(&rd);

    /* ENet host on an ephemeral-ish high port; a busy port must not fail the
     * test (CI runners are shared), so a NULL host is tolerated below. */
    JceNetHostDesc nd = (JceNetHostDesc){0};
    nd.port          = 45821;
    nd.max_peers     = 8;
    nd.channel_count = 2;
    s_host = jce_net_host_create(&nd, jce_allocator_default());
    return true;
}

static void hb_sim_update(float dt, void *user_data)
{
    (void)user_data;
    s_update_count++;
    if (s_runtime) jce_runtime_step(s_runtime, dt);
    else if (s_scene) jce_scene_update(s_scene, dt);
    if (s_host) {
        JceNetEvent ev;
        while (jce_net_poll(s_host, &ev, 0)) { /* drain */ }
        jce_net_service(s_host);
    }
}

static void hb_sim_exit(void *user_data)
{
    (void)user_data;
    s_exit_called = true;
    if (s_host)    { jce_net_host_destroy(s_host);   s_host = NULL; }
    if (s_runtime) { jce_runtime_destroy(s_runtime); s_runtime = NULL; }
    if (s_scene)   { jce_scene_destroy(s_scene);     s_scene = NULL; }
}

void test_headless_runs_scene_physics_and_network(void)
{
    JceAppDesc desc = (JceAppDesc){0};
    desc.name     = "headless-sim-test";
    desc.headless = true;
    desc.init     = hb_sim_init;
    desc.update   = hb_sim_update;
    desc.exit     = hb_sim_exit;

    jce_engine_set_app_desc(&desc);

    char  arg0[] = "test_jce_headless_boot";
    char *argv[] = { arg0, NULL };
    JceEngine *e = jce_engine_create(1, argv);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "headless engine (sim) failed to boot");
    TEST_ASSERT_TRUE_MESSAGE(s_renderer_is_headless, "renderer is not NullRHI");
    TEST_ASSERT_NOT_NULL_MESSAGE(s_scene, "scene was not created headless");
    TEST_ASSERT_NOT_NULL_MESSAGE(s_runtime,
                                 "jce_runtime_create failed headless "
                                 "(physics/scene sim unavailable on a server)");

    /* Tick long enough for gravity to visibly move the bodies. */
    for (int i = 0; i < 90; ++i) {
        JceAppResult r = jce_engine_iterate(e);
        TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_APP_CONTINUE, r,
                                      "headless sim iterate did not CONTINUE");
    }
    TEST_ASSERT_TRUE_MESSAGE(s_update_count >= 90u, "sim did not tick");

    /* The simulation actually advanced with no GPU: physics pulled the bodies
     * down from their spawn height. */
    {
        const JceTransform *t = jce_scene_get_transform(s_scene, s_bodies[0]);
        TEST_ASSERT_NOT_NULL_MESSAGE(t, "body transform missing after sim");
        TEST_ASSERT_TRUE_MESSAGE(t->position.y < s_start_y,
                                 "physics did not advance headless "
                                 "(body never fell)");
    }

    jce_engine_destroy(e);
    TEST_ASSERT_TRUE_MESSAGE(s_exit_called, "app exit not called on destroy");
}

/* ================================================================== */
/* JCE_HEADLESS=1 env override                                         */
/* ================================================================== */
/*
 * Deployment reality: the same game binary is often shipped as both client and
 * server, with the mode chosen by the launcher.  JCE_HEADLESS=1 must therefore
 * force headless even when the app descriptor asks for a windowed boot (same
 * diagnostic env family as JCE_BACKEND / JCE_AUDIO_DISABLE).
 */

static void hb_set_env(const char *name, const char *value)
{
#if defined(_WIN32)
    (void)_putenv_s(name, value);
#else
    (void)setenv(name, value, 1);
#endif
}

void test_jce_headless_env_forces_headless(void)
{
    hb_set_env("JCE_HEADLESS", "1");

    JceAppDesc desc = (JceAppDesc){0};
    desc.name     = "headless-env-test";
    desc.headless = false;        /* descriptor says windowed... */
    desc.init     = hb_init;
    desc.update   = hb_update;
    desc.exit     = hb_exit;

    jce_engine_set_app_desc(&desc);

    char  arg0[] = "test_jce_headless_boot";
    char *argv[] = { arg0, NULL };
    JceEngine *e = jce_engine_create(1, argv);

    /* ...but the env override wins: no window, NullRHI. */
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "JCE_HEADLESS=1 boot returned NULL");
    TEST_ASSERT_TRUE_MESSAGE(s_renderer_is_headless,
                             "JCE_HEADLESS=1 did not force the NullRHI renderer");
    TEST_ASSERT_TRUE_MESSAGE(s_window_is_null,
                             "JCE_HEADLESS=1 still created a window");

    jce_engine_destroy(e);
    hb_set_env("JCE_HEADLESS", "0");
}


/* ------------------------------------------------------------------ *
 *  Short-descriptor safety (ABI: jce_engine_set_app_desc_sized).
 *
 *  The engine COPIES JceAppDesc by value.  A game built against an older
 *  SDK allocates a SMALLER struct — this one has grown twice already
 *  (window_width/window_height, then headless) — so copying the engine's
 *  sizeof reads past the end of the caller's object.  The trailing fields
 *  then hold whatever happened to follow it in memory, and "headless"
 *  coming back true is a memorable way to discover that.
 *
 *  This simulates exactly that consumer: build a descriptor that ends
 *  where the OLD struct ended, poison the bytes after it, and hand the
 *  engine the old size.  A regression fails by booting headless off
 *  poison, or by crashing on a garbage callback pointer.
 * ------------------------------------------------------------------ */

void test_short_app_desc_is_not_read_past_its_end(void)
{
    /* Everything up to (but excluding) `headless` = the struct as it was
       before the dedicated-server work added that field. */
    const size_t legacy_size = offsetof(JceAppDesc, headless);
    TEST_ASSERT_TRUE_MESSAGE(legacy_size < sizeof(JceAppDesc),
        "headless is no longer the last member — update this test's notion "
        "of the legacy layout");

    /* Poisoned arena: the legacy-sized descriptor, followed by 0xFF.  If the
       engine over-reads, `headless` becomes non-zero => true. */
    unsigned char arena[sizeof(JceAppDesc) * 2];
    memset(arena, 0xFF, sizeof(arena));

    JceAppDesc staging = (JceAppDesc){0};
    staging.name   = "short-desc-abi-test";
    staging.init   = hb_init;
    staging.update = hb_update;
    staging.exit   = hb_exit;
    /* staging.headless stays false — and is NOT copied into the arena. */
    memcpy(arena, &staging, legacy_size);

    jce_engine_set_app_desc_sized((const JceAppDesc *)arena, legacy_size);

    char  arg0[] = "test_jce_headless_boot";
    char *argv[] = { arg0, NULL };
    JceEngine *e = jce_engine_create(1, argv);

    if (!e) {
        /* No display on this host: the engine legitimately fails to open a
           window.  That still proves the point — it did NOT read 0xFF into
           `headless` and silently take the headless path. */
        TEST_ASSERT_FALSE_MESSAGE(s_renderer_is_headless,
            "short descriptor was over-read: engine booted headless from "
            "poison bytes past the end of the caller's struct");
        return;
    }

    TEST_ASSERT_TRUE_MESSAGE(s_init_called, "app init was not called");
    TEST_ASSERT_FALSE_MESSAGE(s_renderer_is_headless,
        "short descriptor was over-read: `headless` picked up poison "
        "(0xFF) from beyond the caller's object");
    TEST_ASSERT_FALSE_MESSAGE(s_window_is_null,
        "windowed boot expected — headless was not requested");

    jce_engine_destroy(e);
}

/* A descriptor LONGER than the engine's (game newer than engine) must have
   its unknown tail ignored rather than overflowing the engine's copy. */
void test_long_app_desc_tail_is_ignored(void)
{
    unsigned char arena[sizeof(JceAppDesc) * 2];
    memset(arena, 0xAB, sizeof(arena));

    JceAppDesc staging = (JceAppDesc){0};
    staging.name     = "long-desc-abi-test";
    staging.headless = true;
    staging.init     = hb_init;
    staging.update   = hb_update;
    staging.exit     = hb_exit;
    memcpy(arena, &staging, sizeof(staging));

    jce_engine_set_app_desc_sized((const JceAppDesc *)arena, sizeof(arena));

    char  arg0[] = "test_jce_headless_boot";
    char *argv[] = { arg0, NULL };
    JceEngine *e = jce_engine_create(1, argv);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "oversized descriptor broke engine create");
    TEST_ASSERT_TRUE_MESSAGE(s_renderer_is_headless,
        "the fields the engine DOES understand were not honoured");

    jce_engine_destroy(e);
}

int main(void)
{
    /* NO WINDOW ON ANYONE'S DESKTOP, whatever any case in this file asks for.
     *
     * Most of these boots are headless and ignore this.  Two are not: the
     * short/long descriptor cases run AFTER test_jce_headless_env_forces_
     * headless has restored JCE_HEADLESS=0, and they boot from a descriptor
     * whose headless byte is deliberately poison -- so whether they open a
     * window is decided by the very bytes the test is asserting about.  That
     * is not a question worth leaving open in a test suite someone runs while
     * doing something else.
     *
     * JCE_WINDOW_HIDDEN withholds the mapping and nothing else, so every
     * assertion in this file is about the same code path it was before. */
    hb_set_env("JCE_WINDOW_HIDDEN", "1");

    UNITY_BEGIN();
    RUN_TEST(test_headless_engine_boots_ticks_and_shuts_down);
    RUN_TEST(test_headless_runs_scene_physics_and_network);
    RUN_TEST(test_jce_headless_env_forces_headless);
    RUN_TEST(test_short_app_desc_is_not_read_past_its_end);
    RUN_TEST(test_long_app_desc_tail_is_ignored);
    return UNITY_END();
}
