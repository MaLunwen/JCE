#include "jce_pick_capability_internal.h"

#include <bgfx/c99/bgfx.h>
#include <jce/os/core/jce_env.h>
#include <stdlib.h>

bool jce_pick_gpu_readback_available(void)
{
    if (jce_env_flag_enabled(getenv("JCE_DBG_PICK_CPU")))
        return false;

    const bgfx_caps_t *caps = bgfx_get_caps();
    const uint64_t required = BGFX_CAPS_TEXTURE_READ_BACK | BGFX_CAPS_TEXTURE_BLIT;
    return caps && (caps->supported & required) == required;
}
