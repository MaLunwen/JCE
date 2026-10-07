#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "../../editor/src/core/jce_editor_play_transaction.h"
#include <string>

TEST_CASE("capture failure cannot admit a mutable runtime and can be retried") {
    bool valid = false;
    int scene = 7, snapshot = -1, creates = 0;
    CHECK_FALSE(jce_editor_play_capture(valid,
        [&] { snapshot = 99; return false; }, [&] { snapshot = -1; }));
    CHECK(snapshot == -1);
    CHECK_FALSE(jce_editor_play_create(valid,
        [&] { ++creates; scene = 8; return true; }, [] {}, [] { return true; }, [] {}));
    CHECK(creates == 0);
    CHECK(scene == 7);
    REQUIRE(jce_editor_play_capture(valid,
        [&] { snapshot = scene; return true; }, [] {}));
    CHECK(jce_editor_play_create(valid,
        [&] { ++creates; scene = 8; return true; }, [] {}, [] { return true; }, [] {}));
    CHECK(creates == 1);
    CHECK(valid);
}

TEST_CASE("failed runtime construction releases owners before authored rollback") {
    bool valid = true, audio = true, stats = true;
    int scene = 99, snapshot = 7;
    std::string order;
    CHECK_FALSE(jce_editor_play_create(valid,
        [&] { order += 'C'; scene = 100; return false; },
        [&] { order += 'T'; audio = stats = false; },
        [&] { order += 'R'; CHECK_FALSE(audio); CHECK_FALSE(stats);
              scene = snapshot; return true; },
        [&] { order += 'D'; snapshot = -1; }));
    CHECK(order == "CTRD");
    CHECK(scene == 7);
    CHECK_FALSE(valid);
    REQUIRE(jce_editor_play_capture(valid,
        [&] { snapshot = scene; return true; }, [] {}));
    CHECK(jce_editor_play_create(valid, [] { return true; }, [] {},
        [] { return false; }, [] {}));
}

TEST_CASE("rollback failure preserves snapshot until explicit Stop retry succeeds") {
    bool valid = true;
    int snapshot = 7, creates = 0;
    CHECK_FALSE(jce_editor_play_create(valid, [] { return false; }, [] {},
        [] { return false; }, [&] { snapshot = -1; }));
    CHECK(valid);
    CHECK(snapshot == 7);
    CHECK_FALSE(jce_editor_play_capture(valid,
        [&] { ++creates; return true; }, [&] { snapshot = -1; }));
    CHECK(creates == 0);
    CHECK_FALSE(jce_editor_play_restore(valid, [] { return false; },
        [&] { snapshot = -1; }));
    CHECK(snapshot == 7);
    CHECK(jce_editor_play_restore(valid, [] { return true; },
        [&] { snapshot = -1; }));
    CHECK_FALSE(valid);
    CHECK(snapshot == -1);
    CHECK(jce_editor_play_restore(valid, [] { FAIL("no second restore"); return false; }, [] {}));
}
