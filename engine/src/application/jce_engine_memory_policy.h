/* Private boot policy; shared with native decision controls. */
#ifndef JCE_ENGINE_MEMORY_POLICY_H
#define JCE_ENGINE_MEMORY_POLICY_H

#include <jce/os/core/jce_config.h>
#include <stdbool.h>

static inline bool jce_engine_low_memory_policy(int ram_mb, int cores,
                                                JceMachineClass machine,
                                                const char *override)
{
    bool low = (ram_mb > 0 && ram_mb < 2048) || cores <= 1;
    if (machine == JCE_MACHINE_CLASS_LOW) low = true;
    else if (machine == JCE_MACHINE_CLASS_FULL) low = false;
    if (override && override[0]) low = override[0] != '0';
    return low;
}

#endif
