#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "../../editor/src/core/jce_editor_screenshot_completion.h"
#include <string>

TEST_CASE("accepted screenshot remains pending until actual write and notifies once") {
    JceEditorScreenshotOwner owner = {};
    int submits = 0, polls = 0, notices = 0;
    auto submit = [&](const char *) { ++submits; return true; };
    CHECK(jce_editor_screenshot_request(owner, "first.png", true, 0, submit));
    CHECK_FALSE(jce_editor_screenshot_request(owner, "second.png", false, 2, submit));
    CHECK(submits == 1);
    CHECK(std::string(owner.path) == "first.png");
    auto notify = [&](const JceEditorScreenshotOwner &done, int status) {
        ++notices;
        CHECK_FALSE(owner.active);
        CHECK(done.interactive);
        CHECK(std::string(done.path) == "first.png");
        CHECK(status == 1);
    };
    jce_editor_screenshot_complete(owner, [&] { ++polls; return 0; }, notify);
    CHECK(owner.active);
    CHECK(notices == 0);
    jce_editor_screenshot_complete(owner, [&] { ++polls; return 1; }, notify);
    jce_editor_screenshot_complete(owner, [&] { ++polls; return 1; }, notify);
    CHECK(polls == 2);
    CHECK(notices == 1);
}

TEST_CASE("bounds and rejected SDK requests cannot acquire or truncate ownership") {
    JceEditorScreenshotOwner owner = {};
    int submits = 0;
    auto submit = [&](const char *) { ++submits; return false; };
    CHECK_FALSE(jce_editor_screenshot_request(owner, nullptr, true, 0, submit));
    CHECK_FALSE(jce_editor_screenshot_request(owner, "", true, 0, submit));
    std::string overflow(sizeof(owner.path), 'a');
    CHECK_FALSE(jce_editor_screenshot_request(owner, overflow.c_str(), true, 0, submit));
    CHECK(submits == 0);
    CHECK_FALSE(jce_editor_screenshot_request(owner, "failed.png", true, 0, submit));
    CHECK(submits == 1);
    CHECK_FALSE(owner.active);
    std::string maximum(sizeof(owner.path) - 1, 'b');
    CHECK(jce_editor_screenshot_request(owner, maximum.c_str(), false, 1,
        [](const char *) { return true; }));
    CHECK(std::string(owner.path) == maximum);
}

TEST_CASE("maintenance write failure retains source and terminal callback may admit next request") {
    JceEditorScreenshotOwner owner = {};
    REQUIRE(jce_editor_screenshot_request(owner, "qa.png", false, 2,
        [](const char *) { return true; }));
    int notices = 0;
    jce_editor_screenshot_complete(owner, [] { return 2; },
        [&](const JceEditorScreenshotOwner &done, int status) {
            ++notices;
            CHECK_FALSE(done.interactive);
            CHECK(done.source == 2);
            CHECK(status == 2);
            CHECK(std::string(done.path) == "qa.png");
            CHECK(jce_editor_screenshot_request(owner, "retry.png", true, 0,
                [](const char *) { return true; }));
        });
    CHECK(notices == 1);
    CHECK(owner.active);
    CHECK(std::string(owner.path) == "retry.png");
    jce_editor_screenshot_complete(owner, [] { return -1; },
        [&](const JceEditorScreenshotOwner &, int) { ++notices; });
    CHECK(owner.active);
    CHECK(notices == 1);
}

TEST_CASE("shared slot blocks worker-finished interleaving until matching owner polls") {
    for (unsigned first : {1u, 2u, 3u}) {
        JceEditorScreenshotSlot slot = {};
        int backend = 0, submits = 0, polls = 0;
        auto submit = [&] { ++submits; backend = 0; return true; };
        REQUIRE(jce_editor_screenshot_slot_submit(slot, first, submit));
        CHECK(jce_editor_screenshot_slot_poll(slot, first,
            [&] { ++polls; return backend; }) == 0);
        backend = 1; /* PNG worker completes after the early owner poll. */
        unsigned other = first == 1 ? 2 : 1;
        CHECK_FALSE(jce_editor_screenshot_slot_submit(slot, other, submit));
        CHECK(submits == 1);
        CHECK(backend == 1);
        CHECK(jce_editor_screenshot_slot_poll(slot, other,
            [&] { ++polls; backend = -1; return 1; }) == -1);
        CHECK(polls == 1);
        CHECK(jce_editor_screenshot_slot_poll(slot, first,
            [&] { ++polls; return backend; }) == 1);
        CHECK(slot.owner == 0);
        CHECK(jce_editor_screenshot_slot_submit(slot, other, submit));
        CHECK(submits == 2);
    }
}

