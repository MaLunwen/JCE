$input a_position, a_normal, a_indices, a_weight
$output v_normal, v_curClip, v_prevClip

#include <bgfx_shader.sh>

/*
 * vs_gbuffer_vel_skinned.sc — SKINNED G-buffer + per-bone motion-vector vs.
 *
 * Skins the vertex TWICE: with the CURRENT world bone palette (u_model[], set
 * via bgfx_set_transform like the color/shadow pass) for the current clip
 * position, and with the PREVIOUS frame's world bone palette (u_prevBones[]) for
 * the previous clip position.  Projecting both through the un-jittered cur/prev
 * view*proj and taking the NDC delta yields true per-limb motion vectors, so
 * even limb animation does not ghost in TAA.  Also emits the current world
 * normal (slot 0) so skinned meshes contribute real normals to SSR too.
 *
 * Bone-index decode is kept in lock-step with vs_pbr_skinned.sc /
 * vs_shadow_skinned.sc so the deformation matches the lit + shadowed mesh.
 */
uniform mat4 u_prevBones[BGFX_CONFIG_MAX_BONES];
uniform mat4 u_curViewProj;
uniform mat4 u_prevViewProj;

float decode_bone_index(float raw_index)
{
	float idx = raw_index;
	if (idx <= 1.0) {
		idx *= 255.0;
	}
	return floor(idx + 0.5);
}

int decode_bone_index_clamped(float raw_index)
{
	float idx = decode_bone_index(raw_index);
	if (idx < 0.0) {
		idx = 0.0;
	}
	float max_idx = float(BGFX_CONFIG_MAX_BONES - 1);
	if (idx > max_idx) {
		idx = max_idx;
	}
	return int(idx);
}

void main()
{
	int i0 = decode_bone_index_clamped(a_indices.x);
	int i1 = decode_bone_index_clamped(a_indices.y);
	int i2 = decode_bone_index_clamped(a_indices.z);
	int i3 = decode_bone_index_clamped(a_indices.w);

	/* Current frame skin matrix (world palette in u_model[]). */
	mat4 curSkin = a_weight.x * u_model[i0]
	             + a_weight.y * u_model[i1]
	             + a_weight.z * u_model[i2]
	             + a_weight.w * u_model[i3];

	/* Previous frame skin matrix (world palette in u_prevBones[]).
	 * scalar*matrix linear-blend skinning is portable (identical to the
	 * a_weight.x * u_model[...] pattern in vs_pbr_skinned.sc; u_model is a bgfx
	 * builtin so the linter doesn't flag it — u_prevBones is declared here). */
	mat4 prevSkin = a_weight.x * u_prevBones[i0]   // shader-lint: allow
	              + a_weight.y * u_prevBones[i1]   // shader-lint: allow
	              + a_weight.z * u_prevBones[i2]   // shader-lint: allow
	              + a_weight.w * u_prevBones[i3];  // shader-lint: allow

	vec3 curWpos  = mul(curSkin,  vec4(a_position, 1.0)).xyz;
	vec3 prevWpos = mul(prevSkin, vec4(a_position, 1.0)).xyz;

	vec4 curClip  = mul(u_curViewProj,  vec4(curWpos,  1.0));
	vec4 prevClip = mul(u_prevViewProj, vec4(prevWpos, 1.0));

	gl_Position = curClip;
	v_curClip   = curClip;
	v_prevClip  = prevClip;
	v_normal    = normalize(mul(curSkin, vec4(a_normal, 0.0)).xyz);
}
