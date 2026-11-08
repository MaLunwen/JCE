/* Complete private CSV producer, actual ring/UI export bodies and real host IO.
 * Counted fault ports do not certify native button delivery or GUI pixels. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_owned_directory.h"
#include "core/jce_editor_alloc.h"
#include "core/jce_editor_config.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_path.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
struct Ports {
    unsigned atomic = 0, ordinary = 0, alloc = 0, release = 0;
    bool fail_write = false, fail_alloc = false, export_pressed = false;
    std::vector<int> levels;
    std::vector<std::string> ui;
    std::string wrong_root;
} ports;

void *alloc_port(size_t size)
{
    ++ports.alloc;
    return ports.fail_alloc ? nullptr : jce_malloc(size);
}
void release_port(void *bytes) { ++ports.release; jce_free(bytes); }
bool atomic_port(const char *path, const void *bytes, uint64_t length)
{
    ++ports.atomic;
    return !ports.fail_write && jce_fs_host_write_all_atomic(path, bytes, length);
}
[[maybe_unused]] bool ordinary_port(const char *path, const void *bytes, uint64_t length)
{
    ++ports.ordinary;
    if (ports.fail_write) {
        (void)jce_fs_host_write_all(path, "partial", 7);
        return false;
    }
    return jce_fs_host_write_all(path, bytes, length);
}
[[maybe_unused]] bool old_profile_path(const char *name, char *out, size_t capacity)
{
    return jce_path_join(out, capacity, ports.wrong_root.c_str(), name);
}
void log_port(JceLogLevel level, const char *, const char *, int, const char *, ...)
{
    ports.levels.push_back(static_cast<int>(level));
}
std::string read(const char *path)
{
    uint64_t length = 0, total = 0;
    char *body = static_cast<char *>(jce_fs_host_read_capped(path, 65536, &length, &total));
    if (!body) return {};
    CHECK(length == total);
    const std::string text(body, static_cast<size_t>(length));
    jce_fs_buffer_free(body);
    return text;
}
} // namespace

#undef ED_MALLOC
#undef ED_FREE
#define ED_MALLOC(size) alloc_port(size)
#define ED_FREE(bytes) release_port(bytes)
#define jce_fs_host_write_all_atomic atomic_port
#define jce_fs_host_write_all ordinary_port
#ifndef JCE_EDITOR_ANALYZER_EXPORT_HEADER
#define JCE_EDITOR_ANALYZER_EXPORT_HEADER "panels/jce_panel_profile_analyzer.h"
#endif
#include JCE_EDITOR_ANALYZER_EXPORT_HEADER
#undef ED_MALLOC
#undef ED_FREE
#undef jce_fs_host_write_all_atomic
#undef jce_fs_host_write_all

/* Typed UI ports exercise the actual export button/status consumer without
 * an ImGui context. Stored knobs use their ordinary unchanged default values. */
const char *jce_editor_i18n(const char *key) { return key; }
const char *jce_editor_i18n_id(const char *, const char *id) { return id; }
float jce_editor_ui_state_load_float(const char *, float fallback, float, float) { return fallback; }
int jce_editor_ui_state_load_int(const char *, int fallback, int, int) { return fallback; }
void jce_editor_ui_state_save_float(const char *, float) {}
void jce_editor_ui_state_save_int(const char *, int) {}
namespace ImGui {
bool Checkbox(const char *, bool *) { return false; }
void SameLine() {}
void SetNextItemWidth(float) {}
bool DragFloat(const char *, float *, float, float, float, const char *) { return false; }
bool SmallButton(const char *id)
{
    if (std::strcmp(id, "pa_csv") != 0 || !ports.export_pressed) return false;
    ports.export_pressed = false;
    return true;
}
void TextWrapped(const char *format, ...)
{
    char text[2048]; va_list args; va_start(args, format);
    const int n = std::vsnprintf(text, sizeof text, format, args); va_end(args);
    REQUIRE(n >= 0);
    REQUIRE(static_cast<size_t>(n) < sizeof text);
    ports.ui.emplace_back(text);
}
} // namespace ImGui

