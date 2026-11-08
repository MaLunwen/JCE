$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_water_disp, 0);
uniform vec4 u_caustic_project;    // sample offset XZ and inverse period
uniform vec4 u_caustic_tile;       // atlas UV origin and extent, tile width in pixels

void main()
{
    vec2 uv = a_position.xz;
    vec2 sample_uv = uv + u_caustic_project.xy;
    vec2 horizontal = texture2DLod(s_water_disp, sample_uv, 0.0).gb;
    vec2 projected = uv + horizontal * u_caustic_project.z;
    vec2 clip = (u_caustic_tile.xy + projected * u_caustic_tile.z) * 2.0 - 1.0;
#if !BGFX_SHADER_LANGUAGE_GLSL
    clip.y = -clip.y;
#endif
    gl_Position = vec4(clip, 0.0, 1.0);
    v_texcoord0 = uv;
}
