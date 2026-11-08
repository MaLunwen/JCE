uniform vec4 u_particle_cover[24]; // 8 triangles, first origin.w stores count

bool particle_roof_blocks(vec3 center, vec3 velocity)
{
    float count = u_particle_cover[0].w;
    if (count <= 0.0 || !(velocity.y < -0.05)) return false;
    vec2 direction = velocity.xz / velocity.y;
    for (int i = 0; i < 8; ++i) {
        if (float(i) >= count) break;
        vec3 origin = u_particle_cover[3 * i].xyz;
        vec4 basis = u_particle_cover[3 * i + 1];
        vec2 slope = u_particle_cover[3 * i + 2].xy;
        vec2 delta = center.xz - origin.xz;
        vec2 p = vec2(dot(basis.xy, delta), dot(basis.zw, delta));
        vec2 step_xz = vec2(dot(basis.xy, direction), dot(basis.zw, direction));
        float rise = 1.0 - dot(step_xz, slope);
        if (!(rise > 0.0001)) continue;
        float lift = (origin.y + dot(p, slope) - center.y) / rise;
        if (!(lift > 0.08 && lift <= 24.0)) continue;
        vec2 hit = p + step_xz * lift;
        if (hit.x >= -0.00002 && hit.y >= -0.00002 && hit.x + hit.y <= 1.00002)
            return true;
    }
    return false;
}
