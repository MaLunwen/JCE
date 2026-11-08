#ifndef JCE_PARTICLE_RAIN_SURFACE_SH
#define JCE_PARTICLE_RAIN_SURFACE_SH

SAMPLER2D(s_particle_rain, 2);
uniform vec4 u_particle_rain[2]; // min XZ, inv cell, side; inv atlas XY, still Y, on

bool particle_rain_enabled()
{
    return u_particle_rain[1].w > 0.5;
}

/* Same padded-cell convention as water. The interior margin makes the
 * nearshore blend one, so this path needs no approximate ambient FFT. */
bool particle_rain_surface(vec2 world_xz, out float surface_y)
{
    surface_y = u_particle_rain[1].z;
    if (!particle_rain_enabled()) return false;
    vec2 cell = (world_xz - u_particle_rain[0].xy) * u_particle_rain[0].z;
    float side = u_particle_rain[0].w;
    vec2 edge = min(cell, vec2_splat(side) - cell);
    if (min(edge.x, edge.y) < max(1.0, side * 0.15)) return false;
    vec2 uv = (vec2_splat(1.0) + cell) * u_particle_rain[1].xy;
#ifdef JCE_PARTICLE_RAIN_FRAGMENT
    vec2 water = texture2D(s_particle_rain, uv).rg;
#else
    vec2 water = texture2DLod(s_particle_rain, uv, 0.0).rg;
#endif
    surface_y += water.x;
    return water.y > 0.025;
}

#endif
