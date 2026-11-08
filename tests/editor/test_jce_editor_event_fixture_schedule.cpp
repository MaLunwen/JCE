#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_game_input_bridge.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_timer.h>
#include <cstring>
#include <limits>
#include <string>

namespace {
uint64_t counter = 1000000, frequency = 6000000;
unsigned counter_reads = 0, frequency_reads = 0, marker_reads = 0, key_events = 0;
uint64_t fixture_counter() { ++counter_reads; return counter; }
uint64_t fixture_frequency() { ++frequency_reads; return frequency; }
bool fixture_file(const char *path) { ++marker_reads; return jce_fs_host_exists_file(path); }
bool fixture_directory(const char *path) { ++marker_reads; return jce_fs_host_exists_dir(path); }
bool key_event(const JceEvent *event) {
    CHECK(event->key.scancode == JCE_KEY_F12); ++key_events; return true;
}
}
#define jce_time_perf_counter fixture_counter
#define jce_time_perf_freq fixture_frequency
#define jce_fs_host_exists_file fixture_file
#define jce_fs_host_exists_dir fixture_directory
#include JCE_EDITOR_EVENT_FIXTURE_SCHEDULE_OWNER
#undef jce_time_perf_counter
#undef jce_time_perf_freq
#undef jce_fs_host_exists_file
#undef jce_fs_host_exists_dir

namespace {
JceEditorEventFixture g_game_events;
JceRuntime *main_runtime = nullptr;
constexpr int JCE_PLAY_PLAYING = 1;
int main_play_state = JCE_PLAY_PLAYING;
int jce_state_get_play_state() { return main_play_state; }
JceRuntime *jce_editor_play_get_runtime() { return main_runtime; }
bool jce_editor_process_event(const JceEvent *event) { return key_event(event); }
#include JCE_EDITOR_EVENT_FIXTURE_MAIN_OWNER
#include JCE_EDITOR_EVENT_FIXTURE_PRE_COMMIT_OWNER

struct Fixture {
    JceEditorEventFixture events;
    JceEditorGameInputBridge bridge{};
    JceEditorGameInputViewport view{};
    std::string path, marker;
    const void *runtime = this;
    Fixture(const char *rows) {
        counter = 1000000; frequency = 6000000;
        counter_reads = frequency_reads = marker_reads = key_events = 0;
        char cwd[4096]{}; REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
        path = std::string(cwd) + "/events.txt";
        marker = std::string(cwd) + "/ready.json";
        REQUIRE(jce_fs_host_remove_file(marker.c_str()));
        REQUIRE(jce_fs_host_write_all(path.c_str(), rows, std::strlen(rows)));
        view.x = 100; view.y = 50; view.width = 640; view.height = 360;
        view.render_width = 1280; view.render_height = 1080;
        view.visible = true; view.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
        publish();
    }
    ~Fixture() {
        events.end(&bridge);
        jce_fs_host_remove_file(path.c_str());
        jce_fs_host_remove_recursive(marker.c_str());
    }
    void publish() { jce_editor_game_input_bridge_publish(&bridge, &view); }
    void tick(bool playing = true) { events.update(&bridge, playing, runtime, key_event); }
    void ready() {
        const char content[] = "{\"wpr\":\"ready\",\"nonce\":42}";
        REQUIRE(jce_fs_host_write_all_atomic(marker.c_str(), content, sizeof(content)-1));
    }
};
struct Commit {
    float dx = 0, dy = 0, wheel = 0, width = 0, height = 0;
    uint32_t buttons = 0;
    unsigned calls = 0, steps = 0;
    static void pointer(void *ud, float x, float y, float w, uint32_t b) {
        auto &p = *static_cast<Commit *>(ud);
        CHECK(p.calls++ == 0); p.dx = x; p.dy = y; p.wheel = w; p.buttons = b;
    }
    static void position(void *ud, float, float, float w, float h) {
        auto &p = *static_cast<Commit *>(ud); CHECK(p.calls++ == 1); p.width = w; p.height = h;
    }
    static void touches(void *ud, const JceRuntimeTouch *, int) {
        CHECK(static_cast<Commit *>(ud)->calls++ == 2);
    }
    static void actions(void *ud, const JceInputActions *) {
        CHECK(static_cast<Commit *>(ud)->calls++ == 3);
    }
    static void step(void *ud, float) {
        auto &p = *static_cast<Commit *>(ud); CHECK(p.calls++ == 4); ++p.steps;
    }
    void consume(JceEditorGameInputBridge &bridge) {
        calls = 0;
        const JceEditorGameInputCommitOps ops = {pointer, touches, actions, step, position};
        REQUIRE(jce_editor_game_input_bridge_commit(&bridge, nullptr, 0, nullptr, 1.0f/60, &ops, this));
        CHECK(calls == 5);
    }
};
}

