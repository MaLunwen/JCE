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
 *   u_sky_dome_sun_col = (sun_color.rgb, pad)
 *   u_sky_dome_ray     = (ray_count, ray_length_rad, sharpness, strength) */
uniform vec4 u_sky_dome_mid;
uniform vec4 u_sky_dome_glow;
uniform vec4 u_sky_dome_sun;
uniform vec4 u_sky_dome_sun_col;
uniform vec4 u_sky_dome_ray;
/* Scene fog (same values the lit shaders receive; bound by the sky pass
 * only for the anchored-dome horizon blend below). */
uniform vec4 u_fogParams;    // x=mode (0 = off), y=density, z=start, w=end
uniform vec4 u_fogColor;     // xyz=fog color

SAMPLER2D(s_equirect, 0);

/* Perez F(theta,gamma): abcde = (A,B,C,D,E). Identical to sky_perez(). */
float sky_perez(vec4 abcd, float E, float cos_theta, float gamma)
{
    float cg = cos(gamma);
    return (1.0 + abcd.x * exp(abcd.y / cos_theta))
         * (1.0 + abcd.z * exp(abcd.w * gamma) + E * cg * cg);
}

/* ── Night-sky helpers (mode 3, dark-zenith scenes only) ──────────────
 * Stars + moon craters engage when the authored dome zenith is dark
 * (uniform-derived gate, so bright/day scenes are byte-identical). */
float sky_hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float sky_hash13(vec3 p)
{
    return fract(sin(dot(p, vec3(12.9898, 78.233, 45.164))) * 43758.5453);
}

