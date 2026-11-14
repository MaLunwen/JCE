/* CPU execution of the real private overlay owner and public SDK types/math.
 * The physics line source and renderer are counted ports, not a Bullet/GPU run. */
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/renderer/jce_debug_draw.h>
#include <jce/os/core/jce_log.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>
#include "../../engine/src/renderer/jce_debug_line_sample.h"
#include "../../editor/src/scene/jce_editor_physics_debug_settings.h"

/* Existing per-user persistence and runtime position endpoints are counted
 * ports here. The policy/overlay owners and all SDK types/math are real. */
static bool have_radius, have_center, have_frustum, position_available;
static float stored_radius;
static int stored_center, stored_frustum;
static unsigned setting_writes, player_reads;
static jce_vec3 player_position;
extern "C" float jce_editor_ui_state_load_float(const char *, float fallback, float, float)
{ return have_radius ? stored_radius : fallback; }
extern "C" void jce_editor_ui_state_save_float(const char *, float value)
{ have_radius = true; stored_radius = value; ++setting_writes; }
extern "C" int jce_editor_ui_state_load_int(const char *key, int fallback, int, int)
{
    if (std::strcmp(key, jce_editor_physics_debug_settings::center_key) == 0)
        return have_center ? stored_center : fallback;
    return have_frustum ? stored_frustum : fallback;
}
extern "C" void jce_editor_ui_state_save_int(const char *key, int value)
{
    if (std::strcmp(key, jce_editor_physics_debug_settings::center_key) == 0)
        { have_center = true; stored_center = value; }
    else { have_frustum = true; stored_frustum = value; }
    ++setting_writes;
}
extern "C" bool JCE_CALL jce_runtime_get_player_position(const JceRuntime *runtime, jce_vec3 *out)
{
    ++player_reads;
    if (!runtime || !position_available) return false;
    *out = player_position; return true;
}

namespace {
struct Line { jce_vec3 a, b; uint32_t color; };
std::vector<Line> source, drawn;
std::vector<uint32_t> observed_flags, query_flags;
uint32_t query_status; bool query_success;
JcePhysicsDebugQuery last_query{};
uint32_t flags;
jce_debug_line_fn sink_fn;
void *sink_ud;
unsigned flushes, warnings, checks, failures;

void check(bool result, const char *name)
{
    ++checks;
    if (!result) { ++failures; std::fprintf(stderr, "%s\n", name); }
}
uint32_t JCE_CALL port_get_flags(void) { return flags; }
void JCE_CALL port_set_flags(uint32_t value) { flags = value; }
void JCE_CALL port_set_sink(jce_debug_line_fn fn, void *ud)
{ sink_fn = fn; sink_ud = ud; }
void JCE_CALL port_world_flush(JcePhysicsWorld *)
{
    observed_flags.push_back(flags);
    /* Contacts/constraints port intentionally has no shape lines. */
}
bool JCE_CALL port_query(JcePhysicsWorld *, const JcePhysicsDebugQuery *q,
    jce_debug_line_fn fn, void *ud, JcePhysicsDebugQueryStats *stats)
{
    query_flags.push_back(q->flags);
    last_query = *q;
    check(q->max_triangles == 8192 && q->max_lines == 32768, "native source budget supplied");
    check(q->max_objects && q->max_shapes && q->max_mesh_parts, "native metadata budgets supplied");
    stats->status = query_status;
    if (!query_success) return false;
    for (const Line &line : source) fn(line.a, line.b, line.color, ud);
    return true;
}
void port_line(jce_vec3 a, jce_vec3 b, uint32_t color)
{ drawn.push_back({a, b, color}); }
void port_render_flush(uint16_t id, const JceRenderer *)
{ check(id == 121, "requested overlay view retained"); ++flushes; }
void JCE_CALL port_log(JceLogLevel, const char *, const char *, int, const char *, ...)
{ ++warnings; }

static_assert(std::is_same<decltype(&port_get_flags), decltype(&jce_physics_debug_get_flags)>::value, "get flags ABI");
static_assert(std::is_same<decltype(&port_set_flags), decltype(&jce_physics_debug_set_flags)>::value, "set flags ABI");
static_assert(std::is_same<decltype(&port_set_sink), decltype(&jce_physics_debug_set_line_sink)>::value, "sink ABI");
static_assert(std::is_same<decltype(&port_query), decltype(&jce_physics_debug_query_shapes)>::value, "bounded query ABI");
static_assert(std::is_same<decltype(&port_world_flush), decltype(&jce_physics_debug_flush)>::value, "physics flush ABI");
static_assert(std::is_same<decltype(&port_line), decltype(&jce_debug_draw_line)>::value, "line ABI");
static_assert(std::is_same<decltype(&port_render_flush), decltype(&jce_debug_draw_flush)>::value, "draw flush ABI");
static_assert(std::is_same<decltype(&port_log), decltype(&jce_log_write)>::value, "logger ABI");
} // namespace

