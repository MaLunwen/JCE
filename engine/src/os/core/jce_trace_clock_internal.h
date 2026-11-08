/* Process-stable trace timestamp conversion; no SDL lifetime-relative epoch. */
#ifndef JCE_TRACE_CLOCK_INTERNAL_H
#define JCE_TRACE_CLOCK_INTERNAL_H
#include <jce/os/core/jce_timer.h>

/* Exact floor(counter * 1e9 / frequency), refusing unrepresentable clocks.
 * Normal hardware frequencies use one bounded remainder multiplication. The
 * rare wide-frequency path divides the product incrementally without uint128,
 * floating-point rounding, overflow, allocation or an epoch/reset registry. */
static inline bool jce_trace_clock_convert_internal(uint64_t counter,
                                                    uint64_t frequency,
                                                    uint64_t *nanoseconds)
{
    const uint64_t scale = UINT64_C(1000000000);
    if (!frequency || !nanoseconds) return false;
    const uint64_t whole = counter / frequency;
    const uint64_t remainder = counter % frequency;
    if (whole > UINT64_MAX / scale) return false;
    uint64_t fraction;
    if (remainder <= UINT64_MAX / scale) {
        fraction = remainder * scale / frequency;
    } else {
        uint64_t residue = 0;
        fraction = 0;
        for (uint32_t bit = UINT32_C(1) << 29; bit; bit >>= 1) {
            fraction *= 2;
            if (residue >= frequency - residue) {
                residue -= frequency - residue;
                ++fraction;
            } else {
                residue += residue;
            }
            if (scale & bit) {
                if (residue >= frequency - remainder) {
                    residue -= frequency - remainder;
                    ++fraction;
                } else {
                    residue += remainder;
                }
            }
        }
    }
    const uint64_t integral = whole * scale;
    if (fraction > UINT64_MAX - integral) return false;
    *nanoseconds = integral + fraction;
    return true;
}

static inline bool jce_trace_clock_now_internal(uint64_t *nanoseconds)
{
    const uint64_t frequency = jce_time_perf_freq();
    const uint64_t counter = jce_time_perf_counter();
    return jce_trace_clock_convert_internal(counter, frequency, nanoseconds);
}
#endif
