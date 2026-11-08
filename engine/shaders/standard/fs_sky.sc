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

/* u_sky_params.x = mode  (0 = gradient, 1 = equirect HDR, 2 = Preetham,
 *                          3 = stylized dome, 4 = PHYSICAL atmosphere+clouds)
 * u_sky_params.y = exposure
 * u_sky_params.z = y-rotation (radians, equirect mode only)
 *
 * The dispatch below tests these in DESCENDING order and must stay that way --
 * see the note at the ladder.  This comment listed only 0-2 while modes 3 and 4
 * both existed, which is part of how mode 4 stayed unreachable unnoticed. */
uniform vec4 u_sky_params;

#if !defined(JCE_SKY_GL21_STOCK)
#include "sky_stylise.sh"
#endif

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
#if !defined(JCE_SKY_GL21_STOCK)
/* Baked atmospheric transmittance, 256x64 RGBA: u = (cos(sun zenith)+1)/2,
 * v = altitude / atmosphere height.  Sampled at TEXEL CENTRES -- see
 * jce_atmosphere_lut_uv; the half-texel matters most at the horizon, which is
 * exactly where transmittance changes fastest. */
SAMPLER2D(s_sky_transmittance, 1);
/* Baked cloud density, folded as a 2-D slice atlas (jce_cloud_atlas_uv).
 * A slice atlas rather than a 3-D texture because the charter's minimum
 * profile has no compute and the oldest targets have no 3-D sampling either;
 * the fold costs one extra mul per fetch and works everywhere. */
SAMPLER2D(s_sky_clouds, 2);
/* Hillaire multiple-scattering table, 32x32 RGBA: same axes as the
 * transmittance one -- u = (cos(sun zenith)+1)/2, v = altitude.
 *
 * Already normalised on the CPU by the same Rec.709 luminance beta_R below is
 * normalised by, and already carrying its own scattering coefficient, the
 * isotropic phase, the transmittance and the geometric series over every
 * further scattering order. So it is ADDED to the single-scatter term, not
 * multiplied into it, and it takes neither beta_R nor the Rayleigh phase --
 * doing either would count those twice. */
SAMPLER2D(s_sky_multiscatter, 3);
uniform vec4 u_cloud_params;   // x=coverage y=density z=bottom_km w=top_km
uniform vec4 u_cloud_atlas;    // x=dim_x y=dim_y z=dim_z w=tiles_x
uniform vec4 u_cloud_quality;  // x=step budget, yz=wind offset (tile units), w=cumulus material
uniform vec4 u_cloud_period;   // xy = 1/period_x, 1/period_z, per KILOMETRE
#endif

#include "sky_preetham_math.sh"

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

#if !defined(JCE_SKY_GL21_STOCK)
#include "sky_cloud_atlas.sh"
#include "sky_celestial.sh"

#include "sky_cloud_march.sh"

SAMPLER2D(s_sky_cloud_resolved, 7);
uniform vec4 u_cloud_temporal;  // x=main sky has a resolved cloud texture
uniform vec4 u_cloud_rt_params; // x=originBottomLeft
uniform mat4 u_cloud_sample_view_proj; // clean projection used by the half-res march

