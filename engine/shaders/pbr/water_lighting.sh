#ifndef JCE_WATER_LIGHTING_SH
#define JCE_WATER_LIGHTING_SH

// Inputs are the same linear RGB light environment used by opaque PBR.
// The RGB and scalar intensity are one product: time-of-day can put all
// changing energy in RGB while keeping the scalar at one.
vec3 water_ambient_incident(vec4 ambient)
{
    return max(ambient.xyz * max(ambient.w, 0.0), vec3_splat(0.0));
}

// Irradiance crossing the mean horizontal interface. This inexpensive
// homogeneous medium source is shared by the surface and refraction trace;
// it does not integrate shadowed participating media along the refracted ray.
vec3 water_direct_incident(vec4 direction, vec3 color, float light_count)
{
    float length2 = dot(direction.xyz, direction.xyz);
    if (light_count < 0.5 || length2 <= 0.000001)
        return vec3_splat(0.0);
    float cosine = clamp(-direction.y * inversesqrt(length2), 0.0, 1.0);
    return max(color, vec3_splat(0.0)) * (max(direction.w, 0.0) * cosine);
}

vec3 water_medium_source(vec3 tint, vec3 ambient, vec3 direct)
{
    return max(tint, vec3_splat(0.0)) * (ambient + direct);
}

// A diffuse sky surrogate, only when no radiance cubemap is present.
// ToD stores one quarter of (zenith+horizon) as ambient: twice that RGB
// recovers the mean radiance. The smooth angular lobe has no emission floor
// and uses the actual ambient hue, including night/moon ambient color.
vec3 water_ambient_reflection(vec3 reflected, vec3 ambient)
{
    float sky_up = sqrt(clamp(reflected.y, 0.0, 1.0));
    return ambient * (2.0 * mix(1.15, 0.85, sky_up));
}

#endif
