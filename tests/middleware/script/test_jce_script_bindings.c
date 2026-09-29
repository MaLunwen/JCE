/* test_jce_script_bindings.c
 *
 * Headless mock-host coverage for the gameplay scripting-depth bindings added
 * to the Lua VM (physics raycast / impulse / velocity, one-shot audio, and the
 * UI slider / toggle / text accessors).
 *
 * The VM (jce_script.c) is a GENERIC Lua host: every engine reach goes through
 * the JceScriptHost callback table the runtime installs.  This test installs a
 * MOCK host whose callbacks record their marshalled arguments into a struct (and
 * return canned values), runs Lua source that calls each binding, and asserts
 * the C↔Lua marshalling is exact in BOTH directions.  No runtime, no physics,
 * no audio, no bgfx — purely the binding glue.
 *
 * It also pins the default-safe contract: a second VM whose new callbacks are
 * all NULL must let the SAME script run without crashing, returning nil/0 /
 * doing nothing — old scripts (and partially-wired hosts) behave identically.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>   /* snprintf */
#include <string.h>

/* ── Mock host recorder ─────────────────────────────────────────────────── */
typedef struct {
    /* raycast */
    int    raycast_calls;
    float  ray_origin[3];
    float  ray_dir[3];
    float  ray_max;
    bool   ray_return_hit;        /* what the mock raycast returns */
    JceScriptRaycastHit ray_canned; /* the hit it fills in on a "hit" */

    /* apply_impulse / set_velocity */
    int    impulse_calls;
    JceScriptEntity impulse_e;
    float  impulse_xyz[3];
    int    setvel_calls;
    JceScriptEntity setvel_e;
    float  setvel_xyz[3];
    bool   getvel_return;
    float  getvel_canned[3];

    /* play_sound */
    int    sound_calls;
    char   sound_path[256];
    bool   sound_had_pos;
    float  sound_pos[3];
    float  sound_volume;
    int    spatial_sound_calls;
    float  sound_min_distance;
    float  sound_max_distance;
    float  sound_rolloff;

    /* raw pointer input */
    float  pointer_delta[2];
    float  pointer_wheel;
    uint32_t pointer_buttons;

    /* ui */
    int    slider_set_calls;
    float  slider_set_val;
    float  slider_get_canned;
    int    progress_set_calls;
    float  progress_set_val;
    float  progress_get_canned;
    int    toggle_set_calls;
    bool   toggle_set_val;
    bool   toggle_get_canned;
    int    text_set_calls;
    char   text_set_val[512];

    /* hierarchy */
    int    set_parent_calls;
    JceScriptEntity parent_child;
    JceScriptEntity parent_value;
    bool   parent_preserve_world;
    bool   set_parent_canned;
    JceScriptEntity get_parent_canned;
} Recorder;

static Recorder g_rec;

/* ── Mock callbacks ─────────────────────────────────────────────────────── */
static bool mock_raycast(void *user, const float origin[3], const float dir[3],
                         float max_dist, JceScriptRaycastHit *out)
{
    Recorder *r = (Recorder *)user;
    r->raycast_calls++;
    memcpy(r->ray_origin, origin, sizeof r->ray_origin);
    memcpy(r->ray_dir, dir, sizeof r->ray_dir);
    r->ray_max = max_dist;
    if (!r->ray_return_hit) return false;     /* miss: leave *out untouched */
    *out = r->ray_canned;
    return true;
}

static void mock_apply_impulse(void *user, JceScriptEntity e,
                               float x, float y, float z)
{
    Recorder *r = (Recorder *)user;
    r->impulse_calls++;
    r->impulse_e = e;
    r->impulse_xyz[0] = x; r->impulse_xyz[1] = y; r->impulse_xyz[2] = z;
}

static void mock_set_velocity(void *user, JceScriptEntity e,
                              float x, float y, float z)
{
    Recorder *r = (Recorder *)user;
    r->setvel_calls++;
    r->setvel_e = e;
    r->setvel_xyz[0] = x; r->setvel_xyz[1] = y; r->setvel_xyz[2] = z;
}

static bool mock_get_velocity(void *user, JceScriptEntity e, float out[3])
{
    Recorder *r = (Recorder *)user;
    (void)e;
    if (!r->getvel_return) return false;
    memcpy(out, r->getvel_canned, sizeof r->getvel_canned);
    return true;
}

