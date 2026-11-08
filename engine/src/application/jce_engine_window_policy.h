/*
 * jce_engine_window_policy.h  Private bootstrap window defaults.
 *
 * Seed application defaults before loading user configuration. Window
 * creation uses the resolved configuration without changing restore bounds.
 */
#ifndef JCE_ENGINE_WINDOW_POLICY_H
#define JCE_ENGINE_WINDOW_POLICY_H

#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_env.h>

#include <limits.h>

static inline void jce_engine_window_policy_seed(JceConfig *cfg,
                                                const JceAppDesc *desc)
{
    if (!cfg || !desc) return;

    /* JceConfig and the window adapter use positive int dimensions. */
    if (desc->window_width > 0 &&
        (uint64_t)desc->window_width <= (uint64_t)INT_MAX)
        cfg->window_width = (int)desc->window_width;
    if (desc->window_height > 0 &&
        (uint64_t)desc->window_height <= (uint64_t)INT_MAX)
        cfg->window_height = (int)desc->window_height;

    /* false is a real default, not an unspecified boolean. There are no
     * fullscreen or resizable members in the application descriptor. */
    cfg->maximized = desc->maximized;
}

static inline bool jce_engine_window_policy_maximized(const JceConfig *cfg)
{
    /* Fullscreen owns the created surface; retain the saved windowed mode
     * and restore dimensions in cfg for later persistence. */
    return cfg && cfg->maximized && !cfg->fullscreen;
}

/* Explicit opt-in only. Headless creates no window; shared environment bool
 * rules preserve leading-zero/empty behavior without a second parser. */
static inline bool jce_engine_window_policy_not_focusable(bool headless,
                                                          const char *value)
{
    return !headless && jce_env_flag_enabled(value) != 0;
}

/* Modal errors must honor the already resolved bootstrap UI request, even
 * before a window or graphics device exists. This is not a new launch flag. */
static inline bool jce_engine_window_policy_error_dialog(bool headless,
    bool hidden, bool not_focusable)
{
    return !headless && !hidden && !not_focusable;
}

#endif /* JCE_ENGINE_WINDOW_POLICY_H */