TEST_CASE("shared slot rejection and failure retain exact legacy owner") {
    JceEditorScreenshotSlot slot = {};
    CHECK_FALSE(jce_editor_screenshot_slot_submit(slot, 2, [] { return false; }));
    CHECK(slot.owner == 0);
    REQUIRE(jce_editor_screenshot_slot_submit(slot, 3, [] { return true; }));
    CHECK(jce_editor_screenshot_slot_poll(slot, 2, [] { return 2; }) == -1);
    CHECK(slot.owner == 3);
    CHECK(jce_editor_screenshot_slot_poll(slot, 3, [] { return 2; }) == 2);
    CHECK(slot.owner == 0);
    CHECK(jce_editor_screenshot_slot_poll(slot, 3, [] { return 2; }) == -1);
}

TEST_CASE("uncoordinated admission destroys the prior latest-result identity") {
    int result = 0;
    unsigned backend_owner = 1;
    result = 1; /* First worker finishes between early poll and late submit. */
    auto raw_submit = [&] { backend_owner = 2; result = 0; return true; };
    CHECK(raw_submit());
    CHECK(backend_owner != 1);
    CHECK(result != 1); /* Negative control: old owner can no longer observe its result. */
}

TEST_CASE("idle legacy poll pumps recording without consuming backbuffer ownership") {
    JceEditorScreenshotSlot slot = {};
    int pumps = 0;
    auto recorder = [&] { ++pumps; return 1; };
    CHECK(jce_editor_screenshot_legacy_poll(slot, 2, recorder) == -1);
    CHECK(pumps == 1);
    CHECK(slot.owner == 0);
    REQUIRE(jce_editor_screenshot_slot_submit(slot, 1, [] { return true; }));
    CHECK(jce_editor_screenshot_legacy_poll(slot, 2, recorder) == -1);
    CHECK(pumps == 2);
    CHECK(slot.owner == 1);
    REQUIRE(jce_editor_screenshot_slot_poll(slot, 1, [] { return 1; }) == 1);
    REQUIRE(jce_editor_screenshot_slot_submit(slot, 3, [] { return true; }));
    CHECK(jce_editor_screenshot_legacy_poll(slot, 2, recorder) == -1);
    CHECK(pumps == 2);
    CHECK(slot.owner == 3);
    CHECK(jce_editor_screenshot_legacy_poll(slot, 3, recorder) == 1);
    CHECK(pumps == 3);
    CHECK(slot.owner == 0);
}

TEST_CASE("automatic Game capture routes by actual lease without a KPI schedule") {
    JceEditorScreenshotSlot slot = {};
    CHECK_FALSE(jce_editor_screenshot_poll_game(slot, false));
    REQUIRE(jce_editor_screenshot_slot_submit(slot, 3, [] { return true; }));
    CHECK(jce_editor_screenshot_poll_game(slot, false));
    CHECK(jce_editor_screenshot_legacy_poll(slot, 3, [] { return 0; }) == 0);
    CHECK(jce_editor_screenshot_poll_game(slot, false));
    CHECK(jce_editor_screenshot_legacy_poll(slot, 3, [] { return 1; }) == 1);
    CHECK_FALSE(jce_editor_screenshot_poll_game(slot, false));
    CHECK(jce_editor_screenshot_poll_game(slot, true));
}


/* Actual Core screenshot poll and shutdown-observer block. The typed public
 * CPU-queue port is independently covered by the real Renderer PNG fixture. */
