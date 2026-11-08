#ifndef JCE_SKY_STYLIZED_CLOUDS_SH
#define JCE_SKY_STYLIZED_CLOUDS_SH
/* Illustrated cloud lighting over the authored dome. This reuses the SAME
 * world-space density atlas, coverage and wind as the physical sky. It is
 * neither a background image nor a second procedural weather simulation.
 * No new sampler, uniform, public field, or render target.
 * Coverage zero returns the input unchanged before any texture access.
 */
/* The old atlas reader selects the nearest Z slice. In an illustrated dome
 * that produces visible horizontal terraces under a frozen camera. Interpolate
 * two adjacent slices, with periodic wrapping, ONLY for this opt-in mode.
 * X/Y filtering remains the sampler's bilinear filtering inside each tile. */
float sky_cloud_density_smooth(vec3 pointKm, float bottom, float top)
{
    float height = (pointKm.y - bottom) / (top - bottom);
    if (height <= 0.0 || height >= 1.0) return 0.0;
    float x = fract(pointKm.x * u_cloud_period.x - u_cloud_quality.y);
    float z = fract(pointKm.z * u_cloud_period.y - u_cloud_quality.z);
    float slices = max(u_cloud_atlas.z, 1.0);
    float slicePos = z * slices - 0.5;
    float slice0 = floor(slicePos);
    float z0 = (mod(slice0, slices) + 0.5) / slices;
    float z1 = (mod(slice0 + 1.0, slices) + 0.5) / slices;
    float d = mix(cloud_atlas_fetch(x, height, z0),
                  cloud_atlas_fetch(x, height, z1), fract(slicePos));
    return clamp((d - 0.02) / 0.98, 0.0, 1.0);
}

vec3 sky_stylized_clouds(vec3 background, vec3 ray, vec3 eyeKm)
{
    if (u_cloud_params.x <= 0.0 || abs(ray.y) < 0.0001)
        return background;
    float bottom = u_cloud_params.z;
    float top = max(u_cloud_params.w, bottom + 0.01);
    float a = (bottom - eyeKm.y) / ray.y;
    float b = (top - eyeKm.y) / ray.y;
    float entry = max(min(a, b), 0.0);
    float exitT = min(max(a, b), 24.0);
    if (exitT <= entry) return background;

    /* Bounded fragment work; use the existing hardware budget. No temporal
     * jitter, so a frozen camera/light yields stable comparison captures. */
    float steps = clamp(floor(u_cloud_quality.x), 8.0, 16.0);
    float stepKm = (exitT - entry) / steps;
    float extinction = max(u_cloud_params.y, 0.0) * 4.5;
    float transmittance = 1.0;
    vec3 scattered = vec3_splat(0.0);
    vec3 sun = normalize(u_sky_sun_dir.xyz + vec3(0.0, 0.00001, 0.0));
    float daylight = smoothstep(0.025, 0.18,
        dot(u_sky_colors[0].rgb, vec3(0.2126, 0.7152, 0.0722)));
    vec3 lit = mix(u_sky_colors[1].rgb * 0.22,
        mix(vec3_splat(0.96), u_sky_dome_sun_col.rgb, 0.18), daylight);
    vec3 shade = mix(u_sky_colors[1].rgb * 0.10,
        mix(u_sky_colors[1].rgb, vec3(0.63, 0.70, 0.82), 0.58), daylight);
    for (int i = 0; i < 16; ++i) {
        if (float(i) >= steps || transmittance < 0.015) break;
        vec3 pointKm = eyeKm + ray * (entry + (float(i) + 0.5) * stepKm);
        float density = sky_cloud_density_smooth(pointKm, bottom, top);
        if (density <= 0.001) continue;
        float lightDensity = sky_cloud_density_smooth(pointKm + sun * 0.18,
                                                    bottom, top);
        float height = clamp((pointKm.y - bottom) / (top - bottom), 0.0, 1.0);
        float illumination = exp(-lightDensity * 4.0);
        float tone = smoothstep(0.18, 0.95, illumination * 0.70 + height * 0.30);
        vec3 radiance = mix(shade, lit, tone);
        float alpha = 1.0 - exp(-density * extinction * stepKm);
        scattered += transmittance * alpha * radiance;
        transmittance *= 1.0 - alpha;
    }
    /* Do not accumulate a 24 km opaque wall at a nearly horizontal ray. */
    float horizonFade = smoothstep(0.035, 0.18, abs(ray.y));
    return mix(background, background * transmittance + scattered, horizonFade);
}
#endif
