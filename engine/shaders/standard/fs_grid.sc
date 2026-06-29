$input v_texcoord0

#include <bgfx_shader.sh>

uniform vec4 u_grid_camera;
uniform vec4 u_grid_fade;

float grid_line(vec2 world_xz, float spacing)
{
    vec2 scaled = world_xz / max(spacing, 1e-5);
    vec2 deriv = max(fwidth(scaled), vec2(1e-4, 1e-4));
    vec2 cell = abs(fract(scaled - 0.5) - 0.5) / deriv;
    float cov = 1.0 - clamp(min(cell.x, cell.y), 0.0, 1.0);
    /* Grid-LOD fade: once a cell shrinks below ~1px (deriv >= ~1) the lines are
       sub-pixel and alias/shimmer under camera motion (the far-grid "屏闪").
       Fade each level out as it becomes too dense to resolve, so the grid stays
       crisp up close and dissolves cleanly with distance instead of shimmering
       (the coarser major level survives further out).  ('line' is reserved in
       HLSL, so this local is 'cov'.) */
    float lod = 1.0 - smoothstep(0.5, 2.0, max(deriv.x, deriv.y));
    return cov * lod;
}

float axis_line(float coord)
{
    float deriv = max(fwidth(coord) * 1.5, 1e-4);
    return 1.0 - clamp(abs(coord) / deriv, 0.0, 1.0);
}

void main()
{
    vec4 nearH = mul(u_invViewProj, vec4(v_texcoord0, -1.0, 1.0));
    vec4 farH = mul(u_invViewProj, vec4(v_texcoord0, 1.0, 1.0));
    vec3 nearP = nearH.xyz / nearH.w;
    vec3 farP = farH.xyz / farH.w;
    vec3 dir = normalize(farP - nearP);

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

    vec3 color = vec3(0.33, 0.37, 0.43) * minor;
    color = mix(color, vec3(0.52, 0.57, 0.64), major);
    color = mix(color, vec3(0.88, 0.34, 0.34), axisX);
    color = mix(color, vec3(0.34, 0.48, 0.95), axisZ);

    float dist = length(world - u_grid_camera.xyz);
    float fade = 1.0 - smoothstep(u_grid_fade.x, u_grid_fade.y, dist);
    /* Fade fully to 0 toward the horizon (no +floor): at grazing angles world
       coords explode and fract() precision dies, so any residual grid there is
       pure shimmer. */
    float angleFade = clamp(abs(dir.y) * 6.0, 0.0, 1.0);
    float alpha = max(max(minor * 0.42, major * 0.95), max(axisX, axisZ));
    alpha *= fade * angleFade;

    if (alpha <= 0.001)
        discard;

    gl_FragColor = vec4(color, alpha);
}