#ifndef JCE_SKY_CELESTIAL_SH
#define JCE_SKY_CELESTIAL_SH

uniform vec4 u_sky_celestial;     // x=stars y=moon z=solar flare w=reserved
uniform vec4 u_sky_moon_dir_phase; // xyz=toward moon, w=illuminated fraction
uniform vec4 u_sky_night_style;    // x=moon chord radius, y=night gradient strength

vec3 sky_authored_night_gradient(vec3 dir)
{
    float strength = u_sky_night_style.y;
    if (strength <= 0.0)
        return vec3_splat(0.0);

    float night = 1.0 - smoothstep(-0.28, -0.035, u_sky_sun_dir.y);
    float above_horizon = smoothstep(-0.035, 0.025, dir.y);
    float elevation = smoothstep(0.005, 0.47, dir.y);
    vec3 color = mix(vec3(0.057, 0.024, 0.10), vec3(0.009, 0.032, 0.090), elevation);
    return color * (night * strength * above_horizon);
}

bool sky_celestial_enabled()
{
    return max(max(u_sky_celestial.x, u_sky_celestial.y), u_sky_celestial.z) > 0.0;
}

/* x: solar direct scale, y: lunar direct scale, z: night ambient mix.
 * The direct fades meet at zero, so the march can choose one light without
 * interpolating opposing celestial directions. */
vec3 sky_cloud_celestial_light(float sun_elevation, out vec3 moon_direction)
{
    moon_direction = vec3_splat(0.0);
    vec3 to_moon = u_sky_moon_dir_phase.xyz;
    float moon_len2 = dot(to_moon, to_moon);
    float strength = u_sky_celestial.y;
    float phase = u_sky_moon_dir_phase.w;
    if (!(strength > 0.0 && phase >= 0.0 && moon_len2 >= 0.5 && moon_len2 <= 4.0))
        return vec3(1.0, 0.0, 0.0);

    moon_direction = to_moon * inversesqrt(moon_len2);
    float solar = smoothstep(-0.08, 0.0, sun_elevation);
    float lunar = (1.0 - smoothstep(-0.16, -0.08, sun_elevation))
                * smoothstep(0.02, 0.15, moon_direction.y);
    lunar *= 0.025 * clamp(strength, 0.0, 4.0) * clamp(phase, 0.0, 1.0);
    float ambient_night = 1.0 - smoothstep(-0.20, 0.04, sun_elevation);
    return vec3(solar, lunar, ambient_night);
}

vec3 sky_cloud_moon_tint(float elevation)
{
    return mix(vec3(0.86, 0.78, 0.70), vec3(0.55, 0.72, 1.0),
               smoothstep(0.03, 0.42, elevation));
}

vec3 sky_cloud_night_ambient()
{
    return vec3(0.014, 0.025, 0.045);
}

vec3 sky_star_seed(vec3 cell)
{
    vec3 p = fract(cell * vec3(0.1031, 0.11369, 0.13787));
    p += vec3_splat(dot(p, p.yzx + vec3_splat(19.19)));
    return fract((p.xxy + p.yzz) * p.zyx);
}

/* A jittered directional lattice gives a fixed star field across camera
 * translations and longitude wraps. Keep each filtered star within its cell. */
vec3 sky_star_radiance(vec3 dir, float angular_aa)
{
    const float frequency = 96.0;
    vec3 grid = dir * frequency + vec3(0.21, 0.47, 0.76);
    vec3 seed = sky_star_seed(floor(grid));
    float roll = fract(dot(seed, vec3(29.37, 13.71, 49.19)));
    if (roll < 0.70)
        return vec3_splat(0.0);

    vec3 offset = fract(grid) - mix(vec3_splat(0.33), vec3_splat(0.67), seed);
    float radius = mix(0.10, 0.18, seed.z);
    float width = clamp(angular_aa * frequency, 0.015, 0.14);
    float coverage = 1.0 - smoothstep(max(radius - width, 0.0),
                                      radius + width, length(offset));
    float brightness = mix(0.18, 1.25, seed.x * seed.x);
    vec3 tint = mix(vec3(1.0, 0.76, 0.60), vec3(0.68, 0.82, 1.0), seed.y);
    return tint * (brightness * coverage);
}

float sky_lunar_basin(vec2 surface_uv, vec2 center, float radius)
{
    float d = length(surface_uv - center) / radius;
    float bowl = 1.0 - smoothstep(0.28, 0.88, d);
    float rim = 1.0 - smoothstep(0.0, 0.16, abs(d - 0.87));
    return 0.13 * rim - 0.19 * bowl;
}