TEST_CASE("elapsed rows reach complete bridge pre-step commit independently at 60 and 600 Hz") {
    for (unsigned hz : {60u, 600u}) {
        CAPTURE(hz);
        Fixture f("0 B .5 .5 1 1\n100 M .6 .6 .1 -.1\n200 W 0 0 2 0\n300 B .6 .6 1 0\n");
        f.events.begin(f.path.c_str(), "elapsed_ms", nullptr);
        Commit p;
        const uint64_t base = counter;
        double total_dx = 0, total_dy = 0, total_wheel = 0;
        for (unsigned i = 0; i <= hz * 4 / 10; ++i) {
            counter = base + i * frequency / hz;
            f.tick(); p.consume(f.bridge);
            total_dx += p.dx; total_dy += p.dy; total_wheel += p.wheel;
            const uint64_t ms = (counter - base) * 1000 / frequency;
            CHECK(p.buttons == (ms < 300 ? 1u : 0u));
            if (p.dx != 0) { CHECK(ms == 100); CHECK(p.dx == 128); CHECK(p.dy == -108); }
            if (p.wheel != 0) CHECK(ms == 200);
        }
        CHECK(total_dx == 128); CHECK(total_dy == -108); CHECK(total_wheel == 2);
        CHECK(marker_reads == 0); CHECK(frequency_reads == 1);
        CHECK_FALSE(f.bridge.viewport.focused); CHECK_FALSE(f.bridge.viewport.hovered);
        CHECK_FALSE(f.bridge.viewport.captured);
    }
}

TEST_CASE("late arbitrary JSON marker gates docked Play then Max and polls at most ten Hz") {
    Fixture f("0 B .5 .5 1 1\n300 B .5 .5 1 0\n");
    f.events.begin(f.path.c_str(), "elapsed_ms", f.marker.c_str());
    f.tick(); CHECK_FALSE(f.bridge.virtual_pointer.active);
    const unsigned first_reads = marker_reads;
    for (unsigned i = 1; i < 600; ++i) { counter += 1; f.tick(); }
    CHECK(marker_reads == first_reads);
    for (unsigned i = 0; i < 600; ++i) { counter += frequency / 600; f.tick(); }
    CHECK(marker_reads <= first_reads + 20);
    f.view.width = 1920; f.view.height = 1080; f.publish(); // Actual Max publication before readiness.
    f.runtime = &f.bridge;
    f.tick(); CHECK_FALSE(f.bridge.virtual_pointer.active);
    f.ready(); counter += frequency / 10;
    f.tick(); REQUIRE(f.events.active); CHECK(f.events.geometry.width == 1920);
    CHECK(f.events.runtime_owner == f.runtime);
    Commit p; p.consume(f.bridge); CHECK(p.buttons == 1);
    counter += frequency * 3 / 10; f.tick(); p.consume(f.bridge); CHECK(p.buttons == 0);
    CHECK_FALSE(f.events.data); CHECK(f.events.end_pending);
    f.tick(); CHECK_FALSE(f.bridge.virtual_pointer.active);
}

