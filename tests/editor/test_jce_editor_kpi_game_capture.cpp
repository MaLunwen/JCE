#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_kpi_game_capture.h"

#include <cmath>
#include <cstring>

TEST_CASE("game capture schedule accepts one ordered absolute-path list")
{
    JceEditorKpiGameCaptureSchedule schedule{};
    char error[160] = {};
    const char *spec =
        "35.0|C:/tmp/space_transfer.png;"
        "70.0|C:/tmp/space_capture.png;"
        "100.0|C:/tmp/space_result.png";

    REQUIRE(jce_editor_kpi_game_capture_parse(spec, &schedule,
                                               error, sizeof(error)));
    CHECK(schedule.count == 3);
    CHECK(schedule.entries[0].seconds == doctest::Approx(35.0));
    CHECK(std::strcmp(schedule.entries[1].path,
                      "C:/tmp/space_capture.png") == 0);
    CHECK(std::strcmp(schedule.entries[2].path,
                      "C:/tmp/space_result.png") == 0);
}

TEST_CASE("game capture schedule rejects malformed or ambiguous input")
{
    const char *invalid[] = {
        "35|relative.png",
        "35|",
        "0|C:/tmp/a.png",
        "nan|C:/tmp/a.png",
        "35|C:/tmp/a.png;35|C:/tmp/b.png",
        "70|C:/tmp/a.png;35|C:/tmp/b.png",
        "1|C:/1.png;2|C:/2.png;3|C:/3.png;4|C:/4.png;"
        "5|C:/5.png;6|C:/6.png;7|C:/7.png;8|C:/8.png;9|C:/9.png",
        "35C:/tmp/a.png",
    };

    for (const char *spec : invalid) {
        CAPTURE(spec);
        JceEditorKpiGameCaptureSchedule schedule{};
        char error[160] = {};
        CHECK_FALSE(jce_editor_kpi_game_capture_parse(
            spec, &schedule, error, sizeof(error)));
        CHECK(error[0] != '\0');
    }
}

TEST_CASE("game capture size is strict and bounded")
{
    uint16_t width = 0;
    uint16_t height = 0;
    CHECK(jce_editor_kpi_game_capture_parse_size("1280x720", &width, &height));
    CHECK(width == 1280);
    CHECK(height == 720);

    CHECK_FALSE(jce_editor_kpi_game_capture_parse_size("1280X720", &width, &height));
    CHECK_FALSE(jce_editor_kpi_game_capture_parse_size("0x720", &width, &height));
    CHECK_FALSE(jce_editor_kpi_game_capture_parse_size("1280x", &width, &height));
    CHECK_FALSE(jce_editor_kpi_game_capture_parse_size("70000x720", &width, &height));
    CHECK_FALSE(jce_editor_kpi_game_capture_parse_size("1280x720junk", &width, &height));
}

TEST_CASE("game capture clock advances only while playing and serializes readback")
{
    JceEditorKpiGameCaptureSchedule schedule{};
    char error[160] = {};
    REQUIRE(jce_editor_kpi_game_capture_parse(
        "1|C:/tmp/a.png;2|C:/tmp/b.png", &schedule,
        error, sizeof(error)));

    jce_editor_kpi_game_capture_tick(
        &schedule, JCE_EDITOR_KPI_PLAY_STOPPED, 20.0);
    CHECK(schedule.elapsed_seconds == doctest::Approx(0.0));

    /* Every PLAYING step is clamped to 100 ms (the deterministic-clock stall
     * clamp) — advance in frame-sized steps like the editor does. */
    auto tick_playing = [](JceEditorKpiGameCaptureSchedule *s, double total) {
        while (total > 1e-9) {
            double step = total > 0.05 ? 0.05 : total;
            jce_editor_kpi_game_capture_tick(
                s, JCE_EDITOR_KPI_PLAY_PLAYING, step);
            total -= step;
        }
    };

    tick_playing(&schedule, 0.75);
    CHECK(jce_editor_kpi_game_capture_due(&schedule) == nullptr);
    jce_editor_kpi_game_capture_tick(
        &schedule, JCE_EDITOR_KPI_PLAY_PAUSED, 5.0);
    CHECK(schedule.elapsed_seconds == doctest::Approx(0.75));
    tick_playing(&schedule, 0.25);

    const JceEditorKpiGameCaptureEntry *due =
        jce_editor_kpi_game_capture_due(&schedule);
    REQUIRE(due != nullptr);
    CHECK(std::strcmp(due->path, "C:/tmp/a.png") == 0);
    REQUIRE(jce_editor_kpi_game_capture_mark_submitted(&schedule));
    CHECK(jce_editor_kpi_game_capture_due(&schedule) == nullptr);

    tick_playing(&schedule, 2.0);
    CHECK(jce_editor_kpi_game_capture_due(&schedule) == nullptr);
    jce_editor_kpi_game_capture_mark_complete(&schedule, true);
    due = jce_editor_kpi_game_capture_due(&schedule);
    REQUIRE(due != nullptr);
    CHECK(std::strcmp(due->path, "C:/tmp/b.png") == 0);

    REQUIRE(jce_editor_kpi_game_capture_mark_submitted(&schedule));
    jce_editor_kpi_game_capture_mark_complete(&schedule, true);
    CHECK(jce_editor_kpi_game_capture_finished(&schedule));

    jce_editor_kpi_game_capture_tick(
        &schedule, JCE_EDITOR_KPI_PLAY_STOPPED, 1.0);
    jce_editor_kpi_game_capture_tick(
        &schedule, JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
    CHECK(schedule.next_index == 0);
    CHECK(schedule.elapsed_seconds == doctest::Approx(0.1));
}
