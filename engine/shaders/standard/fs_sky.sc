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

#include "sky_stylise.sh"

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

/* Absolute scale of the PHYSICAL sky, see the derivation at its use site.
 * A single named constant rather than a number buried in an expression,
 * because it is the one figure in this shader that is calibrated against a
 * measurement instead of derived from the atmosphere. */
#define JCE_SKY_SCALE 0.21

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
uniform vec4 u_cloud_quality;  // x=step budget (<= the loop bound) yz=wind offset (tile units)
uniform vec4 u_cloud_period;   // xy = 1/period_x, 1/period_z, per KILOMETRE

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

/* Density of the cloud field at a world point, in the atlas's own terms.
 *
 * Lifted out of the view march so the LIGHT march can ask the same question.
 * Two copies of this addressing is exactly how the axis swap survived as long
 * as it did -- it was wrong in the march and right in the CPU bake, and there
 * was nothing to compare. */
/* One trilinear-in-XY, nearest-in-Z fetch of the folded atlas.
 *
 * Split out of cloud_density_at so the same addressing can be reused at a
 * second, higher frequency for the detail term below.  Two copies of this
 * arithmetic is exactly how the axis swap documented above survived. */
float cloud_atlas_fetch(float u01, float v01, float w01)
{
	float dimx = u_cloud_atlas.x, dimy = u_cloud_atlas.y;
	float dimz = u_cloud_atlas.z, tilesx = u_cloud_atlas.w;
	float slice = floor(clamp(w01, 0.0, 0.999) * dimz);
	float tcol  = mod(slice, tilesx);
	float trow  = floor(slice / tilesx);
	float aw    = dimx * tilesx;
	float ah    = dimy * ceil(dimz / tilesx);
	float px    = tcol * dimx + clamp(u01, 0.0, 1.0) * (dimx - 1.0) + 0.5;
	float py    = trow * dimy + clamp(v01, 0.0, 1.0) * (dimy - 1.0) + 0.5;
	return texture2D(s_sky_clouds, vec2(px / aw, py / ah)).r;
}

/* `detail` selects whether the high-frequency erosion runs.
 *
 * The light march passes 0.  It takes six samples for every view sample, so
 * giving it the second fetch would nearly double the cost of the whole layer
 * to sharpen a self-shadow that is then blurred by the phase function anyway.
 * The reference makes the same split: full detail on the view ray, cheap
 * sampling on the light ray. */