TEST_CASE("legacy frame scheduling snapshots configuration and performs no clock or marker work") {
    for (const char *clock : {static_cast<const char *>(nullptr), "", "frame"}) {
        Fixture f("2 K 0 0 69 1\n3 B .5 .5 1 1\n");
        f.events.begin(f.path.c_str(), clock, nullptr);
        for (int i = 0; i < 30; ++i) f.tick(false);
        f.view.visible = false; f.publish(); f.tick();
        f.view.visible = true; f.publish();
        f.tick(); CHECK(key_events == 0); CHECK(f.events.frame == 1);
        f.tick(); CHECK(key_events == 1); CHECK_FALSE(f.bridge.virtual_pointer.active);
        f.tick(); CHECK(f.bridge.virtual_pointer.buttons == 1); CHECK(f.events.end_pending);
        Commit p; p.consume(f.bridge); CHECK(p.buttons == 1);
        f.tick(); p.consume(f.bridge); CHECK(p.buttons == 0);
        CHECK(counter_reads == 0); CHECK(frequency_reads == 0); CHECK(marker_reads == 0);
    }
}

TEST_CASE("stale early directory relative and unsupported marker configurations refuse only fixture") {
    for (unsigned mode = 0; mode < 7; ++mode) {
        CAPTURE(mode);
        Fixture f("0 B .5 .5 1 1\n");
        if (mode == 0) f.ready();
        if (mode == 1) REQUIRE(jce_fs_host_create_directory(f.marker.c_str()));
        const char *clock = mode == 4 ? "bad" : mode == 5 ? "frame" : "elapsed_ms";
        const char *marker = mode == 2 ? "relative.json" : f.marker.c_str();
        if (mode == 6) frequency = 0;
        f.events.begin(f.path.c_str(), clock, marker);
        if (mode == 3) { f.ready(); f.tick(); } // Appeared before the first admitted Play observation.
        CHECK_FALSE(f.events.data); CHECK_FALSE(f.bridge.virtual_pointer.active);
    }
}

TEST_CASE("marker appearance while hidden refuses on restore and fresh host reinitializes") {
    Fixture f("0 B .5 .5 1 1\n");
    f.events.begin(f.path.c_str(), "elapsed_ms", f.marker.c_str());
    f.tick(); f.view.visible = false; f.publish(); f.tick(); f.ready();
    f.view.visible = true; f.publish(); counter += frequency / 10; f.tick();
    CHECK_FALSE(f.events.data); CHECK_FALSE(f.bridge.virtual_pointer.active);
    REQUIRE(jce_fs_host_remove_file(f.marker.c_str()));
    f.events.begin(f.path.c_str(), "elapsed_ms", f.marker.c_str()); f.tick();
    f.ready(); counter += frequency / 10; f.tick();
    CHECK(f.bridge.virtual_pointer.buttons == 1);
}

TEST_CASE("rapid hide and restore cannot bypass the waiting marker poll budget") {
    Fixture f("0 B .5 .5 1 1\n");
    f.events.begin(f.path.c_str(), "elapsed_ms", f.marker.c_str()); f.tick();
    const unsigned baseline = marker_reads;
    for (unsigned i = 0; i < 600; ++i) {
        f.view.visible = (i % 2) != 0; f.publish();
        counter += frequency / 600; f.tick();
    }
    CHECK(marker_reads <= baseline + 20);
    CHECK_FALSE(f.bridge.virtual_pointer.active); CHECK(f.events.data != nullptr);
}

TEST_CASE("elapsed activation pins runtime and geometry then retires on every admission loss") {
    for (unsigned mode = 0; mode < 13; ++mode) {
        CAPTURE(mode);
        Fixture f("0 B .5 .5 1 1\n1000 M .6 .6 .1 .1\n");
        f.events.begin(f.path.c_str(), "elapsed_ms", nullptr); f.tick();
        REQUIRE(f.bridge.virtual_pointer.active);
        if (mode == 0) f.view.visible = false;
        if (mode == 1) f.view.owner = JCE_EDITOR_GAME_INPUT_FREE_FLY;
        if (mode == 2) f.view.x += 1;
        if (mode == 3) f.view.y += 1;
        if (mode == 4) f.view.width += 1;
        if (mode == 5) f.view.height += 1;
        if (mode == 6) f.view.render_width += 1;
        if (mode == 7) f.view.render_height += 1;
        if (mode == 8) f.runtime = &f.bridge;
        if (mode == 9) f.runtime = nullptr;
        if (mode == 12) f.view.render_width = std::numeric_limits<float>::infinity();
        f.publish(); f.tick(mode != 10 && mode != 11); // Pause and Stop both cease Playing.
        CHECK_FALSE(f.events.data); CHECK_FALSE(f.bridge.virtual_pointer.active);
        f.view.visible = true; f.view.owner = JCE_EDITOR_GAME_INPUT_RUNTIME; f.publish();
        counter += frequency * 2; f.tick(); CHECK_FALSE(f.bridge.virtual_pointer.active);
    }
}