static void mock_play_sound(void *user, const char *path, const float pos[3],
                            float volume)
{
    Recorder *r = (Recorder *)user;
    r->sound_calls++;
    snprintf(r->sound_path, sizeof r->sound_path, "%s", path ? path : "");
    r->sound_had_pos = (pos != NULL);
    if (pos) { r->sound_pos[0] = pos[0]; r->sound_pos[1] = pos[1]; r->sound_pos[2] = pos[2]; }
    else     { r->sound_pos[0] = r->sound_pos[1] = r->sound_pos[2] = 0.0f; }
    r->sound_volume = volume;
}

static void mock_play_sound_spatial(void *user, const char *path,
                                    const float pos[3], float volume,
                                    float min_distance, float max_distance,
                                    float rolloff)
{
    mock_play_sound(user, path, pos, volume);
    Recorder *r = (Recorder *)user;
    r->spatial_sound_calls++;
    r->sound_min_distance = min_distance;
    r->sound_max_distance = max_distance;
    r->sound_rolloff = rolloff;
}

static void mock_pointer_delta(void *user, float out_xy[2])
{
    Recorder *r = (Recorder *)user;
    out_xy[0] = r->pointer_delta[0];
    out_xy[1] = r->pointer_delta[1];
}

static float mock_pointer_wheel(void *user)
{
    return ((Recorder *)user)->pointer_wheel;
}

static bool mock_pointer_button(void *user, int button)
{
    Recorder *r = (Recorder *)user;
    if (button < 1 || button > 32) return false;
    return (r->pointer_buttons & (1u << (button - 1))) != 0;
}

static bool mock_ui_get_slider(void *user, JceScriptEntity e, float *out)
{
    Recorder *r = (Recorder *)user;
    (void)e;
    *out = r->slider_get_canned;
    return true;
}

static void mock_ui_set_slider(void *user, JceScriptEntity e, float v)
{
    Recorder *r = (Recorder *)user;
    (void)e;
    r->slider_set_calls++;
    r->slider_set_val = v;
}

/* UIProgressBar's pair.  Present here for the same reason the slider's is:
 * a binding that reaches the WRONG SLOT calls a different function with a
 * different signature and is invisible until it corrupts something. */
static bool mock_ui_get_progress(void *user, JceScriptEntity e, float *out)
{
    Recorder *r = (Recorder *)user;
    (void)e;
    *out = r->progress_get_canned;
    return true;
}

static void mock_ui_set_progress(void *user, JceScriptEntity e, float v)
{
    Recorder *r = (Recorder *)user;
    (void)e;
    r->progress_set_calls++;
    r->progress_set_val = v;
}

static bool mock_ui_get_toggle(void *user, JceScriptEntity e, bool *out)
{
    Recorder *r = (Recorder *)user;
    (void)e;
    *out = r->toggle_get_canned;
    return true;
}

static void mock_ui_set_toggle(void *user, JceScriptEntity e, bool v)
{
    Recorder *r = (Recorder *)user;
    (void)e;
    r->toggle_set_calls++;
    r->toggle_set_val = v;
}

static void mock_ui_set_text(void *user, JceScriptEntity e, const char *txt)
{
    Recorder *r = (Recorder *)user;
    (void)e;
    r->text_set_calls++;
    snprintf(r->text_set_val, sizeof r->text_set_val, "%s", txt ? txt : "");
}

static bool mock_set_parent(void *user, JceScriptEntity child,
                            JceScriptEntity parent, bool preserve_world)
{
    Recorder *r = (Recorder *)user;
    r->set_parent_calls++;
    r->parent_child = child;
    r->parent_value = parent;
    r->parent_preserve_world = preserve_world;
    return r->set_parent_canned;
}

static JceScriptEntity mock_get_parent(void *user, JceScriptEntity child)
{
    Recorder *r = (Recorder *)user;
    r->parent_child = child;
    return r->get_parent_canned;
}

/* Build a host that wires every new binding to the recorder. */
static JceScriptHost make_mock_host(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user          = &g_rec;
    h.raycast       = mock_raycast;
    h.apply_impulse = mock_apply_impulse;
    h.set_velocity  = mock_set_velocity;
    h.get_velocity  = mock_get_velocity;
    h.play_sound    = mock_play_sound;
    h.play_sound_spatial = mock_play_sound_spatial;
    h.pointer_delta = mock_pointer_delta;
    h.pointer_wheel = mock_pointer_wheel;
    h.pointer_button = mock_pointer_button;
    h.ui_get_slider = mock_ui_get_slider;
    h.ui_set_slider = mock_ui_set_slider;
    h.ui_get_progress = mock_ui_get_progress;
    h.ui_set_progress = mock_ui_set_progress;
    h.ui_get_toggle = mock_ui_get_toggle;
    h.ui_set_toggle = mock_ui_set_toggle;
    h.ui_set_text   = mock_ui_set_text;
    h.set_parent    = mock_set_parent;
    h.get_parent    = mock_get_parent;
    return h;
}

