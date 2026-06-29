$input a_position, a_texcoord0, i_data0, i_data1
$output v_texcoord0, v_worldpos, v_tint

#include <bgfx_shader.sh>

/* This shader reuses two existing pbr varyings to avoid growing the shared
   varying_pbr.def.sc (which is compiled against every pbr shader):
     v_worldpos (vec3) carries the per-card world-space view direction.
     v_tint     (vec4) carries the atlas params: x=gridN, yzw=baseColor tint. */

/* Octahedral impostor card (terminal LOD).
 *
 * Each instance is one camera-facing quad.  The mesh VB is a unit quad with
 * a_position.xy in [-0.5, 0.5] (z = 0) and a_texcoord0 the quad UV in [0,1].
 *
 * Per-instance data packed by the renderer (jce_impostor_draw_instanced):
 *   i_data0 = (centerX, centerY, centerZ, radius)   world center + billboard half-size
 *   i_data1 = (gridN,   tintR,   tintG,   tintB)     atlas grid dim + baseColor tint
 *
 * The quad is expanded in WORLD space using the camera's right/up basis (so it
 * always faces the camera), and the per-card view direction (card -> camera) is
 * forwarded to the fragment shader for the octahedral atlas lookup.
 */
void main()
{
    vec3  center = i_data0.xyz;
    float radius = i_data0.w;

    /* Camera world-space basis via the inverse-view matrix (mul handles HLSL
       row-major vs GLSL column-major correctly, unlike raw column indexing):
         right        = invView * (1,0,0,0)
         up           = invView * (0,1,0,0)
         toward-viewer= invView * (0,0,1,0)   (view +Z points from scene to eye) */
    vec3 camRight = normalize(mul(u_invView, vec4(1.0, 0.0, 0.0, 0.0)).xyz);
    vec3 camUp    = normalize(mul(u_invView, vec4(0.0, 1.0, 0.0, 0.0)).xyz);
    vec3 camFwd   = normalize(mul(u_invView, vec4(0.0, 0.0, 1.0, 0.0)).xyz); /* toward viewer */

    /* Expand the unit quad into a world-space billboard sized to the card.  The
       card spans the full sphere diameter so the silhouette is never clipped. */
    vec2 corner = a_position.xy * (radius * 2.0);
    vec3 wpos   = center + camRight * corner.x + camUp * corner.y;

    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));

    v_texcoord0 = a_texcoord0;
    /* Direction from the card toward the camera, in WORLD space (== object space
       for un-rotated props such as trees).  camFwd already points toward the
       viewer for a right-handed view matrix.  Carried via v_worldpos. */
    v_worldpos  = normalize(camFwd);
    /* Atlas params via v_tint: x=gridN, yzw=baseColor tint. */
    v_tint      = vec4(i_data1.x, i_data1.y, i_data1.z, i_data1.w);
}
