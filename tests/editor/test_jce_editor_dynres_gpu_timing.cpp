/* Complete production policy plus the strict actual Scene getter/consume wire.
 * Typed public observations are CPU controls, not real GPU or frame-join proof. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#ifndef JCE_DYNRES_POLICY_HEADER
#define JCE_DYNRES_POLICY_HEADER "scene/jce_editor_dynres_policy.h"
#endif
#include JCE_DYNRES_POLICY_HEADER
#include <cstring>
#include <limits>

static JceGpuTimingObservation provided;
static bool getter_ok = true;
static unsigned getter_calls;

bool jce_renderer_get_gpu_timing_observation(JceGpuTimingObservation *out, size_t size)
{
    ++getter_calls;
    if (!getter_ok || !out || size < sizeof(*out)) return false;
    std::memcpy(out, &provided, sizeof(provided));
    return true;
}
static void production_poll(JceEditorDynresPolicy &s_dynres)
{
#include JCE_DYNRES_WIRE_HEADER
}
static void set_duration(double duration)
{
    provided.duration_ms = duration;
    provided.raw_end = provided.raw_begin + static_cast<int64_t>(duration * 1000.0);
}
static void reset(double duration = 32.0)
{
    provided = {};
    provided.struct_size = sizeof(provided); provided.version = 1;
    provided.context_available = provided.backend_available = provided.timer_available = true;
    provided.unavailable_reason = JCE_GPU_TIMING_AVAILABLE;
    provided.context_generation = 11; provided.gpu_source_frame = 7;
    provided.timer_frequency = 1000000; provided.raw_begin = 1000000;
    set_duration(duration);
    getter_ok = true; getter_calls = 0;
}
static void next_sample()
{
    ++provided.gpu_source_frame; ++provided.read_host_frame_index;
    provided.raw_begin += 1000000; provided.raw_end += 1000000;
}
TEST_CASE("actual Scene wire consumes the public bounded observation")
{
    reset(); JceEditorDynresPolicy state;
    production_poll(state);
    CHECK(getter_calls == 1); CHECK(state.has_sample);
    CHECK(state.context_generation == 11); CHECK(state.over == 1);
    CHECK(state.scale == doctest::Approx(1.0));
}
TEST_CASE("one latched sample never becomes three sustained GPU intervals")
{
    reset(); JceEditorDynresPolicy state;
    for (unsigned read = 0; read < 30; ++read) {
        provided.read_host_frame_index = read;
        production_poll(state);
    }
    CHECK(getter_calls == 30); CHECK(state.over == 1);
    CHECK(state.scale == doctest::Approx(1.0));
}
TEST_CASE("three distinct over-target samples shrink by the existing step")
{
    reset(); JceEditorDynresPolicy state;
    for (unsigned sample = 0; sample < 3; ++sample) { production_poll(state); next_sample(); }
    CHECK(state.over == 0); CHECK(state.under == 0);
    CHECK(state.scale == doctest::Approx(0.9));
}
TEST_CASE("twelve distinct low samples grow and repeated low samples hold")
{
    reset(5.0); JceEditorDynresPolicy state; state.scale = 0.8f;
    for (unsigned read = 0; read < 40; ++read) production_poll(state);
    CHECK(state.under == 1); CHECK(state.scale == doctest::Approx(0.8));
    for (unsigned sample = 1; sample < 12; ++sample) { next_sample(); production_poll(state); }
    CHECK(state.under == 0); CHECK(state.scale == doctest::Approx(0.9));
}
TEST_CASE("dead zone and tiny intervals do not preserve a load streak")
{
    reset(); JceEditorDynresPolicy state;
    production_poll(state); next_sample(); production_poll(state);
    next_sample(); set_duration(11); production_poll(state);
    CHECK(state.over == 0); CHECK(state.under == 0);
    next_sample(); set_duration(16); production_poll(state);
    CHECK(state.over == 0); CHECK(state.under == 0);
    next_sample(); set_duration(0.01); state.over = 2; production_poll(state);
    CHECK(state.over == 0); CHECK(state.scale == doctest::Approx(1.0));
}
TEST_CASE("invalid public observations clear streaks without inventing zero cost")
{
    for (unsigned invalid = 0; invalid < 12; ++invalid) {
        reset(); JceEditorDynresPolicy state; state.over = 2;
        switch (invalid) {
        case 0: getter_ok = false; break;
        case 1: provided.version = 2; break;
        case 2: --provided.struct_size; break;
        case 3: provided.context_available = false; break;
        case 4: provided.backend_available = false; break;
        case 5: provided.timer_available = false; break;
        case 6: provided.context_generation = 0; break;
        case 7: provided.timer_frequency = 0; break;
        case 8: provided.raw_end = provided.raw_begin; break;
        case 9: provided.raw_begin = -1; break;
        case 10: provided.duration_ms = std::numeric_limits<double>::quiet_NaN(); break;
        case 11: provided.unavailable_reason = JCE_GPU_TIMING_INTERVAL_UNAVAILABLE; break;
        }
        production_poll(state);
        CHECK(state.over == 0); CHECK(state.under == 0);
        CHECK(state.scale == doctest::Approx(1.0));
    }
}
TEST_CASE("unavailable read does not make the previous accepted tuple new again")
{
    reset(); JceEditorDynresPolicy state;
    production_poll(state); next_sample(); production_poll(state);
    getter_ok = false; production_poll(state);
    CHECK(state.over == 0);
    getter_ok = true; production_poll(state);
    CHECK(state.over == 0); CHECK(state.scale == doctest::Approx(1.0));
}
TEST_CASE("context replacement resets streaks and raw frame wrap is an identity")
{
    reset(); JceEditorDynresPolicy state;
    production_poll(state); next_sample(); production_poll(state);
    ++provided.context_generation; production_poll(state);
    CHECK(state.over == 1); CHECK(state.scale == doctest::Approx(1.0));
    ++provided.context_generation; provided.timer_frequency = 0; production_poll(state);
    CHECK(state.over == 0); CHECK_FALSE(state.has_sample);
    provided.timer_frequency = 1000000; production_poll(state);
    CHECK(state.over == 1);
    JceEditorDynresPolicy wrap;
    provided.gpu_source_frame = UINT32_MAX; production_poll(wrap);
    next_sample(); production_poll(wrap);
    CHECK(wrap.over == 2); CHECK(wrap.source_frame == 0);
}
TEST_CASE("scale endpoints and streak counters remain bounded")
{
    reset(); JceEditorDynresPolicy state;
    for (unsigned sample = 0; sample < 300; ++sample) { production_poll(state); next_sample(); }
    CHECK(state.scale == doctest::Approx(0.55)); CHECK(state.over == 3);
    set_duration(5);
    for (unsigned sample = 0; sample < 300; ++sample) { production_poll(state); next_sample(); }
    CHECK(state.scale == doctest::Approx(1.0)); CHECK(state.under == 12);
}
