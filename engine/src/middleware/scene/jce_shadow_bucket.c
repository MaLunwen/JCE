#include "jce_shadow_bucket.h"

#include <stdlib.h>

uint32_t jce_shadow_bucket_key(JceShadowBucket bucket)
{
    /* Widely spaced rather than 0/1: bgfx packs this into a sort key beside
     * other fields, and leaving room means a future third bucket (decals,
     * two-sided masked) can be inserted between these without renumbering the
     * two that exist. */
    return (bucket == JCE_SHADOW_BUCKET_MASKED) ? 0x8000u : 0x0000u;
}

bool jce_shadow_masked_last_enabled(void)
{
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char *v = getenv("JCE_SHADOW_MASKED_LAST");
        s_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return s_enabled != 0;
}
