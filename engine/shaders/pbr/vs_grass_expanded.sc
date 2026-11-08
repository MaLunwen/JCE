$input a_position, a_normal, a_tangent, a_texcoord0
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/* Ordinary vertex attributes carry a blade's root, scale and tint. */
#define JCE_GRASS_ROOT a_normal
#define JCE_GRASS_SCALE a_tangent.x
#define JCE_GRASS_TINT vec4(a_tangent.yzw, 1.0)
#include "vs_grass_body.sh"
