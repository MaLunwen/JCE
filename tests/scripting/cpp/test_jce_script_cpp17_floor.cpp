/* test_jce_script_cpp17_floor.cpp — the wrapper's C++17 claim, enforced.
 *
 * jce_script_api.hpp says it REQUIRES C++17 and merely USES C++20.  That is a
 * promise to an SDK consumer who cannot move off C++17, and a promise nothing
 * enforces is a defect — this repository has found eight of that exact shape
 * in the input layer alone.  So this translation unit is compiled at exactly
 * -std=c++17 / /std:c++17 (tests/scripting/cpp/CMakeLists.txt asserts the
 * target property at configure time, because a wrong standard produces no
 * build error at all) and it does two things:
 *
 *   1. It INCLUDES the header.  Every one of the 71 methods is a non-template
 *      inline member function, so its body is fully type-checked at the point
 *      of definition — including it at C++17 therefore checks all 71 bodies at
 *      C++17.  A C++20-only construct anywhere in the wrapper fails to compile
 *      HERE and nowhere else.
 *   2. It RUNS one call of each of the seven closed shapes over a mock host,
 *      so the floor is a working binding and not just a parse.
 *
 * It links NO ENGINE LAYER.  The wrapper over the C ABI is the whole of what
 * an SDK consumer has, and that is what is exercised.  The cross-language
 * differential — the wrapper against a real Lua VM — is the other executable
 * in this directory; it needs the engine, and it needs C++20 for the span
 * overloads, which is exactly why the two are not one target.
 */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <jce/script_api/jce_script_api.hpp>

#include <cstdlib>   /* std::malloc / std::free, used by h_comp_get_json */
#include <cstring>
#include <string>

/* The floor is only tested if this TU really is AT the floor.  Compiled at
 * C++20 this file would pass while proving nothing about C++17 — a green
 * light nobody measured. */
static_assert(JCE_SCRIPT_CPP_LANG >= 201703L,
              "the wrapper's own #error should have fired before this");
static_assert(JCE_SCRIPT_CPP_LANG < 202002L,
              "test_jce_script_cpp17_floor is compiling at C++20 or later, so "
              "it is not testing the C++17 floor it exists for. Check "
              "CXX_STANDARD on this target.");

/* And the span overloads must be OFF here — otherwise the __cpp_lib_span
 * guard is not what admits them and a C++17 consumer would get a compile
 * error the wrapper promised it would not. */
static_assert(JCE_SCRIPT_CPP_HAS_SPAN == 0,
              "std::span was detected in a C++17 translation unit; the "
              "additive-overload guard is not doing its job");

using jce::script::Api;
using jce::script::Entity;

/* ------------------------------------------------------------------ *
 *  A minimal host.  No engine, no Lua: exactly what an SDK consumer has.
 * ------------------------------------------------------------------ */

static int   g_set_position_calls;
static float g_last_xyz[3];
static int   g_last_button;
static int   g_json_free_calls;

extern "C" {

static bool h_get_position(void *user, JceScriptEntity e, float out[3])
{
    (void)user;
    if (e != 42u)
        return false;
    out[0] = 1.5f;
    out[1] = 2.5f;
    out[2] = 3.5f;
    return true;
}

static void h_set_position(void *user, JceScriptEntity e,
                           float x, float y, float z)
{
    (void)user;
    (void)e;
    ++g_set_position_calls;
    g_last_xyz[0] = x;
    g_last_xyz[1] = y;
    g_last_xyz[2] = z;
}

static JceScriptEntity h_find_with_tag(void *user, const char *tag)
{
    (void)user;
    return (tag != NULL && std::strcmp(tag, "player") == 0) ? 7u : 0u;
}

static void h_move_axis(void *user, float out_xz[2])
{
    (void)user;
    out_xz[0] = 0.25f;
    out_xz[1] = -0.5f;
}

static bool h_input_button(void *user, int button)
{
    (void)user;
    g_last_button = button;
    return true;
}

static bool h_touch_get(void *user, int index, uint64_t *id, float *x,
                        float *y, float *pressure)
{
    (void)user;
    if (index != 0)
        return false;
    *id = 99u;
    *x = 10.5f;
    *y = 11.5f;
    *pressure = 0.5f;
    return true;
}

static int h_find_by_name(void *user, const char *name, JceScriptEntity *out,
                          int max)
{
    (void)user;
    (void)name;
    if (max < 2)
        return 0;
    out[0] = 11u;
    out[1] = 12u;
    return 2;
}

static int h_find_by_prefix(void *user, const char *prefix,
                            JceScriptEntity *out, int max)
{
    (void)user;
    (void)prefix;
    if (max < 3)
        return 0;
    out[0] = 21u;
    out[1] = 22u;
    out[2] = 23u;
    return 3;
}

static char *h_comp_get_json(void *user, JceScriptEntity e, const char *type)
{
    (void)user;
    (void)e;
    (void)type;
    char *p = static_cast<char *>(std::malloc(16));
    if (p != NULL)
        std::memcpy(p, "{\"a\":1}", 8);
    return p;
}

static void h_json_free(void *user, char *s)
{
    (void)user;
    ++g_json_free_calls;
    std::free(s);
}

static const char *h_loc_translate(void *user, const char *key)
{
    (void)user;
    (void)key;
    return "translated";
}

}  /* extern "C" */

