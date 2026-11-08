/* Host presentation is copied independently of transient pointer ownership. */
#include "jce_rt_internal.h"
#include <math.h>
#include <string.h>

static bool viewport_dimensions_valid(float width, float height)
{
    return isfinite(width) && isfinite(height) &&
        ((width == 0.0f && height == 0.0f) ||
         (width > 0.0f && height > 0.0f && width <= 65535.0f && height <= 65535.0f));
}

JCE_API bool JCE_CALL jce_runtime_set_viewport_size(JceRuntime *rt, float width, float height)
{
    if (!rt) return false;
    const float old_width = rt->viewport_wh[0], old_height = rt->viewport_wh[1];
    rt->viewport_wh[0] = rt->viewport_wh[1] = 0.0f;
    if (!viewport_dimensions_valid(width, height)) {
        if (!rt->viewport_invalid_reported) {
            rt->viewport_invalid_reported = true;
            LOG_WARN(LOG_TAG, "viewport size rejected owner=%p width=%g height=%g reason=invalid_dimensions",
                (void *)rt, (double)width, (double)height);
        }
        if (old_width != 0.0f || old_height != 0.0f) rt_script_viewport_changed(rt);
        return false;
    }
    rt->viewport_wh[0] = width;
    rt->viewport_wh[1] = height;
    if (old_width != width || old_height != height) rt_script_viewport_changed(rt);
    return true;
}

bool rt_viewport_initialize(JceRuntime *rt, struct JcePlatformServices *services,
    const JceRuntimeCreateOptions *options, size_t options_size)
{
    JceRuntimeCreateOptions initial = {0};
    if ((!options && options_size) || (options_size && options_size < sizeof initial)) {
        LOG_ERROR(LOG_TAG, "runtime creation rejected reason=invalid_viewport_options_size bytes=%zu minimum=%zu",
            options_size, sizeof initial);
        return false;
    }
    if (options && options_size) memcpy(&initial, options, sizeof initial);
    if (!viewport_dimensions_valid(initial.viewport_width, initial.viewport_height)) {
        LOG_ERROR(LOG_TAG, "runtime creation rejected reason=invalid_initial_viewport width=%g height=%g",
            (double)initial.viewport_width, (double)initial.viewport_height);
        return false;
    }
    rt->platform_services = services;
    return jce_runtime_set_viewport_size(rt, initial.viewport_width, initial.viewport_height);
}

void rt_script_viewport_size(void *user, float out_wh[2])
{
    const JceRuntime *rt = user;
    if (!out_wh) return;
    out_wh[0] = rt ? rt->viewport_wh[0] : 0.0f;
    out_wh[1] = rt ? rt->viewport_wh[1] : 0.0f;
}