/* Run a chunk that stores its observable results into GLOBALS the test reads
 * back, by exposing them through a tiny module table.  We use the public
 * source-eval entry jce_script_instantiate_source: the chunk's BODY runs at
 * load time (before it returns the module table), so a bare script that calls
 * the bindings at top level exercises them immediately.  We stash the multi-
 * return values into module fields and read them via a follow-up chunk that
 * also runs at load time. */

void setUp(void)    { memset(&g_rec, 0, sizeof g_rec); }
void tearDown(void) {}

/* ── raycast: hit + miss ────────────────────────────────────────────────── */
static void test_raycast_hit_and_miss(void)
{
    JceScriptHost h = make_mock_host();
    /* Canned hit: entity 42, point (1,2,3), normal (0,1,0), dist 7.5. */
    g_rec.ray_return_hit = true;
    g_rec.ray_canned.entity = 42;
    g_rec.ray_canned.point[0]  = 1.0f; g_rec.ray_canned.point[1]  = 2.0f; g_rec.ray_canned.point[2]  = 3.0f;
    g_rec.ray_canned.normal[0] = 0.0f; g_rec.ray_canned.normal[1] = 1.0f; g_rec.ray_canned.normal[2] = 0.0f;
    g_rec.ray_canned.distance  = 7.5f;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    /* HIT: capture the 8 return values into module fields, asserted in Lua via
     * error() so a mismatch fails the chunk loudly; but simplest is to assert
     * the marshalled INPUTS via the recorder + the OUTPUTS via globals stored
     * by the script.  We store the outputs into a global table `R`. */
    JceScriptInstance hit = jce_script_instantiate_source(s, "@ray_hit",
        "R = {}\n"
        "R.e, R.px, R.py, R.pz, R.nx, R.ny, R.nz, R.d = "
        "  jce.raycast(10, 20, 30, 0, 0, -1, 99)\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, hit);

    /* Inputs marshalled C-side. */
    TEST_ASSERT_EQUAL_INT(1, g_rec.raycast_calls);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, g_rec.ray_origin[0]);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, g_rec.ray_origin[1]);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, g_rec.ray_origin[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  g_rec.ray_dir[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  g_rec.ray_dir[1]);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, g_rec.ray_dir[2]);
    TEST_ASSERT_EQUAL_FLOAT(99.0f, g_rec.ray_max);

    /* Outputs delivered to Lua: read them back via a follow-up chunk that
     * asserts each value and only "returns a table" when all match — so a
     * non-zero instance proves the values arrived correctly. */
    JceScriptInstance chk = jce_script_instantiate_source(s, "@ray_hit_check",
        "assert(R.e == 42, 'entity')\n"
        "assert(R.px == 1 and R.py == 2 and R.pz == 3, 'point')\n"
        "assert(R.nx == 0 and R.ny == 1 and R.nz == 0, 'normal')\n"
        "assert(R.d == 7.5, 'dist')\n"
        "local M = {}; return M\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, chk);   /* 0 would mean an assert() blew the chunk up */

    /* MISS: mock returns false → Lua sees a single 0 (entity). */
    g_rec.ray_return_hit = false;
    JceScriptInstance miss = jce_script_instantiate_source(s, "@ray_miss",
        "local e, rest = jce.raycast(0,0,0, 1,0,0, 5)\n"
        "assert(e == 0, 'miss entity must be 0')\n"
        "assert(rest == nil, 'miss must return a single value')\n"
        "local M = {}; return M\n", 3);
    TEST_ASSERT_NOT_EQUAL(0, miss);
    TEST_ASSERT_EQUAL_INT(2, g_rec.raycast_calls);

    jce_script_destroy(s);
}

