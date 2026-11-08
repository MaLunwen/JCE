$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_caustic_tile;    // atlas UV origin and extent, tile width in pixels
uniform vec4 u_caustic_channel; // unit R, G or B channel

void main()
{
    // A folded triangle still contributes irradiance. Use area magnitude.
    vec2 dx = dFdx(v_texcoord0);
    vec2 dy = dFdy(v_texcoord0);
    float inverse_area = abs(dx.x * dy.y - dx.y * dy.x);
    float concentration = inverse_area * u_caustic_tile.w * u_caustic_tile.w;
    float excess = clamp(concentration - 1.0, 0.0, 3.0);
    gl_FragColor = vec4(u_caustic_channel.rgb * excess, 0.0);
}
