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

/* u_sky_params.x = mode  (0 = gradient, 1 = equirect HDR, 2 = Preetham)
 * u_sky_params.y = exposure
 * u_sky_params.z = y-rotation (radians, equirect mode only) */
uniform vec4 u_sky_params;

/* ── Preetham analytic sky (mode 2) — GPU twin of jce_sky.c ──────────
 * Mirror of jce_sky_radiance(): keep this byte-aligned with the C math.
 *
 *   u_sky_perez[0] = Y channel A..D   (.x=A .y=B .z=C .w=D)
 *   u_sky_perez[1] = x channel A..D
 *   u_sky_perez[2] = y channel A..D
 *   u_sky_perez[3] = E coeffs         (.x=EY .y=Ex .z=Ey  .w unused)
 *   u_sky_zenith   = (.x=Yz .y=xz .z=yz  .w=normalize flag)
 *   u_sky_sun_dir  = unit vector toward the sun (.xyz) */
uniform vec4 u_sky_perez[4];
uniform vec4 u_sky_zenith;
uniform vec4 u_sky_sun_dir;

/* ── Stylized sky dome (mode 3, u_sky_params.x > 2.5) ─────────────────
 * Reuses u_sky_colors[0]=zenith, [1]=horizon, [2]=ground and
 * u_sky_sun_dir (toward the sun).  All-linear; no pow here.
 *   u_sky_dome_mid     = (mid.rgb, mid_pos)
 *   u_sky_dome_glow    = (glow.rgb, glow_falloff)
 *   u_sky_dome_sun     = (sun_size, sun_softness, halo_power, halo_strength)
 *   u_sky_dome_sun_col = (sun_color.rgb, pad) */
uniform vec4 u_sky_dome_mid;
uniform vec4 u_sky_dome_glow;
uniform vec4 u_sky_dome_sun;
uniform vec4 u_sky_dome_sun_col;

SAMPLER2D(s_equirect, 0);

/* Perez F(theta,gamma): abcde = (A,B,C,D,E). Identical to sky_perez(). */
float sky_perez(vec4 abcd, float E, float cos_theta, float gamma)
{
    float cg = cos(gamma);
    return (1.0 + abcd.x * exp(abcd.y / cos_theta))
         * (1.0 + abcd.z * exp(abcd.w * gamma) + E * cg * cg);
}

