#ifndef JCE_WATER_FOOTPRINT_SH
#define JCE_WATER_FOOTPRINT_SH
/* Reuses an UNUSED slot in the already shared VS/FS water uniform:
 * x = wave count, y = base height, z = valid authored dataTex, w = reserved.
 * z is reset by each draw; unmasked legacy water does not sample a texture.
 * dataTex R = authored shore-distance grade, G = visible footprint (holes
 * allowed). This is not bathymetry and does not alter physical water queries.
 */
uniform vec4 u_water_params;
vec2 water_footprint_sample(vec2 uv)
{
    if (u_water_params.z < 0.5) return vec2_splat(1.0);
    return texture2D(s_water_data, uv).rg;
}
#endif