#ifndef JCE_EDITOR_ANALYZER_ACTUAL_WIRE
#error JCE_EDITOR_ANALYZER_ACTUAL_WIRE must name the actual producer consumer
#endif
#define jce_log_write log_port
#include JCE_EDITOR_ANALYZER_ACTUAL_WIRE
#undef jce_log_write

namespace {
using Result = JceEditorProfileAnalyzerExportResult;
constexpr const char *header =
    "frame,frame_ms,backend_cpu_interval_ms,latched_gpu_ms,api_wait_ms,"
    "cpu_available,gpu_available,wait_available\n";

struct Fixture {
    JceTestOwnedDirectory owned{"_ut_profile_analyzer_export"};
    std::string profile = owned.path("profile");
    JceTestEnvironment isolated{"JCE_EDITOR_PROFILE_DIR", profile.c_str()};
    std::string desired = profile + "/reports/profiler/profile_export.csv";
    Fixture()
    {
        ports = Ports{};
        ports.wrong_root = owned.path("outside-profile");
        analyzer_actual::s = analyzer_actual::State{};
    }
    void push(uint64_t frame, float duration = 16.0f)
    {
        JceEditorProfileAnalyzerRecord r{};
        r.frame_no = frame; r.frame_ms = duration;
        analyzer_actual::push_record(r);
    }
    Result publish()
    {
        ports.ui.clear();
        ports.export_pressed = true;
        analyzer_actual::draw_toolbar();
        CHECK(analyzer_actual::s.export_attempted);
        CHECK_FALSE(ports.export_pressed);
        return analyzer_actual::s.export_result;
    }
    void check_ui(const char *key)
    {
        CHECK(std::find(ports.ui.begin(), ports.ui.end(), key) != ports.ui.end());
    }
};
} // namespace

TEST_CASE("actual export distinguishes unavailable fields from a valid zero")
{
    Fixture f;
    JceEditorProfileAnalyzerRecord r{}; r.frame_no = 1; r.frame_ms = 16;
    r.cpu_ms = std::numeric_limits<float>::quiet_NaN();
    r.gpu_ms = 0; r.gpu_available = true;
    analyzer_actual::push_record(r);
    REQUIRE(f.publish() == Result::Written);
    CHECK(read(analyzer_actual::s.export_path) == std::string(header) + "1,16.000,,0.000,,0,1,0\n");
    CHECK(std::string(analyzer_actual::s.export_path) == f.desired);
    CHECK(ports.atomic == 1); CHECK(ports.ordinary == 0);
    CHECK(ports.alloc == 1); CHECK(ports.release == 1);
    REQUIRE(ports.levels.size() == 1); CHECK(ports.levels.back() == JCE_LOG_LEVEL_INFO);
    f.check_ui("profileAnalyzer.export.saved");
}

TEST_CASE("actual ring wrap exports every retained sample from oldest to newest")
{
    Fixture f;
    for (uint64_t frame = 1; frame <= 300; ++frame) f.push(frame);
    REQUIRE(f.publish() == Result::Written);
    std::string expected = header;
    for (uint64_t frame = 45; frame <= 300; ++frame)
        expected += std::to_string(frame) + ",16.000,,,,0,0,0\n";
    CHECK(read(analyzer_actual::s.export_path) == expected);
    CHECK(analyzer_actual::at(0).frame_no == 45);
    CHECK(analyzer_actual::at(255).frame_no == 300);
}

TEST_CASE("bounded encoding rejects truncation and nonfinite known values before publication")
{
    Fixture f; f.push(1);
    char tiny[16]; size_t length = 99;
    CHECK_FALSE(jce_editor_profile_analyzer_encode(analyzer_actual::s.buf, 1, 1, tiny, sizeof tiny, length));
    CHECK(length == 0);
    analyzer_actual::s.buf[0].cpu_available = true;
    analyzer_actual::s.buf[0].cpu_ms = std::numeric_limits<float>::infinity();
    CHECK(f.publish() == Result::Incomplete);
    CHECK(ports.atomic == 0); CHECK(ports.ordinary == 0);
    CHECK_FALSE(jce_fs_host_exists_file(f.desired.c_str()));
    CHECK(ports.alloc == ports.release);
    f.check_ui("profileAnalyzer.export.incomplete");
    REQUIRE(ports.levels.size() == 1); CHECK(ports.levels.back() == JCE_LOG_LEVEL_WARN);
}

