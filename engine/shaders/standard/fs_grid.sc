$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_grid_camera;
uniform vec4 u_grid_fade;

float grid_line(vec2 world_xz, float spacing)
{
    vec2 scaled = world_xz / max(spacing, 1e-5);
    vec2 deriv = max(fwidth(scaled), vec2(1e-4, 1e-4));
    vec2 cell = abs(fract(scaled - 0.5) - 0.5) / deriv;
    vec2 cov = 1.0 - clamp(cell, vec2(0.0, 0.0), vec2(1.0, 1.0));
    /* Fade each axis by its own projected frequency.  The old narrow
       smoothstep cut off a whole family of lines across one screen region;
       this broad Gaussian roll-off has no finite LOD edge and suppresses
       sub-pixel lines before they can shimmer under camera motion. */
    vec2 lod = exp(-64.0 * deriv * deriv);
    return max(cov.x * lod.x, cov.y * lod.y);
}

float axis_line(float coord)
{
    float deriv = max(fwidth(coord) * 1.5, 1e-4);
    return 1.0 - clamp(abs(coord) / deriv, 0.0, 1.0);
}

void main()
{
    /* Two interior clip depths work on both [-1,1] and [0,1] backends.
       Unproject in view space first: dividing at the far plane and subtracting
       two large world positions loses precision in distant editor cameras. */
    vec4 nearH = mul(u_invProj, vec4(v_texcoord0, 0.0, 1.0));
    vec4 midH = mul(u_invProj, vec4(v_texcoord0, 0.5, 1.0));
    vec3 nearV = nearH.xyz / nearH.w;
    vec3 midV = midH.xyz / midH.w;
    vec3 nearP = mul(u_invView, vec4(nearV, 1.0)).xyz;
    vec3 dir = normalize(mul(u_invView, vec4(midV - nearV, 0.0)).xyz);

    float denom = dir.y;
    if (abs(denom) < 1e-5)
        discard;

    float t = -nearP.y / denom;
    if (t <= 0.0)
        discard;

    vec3 world = nearP + dir * t;
    vec2 xz = world.xz;

    float minor = grid_line(xz, u_grid_fade.w);
    float major = grid_line(xz, u_grid_fade.z);
    float axisX = axis_line(world.x);
    float axisZ = axis_line(world.z);

    vec3 color = vec3(0.52, 0.57, 0.64);
    color = mix(color, vec3(0.88, 0.34, 0.34), axisX);
    color = mix(color, vec3(0.34, 0.48, 0.95), axisZ);

    float dist = length(world - u_grid_camera.xyz);
    float fade = 1.0 - smoothstep(u_grid_fade.x, u_grid_fade.y, dist);
    /* Fade fully to 0 toward the horizon (no +floor): at grazing angles world
       coords explode and fract() precision dies, so any residual grid there is
       pure shimmer. */
    float angleFade = clamp(abs(dir.y) * 6.0, 0.0, 1.0);
    float grid = max(minor * u_grid_camera.w, major);
    float alpha = max(grid * 0.35, max(axisX, axisZ));
    alpha *= fade * angleFade;

    if (alpha <= 0.001)
        discard;

    gl_FragColor = vec4(color, alpha);
}