vec4 sky_moon_radiance(vec3 dir, vec3 sun_dir, float angular_aa)
{
    vec3 toward_moon = u_sky_moon_dir_phase.xyz;
    float phase = u_sky_moon_dir_phase.w;
    float moon_len2 = dot(toward_moon, toward_moon);
    if (phase < 0.0 || moon_len2 < 0.5)
        return vec4_splat(0.0);

    vec3 moon_dir = toward_moon * inversesqrt(moon_len2);
    vec3 moon_offset = dir - moon_dir;
    float distance2 = dot(moon_offset, moon_offset);
    bool authored_disk = u_sky_night_style.x > 0.0;
    float radius = authored_disk ? u_sky_night_style.x : 0.008;
    float halo_distance2 = authored_disk ? distance2 * 0.57 : distance2;
    if (halo_distance2 > 0.025)
        return vec4_splat(0.0);

    phase = clamp(phase, 0.0, 1.0);
    float halo = 0.14 * phase * exp2(-420.0 * halo_distance2)
               * (1.0 - smoothstep(0.01, 0.025, halo_distance2));
    float distance_to_center = sqrt(distance2);
    float edge_width = min(angular_aa, radius * 0.45);
    float disk = 1.0 - smoothstep(radius - edge_width,
                                  radius + edge_width, distance_to_center);
    float lit = 0.0;
    if (disk > 0.0 && phase > 0.0) {
        vec3 projected_sun = sun_dir - moon_dir * dot(sun_dir, moon_dir);
        /* Antipodal directions occur with the default day/night driver.
         * Give an authored partial phase a stable lit side in that case. */
        if (dot(projected_sun, projected_sun) < 0.000001) {
            vec3 axis = abs(moon_dir.y) < 0.95
                      ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
            projected_sun = cross(axis, moon_dir);
        }
        vec3 tangent_light = normalize(projected_sun);
        vec3 offset = (dir - moon_dir * dot(dir, moon_dir)) / radius;
        float facing = sqrt(max(0.0, 1.0 - dot(offset, offset)));
        float phase_cos = 2.0 * phase - 1.0;
        float phase_sin = sqrt(max(0.0, 1.0 - phase_cos * phase_cos));
        float ndotl = dot(offset, tangent_light) * phase_sin + facing * phase_cos;
        float terminator = smoothstep(-0.018, 0.018, ndotl);
        float albedo;
        BRANCH if (authored_disk) {
            vec2 face = vec2(dot(offset, tangent_light),
                             dot(offset, cross(moon_dir, tangent_light)));
            albedo = 0.74 + 0.07 * sin(dot(face, vec2(31.0, 23.0)))
                          * sin(dot(face, vec2(-15.0, 37.0)));
            albedo += sky_lunar_basin(face, vec2(0.22, -0.29), 0.42);
            albedo += sky_lunar_basin(face, vec2(-0.35, 0.34), 0.32);
            albedo += sky_lunar_basin(face, vec2(0.49, 0.32), 0.23);
            albedo += sky_lunar_basin(face, vec2(-0.48, -0.30), 0.17);
        } else {
            albedo = 0.84 + 0.08 * sin(dot(offset, vec3(16.0, 11.0, 7.0)))
                          + 0.06 * sin(dot(offset, vec3(-13.0, 17.0, 9.0)));
        }
        lit = disk * smoothstep(0.0, 0.015, phase) * terminator * albedo
            * (0.18 + 1.55 * max(ndotl, 0.0));
    }
    vec3 disk_tint = authored_disk ? vec3(0.34, 0.69, 0.96) : vec3(0.92, 0.95, 1.0);
    vec3 radiance = disk_tint * lit + vec3(0.55, 0.68, 1.0) * halo;
    return vec4(radiance, disk);
}

vec3 sky_solar_flare(vec3 dir, vec3 sun_dir, vec3 solar_transmittance,
                     float angular_aa)
{
    vec3 offset = dir - sun_dir;
    float distance2 = dot(offset, offset);
    if (distance2 > 0.065)
        return vec3_splat(0.0);

    float distance_to_center = sqrt(distance2);
    float radius = 0.0047;
    float edge_width = min(angular_aa, radius * 0.60);
    float disk = 1.0 - smoothstep(radius - edge_width,
                                  radius + edge_width, distance_to_center);
    float halo = (0.72 * exp2(-1600.0 * distance2)
                + 0.17 * exp2(-90.0 * distance2))
               * (1.0 - smoothstep(0.035, 0.065, distance2));
    float horizon = smoothstep(-0.025, 0.02, sun_dir.y)
                  * smoothstep(-0.025, 0.02, dir.y);
    return solar_transmittance * ((7.0 * disk + halo) * horizon);
}

vec3 sky_celestial_radiance(vec3 dir, vec3 solar_transmittance)
{
    vec3 sun_dir = u_sky_sun_dir.xyz;
    float angular_aa = clamp(0.6 * u_viewTexel.y, 0.00016, 0.004);
    float horizon = smoothstep(0.005, 0.15, dir.y);
    float star_weight = clamp(u_sky_celestial.x, 0.0, 8.0) * horizon
                      * (1.0 - smoothstep(-0.20, -0.045, sun_dir.y));
    float moon_weight = clamp(u_sky_celestial.y, 0.0, 8.0) * horizon
                      * (1.0 - smoothstep(-0.08, 0.08, sun_dir.y));
    vec3 radiance = vec3_splat(0.0);
    if (star_weight > 0.0)
        radiance = sky_star_radiance(dir, angular_aa) * star_weight;
    if (moon_weight > 0.0) {
        vec4 moon = sky_moon_radiance(dir, sun_dir, angular_aa);
        radiance = radiance * (1.0 - moon.w) + moon.rgb * moon_weight;
    }
    if (u_sky_celestial.z > 0.0 && sun_dir.y > -0.025)
        radiance += sky_solar_flare(dir, sun_dir, solar_transmittance, angular_aa)
                  * clamp(u_sky_celestial.z, 0.0, 8.0);
    return radiance;
}

#endif