void main()
{
    /* Reconstruct world-space view ray direction.  The NDC near-plane z is
     * backend-dependent: OpenGL clip space is [-1,1] (near = -1) while
     * D3D/Vulkan/Metal are [0,1] (near = 0).  Unprojecting z = -1 on a [0,1]
     * backend yields a point OUTSIDE the frustum (negative w after the
     * homogeneous divide) → a garbage ray → the dome gradient samples the
     * wrong band → dark sky on D3D/VK (OpenGL stayed correct). */
#if BGFX_SHADER_LANGUAGE_GLSL
    float ndcNearZ = -1.0;
#else
    float ndcNearZ =  0.0;
#endif
    vec4 nearH = mul(u_invViewProj, vec4(v_texcoord0, ndcNearZ, 1.0));
    vec4 farH  = mul(u_invViewProj, vec4(v_texcoord0,      1.0, 1.0));
    vec3 dir   = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);

    if (u_sky_params.x > 2.5) {
        /* ── Stylized dome (mode 3) ──────────────────────────────────
         * (1) multi-stop vertical ramp zenith→mid→horizon→ground
         * (2) additive horizon glow band
         * (3) sun disk + halo aligned to u_sky_sun_dir.
         * All linear; tonemap downstream applies gamma. */
        float y    = clamp(dir.y, -1.0, 1.0);
        float midP = clamp(u_sky_dome_mid.w, 0.0, 1.0);

        /* Above horizon: horizon → mid → zenith via two smoothsteps. */
        float tLow  = smoothstep(0.0,  midP, max(y, 0.0));          /* horizon→mid */
        float tHigh = smoothstep(midP, 1.0,  max(y, 0.0));          /* mid→zenith  */
        vec3 above  = mix(u_sky_colors[1].rgb, u_sky_dome_mid.rgb, tLow);
        above       = mix(above, u_sky_colors[0].rgb, tHigh);

        /* Below horizon: horizon → ground. */
        vec3 below  = mix(u_sky_colors[1].rgb, u_sky_colors[2].rgb, smoothstep(0.0, 1.0, -y));

        float aboveSel = step(0.0, y);
        vec3 col = mix(below, above, aboveSel);

        /* (2) Horizon glow band: brightest at the horizon, exp falloff. */
        float band = exp(-abs(y) * u_sky_dome_glow.w);
        col += u_sky_dome_glow.rgb * band;

        /* (3) Sun disk + halo (u_sky_sun_dir points toward the sun). */
        float cg   = clamp(dot(dir, u_sky_sun_dir.xyz), -1.0, 1.0);
        float disk = smoothstep(u_sky_dome_sun.x - u_sky_dome_sun.y,
                                u_sky_dome_sun.x + u_sky_dome_sun.y, cg);
        float halo = pow(max(cg, 0.0), u_sky_dome_sun.z);
        col += u_sky_dome_sun_col.rgb * (disk + halo * u_sky_dome_sun.w);

        col *= u_sky_params.y;                /* exposure */
        col  = max(col, vec3_splat(0.0));
        gl_FragColor = vec4(col, 1.0);
    } else if (u_sky_params.x > 1.5) {
        /* ── Preetham analytic daylight (mode 2) ──────────────────── */
        /* theta = view-zenith angle; clamp cos just above 0 (sky hemi). */
        float cos_theta = clamp(dir.y, 0.01, 1.0);

        float cos_gamma = clamp(dot(dir, u_sky_sun_dir.xyz), -1.0, 1.0);
        float gamma     = acos(cos_gamma);

        float cos_ts  = clamp(u_sky_sun_dir.y, 0.01, 1.0);
        float theta_s = acos(cos_ts);

        /* F(0, theta_s): cos(0)=1, gamma=theta_s. */
        float fY0 = sky_perez(u_sky_perez[0], u_sky_perez[3].x, 1.0, theta_s);
        float fx0 = sky_perez(u_sky_perez[1], u_sky_perez[3].y, 1.0, theta_s);
        float fy0 = sky_perez(u_sky_perez[2], u_sky_perez[3].z, 1.0, theta_s);
        fY0 = (abs(fY0) < 1e-6) ? 1e-6 : fY0;
        fx0 = (abs(fx0) < 1e-6) ? 1e-6 : fx0;
        fy0 = (abs(fy0) < 1e-6) ? 1e-6 : fy0;

        float Y = u_sky_zenith.x * sky_perez(u_sky_perez[0], u_sky_perez[3].x, cos_theta, gamma) / fY0;
        float x = u_sky_zenith.y * sky_perez(u_sky_perez[1], u_sky_perez[3].y, cos_theta, gamma) / fx0;
        float y = u_sky_zenith.z * sky_perez(u_sky_perez[2], u_sky_perez[3].z, cos_theta, gamma) / fy0;

        Y = max(Y, 0.0);
        if (u_sky_zenith.w > 0.5) {
            float Yz = max(u_sky_zenith.x, 1e-6);
            Y /= Yz;
        }

        /* xyY -> XYZ (guard y). */
        y = max(y, 1e-4);
        float X = (x / y) * Y;
        float Z = ((1.0 - x - y) / y) * Y;

        /* XYZ -> linear sRGB (D65) — mirrors jce_sky_radiance(). */
        vec3 col;
        col.r =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
        col.g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
        col.b =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;

        col *= u_sky_params.y;          /* exposure */
        col  = max(col, vec3_splat(0.0));
        gl_FragColor = vec4(col, 1.0);
    } else if (u_sky_params.x > 0.5) {
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