TEST_CASE("equal monotonic reads preserve held state while rollback cancels active and waiting") {
    for (bool gated : {false, true}) {
        Fixture f("0 B .5 .5 1 1\n1000 B .5 .5 1 0\n");
        f.events.begin(f.path.c_str(), "elapsed_ms", gated ? f.marker.c_str() : nullptr);
        f.tick(); const unsigned initial = marker_reads;
        for (unsigned i = 0; i < 50; ++i) f.tick();
        CHECK(marker_reads == initial);
        CHECK(f.bridge.virtual_pointer.buttons == (gated ? 0u : 1u));
        --counter; f.tick(); CHECK_FALSE(f.events.data); CHECK_FALSE(f.bridge.virtual_pointer.active);
    }
}

TEST_CASE("EOF holds final row for commit then clears and malformed rows retain bounded refusal") {
    Fixture f("0 B .5 .5 1 1\n");
    f.events.begin(f.path.c_str(), "elapsed_ms", nullptr); f.tick();
    REQUIRE(f.events.end_pending); Commit p; p.consume(f.bridge); CHECK(p.buttons == 1);
    f.tick(); p.consume(f.bridge); CHECK(p.buttons == 0); CHECK_FALSE(f.bridge.virtual_pointer.active);
    const char bad[] = "nan M .5 .5 .1 0\n";
    REQUIRE(jce_fs_host_write_all(f.path.c_str(), bad, sizeof(bad)-1));
    f.events.begin(f.path.c_str(), "elapsed_ms", nullptr); f.tick(); CHECK_FALSE(f.events.data);
    f.events.end(&f.bridge); CHECK_FALSE(f.events.end_pending);
}

TEST_CASE("complete actual main dispatch uses the shared bridge and ordinary key destination") {
    Fixture f("0 B .5 .5 1 1\n0 K 0 0 69 1\n");
    auto *shared = jce_editor_game_input_bridge_shared();
    jce_editor_game_input_bridge_publish(shared, &f.view);
    main_runtime = reinterpret_cast<JceRuntime *>(&f);
    g_game_events.begin(f.path.c_str(), "elapsed_ms", nullptr);
    editor_game_event_fixture();
    Commit p; p.consume(*shared); CHECK(p.buttons == 1); CHECK(key_events == 1);
    editor_game_event_fixture(); p.consume(*shared); CHECK(p.buttons == 0);
    g_game_events.end(shared); jce_editor_game_input_bridge_reset(shared);
    main_runtime = nullptr;
}

TEST_CASE("active admission validation clears held virtual input before a new runtime commit") {
    Fixture f("0 B .5 .5 1 1\n1000 B .5 .5 1 0\n");
    f.events.begin(f.path.c_str(), "elapsed_ms", nullptr); f.tick();
    REQUIRE(f.bridge.virtual_pointer.buttons == 1);
    f.events.validate_active(&f.bridge, true, &f.bridge);
    Commit p; p.consume(f.bridge); CHECK(p.buttons == 0); CHECK_FALSE(f.events.data);
}

TEST_CASE("launch options path and capped input bytes are owned snapshots") {
    Fixture f("0 B .5 .5 1 1\n");
    char clock[] = "elapsed_ms";
    std::string borrowed = f.marker;
    f.events.begin(f.path.c_str(), clock, borrowed.c_str());
    clock[0] = 'X'; borrowed.assign("wrong marker");
    const char replacement[] = "0 B .5 .5 1 0\n";
    REQUIRE(jce_fs_host_write_all(f.path.c_str(), replacement, sizeof(replacement)-1));
    f.tick(); f.ready(); counter += frequency / 10; f.tick();
    CHECK(f.bridge.virtual_pointer.buttons == 1);
    f.events.end(&f.bridge);
    const std::string large(1048577, ' ');
    REQUIRE(jce_fs_host_write_all(f.path.c_str(), large.data(), large.size()));
    f.events.begin(f.path.c_str(), "elapsed_ms", nullptr); CHECK_FALSE(f.events.data);
    const unsigned reads = counter_reads;
    for (unsigned i = 0; i < 1000; ++i) f.tick();
    CHECK(counter_reads == reads); CHECK_FALSE(f.bridge.virtual_pointer.active);
}


