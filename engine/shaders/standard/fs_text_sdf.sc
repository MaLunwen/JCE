$input v_color0, v_texcoord0

/*
 * fs_text_sdf.sc -- a glyph drawn from a SIGNED DISTANCE FIELD, with the
 * effects the field makes cheap.
 *
 * WHY A FIELD AT ALL.  A bitmap atlas stores the glyph at ONE size.  Draw it
 * bigger and the sampler interpolates coverage, so the edge becomes a ramp as
 * wide as the magnification: a heading at 4x is visibly soft, and the only fix
 * in that model is a second atlas per size.  A distance field stores how far
 * each texel is from the outline, which is a smooth function -- so bilinear
 * interpolation between texels lands ON the outline rather than between two
 * coverage values, and one atlas is crisp at every size.  This is what lets a
 * 512x512 atlas serve a whole UI, which matters most on the machines that can
 * least afford several.
 *
 * AND WHY THE EFFECTS BELONG HERE.  Once the field exists, an outline is a
 * SECOND THRESHOLD further out from the same number, and a drop shadow is the
 * same read at an offset.  Doing them any other way -- a second draw, a second
 * atlas, a blur pass -- costs a pass to produce what one already-loaded value
 * answers.  That is why TextMeshPro, Godot's MSDF fonts and Slate all put them
 * on the field.
 *
 * NO DERIVATIVES, DELIBERATELY.  The usual formulation takes the filter width
 * from fwidth(d).  dFdx/dFdy are not in GLSL ES 1.00 without
 * OES_standard_derivatives, and this engine's floor is ES 2.0 / desktop GL
 * 2.1, so the width arrives as a UNIFORM the CPU already knows: the text
 * renderer is the thing that decided how many screen pixels one atlas texel
 * covers.  It is one float per draw instead of two extra instructions per
 * fragment, and it works on every backend this engine claims.
 *
 * WHAT PLAIN TEXT PAYS.  Both effects are behind branches on UNIFORMS, which
 * is uniform control flow: every fragment in the draw takes the same side, so
 * there is no divergence and the shadow's second texture read does not happen
 * at all for a label that has no shadow.  A label with neither effect costs
 * two compares more than before.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

/* x = smoothing half-width in DISTANCE units (0 => a hard cutoff, which is
 *     what a glyph drawn far below one texel per pixel wants);
 * y = the distance value that IS the outline;
 * z = outline width in the same distance units (0 = no outline);
 * w = free. */
uniform vec4 u_sdfParams;

/* Outline colour.  Premultiplied by nothing: composited under the face below,
 * so its own alpha is what decides how much of it shows. */
uniform vec4 u_sdfOutline;

/* x,y = shadow offset in TEXTURE space (already divided by the atlas size by
 *       the caller, because the shader has no business knowing the atlas
 *       dimensions);
 * z,w = free. */
uniform vec4 u_sdfShadowOffset;

/* Shadow colour; .w == 0 disables the whole effect, second sample included. */
uniform vec4 u_sdfShadowColor;

void main()
{
    /* .w, not .x.  The glyph atlas is RGBA with WHITE rgb and the glyph in
     * ALPHA -- that is where bitmap coverage already lives, so a distance
     * field lands in the same channel and the atlas packing is untouched.
     * Reading .x would sample the constant 255 and every glyph would be a
     * solid block. */
    float d = texture2D(s_texColor, v_texcoord0).w;

    float edge = u_sdfParams.y;
    /* max(w, tiny): smoothstep with a zero width is undefined, and a glyph
     * scaled down far enough legitimately asks for one. */
    float ww   = max(u_sdfParams.x, 0.0001);
    float ow   = u_sdfParams.z;

    /* The FACE: the glyph itself, exactly as before. */
    float faceA = smoothstep(edge - ww, edge + ww, d);

    /* The OUTLINE is a second threshold on the same number, `ow` further OUT
     * -- i.e. at a LOWER distance value, because the field decreases outward.
     * The band between the two thresholds is the stroke; inside the face the
     * outline is hidden by the face composited over it. */
    vec4 col = vec4(v_color0.xyz, v_color0.w * faceA);
    if (ow > 0.0) {
        float outA = smoothstep(edge - ow - ww, edge - ow + ww, d);
        vec4 o = vec4(u_sdfOutline.xyz, u_sdfOutline.w * outA);
        /* Face OVER outline.  Straight alpha, so the face's own coverage
         * decides how much outline survives underneath it. */
        float a = col.w + o.w * (1.0 - col.w);
        vec3 rgb = (col.w > 0.0 || o.w > 0.0)
                 ? (col.xyz * col.w + o.xyz * o.w * (1.0 - col.w)) / max(a, 0.0001)
                 : col.xyz;
        col = vec4(rgb, a);
    }

    /* The SHADOW is the same field read at an offset, composited UNDER
     * everything.  Behind a uniform branch so a label without one does not pay
     * the second texture read. */
    if (u_sdfShadowColor.w > 0.0) {
        float sd = texture2D(s_texColor,
                             v_texcoord0 - u_sdfShadowOffset.xy).w;
        /* The shadow takes the OUTLINE's extent when there is one, so a
         * stroked glyph casts the shape it actually shows rather than the
         * narrower face inside it. */
        float sEdge = edge - ow;
        float sA = smoothstep(sEdge - ww, sEdge + ww, sd) * u_sdfShadowColor.w;
        float a = col.w + sA * (1.0 - col.w);
        vec3 rgb = (col.w > 0.0 || sA > 0.0)
                 ? (col.xyz * col.w + u_sdfShadowColor.xyz * sA * (1.0 - col.w))
                   / max(a, 0.0001)
                 : col.xyz;
        col = vec4(rgb, a);
    }

    gl_FragColor = col;
}
