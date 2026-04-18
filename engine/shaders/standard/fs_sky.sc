$input v_texcoord0

#include <bgfx_shader.sh>

/* Sky gradient colors:
 *   [0] = top     (zenith)
 *   [1] = horizon
 *   [2] = ground
 *
 * Each element is a vec4; only .rgb is used (alpha ignored).
 * Default values are set on the CPU side before each submit. */
uniform vec4 u_sky_colors[3];

/* u_sky_params.x = mode  (0 = gradient, 1 = equirect HDR)
 * u_sky_params.y = exposure
 * u_sky_params.z = y-rotation (radians) */
uniform vec4 u_sky_params;

SAMPLER2D(s_equirect, 0);

void main()
{
    /* Reconstruct world-space view ray direction. */
    vec4 nearH = mul(u_invViewProj, vec4(v_texcoord0, -1.0, 1.0));
    vec4 farH  = mul(u_invViewProj, vec4(v_texcoord0,  1.0, 1.0));
    vec3 dir   = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);

    if (u_sky_params.x > 0.5) {
        /* Equirectangular HDR sky. */
        /* Apply Y-axis rotation. */
        float cosR = cos(u_sky_params.z);
        float sinR = sin(u_sky_params.z);
        vec3 rd = vec3(cosR * dir.x + sinR * dir.z, dir.y,
                      -sinR * dir.x + cosR * dir.z);
        /* Spherical → UV. */
        float u = atan2(rd.x, -rd.z) * 0.1591549 + 0.5;  /* 1/(2*PI) */
        float v = asin(clamp(rd.y, -1.0, 1.0)) * 0.3183099 + 0.5; /* 1/PI */
        vec3 hdr = texture2D(s_equirect, vec2(u, 1.0 - v)).rgb;
        /* Simple Reinhard tone-map with exposure. */
        hdr *= u_sky_params.y;
        hdr = hdr / (hdr + vec3_splat(1.0));
        /* Gamma correction. */
        hdr = pow(hdr, vec3_splat(1.0 / 2.2));
        gl_FragColor = vec4(hdr, 1.0);
    } else {
        /* Procedural gradient mode. */
        float t        = clamp(dir.y, -1.0, 1.0);
        vec3 sky_col   = mix(u_sky_colors[1].rgb, u_sky_colors[0].rgb, clamp( t, 0.0, 1.0));
        vec3 gnd_col   = mix(u_sky_colors[1].rgb, u_sky_colors[2].rgb, clamp(-t, 0.0, 1.0));
        float above    = step(0.0, t);
        vec3 col       = mix(gnd_col, sky_col, above);
        gl_FragColor = vec4(col, 1.0);
    }
}