TEST_CASE("terminal held row retains exactly one same-owner commit then retires") {
    for (const char *tail : {"", "invalid row\n", "0 X .5 .5 0 0\n"}) {
        CAPTURE(tail);
        const std::string rows = std::string("0 B .5 .5 1 1\n") + tail;
        Fixture f(rows.c_str());
        f.events.begin(f.path.c_str(), "elapsed_ms", nullptr); f.tick();
        REQUIRE(f.events.end_pending); REQUIRE_FALSE(f.events.data);
        f.events.validate_active(&f.bridge, true, f.runtime);
        Commit p; p.consume(f.bridge); CHECK(p.buttons == 1);
        f.tick(); p.consume(f.bridge); CHECK(p.buttons == 0);
        CHECK_FALSE(f.events.end_pending); CHECK_FALSE(f.events.active);
        const unsigned reads = counter_reads;
        for (unsigned i = 0; i < 20; ++i) {
            f.events.validate_active(&f.bridge, false, nullptr); f.tick();
        }
        CHECK(counter_reads == reads); CHECK_FALSE(f.bridge.virtual_pointer.active);
    }
}

TEST_CASE("actual pre-commit guard rejects terminal held rows after every admission loss") {
    for (const char *tail : {"", "invalid row\n", "0 X .5 .5 0 0\n"}) {
        for (unsigned mode = 0; mode < 13; ++mode) {
            CAPTURE(tail); CAPTURE(mode);
            const std::string rows = std::string("0 B .5 .5 1 1\n") + tail;
            Fixture f(rows.c_str());
            auto *shared = jce_editor_game_input_bridge_shared();
            jce_editor_game_input_bridge_publish(shared, &f.view);
            main_runtime = reinterpret_cast<JceRuntime *>(&f);
            main_play_state = JCE_PLAY_PLAYING;
            g_game_events.begin(f.path.c_str(), "elapsed_ms", nullptr);
            editor_game_event_fixture();
            REQUIRE(g_game_events.end_pending); REQUIRE_FALSE(g_game_events.data);
            REQUIRE(shared->virtual_pointer.buttons == 1);
            if (mode == 0) f.view.visible = false;
            if (mode == 1) f.view.owner = JCE_EDITOR_GAME_INPUT_FREE_FLY;
            if (mode == 2) f.view.x += 1;
            if (mode == 3) f.view.y += 1;
            if (mode == 4) f.view.width += 1;
            if (mode == 5) f.view.height += 1;
            if (mode == 6) f.view.render_width += 1;
            if (mode == 7) f.view.render_height += 1;
            if (mode == 8) main_runtime = reinterpret_cast<JceRuntime *>(shared);
            if (mode == 9) main_runtime = nullptr;
            if (mode == 10) main_play_state = 2; // Paused.
            if (mode == 11) main_play_state = 0; // Stopped.
            if (mode == 12) f.view.render_width = std::numeric_limits<float>::infinity();
            jce_editor_game_input_bridge_publish(shared, &f.view);
            fixture_pre_commit();
            CHECK_FALSE(shared->virtual_pointer.active);
            CHECK_FALSE(g_game_events.end_pending); CHECK_FALSE(g_game_events.active);
            // The ordinary commit may refuse a non-runtime viewport; republish
            // only after the actual guard to count what a new runtime receives.
            f.view.visible = true; f.view.owner = JCE_EDITOR_GAME_INPUT_RUNTIME;
            f.view.render_width = 1280;
            jce_editor_game_input_bridge_publish(shared, &f.view);
            Commit p; p.consume(*shared); CHECK(p.buttons == 0);
            g_game_events.end(shared); jce_editor_game_input_bridge_reset(shared);
            main_runtime = nullptr; main_play_state = JCE_PLAY_PLAYING;
        }
    }
}
