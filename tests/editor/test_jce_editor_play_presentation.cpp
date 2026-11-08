/* Exact private presentation owner with counted public engine/renderer ports.
 * This is a CPU consumption/transaction contract, not a bgfx/GPU certificate. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_play_presentation.h"
#include "core/jce_editor_play_transaction.h"

struct JceRenderer { bool vsync = false; bool fallback = false; };
namespace {
int target_fps = 0;
unsigned reads = 0, writes = 0, resets = 0, fallback_queries = 0, fps_writes = 0;
struct Fixture {
    JceRenderer renderer;
    JceEditorPlayPresentationState state;
    Fixture() {
        target_fps = 45;
        reads = writes = resets = fallback_queries = fps_writes = 0;
    }
    JceRenderSettings authored(int vsync, int fps) {
        JceRenderSettings result = {};
        result.vsync = vsync;
        result.target_framerate = fps;
        return result;
    }
};
}

extern "C" {
bool jce_renderer_is_fallback(const JceRenderer *r) {
    REQUIRE(r != nullptr);
    ++fallback_queries;
    return r->fallback;
}
bool jce_renderer_get_vsync(const JceRenderer *r) {
    REQUIRE(r != nullptr);
    REQUIRE_FALSE(r->fallback);
    ++reads;
    return r->vsync;
}
void jce_renderer_set_vsync(JceRenderer *r, bool enabled) {
    REQUIRE(r != nullptr);
    REQUIRE_FALSE(r->fallback);
    ++writes;
    if (r->vsync != enabled) ++resets;
    r->vsync = enabled;
}
int JCE_CALL jce_engine_get_target_fps(void) { return target_fps; }
void JCE_CALL jce_engine_set_target_fps(int fps) {
    REQUIRE(fps >= 0);
    ++fps_writes;
    target_fps = fps;
}
}

TEST_CASE("standard synced Play applies and restores the entry presentation") {
    Fixture f;
    const auto authored = f.authored(1, -1);
    jce_editor_play_presentation_begin(f.state, &f.renderer, authored, 0);
    CHECK(f.renderer.vsync);
    CHECK(target_fps == 0);
    CHECK(authored.vsync == 1);
    CHECK(authored.target_framerate == -1);
    CHECK(f.state.restore_pending);
    jce_editor_play_presentation_restore(f.state);
    CHECK_FALSE(f.renderer.vsync);
    CHECK(target_fps == 45);
    CHECK_FALSE(f.state.restore_pending);
    CHECK(f.state.renderer == nullptr);
    CHECK(resets == 2);
}

TEST_CASE("explicit uncapped Play disables sync without inventing a frame cap") {
    Fixture f;
    f.renderer.vsync = true;
    target_fps = 120;
    const auto authored = f.authored(0, 0);
    jce_editor_play_presentation_begin(f.state, &f.renderer, authored, 0);
    CHECK_FALSE(f.renderer.vsync);
    CHECK(target_fps == 0);
    jce_editor_play_presentation_restore(f.state);
    CHECK(f.renderer.vsync);
    CHECK(target_fps == 120);
}

TEST_CASE("positive authored frame rate is preserved above or below 60") {
    Fixture f;
    const int authored_rates[] = {30, 144};
    for (const int fps : authored_rates) {
        const auto authored = f.authored(1, fps);
        jce_editor_play_presentation_begin(f.state, &f.renderer, authored, 0);
        CHECK(target_fps == fps);
        CHECK(authored.target_framerate == fps);
        jce_editor_play_presentation_restore(f.state);
        CHECK(target_fps == 45);
    }
}

TEST_CASE("renderer absent still restores the renderer independent frame API") {
    Fixture f;
    jce_editor_play_presentation_begin(f.state, nullptr, f.authored(1, 30), 0);
    CHECK(target_fps == 30);
    jce_editor_play_presentation_restore(f.state);
    CHECK(target_fps == 45);
    CHECK(reads == 0);
    CHECK(writes == 0);
    CHECK(fallback_queries == 0);
}

TEST_CASE("fallback does not query or reset an unavailable bgfx backend") {
    Fixture f;
    f.renderer.fallback = true;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(1, 0), 0);
    CHECK(target_fps == 0);
    jce_editor_play_presentation_restore(f.state);
    CHECK(target_fps == 45);
    CHECK(fallback_queries == 1);
    CHECK(reads == 0);
    CHECK(writes == 0);
}

TEST_CASE("equal sync state submits no reset and repeated Stop is inert") {
    Fixture f;
    f.renderer.vsync = true;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(1, 45), 0);
    jce_editor_play_presentation_restore(f.state);
    const unsigned prior_reads = reads, prior_fps = fps_writes;
    jce_editor_play_presentation_restore(f.state);
    CHECK(reads == prior_reads);
    CHECK(fps_writes == prior_fps);
    CHECK(writes == 0);
    CHECK(resets == 0);
}

TEST_CASE("repeated begin cannot replace the entry snapshot") {
    Fixture f;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(1, 30), 0);
    const unsigned prior_reads = reads, prior_fps = fps_writes;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(0, 144), 0);
    CHECK(f.renderer.vsync);
    CHECK(target_fps == 30);
    CHECK(reads == prior_reads);
    CHECK(fps_writes == prior_fps);
    jce_editor_play_presentation_restore(f.state);
    CHECK_FALSE(f.renderer.vsync);
    CHECK(target_fps == 45);
}

TEST_CASE("runtime construction failure restores before scene rollback") {
    Fixture f;
    bool valid = true, rollback = false;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(1, 30), 0);
    CHECK_FALSE(jce_editor_play_create(valid,
        [] { return false; },
        [&] { jce_editor_play_presentation_restore(f.state); },
        [&] { rollback = true; CHECK_FALSE(f.renderer.vsync);
              CHECK(target_fps == 45); return true; }, [] {}));
    CHECK(rollback);
    CHECK_FALSE(valid);
    CHECK_FALSE(f.state.restore_pending);
}

TEST_CASE("failed scene rollback retains its snapshot but not Play presentation") {
    Fixture f;
    bool valid = true;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(1, 30), 0);
    CHECK_FALSE(jce_editor_play_create(valid, [] { return false; },
        [&] { jce_editor_play_presentation_restore(f.state); },
        [] { return false; }, [] {}));
    CHECK(valid);
    CHECK_FALSE(f.state.restore_pending);
    CHECK_FALSE(f.renderer.vsync);
    CHECK(target_fps == 45);
    const unsigned prior_writes = writes, prior_fps = fps_writes;
    jce_editor_play_presentation_restore(f.state);
    CHECK(jce_editor_play_restore(valid, [] { return true; }, [] {}));
    CHECK(writes == prior_writes);
    CHECK(fps_writes == prior_fps);
}

TEST_CASE("successful runtime construction keeps Play settings until Stop") {
    Fixture f;
    bool valid = true;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(1, 30), 0);
    CHECK(jce_editor_play_create(valid, [] { return true; },
        [&] { FAIL("successful construction must not tear down"); },
        [] { return false; }, [] {}));
    CHECK(f.renderer.vsync);
    CHECK(target_fps == 30);
    jce_editor_play_presentation_restore(f.state);
    CHECK_FALSE(f.renderer.vsync);
    CHECK(target_fps == 45);
}

TEST_CASE("capture rejection does not admit a presentation-changing transaction") {
    Fixture f;
    bool valid = false;
    CHECK_FALSE(jce_editor_play_capture(valid, [] { return false; }, [] {}));
    CHECK_FALSE(jce_editor_play_create(valid,
        [&] { jce_editor_play_presentation_begin(f.state, &f.renderer,
                                               f.authored(1, 30), 0); return true; },
        [] {}, [] { return true; }, [] {}));
    CHECK_FALSE(f.renderer.vsync);
    CHECK(target_fps == 45);
    CHECK_FALSE(f.state.restore_pending);
    CHECK(fps_writes == 0);
}

TEST_CASE("a later Play session captures the new editor entry values") {
    Fixture f;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(1, 30), 0);
    jce_editor_play_presentation_restore(f.state);
    f.renderer.vsync = true;
    target_fps = 72;
    jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(0, 144), 0);
    jce_editor_play_presentation_restore(f.state);
    CHECK(f.renderer.vsync);
    CHECK(target_fps == 72);
}

TEST_CASE("preview default cap and explicit opt-out retain project FPS and VSync") {
    Fixture f;
    struct Rate { int project, preview, expected; };
    const Rate rates[] = {{0,60,60}, {-1,60,60}, {30,60,30}, {144,60,60},
                          {0,0,0}, {144,0,144}, {0,144,144}, {144,240,144}};
    for (const auto &rate : rates) {
        const auto authored = f.authored(1, rate.project);
        jce_editor_play_presentation_begin(f.state, &f.renderer, authored, rate.preview);
        CHECK(target_fps == rate.expected);
        CHECK(authored.target_framerate == rate.project);
        CHECK(f.renderer.vsync);
        CHECK(f.state.authored_target_fps == (rate.project > 0 ? rate.project : 0));
        jce_editor_play_presentation_restore(f.state);
        CHECK(target_fps == 45);
        CHECK_FALSE(f.renderer.vsync);
    }
    jce_editor_play_presentation_begin(f.state, nullptr, f.authored(0,0));
    CHECK(target_fps == 60);
    CHECK(f.state.renderer == nullptr);
    jce_editor_play_presentation_restore(f.state);
    CHECK(target_fps == 45);
}

TEST_CASE("absent authored settings still cap FPS and retain entry VSync") {
    Fixture f;
    f.renderer.vsync = true;
    const int caps[] = {60, 0, 144};
    for (const int cap : caps) {
        jce_editor_play_presentation_begin(f.state, &f.renderer, f.authored(0,0), cap, false);
        CHECK(target_fps == cap);
        CHECK(f.renderer.vsync);
        CHECK(f.state.restore_pending);
        CHECK(writes == 0);
        jce_editor_play_presentation_restore(f.state);
        CHECK(target_fps == 45);
        CHECK(f.renderer.vsync);
        CHECK(writes == 0);
    }
}
