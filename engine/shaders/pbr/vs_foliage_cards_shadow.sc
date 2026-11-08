$input a_position, a_normal, a_texcoord0, a_tangent
$output v_texcoord0, v_normal, v_worldpos, v_localpos

#include <bgfx_shader.sh>
#include "shadow_pancake.sh"

#define JCE_FOLIAGE_EXPANDED_CARDS 1
#include "foliage_card_input.sh"
#include "vs_foliage_shadow_body.sh"
