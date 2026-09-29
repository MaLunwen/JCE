/* jce_script_api.hpp -- GENERATED. DO NOT EDIT.
 *
 *   python tools/scriptgen/gen_script_bindings.py --write
 *
 * A header-only C++ wrapper over the script-facing C ABI.
 *
 * THE C BINDING IS NOT HERE, BECAUSE IT ALREADY EXISTS.  <jce/script_api/
 * jce_script_api.h>, generated beside this file from the same manifest, IS the
 * C binding of the scripting surface: plain C99, an opaque handle, one
 * exported symbol per manifest entry.  A C consumer includes that header and
 * links jce_script_api, and needs nothing from this directory.  This file adds
 * only what C cannot express.
 *
 * WHAT IT ADDS, and nothing else:
 *   * RAII.  `Api` owns the `JceScriptApi *`, closes it in ~Api, and is
 *     move-only.  The handle is the only resource on this surface.
 *   * The seven closed shapes as C++ TYPES rather than out parameters:
 *     std::optional<T>, std::array<float,N>, std::vector<Entity>, and
 *     std::optional<std::string> for the owned-string shape -- which is the
 *     one that could leak, and now cannot, because no char * and no buffer
 *     size ever reach the caller.
 *   * The manifest's `optional` defaults, as C++ default arguments where C++
 *     allows them (they must be trailing) and as named constants in
 *     `jce::script::defaults` where it does not.
 *   * The manifest's `index_base` floor, restored.  This is the ONLY place the
 *     wrapper adds behaviour instead of sugar; see get_touch below.
 *
 * WHAT IT DOES NOT ADD:  the seven hand-written manifest entries, excluded as
 * a class exactly as the C ABI excludes them.  Their reasons are printed
 * below, verbatim from the manifest.  Reaching around them to a raw host
 * member is the sandbox escape the manifest calls P0-2, and a C++ convenience
 * wrapper is the most tempting place in the tree to do it.
 *
 * LANGUAGE LEVEL.  REQUIRES C++17 -- and requires it in the enforced sense:
 * tests/scripting/cpp/test_jce_script_cpp17_floor.cpp compiles this header at
 * -std=c++17 and runs it, so the claim fails a build rather than a code
 * review.  C++20 is USED, never required: when <span> is available the
 * entity-table entries gain an additional caller-buffer overload, guarded on
 * __cpp_lib_span.  An SDK consumer on C++17 loses that overload and nothing
 * else.
 *
 * ABSENT MEMBERS, and a DEFAULT-CONSTRUCTED Api.  Every entry point of the C
 * ABI already answers `!api` and `!api->host.<member>` with the absent value,
 * so calling any method on a closed or never-opened `Api` is defined and
 * returns exactly what the Lua binding pushes with no host: nullopt / 0 /
 * false / an empty array.  Nothing here needs to re-check the handle, and
 * [doctest] "an empty handle answers like an absent host" is what fails if
 * that stops being true.
 */
#ifndef JCE_SCRIPT_API_HPP
#define JCE_SCRIPT_API_HPP

#include <jce/script_api/jce_script_api.h>

/* MSVC reports 199711L in __cplusplus unless /Zc:__cplusplus is passed, and
 * reports the real level in _MSVC_LANG either way.  Reading only __cplusplus
 * would reject every default MSVC build of a perfectly conforming C++17
 * consumer. */
#if defined(_MSVC_LANG)
#  define JCE_SCRIPT_CPP_LANG _MSVC_LANG
#else
#  define JCE_SCRIPT_CPP_LANG __cplusplus
#endif

#if JCE_SCRIPT_CPP_LANG < 201703L
#  error "jce_script_api.hpp requires C++17 or later"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/* C++20's std::span, USED and not required.  __cpp_lib_span is the library
 * feature-test macro, which is the only thing that answers "can I include
 * <span>"; a bare __cplusplus >= 202002L test is a compile error on a toolchain
 * whose front end is C++20 and whose library is not. */
#if defined(__has_include)
#  if __has_include(<version>)
#    include <version>
#  endif
#endif
#if defined(__cpp_lib_span) && __cpp_lib_span >= 202002L
#  include <span>
#  define JCE_SCRIPT_CPP_HAS_SPAN 1
#else
#  define JCE_SCRIPT_CPP_HAS_SPAN 0
#endif

