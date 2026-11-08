/*
 * jce_env.h  Shared environment flag value policy.
 */

#ifndef JCE_ENV_H
#define JCE_ENV_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/**
 * Evaluate a borrowed value using JCE's environment flag convention.
 *
 * NULL, an empty string, and any value beginning with '0' are disabled.
 * Every other nonempty value is enabled, including "false" and "off".
 * The caller retains ownership and keeps the value readable and stable
 * for this call.
 * This pure, thread-safe helper reads the value without accessing the process
 * environment, allocating resources, or retaining state.
 *
 * @param value Borrowed NUL-terminated flag value, or NULL.
 * @return Normalized JceBool: 0 for disabled, 1 for enabled.
 */
JCE_INLINE JceBool jce_env_flag_enabled(const char *value)
{
    return (JceBool)(value && value[0] && value[0] != '0');
}

JCE_EXTERN_C_END

#endif /* JCE_ENV_H */
