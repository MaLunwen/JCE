#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_editor_kpi_game_capture.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_timer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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

namespace {

struct GlobalCaptureFixture {
    std::mutex log_mutex;
    std::vector<std::string> logs;

    GlobalCaptureFixture()
    {
        jce_editor_kpi_game_capture_global_disable();
        jce_log_set_level(JCE_LOG_LEVEL_INFO);
        jce_log_set_sink(receive_log, this);
    }

    ~GlobalCaptureFixture()
    {
        jce_editor_kpi_game_capture_global_disable();
        jce_log_flush();
        jce_log_set_sink(nullptr, nullptr);
    }

    static void receive_log(const JceLogRecord *record, void *user)
    {
        if (std::strcmp(record->tag, "editor.game_view") != 0)
            return;
        auto &fixture = *static_cast<GlobalCaptureFixture *>(user);
        const std::lock_guard<std::mutex> lock(fixture.log_mutex);
        fixture.logs.emplace_back(record->message);
    }

    std::vector<std::string> recorded()
    {
        jce_log_flush();
        const std::lock_guard<std::mutex> lock(log_mutex);
        return logs;
    }

    bool configure(const char *size, const char *shots =
        "0.1|C:/tmp/first image.png;0.2|C:/tmp/second image.png")
    {
        char error[160] = {};
        return jce_editor_kpi_game_capture_global_configure(
            shots, size, error, sizeof(error));
    }

    static void natural_policy()
    {
        CHECK(jce_editor_kpi_game_capture_global_active());
        CHECK_FALSE(jce_editor_kpi_game_capture_global_forces_render());
        CHECK_FALSE(jce_editor_kpi_game_capture_global_needs_focus());
        uint16_t width = 1380, height = 876;
        CHECK_FALSE(jce_editor_kpi_game_capture_global_render_size(&width, &height));
        CHECK(width == 1380);
        CHECK(height == 876);
    }
};

} // namespace

TEST_CASE_FIXTURE(GlobalCaptureFixture,
                  "natural capture keeps ordinary extent and admission through its lifecycle")
{
    REQUIRE(configure("natural"));
    natural_policy();
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_STOPPED, 10.0);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PAUSED, 10.0);
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    CHECK(recorded().empty());

    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
    REQUIRE(jce_editor_kpi_game_capture_global_due_path() != nullptr);
    CHECK(std::strcmp(jce_editor_kpi_game_capture_global_due_path(),
                      "C:/tmp/first image.png") == 0);
    natural_policy();
    jce_editor_kpi_game_capture_global_mark_submitted(17, 18);
    REQUIRE(jce_editor_kpi_game_capture_global_in_flight());
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    natural_policy();
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PAUSED, 10.0);
    CHECK(jce_editor_kpi_game_capture_global_in_flight());

    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_STOPPED, 10.0);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 10.0);
    CHECK(jce_editor_kpi_game_capture_global_in_flight());
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    natural_policy();
    jce_editor_kpi_game_capture_global_mark_complete(true);
    CHECK_FALSE(jce_editor_kpi_game_capture_global_in_flight());
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    natural_policy();
}

TEST_CASE_FIXTURE(GlobalCaptureFixture,
                  "natural capture retains one accepted identity through Stop to terminal")
{
    REQUIRE(configure("natural"));
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
    const uint64_t before_admit = jce_time_perf_counter();
    jce_editor_kpi_game_capture_global_mark_submitted(17, 18);
    const uint64_t after_admit = jce_time_perf_counter();
    jce_editor_kpi_game_capture_global_mark_submitted(90, 91);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_STOPPED, 2.0);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_STOPPED, 2.0);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 2.0);
    const uint64_t before_terminal = jce_time_perf_counter();
    jce_editor_kpi_game_capture_global_mark_complete(true);
    const uint64_t after_terminal = jce_time_perf_counter();
    jce_editor_kpi_game_capture_global_mark_complete(false);
    jce_editor_kpi_game_capture_global_disable();

    const auto messages = recorded();
    REQUIRE(messages.size() == 3);
    unsigned source = 0, submit = 0;
    unsigned long long counter = 0, frequency = 0;
    double due = 0.0, admitted = 0.0;
    REQUIRE(std::sscanf(messages[0].c_str(),
        "Game View capture admitted source_frame=%u submit_frame=%u "
        "due_seconds=%lf admitted_seconds=%lf counter=%llu frequency=%llu",
        &source, &submit, &due, &admitted, &counter, &frequency) == 6);
    CHECK(source == 17);
    CHECK(submit == 18);
    CHECK(due == doctest::Approx(0.1));
    CHECK(admitted == doctest::Approx(0.1));
    CHECK(counter >= before_admit);
    CHECK(counter <= after_admit);
    CHECK(frequency == jce_time_perf_freq());
    CHECK(messages[0].ends_with(" path=C:/tmp/first image.png"));
    CHECK(messages[1] ==
        "Game View capture schedule ended reason=stopped elapsed_seconds=0.100000000");
    int success = -1;
    REQUIRE(std::sscanf(messages[2].c_str(),
        "Game View capture terminal success=%d source_frame=%u submit_frame=%u "
        "counter=%llu frequency=%llu",
        &success, &source, &submit, &counter, &frequency) == 5);
    CHECK(success == 1);
    CHECK(source == 17);
    CHECK(submit == 18);
    CHECK(counter >= before_terminal);
    CHECK(counter <= after_terminal);
    CHECK(frequency == jce_time_perf_freq());
    CHECK(messages[2].ends_with(" path=C:/tmp/first image.png"));
}