/* Cheap trilinear 3-D value noise (for moon crater mottling). */
float sky_vnoise3(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = sky_hash13(i + vec3(0.0, 0.0, 0.0));
    float n100 = sky_hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = sky_hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = sky_hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = sky_hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = sky_hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = sky_hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = sky_hash13(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
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

        /* ORIGIN-ANCHORED dome (u_sky_dome_sun_col.w = sphere radius; 0 =
         * infinite view-direction dome).  The anchored mode intersects the
         * view ray with a world sphere.  Near its shell the mapping becomes
         * extremely distorted, and an outside camera can miss the sphere
         * entirely.  Fade to the infinite-sky direction before the shell and
         * use it directly for misses/outside cameras. */
        if (u_sky_dome_sun_col.w > 0.5) {
            float R          = u_sky_dome_sun_col.w;
            vec3 viewDir     = dir;
            vec3 camW        = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
            float camRadius2 = dot(camW, camW);
            float b2         = dot(camW, dir);
            float disc       = b2 * b2 - camRadius2 + R * R;
            if (disc > 0.0 && camRadius2 < R * R) {
                float t = -b2 + sqrt(disc);
                vec3 anchoredDir = normalize(camW + dir * t);
                float camRadius = sqrt(camRadius2);
                float anchorWeight = 1.0 -
                    smoothstep(R * 0.75, R * 0.95, camRadius);
                dir = normalize(mix(viewDir, anchoredDir, anchorWeight));
            }
        }

        float y    = clamp(dir.y, -1.0, 1.0);
        float midP = clamp(u_sky_dome_mid.w, 0.0, 1.0);

        /* Above horizon: horizon → mid → zenith via two smoothsteps. */
        float tLow  = smoothstep(0.0,  midP, max(y, 0.0));          /* horizon→mid */
        float tHigh = smoothstep(midP, 1.0,  max(y, 0.0));          /* mid→zenith  */
        vec3 above  = mix(u_sky_colors[1].rgb, u_sky_dome_mid.rgb, tLow);
        above       = mix(above, u_sky_colors[0].rgb, tHigh);

        /* Below horizon: horizon → ground with a CUBIC ramp (|y|^3, the
         * Elemental-Serenity skydome curve).  smoothstep leaked 6-20% of the
         * ground color into the just-below-horizon band every framed diorama
         * shot lives in (pitch -5..-10 deg), painting a distinct "sea" stripe
         * between the fog band and the sky that outlined the world edge; the
         * cubic keeps that band ~pure horizon color so the boundary
         * dissolves (ground color only shows at steep downward angles). */
        float yb    = -min(y, 0.0);
        vec3 below  = mix(u_sky_colors[1].rgb, u_sky_colors[2].rgb, yb * yb * yb);

        float aboveSel = step(0.0, y);
        vec3 col = mix(below, above, aboveSel);

        /* (2) Horizon glow band: brightest at the horizon, exp falloff. */
        float band = exp(-abs(y) * u_sky_dome_glow.w);
        col += u_sky_dome_glow.rgb * band;

        /* Nightness gate: uniform-only (authored zenith luminance), so
         * day/bright scenes take the exact pre-existing path. */
        float zlum      = dot(u_sky_colors[0].rgb, vec3(0.2126, 0.7152, 0.0722));
        float nightness = 1.0 - smoothstep(0.06, 0.18, zlum);

        /* (3) Sun/moon disk + halo (u_sky_sun_dir points toward it). */
        float cg   = clamp(dot(dir, u_sky_sun_dir.xyz), -1.0, 1.0);
        float disk = smoothstep(u_sky_dome_sun.x - u_sky_dome_sun.y,
                                u_sky_dome_sun.x + u_sky_dome_sun.y, cg);
        float halo = pow(max(cg, 0.0), u_sky_dome_sun.z);

        /* Moon craters (night only): value-noise mottling inside the disk. */
        float craterMul = 1.0;
        if (nightness > 0.001) {
            float cr  = sky_vnoise3(dir * 48.0) * 0.65
                      + sky_vnoise3(dir * 96.0) * 0.35;
            craterMul = mix(1.0, 0.58 + 0.42 * cr, nightness);
        }
        /* Accumulate the sun/moon disk + halo (and rays below) SEPARATELY so
         * the terrain-edge fog blend at the end does NOT paint over them: the
         * authored disk sits at y~-0.076 (just below the horizon, where the
         * reference moon glows), and the below-horizon fog fill would erase it
         * entirely — the "sun/moon can't be seen" regression.  Added back on
         * top of the fogged sky so it reads like a disc glowing through haze. */
        vec3 sunAdd = u_sky_dome_sun_col.rgb * (disk * craterMul + halo * u_sky_dome_sun.w);

        /* (3b) Stylized sun rays (count < 0.5 => bit-exact no-op).  Port of
         * the reference animeSun: cos(angle*count)^sharpness petal spokes
         * between sunSize*0.8 and sunSize+length (angular space, radians).
         * The petal angle is measured around the disk in the sun's own
         * right/up basis; cross() degenerates only for a zenith sun, which
         * the authored near-horizon disk never is. */
        if (u_sky_dome_ray.x > 0.5) {
            float aDist  = acos(cg);
            vec3  sright = normalize(cross(u_sky_sun_dir.xyz, vec3(0.0, 1.0, 0.0)));
            vec3  sup    = normalize(cross(sright, u_sky_sun_dir.xyz));
            vec3  offAx  = dir - u_sky_sun_dir.xyz * cg;
            float ang    = atan2(dot(offAx, sup), dot(offAx, sright));
            float petals = pow(cos(ang * u_sky_dome_ray.x) * 0.5 + 0.5,
                               u_sky_dome_ray.z);
            float sunRad = acos(clamp(u_sky_dome_sun.x, -1.0, 1.0));
            float rEnd   = sunRad + u_sky_dome_ray.y;
            float rayMsk = smoothstep(rEnd, sunRad * 0.8, aDist)
                         * smoothstep(sunRad * 0.5, sunRad * 0.8, aDist);
            sunAdd += u_sky_dome_sun_col.rgb *
                      (petals * rayMsk * u_sky_dome_ray.w);
        }

        /* (4) Star field (night only): hashed cells on an octahedral map
         * of the upper hemisphere (no trig — backend-safe). */
        if (nightness > 0.001) {
            float octK  = abs(dir.x) + abs(dir.y) + abs(dir.z);
            vec2  oct   = dir.xz / max(octK, 1e-4);
            vec2  g     = oct * 42.0;
            vec2  cell  = floor(g);
            vec2  fcell = fract(g);
            float h     = sky_hash12(cell);
            vec2  sp    = vec2(sky_hash12(cell + 17.0), sky_hash12(cell + 31.0));
            float sd    = length(fcell - sp);
            float star  = (1.0 - smoothstep(0.0, 0.09, sd)) * step(0.80, h);
            float fade  = smoothstep(-0.05, 0.25, dir.y);      /* horizon fade  */
            float bri   = 0.6 + 1.4 * fract(h * 7.31);          /* per-star size */
            col += vec3_splat(star * bri) * fade * nightness * (1.0 - disk);
        }

        /* Terrain-edge fog blend (anchored dome + scene fog only): the far
         * ground fades to u_fogColor by the fog end, but the DOME behind its
         * silhouette kept its own gradient — a visible three-band seam
         * (bright fogged ring / darker dome strip / sky) outlined the world
         * edge.  Paint the dome with the SAME fog color below the horizon
         * (the region only ever seen behind fully-fogged terrain) and fade
         * it out over ~10 deg above, so terrain dissolves into the sky the
         * way the reference reads.  Gated on the opt-in anchor so existing
         * scenes are byte-identical. */
        if (u_sky_dome_sun_col.w > 0.5 && u_fogParams.x > 0.5) {
            float w = (y < 0.0) ? 1.0 : exp(-y * 10.0);
            col = mix(col, u_fogColor.rgb, w);
        }

        /* Sun/moon on TOP of the fog blend so the near-horizon disc survives. */
        col += sunAdd;

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