// Segment Beer transmittance from the actual atmosphere LUT. The CPU bake
// uses Earth radius6360km, top60km, Rayleigh8km/Mie1.2km scale heights,
// and per-km extinction (including Mie absorption and ozone). No normalized
// display coefficient is used as a physical extinction coefficient here.
vec3 sky_ground_segment_transmittance(vec3 eye_km, vec3 ray, out float distance_km)
{
    distance_km = -1.0;
    float b = dot(eye_km, ray) + JCE_CLOUD_PLANET_KM * ray.y;
    float c = dot(eye_km, eye_km) + 2.0 * JCE_CLOUD_PLANET_KM * eye_km.y;
    vec2 roots = cloud_sphere_roots(b, c);
    if (roots.x < 0.0 || roots.y < roots.x) return vec3_splat(1.0);
    distance_km = roots.x;
    if (distance_km < 0.0001) return vec3_splat(1.0);
    vec3 radial = eye_km + vec3(0.0, JCE_CLOUD_PLANET_KM, 0.0);
    float radius = length(radial);
    float altitude_km = max(c / (radius + JCE_CLOUD_PLANET_KM), 0.0);
    vec3 ground_up = (radial + ray * distance_km) / JCE_CLOUD_PLANET_KM;
    // The reverse ray passes from ground through the eye and then to space.
    // T(G,space) = T(G,eye) * T(eye,space), including varying air density.
    float ground_mu = clamp(dot(ground_up, -ray), -1.0, 1.0);
    float eye_mu = clamp(dot(radial / radius, -ray), -1.0, 1.0);
    vec2 ground_uv = vec2(((ground_mu * 0.5 + 0.5) * 255.0 + 0.5) / 256.0,
                          0.5 / 64.0);
    vec2 eye_uv = vec2(((eye_mu * 0.5 + 0.5) * 255.0 + 0.5) / 256.0,
        (clamp(altitude_km / 60.0, 0.0, 1.0) * 63.0 + 0.5) / 64.0);
#if BGFX_SHADER_LANGUAGE_HLSL || BGFX_SHADER_LANGUAGE_SPIRV || BGFX_SHADER_LANGUAGE_METAL
    vec3 ground_to_space = texture2DLod(s_sky_transmittance, ground_uv, 0.0).rgb;
    vec3 eye_to_space = texture2DLod(s_sky_transmittance, eye_uv, 0.0).rgb;
#else
    vec3 ground_to_space = texture2D(s_sky_transmittance, ground_uv).rgb;
    vec3 eye_to_space = texture2D(s_sky_transmittance, eye_uv).rgb;
#endif
    return clamp(ground_to_space / max(eye_to_space, vec3_splat(1e-8)),
                 vec3_splat(0.0), vec3_splat(1.0));
}
#endif

#if !defined(JCE_SKY_GL21_STOCK)
#include "sky_stylized_clouds.sh"
#endif