TEST_CASE_FIXTURE(GlobalCaptureFixture,
                  "natural capture stops before due and never restarts finished paths")
{
    REQUIRE(configure("natural"));
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.05);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_STOPPED, 1.0);
    for (int i = 0; i < 5; ++i)
        jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    jce_editor_kpi_game_capture_global_mark_submitted(30, 31);
    CHECK_FALSE(jce_editor_kpi_game_capture_global_in_flight());

    REQUIRE(configure("natural"));
    for (unsigned frame = 1; frame <= 2; ++frame) {
        jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
        REQUIRE(jce_editor_kpi_game_capture_global_due_path() != nullptr);
        jce_editor_kpi_game_capture_global_mark_submitted(frame, frame);
        REQUIRE(jce_editor_kpi_game_capture_global_in_flight());
        jce_editor_kpi_game_capture_global_mark_complete(true);
    }
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_STOPPED, 1.0);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 1.0);
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    CHECK_FALSE(jce_editor_kpi_game_capture_global_in_flight());
}

TEST_CASE_FIXTURE(GlobalCaptureFixture,
                  "natural capture distinguishes failure from an unfinished shutdown")
{
    REQUIRE(configure("natural"));
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
    jce_editor_kpi_game_capture_global_mark_submitted(4, 5);
    jce_editor_kpi_game_capture_global_mark_complete(false);
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
    CHECK_FALSE(jce_editor_kpi_game_capture_global_in_flight());
    CHECK(jce_editor_kpi_game_capture_global_due_path() == nullptr);
    auto messages = recorded();
    REQUIRE(messages.size() == 2);
    CHECK(messages[1].find("Game View capture terminal success=0 ") == 0);

    jce_editor_kpi_game_capture_global_disable();
    REQUIRE(configure("natural"));
    jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
    jce_editor_kpi_game_capture_global_mark_submitted(6, 7);
    const size_t before_shutdown = recorded().size();
    jce_editor_kpi_game_capture_global_disable();
    messages = recorded();
    REQUIRE(messages.size() == before_shutdown + 1);
    CHECK(messages.back() ==
        "Game View capture schedule ended reason=shutdown elapsed_seconds=0.100000000");
    CHECK_FALSE(jce_editor_kpi_game_capture_global_active());
    CHECK_FALSE(jce_editor_kpi_game_capture_global_in_flight());
}

TEST_CASE_FIXTURE(GlobalCaptureFixture,
                  "default and numeric capture retain deterministic size focus and restart")
{
    const char *sizes[] = {nullptr, "", "640x360"};
    for (const char *size : sizes) {
        REQUIRE(configure(size));
        CHECK(jce_editor_kpi_game_capture_global_active());
        CHECK(jce_editor_kpi_game_capture_global_forces_render());
        CHECK(jce_editor_kpi_game_capture_global_needs_focus());
        uint16_t width = 0, height = 0;
        REQUIRE(jce_editor_kpi_game_capture_global_render_size(&width, &height));
        CHECK(width == (size && size[0] ? 640 : 1280));
        CHECK(height == (size && size[0] ? 360 : 720));
        for (int i = 0; i < 10; ++i)
            jce_editor_kpi_game_capture_global_note_rendered();
        CHECK_FALSE(jce_editor_kpi_game_capture_global_needs_focus());
        jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
        REQUIRE(jce_editor_kpi_game_capture_global_due_path() != nullptr);
        jce_editor_kpi_game_capture_global_mark_submitted(8, 9);
        jce_editor_kpi_game_capture_global_mark_complete(true);
        jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_STOPPED, 1.0);
        jce_editor_kpi_game_capture_global_tick(JCE_EDITOR_KPI_PLAY_PLAYING, 0.1);
        REQUIRE(jce_editor_kpi_game_capture_global_due_path() != nullptr);
        CHECK(std::strcmp(jce_editor_kpi_game_capture_global_due_path(),
                          "C:/tmp/first image.png") == 0);
    }
    CHECK_FALSE(configure("natural "));
    CHECK_FALSE(jce_editor_kpi_game_capture_global_active());
    CHECK_FALSE(jce_editor_kpi_game_capture_global_forces_render());
}
