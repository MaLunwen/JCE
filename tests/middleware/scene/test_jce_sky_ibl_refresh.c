/* Actual private IBL admission owner; no window, GPU or worker execution. */
#include "middleware/scene/jce_sr_ibl_refresh.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)

int main(void)
{
    SrSkyIblSnapshot s = {0};
    float sun[3] = {0.0f, 1.0f, 0.0f};
    const float ambient = 0.28543746f; /* not representable by %.4f */
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    sr_sky_ibl_snapshot_set(&s, sun, 2.0037f, ambient);
    CHECK(!sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    CHECK(s.ambient == ambient && s.turbidity == 2.0037f);
    for (int tenth = 1; tenth <= 19; ++tenth) {
        const float radians = (float)tenth * 0.001745329252f;
        sun[0] = sinf(radians); sun[1] = cosf(radians);
        CHECK(!sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    }
    sun[0] = sinf(2.1f * 0.01745329252f);
    sun[1] = cosf(2.1f * 0.01745329252f);
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    sr_sky_ibl_snapshot_set(&s, sun, 2.0037f, ambient);
    for (int axis = 0; axis < 3; ++axis) sun[axis] *= 0.37f;
    CHECK(!sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.01f, ambient));
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient + 0.00001f));
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, 0.0f));
    s.valid = false; /* queue/bake/upload failure or HDR transition */
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    sr_sky_ibl_snapshot_set(&s, sun, 2.0037f, ambient);
    sun[0] = sun[1] = sun[2] = 0.0f;
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    sun[0] = NAN;
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));
    sun[0] = INFINITY;
    CHECK(sr_sky_ibl_refresh_needed(&s, sun, 2.0037f, ambient));

    /* 120 small steps must accumulate against the last admitted direction,
     * not the previous frame. Exact scalar edits remain immediately visible. */
    memset(&s, 0, sizeof(s));
    unsigned admitted = 0;
    for (int step = 0; step < 120; ++step) {
        const float angle = (float)step * 0.1f * 0.01745329252f;
        sun[0] = sinf(angle); sun[1] = cosf(angle); sun[2] = 0.0f;
        if (sr_sky_ibl_refresh_needed(&s, sun, 2.0f, ambient)) {
            sr_sky_ibl_snapshot_set(&s, sun, 2.0f, ambient);
            ++admitted;
        }
    }
    CHECK(admitted >= 5 && admitted <= 6);
    printf("sky IBL numeric admission: %u checks, %u failures, %u/120 admissions (CPU only)\n",
           checks, failures, admitted);
    return failures ? 1 : 0;
}
