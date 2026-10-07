#ifndef JCE_FV_VIDEO_POLICY_H
#define JCE_FV_VIDEO_POLICY_H
#include <cmath>

/* Both the interpolated video deadline and consumed PCM constrain admission. */
static inline bool fv_video_picture_ready(double video, double pcm,
                                         double pts, double interval)
{
    return std::isfinite(video) && std::isfinite(pcm) && std::isfinite(pts)
        && std::isfinite(interval) && interval > 0.0
        && (video > pcm ? video : pcm) <= pts + interval;
}
/* Presentation may catch up only to PCM already consumed by the device.
 * Never predict the next PCM block or advance more than one picture interval. */
static inline double fv_video_consumed_settle_delta(double video, double pcm,
                                                    double interval)
{
    if (!std::isfinite(video) || !std::isfinite(pcm) ||
        !std::isfinite(interval) || interval <= 0.0 || pcm <= video)
        return 0.0;
    const double lead = pcm - video;
    return lead < interval ? lead : interval;
}
/* Ordinary playback must present due output before judging starvation.
 * An existing buffer episode must settle/quiesce before any normal advance. */
template <typename Advance, typename Reconcile>
static inline bool fv_video_tick_transport(bool buffering, Advance advance,
                                           Reconcile reconcile)
{
    if (buffering && !reconcile()) return false;
    advance();
    return reconcile();
}
#endif
