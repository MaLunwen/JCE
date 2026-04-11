/*
 * fs_bloom_combine.sc  Additive blend of bloom texture onto scene.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);     /* scene color */
SAMPLER2D(s_texBloom, 1);     /* blurred bloom */
uniform vec4 u_bloomParams;   /* x=threshold, y=intensity, z=unused, w=unused */

void main()
{
    vec3 scene = texture2D(s_texColor, v_texcoord0).rgb;
    vec3 bloom = texture2D(s_texBloom, v_texcoord0).rgb;
    float intensity = u_bloomParams.y;

    gl_FragColor = vec4(scene + bloom * intensity, 1.0);
}