namespace jce {
namespace script {

/* The entity id as scripts see it. */
using Entity = JceScriptEntity;

/* raycast's POD result, unchanged -- restating it would be a second
 * declaration of an engine type, which this layer does not do. */
using RaycastHit = JceScriptRaycastHit;

/* A NUL-terminated string argument.
 *
 * Converts implicitly from `const char *` (including a literal) and from
 * `const std::string &` -- c_str() is already NUL-terminated, so neither
 * conversion copies.  It deliberately does NOT convert from std::string_view:
 * a view is not NUL-terminated, the C ABI needs a `const char *`, and the only
 * way to bridge that is to copy the bytes to add a NUL -- a heap allocation
 * per call, in on_update.  The omission is the point.
 *
 * A default-constructed CStr is a null pointer, which is what the Lua binding
 * passes for an omitted nil-able string argument (send_message, broadcast,
 * rpc_send). */
class CStr {
public:
    constexpr CStr() noexcept : p_(nullptr) {}
    constexpr CStr(const char *s) noexcept : p_(s) {}       /* NOLINT: implicit */
    CStr(const std::string &s) noexcept : p_(s.c_str()) {}  /* NOLINT: implicit */
    constexpr const char *c_str() const noexcept { return p_; }
    constexpr explicit operator bool() const noexcept { return p_ != nullptr; }
private:
    const char *p_;
};

/* The first_and_count shape.  Lua returns (first_or_nil, count); the same two
 * facts, with the "or nil" in the type instead of in a convention.  `count` is
 * the host's own answer and MAY exceed the manifest's out_capacity -- that is
 * the point of the shape: a scene director can reject duplicate authored names
 * it did not receive. */
struct FirstAndCount {
    std::optional<Entity> first;
    int                   count = 0;
};
/* get_touch: 4 out parameters, so the success value is a
 * struct.  FIELD ORDER IS THE DECLARATION ORDER OF THE HOST'S OWN OUT
 * PARAMETERS, which is also the order the Lua binding pushes them; the
 * differential compares the two orders slot by slot.
 */
struct GetTouchResult {
    std::uint64_t id = 0;
    float x = 0;
    float y = 0;
    float pressure = 0;
};

/* get_param: 3 out parameters, so the success value is a
 * struct.  FIELD ORDER IS THE DECLARATION ORDER OF THE HOST'S OWN OUT
 * PARAMETERS, which is also the order the Lua binding pushes them; the
 * differential compares the two orders slot by slot.
 */
struct GetParamResult {
    int out_kind = 0;
    double out_number = 0;
    Entity out_entity = 0;
};

/* The manifest's `optional` defaults.  Named because C++ default
 * arguments must be trailing and some of these are not -- and named
 * for ALL of them, so which ones those are can change without the
 * others moving. */
namespace defaults {
inline constexpr float spawn_x = 0.0f;
inline constexpr float spawn_y = 0.0f;
inline constexpr float spawn_z = 0.0f;
inline constexpr bool pause_paused = true;
inline constexpr float shake_camera_amount = 0.5f;
inline constexpr int gas_apply_op = 0;
inline constexpr float gas_apply_duration_seconds = 0.0f;
inline constexpr std::uint32_t raycast_filtered_layer_mask = 0;
inline constexpr bool raycast_filtered_hit_triggers = false;
inline constexpr std::uint32_t raycast_all_layer_mask = 0;
inline constexpr bool raycast_all_hit_triggers = false;
inline constexpr double send_message_number_arg = 0.0;
inline constexpr CStr send_message_str_arg = CStr();
inline constexpr double broadcast_number_arg = 0.0;
inline constexpr CStr broadcast_str_arg = CStr();
inline constexpr int rpc_send_target = 0;
inline constexpr CStr rpc_send_payload = CStr();
inline constexpr std::uint32_t overlap_sphere_layer_mask = 0;
inline constexpr std::uint32_t overlap_box_layer_mask = 0;
}  /* namespace defaults */

/* ------------------------------------------------------------------ *
 *  NOT wrapped: the manifest's hand-written entries.
 *
 *  Excluded as a CLASS, by the same rule the C ABI excludes them by,
 *  and for the same reason: three are Lua-VM machinery with no meaning
 *  outside a lua_State, and the two asset readers carry sandbox policy
 *  that lives in `static` functions inside jce_script.c.  A C++
 *  convenience wrapper is the most tempting place in this tree to
 *  "finish the job" by calling the raw read_file member instead --
 *  that is the escape the manifest itself calls P0-2.  The manifest's
 *  own reasons, verbatim:
 *
 *    line_set_points
 *        marshalling, not glue: the host takes a packed `const float *xyz,
 *        int count` and no generated shape reads a Lua array into one.
 *        Hand-written so the table walk, the JCE_LINE_MAX_POINTS clamp and
 *        the returned stored-count live in one place. Exists because the
 *        comp_set/JSON route is O(n^2): cJSON resolves each flat px/py/pz key
 *        by walking the object's child list (jce_scene_components_render.c
 *        parse_line_renderer)
 *    log
 *        no-host fallback: the LOG_INFO else-branch at jce_script.c:63 is the
 *        only one among the 78
 *    asset_read_text
 *        policy, not glue: script_virtual_asset_path_valid (:67, called
 *        :103), the 1 MiB JCE_SCRIPT_TEXT_ASSET_MAX_BYTES cap
 *        (jce_script.h:114), jce_free on every exit path. A generated
 *        read_file template is a sandbox escape (P0-2)
 *    asset_read_json
 *        all of asset_read_text (the same script_virtual_asset_path_valid at
 *        :220) plus a depth- and node-capped JSON walk, non-finite rejection,
 *        the json_null sentinel and distinct string error codes (P0-2)
 *    play_sound
 *        arity dispatch across two members: lua_gettop at :264 routes to
 *        play_sound_spatial (:287) or play_sound (:290); its own comment at
 *        :281 concedes top == 3 is ambiguous
 *    start_coroutine
 *        Lua VM machinery: lua_newthread / lua_xmove / luaL_ref / lua_resume;
 *        owns s->coros[]
 *    wait_seconds
 *        lua_yield into the same scheduler
 *    stop_coroutine
 *        scans s->coros[]
 * ------------------------------------------------------------------ */

/* The RAII handle.
 *
 * Move-only: two owners would close one handle twice.  A default-constructed
 * Api is EMPTY and every method on it is still callable -- see the header
 * banner.  There is no throwing constructor and no exception on this surface;
 * `open` reports failure by returning an empty Api, which `operator bool`
 * answers. */
class Api {
public:
    Api() noexcept = default;

    /* Bind to a host.
     *
     * `host_size` defaults to the CALLER's sizeof(JceScriptHost) -- and it is
     * the caller's precisely BECAUSE this wrapper is header-only.  The default
     * argument is evaluated in the consumer's translation unit, against the
     * consumer's own copy of the engine header.  A compiled wrapper would bake
     * ITS sizeof into the library and hand the C ABI a number the caller never
     * agreed to, defeating the min(caller, engine) copy that makes a short host
     * safe.  What fails if this becomes a fixed number is that the caller's
     * host_size stops being the number that reaches the C ABI -- the property
     * the C++ suite measures.  That suite is not part of this repository, so
     * it is named here and not cited: an unresolvable citation reads the same
     * whether the test is missing or renamed.
     *
     * Returns an empty Api when the host is null, the size is 0, or
     * `script_api_min` is newer than the loaded library. */
    static Api open(const JceScriptHost &host,
                    std::size_t host_size = sizeof(JceScriptHost),
                    std::uint32_t script_api_min = JCE_SCRIPT_API_VERSION) noexcept
    {
        return Api(jce_script_api_open(&host, host_size, script_api_min));
    }

    ~Api() { close(); }

    Api(Api &&other) noexcept : h_(other.h_) { other.h_ = nullptr; }

    Api &operator=(Api &&other) noexcept
    {
        if (this != &other) {
            close();
            h_ = other.h_;
            other.h_ = nullptr;
        }
        return *this;
    }

    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;

    void close() noexcept
    {
        if (h_ != nullptr) {
            jce_script_api_close(h_);
            h_ = nullptr;
        }
    }

    explicit operator bool() const noexcept { return h_ != nullptr; }

    /* The raw handle, for a caller that must reach an entry point this
     * wrapper does not name.  Ownership does not move. */
    JceScriptApi *get() const noexcept { return h_; }

    /* How many bytes of an owned string are taken on the stack before the
     * wrapper has to ask the host a second time.  Public because that second
     * ask is an OBSERVABLE second host call, and a caller tuning for a
     * frame budget is entitled to know where the edge is. */
    static constexpr int kOwnedStringInlineBytes = 256;

    /* jce.get_position -- shape: fallible_out, since 1
     * LOCAL position of `entity` -- its own translation, not composed up the
     * parent chain. Absent when the entity has no transform. Use
     * get_world_position for the composed pose. This doc said 'World-space'
     * until 2026-08-26; the implementation always returned the local TRS
     * (jce_scene_get_transform), and the wrong word was generated into all
     * five language SDKs.
     */
    [[nodiscard]] std::optional<std::array<float, 3>> get_position(Entity e) const noexcept
    {
        std::array<float, 3> v{};
        if (!jce_script_api_get_position(h_, e, v.data()))
            return std::nullopt;
        return v;
    }

