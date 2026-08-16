$input v_texcoord0, v_normal, v_worldpos, v_localpos

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

#include "hashed_alpha.sh"

SAMPLER2D(s_foliageAlpha, 0);

/* x = hashed-alpha noise scale in pixels; <= 0 keeps the fixed 0.8 cutoff.
 *
 * Off by default, and the default matters: hashed alpha is STOCHASTIC, and a
 * stochastic shadow with nothing to resolve it is a stippled shadow.  Only the
 * tiers that have a temporal resolve should turn it on -- the same rule the
 * cascade dither follows, for the same reason. */
uniform vec4 u_foliage_hashed_alpha;

void main()
{
    float alpha = texture2D(s_foliageAlpha, v_texcoord0).a;

    /* Hashed alpha stabilises LOD transitions: with a fixed cutoff, whole
     * leaves cross the threshold at once as the mask mips down and a tree
     * loses a visible chunk of canopy in one frame -- which reads as the LOD
     * system being badly tuned rather than as the alpha test. */
    if (u_foliage_hashed_alpha.x > 0.0)
    {
        // Composed with the 0.8 shrink, not a replacement for it.
        //
        // A uniform hashed threshold averages 0.5, so testing alpha against it
        // directly would make the shadow leaves BIGGER than the fixed 0.8
        // cutoff produced -- reversing the deliberate shrink that keeps the
        // shadow slightly inside the leaf and avoids rim acne.  Nothing about
        // that reads as a regression; it reads as slightly fuzzier shadows.
        //
        // Instead the alpha is remapped THROUGH the old cutoff first, so the
        // surviving coverage is the shrunk silhouette, and the hash only
        // decides where the boundary falls within it.  Same silhouette, no
        // popping.
        float shrunk = clamp((alpha - 0.8) / 0.2, 0.0, 1.0);
        if (shrunk < hashed_alpha_threshold(v_localpos, u_foliage_hashed_alpha.x))
            discard;
    }
    else if (alpha < 0.8)
    {
        discard;
    }
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
