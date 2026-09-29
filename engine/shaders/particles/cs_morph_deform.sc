/*
 * cs_morph_deform.sc -- blendshape (morph target) deformation on the GPU.
 *
 * One thread per VERTEX.  Copies the whole interleaved vertex through, then
 * rewrites the POSITION and NORMAL fields with the weighted sum of every
 * target's deltas:
 *
 *     pos'    = base_pos    + sum_t( w_t * dpos[t][v] )
 *     normal' = normalize( base_normal + sum_t( w_t * dnorm[t][v] ) )
 *
 * which is jce_morph_apply's formula, unchanged -- deliberately, because the
 * CPU evaluator stays as the fallback for backends without compute and the
 * two must agree.  jce_morph.h is the single statement of the maths; this is
 * the same statement on the other processor.
 *
 * WHY THE WHOLE VERTEX IS COPIED.  The deformed buffer is drawn in place of
 * the static one, so it has to carry the trailing uv / tangent / joint /
 * skin-weight bytes as well -- the GPU palette skin reads them from the same
 * stream.  The CPU path memcpy's the base array and morphs in place for
 * exactly this reason; here the copy is per-thread and free next to the
 * bandwidth already being spent.
 *
 * FLOAT-ADDRESSED, NOT VEC4-ADDRESSED.  A vertex layout's stride is a multiple
 * of 4 BYTES, not of 16, so a vec4-typed buffer cannot address a vertex that
 * starts at float 11.  Both sides are declared float (BGFX_BUFFER_COMPUTE_
 * FORMAT_32X1 | TYPE_FLOAT) and every index below is in FLOATS.
 *
 * Buffer layout (must mirror jce_morph_gpu.c):
 *   b_morph_base  : RO float[]  the model's base interleaved vertices
 *   b_morph_delta : RO float[]  TARGET-MAJOR deltas, matching JceMorphData:
 *                     positions  at (t*NV + v)*3
 *                     normals    at NT*NV*3 + (t*NV + v)*3   (when present)
 *   b_morph_out   : WO float[]  the deformed interleaved vertices
 *
 * Uniforms:
 *   u_morph_params  : {num_verts, num_targets, stride_floats, has_normals}
 *   u_morph_offsets : {pos_offset_floats, normal_offset_floats, -, -}
 *                     normal_offset < 0 means the layout has no NORMAL, which
 *                     is the same sentinel jce_morph_apply takes.
 *   u_morph_weights : 4 vec4 = JCE_MORPH_MAX_WEIGHTS floats, the resolved
 *                     per-instance weight vector.  The count that is READ is
 *                     u_morph_params.y, so a model with fewer targets does not
 *                     depend on what the unused lanes hold.
 */

#include <bgfx_compute.sh>

BUFFER_RO(b_morph_base,  float, 0);
BUFFER_RO(b_morph_delta, float, 1);
BUFFER_WO(b_morph_out,   float, 2);

uniform vec4 u_morph_params;
uniform vec4 u_morph_offsets;
uniform vec4 u_morph_weights[4];

/* The weight vector as a flat array.  Indexed dynamically, so it is written
 * out rather than reached through u_morph_weights[t/4][t%4]: a dynamic index
 * into a vector component is not expressible in every profile this compiles
 * for, and the two-step version compiles everywhere. */
float morph_weight(uint t)
{
	vec4 v = u_morph_weights[t >> 2u];
	uint c = t & 3u;
	if (c == 0u) return v.x;
	if (c == 1u) return v.y;
	if (c == 2u) return v.z;
	return v.w;
}

NUM_THREADS(64, 1, 1)
void main()
{
	uint v  = gl_GlobalInvocationID.x;
	uint nv = uint(u_morph_params.x);
	if (v >= nv) return;

	uint nt     = uint(u_morph_params.y);
	uint stride = uint(u_morph_params.z);
	bool has_n  = u_morph_params.w > 0.5;

	uint vbase = v * stride;

	/* The vertex, byte-for-byte, before anything is rewritten. */
	for (uint i = 0u; i < stride; ++i) {
		b_morph_out[vbase + i] = b_morph_base[vbase + i];
	}

	int  po_i = int(u_morph_offsets.x);
	int  no_i = int(u_morph_offsets.y);
	uint po   = uint(po_i);

	vec3 p = vec3(b_morph_base[vbase + po + 0u],
	              b_morph_base[vbase + po + 1u],
	              b_morph_base[vbase + po + 2u]);

	vec3 n = vec3(0.0, 0.0, 0.0);
	bool do_n = has_n && no_i >= 0;
	uint no = uint(max(no_i, 0));
	if (do_n) {
		n = vec3(b_morph_base[vbase + no + 0u],
		         b_morph_base[vbase + no + 1u],
		         b_morph_base[vbase + no + 2u]);
	}

	/* Deltas are target-major, so consecutive threads read consecutive floats
	 * within one target -- the coalesced direction.  Vertex-major would make
	 * every thread stride by 3*NT. */
	uint nbase = nt * nv * 3u;
	for (uint t = 0u; t < nt; ++t) {
		float w = morph_weight(t);
		if (w == 0.0) continue;   /* the common case for a face rig at rest */

		uint di = (t * nv + v) * 3u;
		p += w * vec3(b_morph_delta[di + 0u],
		              b_morph_delta[di + 1u],
		              b_morph_delta[di + 2u]);
		if (do_n) {
			uint ni = nbase + di;
			n += w * vec3(b_morph_delta[ni + 0u],
			              b_morph_delta[ni + 1u],
			              b_morph_delta[ni + 2u]);
		}
	}

	b_morph_out[vbase + po + 0u] = p.x;
	b_morph_out[vbase + po + 1u] = p.y;
	b_morph_out[vbase + po + 2u] = p.z;

	if (do_n) {
		/* Renormalised, and guarded: summed deltas can cancel a normal to
		 * zero, and normalize(0) is a NaN that reaches lighting as a black
		 * or exploded pixel rather than as an error. */
		float len = length(n);
		if (len > 1.0e-8) n = n / len;
		b_morph_out[vbase + no + 0u] = n.x;
		b_morph_out[vbase + no + 1u] = n.y;
		b_morph_out[vbase + no + 2u] = n.z;
	}
}
