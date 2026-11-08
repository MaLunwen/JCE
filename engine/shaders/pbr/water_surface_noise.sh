#ifndef JCE_WATER_SURFACE_NOISE_SH
#define JCE_WATER_SURFACE_NOISE_SH

// One value-noise interpolant for water colour, foam and optical normals.
float water_hash12(vec2 p)
{
    vec3 q = fract(vec3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float water_vnoise2(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = water_hash12(i);
    float b = water_hash12(i + vec2(1.0, 0.0));
    float c = water_hash12(i + vec2(0.0, 1.0));
    float d = water_hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

#if !defined(JCE_WATER_GL21) || !JCE_WATER_GL21
float water_detail_weight(vec2 uv)
{
    float footprint = max(length(dFdx(uv)), length(dFdy(uv)));
    return 1.0 - smoothstep(0.18, 0.65, footprint);
}

float water_ice_vein(vec2 uv)
{
    float detail = water_detail_weight(uv);
    BRANCH if (detail <= 0.0) return 0.0;
    float ridge = 1.0 - smoothstep(0.02, 0.12, abs(water_vnoise2(uv) - 0.5));
    return ridge * detail;
}

vec3 water_noise_gradient(vec2 uv)
{
    vec2 p = floor(uv), f = fract(uv);
    vec2 u = f * f * (3.0 - 2.0 * f);
    vec2 du = 6.0 * f * (1.0 - f);
    float a = water_hash12(p);
    float b = water_hash12(p + vec2(1.0, 0.0));
    float c = water_hash12(p + vec2(0.0, 1.0));
    float d = water_hash12(p + vec2(1.0, 1.0));
    return vec3(mix(mix(a, b, u.x), mix(c, d, u.x), u.y),
                vec2(mix(b - a, d - c, u.y), mix(c - a, d - b, u.x)) * du);
}
#endif

#endif
