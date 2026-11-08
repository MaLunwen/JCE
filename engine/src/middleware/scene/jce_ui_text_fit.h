/* Best Fit uses one measured atlas for every candidate and the final draw. */
#ifndef JCE_UI_TEXT_FIT_H
#define JCE_UI_TEXT_FIT_H
#include <math.h>

static inline int uc_text_fit_size(int lower, int upper, int atlas_px,
                                   float measured_width, float measured_height,
                                   float available_width, float available_height)
{
    if (lower < 1) lower = 1;
    if (upper < lower) upper = lower;
    if (atlas_px < 1 || !isfinite(measured_width) ||
        !isfinite(measured_height) || !isfinite(available_width) ||
        !isfinite(available_height) || measured_width < 0.0f ||
        measured_height < 0.0f) return lower;
    int best = lower;
    while (lower <= upper) {
        int middle = lower + (upper - lower) / 2;
        float scale = (float)middle / (float)atlas_px;
        if (measured_width * scale <= available_width &&
            measured_height * scale <= available_height) {
            best = middle;
            if (middle == upper) break;
            lower = middle + 1;
        } else {
            upper = middle - 1;
        }
    }
    return best;
}
#endif