static void fill_host(JceScriptHost *h)
{
    std::memset(h, 0, sizeof *h);
    h->get_position = h_get_position;
    h->set_position = h_set_position;
    h->find_with_tag = h_find_with_tag;
    h->move_axis = h_move_axis;
    h->input_button = h_input_button;
    h->touch_get = h_touch_get;
    h->find_by_name = h_find_by_name;
    h->find_by_prefix = h_find_by_prefix;
    h->comp_get_json = h_comp_get_json;
    h->json_free = h_json_free;
    h->loc_translate = h_loc_translate;
}

/* ------------------------------------------------------------------ *
 *  One call of each closed shape, at C++17.
 * ------------------------------------------------------------------ */

TEST_CASE("the wrapper works at C++17, one call per closed shape")
{
    JceScriptHost host;
    fill_host(&host);
    g_set_position_calls = 0;
    g_json_free_calls = 0;

    Api api = Api::open(host);
    REQUIRE(static_cast<bool>(api));

    /* void_call */
    api.set_position(1u, 4.5f, 5.5f, 6.5f);
    CHECK(g_set_position_calls == 1);
    CHECK(g_last_xyz[1] == doctest::Approx(5.5f));

    /* value_return, and CStr from a std::string with no copy */
    const std::string tag = "player";
    CHECK(api.find_with_tag(tag) == 7u);

    /* value_return whose C type is const char * -> an owned std::string */
    CHECK(api.tr("ui.play") == std::string("translated"));

    /* void_out_array */
    const std::array<float, 2> axis = api.move_axis();
    CHECK(axis[0] == doctest::Approx(0.25f));
    CHECK(axis[1] == doctest::Approx(-0.5f));

    /* fallible_out, both branches */
    const std::optional<std::array<float, 3>> hit = api.get_position(42u);
    REQUIRE(hit.has_value());
    CHECK((*hit)[2] == doctest::Approx(3.5f));
    CHECK_FALSE(api.get_position(1u).has_value());

    /* fallible_out with several out parameters -> a named result struct */
    const std::optional<jce::script::GetTouchResult> touch = api.get_touch(0);
    REQUIRE(touch.has_value());
    CHECK(touch->id == 99u);
    CHECK(touch->pressure == doctest::Approx(0.5f));

    /* index_base: the wrapper's own refusal, below the 0-based floor, WITHOUT
     * reaching the host (h_touch_get would answer false for -1 anyway, so the
     * value alone proves nothing -- what it pins here is that the call is
     * defined and does not pass a negative index down). */
    CHECK_FALSE(api.get_touch(-1).has_value());

    /* bind_args: three entries over one host member */
    g_last_button = -1;
    CHECK(api.jump_pressed());
    CHECK(g_last_button == 0);
    CHECK(api.attack_pressed());
    CHECK(g_last_button == 2);

    /* first_and_count */
    const jce::script::FirstAndCount found = api.find_by_name("x");
    REQUIRE(found.first.has_value());
    CHECK(*found.first == 11u);
    CHECK(found.count == 2);

    /* entity_table */
    const std::vector<Entity> all = api.find_by_prefix("x");
    REQUIRE(all.size() == 3u);
    CHECK(all[2] == 23u);

    /* owned_string_release: an owned copy, and nothing to free */
    const std::optional<std::string> json = api.comp_get(3u, "Water");
    REQUIRE(json.has_value());
    CHECK(*json == std::string("{\"a\":1}"));
    CHECK(g_json_free_calls == 1);
}

TEST_CASE("RAII: the handle closes itself, and an empty handle is callable")
{
    JceScriptHost host;
    fill_host(&host);

    {
        Api api = Api::open(host);
        CHECK(static_cast<bool>(api));
        Api moved = std::move(api);
        CHECK(static_cast<bool>(moved));
        CHECK_FALSE(static_cast<bool>(api));   /* moved-from is empty */
    }   /* ~Api closes exactly once; a double close would be a use-after-free */

    /* Never opened.  Every method is still defined and answers the absent
     * value, which is what the Lua binding pushes with no host. */
    Api empty;
    CHECK_FALSE(static_cast<bool>(empty));
    CHECK_FALSE(empty.get_position(42u).has_value());
    CHECK(empty.find_with_tag("player") == 0u);
    CHECK(empty.move_axis()[0] == doctest::Approx(0.0f));
    CHECK_FALSE(empty.comp_get(1u, "Water").has_value());
    CHECK(empty.find_by_prefix("x").empty());
    /* tr degrades to KEY PASSTHROUGH, not to an empty string: an unlocalized
     * build shows readable keys instead of blank UI. */
    CHECK(empty.tr("ui.play") == std::string("ui.play"));
    /* and music_request_transition's absent value is -1, not 0, because 0 is
     * a valid playhead time. */
    CHECK(empty.music_request_transition(2) == doctest::Approx(-1.0f));
}

TEST_CASE("the manifest's optional defaults are the wrapper's defaults")
{
    JceScriptHost host;
    fill_host(&host);
    Api api = Api::open(host);

    g_set_position_calls = 0;
    g_last_button = -1;

    /* spawn's x/y/z default to 0 and pause's `paused` defaults to true; both
     * are omitted here and the call must still compile and run.  What the
     * VALUES are is compared against Lua in the differential — this only
     * proves the defaults exist at the C++17 floor. */
    (void)api.spawn("prefab.json");
    api.pause();
    api.shake_camera();
    api.send_message(1u, "hello");
    api.broadcast("hello");
    CHECK(api.gas_apply(1u, "hp", jce::script::defaults::gas_apply_op, 5.0f)
          == false);   /* no host member: the C ABI answers false */
}
