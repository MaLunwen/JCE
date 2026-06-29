$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_outlineColor;  // xyz = outline color (linear), w = 1

void main()
{
    // Flat silhouette in LINEAR (downstream tonemap/gamma applies once).
    gl_FragColor = vec4(u_outlineColor.xyz, 1.0);
}
