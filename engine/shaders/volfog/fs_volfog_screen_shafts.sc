$input v_texcoord0

#include <bgfx_shader.sh>
#include "screen_reconstruct.sh"

SAMPLER2D(s_depth, 0);
SAMPLER2D(s_volfog_cloud_shadow, 1);
uniform vec4 u_screenShaftSource; // xy sun NDC, z far depth, w aspect
uniform vec4 u_volfog_cloud_map;  // extent, center X/Z, cloud base Y
uniform vec4 u_volfog_cloud_ray;  // published bake sun XZ/Y

float shaft_cloud_visibility(vec2 uv, vec3 camera_pos)
{
    if (u_volfog_cloud_map.x <= 0.0 || camera_pos.y >= u_volfog_cloud_map.w)
        return 1.0;

    vec3 ray = jce_uv_depth_to_world(u_invViewProj, uv, 0.5) - camera_pos;
    if (ray.y <= 1e-5) return 1.0;

    float base_y = u_volfog_cloud_map.w;
    vec2 ground = camera_pos.xz + ray.xz * ((base_y - camera_pos.y) / ray.y)
                - base_y * u_volfog_cloud_ray.xy;
    vec2 projected = (ground - u_volfog_cloud_map.yz) / u_volfog_cloud_map.x + 0.5;
    if (any(lessThan(projected, vec2_splat(0.0))) ||
        any(greaterThan(projected, vec2_splat(1.0)))) return 1.0;
#if BGFX_SHADER_LANGUAGE_HLSL
    return texture2DLod(s_volfog_cloud_shadow, projected, 0.0).r;
#else
    return texture2D(s_volfog_cloud_shadow, projected).r;
#endif
}

void main()
{
    vec2 sun_uv = jce_clip_to_uv(vec4(u_screenShaftSource.xy, 0.0, 1.0));
    vec2 screen_uv = v_texcoord0;
#if BGFX_SHADER_LANGUAGE_GLSL
    /* The shared quad carries top-left UV even on GL framebuffers. */
    screen_uv.y = 1.0 - screen_uv.y;
#endif
    vec2 direction = sun_uv - screen_uv;
    float span = length(direction * vec2(u_screenShaftSource.w, 1.0));
    float envelope = 1.0 - smoothstep(0.12, 1.25, span);
    float sum = 0.0;
    float total = 0.0;
    float weight = 1.0;
    vec3 camera_pos = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
#if BGFX_SHADER_LANGUAGE_HLSL
    [loop]
#endif
    for (int i = 0; i < 12; ++i) {
        float t = (float(i) + 0.5) * (1.0 / 12.0);
        vec2 uv = screen_uv + direction * t;
        total += weight;
        if (all(greaterThanEqual(uv, vec2_splat(0.0))) &&
            all(lessThanEqual(uv, vec2_splat(1.0)))) {
#if BGFX_SHADER_LANGUAGE_HLSL
            float depth = texture2DLod(s_depth, uv, 0.0).r;
#else
            float depth = texture2D(s_depth, uv).r;
#endif
            /* Depth-tested geometry blocks the sky emitter at this tap.
             * On the remaining sky, use the resident projected cloud field. */
            if (depth == u_screenShaftSource.z)
                sum += weight * shaft_cloud_visibility(uv, camera_pos);
        }
        weight *= 0.88;
    }
    float shaft = envelope * clamp(sum / total, 0.0, 1.0);
    gl_FragColor = vec4(shaft, 0.0, 0.0, 1.0);
}
