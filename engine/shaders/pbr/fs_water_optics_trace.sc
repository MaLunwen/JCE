$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>
#include "screen_reconstruct.sh"
#include "water_disturbance.sh"

SAMPLER2D(s_water_optics_depth, 2);
SAMPLER2D(s_water_optics_opaque, 3);
SAMPLER2D(s_water_gradient, 14);

uniform vec4 u_cameraPos;
uniform vec4 u_ambientColor;
uniform vec4 u_dirLights[4];
uniform vec4 u_lightCounts;
uniform vec4 u_water_mode;
uniform vec4 u_water_params;
uniform vec4 u_water_shading;
uniform vec4 u_water_time;
uniform vec4 u_water_normal_detail; // low/high 1/m, slope strength, FFT phase
uniform vec4 u_weatherSurface;
uniform vec4 u_water_absorb;
uniform vec4 u_water_absorb_tint;
uniform vec4 u_water_optics0; // SSR strength, refraction strength, water IOR, max distance
uniform vec4 u_water_optics1; // camera near, camera far, hit thickness, march steps
uniform vec4 u_water_optics_sky; // submitted sky, inverse viewport width/height, reserved

#include "water_surface_normal.sh"
#include "water_lighting.sh"

float optics_depth_linear(float depth, float nearPlane, float farPlane)
{
    return (nearPlane * farPlane) / (farPlane - depth * (farPlane - nearPlane));
}

// The opaque depth and completed opaque colour share a clean perspective camera.
// Raw hardware depth is 0..1 on all supported backends.
bool optics_probe(vec3 viewPos, out vec2 uv, out float rawDepth, out float delta)
{
    uv = vec2_splat(0.0);
    rawDepth = 1.0;
    delta = 0.0;
    float nearPlane = u_water_optics1.x;
    float farPlane = u_water_optics1.y;
    float viewDepth = -viewPos.z;
    if (!(viewDepth > nearPlane && viewDepth < farPlane)) return false;
    vec4 clip = mul(u_proj, vec4(viewPos, 1.0));
    if (!(clip.w > 0.0)) return false;
    uv = jce_clip_to_uv(clip);
    if (!(uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0)) return false;
    rawDepth = texture2DLod(s_water_optics_depth, uv, 0.0).r;
    if (!(rawDepth >= 0.0 && rawDepth <= 1.0)) return false;
    delta = viewDepth - optics_depth_linear(rawDepth, nearPlane, farPlane);
    return true;
}

// Only the sky already visible in this viewport can supply this fallback.
// Geometry SSR remains authoritative; this is not offscreen environment IBL.
vec4 optics_visible_sky(vec3 worldDirection, vec3 viewDirection)
{
    if (!(u_water_optics_sky.x > 0.5 && u_water_optics_sky.y > 0.0 &&
          u_water_optics_sky.z > 0.0 && worldDirection.y > 0.002))
        return vec4_splat(0.0);
    vec4 clip = mul(u_proj, vec4(viewDirection, 0.0));
    if (!(clip.w > 0.00001)) return vec4_splat(0.0);
    vec2 uv = jce_clip_to_uv(clip);
    vec2 texel = u_water_optics_sky.yz;
    // Keep the entire bilinear colour footprint inside this same viewport.
    if (!(uv.x > texel.x && uv.x < 1.0 - texel.x &&
          uv.y > texel.y && uv.y < 1.0 - texel.y))
        return vec4_splat(0.0);
    vec2 corner = (floor(uv / texel - vec2_splat(0.5)) + vec2_splat(0.5)) * texel;
    // A near-one threshold admits distant opaque geometry at a long far clip.
    // The depth producer clears to exactly one; all four colour neighbours
    // must be clear so linear colour cannot leak an opaque silhouette.
    if (!(texture2DLod(s_water_optics_depth, corner, 0.0).r == 1.0 &&
          texture2DLod(s_water_optics_depth, corner + vec2(texel.x, 0.0), 0.0).r == 1.0 &&
          texture2DLod(s_water_optics_depth, corner + vec2(0.0, texel.y), 0.0).r == 1.0 &&
          texture2DLod(s_water_optics_depth, corner + texel, 0.0).r == 1.0))
        return vec4_splat(0.0);
    vec2 edge = min(uv, vec2_splat(1.0) - uv);
    float confidence = smoothstep(0.0, 0.04, min(edge.x, edge.y)) *
                       smoothstep(0.002, 0.025, worldDirection.y);
    // Source is the completed linear sky, including its real cloud layer.
    return vec4(texture2DLod(s_water_optics_opaque, uv, 0.0).rgb, confidence);
}

