/*
 * test_jce_backend_contract.c
 *
 * Pure CPU tests for the render backend contract. These lock the rule that
 * backend-space semantics come from runtime caps, not shader-language macros.
 */

#include <jce/renderer/jce_lowlevel.h>

#include <math.h>
#include <stdio.h>

static int nearly(float a, float b)
{
    return fabsf(a - b) < 0.00001f;
}

static int expect_contract(const char *name,
                           JceRenderBackendContract c,
                           float depth_scale,
                           float depth_bias,
                           float shadow_flip,
                           float screen_flip)
{
    if (!nearly(c.ndc_depth_scale, depth_scale) ||
        !nearly(c.ndc_depth_bias, depth_bias) ||
        !nearly(c.shadow_uv_flip_y, shadow_flip) ||
        !nearly(c.screen_uv_flip_y, screen_flip)) {
        fprintf(stderr,
                "%s: got depth=(%.3f, %.3f) shadowFlip=%.3f screenFlip=%.3f\n",
                name,
                c.ndc_depth_scale,
                c.ndc_depth_bias,
                c.shadow_uv_flip_y,
                c.screen_uv_flip_y);
        fprintf(stderr,
                "%s: want depth=(%.3f, %.3f) shadowFlip=%.3f screenFlip=%.3f\n",
                name,
                depth_scale,
                depth_bias,
                shadow_flip,
                screen_flip);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    JceRenderBackendContract gl =
        jce_render_backend_contract_from_caps(true, true);
    fails += expect_contract("homogeneous bottom-left",
                             gl, 0.5f, 0.5f, 0.0f, 1.0f);

    JceRenderBackendContract d3d =
        jce_render_backend_contract_from_caps(false, false);
    fails += expect_contract("zero-to-one top-left",
                             d3d, 1.0f, 0.0f, 1.0f, 0.0f);

    JceRenderBackendContract mixed =
        jce_render_backend_contract_from_caps(true, false);
    fails += expect_contract("homogeneous top-left",
                             mixed, 0.5f, 0.5f, 1.0f, 0.0f);

    return fails ? 1 : 0;
}
