#ifndef JCE_TERRAIN_SNOW_HEIGHT_SH
#define JCE_TERRAIN_SNOW_HEIGHT_SH

uniform vec4 u_terrainParams;
uniform vec4 u_terrainSnowStamps[8]; // world XZ, radius and depth in metres
uniform vec4 u_terrainSnowCover[24]; // 8 planes or mesh triangles, 3 vec4 each
uniform vec4 u_terrainSnowGeometry; // x=quantized snow, y=minimum resolved radius, zw=max depression/rim metres

void terrain_snow_counts(out float stamps, out float covers)
{
    float encoded_count = u_terrainParams.z;
    covers = encoded_count < 0.0 ? floor(-encoded_count / 16.0) : 0.0;
    stamps = abs(encoded_count) - 16.0 * covers;
}

/* Signed snow height and its derivative with respect to squared radius.
 * The smooth centre cavity and its narrower rim both meet untouched snow
 * with zero slope. Derivatives reconstruct the normal without a sampler. */
vec2 terrain_snow_profile(float q)
{
    vec3 width = vec3(0.60, 0.28, 0.30);
    vec3 t = clamp((vec3_splat(q) - vec3(0.04, 0.42, 0.70)) / width,
                   vec3_splat(0.0), vec3_splat(1.0));
    vec3 s = t * t * (vec3_splat(3.0) - 2.0 * t);
    vec3 ds = 6.0 * t * (vec3_splat(1.0) - t) / width;
    float rim = s.y * (1.0 - s.z);
    float rim_slope = ds.y * (1.0 - s.z) - s.y * ds.z;
    return vec2(s.x - 1.0 + 0.24 * rim, ds.x + 0.24 * rim_slope);
}

/* Return signed height and the XZ gradient. A bounded perturbation gives
 * the rim a natural edge and its derivative keeps the lighting continuous. */
vec3 terrain_snow_track(vec2 world_xz, float stamp_count, float minimum_radius)
{
    vec3 result = vec3_splat(0.0);
    for (int i = 0; i < 8; ++i) {
        if (float(i) >= stamp_count) break;
        vec4 stamp = u_terrainSnowStamps[i];
        if (stamp.z < minimum_radius) continue;
        vec2 p = (world_xz - stamp.xy) / stamp.z;
        float radius2 = dot(p, p);
        if (radius2 >= 1.07) continue;
        vec2 axis_a = vec2(7.0, 3.0);
        vec2 axis_b = vec2(-4.0, 9.0);
        float a = dot(p, axis_a);
        float b = dot(p, axis_b);
        float warp = 1.0 + 0.06 * sin(a) * sin(b);
        float q = radius2 * warp;
        if (q >= 1.0) continue;
        vec2 dw = 0.06 * (axis_a * cos(a) * sin(b) + axis_b * sin(a) * cos(b));
        vec2 gradient = (2.0 * p * warp + radius2 * dw) / stamp.z;
        vec2 profile = terrain_snow_profile(q) * stamp.w;
        result += vec3(profile.x, profile.y * gradient);
    }
    return result;
}

/* The CPU owner supplies the same metre bounds used for admission and
 * colour/shadow culling. Apply coverage before limiting the accumulated field;
 * a clipped plateau has zero field gradient, not the unclipped slope. */
vec3 terrain_snow_limit(vec3 field, float coverage)
{
    vec3 value = field * coverage;
    float depth_limit = max(u_terrainSnowGeometry.z, 0.0);
    float rim_limit = max(u_terrainSnowGeometry.w, 0.0);
    if (value.x <= -depth_limit || value.x >= rim_limit) {
        value.y = 0.0;
        value.z = 0.0;
    }
    value.x = clamp(value.x, -depth_limit, rim_limit);
    return value;
}

/* Project world XZ into the blocker footprint and evaluate its underside
 * at this exact point. Each canopy transmits a fraction of falling snow. */
float terrain_snow_exposure(vec3 world_pos, float cover_count)
{
    float exposure = 1.0;
    for (int i = 0; i < 8; ++i) {
        if (float(i) >= cover_count) break;
        vec4 center = u_terrainSnowCover[3 * i];
        vec4 projection = u_terrainSnowCover[3 * i + 1];
        vec4 slope_and_reach = u_terrainSnowCover[3 * i + 2];
        vec2 delta = world_pos.xz - center.xz;
        if (any(greaterThanEqual(abs(delta), slope_and_reach.zw))) continue;
        vec2 p = vec2(dot(projection.xy, delta), dot(projection.zw, delta));
        float above = center.y + dot(p, slope_and_reach.xy) - world_pos.y;
        if (above <= 0.08) continue;
        if (center.w < -1.5) {
            /* Hard barycentric edges preserve the shared diagonal of a
             * roof triangulation. The existing vertical bias avoids self-cover. */
            if (p.x >= -0.00002 && p.y >= -0.00002 && p.x + p.y <= 1.00002)
                exposure *= 1.0 - smoothstep(0.08, 0.25, above);
            continue;
        }
        bool roof = center.w < 0.0;
        float distance = roof ? max(abs(p.x), abs(p.y)) : dot(p, p);
        float mask = 1.0 - smoothstep(roof ? 0.90 : 0.42, 1.0, distance);
        float weight = mask * smoothstep(0.08, 0.25, above);
        exposure *= mix(1.0, max(center.w, 0.0), weight);
    }
    return exposure;
}

/* Use the same signed field in the colour and depth vertex programs.
 * The fragment stage retains the original detail normal at finer scales. */
vec3 terrain_snow_displace(vec3 world_pos, float up)
{
    if (u_terrainSnowGeometry.x <= 0.0) return world_pos;
    float stamps, covers;
    terrain_snow_counts(stamps, covers);
    if (stamps <= 0.0) return world_pos;
    float snow = u_terrainSnowGeometry.x * smoothstep(0.35, 0.85, clamp(up, 0.0, 1.0));
    if (covers > 0.0) snow *= terrain_snow_exposure(world_pos, covers);
    if (snow <= 0.0) return world_pos;
    vec3 field = terrain_snow_track(world_pos.xz, stamps, u_terrainSnowGeometry.y);
    world_pos.y += terrain_snow_limit(field, snow).x;
    return world_pos;
}

#endif