vec4 optics_reflect(vec3 surface, vec3 surfaceView, vec3 normal, vec3 dir)
{
    vec3 direction = normalize(mul(u_view, vec4(dir, 0.0)).xyz);
    float budget = clamp(u_water_optics1.w, 1.0, 16.0);
    float maxDistance = u_water_optics0.w;
    float stepLength = maxDistance / budget;
    float previous = 0.0;
    bool blocked = false;
    vec2 uv;
    float depth;
    float delta;

    LOOP for (int i = 0; i < 16; ++i)
    {
        if (float(i) >= budget) break;
        float current = min(float(i + 1) * stepLength, maxDistance);
        if (!optics_probe(surfaceView + direction * current, uv, depth, delta))
            break;
        if (delta >= 0.0)
        {
            // An uncertain opaque crossing is not permission to see the sky.
            blocked = true;
            // Tighten the front/back crossing with fixed bisection. A depth
            // discontinuity can bracket a false hit, so validate thickness
            // and receiver side after refinement.
            float low = previous;
            float high = current;
            for (int j = 0; j < 5; ++j)
            {
                float mid = (low + high) * 0.5;
                vec2 midUv;
                float midDepth;
                float midDelta;
                if (!optics_probe(surfaceView + direction * mid, midUv, midDepth, midDelta))
                    return vec4_splat(0.0);
                if (midDelta >= 0.0)
                {
                    high = mid;
                    uv = midUv;
                    depth = midDepth;
                    delta = midDelta;
                }
                else
                {
                    low = mid;
                }
            }
            if (!(depth < 0.999999 && delta <= u_water_optics1.z))
                break;

            vec3 receiver = jce_uv_depth_to_world(u_invViewProj, uv, depth);
            vec3 offset = receiver - surface;
            float above = dot(offset, normal);
            if (!(above > 0.0))
                break;

            vec2 edge = min(uv, vec2_splat(1.0) - uv);
            float confidence = smoothstep(0.0, 0.04, min(edge.x, edge.y)) *
                               (1.0 - smoothstep(maxDistance * 0.85, maxDistance, high)) *
                               smoothstep(0.0, 0.10, above) *
                               (1.0 - smoothstep(u_water_optics1.z * 0.4, u_water_optics1.z, delta));
            vec3 source = texture2DLod(s_water_optics_opaque, uv, 0.0).rgb;
            return vec4(source, confidence);
        }
        previous = current;
    }
    if (blocked) return vec4_splat(0.0);
    return optics_visible_sky(dir, direction);
}

// Estimate the bent ray's screen offset using the local column depth. Across
// a depth discontinuity blend toward the undistorted, submerged receiver.
vec4 optics_refract(vec3 surface, vec3 surfaceView, vec3 direction,
                    vec2 surfaceUv, float surfaceDepth, float surfaceDelta)
{
    if (surfaceDepth >= 0.999999) return vec4_splat(0.0);

    vec3 center = jce_uv_depth_to_world(u_invViewProj, surfaceUv, surfaceDepth);
    float verticalDepth = surface.y - center.y;
    if (verticalDepth <= 0.0) return vec4_splat(0.0);

    float path = length(center - surface);
    float maxDistance = u_water_optics0.w;
    float confidence = smoothstep(0.0, 0.18, verticalDepth) *
                       (1.0 - smoothstep(maxDistance * 0.8, maxDistance, path));
    if (confidence <= 0.0) return vec4_splat(0.0);

    vec3 source = texture2DLod(s_water_optics_opaque, surfaceUv, 0.0).rgb;
    vec3 bent = mul(u_view, vec4(direction, 0.0)).xyz;
    float forward = -bent.z;
    if (forward > 0.05)
    {
        float distanceAlongRay = -surfaceDelta / forward;
        vec2 uv;
        float depth;
        float delta;
        if (distanceAlongRay < maxDistance &&
            optics_probe(surfaceView + bent * distanceAlongRay, uv, depth, delta) &&
            depth < 0.999999)
        {
            vec3 receiver = jce_uv_depth_to_world(u_invViewProj, uv, depth);
            float below = surface.y - receiver.y;
            float depthBehind = -surfaceDelta - delta;
            vec2 edge = min(uv, vec2_splat(1.0) - uv);
            float continuity = 1.0 - smoothstep(max(0.12, -surfaceDelta * 0.25),
                                               max(0.35, -surfaceDelta * 0.8), abs(delta));
            float distortion = smoothstep(0.0, 0.05, depthBehind) *
                               smoothstep(0.0, 0.18, below) * continuity *
                               smoothstep(0.0, 0.04, min(edge.x, edge.y));
            if (distortion > 0.0)
            {
                source = mix(source, texture2DLod(s_water_optics_opaque, uv, 0.0).rgb,
                             distortion);
                path = mix(path, length(receiver - surface), distortion);
            }
        }
    }

    vec3 through = exp(-max(u_water_absorb.xyz, vec3_splat(0.0)) * path);
    vec3 medium = water_medium_source(u_water_absorb_tint.rgb,
        water_ambient_incident(u_ambientColor),
        water_direct_incident(u_dirLights[0], u_dirLights[1].xyz, u_lightCounts.x));
    // Opaque color already contains illumination; attenuate it only by Beer.
    return vec4(source * through + medium * (1.0 - through), confidence);
}