float cloud_density_at(vec3 wp, float bot, float top, float detail)
{
	float h01 = clamp((wp.y - bot) / (top - bot), 0.0, 1.0);
	/* Wrap XZ into the baked period; the bake tiles seamlessly.
	 *
	 * The factor was 0.05 per kilometre -- a declared period of
	 * 20 km. The atlas covers exactly ONE period of the field,
	 * which is 4096 m by default, so the sky was drawing the same
	 * clouds 4.88x too large while the ground sampled the CPU
	 * field at its true size. u_cloud_period carries 1/period.
	 *
	 * u_cloud_quality.yz is how far the wind has carried the
	 * field, already in tile units and already wrapped. Subtracting
	 * it moves the CLOUDS downwind rather than moving the world
	 * upwind -- the same picture with the opposite sign, and the
	 * sign is checkable, because the shadow bake subtracts the same
	 * displacement from the same world position. */
	float u01 = fract(wp.x * u_cloud_period.x - u_cloud_quality.y);
	float w01 = fract(wp.z * u_cloud_period.y - u_cloud_quality.z);

	/* Fold into the slice atlas.
	 *
	 * The bake writes slice = world Z, column = world X, row =
	 * NORMALISED ALTITUDE (jce_cloud_noise.c:655-667: zz from k,
	 * xx from i, h01 from j). This read had altitude selecting the
	 * slice and world Z selecting the row -- the two exchanged. A
	 * cloud's vertical structure was therefore laid out along Z and
	 * its Z extent along the height gradient, which is why the
	 * height profile and anvil shape were correct in the shadow
	 * bake, which samples the field directly, and wrong in the sky. */
	float v01 = clamp(h01, 0.0, 1.0);
	float d   = cloud_atlas_fetch(u01, v01, w01);

	/* Coverage carves the field: below the threshold there is no
	 * cloud at all, so raising coverage grows existing clouds
	 * rather than fading a uniform haze in. */
	/* Trim the noise floor ONLY.  Coverage is not applied here:
	 * it is baked into the atlas via the field's own coverage
	 * bias, which is the control that decides how much cloud
	 * exists at all.  This line used to threshold by coverage as
	 * well, so the slider was applied twice -- once to a field
	 * that had never heard of it (bias fixed at its default,
	 * leaving 13.8% of the volume non-zero and a peak density of
	 * 0.46) and again here, carving most of what remained.  The
	 * result was a slider that did nothing until it neared 1 and
	 * then produced wisps. */
	d = clamp((d - 0.02) / 0.98, 0.0, 1.0);

	/* HIGH-FREQUENCY EROSION.
	 *
	 * The comment on the atlas bake says the layer is deliberately small
	 * because "the DETAIL that makes a cloud edge look eroded comes from the
	 * shader's own high-frequency term, not from a larger bake".  There was no
	 * such term.  This function was one atlas fetch and a noise-floor trim, and
	 * the field's finest feature is 171 m (detail_freq 24 over a 4096 m
	 * period) -- so every cloud in this sky was a smooth blob at a scale where
	 * the reference has cauliflower.
	 *
	 * A second fetch of the SAME atlas at eight times the frequency supplies
	 * it without a second bake or a second texture.  The atlas is a coherent
	 * tiling 3D field, so read at 8x it is valid noise at 8x the frequency;
	 * what it is not is INDEPENDENT noise, so the offsets and the axis
	 * permutation below matter -- reading it at the same phase would carve
	 * every cloud with a scaled copy of its own silhouette.  The altitude axis
	 * is tiled only 3x because it spans 1 km against 4 km of XZ, and matching
	 * the horizontal rate there would alias against the height gradient.
	 *
	 * REMAP, not multiply -- this is the one detail the reference is explicit
	 * about: "Remapping prevents a loss of too much density at the core."
	 * (low - high*k) / (1 - high*k) subtracts the detail from the base and
	 * then renormalises, so a dense core stays dense while a thin edge is
	 * carved away.  A plain multiply would thin the whole cloud uniformly,
	 * which reads as lower coverage rather than as erosion. */
	if (detail > 0.5 && d > 0.0)
	{
		float du = fract(u01 * 8.0 + 0.37);
		float dv = fract(v01 * 3.0 + 0.11);
		float dw = fract(w01 * 8.0 + 0.61);
		/* Axis permutation: feed the horizontal coordinate into the atlas's
		 * altitude row and vice versa, so the detail carries none of the base
		 * field's vertical structure. */
		float hi = cloud_atlas_fetch(dw, du, dv);

		const float k = 0.42;
		float sub_ = hi * k;
		d = clamp((d - sub_) / max(1.0 - sub_, 1e-3), 0.0, 1.0);
	}
	return d;
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
    if (u_sky_params.x > 3.5) {
        /* Cloud silhouette term for the stylisation layer.  0 in open sky and
         * deep inside a cloud, 1 at the edge -- see where it is set below. */
        float cloud_rim = 0.0;
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
        /* Below the horizon fade to the ground colour rather than cutting. */
        col = mix(u_sky_colors[2].rgb * 0.15, col, smoothstep(-0.1, 0.05, dir.y));

        /* ── Volumetric clouds ──────────────────────────────────────────
         *
         * coverage 0 skips the march entirely, which is what keeps a scene
         * without clouds bit-identical rather than paying for a transparent
         * layer.  Only rays that actually rise into the slab march at all. */
        if (u_cloud_params.x > 0.001 && dir.y > 0.02)
        {
            float botKm = (u_cloud_params.z > 0.0) ? u_cloud_params.z : 1.5;
            float topKm = (u_cloud_params.w > botKm) ? u_cloud_params.w : 4.0;
            float dens  = (u_cloud_params.y > 0.0) ? u_cloud_params.y : 1.0;

            /* Flat-slab intersection: the camera is treated as being at
             * ground level, which is exact enough for a layer a few km up and
             * avoids a sphere intersection per pixel. */
            /* The camera's world position, in kilometres, so the layer can
             * be addressed in world space (see the march below). */
            vec3 camWKm = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz * 0.001;

            float t0 = botKm / dir.y;
            float t1 = topKm / dir.y;

            /* The loop BOUND is a compile-time constant -- a dynamic bound
             * does not compile on the oldest profile this shader targets -- so
             * the march always loops to the maximum and breaks on the budget.
             * dt is derived from the BUDGET, not the bound, so a cheaper tier
             * still traverses the whole slab with fewer, longer steps rather
             * than stopping part-way through the cloud. */
            const int STEPS_MAX = 24;
            float budget = max(u_cloud_quality.x, 1.0);
            float dt = (t1 - t0) / budget;

            /* Wrenninge's contrast approximation (SIGGRAPH 2015): each octave
             * i weakens the source (b^i), weakens extinction (a^i) and pushes
             * the phase toward ISOTROPIC (c^i * g).
             *
             * That last term is the whole point.  A single anisotropic term
             * makes a cloud dark everywhere it is not facing the sun.  Real
             * clouds are dark only at the THIN edges, where light scatters
             * once; deep inside, light has bounced so many times that the
             * field is isotropic and the interior is bright.  Summing octaves
             * with c^i -> 0 reproduces exactly that: rim anisotropic and dark,
             * core isotropic and bright. */
            float g = 0.6;
            vec3  scatter = vec3_splat(0.0);
            float trans   = 1.0;

            /* Break the march's own step boundary with a per-pixel offset.
             *
             * Every pixel sampled at exactly t0 + (si + 0.5) * dt, so every
             * pixel crossed the same density contour at the same place and
             * the layer came out in visible steps -- a low sample count shows
             * up as banding, and banding at a FIXED phase is the one kind a
             * viewer reads as a defect rather than as texture.
             *
             * Interleaved gradient noise (Jimenez): one dot product and a
             * fract, no texture, no state. It is a function of gl_FragCoord
             * alone, so the dither pattern is IDENTICAL every frame -- the
             * image stays deterministic, which matters because a temporal
             * jitter without TAA to resolve it trades banding for crawling,
             * and TAA is not on in every tier this has to run on.
             *
             * The offset is bounded to one step: sampling outside [t0, t0+dt)
             * would push the first sample past geometry the slab test already
             * decided was in front. */
            float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy,
                                                    vec2(0.06711056, 0.00583715))));

            for (int si = 0; si < STEPS_MAX; ++si)
            {
                if (float(si) >= budget) break;
                float t = t0 + (float(si) + ign) * dt;
                /* WORLD space, not camera space.
                 *
                 * This was `dir * t` -- the ray from the camera, with the
                 * camera's own world position never added.  The clouds
                 * therefore travelled with the viewer: you could not walk
                 * under one, and the layer was pinned to the eye like a
                 * skybox.  Worse, the cloud SHADOW on the ground is baked at
                 * true world XZ (jce_cloud_shadow.c), so the sky and its own
                 * shadow disagreed by exactly the camera's world position --
                 * a disagreement that CHANGED as the camera moved, which is
                 * the shape of error that reads as "the shadows are wrong"
                 * from every position except the origin.
                 *
                 * camWKm is metres/1000 because the slab, the periods and t
                 * are all in kilometres. The mul() form is the one the mode-3
                 * branch below already uses; raw element indexing of u_invView
                 * reads the transpose on one backend (see vs_foliage.sc). */
                /* XZ only. The slab intersection above keeps the existing
                 * "camera is at ground level" convention (t0 = botKm/dir.y),
                 * so adding the camera's world Y here would raise every sample
                 * altitude by the eye height while t0 still assumed zero --
                 * a second, quieter version of the same class of mismatch this
                 * change exists to remove. The reported defect is horizontal:
                 * the layer slid with the viewer in XZ while its ground shadow
                 * did not. */
                vec3  wp = vec3(camWKm.x, 0.0, camWKm.z) + dir * t;  /* km */

                float d = cloud_density_at(wp, botKm, topKm, 1.0);
                float sigma = d * dens;
                if (sigma <= 0.0) continue;

                /* Where this sample sits in the slab: 0 at the base, 1 at the
                 * top. Used by the ambient term below. */
                float h01s = clamp((wp.y - botKm) / max(topKm - botKm, 1e-4),
                                   0.0, 1.0);

                float sun_cos = clamp(dot(normalize(dir), normalize(u_sky_sun_dir.xyz)), -1.0, 1.0);

                /* Short march toward the sun: the self-shadow (capability B15).
                 *
                 * Six samples over one slab thickness, with the step growing
                 * geometrically. Uniform steps spend their whole budget in the
                 * first few hundred metres, which is where the density is most
                 * like the sample we already have; the far samples are what
                 * tell a thick cloud from a thin one, and a growing step is
                 * how they get reached without paying for the gap.
                 *
                 * Toward the sun, not along the light: u_sky_sun_dir is what
                 * the Henyey-Greenstein above already uses as the direction
                 * light arrives FROM, and a forward phase with g = 0.6 peaks
                 * where the two agree -- so the two terms cannot disagree
                 * about which way the sun is.
                 *
                 * The cost is real: six atlas fetches per view sample, on a
                 * march that already takes up to 24. It is gated on the same
                 * quality budget the view march uses, so the low tiers that
                 * take fewer view steps also take fewer light steps. */
                float sun_tau = 0.0;
                {
                    vec3  ldir  = normalize(u_sky_sun_dir.xyz);
                    float lstep = (topKm - botKm) * 0.25;
                    vec3  lp    = wp;
                    for (int li_ = 0; li_ < 6; ++li_)
                    {
                        if (float(li_) * 4.0 >= budget) break;
                        lp += ldir * lstep;
                        /* Above the slab there is nothing left to shadow. */
                        if (lp.y > topKm) break;
                        sun_tau += cloud_density_at(lp, botKm, topKm, 0.0)
                                 * dens * lstep;
                        lstep *= 1.5;
                    }
                }

                vec3 octave_sum = vec3_splat(0.0);
                float a = 1.0, b = 1.0, c = 1.0;
                for (int oi = 0; oi < 3; ++oi)
                {
                    /* Henyey-Greenstein with a per-octave eccentricity. */
                    float gi = g * c;
                    float den = 1.0 + gi * gi - 2.0 * gi * sun_cos;
                    float ph  = (1.0 - gi * gi) / (4.0 * 3.14159265 * pow(max(den, 1e-4), 1.5));

                    /* SILVER LINING -- a second, much tighter FORWARD lobe.
                     *
                     * Both lobes point forward and they are combined with
                     * max(), which is the reference's own formulation:
                     *   max(HG(cos, 0.6), silver * HG(cos, 0.99 - spread))
                     * It is not the forward+backward weighted sum that the
                     * generic literature uses; that shape cannot produce a
                     * bright rim on a cloud that is between you and the sun.
                     *
                     * A single g = 0.6 lobe is broad enough to look right at
                     * midday and visibly wrong at a low sun, which is exactly
                     * the picture this is aimed at: the reference's brightest
                     * feature is the blown-out edge where the sun is directly
                     * behind a cloud, and a broad lobe spreads that energy
                     * into general haze instead of concentrating it at the
                     * silhouette.
                     *
                     * Applied at the FIRST octave only. The deeper Wrenninge
                     * octaves exist to push the phase toward isotropic for the
                     * multiply-scattered interior; a tight forward spike there
                     * would undo the thing they are for. */
                    if (oi == 0)
                    {
                        const float sil_g = 0.91;   /* 0.99 - spread(0.08) */
                        const float sil_i = 0.70;
                        float dens_s = 1.0 + sil_g * sil_g - 2.0 * sil_g * sun_cos;
                        float ph_s = (1.0 - sil_g * sil_g)
                                   / (4.0 * 3.14159265 * pow(max(dens_s, 1e-4), 1.5));
                        ph = max(ph, sil_i * ph_s);
                    }
                    /* Light that survived to this point, per octave.
                     *
                     * This used to be exp(-sigma * a * dt * 4.0): the LOCAL
                     * sample's own density, times a 4 that stood for the
                     * distance to the top of the cloud without measuring it.
                     * A cloud therefore never shadowed itself -- the underside
                     * of a thick cumulus was exactly as bright as its sunlit
                     * crown, and the whole layer read flat.
                     *
                     * sun_tau is the real optical depth from this sample to
                     * the sun, marched below. Multiplying by `a` keeps the
                     * per-octave falloff the multiple-scattering approximation
                     * is built on: deeper octaves see a thinner medium, which
                     * is what makes the core bright instead of black. */
                    float li  = exp(-sun_tau * a);
                    octave_sum += T * b * ph * li;
                    a *= 0.5; b *= 0.5; c *= 0.5;
                }

                /* AMBIENT: the sky's own light falling on the cloud.
                 *
                 * The only source in this march was `T` -- sun transmittance,
                 * and sampled at GROUND altitude at that (v_lu = 0.5/64) for a
                 * layer 1.4 to 4.2 km up. A cloud lit by nothing but a
                 * collimated sun is black wherever the sun does not reach,
                 * which is what made this layer read as flat grey: the octave
                 * approximation was carrying the entire burden of not being
                 * black.
                 *
                 * `ms` is the multiple-scattered sky radiance already computed
                 * for this pixel, so the light the cloud receives from the sky
                 * is the same quantity the sky itself is drawn with -- one
                 * source, not a second invented constant.
                 *
                 * Ramped with height because the top of a cloud sees most of
                 * the hemisphere and the base sees mostly more cloud. That is
                 * the cheap stand-in for the reference's density-profile AO,
                 * and it is the dominant part of it. */
                vec3 ambient = ms * mix(0.15, 0.70, h01s);

                float step_t = exp(-sigma * dt);
                scatter += trans * (octave_sum + ambient) * sigma * dt;
                trans   *= step_t;
                if (trans < 0.01) break;
            }

            /* The silhouette is where the layer is PARTIALLY transparent:
             * fully clear sky and the opaque interior of a cloud are both
             * "not an edge".  4*t*(1-t) peaks at t=0.5 and vanishes at both
             * ends, which is the edge without a screen-space derivative --
             * and a derivative-based edge would also outline the horizon and
             * every other depth discontinuity, which is not a cloud. */
            cloud_rim = clamp(4.0 * trans * (1.0 - trans), 0.0, 1.0);

            /* Composite over the sky.  Fade the layer out toward the horizon
             * where the slab intersection degenerates. */
            float edge = smoothstep(0.02, 0.15, dir.y);
            /* JCE_SKY_SCALE applies here too.  The cloud's radiance is built
             * from T and ms -- the same two quantities the sky behind it is
             * built from -- so scaling one and not the other would leave the
             * clouds roughly five times brighter than the sky they sit in,
             * which is not a look, it is a unit mismatch wearing one. */
            col = col * mix(1.0, trans, edge)
                + scatter * u_sky_params.y * JCE_SKY_SCALE * edge;
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
    } else if (u_sky_params.x > 2.5) {
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
