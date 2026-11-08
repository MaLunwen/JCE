$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_cloud_scene_depth, 0);
SAMPLER2D(s_sky_transmittance, 1);
SAMPLER2D(s_sky_clouds, 2);
SAMPLER2D(s_sky_multiscatter, 3);
SAMPLER2D(s_sky_cloud_resolved, 7);
uniform vec4 u_sky_sun_dir;
uniform vec4 u_sky_params;
uniform vec4 u_cloud_params;
uniform vec4 u_cloud_atlas;
uniform vec4 u_cloud_quality;
uniform vec4 u_cloud_period;
uniform vec4 u_cloud_temporal;
uniform vec4 u_cloud_rt_params;
uniform vec4 u_cloud_layer_wind;
uniform vec4 u_cloud_depth_params;

#include "sky_preetham_math.sh"
#include "sky_stylise.sh"
#include "sky_cloud_atlas.sh"
#include "sky_celestial.sh"
#define JCE_SKY_CLOUD_LAYERED 1
#include "sky_cloud_march.sh"

void main()
{
    float y_sign = u_cloud_rt_params.x > 0.5 ? 0.5 : -0.5;
    vec2 uv = v_texcoord0 * vec2(0.5, y_sign) + vec2_splat(0.5);
    float depth = texture2DLod(s_cloud_scene_depth, uv, 0.0).r;
    // Sky already received the same cloud layer in its background pass.
    // Only an opaque depth pixel can have overwritten that background.
    if (!(depth >= 0.0 && depth < 1.0)) discard;

#include "sky_view_ray.sh"
    vec3 eye = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float ndc_depth = mix(depth, depth * 2.0 - 1.0, u_cloud_depth_params.y);
    vec4 hit = mul(u_invViewProj, vec4(v_texcoord0, ndc_depth, 1.0));
    if (abs(hit.w) <= 1e-6) discard;
    float geometry_km = max(dot(hit.xyz / hit.w - eye, dir) * 0.001, 0.0);
    float bottom = u_cloud_params.z > 0.0 ? u_cloud_params.z : 1.5;
    float top = u_cloud_params.w > bottom ? u_cloud_params.w : 4.0;
    vec2 interval = cloud_shell_range(eye * 0.001, dir, bottom, top);
    if (interval.y <= interval.x || geometry_km <= interval.x) discard;

    vec4 cloud = texture2DLod(s_sky_cloud_resolved, uv, 0.0);
    BRANCH if (geometry_km < interval.y) {
        // A surface inside the cloud needs its exact full-resolution bound.
        // The conservative half-resolution producer can contain deeper sky
        // from a neighbour. Do not project that extra cloud onto this surface.
        float sun_elevation = clamp(u_sky_sun_dir.y, -1.0, 1.0);
        float mu = sun_elevation * 0.5 + 0.5;
        vec3 solar = texture2DLod(s_sky_transmittance,
            vec2((mu * 255.0 + 0.5) / 256.0, 0.5 / 64.0), 0.0).rgb;
        vec3 ambient = texture2DLod(s_sky_multiscatter,
            vec2((mu * 31.0 + 0.5) / 32.0, 0.5 / 32.0), 0.0).rgb;
        float distance_km, rim;
        vec2 phase = u_cloud_temporal.z * vec2(59.0, 83.0);
        cloud = jce_cloud_march_bounded(eye * 0.001, dir, sun_elevation,
            solar, ambient, gl_FragCoord.xy + phase, geometry_km,
            u_cloud_layer_wind.xyz, distance_km, rim);
    }
    float transmittance = clamp(cloud.a, 0.0, 1.0);
    float rim = clamp(4.0 * transmittance * (1.0 - transmittance), 0.0, 1.0);
    vec3 scatter = sky_stylise(max(cloud.rgb, vec3_splat(0.0)) *
        (u_sky_params.y * JCE_SKY_SCALE), rim);
    // ONE/SRC_ALPHA blends premultiplied in-scatter over opaque radiance.
    gl_FragColor = vec4(scatter, transmittance);
}
