$input a_position, a_normal, a_texcoord0, i_data0, i_data1
$output v_texcoord0, v_normal, v_worldpos, v_localpos

#include <bgfx_shader.sh>
#include "shadow_pancake.sh"

#include "foliage_card_input.sh"
#include "vs_foliage_shadow_body.sh"
