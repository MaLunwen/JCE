$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_sky_transmittance, 1);
SAMPLER2D(s_sky_clouds, 2);
SAMPLER2D(s_sky_multiscatter, 3);
SAMPLER2D(s_cloud_previous_color, 7);
SAMPLER2D(s_cloud_scene_depth, 0);
SAMPLER2D(s_cloud_previous_distance, 6);
uniform vec4 u_sky_sun_dir;
uniform vec4 u_cloud_params;
uniform vec4 u_cloud_atlas;
uniform vec4 u_cloud_quality;
uniform vec4 u_cloud_period;
uniform vec4 u_cloud_temporal;
uniform vec4 u_cloud_rt_params;
uniform mat4 u_cloud_previous_view_proj;
uniform vec4 u_cloud_previous_eye;
uniform vec4 u_cloud_history_wind; // lower XZ, upper XZ in metres per frame
uniform vec4 u_cloud_layer_wind; // xy wrapped upper atlas offset, z=active
uniform vec4 u_cloud_depth_params; // x=active, y=homogeneous depth, zw=half depth texel

#include "sky_cloud_atlas.sh"
#include "sky_celestial.sh"
#define JCE_SKY_CLOUD_LAYERED 1
#include "sky_cloud_march.sh"

float cloud_geometry_limit(vec3 eye, vec3 ray, vec2 ndc)
{
    BRANCH if (u_cloud_depth_params.x < 0.5) return JCE_CLOUD_MARCH_LIMIT_KM;
    float y_sign = u_cloud_rt_params.x > 0.5 ? 0.5 : -0.5;
    vec2 uv = ndc * vec2(0.5, y_sign) + vec2_splat(0.5);
    vec2 d = u_cloud_depth_params.zw * step(1.5, u_cloud_rt_params.w);
    // Conservative for visible sky at the half-resolution silhouette:
    // a single foreground texel cannot erase clouds in neighbouring sky.
    float d0 = texture2DLod(s_cloud_scene_depth, uv - d, 0.0).r;
    float d1 = texture2DLod(s_cloud_scene_depth, uv + vec2(d.x, -d.y), 0.0).r;
    float d2 = texture2DLod(s_cloud_scene_depth, uv + vec2(-d.x, d.y), 0.0).r;
    float d3 = texture2DLod(s_cloud_scene_depth, uv + d, 0.0).r;
    float depth = max(max(d0, d1), max(d2, d3));
    // The shared clean camera prepass clears to far and uses depth LESS.
    if (!(depth >= 0.0 && depth < 1.0)) return JCE_CLOUD_MARCH_LIMIT_KM;
    float ndc_depth = mix(depth, depth * 2.0 - 1.0, u_cloud_depth_params.y);
    vec4 hit = mul(u_invViewProj, vec4(ndc, ndc_depth, 1.0));
    if (abs(hit.w) <= 1e-6) return JCE_CLOUD_MARCH_LIMIT_KM;
    return clamp(dot(hit.xyz / hit.w - eye, ray) * 0.001,
        0.0, JCE_CLOUD_MARCH_LIMIT_KM);
}

// Empty stochastic rays still advance transparency. The minimum step is
// larger than half-float transmittance rounding, so an old edge cannot stick.
float cloud_clear_history_opacity(float opacity, float weight)
{
    return max(0.0, min(opacity * weight, opacity - 0.0009765625));
}

