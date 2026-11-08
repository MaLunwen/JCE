$input v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/* The colour/depth programs share vs_grass and its complete varying interface.
 * Current grass cards are opaque in fs_grass; this stage preserves the stock
 * fs_shadow depth semantics without changing its other vertex consumers.
 * Animated camera-facing geometry, wind and fade remain in the shared VS.
 */
void main()
{
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
}