#include <jce/application/jce_screenshot.h>
#include <jce/renderer/jce_renderer.h>
#include "../../editor/src/core/jce_editor.h"
#include "../../editor/src/core/jce_editor_i18n.h"
#include "../../editor/src/core/jce_editor_toast.h"
#ifndef JCE_TEST_EDITOR_SCREENSHOT_CLOSE_BODY
#error actual Editor screenshot close owner is required
#endif
static struct {
    JceEditorScreenshotOwner screenshot;
    JceEditorScreenshotSlot screenshot_slot;
} s_editor;
static struct {
    int result = 0, queued_result = 1;
    bool queued = false;
    int flushes = 0, polls = 0, infos = 0, errors = 0, warnings = 0, toasts = 0;
    std::string order;
} close_ports;
extern "C" void jce_renderer_readback_capture_flush(void)
{
    ++close_ports.flushes;
    close_ports.order += 'F';
    if (close_ports.queued) {
        close_ports.result = close_ports.queued_result;
        close_ports.queued = false;
    }
}
extern "C" int jce_screenshot_poll(void)
{
    ++close_ports.polls;
    close_ports.order += 'P';
    return close_ports.result;
}
extern "C" const char *jce_editor_i18n(const char *key) { return key; }
extern "C" void jce_toast_success(const char *, ...) { ++close_ports.toasts; }
extern "C" void jce_toast_error(const char *, ...) { ++close_ports.toasts; }
#define LOG_TAG "screenshot-close-test"
#define LOG_INFO(...) (++close_ports.infos)
#define LOG_ERROR(...) (++close_ports.errors)
#define LOG_WARN(...) (++close_ports.warnings)
#include JCE_TEST_EDITOR_SCREENSHOT_CLOSE_BODY
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_INFO
#undef LOG_TAG

static void reset_close_owner(bool active)
{
    s_editor = {};
    close_ports = {};
    if (active) {
        REQUIRE(jce_editor_screenshot_request(s_editor.screenshot,
            "final-frame.png", true, 2, [](const char *) { return true; }));
        REQUIRE(jce_editor_screenshot_slot_submit(s_editor.screenshot_slot,
            JCE_EDITOR_SCREENSHOT_BACKBUFFER, [] { return true; }));
    }
}
TEST_CASE("actual close drains queued CPU success before its one terminal poll")
{
    reset_close_owner(true);
    close_ports.queued = true;
    test_actual_screenshot_close();
    CHECK(close_ports.order == "FP");
    CHECK(close_ports.flushes == 1); CHECK(close_ports.polls == 1);
    CHECK(close_ports.infos == 1); CHECK(close_ports.errors == 0);
    CHECK(close_ports.warnings == 0); CHECK(close_ports.toasts == 0);
    CHECK_FALSE(s_editor.screenshot.active);
    CHECK(s_editor.screenshot_slot.owner == 0);
    CHECK(std::string(s_editor.screenshot.path) == "final-frame.png");
    test_actual_screenshot_close();
    CHECK(close_ports.infos == 1); CHECK(close_ports.polls == 1);
}
TEST_CASE("actual close reports queued CPU failure without inventing success")
{
    reset_close_owner(true);
    close_ports.queued = true; close_ports.queued_result = 2;
    test_actual_screenshot_close();
    CHECK(close_ports.order == "FP");
    CHECK(close_ports.errors == 1); CHECK(close_ports.infos == 0);
    CHECK(close_ports.warnings == 0); CHECK(close_ports.toasts == 0);
    CHECK_FALSE(s_editor.screenshot.active);
    CHECK(s_editor.screenshot_slot.owner == 0);
}
TEST_CASE("actual close leaves undelivered and copying callbacks explicitly unresolved")
{
    reset_close_owner(true); /* No CPU job is queued yet. Flush cannot wait for GPU. */
    test_actual_screenshot_close();
    CHECK(close_ports.order == "FP"); CHECK(close_ports.result == 0);
    CHECK(close_ports.infos == 0); CHECK(close_ports.errors == 0);
    CHECK(close_ports.warnings == 1); CHECK(close_ports.toasts == 0);
    CHECK_FALSE(s_editor.screenshot.active);
    CHECK(s_editor.screenshot_slot.owner == JCE_EDITOR_SCREENSHOT_BACKBUFFER);
}
TEST_CASE("actual close idle and foreign viewport leases do not consume a result")
{
    reset_close_owner(false);
    test_actual_screenshot_close();
    CHECK(close_ports.order == "F"); CHECK(close_ports.polls == 0);
    CHECK(close_ports.infos == 0); CHECK(close_ports.errors == 0);
    CHECK(close_ports.warnings == 0); CHECK(close_ports.toasts == 0);
    reset_close_owner(false);
    REQUIRE(jce_editor_screenshot_slot_submit(s_editor.screenshot_slot,
        JCE_EDITOR_SCREENSHOT_GAME, [] { return true; }));
    close_ports.result = 1;
    test_actual_screenshot_close();
    CHECK(close_ports.polls == 0); CHECK(close_ports.infos == 0);
    CHECK(s_editor.screenshot_slot.owner == JCE_EDITOR_SCREENSHOT_GAME);
}