    /* jce.set_position -- shape: void_call, since 1
     */
    void set_position(Entity e, float x, float y, float z) const noexcept
    {
        jce_script_api_set_position(h_, e, x, y, z);
    }

    /* jce.get_rotation -- shape: fallible_out, since 1
     */
    [[nodiscard]] std::optional<std::array<float, 3>> get_rotation(Entity e) const noexcept
    {
        std::array<float, 3> v{};
        if (!jce_script_api_get_rotation(h_, e, v.data()))
            return std::nullopt;
        return v;
    }

    /* jce.set_rotation -- shape: void_call, since 1
     */
    void set_rotation(Entity e, float x, float y, float z) const noexcept
    {
        jce_script_api_set_rotation(h_, e, x, y, z);
    }

    /* jce.get_scale -- shape: fallible_out, since 1
     */
    [[nodiscard]] std::optional<std::array<float, 3>> get_scale(Entity e) const noexcept
    {
        std::array<float, 3> v{};
        if (!jce_script_api_get_scale(h_, e, v.data()))
            return std::nullopt;
        return v;
    }

    /* jce.get_world_position -- shape: fallible_out, since 1
     * WORLD position of `entity`: its local TRS composed up the parent chain
     * (jce_scene_get_world_matrix). Absent when the entity has no transform.
     * Every parented rig -- arms, jaws, fingers, pads -- needs this rather
     * than get_position.
     */
    [[nodiscard]] std::optional<std::array<float, 3>> get_world_position(Entity e) const noexcept
    {
        std::array<float, 3> v{};
        if (!jce_script_api_get_world_position(h_, e, v.data()))
            return std::nullopt;
        return v;
    }

    /* jce.set_scale -- shape: void_call, since 1
     */
    void set_scale(Entity e, float x, float y, float z) const noexcept
    {
        jce_script_api_set_scale(h_, e, x, y, z);
    }

    /* jce.set_parent -- shape: value_return, since 1
     * The ONLY boolean argument on this surface that is type-checked; the
     * other five accept any truthy value. The luaL_checktype is emitted from
     * this entry's `strict` modifier -- no line citation, because the
     * hand-written body that carried it is gone.
     * test_strict_emits_a_type_check_only_for_the_named_parameter is what
     * fails if the emitter drops it.
     * The manifest marks preserve_world `strict` -- Lua type-checks it
     * because Lua cannot do so at compile time. Here the parameter IS a bool
     * and the compiler is the check, so nothing is emitted for it.
     */
    [[nodiscard]] bool set_parent(Entity child, Entity parent,
                                  bool preserve_world) const noexcept
    {
        return jce_script_api_set_parent(h_, child, parent, preserve_world);
    }

    /* jce.get_parent -- shape: value_return, since 1
     */
    [[nodiscard]] Entity get_parent(Entity child) const noexcept
    {
        return jce_script_api_get_parent(h_, child);
    }

    /* jce.is_key_down -- shape: value_return, since 1
     */
    [[nodiscard]] bool is_key_down(int keycode) const noexcept
    {
        return jce_script_api_is_key_down(h_, keycode);
    }

    /* jce.find_with_tag -- shape: value_return, since 1
     */
    [[nodiscard]] Entity find_with_tag(CStr tag) const noexcept
    {
        return jce_script_api_find_with_tag(h_, tag.c_str());
    }

    /* jce.destroy -- shape: void_call, since 1
     */
    void destroy(Entity e) const noexcept
    {
        jce_script_api_destroy(h_, e);
    }

    /* jce.spawn -- shape: value_return, since 1
     */
    [[nodiscard]] Entity spawn(CStr prefab_path, float x = defaults::spawn_x,
                               float y = defaults::spawn_y,
                               float z = defaults::spawn_z) const noexcept
    {
        return jce_script_api_spawn(h_, prefab_path.c_str(), x, y, z);
    }

    /* jce.move_axis -- shape: void_out_array, since 1
     */
    [[nodiscard]] std::array<float, 2> move_axis() const noexcept
    {
        std::array<float, 2> out{};
        jce_script_api_move_axis(h_, out.data());
        return out;
    }

    /* jce.jump_pressed -- shape: value_return, since 1
     * The C ABI supplies button=0 (manifest bind_args), so it is not a
     * parameter here either.
     */
    [[nodiscard]] bool jump_pressed() const noexcept
    {
        return jce_script_api_jump_pressed(h_);
    }

    /* jce.sprint -- shape: value_return, since 1
     * The C ABI supplies button=1 (manifest bind_args), so it is not a
     * parameter here either.
     */
    [[nodiscard]] bool sprint() const noexcept
    {
        return jce_script_api_sprint(h_);
    }

    /* jce.attack_pressed -- shape: value_return, since 1
     * The C ABI supplies button=2 (manifest bind_args), so it is not a
     * parameter here either.
     */
    [[nodiscard]] bool attack_pressed() const noexcept
    {
        return jce_script_api_attack_pressed(h_);
    }

    /* jce.set_time_scale -- shape: void_call, since 1
     */
    void set_time_scale(float scale) const noexcept
    {
        jce_script_api_set_time_scale(h_, scale);
    }

    /* jce.pause -- shape: void_call, since 1
     * jce.pause() with no argument pauses; jce.pause(false) resumes.
     */
    void pause(bool paused = defaults::pause_paused) const noexcept
    {
        jce_script_api_pause(h_, paused);
    }

    /* jce.shake_camera -- shape: void_call, since 1
     */
    void shake_camera(float amount = defaults::shake_camera_amount) const noexcept
    {
        jce_script_api_shake_camera(h_, amount);
    }

    /* jce.music_set_intensity -- shape: void_call, since 1
     */
    void music_set_intensity(float intensity) const noexcept
    {
        jce_script_api_music_set_intensity(h_, intensity);
    }

    /* jce.music_get_intensity -- shape: value_return, since 1
     */
    [[nodiscard]] float music_get_intensity() const noexcept
    {
        return jce_script_api_music_get_intensity(h_);
    }

    /* jce.music_request_transition -- shape: value_return, since 1
     * Absolute playhead time of the quantized switch; negative on miss or no
     * track, which is why the no-host value is -1 and not 0.
     * When the host member is absent this returns -1.0, not 0.
     */
    [[nodiscard]] float music_request_transition(int to_segment) const noexcept
    {
        return jce_script_api_music_request_transition(h_, to_segment);
    }

    /* jce.gas_activate -- shape: value_return, since 1
     */
    [[nodiscard]] bool gas_activate(Entity e, std::uint32_t ability_id) const noexcept
    {
        return jce_script_api_gas_activate(h_, e, ability_id);
    }

    /* jce.gas_get -- shape: fallible_out, since 1
     */
    [[nodiscard]] std::optional<float> gas_get(Entity e, CStr attr_name) const noexcept
    {
        float v{};
        if (!jce_script_api_gas_get(h_, e, attr_name.c_str(), &v))
            return std::nullopt;
        return v;
    }