void main()
{
#include "sky_view_ray.sh"
    float sun_elevation = clamp(u_sky_sun_dir.y, -1.0, 1.0);
    float mu = sun_elevation * 0.5 + 0.5;
    vec3 solar = texture2D(s_sky_transmittance,
        vec2((mu * 255.0 + 0.5) / 256.0, 0.5 / 64.0)).rgb;
    vec3 ambient = texture2D(s_sky_multiscatter,
        vec2((mu * 31.0 + 0.5) / 32.0, 0.5 / 32.0)).rgb;

    vec3 eye = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float distance_km;
    float cloud_rim;
    vec2 phase = u_cloud_temporal.z * vec2(59.0, 83.0);
    float geometry_km = cloud_geometry_limit(eye, dir, v_texcoord0);
    vec4 cloud = jce_cloud_march_bounded(eye * 0.001, dir, sun_elevation,
        solar, ambient, gl_FragCoord.xy + phase, geometry_km,
        u_cloud_layer_wind.xyz, distance_km, cloud_rim);

    float bottom_km = u_cloud_params.z > 0.0 ? u_cloud_params.z : 1.5;
    float top_km = u_cloud_params.w > bottom_km ? u_cloud_params.w : 4.0;
    vec2 complete_interval = cloud_shell_range(eye * 0.001, dir, bottom_km, top_km);
    // A distant opaque surface does not invalidate history for a complete
    // cloud interval in front of it. Truncated intervals remain history-free,
    // so moving foreground silhouettes cannot pull old clouds across depth.
    bool complete_cloud = geometry_km >= min(complete_interval.y, JCE_CLOUD_MARCH_LIMIT_KM);
    vec2 pixel_angle = 2.0 * u_cloud_rt_params.yz /
        max(abs(vec2(u_proj[0][0], u_proj[1][1])), vec2_splat(0.0001));
    float pixel_span = max(pixel_angle.x, pixel_angle.y);
    float camera_motion = length(eye - u_cloud_previous_eye.xyz);
    float wind_motion = max(length(u_cloud_history_wind.xy), length(u_cloud_history_wind.zw));
    // Outside the shell, motion smaller than a quarter pixel at its NEAREST
    // point bounds parallax throughout the interval. Pure camera rotation
    // reprojects exactly at any depth. Inside/cloud-clipped rays retain the
    // stricter original representative-distance and opacity rejection.
    bool stable_ray = complete_cloud && complete_interval.x > 0.1 &&
        complete_interval.y > complete_interval.x &&
        camera_motion + wind_motion < 250.0 * complete_interval.x * pixel_span;
    bool empty_ray = distance_km <= 0.0;
    BRANCH if (u_cloud_temporal.y > 0.5 && complete_cloud &&
               (distance_km > 0.0 || stable_ray))
    {
        /* Reproject a representative point inside the cloud. Subtract this
         * frame's wind advection to follow the same density feature. */
        float anchor_km = empty_ray ? 0.5 * (complete_interval.x + complete_interval.y) : distance_km;
        vec3 world = eye + dir * (1000.0 * anchor_km);
        float height = (cloud_world_altitude(world * 0.001) - bottom_km) / max(top_km - bottom_km, 0.01);
        float layer = u_cloud_layer_wind.z > 0.5 ? cloud_layer_blend(height) : 0.0;
        vec2 wind = mix(u_cloud_history_wind.xy, u_cloud_history_wind.zw, layer);
        world -= vec3(wind.x, 0.0, wind.y);
        vec4 previous_clip = mul(u_cloud_previous_view_proj, vec4(world, 1.0));
        if (previous_clip.w > 1e-3)
        {
            vec2 ndc = previous_clip.xy / previous_clip.w;
            float y_sign = u_cloud_rt_params.x > 0.5 ? 0.5 : -0.5;
            vec2 uv = vec2(ndc.x * 0.5, ndc.y * y_sign) + 0.5;
            vec2 margin = u_cloud_rt_params.yz;
            if (all(greaterThan(uv, margin)) && all(lessThan(uv, 1.0 - margin)))
            {
                float old_distance = texture2DLod(s_cloud_previous_distance, uv, 0.0).x;
                float predicted = length(world - u_cloud_previous_eye.xyz) * 0.001;
                float tolerance = max(0.12, predicted * 0.12);
                float confidence = 1.0 - smoothstep(tolerance * 0.5, tolerance,
                                                     abs(predicted - old_distance));
                vec4 old_cloud = texture2DLod(s_cloud_previous_color, uv, 0.0);
                if (stable_ray) {
                    // A miss/hit change from the stratified estimator is not
                    // a density disocclusion on this bounded world-space ray.
                    float margin_km = (camera_motion + wind_motion) * 0.001 + 0.002;
                    confidence = step(complete_interval.x - margin_km, old_distance)
                        * step(old_distance, complete_interval.y + margin_km);
                } else {
                    confidence *= 1.0 - smoothstep(0.08, 0.40, abs(old_cloud.a - cloud.a));
                    // In the transition, two features have different velocities.
                    confidence *= 1.0 - 2.0 * layer * (1.0 - layer);
                    vec3 extent = max(vec3_splat(0.08), cloud.rgb * 0.35);
                    old_cloud.rgb = clamp(old_cloud.rgb, cloud.rgb - extent, cloud.rgb + extent);
                    old_cloud.a = clamp(old_cloud.a, cloud.a - 0.16, cloud.a + 0.16);
                }
                confidence *= step(1e-4, old_distance);
                float weight = u_cloud_temporal.w * confidence;
                if (empty_ray) {
                    float opacity = max(1.0 - old_cloud.a, 0.0);
                    float retained = cloud_clear_history_opacity(opacity, weight);
                    cloud = vec4(old_cloud.rgb * (retained / max(opacity, 1e-6)), 1.0 - retained);
                    distance_km = retained > 0.0 ? old_distance : 0.0;
                } else {
                    cloud = mix(cloud, old_cloud, weight);
                }
            }
        }
    }

    gl_FragData[0] = cloud;
    // A truncated ray may still contribute this frame, but cannot become
    // next frame's complete-cloud history after a depth disocclusion.
    gl_FragData[1] = vec4(complete_cloud ? distance_km : 0.0, 0.0, 0.0, 1.0);
}
