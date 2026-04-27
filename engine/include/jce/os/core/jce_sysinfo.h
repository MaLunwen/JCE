/*
 * jce_sysinfo.h  Cross-platform system information for debug HUD.
 *
 * Static info (CPU cores, total RAM) is collected once at init.
 * Dynamic info (process RAM, CPU usage) is updated periodically.
 */

#ifndef JCE_SYSINFO_H
#define JCE_SYSINFO_H


#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSysInfo {
    /* Static (collected once at init). */
    int    cpu_cores;       /* Logical CPU core count. */
    int    ram_total_mb;    /* Total system RAM in MB. */

    /* Dynamic (updated by jce_sysinfo_update). */
    int    ram_used_mb;     /* Process resident set size in MB. */
    float  cpu_usage;       /* Process CPU usage 0-100%. */

    /* Internal state for CPU usage calculation. */
    uint64_t _prev_time;
    uint64_t _prev_kernel;
    uint64_t _prev_user;
} JceSysInfo;

/* Collect static info and initialize internal state. */
JCE_API void jce_sysinfo_init(JceSysInfo *info);

/* Update dynamic info. Call roughly once per second. */
JCE_API void jce_sysinfo_update(JceSysInfo *info);

JCE_EXTERN_C_END

#endif /* JCE_SYSINFO_H */