/* ── apply_impulse / set_velocity / get_velocity ────────────────────────── */
static void test_physics_force_bindings(void)
{
    JceScriptHost h = make_mock_host();
    g_rec.getvel_return = true;
    g_rec.getvel_canned[0] = 4.0f; g_rec.getvel_canned[1] = 5.0f; g_rec.getvel_canned[2] = 6.0f;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@force",
        "jce.apply_impulse(7, 1.5, -2.5, 3.5)\n"
        "jce.set_velocity(8, -1, 0, 2)\n"
        "local vx, vy, vz = jce.get_velocity(8)\n"
        "assert(vx == 4 and vy == 5 and vz == 6, 'velocity readback')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    TEST_ASSERT_EQUAL_INT(1, g_rec.impulse_calls);
    TEST_ASSERT_EQUAL_UINT64(7u, g_rec.impulse_e);
    TEST_ASSERT_EQUAL_FLOAT(1.5f,  g_rec.impulse_xyz[0]);
    TEST_ASSERT_EQUAL_FLOAT(-2.5f, g_rec.impulse_xyz[1]);
    TEST_ASSERT_EQUAL_FLOAT(3.5f,  g_rec.impulse_xyz[2]);

    TEST_ASSERT_EQUAL_INT(1, g_rec.setvel_calls);
    TEST_ASSERT_EQUAL_UINT64(8u, g_rec.setvel_e);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, g_rec.setvel_xyz[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  g_rec.setvel_xyz[1]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f,  g_rec.setvel_xyz[2]);

    jce_script_destroy(s);
}

/* ── play_sound: 3D + 2D arities ────────────────────────────────────────── */
static void test_play_sound_arities(void)
{
    JceScriptHost h = make_mock_host();
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    /* 3D: path + coords + volume. */
    JceScriptInstance a = jce_script_instantiate_source(s, "@snd3d",
        "jce.play_sound('a.wav', 1, 2, 3, 0.5)\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_EQUAL_INT(1, g_rec.sound_calls);
    TEST_ASSERT_EQUAL_STRING("a.wav", g_rec.sound_path);
    TEST_ASSERT_TRUE(g_rec.sound_had_pos);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, g_rec.sound_pos[0]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, g_rec.sound_pos[1]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, g_rec.sound_pos[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, g_rec.sound_volume);

    /* 2D: path only → no position, default volume 1.0. */
    JceScriptInstance b = jce_script_instantiate_source(s, "@snd2d",
        "jce.play_sound('b.wav')\n"
        "local M = {}; return M\n", 2);
    TEST_ASSERT_NOT_EQUAL(0, b);
    TEST_ASSERT_EQUAL_INT(2, g_rec.sound_calls);
    TEST_ASSERT_EQUAL_STRING("b.wav", g_rec.sound_path);
    TEST_ASSERT_FALSE(g_rec.sound_had_pos);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, g_rec.sound_volume);

    jce_script_destroy(s);
}

static void test_play_sound_forwards_spatial_attenuation(void)
{
    JceScriptHost h = make_mock_host();
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance instance = jce_script_instantiate_source(s, "@snd_far",
        "jce.play_sound('thunder.wav', 4, 5, 6, 0.8, 8, 1200, 0.35)\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, instance);
    TEST_ASSERT_EQUAL_INT(1, g_rec.spatial_sound_calls);
    TEST_ASSERT_EQUAL_STRING("thunder.wav", g_rec.sound_path);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, g_rec.sound_pos[0]);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, g_rec.sound_pos[1]);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, g_rec.sound_pos[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.8f, g_rec.sound_volume);
    TEST_ASSERT_EQUAL_FLOAT(8.0f, g_rec.sound_min_distance);
    TEST_ASSERT_EQUAL_FLOAT(1200.0f, g_rec.sound_max_distance);
    TEST_ASSERT_EQUAL_FLOAT(0.35f, g_rec.sound_rolloff);

    jce_script_destroy(s);
}

static void test_pointer_input_bindings(void)
{
    JceScriptHost h = make_mock_host();
    g_rec.pointer_delta[0] = 12.5f;
    g_rec.pointer_delta[1] = -3.0f;
    g_rec.pointer_wheel = 2.0f;
    g_rec.pointer_buttons = 1u;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    JceScriptInstance inst = jce_script_instantiate_source(s, "@pointer",
        "local dx, dy = jce.get_pointer_delta()\n"
        "assert(dx == 12.5 and dy == -3.0, 'pointer delta')\n"
        "assert(jce.get_pointer_wheel() == 2.0, 'pointer wheel')\n"
        "assert(jce.is_pointer_down(1) == true, 'left pointer')\n"
        "assert(jce.is_pointer_down(2) == false, 'right pointer')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);
    jce_script_destroy(s);
}

/* ── UI slider / toggle / text ──────────────────────────────────────────── */
static void test_ui_bindings(void)
{
    JceScriptHost h = make_mock_host();
    /* 0.5 is exactly representable in BOTH float and double, so the binding's
     * float->lua_Number(double) promotion compares == in Lua (0.7f would not). */
    g_rec.slider_get_canned = 0.5f;
    g_rec.progress_get_canned = 0.125f;
    g_rec.toggle_get_canned = true;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@ui",
        "jce.ui_set_slider(11, 0.25)\n"
        "assert(jce.ui_get_slider(11) == 0.5, 'slider readback')\n"
        "jce.ui_set_progress(14, 0.75)\n"
        "assert(jce.ui_get_progress(14) == 0.125, 'progress readback')\n"
        "jce.ui_set_toggle(12, true)\n"
        "assert(jce.ui_get_toggle(12) == true, 'toggle readback')\n"
        "jce.ui_set_text(13, 'hi')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    TEST_ASSERT_EQUAL_INT(1, g_rec.slider_set_calls);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, g_rec.slider_set_val);
    TEST_ASSERT_EQUAL_INT(1, g_rec.progress_set_calls);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, g_rec.progress_set_val);
    TEST_ASSERT_EQUAL_INT(1, g_rec.toggle_set_calls);
    TEST_ASSERT_TRUE(g_rec.toggle_set_val);
    TEST_ASSERT_EQUAL_INT(1, g_rec.text_set_calls);
    TEST_ASSERT_EQUAL_STRING("hi", g_rec.text_set_val);

    jce_script_destroy(s);
}

