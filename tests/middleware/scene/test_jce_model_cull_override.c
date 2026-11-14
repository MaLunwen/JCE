/* Actual private scope/state helper. The mask is a controlled CPU input;
 * this does not execute bgfx, shaders, imported model draws or native pixels. */
#include "renderer/jce_pbr_cull_override.h"
#include <stdio.h>

/* The isolated fixture supplies only the owner's render-thread storage. */
bool jce_pbr_force_two_sided = false;
static unsigned checks, failures;
static void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        ++failures;
        fprintf(stderr, "%s\n", message);
    }
}

int main(void)
{
    const uint64_t cull = UINT64_C(3) << 36;
    const uint64_t depth_color = UINT64_C(0x0a00000001234567);
    const uint64_t original = depth_color | cull;
    check(jce_pbr_cull_override(original, cull) == original,
        "unarmed scope preserves complete original state");
    const bool outer = jce_pbr_set_two_sided_override(true);
    check(!outer, "first scope saves false");
    check(jce_pbr_cull_override(original, cull) == depth_color,
        "two-sided request removes only culling");
    check(jce_pbr_cull_override(depth_color, cull) == depth_color,
        "already two-sided state remains unchanged");

    /* Material blend/depth/write/topology bits must survive every position. */
    for (unsigned i = 0; i < 64u; ++i) {
        const uint64_t bit = UINT64_C(1) << i;
        if (!(bit & cull))
            check(jce_pbr_cull_override(bit | cull, cull) == bit,
                "an unrelated render-state bit was changed");
    }

    const bool nested = jce_pbr_set_two_sided_override(false);
    check(nested, "nested scope saves true");
    check(jce_pbr_cull_override(original, cull) == original,
        "nested ordinary draw keeps its own culling");
    jce_pbr_set_two_sided_override(nested);
    check(jce_pbr_cull_override(original, cull) == depth_color,
        "nested return restores outer two-sided request");
    jce_pbr_set_two_sided_override(outer);
    check(jce_pbr_cull_override(original, cull) == original,
        "following unrelated draw must not inherit two-sided state");
    check(!jce_pbr_force_two_sided, "final scope is restored");
    printf("model cull override: %u checks, %u failures (CPU only)\n", checks, failures);
    return failures ? 1 : 0;
}
