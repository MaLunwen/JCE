$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>
#include "shadow_pancake.sh"

void main()
{
    /* Clamped to the near plane, not rejected by it -- see
     * shadow_pancake.sh.  A caster further up-sun than the cascade
     * box reaches would otherwise be clipped away entirely and the
     * shadow it owes would be missing. */
    gl_Position = jce_shadow_pancake(mul(u_modelViewProj, vec4(a_position, 1.0)));
    v_texcoord0 = vec2(0.0, 0.0);
}