static void test_hierarchy_bindings(void)
{
    JceScriptHost h = make_mock_host();
    g_rec.set_parent_canned = true;
    g_rec.get_parent_canned = 29;

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@hierarchy",
        "assert(jce.set_parent(17, 29, true) == true, 'set parent')\n"
        "assert(jce.get_parent(17) == 29, 'get parent')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);
    TEST_ASSERT_EQUAL_INT(1, g_rec.set_parent_calls);
    TEST_ASSERT_EQUAL_UINT64(17u, g_rec.parent_child);
    TEST_ASSERT_EQUAL_UINT64(29u, g_rec.parent_value);
    TEST_ASSERT_TRUE(g_rec.parent_preserve_world);

    jce_script_destroy(s);
}

/* ── NULL-callback safety ───────────────────────────────────────────────── */
/* A host that wires NONE of the new callbacks (all NULL) must let the same
 * script run without crashing: each new binding is a safe no-op returning
 * nil / 0 / false (default-safe contract — old scripts behave identically). */
static void test_null_callbacks_are_safe(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);   /* user + every callback NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@nullsafe",
        /* raycast with no host → single 0, no extra values. */
        "local e, rest = jce.raycast(0,0,0, 0,0,-1, 10)\n"
        "assert(e == 0 and rest == nil, 'raycast no-host -> 0')\n"
        /* force ops are no-ops (must not error). */
        "jce.apply_impulse(1, 1, 2, 3)\n"
        "jce.set_velocity(1, 4, 5, 6)\n"
        /* getters return nil. */
        "assert(jce.get_velocity(1) == nil, 'get_velocity no-host -> nil')\n"
        "assert(jce.ui_get_slider(1) == nil, 'ui_get_slider no-host -> nil')\n"
        "assert(jce.ui_get_toggle(1) == nil, 'ui_get_toggle no-host -> nil')\n"
        /* setters are no-ops. */
        "jce.ui_set_slider(1, 0.5)\n"
        "jce.ui_set_toggle(1, true)\n"
        "jce.ui_set_text(1, 'x')\n"
        "jce.play_sound('z.wav', 1, 2, 3, 0.5)\n"
        "assert(jce.set_parent(1, 2, true) == false, 'set_parent no-host')\n"
        "assert(jce.get_parent(1) == 0, 'get_parent no-host')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);   /* survived: no crash, no assert tripped */

    jce_script_destroy(s);
}

/* A host that wires ONLY user (so callbacks are NULL) must equally be safe and
 * leave the recorder untouched — proving the bindings guard on the callback,
 * not just on have_host. */
static void test_partial_host_records_nothing(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = &g_rec;           /* have_host true, but new callbacks NULL */

    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@partial",
        "jce.apply_impulse(1, 1, 2, 3)\n"
        "jce.ui_set_text(1, 'should not record')\n"
        "local M = {}; return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    TEST_ASSERT_EQUAL_INT(0, g_rec.impulse_calls);
    TEST_ASSERT_EQUAL_INT(0, g_rec.text_set_calls);

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_raycast_hit_and_miss);
    RUN_TEST(test_physics_force_bindings);
    RUN_TEST(test_play_sound_arities);
    RUN_TEST(test_play_sound_forwards_spatial_attenuation);
    RUN_TEST(test_pointer_input_bindings);
    RUN_TEST(test_ui_bindings);
    RUN_TEST(test_hierarchy_bindings);
    RUN_TEST(test_null_callbacks_are_safe);
    RUN_TEST(test_partial_host_records_nothing);
    return UNITY_END();
}
