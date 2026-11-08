/* The vertex and fragment programs share the same stage-13 atlas mapping.
 * The unpadded UV interval [0, side] spans cell edges. Texture filtering
 * samples cell centres at i + 0.5 and the padded edges clamp independently. */
SAMPLER2D(s_water_ripple, 13);
uniform vec4 u_water_nearshore[2];
// [0] = min XZ, inverse cell size, shore side (zero selects legacy wake).
// [1] = inverse atlas width/height, wake side, shore foam opacity.

vec2 water_shore_cell(vec2 worldXZ)
{
    return (worldXZ - u_water_nearshore[0].xy) * u_water_nearshore[0].z;
}

float water_shore_weight(vec2 cell)
{
    float side = u_water_nearshore[0].w;
    vec2 edge = min(cell, vec2_splat(side) - cell);
    float edgeCells = min(edge.x, edge.y);
    return smoothstep(0.0, max(1.0, side * 0.15), edgeCells);
}

vec4 water_shore_data(vec2 cell)
{
    float side = u_water_nearshore[0].w;
    vec2 texel = vec2_splat(1.0) +
        clamp(cell, vec2_splat(0.0), vec2_splat(side));
    return texture2DLod(s_water_ripple, texel * u_water_nearshore[1].xy, 0.0);
}

// Flow occupies a second padded shore tile immediately below the first.
// Old short atlases have no tile; old tall wake atlases contain cleared zeros.
vec2 water_shore_velocity(vec2 cell)
{
    float side = u_water_nearshore[0].w;
    float requiredHeight = 2.0 * (side + 2.0);
    if (side <= 0.0 || u_water_nearshore[1].y <= 0.0 ||
        requiredHeight * u_water_nearshore[1].y > 1.0001) return vec2_splat(0.0);
    vec2 texel = vec2(1.0, side + 3.0) +
        clamp(cell, vec2_splat(0.0), vec2_splat(side));
    return texture2DLod(s_water_ripple, texel * u_water_nearshore[1].xy, 0.0).rg;
}
