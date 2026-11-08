$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_fog, 0);
SAMPLER2D(s_screenShafts, 1);
uniform vec4 u_screenShaftTint;

void main()
{
    vec4 fog = texture2D(s_fog, v_texcoord0);
    vec2 shaft_uv = v_texcoord0;
#if BGFX_SHADER_LANGUAGE_GLSL
    /* The radial target is stored in native framebuffer orientation. */
    shaft_uv.y = 1.0 - shaft_uv.y;
#endif
    float shaft = texture2D(s_screenShafts, shaft_uv).r;
    fog.rgb += u_screenShaftTint.rgb * (shaft * (1.0 - fog.a));
    gl_FragColor = fog;
}
