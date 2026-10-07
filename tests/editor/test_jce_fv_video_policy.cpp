#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "viewers/jce_fv_video_policy.h"
#include <limits>

TEST_CASE("Interpolated deadline cannot hide behind the consumed PCM cursor")
{
    const double frame = 1.0 / 60.0;
    CHECK(fv_video_picture_ready(9.630, 9.630, 9.617, frame));
    CHECK_FALSE(fv_video_picture_ready(9.636, 9.630, 9.617, frame));
    CHECK_FALSE(fv_video_picture_ready(9.630, 9.636, 9.617, frame));
    CHECK(fv_video_picture_ready(1.0, 1.0, 1.0 - frame, frame));
    CHECK_FALSE(fv_video_picture_ready(1.0, 1.0, 1.0 - frame - .000001, frame));
    CHECK_FALSE(fv_video_picture_ready(1.0, 1.0, 1.0, 0.0));
    CHECK_FALSE(fv_video_picture_ready(std::numeric_limits<double>::quiet_NaN(),
        1.0, 1.0, frame));
}

TEST_CASE("A queued due picture is presented before starvation admission")
{
    double media = .099192, pcm = .110000, pts = .083333;
    const double interval = 1.0 / 60.0;
    REQUIRE_FALSE(fv_video_picture_ready(media, pcm, pts, interval));
    int advances = 0, admissions = 0;
    const bool ready = fv_video_tick_transport(false, [&]() {
        ++advances;
        media = .110000;
        pts = .100000;  // independently retained same-wall actual episode2 oracle
    }, [&]() {
        ++admissions;
        return fv_video_picture_ready(media, pcm, pts, interval);
    });
    CHECK(ready);
    CHECK(advances == 1);
    CHECK(admissions == 1);
}
TEST_CASE("A genuine queue underrun still fails post-advance admission")
{
    double media = .099192, pcm = .110000, pts = .083333;
    int advances = 0;
    CHECK_FALSE(fv_video_tick_transport(false, [&]() {
        ++advances;
        media = .110000;  // queue had no new picture; preserve old pts
    }, [&]() { return fv_video_picture_ready(media, pcm, pts, 1.0 / 60.0); }));
    CHECK(advances == 1);
}
TEST_CASE("An existing buffered seek or nonquiescent voice prevents normal advance")
{
    int advances = 0, admissions = 0;
    CHECK_FALSE(fv_video_tick_transport(true, [&]() { ++advances; }, [&]() {
        ++admissions;
        // Existing reconciler can reject preroll or quiescence even with a picture.
        return false;
    }));
    CHECK(advances == 0);
    CHECK(admissions == 1);
    bool settled = false;
    CHECK(fv_video_tick_transport(true, [&]() {
        CHECK(settled);
        ++advances;
    }, [&]() {
        settled = true;
        return true;
    }));
    CHECK(advances == 1);
}

TEST_CASE("Already consumed PCM settles a queued picture before a new pause")
{
    double video = .032251, pcm = .040000, pts = .016667;
    const double interval = 1.0 / 60.0;
    REQUIRE_FALSE(fv_video_picture_ready(video, pcm, pts, interval));
    const double delta = fv_video_consumed_settle_delta(video, pcm, interval);
    CHECK(delta > 0.0);
    CHECK(delta <= interval);
    video += delta;
    // Retained Editor25 episode2 independently observes this due queue output.
    pts = .033333;
    CHECK(fv_video_picture_ready(video, pcm, pts, interval));
}
TEST_CASE("Missing queued picture still fails the unchanged admission guard")
{
    double video = .032251, pcm = .040000, pts = .016667;
    video += fv_video_consumed_settle_delta(video, pcm, 1.0 / 60.0);
    CHECK_FALSE(fv_video_picture_ready(video, pcm, pts, 1.0 / 60.0));
    // PCM can move during presentation: rereading it must still fail closed.
    CHECK_FALSE(fv_video_picture_ready(video, .060, .033333, 1.0 / 60.0));
}
TEST_CASE("Settle never advances toward future PCM or invalid clocks")
{
    CHECK(fv_video_consumed_settle_delta(.04, .04, 1.0 / 60.0) == 0.0);
    CHECK(fv_video_consumed_settle_delta(.04, .03, 1.0 / 60.0) == 0.0);
    CHECK(fv_video_consumed_settle_delta(.04, .05, 0.0) == 0.0);
    CHECK(fv_video_consumed_settle_delta(.04,
        std::numeric_limits<double>::quiet_NaN(), .02) == 0.0);
    CHECK(fv_video_consumed_settle_delta(
        std::numeric_limits<double>::infinity(), .05, .02) == 0.0);
    CHECK(fv_video_consumed_settle_delta(.04, 9.0, .02) == .02);
    CHECK_FALSE(fv_video_picture_ready(.05, .04, .033333, 1.0 / 60.0));
}
