/*
 * fs_bloom_extract.sc  Extract bright pixels above threshold with soft knee.
 *
 * u_bloomParams.z = knee (0..1 relative to threshold).
 * knee=0 collapses to the original hard step(threshold) cutoff (byte-identical).
 * knee>0 uses a quadratic soft-knee ramp over [threshold-knee*t, threshold+knee*t]
 * (CoD/Karis formulation) for a smoother, ring-free bloom halo.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_bloomParams;   /* x=threshold y=intensity z=knee w=unused */

void main()
{
    vec3 color = texture2D(s_texColor, v_texcoord0).rgb;
    float brightness = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float threshold = u_bloomParams.x;
    float knee = u_bloomParams.z * threshold; /* knee width scaled to threshold */
    /* Soft-knee curve (CoD/Karis): smooth ramp across [threshold-knee, threshold+knee].
     * knee=0 collapses to step(threshold) * (brightness-threshold), i.e. the old hard cutoff. */
    float soft = brightness - threshold + knee;
    soft = clamp(soft, 0.0, 2.0 * knee);
    soft = (knee > 0.0) ? (soft * soft / (4.0 * knee + 1e-6)) : 0.0;
    float contribution = max(soft, brightness - threshold);
    contribution = max(contribution, 0.0);
    gl_FragColor = vec4(color * contribution, 1.0);
}
