/*
 * Object-ID ("pick") fragment shader for the editor pick pass.
 *
 * Takes NO interpolated varyings and writes a per-draw id colour (uniform).
 * Paired with vs_pick / vs_pick_skinned, which also output NO varyings, so the
 * vertex-output and fragment-input varying interfaces match exactly (both
 * empty) and bgfx_create_program links them. (bgfx requires an EXACT varying
 * interface match between VS and FS, not merely a subset — see jce_scene_pick.)
 * Living in the PBR shader set means a_indices/a_weight and BGFX_CONFIG_MAX_BONES
 * are available to the skinned variant.
 */
#include <bgfx_shader.sh>

uniform vec4 u_pickId;

void main()
{
    gl_FragColor = u_pickId;
}
