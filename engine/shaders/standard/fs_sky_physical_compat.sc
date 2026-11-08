$input v_texcoord0

#include <bgfx_shader.sh>

/* Desktop GL2.1 physical fallback uses the analytic base and two density taps.
 * Ordinary sky modes have their own program. */
uniform vec4 u_sky_params;    // y = exposure; w = 2 when cloud atlas is ready
uniform vec4 u_sky_perez[4];
uniform vec4 u_sky_zenith;
uniform vec4 u_sky_sun_dir;
uniform vec4 u_sky_colors[3]; // zenith, horizon, ground

SAMPLER2D(s_sky_clouds, 2);
uniform vec4 u_cloud_params;   // x=coverage y=density z=bottom_km w=top_km
uniform vec4 u_cloud_atlas;    // x=dim_x y=dim_y z=dim_z w=tiles_x
uniform vec4 u_cloud_quality;  // yz=wind offset (tile units)
uniform vec4 u_cloud_period;   // xy=reciprocal periods per kilometre

#include "sky_preetham_math.sh"
#include "sky_cloud_atlas.sh"
#include "sky_stylise.sh"
#include "sky_celestial.sh"

vec3 sky_gl21_base(vec3 radiance, vec3 dir)
{
    radiance *= JCE_SKY_SCALE;
    /* Comparisons also reject NaN on the GLSL 1.20 path. The analytic
     * approximation can produce unusably large radiance near the horizon. */
    if (!all(greaterThanEqual(radiance, vec3_splat(0.0))) ||
        !all(lessThanEqual(radiance, vec3_splat(32.0)))) {
        float elevation = clamp(dir.y, -1.0, 1.0);
        vec3 sky = mix(u_sky_colors[1].rgb, u_sky_colors[0].rgb, max(elevation, 0.0));
        vec3 ground = mix(sky, u_sky_colors[2].rgb, max(-elevation, 0.0));
        radiance = clamp(ground, vec3_splat(0.0), vec3_splat(1.0)) * JCE_SKY_SCALE;
    }

    /* Compress the RGB peak as a unit. Clear sky stays below 0.52 linear
     * while retaining its hue and leaving room for cloud highlights. */
    const float knee = 0.24;
    const float headroom = 0.28;
    float peak = max(radiance.r, max(radiance.g, radiance.b));
    if (peak <= knee)
        return radiance;
    float excess = peak - knee;
    float compressed = knee + headroom * excess / (headroom + excess);
    return radiance * (compressed / peak);
}

float cloud_gl21_sample(vec2 world_xz_km, float height)
{
    vec2 uv = fract(world_xz_km * u_cloud_period.xy - u_cloud_quality.yz);
    float density = cloud_atlas_fetch(uv.x, height, uv.y);
    return clamp((density - 0.02) / 0.98, 0.0, 1.0);
}

vec3 cloud_gl21_apply(vec3 sky, vec3 dir, out float transmittance)
{
    transmittance = 1.0;
    if (dir.y <= 0.02)
        return sky;

    float bottom = u_cloud_params.z > 0.0 ? u_cloud_params.z : 1.5;
    float top = u_cloud_params.w > bottom ? u_cloud_params.w : max(4.0, bottom + 0.01);
    float density = u_cloud_params.y > 0.0 ? u_cloud_params.y : 1.0;
    vec2 camera_xz = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xz * 0.001;
    vec2 ray_xz = dir.xz / dir.y;

    /* Two fixed interior samples integrate the authored slab approximately.
     * Coverage already carves the baked field. Wind advects the same atlas
     * in world space, independently of the camera and without rebaking it. */
    float low = cloud_gl21_sample(camera_xz + ray_xz * mix(bottom, top, 0.3), 0.3);
    float high = cloud_gl21_sample(camera_xz + ray_xz * mix(bottom, top, 0.7), 0.7);
    float mean = 0.5 * (low + high);
    if (mean <= 0.0)
        return sky;
    float optical_depth = mean * density * min((top - bottom) / dir.y, 20.0);
    float opacity = (1.0 - exp(-optical_depth)) * smoothstep(0.02, 0.15, dir.y);

    /* The analytic sky supplies the ambient tint. A modest daylight lobe
     * follows the scene's sun. The existing grade can tint the cloud edge. */
    float day = smoothstep(-0.08, 0.3, u_sky_sun_dir.y);
    float toward_sun = max(dot(dir, u_sky_sun_dir.xyz), 0.0);
    vec3 sunlight = vec3_splat(0.55) + vec3(0.22, 0.15, 0.08) * toward_sun;
    vec3 cloud = mix(sky * 0.55, sunlight + sky * 0.25, day);
    vec3 moon_direction;
    vec3 lighting = sky_cloud_celestial_light(u_sky_sun_dir.y, moon_direction);
    if (lighting.x < 1.0 || lighting.z > 0.0) {
        float mean_height = (0.3 * low + 0.7 * high) / (low + high);
        vec3 night_ambient = sky_cloud_night_ambient() * mix(0.15, 0.70, mean_height)
                           * (JCE_SKY_SCALE * u_sky_params.y);
        vec3 ambient = mix(sky * 0.55, sky * 0.25, day);
        cloud = mix(ambient, night_ambient, lighting.z) + sunlight * (day * lighting.x);
    }
    if (lighting.y > 0.0) {
        float toward_moon = max(dot(dir, moon_direction), 0.0);
        float lobe = 1.0 + 6.0 * pow(toward_moon, 12.0);
        cloud += sky_cloud_moon_tint(moon_direction.y) * lobe * lighting.y
               * (JCE_SKY_SCALE * u_sky_params.y);
    }
    float rim = 4.0 * opacity * (1.0 - opacity);
    vec3 graded = sky_stylise(cloud, rim);
    float peak = max(graded.r, max(graded.g, graded.b));
    graded *= 0.82 / max(0.82, peak);
    transmittance = 1.0 - opacity;
    return mix(sky, graded, opacity);
}

void main()
{
#include "sky_view_ray.sh"
    {
#include "sky_preetham_body.sh"
        col = sky_gl21_base(col, dir);
        float cloud_transmittance = 1.0;
        if (u_sky_params.w > 1.5)
            col = cloud_gl21_apply(col, dir, cloud_transmittance);
        if (sky_celestial_enabled()) {
            /* The compatibility path has no atmosphere LUT. Approximate
             * its solar extinction using the analytic sun elevation. */
            vec3 solar_transmittance = mix(vec3(0.48, 0.22, 0.08),
                                           vec3(0.93, 0.89, 0.81),
                                           smoothstep(0.0, 0.50, u_sky_sun_dir.y));
            col += sky_celestial_radiance(dir, solar_transmittance)
                 * (u_sky_params.y * cloud_transmittance);
        }
        gl_FragColor = vec4(col, 1.0);
    }
}
