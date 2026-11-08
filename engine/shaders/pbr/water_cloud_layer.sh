#ifndef JCE_WATER_CLOUD_LAYER_SH
#define JCE_WATER_CLOUD_LAYER_SH

#include "../standard/sky_cloud_geometry.sh"
#include "../standard/sky_preetham_math.sh"
#include "../standard/sky_stylise.sh"

vec4 water_cloud_composite(vec4 surface, vec3 scatter, float transmittance, bool opaque_background)
{
    if (opaque_background) {
        // The opaque receiver overwrote the sky, so the cloud must cover
        // both water and its still-visible background. Convert the composed
        // premultiplied radiance back to the draw's straight-alpha contract.
        float coverage = 1.0 - transmittance * (1.0 - surface.a);
        vec3 radiance = surface.rgb * (surface.a * transmittance) + scatter;
        return vec4(radiance / max(coverage, 1e-5), coverage);
    }
    // Clear depth still contains the sky's cloud. Its in-scatter is present
    // in both source and background and the alpha blend contributes it once.
    return vec4(surface.rgb * transmittance + scatter, surface.a);
}

// Stage 8 belongs to either planar reflection or the current cloud layer.
// Negative mode encodes originBottomLeft: -1 top-left, -2 bottom-left.
// Modes -3/-4 additionally certify a submitted cloud pass over opaque pixels.
// y is the owning sky exposure, zw are its lower/upper shell altitudes.
vec4 water_cloud_layer(vec4 surface, vec3 world, vec3 eye, vec2 pixel_xy)
{
    BRANCH if (u_water_planar.x >= -0.5) return surface;
    if (!cloud_shell_before_surface(eye * 0.001, world * 0.001,
                                   u_water_planar.z, u_water_planar.w))
        return surface;

    vec4 clip = mul(u_water_planar_vp, vec4(world, 1.0));
    if (clip.w <= 1e-4) return surface;
    bool opaque_composited = u_water_planar.x < -2.5;
    float origin_bottom_left = -u_water_planar.x - (opaque_composited ? 3.0 : 1.0);
    float y_sign = origin_bottom_left > 0.5 ? 0.5 : -0.5;
    vec2 uv = clip.xy / clip.w * vec2(0.5, y_sign) + vec2_splat(0.5);
    if (any(lessThan(uv, vec2_splat(0.0))) || any(greaterThan(uv, vec2_splat(1.0))))
        return surface;
    vec4 cloud = texture2DLod(s_water_planar, uv, 0.0);
    float transmittance = clamp(cloud.a, 0.0, 1.0);
    float rim = clamp(4.0 * transmittance * (1.0 - transmittance), 0.0, 1.0);
    vec3 scatter = sky_stylise(max(cloud.rgb, vec3_splat(0.0)) *
        (u_water_planar.y * JCE_SKY_SCALE), rim);
    vec2 depth_uv = pixel_xy * u_water_depth_params.zw;
    float background_depth = texture2DLod(s_water_depth, depth_uv, 0.0).r;
    return water_cloud_composite(surface, scatter, transmittance,
        background_depth < 1.0 && !opaque_composited);
}

#endif