void main()
{
    float shoreDepth = 0.0;
    float shoreCoverage = 1.0;
    if (u_water_nearshore[0].w > 0.0)
    {
        vec2 cell = water_shore_cell(v_worldpos.xz);
        float weight = water_shore_weight(cell);
        if (weight > 0.0)
        {
            vec4 shore = water_shore_data(cell);
            shoreDepth = max(shore.g, 0.0);
            shoreCoverage = mix(1.0, smoothstep(0.001, 0.01, shore.g), weight);
        }
    }
    WaterSurfaceNormal surfaceNormal = water_surface_normal(
        v_normal, v_worldpos, v_tangent, v_bitangent.xy, shoreDepth);
    // Keep helper lanes alive through all normal derivatives, as in colour.
    if (shoreCoverage < 0.01) discard;
    vec3 normal = surfaceNormal.normal;
    vec3 viewDir = surfaceNormal.viewDirection;
    float facing = surfaceNormal.facing;
    if (!(u_cameraPos.y > v_worldpos.y && facing > 0.0 &&
          u_water_optics0.w > 0.0 && u_water_optics1.x > 0.0 &&
          u_water_optics1.y > u_water_optics1.x && u_water_optics1.z > 0.0))
    {
        gl_FragColor = vec4_splat(0.0);
        return;
    }

    float ior = clamp(u_water_optics0.z, 1.0, 2.0);
    float f0 = (ior - 1.0) / (ior + 1.0);
    f0 *= f0;
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - clamp(facing, 0.0, 1.0), 5.0);
    // A unit IOR is a no-interface reference case.
    if (ior <= 1.0) fresnel = 0.0;
    vec2 strength = clamp(u_water_optics0.xy, vec2_splat(0.0), vec2_splat(1.0));
    vec2 weights = vec2(fresnel, 1.0 - fresnel) * strength;
    if (weights.x + weights.y <= 0.0) discard;

    // The trace target has independent depth ownership. Reject fragments
    // hidden in the opaque pass and use this common front sample to start
    // reflection and refraction.
    vec3 surfaceView = mul(u_view, vec4(v_worldpos, 1.0)).xyz;
    vec2 surfaceUv;
    float surfaceDepth;
    float surfaceDelta;
    if (!optics_probe(surfaceView, surfaceUv, surfaceDepth, surfaceDelta) ||
        surfaceDelta >= -0.001) discard;

    vec4 optical = vec4_splat(0.0);
    BRANCH if (weights.x > 0.0)
    {
        vec4 hit = optics_reflect(v_worldpos, surfaceView, normal, reflect(-viewDir, normal));
        float amount = weights.x * hit.a;
        optical += vec4(hit.rgb * amount, amount);
    }
    BRANCH if (weights.y > 0.0)
    {
        vec3 bent = refract(-viewDir, normal, 1.0 / ior);
        vec4 hit = optics_refract(v_worldpos, surfaceView, bent, surfaceUv, surfaceDepth, surfaceDelta);
        float amount = weights.y * hit.a;
        optical += vec4(hit.rgb * amount, amount);
    }
    gl_FragColor = optical;
}
