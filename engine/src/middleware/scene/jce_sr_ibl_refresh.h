/* Private procedural-sky IBL admission. Log formatting is not cache state. */
#ifndef JCE_SR_IBL_REFRESH_H
#define JCE_SR_IBL_REFRESH_H

#include <stdbool.h>
#include <math.h>

typedef struct SrSkyIblSnapshot {
    float sun[3];
    float turbidity;
    float ambient;
    bool valid;
} SrSkyIblSnapshot;

static inline bool sr_sky_ibl_refresh_needed(const SrSkyIblSnapshot *last,
                                            const float sun[3],
                                            float turbidity, float ambient)
{
    if (!last->valid || last->turbidity != turbidity || last->ambient != ambient)
        return true;
    /* Normalize in the comparison: rounded/non-unit directions must not
     * turn a two-degree angular threshold into a vector-length threshold. */
    double dot = 0.0, old_len2 = 0.0, new_len2 = 0.0;
    for (int i = 0; i < 3; ++i) {
        dot += (double)last->sun[i] * sun[i];
        old_len2 += (double)last->sun[i] * last->sun[i];
        new_len2 += (double)sun[i] * sun[i];
    }
    if (!(old_len2 > 0.0) || !(new_len2 > 0.0) || !isfinite(dot) ||
        !isfinite(old_len2) || !isfinite(new_len2)) return true;
    /* cos(2 degrees); retain exact authored scalar changes. */
    return !(dot / sqrt(old_len2 * new_len2) > 0.9993908270190958);
}

static inline void sr_sky_ibl_snapshot_set(SrSkyIblSnapshot *last,
                                          const float sun[3],
                                          float turbidity, float ambient)
{
    for (int i = 0; i < 3; ++i) last->sun[i] = sun[i];
    last->turbidity = turbidity;
    last->ambient = ambient;
    last->valid = true;
}

#endif