TEST_CASE("maximum finite metrics and unsigned frame identity produce a complete CSV")
{
    Fixture f;
    JceEditorProfileAnalyzerRecord r{};
    r.frame_no = UINT64_MAX; r.frame_ms = r.cpu_ms = r.gpu_ms = r.wait_ms = std::numeric_limits<float>::max();
    r.cpu_available = r.gpu_available = r.wait_available = true;
    for (int i = 0; i < JCE_EDITOR_PROFILE_ANALYZER_CAPACITY; ++i) analyzer_actual::push_record(r);
    REQUIRE(f.publish() == Result::Written);
    const auto text = read(analyzer_actual::s.export_path);
    CHECK(std::count(text.begin(), text.end(), '\n') == 257);
    CHECK(text.find("18446744073709551615,") == std::strlen(header));
    CHECK(text.size() < JCE_EDITOR_PROFILE_ANALYZER_CSV_BYTES);
    CHECK(text.rfind(",1,1,1\n") == text.size() - 7);
}

TEST_CASE("invalid explicit profile and insufficient path capacity never select CWD")
{
    Fixture f; f.push(1);
    f.isolated.set("relative/profile");
    CHECK(f.publish() == Result::ProfileUnavailable);
    CHECK(analyzer_actual::s.export_path[0] == '\0');
    CHECK(ports.atomic == 0); CHECK(ports.ordinary == 0); CHECK(ports.alloc == 0);
    f.check_ui("profileAnalyzer.export.pathUnavailable");
    f.isolated.set(f.profile.c_str());
    char short_path[2];
    CHECK(jce_editor_profile_analyzer_publish(analyzer_actual::s.buf, 1, 1,
          short_path, sizeof short_path) == Result::ProfileUnavailable);
    CHECK(ports.atomic == 0); CHECK(ports.ordinary == 0);
}

TEST_CASE("failed atomic publication preserves the previous complete file and retries")
{
    Fixture f; f.push(1);
    REQUIRE(f.publish() == Result::Written);
    const auto previous = read(f.desired.c_str()); REQUIRE_FALSE(previous.empty());
    f.push(2); ports.fail_write = true;
    CHECK(f.publish() == Result::WriteFailed);
    CHECK(read(f.desired.c_str()) == previous);
    f.check_ui("profileAnalyzer.export.writeFailed");
    REQUIRE(ports.levels.size() == 2); CHECK(ports.levels.back() == JCE_LOG_LEVEL_WARN);
    ports.fail_write = false;
    REQUIRE(f.publish() == Result::Written);
    CHECK(read(f.desired.c_str()) == std::string(header) + "1,16.000,,,,0,0,0\n2,16.000,,,,0,0,0\n");
    CHECK(ports.atomic == 3); CHECK(ports.ordinary == 0); CHECK(ports.alloc == ports.release);
}

TEST_CASE("first failed request retains no report and the same button can retry")
{
    Fixture f; f.push(1); ports.fail_write = true;
    CHECK(f.publish() == Result::WriteFailed);
    CHECK_FALSE(jce_fs_host_exists_file(f.desired.c_str()));
    ports.fail_write = false;
    REQUIRE(f.publish() == Result::Written);
    CHECK_FALSE(read(f.desired.c_str()).empty());
    CHECK(ports.atomic == 2); CHECK(ports.ordinary == 0);
}

TEST_CASE("empty and allocation-refused exports have distinct visible failures")
{
    Fixture f;
    CHECK(f.publish() == Result::Empty);
    f.check_ui("profileAnalyzer.export.empty");
    CHECK(ports.alloc == 0); CHECK(ports.atomic == 0);
    f.push(1); ports.fail_alloc = true;
    CHECK(f.publish() == Result::AllocationFailed);
    f.check_ui("profileAnalyzer.export.allocationFailed");
    CHECK(ports.alloc == 1); CHECK(ports.release == 0); CHECK(ports.atomic == 0);
    ports.fail_alloc = false;
    CHECK(f.publish() == Result::Written);
    CHECK(ports.alloc == 2); CHECK(ports.release == 1);
}
