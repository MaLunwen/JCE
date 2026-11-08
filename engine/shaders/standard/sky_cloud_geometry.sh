#ifndef JCE_SKY_CLOUD_GEOMETRY_SH
#define JCE_SKY_CLOUD_GEOMETRY_SH

/* Match the atmosphere's 6360 km radius. Compute roots using offsets from
 * ground level to avoid subtracting squared planetary radii. */
// HLSL global const values can occupy unbound cbuffer slots in bgfx.
#define JCE_CLOUD_PLANET_KM 6360.0
#define JCE_CLOUD_MARCH_LIMIT_KM 80.0

vec2 cloud_sphere_roots(float b, float c)
{
    float discriminant = b * b - c;
    if (discriminant <= 0.0) return vec2(1e6, -1e6);
    float root = sqrt(discriminant);
    float q = -b - (b >= 0.0 ? root : -root);
    float other = abs(q) > 1e-6 ? c / q : -b;
    return vec2(min(q, other), max(q, other));
}

vec2 cloud_shell_range(vec3 camera_km, vec3 ray, float base_km, float top_km)
{
    float b = dot(camera_km, ray) + JCE_CLOUD_PLANET_KM * ray.y;
    float c = dot(camera_km, camera_km) + 2.0 * JCE_CLOUD_PLANET_KM * camera_km.y;
    float inner_c = c - base_km * (2.0 * JCE_CLOUD_PLANET_KM + base_km);
    float outer_c = c - top_km * (2.0 * JCE_CLOUD_PLANET_KM + top_km);
    vec2 outer = cloud_sphere_roots(b, outer_c);
    vec2 inner = cloud_sphere_roots(b, inner_c);
    float start = max(outer.x, 0.0);
    float end = min(outer.y, JCE_CLOUD_MARCH_LIMIT_KM);
    if (c <= 0.0 && b < 0.0) return vec2(0.0, 0.0);
    vec2 ground = cloud_sphere_roots(b, c);
    if (ground.x >= 0.0) end = min(end, ground.x);
    if (inner_c < 0.0) {
        start = max(start, inner.y);
    } else if (inner.x >= start && inner.x < end) {
        // Looking down from within or above the layer, stop on the first
        // inward crossing instead of marching through the clear core.
        end = inner.x;
    }
    return vec2(start, end);
}

float cloud_world_altitude(vec3 point_km)
{
    float y = JCE_CLOUD_PLANET_KM + point_km.y;
    float radius = length(vec3(point_km.x, y, point_km.z));
    return point_km.y + dot(point_km.xz, point_km.xz) / max(radius + y, 1e-4);
}

// The complete first cloud interval can be reused only for a receiver below
// the shell and beyond its exit. A representative opacity distance cannot
// prove that a translucent receiver is in front of every integrated sample.
bool cloud_shell_before_surface(vec3 eye_km, vec3 surface_km, float bottom_km, float top_km)
{
    vec3 delta = surface_km - eye_km;
    float distance_km = length(delta);
    if (distance_km <= 1e-6 || cloud_world_altitude(surface_km) >= bottom_km)
        return false;
    vec2 interval = cloud_shell_range(eye_km, delta / distance_km, bottom_km, top_km);
    return interval.y > interval.x && interval.y < JCE_CLOUD_MARCH_LIMIT_KM &&
        interval.y <= distance_km;
}

#endif