#define jce_physics_debug_get_flags port_get_flags
#define jce_physics_debug_set_flags port_set_flags
#define jce_physics_debug_set_line_sink port_set_sink
#define jce_physics_debug_flush port_world_flush
#define jce_physics_debug_query_shapes port_query
#define jce_debug_draw_line port_line
#define jce_debug_draw_flush port_render_flush
#define jce_log_write port_log
#include "../../editor/src/scene/jce_editor_physics_overlay.h"
#undef jce_log_write
#undef jce_debug_draw_flush
#undef jce_debug_draw_line
#undef jce_physics_debug_flush
#undef jce_physics_debug_query_shapes
#undef jce_physics_debug_set_line_sink
#undef jce_physics_debug_set_flags
#undef jce_physics_debug_get_flags

static void reset(uint32_t initial_flags = 0)
{
    source.clear(); drawn.clear(); observed_flags.clear(); query_flags.clear();
    query_status = 0; query_success = true;
    flags = initial_flags; sink_fn = nullptr; sink_ud = nullptr;
    flushes = warnings = 0;
}

int main()
{
    auto *world = reinterpret_cast<JcePhysicsWorld *>(uintptr_t(1));
    auto *renderer = reinterpret_cast<JceRenderer *>(uintptr_t(2));
    const jce_mat4 identity = jce_m4_identity();
    bool state = false;
    reset();
    check(!jce_editor_physics_overlay_draw(nullptr, identity, identity, 121,
          renderer, true, state), "missing world remains unavailable");
    check(!jce_editor_physics_overlay_draw(world, identity, identity, 121,
          nullptr, true, state), "missing renderer remains unavailable");
    check(observed_flags.empty() && flushes == 0, "unavailable does not draw authoring substitutes");

    reset(JCE_PHYS_DBG_WIREFRAME);
    check(jce_editor_physics_overlay_draw(world, identity, identity, 121,
          renderer, false, state), "live world with wireframe off is admitted");
    check(observed_flags.empty() && flags == JCE_PHYS_DBG_WIREFRAME,
          "view off wins without overwriting stored flags");

    reset(JCE_PHYS_DBG_CONTACTS | JCE_PHYS_DBG_AABB);
    source.push_back({jce_v3(-.2f, 0, 0), jce_v3(.2f, 0, 0), 7});
    check(jce_editor_physics_overlay_draw(world, identity, identity, 121,
          renderer, true, state), "actual world line path admitted");
    check(query_flags.size() == 1 && observed_flags.size() == 1, "one bounded shape query and one contacts-only legacy pass");
    check(query_flags[0] == (JCE_PHYS_DBG_WIREFRAME | JCE_PHYS_DBG_AABB), "shape flags stay in bounded source path");
    for (uint32_t mask : observed_flags)
        check(mask == JCE_PHYS_DBG_CONTACTS,
              "legacy flush cannot re-enter whole mesh traversal");
    check(flags == (JCE_PHYS_DBG_CONTACTS | JCE_PHYS_DBG_AABB), "public flags restored");
    check(drawn.size() == 1 && drawn[0].color == 7, "source line/color retained");
    check(sink_ud == nullptr && sink_fn == jce_editor_physics_overlay::renderer_sink,
          "no stack context retained in global sink");
    if (sink_ud == nullptr && sink_fn)
        sink_fn(jce_v3(0, 0, 0), jce_v3(1, 1, 1), 9, sink_ud);
    check(drawn.size() == 2 && drawn.back().color == 9, "later unscoped producer remains wired");

    reset();
    for (int axis = 0; axis != 3; ++axis) {
        for (int sign : {-1, 1}) {
            const jce_vec3 a = jce_v3(axis == 0 ? float(sign * 2) : 0,
                axis == 1 ? float(sign * 2) : 0, axis == 2 ? float(sign * 2) : 0);
            const jce_vec3 b = jce_v3_scale(a, 1.5f);
            source.push_back({a, b, 100});
        }
    }
    source.push_back({jce_v3(-2, 0, 0), jce_v3(2, 0, 0), 200});
    source.push_back({jce_v3(0, 0, 0), jce_v3(0, .5f, 0), 201});
    source.push_back({jce_v3(std::numeric_limits<float>::quiet_NaN(), 0, 0), jce_v3(0, 0, 0), 300});
    jce_editor_physics_overlay_draw(world, identity, identity, 121, renderer, true, state);
    check(drawn.size() == 2, "off-frustum and nonfinite segments consume no budget");
    check(drawn.size() == 2 && drawn[0].color == 200 && drawn[1].color == 201,
          "crossing edges survive even with two exterior endpoints");

    reset();
    source.push_back({jce_v3(9.5f, 0, 0), jce_v3(10.5f, 0, 0), 11});
    const jce_mat4 view = jce_m4_translate(jce_v3(-10, 0, 0));
    const jce_mat4 projection = jce_m4_scale(jce_v3(.5f, .5f, .5f));
    jce_editor_physics_overlay_draw(world, view, projection, 121, renderer, true, state);
    check(drawn.size() == 1, "projection times view uses actual camera pose");

    reset();
    for (uint32_t i = 0; i != 50000; ++i)
        source.push_back({jce_v3(20, 0, 0), jce_v3(21, 0, 0), i});
    source.push_back({jce_v3(0, 0, 0), jce_v3(.2f, 0, 0), 50000});
    source.push_back({jce_v3(0, 0, 0), jce_v3(.2f, 0, 0), 50001});
    jce_editor_physics_overlay_draw(world, identity, identity, 121, renderer, true, state);
    check(drawn.size() == 2 && drawn.back().color == 50001,
          "invisible early mesh cannot starve visible later bodies");

    reset();
    state = false;
    const uint32_t total = 100003;
    for (uint32_t i = 0; i != total; ++i)
        source.push_back({jce_v3(-.1f, 0, 0), jce_v3(.1f, 0, 0), i});
    jce_editor_physics_overlay_draw(world, identity, identity, 121, renderer, true, state);
    uint32_t stride = 1;
    while ((total + stride - 1) / stride > jce_editor_physics_overlay::line_budget) stride *= 2;
    check(drawn.size() <= jce_editor_physics_overlay::line_budget, "per-view line budget bounded");
    check(drawn.size() == (total + stride - 1) / stride, "distributed visible sample count");
    for (size_t i = 0; i != drawn.size(); ++i)
        check(drawn[i].color == i * stride, "sampling covers entire stream, not its prefix");
    check(drawn.back().color > 90000, "late mesh region remains represented");
    check(warnings == 1 && state, "incomplete high-density detail is diagnosed");
    jce_editor_physics_overlay_draw(world, identity, identity, 121, renderer, true, state);
    check(warnings == 1, "budget diagnostic is not a per-frame log flood");

    reset(JCE_PHYS_DBG_CONSTRAINTS | JCE_PHYS_DBG_CONTACTS); state = false;
    jce_editor_physics_overlay_draw(world, identity, identity, 121, renderer, false, state);
    check(query_flags.empty() && observed_flags.size() == 1 &&
          observed_flags[0] == (JCE_PHYS_DBG_CONSTRAINTS | JCE_PHYS_DBG_CONTACTS),
          "non-shape debug flags never request shape traversal");
    reset(); state = false; query_status = JCE_PHYS_QUERY_SAMPLED;
    jce_editor_physics_overlay_draw(world, identity, identity, 121, renderer, true, state);
    check(warnings == 1 && state, "native source sampling is explicitly diagnosed even with no visible lines");
    reset(); state = false; query_success = false;
    jce_editor_physics_overlay_draw(world, identity, identity, 121, renderer, true, state);
    check(warnings == 1 && drawn.empty(), "failed query does not fabricate component boxes");
    jce_vec3 lo, hi;
    check(jce_editor_physics_overlay::query_bounds(identity, lo, hi) &&
          lo.x == -1 && lo.y == -1 && lo.z == -1 && hi.x == 1 && hi.y == 1 && hi.z == 1,
          "native spatial query encloses view corners");

    reset(); state = false;
    source.push_back({jce_v3(29, 0, 0), jce_v3(29.5f, 0, 0), 42});
    JceCharacterHandle player{7};
    jce_editor_physics_overlay_draw(world, view, projection, 121, renderer, true, state,
        30.0f, false, player);
    check(last_query.range_radius == 30 && last_query.range_center.x == 10 &&
          last_query.range_center.y == 0 && last_query.range_center.z == 0,
          "actual view inverse supplies camera-centered runtime radius");
    check(last_query.keep_character.idx == 7 &&
          last_query.range_flags == JCE_PHYS_QUERY_KEEP_CHARACTER,
          "exact current player handle reaches native query unchanged");
    check(drawn.size() == 1, "disabled optional frustum does not hide source-admitted nearby geometry");
    reset();
    const jce_mat4 moved_view = jce_m4_translate(jce_v3(-500, 20, -3));
    jce_editor_physics_overlay_draw(world, moved_view, projection, 121, renderer, true, state,
        0.0f, false, JceCharacterHandle{UINT32_MAX});
    check(last_query.range_radius == 0 && last_query.range_center.x == 500 &&
          last_query.range_center.y == -20 && last_query.range_center.z == 3,
          "moved rendered camera replaces old query center and zero disables distance");
    check(last_query.range_flags == 0 && last_query.bounds_min.x < -1.e30f &&
          last_query.bounds_max.x > 1.e30f,
          "absent Player keeps nobody and no-frustum zero range has no distance box");

    namespace policy = jce_editor_physics_debug_settings;
    check(policy::radius() == 30 && policy::center() == policy::CurrentCamera &&
          !policy::frustum() && setting_writes == 0,
          "fresh per-user policy is 30m/current camera/no frustum without writes");
    policy::set_radius(0);
    check(policy::radius() == 0 && setting_writes == 1, "saved zero survives readback as unlimited distance");
    policy::set_radius(-1); check(policy::radius() == 0, "negative input clamps at zero");
    policy::set_radius(1.e8f); check(policy::radius() == 10000, "radius upper bound is 10000m");
    stored_radius = std::numeric_limits<float>::quiet_NaN();
    const unsigned writes_before_read = setting_writes;
    check(policy::radius() == 30 && setting_writes == writes_before_read,
          "invalid stored radius uses safe default without silently rewriting the preference");
    auto *runtime = reinterpret_cast<JceRuntime *>(uintptr_t(3));
    jce_vec3 selected_center{}; player_position = jce_v3(3,4,5); position_available = true;
    check(!policy::player_center(runtime, selected_center) && player_reads == 0,
          "camera mode does not read player position");
    policy::set_center(policy::Player);
    check(policy::player_center(runtime, selected_center) && selected_center.x == 3 &&
          selected_center.y == 4 && selected_center.z == 5,
          "player mode consumes actual runtime position endpoint");
    reset(); jce_editor_physics_overlay_draw(world, moved_view, projection, 121, renderer,
        true, state, 30, false, player, &selected_center);
    check(last_query.range_center.x == 3 && last_query.range_center.y == 4 &&
          last_query.range_center.z == 5 && last_query.keep_character.idx == 7,
          "player-centered radius remains independent of true retained capsule handle");
    position_available = false;
    const bool use_player = policy::player_center(runtime, selected_center);
    reset(); jce_editor_physics_overlay_draw(world, moved_view, projection, 121, renderer,
        true, state, 30, false, JceCharacterHandle{UINT32_MAX}, use_player ? &selected_center : nullptr);
    check(!use_player && last_query.range_center.x == 500 && last_query.range_center.y == -20,
          "missing player position falls back to actual current rendered camera");
    position_available = true; player_position.x = std::numeric_limits<float>::infinity();
    check(!policy::player_center(runtime, selected_center), "nonfinite player position also selects camera fallback");

    /* Actual renderer copy owner: simulate transient space reduced after the
     * view's own budget. Vertex pairs/attributes must remain intact. */
    struct Vertex { float x, y, z; uint32_t color; };
    struct Pair { Vertex a, b; };
    std::vector<Pair> queued(101), copied(101);
    for (uint32_t i = 0; i < queued.size(); ++i)
        queued[i] = {{float(i), 1, 2, i}, {float(i), 3, 4, i + 1000}};
    jce_debug_line_sample(copied.data(), queued.data(), 101, 101, sizeof(Pair));
    check(std::memcmp(copied.data(), queued.data(), queued.size() * sizeof(Pair)) == 0,
          "adequate transient capacity preserves every original endpoint and attribute");
    for (uint32_t capacity : {1u, 2u, 3u, 20u, 100u}) {
        copied.assign(102, Pair{});
        copied[capacity].a.color = 0xDEADBEEF;
        jce_debug_line_sample(copied.data(), queued.data(), 101, capacity, sizeof(Pair));
        check(copied[capacity].a.color == 0xDEADBEEF, "transient copy respects actual capacity");
        if (capacity == 1)
            check(copied[0].a.color == 50, "single available line takes middle of source");
        else
            check(copied[0].a.color == 0 && copied[capacity - 1].a.color == 100,
                  "reduced transient space still covers first and last body regions");
        for (uint32_t i = 0; i < capacity; ++i) {
            const uint32_t at = copied[i].a.color;
            check(at < queued.size() && std::memcmp(&copied[i], &queued[at], sizeof(Pair)) == 0,
                  "sampling preserves a complete original line, including both colors");
            if (i) check(at > copied[i - 1].a.color, "distributed lines are ordered and unique");
        }
    }
    copied[0].a.color = 77;
    jce_debug_line_sample(copied.data(), queued.data(), 101, 0, sizeof(Pair));
    check(copied[0].a.color == 77, "zero transient lines performs no write");
    std::printf("Physics overlay CPU: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
