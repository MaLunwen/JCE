/* Smooth a block-updated audio cursor without resampling or changing rate.
 * Device callbacks advance in batches; snapping a video deadline to each batch
 * repeats and skips pictures even with a full decoder queue.
 */
#ifndef JCE_VIDEO_CLOCK_H
#define JCE_VIDEO_CLOCK_H

static inline double jce_video_clock_follow_audio(double previous, double dt,
                                                  double audio_time)
{
    double target, correction, limit, ceiling;
    if (dt <= 0.0) return previous;
    target = previous + dt;
    correction = audio_time - target;
    if (correction > 0.050) {
        target = previous + dt * 2.0;
    } else {
        limit = dt * 0.05;
        if (correction > limit) correction = limit;
        if (correction < -limit) correction = -limit;
        target += correction;
    }
    /* Interpolate short callback gaps, but never run through an audio stall.
     * Two 10 ms output periods bound the permitted lead. Never rewind a frame.
     */
    ceiling = audio_time + 0.020;
    if (ceiling < previous) ceiling = previous;
    if (target > ceiling) target = ceiling;
    if (target < previous) target = previous;
    return target;
}

#endif