    /* jce.gas_apply -- shape: value_return, since 1
     * `op` is optional in the manifest but is followed by a required
     * parameter, and a C++ default argument must be trailing. Pass
     * defaults::gas_apply_op for the manifest value.
     */
    [[nodiscard]] bool gas_apply(Entity e, CStr attr_name, int op,
                                 float magnitude,
                                 float duration_seconds = defaults::gas_apply_duration_seconds) const noexcept
    {
        return jce_script_api_gas_apply(h_, e, attr_name.c_str(), op, magnitude, duration_seconds);
    }

    /* jce.raycast -- shape: fallible_out, since 1
     * 8 values on a hit; a MISS pushes integer 0, not nil -- scripts branch
     * on `e == 0`.
     */
    [[nodiscard]] std::optional<RaycastHit> raycast(const std::array<float, 3> &origin,
                                                    const std::array<float, 3> &dir,
                                                    float max_dist) const noexcept
    {
        RaycastHit v{};
        if (!jce_script_api_raycast(h_, origin.data(), dir.data(), max_dist, &v))
            return std::nullopt;
        return v;
    }

    /* jce.raycast_filtered -- shape: fallible_out, since 1
     * Closest hit along the ray, honouring a layer mask and the trigger skip
     * -- 8 values on a hit; a MISS pushes integer 0, not nil, so scripts
     * branch on `e == 0`, the same as jce.raycast. layer_mask 0 means every
     * layer and hit_triggers defaults to false, so the common call stays
     * origin/dir/distance and the filter is what you add when you need it.
     * hit_triggers is separate from the mask because a trigger volume is not
     * a layer: collapsing them would make 'ignore triggers on layer 3'
     * inexpressible.
     */
    [[nodiscard]] std::optional<RaycastHit> raycast_filtered(const std::array<float, 3> &origin,
                                                             const std::array<float, 3> &dir,
                                                             float max_dist,
                                                             std::uint32_t layer_mask = defaults::raycast_filtered_layer_mask,
                                                             bool hit_triggers = defaults::raycast_filtered_hit_triggers) const noexcept
    {
        RaycastHit v{};
        if (!jce_script_api_raycast_filtered(h_, origin.data(), dir.data(), max_dist, layer_mask, hit_triggers, &v))
            return std::nullopt;
        return v;
    }

