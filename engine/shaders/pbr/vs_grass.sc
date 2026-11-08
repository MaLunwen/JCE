$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

#define JCE_GRASS_ROOT i_data3.xyz
#define JCE_GRASS_SCALE length(i_data0.xyz)
#define JCE_GRASS_TINT i_data4
#include "vs_grass_body.sh"
