$input v_texcoord0, v_normal, v_worldpos

#include <bgfx_shader.sh>

/*
 * fs_foliage_shadow.sc -- alpha-tested depth for foliage-cluster cards
 * (paired with vs_foliage.sc in the CSM cascade views).
 *
 * The reference casts REAL leaf shadows: its instanced bush mesh renders
 * into the shadow map with a custom depth material that samples the leaf
 * alpha mask and discards below 0.8, driven by the SAME wind vertex shader
 * as the color pass -- so leaf flutter visibly animates the shadows.  This
 * is the JCE port of that depth material; the old sphere-proxy shadow
 * (one blob per cluster) stays only as a fallback when this program is
 * missing from the PAK.
 *
 * Threshold 0.8 (vs the color pass's 0.4-0.6 smoothstep): slightly smaller
 * shadow leaves avoid rim acne where the mask feathers out.
 */

SAMPLER2D(s_foliageAlpha, 0);

void main()
{
    float alpha = texture2D(s_foliageAlpha, v_texcoord0).a;
    if (alpha < 0.8) discard;
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