    /* jce.raycast_all -- shape: entity_table, since 1
     * Every entity the ray passes through, as one array sorted near to far.
     * layer_mask 0 means every layer; hit_triggers defaults to false. Returns
     * ENTITIES rather than full hit records because the eight-value hit does
     * not survive as an array shape across seven languages without inventing
     * a per-language container -- re-query a specific one with
     * jce.raycast_filtered when you need its point and normal.
     */
    [[nodiscard]] std::vector<Entity> raycast_all(const std::array<float, 3> &origin,
                                                  const std::array<float, 3> &dir,
                                                  float max_dist,
                                                  std::uint32_t layer_mask = defaults::raycast_all_layer_mask,
                                                  bool hit_triggers = defaults::raycast_all_hit_triggers) const
    {
        std::vector<Entity> out(static_cast<std::size_t>(256));
        int n = jce_script_api_raycast_all(h_, origin.data(), dir.data(), max_dist, layer_mask, hit_triggers, out.data(), 256);
        /* Clamped on BOTH ends, and the upper clamp is not
         * symmetry: the Lua binding does not have it, so a host
         * that returns more than it was given overruns a stack
         * array there and merely loses entries here.  Stated
         * rather than silently differing.
         */
        if (n < 0)
            n = 0;
        if (n > 256)
            n = 256;
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

#if JCE_SCRIPT_CPP_HAS_SPAN
    /* C++20 only.  The allocation-free form: the CALLER owns the
     * buffer and chooses its size, which is the C ABI's own shape.
     * The return value is the host's count and may exceed the span
     * -- ask again with a bigger one.
     */
    [[nodiscard]] int raycast_all(const std::array<float, 3> &origin,
                                  const std::array<float, 3> &dir,
                                  float max_dist, std::uint32_t layer_mask,
                                  bool hit_triggers, std::span<Entity> out) const noexcept
    {
        return jce_script_api_raycast_all(h_, origin.data(), dir.data(), max_dist, layer_mask, hit_triggers, out.data(), static_cast<int>(out.size()));
    }
#endif

    /* jce.apply_impulse -- shape: void_call, since 1
     */
    void apply_impulse(Entity e, float x, float y, float z) const noexcept
    {
        jce_script_api_apply_impulse(h_, e, x, y, z);
    }

    /* jce.set_velocity -- shape: void_call, since 1
     */
    void set_velocity(Entity e, float x, float y, float z) const noexcept
    {
        jce_script_api_set_velocity(h_, e, x, y, z);
    }

    /* jce.anim_set_float -- shape: void_call, since 1
     */
    void anim_set_float(Entity e, CStr name, float v) const noexcept
    {
        jce_script_api_anim_set_float(h_, e, name.c_str(), v);
    }

    /* jce.anim_set_int -- shape: void_call, since 1
     */
    void anim_set_int(Entity e, CStr name, int v) const noexcept
    {
        jce_script_api_anim_set_int(h_, e, name.c_str(), v);
    }

    /* jce.anim_set_bool -- shape: void_call, since 1
     */
    void anim_set_bool(Entity e, CStr name, bool v) const noexcept
    {
        jce_script_api_anim_set_bool(h_, e, name.c_str(), v);
    }

    /* jce.anim_set_trigger -- shape: void_call, since 1
     */
    void anim_set_trigger(Entity e, CStr name) const noexcept
    {
        jce_script_api_anim_set_trigger(h_, e, name.c_str());
    }

    /* jce.is_action_down -- shape: value_return, since 1
     */
    [[nodiscard]] bool is_action_down(CStr name) const noexcept
    {
        return jce_script_api_is_action_down(h_, name.c_str());
    }

    /* jce.is_action_pressed -- shape: value_return, since 1
     */
    [[nodiscard]] bool is_action_pressed(CStr name) const noexcept
    {
        return jce_script_api_is_action_pressed(h_, name.c_str());
    }

    /* jce.get_axis -- shape: value_return, since 1
     */
    [[nodiscard]] float get_axis(CStr name) const noexcept
    {
        return jce_script_api_get_axis(h_, name.c_str());
    }

    /* jce.get_pointer_delta -- shape: void_out_array, since 1
     */
    [[nodiscard]] std::array<float, 2> get_pointer_delta() const noexcept
    {
        std::array<float, 2> out{};
        jce_script_api_get_pointer_delta(h_, out.data());
        return out;
    }

    /* jce.get_pointer_wheel -- shape: value_return, since 1
     */
    [[nodiscard]] float get_pointer_wheel() const noexcept
    {
        return jce_script_api_get_pointer_wheel(h_);
    }

    /* jce.is_pointer_down -- shape: value_return, since 1
     */
    [[nodiscard]] bool is_pointer_down(int button) const noexcept
    {
        return jce_script_api_is_pointer_down(h_, button);
    }

    /* jce.get_touch_count -- shape: value_return, since 1
     * A host returning a negative count is clamped to 0 so `for i = 1,
     * jce.get_touch_count()` cannot underflow.
     * Clamped by the C ABI to a minimum of 0.
     */
    [[nodiscard]] int get_touch_count() const noexcept
    {
        return jce_script_api_get_touch_count(h_);
    }

    /* jce.get_touch -- shape: fallible_out, since 1
     * 1-based Lua index mapped to 0-based C; an index below 1 returns nil
     * without calling the host.
     * INDEX BASE 1: the Lua binding takes a 1-based index and answers nil for
     * anything below it WITHOUT calling the host. This wrapper is 0-based
     * like the C ABI, so the same refusal sits at 0 -- and it is added HERE,
     * because the C ABI passes the index straight through.
     */
    [[nodiscard]] std::optional<GetTouchResult> get_touch(int index) const noexcept
    {
        if (index < 0)
            return std::nullopt;
        GetTouchResult v;
        if (!jce_script_api_get_touch(h_, index, &v.id, &v.x, &v.y, &v.pressure))
            return std::nullopt;
        return v;
    }

    /* jce.tr -- shape: value_return, since 1
     * Passthrough is the contract, not a fallback: an unlocalized build shows
     * readable keys instead of blank UI.
     * Absent host, or a host that answers NULL: returns `key` itself. The C
     * entry point returns NULL in the second case; normalising it is this
     * wrapper's job, and the differential's MOCK_FAIL sweep is what fails if
     * it stops doing it.
     */
    [[nodiscard]] std::string tr(CStr key) const
    {
        const char *s = jce_script_api_tr(h_, key.c_str());
        if (s == nullptr) s = key.c_str();
        return s != nullptr ? std::string(s) : std::string();
    }

    /* jce.get_locale -- shape: value_return, since 1
     */
    [[nodiscard]] std::string get_locale() const
    {
        const char *s = jce_script_api_get_locale(h_);
        return s != nullptr ? std::string(s) : std::string();
    }

    /* jce.set_locale -- shape: void_call, since 1
     */
    void set_locale(CStr locale) const noexcept
    {
        jce_script_api_set_locale(h_, locale.c_str());
    }

    /* jce.get_velocity -- shape: fallible_out, since 1
     */
    [[nodiscard]] std::optional<std::array<float, 3>> get_velocity(Entity e) const noexcept
    {
        std::array<float, 3> v{};
        if (!jce_script_api_get_velocity(h_, e, v.data()))
            return std::nullopt;
        return v;
    }

    /* jce.vehicle_set_input -- shape: void_call, since 1
     */
    void vehicle_set_input(Entity e, float throttle, float brake, float steer) const noexcept
    {
        jce_script_api_vehicle_set_input(h_, e, throttle, brake, steer);
    }

    /* jce.vehicle_get_speed -- shape: value_return, since 1
     */
    [[nodiscard]] float vehicle_get_speed(Entity e) const noexcept
    {
        return jce_script_api_vehicle_get_speed(h_, e);
    }

    /* jce.get_move -- shape: void_out_array, since 1
     */
    [[nodiscard]] std::array<float, 3> get_move() const noexcept
    {
        std::array<float, 3> out{};
        jce_script_api_get_move(h_, out.data());
        return out;
    }

    /* jce.ui_get_slider -- shape: fallible_out, since 1
     */
    [[nodiscard]] std::optional<float> ui_get_slider(Entity e) const noexcept
    {
        float v{};
        if (!jce_script_api_ui_get_slider(h_, e, &v))
            return std::nullopt;
        return v;
    }

    /* jce.ui_set_slider -- shape: void_call, since 1
     */
    void ui_set_slider(Entity e, float v) const noexcept
    {
        jce_script_api_ui_set_slider(h_, e, v);
    }

    /* jce.ui_get_progress -- shape: fallible_out, since 1
     */
    [[nodiscard]] std::optional<float> ui_get_progress(Entity e) const noexcept
    {
        float v{};
        if (!jce_script_api_ui_get_progress(h_, e, &v))
            return std::nullopt;
        return v;
    }

    /* jce.ui_set_progress -- shape: void_call, since 1
     */
    void ui_set_progress(Entity e, float v) const noexcept
    {
        jce_script_api_ui_set_progress(h_, e, v);
    }

    /* jce.ui_get_toggle -- shape: fallible_out, since 1
     */
    [[nodiscard]] std::optional<bool> ui_get_toggle(Entity e) const noexcept
    {
        bool v{};
        if (!jce_script_api_ui_get_toggle(h_, e, &v))
            return std::nullopt;
        return v;
    }

    /* jce.ui_set_toggle -- shape: void_call, since 1
     */
    void ui_set_toggle(Entity e, bool v) const noexcept
    {
        jce_script_api_ui_set_toggle(h_, e, v);
    }

    /* jce.ui_set_text -- shape: void_call, since 1
     */
    void ui_set_text(Entity e, CStr txt) const noexcept
    {
        jce_script_api_ui_set_text(h_, e, txt.c_str());
    }

    /* jce.send_message -- shape: void_call, since 1
     */
    void send_message(Entity target, CStr msg,
                      double number_arg = defaults::send_message_number_arg,
                      CStr str_arg = defaults::send_message_str_arg) const noexcept
    {
        jce_script_api_send_message(h_, target, msg.c_str(), number_arg, str_arg.c_str());
    }

    /* jce.broadcast -- shape: void_call, since 1
     */
    void broadcast(CStr msg,
                   double number_arg = defaults::broadcast_number_arg,
                   CStr str_arg = defaults::broadcast_str_arg) const noexcept
    {
        jce_script_api_broadcast(h_, msg.c_str(), number_arg, str_arg.c_str());
    }

    /* jce.has_component -- shape: value_return, since 1
     */
    [[nodiscard]] bool has_component(Entity e, CStr comp_name) const noexcept
    {
        return jce_script_api_has_component(h_, e, comp_name.c_str());
    }

    /* jce.is_component_enabled -- shape: value_return, since 1
     */
    [[nodiscard]] bool is_component_enabled(Entity e, CStr comp_name) const noexcept
    {
        return jce_script_api_is_component_enabled(h_, e, comp_name.c_str());
    }

    /* jce.set_component_enabled -- shape: void_call, since 1
     */
    void set_component_enabled(Entity e, CStr comp_name, bool on) const noexcept
    {
        jce_script_api_set_component_enabled(h_, e, comp_name.c_str(), on);
    }

    /* jce.net_is_server -- shape: value_return, since 1
     */
    [[nodiscard]] bool net_is_server() const noexcept
    {
        return jce_script_api_net_is_server(h_);
    }

    /* jce.net_is_client -- shape: value_return, since 1
     */
    [[nodiscard]] bool net_is_client() const noexcept
    {
        return jce_script_api_net_is_client(h_);
    }

    /* jce.net_spawn -- shape: value_return, since 1
     */
    [[nodiscard]] Entity net_spawn(CStr prefab_path, float x, float y, float z) const noexcept
    {
        return jce_script_api_net_spawn(h_, prefab_path.c_str(), x, y, z);
    }

    /* jce.rpc_send -- shape: value_return, since 1
     */
    [[nodiscard]] bool rpc_send(Entity e, CStr event,
                                int target = defaults::rpc_send_target,
                                CStr payload = defaults::rpc_send_payload) const noexcept
    {
        return jce_script_api_rpc_send(h_, e, event.c_str(), target, payload.c_str());
    }

    /* jce.particle_burst -- shape: void_call, since 1
     */
    void particle_burst(Entity e, int count) const noexcept
    {
        jce_script_api_particle_burst(h_, e, count);
    }

    /* jce.particle_set_emitting -- shape: void_call, since 1
     */
    void particle_set_emitting(Entity e, bool on) const noexcept
    {
        jce_script_api_particle_set_emitting(h_, e, on);
    }

    /* jce.particle_set_color -- shape: void_call, since 1
     */
    void particle_set_color(Entity e, float r, float g, float b) const noexcept
    {
        jce_script_api_particle_set_color(h_, e, r, g, b);
    }

    /* jce.find_by_name -- shape: first_and_count, since 1
     * first-or-nil AND a match count, so a strict scene director can reject
     * duplicate authored names. IDENTICAL C signature to find_by_prefix and a
     * DIFFERENT contract.
     */
    [[nodiscard]] FirstAndCount find_by_name(CStr name) const noexcept
    {
        Entity found[2] = {};
        const int n = jce_script_api_find_by_name(h_, name.c_str(), found, 2);
        FirstAndCount r;
        r.count = n;
        if (n > 0)
            r.first = found[0];
        return r;
    }

    /* jce.find_by_prefix -- shape: entity_table, since 1
     * One Lua array. IDENTICAL C signature to find_by_name and a DIFFERENT
     * contract.
     */
    [[nodiscard]] std::vector<Entity> find_by_prefix(CStr prefix) const
    {
        std::vector<Entity> out(static_cast<std::size_t>(1024));
        int n = jce_script_api_find_by_prefix(h_, prefix.c_str(), out.data(), 1024);
        /* Clamped on BOTH ends, and the upper clamp is not
         * symmetry: the Lua binding does not have it, so a host
         * that returns more than it was given overruns a stack
         * array there and merely loses entries here.  Stated
         * rather than silently differing.
         */
        if (n < 0)
            n = 0;
        if (n > 1024)
            n = 1024;
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

#if JCE_SCRIPT_CPP_HAS_SPAN
    /* C++20 only.  The allocation-free form: the CALLER owns the
     * buffer and chooses its size, which is the C ABI's own shape.
     * The return value is the host's count and may exceed the span
     * -- ask again with a bigger one.
     */
    [[nodiscard]] int find_by_prefix(CStr prefix, std::span<Entity> out) const noexcept
    {
        return jce_script_api_find_by_prefix(h_, prefix.c_str(), out.data(), static_cast<int>(out.size()));
    }
#endif

    /* jce.comp_get -- shape: owned_string_release, since 1
     * Returns an owned copy, or nullopt when the host has no answer. Nothing
     * to free: the C ABI released the host's string through `json_free`
     * before returning. A result longer than kOwnedStringInlineBytes-1 costs
     * a SECOND call to the host -- the string was already released and there
     * is no other way to reach the tail.
     */
    [[nodiscard]] std::optional<std::string> comp_get(Entity e, CStr type) const
    {
        char inl[kOwnedStringInlineBytes];
        const int n = jce_script_api_comp_get(h_, e, type.c_str(), inl, static_cast<int>(sizeof inl));
        if (n < 0)
            return std::nullopt;
        if (n < kOwnedStringInlineBytes)
            return std::string(inl, static_cast<std::size_t>(n));
        /* The inline buffer was too small.  The C ABI copied what
         * fitted and RELEASED the host's string before returning,
         * so the tail is unreachable and the only way to get it is
         * to ask again.  That second ask is a second HOST CALL and
         * is visible in the differential's trace.  What that
         * second ask costs is measured by the C++ suite, which is
         * not part of this repository -- so the property is named
         * here rather than cited: an unresolvable name reads the
         * same whether the test is missing or renamed.
         */
        std::string big(static_cast<std::size_t>(n), '\0');
        int m = jce_script_api_comp_get(h_, e, type.c_str(), big.data(), n + 1);
        if (m < 0)
            return std::nullopt;
        if (m > n)
            m = n;   /* the host answered longer the second time */
        big.resize(static_cast<std::size_t>(m));
        return big;
    }

    /* jce.comp_set -- shape: value_return, since 1
     */
    [[nodiscard]] bool comp_set(Entity e, CStr type, CStr json) const noexcept
    {
        return jce_script_api_comp_set(h_, e, type.c_str(), json.c_str());
    }

    /* jce.render_get -- shape: owned_string_release, since 1
     * Returns an owned copy, or nullopt when the host has no answer. Nothing
     * to free: the C ABI released the host's string through `json_free`
     * before returning. A result longer than kOwnedStringInlineBytes-1 costs
     * a SECOND call to the host -- the string was already released and there
     * is no other way to reach the tail.
     */
    [[nodiscard]] std::optional<std::string> render_get() const
    {
        char inl[kOwnedStringInlineBytes];
        const int n = jce_script_api_render_get(h_, inl, static_cast<int>(sizeof inl));
        if (n < 0)
            return std::nullopt;
        if (n < kOwnedStringInlineBytes)
            return std::string(inl, static_cast<std::size_t>(n));
        /* The inline buffer was too small.  The C ABI copied what
         * fitted and RELEASED the host's string before returning,
         * so the tail is unreachable and the only way to get it is
         * to ask again.  That second ask is a second HOST CALL and
         * is visible in the differential's trace.  What that
         * second ask costs is measured by the C++ suite, which is
         * not part of this repository -- so the property is named
         * here rather than cited: an unresolvable name reads the
         * same whether the test is missing or renamed.
         */
        std::string big(static_cast<std::size_t>(n), '\0');
        int m = jce_script_api_render_get(h_, big.data(), n + 1);
        if (m < 0)
            return std::nullopt;
        if (m > n)
            m = n;   /* the host answered longer the second time */
        big.resize(static_cast<std::size_t>(m));
        return big;
    }

    /* jce.render_set -- shape: value_return, since 1
     */
    [[nodiscard]] bool render_set(CStr json) const noexcept
    {
        return jce_script_api_render_set(h_, json.c_str());
    }

    /* jce.audio_set_volume -- shape: void_call, since 1
     */
    void audio_set_volume(Entity e, float volume) const noexcept
    {
        jce_script_api_audio_set_volume(h_, e, volume);
    }

    /* jce.ui_get_dropdown -- shape: fallible_out, since 1
     * Selected option INDEX of `entity`'s UIDropdown. Absent when the entity
     * has no dropdown, so a script can tell 'no dropdown' from 'a dropdown
     * reading 0'. The index and not the label: branching on which option is
     * the common case, and a label would make it a string compare.
     */
    [[nodiscard]] std::optional<int> ui_get_dropdown(Entity e) const noexcept
    {
        int v{};
        if (!jce_script_api_ui_get_dropdown(h_, e, &v))
            return std::nullopt;
        return v;
    }

    /* jce.ui_set_dropdown -- shape: void_call, since 1
     * Select an option by INDEX. Clamped into [0, option_count-1] rather than
     * refused, the way ui_set_progress clamps and the way the scene loader
     * clamps: the draw already clamps, so storing outside the range would
     * make the component and the picture disagree.
     */
    void ui_set_dropdown(Entity e, int index) const noexcept
    {
        jce_script_api_ui_set_dropdown(h_, e, index);
    }

    /* jce.ui_get_input_text -- shape: value_return, since 1
     * Current text of `entity`'s UIInputField, or '' when it has none. The
     * string is the component's own buffer and is valid until the next
     * mutation of that entity -- the same contract tr() and get_locale()
     * carry; every binding copies it and none may store it.
     */
    [[nodiscard]] std::string ui_get_input_text(Entity e) const
    {
        const char *s = jce_script_api_ui_get_input_text(h_, e);
        return s != nullptr ? std::string(s) : std::string();
    }

    /* jce.ui_set_input_text -- shape: void_call, since 1
     * Replace the UIInputField's text. Truncated to the field's capacity and
     * to char_limit when one is set -- the same cap the canvas applies to
     * typed input, so a script write and a keystroke cannot disagree about
     * what the field holds. A truncation is logged rather than silent.
     */
    void ui_set_input_text(Entity e, CStr text) const noexcept
    {
        jce_script_api_ui_set_input_text(h_, e, text.c_str());
    }

    /* jce.ui_get_scroll -- shape: fallible_out, since 1
     * Scroll offset (x, y) of `entity`'s UIScrollView, in REFERENCE units --
     * what the component stores and what the wheel path clamps, not device
     * px. Absent when the entity has no scroll view.
     */
    [[nodiscard]] std::optional<std::array<float, 2>> ui_get_scroll(Entity e) const noexcept
    {
        std::array<float, 2> v{};
        if (!jce_script_api_ui_get_scroll(h_, e, v.data()))
            return std::nullopt;
        return v;
    }

    /* jce.ui_set_scroll -- shape: void_call, since 1
     * Set the scroll offset in reference units. A disabled axis is pinned to
     * 0 and each axis is clamped the way the wheel path clamps, so a script
     * cannot push the offset somewhere a wheel could not; the canvas
     * re-clamps against the resolved viewport on the next render.
     */
    void ui_set_scroll(Entity e, float x, float y) const noexcept
    {
        jce_script_api_ui_set_scroll(h_, e, x, y);
    }

    /* jce.world_get_hour -- shape: value_return, since 1
     * Live hour of day in [0, 24) -- what the sky is showing now, NOT the
     * authored tod_hour seed a scene starts from. Reading the seed would
     * return the level's start-of-day forever while the sky moved.
     */
    [[nodiscard]] float world_get_hour() const noexcept
    {
        return jce_script_api_world_get_hour(h_);
    }

    /* jce.world_set_hour -- shape: void_call, since 1
     * Move the live clock, wrapping into [0, 24). For 'sleep until dawn'. The
     * authored seed is untouched, so reloading the scene still starts where
     * the designer set it.
     */
    void world_set_hour(float hour) const noexcept
    {
        jce_script_api_world_set_hour(h_, hour);
    }

    /* jce.world_is_daytime -- shape: value_return, since 1
     * True while the sun is above the horizon. THE predicate for 'is it
     * night?' -- every key-light chooser in the engine is required to agree
     * on this one, so a script that rolled its own threshold would disagree
     * with the lighting it can see.
     */
    [[nodiscard]] bool world_is_daytime() const noexcept
    {
        return jce_script_api_world_is_daytime(h_);
    }

    /* jce.world_get_weather -- shape: value_return, since 1
     * Authored weather type: 0 clear, 1 rain, 2 snow.
     */
    [[nodiscard]] int world_get_weather() const noexcept
    {
        return jce_script_api_world_get_weather(h_);
    }

    /* jce.world_get_weather_intensity -- shape: value_return, since 1
     * Authored weather intensity in [0, 1].
     */
    [[nodiscard]] float world_get_weather_intensity() const noexcept
    {
        return jce_script_api_world_get_weather_intensity(h_);
    }

    /* jce.world_get_wind_speed -- shape: value_return, since 1
     * Instantaneous wind speed in m/s -- the sustained speed plus this
     * moment's gust. Do NOT key a cache on it: it changes every frame by
     * design. It is the same number the ocean spectrum and the vegetation
     * shader read, so a script cannot disagree with what is on screen.
     */
    [[nodiscard]] float world_get_wind_speed() const noexcept
    {
        return jce_script_api_world_get_wind_speed(h_);
    }

    /* jce.request_scene -- shape: value_return, since 1
     */
    [[nodiscard]] bool request_scene(CStr scene_path) const noexcept
    {
        return jce_script_api_request_scene(h_, scene_path.c_str());
    }

    /* jce.is_transitioning -- shape: value_return, since 1
     */
    [[nodiscard]] bool is_transitioning() const noexcept
    {
        return jce_script_api_is_transitioning(h_);
    }

    /* jce.audio_play -- shape: value_return, since 1
     */
    [[nodiscard]] bool audio_play(Entity e) const noexcept
    {
        return jce_script_api_audio_play(h_, e);
    }

    /* jce.audio_stop -- shape: value_return, since 1
     */
    [[nodiscard]] bool audio_stop(Entity e) const noexcept
    {
        return jce_script_api_audio_stop(h_, e);
    }

    /* jce.audio_is_playing -- shape: value_return, since 1
     */
    [[nodiscard]] bool audio_is_playing(Entity e) const noexcept
    {
        return jce_script_api_audio_is_playing(h_, e);
    }

    /* jce.save_game -- shape: value_return, since 1
     */
    [[nodiscard]] bool save_game(CStr path) const noexcept
    {
        return jce_script_api_save_game(h_, path.c_str());
    }

    /* jce.load_game -- shape: value_return, since 1
     */
    [[nodiscard]] bool load_game(CStr path) const noexcept
    {
        return jce_script_api_load_game(h_, path.c_str());
    }

    /* jce.overlap_sphere -- shape: entity_table, since 1
     * Entities whose collider overlaps the sphere, as one array. layer_mask 0
     * means all layers. Triggers are skipped. layer_mask is OPTIONAL:
     * omitting it means every layer, which is what an explosion or a pickup
     * check wants and keeps the common call to its coordinates and its size.
     */
    [[nodiscard]] std::vector<Entity> overlap_sphere(float x, float y, float z,
                                                     float radius,
                                                     std::uint32_t layer_mask = defaults::overlap_sphere_layer_mask) const
    {
        std::vector<Entity> out(static_cast<std::size_t>(256));
        int n = jce_script_api_overlap_sphere(h_, x, y, z, radius, layer_mask, out.data(), 256);
        /* Clamped on BOTH ends, and the upper clamp is not
         * symmetry: the Lua binding does not have it, so a host
         * that returns more than it was given overruns a stack
         * array there and merely loses entries here.  Stated
         * rather than silently differing.
         */
        if (n < 0)
            n = 0;
        if (n > 256)
            n = 256;
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

#if JCE_SCRIPT_CPP_HAS_SPAN
    /* C++20 only.  The allocation-free form: the CALLER owns the
     * buffer and chooses its size, which is the C ABI's own shape.
     * The return value is the host's count and may exceed the span
     * -- ask again with a bigger one.
     */
    [[nodiscard]] int overlap_sphere(float x, float y, float z, float radius,
                                     std::uint32_t layer_mask,
                                     std::span<Entity> out) const noexcept
    {
        return jce_script_api_overlap_sphere(h_, x, y, z, radius, layer_mask, out.data(), static_cast<int>(out.size()));
    }
#endif

    /* jce.overlap_box -- shape: entity_table, since 1
     * Entities whose collider overlaps the axis-aligned box (half-extents),
     * as one array. layer_mask 0 means all layers. layer_mask is OPTIONAL:
     * omitting it means every layer, which is what an explosion or a pickup
     * check wants and keeps the common call to its coordinates and its size.
     */
    [[nodiscard]] std::vector<Entity> overlap_box(float x, float y, float z,
                                                  float hx, float hy, float hz,
                                                  std::uint32_t layer_mask = defaults::overlap_box_layer_mask) const
    {
        std::vector<Entity> out(static_cast<std::size_t>(256));
        int n = jce_script_api_overlap_box(h_, x, y, z, hx, hy, hz, layer_mask, out.data(), 256);
        /* Clamped on BOTH ends, and the upper clamp is not
         * symmetry: the Lua binding does not have it, so a host
         * that returns more than it was given overruns a stack
         * array there and merely loses entries here.  Stated
         * rather than silently differing.
         */
        if (n < 0)
            n = 0;
        if (n > 256)
            n = 256;
        out.resize(static_cast<std::size_t>(n));
        return out;
    }

#if JCE_SCRIPT_CPP_HAS_SPAN
    /* C++20 only.  The allocation-free form: the CALLER owns the
     * buffer and chooses its size, which is the C ABI's own shape.
     * The return value is the host's count and may exceed the span
     * -- ask again with a bigger one.
     */
    [[nodiscard]] int overlap_box(float x, float y, float z, float hx,
                                  float hy, float hz, std::uint32_t layer_mask,
                                  std::span<Entity> out) const noexcept
    {
        return jce_script_api_overlap_box(h_, x, y, z, hx, hy, hz, layer_mask, out.data(), static_cast<int>(out.size()));
    }
#endif

    /* jce.get_param -- shape: fallible_out, since 1
     * Returns kind, number, entity for an AUTHORED script parameter --
     * Unity's [SerializeField], Godot's @export. Returns nil when the entity
     * has no script component, when no parameter of that name is authored, or
     * when the name is empty: three absences a script cannot act differently
     * on, so `jce.get_param(e, 'speed') or 3.0` reads the way an author
     * expects.
     */
    [[nodiscard]] std::optional<GetParamResult> get_param(Entity e, CStr name) const noexcept
    {
        GetParamResult v;
        if (!jce_script_api_get_param(h_, e, name.c_str(), &v.out_kind, &v.out_number, &v.out_entity))
            return std::nullopt;
        return v;
    }

    /* jce.get_param_text -- shape: value_return, since 1
     * The TEXT value of an authored script parameter, or '' when the entity
     * has no script component, no parameter of that name, or one that is not
     * text. Empty rather than nil for the same reason ui_get_input_text is
     * empty: a script comparing strings should not have to test for nil
     * first. The string is the component's own buffer -- copy it if you keep
     * it.
     */
    [[nodiscard]] std::string get_param_text(Entity e, CStr name) const
    {
        const char *s = jce_script_api_get_param_text(h_, e, name.c_str());
        return s != nullptr ? std::string(s) : std::string();
    }

    /* jce.curve_eval -- shape: fallible_out, since 1
     * Sample an AUTHORED curve -- the documents the editor's Curve Editor
     * writes, which nothing could read until this binding existed. Unity's
     * AnimationCurve shape: the curve is a designer-authored function and the
     * script decides what it means, so the engine never has to invent what a
     * curve DRIVES. Returns nil when the path does not resolve, the document
     * does not parse, the named channel is absent, or that channel has no
     * keys -- so a curve that genuinely evaluates to 0 and a curve that is
     * not there are never one reading, and `jce.curve_eval(p, 'kick', t) or
     * 0.0` reads the way an author expects. An empty channel name means the
     * FIRST channel, which is a different request from a name that is not
     * there. The parsed curve is cached per runtime, so a call inside
     * on_update costs a name compare, not a JSON parse.
     */
    [[nodiscard]] std::optional<double> curve_eval(CStr path, CStr channel,
                                                   double t) const noexcept
    {
        double v{};
        if (!jce_script_api_curve_eval(h_, path.c_str(), channel.c_str(), t, &v))
            return std::nullopt;
        return v;
    }

    /* jce.vcam_activate -- shape: value_return, since 1
     * Cut to the virtual camera with this name, ahead of priority. Returns 1
     * when the name resolves to a camera that is active and enabled, 0
     * otherwise -- the request is recorded either way, so naming a camera in
     * a streaming cell that has not loaded yet does not silently become
     * 'whatever priority says'. Pass an empty string to clear it and hand the
     * decision back to priority. It does NOT rewrite the authored components:
     * the override lives in the vcam system, so a cutscene cannot bake its
     * camera choice into the level file.
     */
    [[nodiscard]] int vcam_activate(CStr name) const noexcept
    {
        return jce_script_api_vcam_activate(h_, name.c_str());
    }

private:
    explicit Api(JceScriptApi *h) noexcept : h_(h) {}

    JceScriptApi *h_ = nullptr;
};

}  /* namespace script */
}  /* namespace jce */

#endif /* JCE_SCRIPT_API_HPP */