void main()
{
#include "sky_view_ray.sh"

    /* Sky mode dispatch, in DESCENDING threshold order.
     *
     * The order is load-bearing and its failure mode is silent.  This chain
     * previously ran 2.5, 1.5, 3.5, 0.5 -- so the `> 3.5` arm holding the
     * PHYSICAL mode was unreachable: every value above 2.5 was already
     * claimed by the stylized arm.  Selecting PHYSICAL did not error, warn,
     * or render black; it quietly drew the stylized dome, and the
     * transmittance LUT and the whole volumetric cloud march below never
     * executed once.
     *
     * Any new mode MUST be inserted in descending order.  A greater-than
     * ladder that is not sorted is not a dispatch, it is a shadowing bug that
     * looks like working code. */
#if !defined(JCE_SKY_GL21_STOCK)
    if (u_sky_params.x > 3.5) {
        /* Cloud silhouette term for the stylisation layer.  0 in open sky and
         * deep inside a cloud, 1 at the edge -- see where it is set below. */
        float cloud_rim = 0.0;
        float cloud_transmittance = 1.0;
        /* ── PHYSICAL: sample the baked transmittance table ─────────────
         *
         * The same table the CPU lighting authority reads, so the sky and the
         * light it casts come from ONE atmosphere.  What reaches the eye along
         * a view ray is the sunlight that survived the path to the sun,
         * modulated by how much of the sky that direction can see.
         *
         * v is the viewer altitude (0 = ground here; a flight model would feed
         * its own).  u is the SUN elevation, not the view elevation: the table
         * is indexed by where the sun is, and the view direction selects how
         * much of that light scatters toward the camera. */
        float mu   = clamp(u_sky_sun_dir.y, -1.0, 1.0);
        float u_lu = mu * 0.5 + 0.5;
        /* Texel centres: entry i of N sits at (i+0.5)/N. */
        u_lu = (u_lu * 255.0 + 0.5) / 256.0;
        float v_lu = 0.5 / 64.0;
        vec3 T = texture2D(s_sky_transmittance, vec2(u_lu, v_lu)).rgb;

        /* Rayleigh phase for the view-to-sun angle: this is what puts the
         * bright halo around the sun and keeps the opposite horizon blue. */
        float cosT  = clamp(dot(normalize(dir), normalize(u_sky_sun_dir.xyz)), -1.0, 1.0);
        float phase = 0.05968310365 * (1.0 + cosT * cosT);   /* 3/(16pi) */

        /* Grazing view rays traverse more air, so they scatter more toward the
         * camera -- this is why the horizon is bright and pale. */
        float up    = clamp(dir.y, 0.0, 1.0);
        float depth = 1.0 / max(up + 0.15, 0.15);

        /* Rayleigh scattering coefficient, luminance-normalised (see
         * engine/src/renderer/jce_rayleigh.h, which pins these exact figures
         * in a test).  This is the ONLY wavelength-dependent term in the
         * product, and therefore the only reason the sky is blue.
         *
         * It was missing, and the omission was self-concealing: the remaining
         * terms are grey except T, the TRANSMITTANCE -- what survives the
         * atmosphere rather than what scatters out of it.  T = exp(-beta*s)
         * and beta is largest for blue, so T is largest for RED, and the sky
         * rendered WARM.  Measured on a daytime capture: blue minus red
         * -14.8/255, where a Rayleigh sky owes about +40.  It kept a sensible
         * gradient and a sensible response to sun elevation the whole time,
         * so it read as a deliberate choice about the hour of the day.
         *
         * Normalised on LUMINANCE, not on max, so this is a hue correction and
         * not an exposure change: u_sky_params.y keeps exactly the meaning it
         * had, and skies that were correctly exposed stay correctly exposed. */
        vec3 beta_R = vec3(0.435585, 1.017867, 2.484984);

        /* MIE SCATTERING -- the warm forward glow around a low sun.
         *
         * The atmosphere module has carried a Mie coefficient since it was
         * written (jce_atmosphere.c: mie_scattering = 3.996e-3), and it is
         * folded into the EXTINCTION that builds the transmittance table and
         * into the multiple-scattering bake.  It was never in the SCATTERING
         * here, which is a specific and self-concealing kind of missing: Mie
         * dimmed the sky correctly and then contributed no light of its own.
         *
         * What that costs is exactly the picture this sky is aimed at.  Mie is
         * the aerosol term: grey rather than blue, and sharply forward-peaked,
         * so it is nearly invisible away from the sun and dominates everything
         * within ~20 degrees of it.  A sunset IS that lobe.  Without it a low
         * sun produces a blue sky that merely gets darker, which is why every
         * evening capture from this renderer reads as an underexposed noon.
         *
         * Normalised on the SAME divisor as beta_R above -- the Rec.709
         * luminance of the Rayleigh coefficients, 13.32e-3 -- so the two are
         * in one system of units and u_sky_params.y keeps its meaning:
         *   3.996e-3 / 13.32e-3 = 0.300
         * Grey, because aerosol scattering is very nearly wavelength-flat over
         * the visible range; that greyness against beta_R's blue is precisely
         * what turns the region near the sun white and leaves the rest blue. */
        vec3 beta_M = vec3(0.300, 0.300, 0.300);

        /* Henyey-Greenstein, g = 0.76.  The standard aerosol asymmetry for a
         * clear-to-hazy atmosphere; higher tightens the glow around the sun,
         * lower spreads it into general haze.  The max() keeps the forward
         * lobe finite: the denominator goes to (1-g)^3 = 0.0138 at cosT = 1,
         * which is small but not zero, so this is a guard against a
         * degenerate g rather than against the normal case. */
        float g_M    = 0.76;
        float g_M2   = g_M * g_M;
        float den_M  = 1.0 + g_M2 - 2.0 * g_M * cosT;
        float phaseM = 0.0795774715 * (1.0 - g_M2)
                     / pow(max(den_M, 1e-4), 1.5);   /* 1/(4pi) * ... */

        /* MULTIPLE SCATTERING.
         *
         * Single scattering alone leaves the horizon too dark and collapses
         * twilight to black, because at those angles most of the light
         * reaching the eye has bounced more than once. The table for this was
         * baked and unit-tested when the atmosphere module was written and
         * then never sampled by anything -- no texture, no uniform, no bind.
         *
         * Indexed by the SUN's elevation, like the transmittance fetch above:
         * both answer "given where the sun is, how much light is available
         * here", and the view direction then selects how much of it comes this
         * way. Viewer altitude is the ground row, matching v_lu above.
         *
         * `depth` is the same grazing path-length proxy the single-scatter
         * term uses, so the two scale together as the ray flattens instead of
         * the multiple-scattering term staying flat and taking over the
         * horizon. */
        float ms_u = (mu * 0.5 + 0.5) * 31.0 / 32.0 + 0.5 / 32.0;
        vec3  ms   = texture2D(s_sky_multiscatter, vec2(ms_u, 0.5 / 32.0)).rgb;

        /* Put the two scattering terms in the SAME UNITS before adding them.
         *
         * `beta_R * T * phase` is a scattering coefficient times a phase: a
         * quantity PER UNIT LENGTH.  `ms` is a path integral -- it already
         * carries the kilometres it was integrated over.  They were being
         * summed directly, so the multiple-scattering term arrived carrying a
         * spurious length factor of roughly one atmospheric scale height and
         * outweighed single scattering by about 20x.  Measured at this scene's
         * sun elevation: single (0.028, 0.050, 0.078) against ms
         * (0.490, 0.878, 1.667).
         *
         * A clear sky is mostly SINGLE scattering.  Getting that backwards
         * does not merely brighten the sky, it desaturates it: both terms are
         * blue, but the sum lands so far up the tonemap curve that every
         * channel compresses toward 1 and the ratio between them is destroyed.
         * Measured on a cloudless capture, blue minus red was +27 where a real
         * sky at that elevation owes +45 to +110; dropping the post exposure
         * from 0.85 to 0.30 recovered +50 from the very same frame, which is
         * what identifies this as a level problem rather than a hue one.
         *
         * airmass is `depth` renormalised so that the zenith is 1.0 -- the
         * same grazing proxy, now expressed as a multiple of the vertical
         * path instead of an arbitrary scale.  H_R is the Rayleigh scale
         * height, which is the length the vertical integral of an exponential
         * atmosphere collapses to. */
        float airmass = depth * 1.15;          /* 1.0 at zenith, 7.67 grazing */
        const float H_R = 8.0;                 /* km */
        vec3 single = (beta_R * phase + beta_M * phaseM) * T * H_R;

        /* u_sky_params.y is the sky's absolute scale, and it has never been
         * anchored to anything: it is JceSkyConfig.exposure, whose default of
         * 1.0 no scene has ever overridden, carried through jce_sky_evaluate
         * unchanged.  With the balance above corrected it needs a value, and
         * the value is chosen against a stated, re-measurable criterion rather
         * than by eye: a cloudless sky in this scene, viewed 30 degrees up and
         * away from a sun 34 degrees up, should land near blue-minus-red +50
         * in sRGB -- the figure the exposure ablation above produced from an
         * otherwise identical frame. */
        vec3 col = (single + ms) * airmass * u_sky_params.y * JCE_SKY_SCALE;
        if (u_cloud_quality.w > 0.5 && u_cloud_params.x > 0.001) {
            // Daytime cumulus look: deepen the clear air away from the sun.
            // The cloud source and atmospheric light LUT keep their own colors.
            float blue_sky = smoothstep(0.12, 0.45, mu) * smoothstep(0.08, 0.38, dir.y);
            blue_sky *= 1.0 - 0.65 * smoothstep(0.86, 0.99, cosT);
            col *= mix(vec3_splat(1.0), vec3(0.40, 0.51, 0.94), blue_sky);
        }
        // Sky is the fallback only where no scene geometry was drawn. A
        // real planetary intersection supplies the background ground distance,
        // rather than attaching a dark directional band to the camera.
        vec3 ground_eye_km = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz * 0.001;
        float ground_distance_km;
        vec3 air_transmittance = sky_ground_segment_transmittance(
            ground_eye_km, dir, ground_distance_km);
        if (ground_distance_km >= 0.0) {
            // Integrate the existing per-kilometre scattering source over
            // the finite segment. Multiplying an already blue path integral
            // by (1-T) again would count wavelength selection twice.
            vec3 middle = ground_eye_km + dir * (0.5 * ground_distance_km);
            float middle_altitude = max(cloud_world_altitude(middle), 0.0);
            float rayleigh_density = exp(-middle_altitude / H_R);
            float mie_density = exp(-middle_altitude / 1.2);
            vec3 source_per_km = (beta_R * (phase * rayleigh_density)
                + beta_M * (phaseM * mie_density)) * T + ms * (rayleigh_density / H_R);
            vec3 optical_depth = -log(max(air_transmittance, vec3_splat(1e-8)));
            vec3 optical_integral = (vec3_splat(1.0) - air_transmittance)
                / max(optical_depth, vec3_splat(0.0001));
            vec3 effective_length = ground_distance_km * mix(vec3_splat(1.0),
                optical_integral, step(vec3_splat(0.0001), optical_depth));
            col = u_sky_colors[2].rgb * 0.15 * air_transmittance
                + source_per_km * effective_length * (u_sky_params.y * JCE_SKY_SCALE);
        }

        vec4 cloud;
        BRANCH if (u_cloud_temporal.x > 0.5) {
            float y_sign = u_cloud_rt_params.x > 0.5 ? 0.5 : -0.5;
            vec4 clean_clip = mul(u_cloud_sample_view_proj, vec4(dir, 0.0));
            vec2 cloud_ndc = clean_clip.w > 0.0001 ? clean_clip.xy / clean_clip.w : v_texcoord0;
            vec2 cloud_uv = cloud_ndc * vec2(0.5, y_sign) + vec2_splat(0.5);
            cloud = texture2D(s_sky_cloud_resolved, cloud_uv);
            cloud_rim = clamp(4.0 * cloud.a * (1.0 - cloud.a), 0.0, 1.0);
        } else {
            vec3 camera_km = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz * 0.001;
            float distance_km;
            cloud = jce_cloud_march_with_rim(camera_km, dir, mu, T, ms,
                gl_FragCoord.xy, distance_km, cloud_rim);
        }
        col += sky_authored_night_gradient(dir);
        cloud_transmittance = cloud.a;
        col = col * cloud_transmittance + cloud.rgb * u_sky_params.y * JCE_SKY_SCALE;

        /* Celestial radiance originates behind the cloud layer. */
        if (sky_celestial_enabled()) {
            col += sky_celestial_radiance(dir, T)
                 * (u_sky_params.y * cloud_transmittance);
        }

        /* The stylisation layer grades the PHYSICAL core's radiance -- it does
         * not replace it.  That ordering is what lets the lighting authority
         * keep deriving sun colour from transmittance while the sky still
         * looks authored, and it is why the grade sits here rather than being
         * a fifth sky mode.
         *
         * `rim` is the cloud silhouette term: 0 in open sky and in the
         * interior of a cloud, 1 at its edge.  Fed from the march's own
         * coverage, so the emphasis follows the actual cloud shape rather than
         * a screen-space edge detect that would also outline the horizon. */
        gl_FragColor = vec4(sky_stylise(max(col, vec3_splat(0.0)),
                                        cloud_rim), 1.0);
    } else
#endif
    if (u_sky_params.x > 2.5) {
        /* ── Stylized dome (mode 3) ──────────────────────────────────
         * (1) multi-stop vertical ramp zenith→mid→horizon→ground
         * (2) additive horizon glow band
         * (3) sun disk + halo aligned to u_sky_sun_dir.
         * All linear; tonemap downstream applies gamma. */

#if !defined(JCE_SKY_GL21_STOCK)
        /* JCE_LOOKDEV_R3: real view ray, not authored anchor direction. */
        vec3 cloudViewRay = dir;
#endif

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

        /* Moon craters contribute only inside the disk. Avoid two 3D noise
         * evaluations for night pixels whose disk contribution is zero. */
        float craterMul = 1.0;
        if (nightness > 0.001 && disk > 0.0) {
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

#if !defined(JCE_SKY_GL21_STOCK)
        /* JCE_LOOKDEV_R3: same renderer cloud atlas in stylized sky. */
        vec3 cloudEyeKm = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz * 0.001;
        col = sky_stylized_clouds(col, cloudViewRay, cloudEyeKm);
#endif

        col *= u_sky_params.y;                /* exposure */
        col  = max(col, vec3_splat(0.0));
        gl_FragColor = vec4(col, 1.0);
    } else if (u_sky_params.x > 1.5) {
#include "sky_preetham_body.sh"
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
        /* LINEAR HDR out, exposure applied -- the same colour-space contract as
         * every other sky mode.
         *
         * This branch used to Reinhard tone-map AND apply pow(1/2.2) here,
         * while modes 0/2/3 output linear and rely on the downstream composite.
         * fs_composite.sc already does ACES/Neutral/AgX plus gamma, so an
         * equirect sky was tone-mapped twice and gamma-corrected twice --
         * washed out and crushed -- and, worse, merely SWITCHING sky mode
         * silently changed what the pass promised its consumer.  A physical
         * sky mode cannot be added on top of an ambiguous contract. */
        hdr *= u_sky_params.y;          /* exposure */
        hdr  = max(hdr, vec3_splat(0.0));
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
