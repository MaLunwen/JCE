$input v_texcoord0, v_worldpos, v_tint

#include <bgfx_shader.sh>

/* v_worldpos carries the per-card world-space view direction; v_tint carries the
   atlas params (x=gridN, yzw=baseColor tint) — see vs_impostor.sc. */
#define v_octViewDir v_worldpos
#define v_octParams  v_tint

/* Octahedral impostor card fragment program (terminal LOD).
 *
 * Maps the per-card world-space view direction to an octahedral atlas cell and
 * samples that cell's pre-baked albedo+coverage.  The quad UV (v_texcoord0)
 * indexes WITHIN the chosen cell; the cell is chosen by octahedral-encoding the
 * view direction onto the grid_n x grid_n atlas.  v1 = nearest cell (multi-cell
 * blend deferred to v2).  Coverage (alpha) is alpha-tested; a simple sun term
 * gives the flat-lit card a little depth.
 */
SAMPLER2D(s_impostorAtlas, 0);

uniform vec4 u_octLightDir;   /* xyz = direction TO the sun (world), w unused   */
uniform vec4 u_octLightColor; /* rgb = sun color * intensity, w = ambient scale */

/* Full-octahedral encode: unit direction -> [0,1]^2 atlas coordinate.  Matches
   the CPU bake (jce_impostor_oct_encode): wrap the lower hemisphere across the
   octahedron's edges so the whole sphere maps to the unit square. */
vec2 oct_encode(vec3 dir)
{
    dir /= (abs(dir.x) + abs(dir.y) + abs(dir.z));
    vec2 oct = dir.xz;
    if (dir.y < 0.0) {
        vec2 s = vec2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
        oct = (1.0 - abs(oct.yx)) * s;
    }
    return oct * 0.5 + 0.5;   /* [-1,1] -> [0,1] */
}

void main()
{
    float gridN = max(v_octParams.x, 1.0);
    vec3  tint  = v_octParams.yzw;

    /* Choose the nearest octahedral cell for this card's view direction. */
    vec2  oc   = oct_encode(normalize(v_octViewDir));
    vec2  cell = floor(clamp(oc * gridN, 0.0, gridN - 1.0)); /* (col,row) */

    /* Atlas UV = (cell + quadUV) / gridN.  The quad UV spans the cell so the
       card silhouette comes straight from the baked view. */
    vec2 atlasUv = (cell + clamp(v_texcoord0, 0.0, 1.0)) / gridN;

    vec4 texel = texture2D(s_impostorAtlas, atlasUv);

    /* Alpha-test the baked coverage mask (hard cutout; cross-fade is v2). */
    if (texel.a < 0.4)
        discard;

    /* Flat sun term: the baked albedo already carries the lit appearance from
       the bake, so apply only a gentle directional wash for a touch of form.
       (Normal-mapped relighting is a v2 defer — no normal atlas in v1.) */
    float ndl = clamp(0.5 + 0.5 * u_octLightDir.y, 0.3, 1.0);
    vec3  lit = texel.rgb * tint * (u_octLightColor.rgb * ndl + u_octLightColor.w);

    gl_FragColor = vec4(lit, 1.0);
}
