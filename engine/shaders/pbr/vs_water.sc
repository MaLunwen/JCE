$input a_position, a_normal, a_tangent, a_texcoord0
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/*
 * vs_water.sc -- GPU twin of jce_water.h (Gerstner / sum-of-sines surface).
 *
 * The CPU model in engine/src/middleware/scene/jce_water.c is the single
 * source of truth; the displacement and analytic-normal math below mirror it
 * EXACTLY (same phase = k*dot(d,(x,z)) + omega*t, same Gerstner roll, same
 * analytic normal).  The incoming a_position is a flat grid vertex in
 * entity-LOCAL space on the XZ plane at y = base_height; we apply the wave
 * displacement in WORLD space (after the model transform) so it matches the
 * CPU sampler, which works in world coordinates.
 *
 * Wave parameters arrive as uniforms (max 4 waves, packed 2 vec4 per wave):
 *   u_water_wave_a[i] = (amplitude, wavelength, speed, steepness)
 *   u_water_wave_b[i] = (dir_x, dir_z, 0, 0)   // direction is normalized here
 *   u_water_params.x  = active wave count [0..4]
 *   u_water_params.y  = base_height (world; the grid is generated at y=0 local)
 *   u_water_time.x    = elapsed time t (seconds)
 *
 * No raw mat3()/mat ctors here — the normal is computed analytically from the
 * wave partials, not via a TBN basis, so there is no per-backend transpose
 * hazard (see the JCE GL TBN bug note).
 *
 * ── FFT-ocean branch (u_water_mode.x > 0.5) ─────────────────────────────
 * Instead of the analytic Gerstner sum, the per-vertex displacement is fetched
 * from a CPU-generated Tessendorf displacement texture (s_water_disp, RGBA32F:
 * R=height/Y, G=disp_x, B=disp_z) that tiles a patch of side u_water_mode.y.
 * The vertex's world XZ wraps into the patch to a [0,1] UV; the position is
 * displaced (Y from R, XZ from G/B), and the normal is rebuilt from central
 * differences of the height channel.  ALL texture fetches use texture2DLod(...,0)
 * (the texture has no mips and vertex fetches have no implicit gradients — see
 * the fp_fetch / fs_pbr_body texture2DLod precedent), so it compiles cross-
 * backend.  The Gerstner branch is byte-identical to before.
 */

uniform vec4 u_water_wave_a[4]; // amplitude, wavelength, speed, steepness
uniform vec4 u_water_wave_b[4]; // dir_x, dir_z, _, _
uniform vec4 u_water_params;    // x=wave_count, y=base_height, z/w unused
uniform vec4 u_water_time;      // x=time
uniform vec4 u_water_mode;      // x=0 Gerstner / 1 FFT, y=patch_size
SAMPLER2D(s_water_disp, 0);     // FFT displacement (R=height,G=dispX,B=dispZ,A=foam)
/* Second cascade.  A single patch tiles and the eye locks onto the REPEAT
 * rather than the detail; two non-commensurate periods summed only repeat
 * where both do.  u_water_mode.W carries the second patch size (0 = absent); .z is
 * already the STYLIZED splash ratio, and overloading one slot across two
 * modes is a trap even when the modes are mutually exclusive. */
SAMPLER2D(s_water_disp2, 1);
/* The DISTURBANCE layer: what objects did to the water, as opposed to what the
 * wind is doing. R holds height in metres about the still surface.
 *
 * A separate texture rather than a channel of the displacement map above,
 * because that map exists only on the FFT path -- Gerstner and Stylized water
 * never allocate it -- and a ripple that only appeared on one of the three
 * wave models would be a worse defect than no ripple at all. This one is
 * sampled after the two branches converge, so all three get it.
 *
 * Addressed in WORLD XZ over the grid's own rectangle, not in patch UV: the
 * grid is anchored to the world (a ring stays where the rock fell), while the
 * ambient patch tiles. u_water_ripple carries that rectangle. */
SAMPLER2D(s_water_ripple, 2);
uniform vec4 u_water_ripple;   // xy = grid centre XZ, z = grid size (m), w = enabled

#define WATER_TWO_PI 6.28318530717958647692

void main()
{
    // World-space flat position of this grid vertex (model places + sizes it).
    vec3 flat_wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;

    // Surface base plane lives at base_height in world Y.
    float base_y = u_water_params.y;
    float x = flat_wpos.x;
    float z = flat_wpos.z;
    float t = u_water_time.x;

    // Displaced position + normal (filled by one of the two branches below).
    float px, py, pz;
    float nx, nz, ny;
    // 1.0 == undisturbed.  The Gerstner and STYLIZED branches never fold, so
    // this default IS their correct answer, not a placeholder.
    float foam_j = 1.0;

    // FFT ONLY at mode 1 — mode 2 (STYLIZED overlay) is a flat plane and
    // must fall through to the Gerstner branch with wave_count 0.
    if (u_water_mode.x > 0.5 && u_water_mode.x < 1.5)
    {
        // ── FFT ocean: sample the tiling displacement texture. ──────────
        // NB: `patch` is a RESERVED word in GLSL ES 3.0 (essl/WebGL2
        // tessellation keyword) — using it as an identifier compiles on
        // dx11/glsl/spv but hard-errors under 300_es.  Name it patchSize.
        float patchSize = u_water_mode.y;
        float invPatch = (patchSize > 0.0) ? (1.0 / patchSize) : 0.0;

        // Wrap world XZ into the patch -> [0,1] UV (fract handles the tiling).
        vec2 uv = fract(vec2(x, z) * invPatch);

        vec4 disp = texture2DLod(s_water_disp, uv, 0.0);

        /* Sum the second cascade at the SAME world XZ -- a different tiling of
         * one ocean, not a different place.  This MUST match what
         * jce_water_field_sample does on the CPU, or a floating body would sit
         * on a surface that is not the one being drawn. */
        float patch2 = u_water_mode.w;
        if (patch2 > 0.0) {
            vec2 uv2 = fract(vec2(x, z) * (1.0 / patch2));
            vec4 d2  = texture2DLod(s_water_disp2, uv2, 0.0);
            disp.xyz += d2.xyz;
            /* Folds compound: whichever cascade folds harder wins. */
            disp.w = min(disp.w, d2.w);
        }

        px = x + disp.y;          // horizontal X roll (G)
        pz = z + disp.z;          // horizontal Z roll (B)
        py = base_y + disp.x;     // height (R)
        // A = Jacobian of the horizontal map (see jce_water_fft.h).  < 1 is
        // compression, < 0 is a fold -- i.e. a breaking crest.  Carried to the
        // fragment stage so foam appears where the surface actually breaks
        // rather than wherever a noise function happens to be bright.
        foam_j = disp.w;

        // Normal from central differences of the height channel (R).  The
        // texel step in patch-UV; world step = patch * texelUV.  We don't know
        // the texture size here, so use a small fixed UV epsilon that is stable
        // across resolutions (the height field is smooth at this scale).
        float e = 1.0 / 256.0;
        float hL = texture2DLod(s_water_disp, fract(uv + vec2(-e, 0.0)), 0.0).x;
        float hR = texture2DLod(s_water_disp, fract(uv + vec2( e, 0.0)), 0.0).x;
        float hD = texture2DLod(s_water_disp, fract(uv + vec2(0.0, -e)), 0.0).x;
        float hU = texture2DLod(s_water_disp, fract(uv + vec2(0.0,  e)), 0.0).x;
        // World-space gradient: d(height)/d(world) = d(height)/d(uv) * invPatch.
        float dhdx = (hR - hL) * (0.5 / e) * invPatch;
        float dhdz = (hU - hD) * (0.5 / e) * invPatch;
        nx = -dhdx;
        nz = -dhdz;
        ny = 1.0;
    }
    else
    {
        int wave_count = int(u_water_params.x);

        // Gerstner displacement accumulators (start from the flat point).
        px = x;
        py = base_y;
        pz = z;

        // Analytic surface normal accumulators (see jce_water.h).
        nx = 0.0;
        nz = 0.0;
        ny = 1.0;

        for (int i = 0; i < 4; i++)
        {
            if (i >= wave_count) break;

            float amplitude  = u_water_wave_a[i].x;
            float wavelength = u_water_wave_a[i].y;
            float speed      = u_water_wave_a[i].z;
            float steepness  = u_water_wave_a[i].w;

            if (wavelength <= 0.0) continue;

            vec2 dir = u_water_wave_b[i].xy;
            float dlen2 = dot(dir, dir);
            if (dlen2 <= 1e-12) continue;
            dir = dir * inversesqrt(dlen2); // normalize(dir_x, dir_z)

            float k     = WATER_TWO_PI / wavelength; // angular wavenumber
            float omega = k * speed;                 // temporal frequency
            float phase = k * (dir.x * x + dir.y * z) + omega * t;
            float c     = cos(phase);
            float s     = sin(phase);
            float qa    = steepness * amplitude;
            float ka    = k * amplitude;

            // Displacement (matches jce_water_sample_displacement).
            px += qa * dir.x * c;
            pz += qa * dir.y * c;
            py += amplitude * s;

            // Normal (matches jce_water_sample_normal).
            nx -= dir.x * ka * c;
            nz -= dir.y * ka * c;
            ny -= steepness * ka * s;
        }
    }

    // Carry the fold determinant to the fragment stage.  COLOR0 is otherwise
    // unused by water, so this costs no new varying slot.
    v_tint = vec4(foam_j, 0.0, 0.0, 1.0);

    /* ── The disturbance, added to whichever branch produced this vertex ──
     *
     * surface = ambient + disturbance, which is the same sum rt_apply_buoyancy
     * computes for a floating body. Adding it HERE rather than inside the two
     * branches is what makes the two agree by construction: neither branch can
     * forget it, and a fourth wave model added later gets it for free.
     *
     * Outside the grid the sample must contribute nothing. The texture is
     * created with CLAMP, so a fetch beyond the rim repeats the edge texel --
     * which would smear the rim value across the whole ocean. The explicit
     * in-rect test is what makes "outside" mean ABSENT rather than "whatever
     * the edge happened to hold". */
    if (u_water_ripple.w > 0.5)
    {
        vec2 rmin = u_water_ripple.xy - vec2(u_water_ripple.z, u_water_ripple.z) * 0.5;
        vec2 ruv  = (vec2(px, pz) - rmin) / max(u_water_ripple.z, 1e-4);
        if (ruv.x >= 0.0 && ruv.x <= 1.0 && ruv.y >= 0.0 && ruv.y <= 1.0)
        {
            py += texture2DLod(s_water_ripple, ruv, 0.0).x;

            /* Perturb the normal too. A displaced surface whose normal still
             * points where it did is lit as though it were flat -- the ripple
             * would be there in silhouette and invisible in shading, which
             * reads as a geometry glitch rather than as a wave.
             *
             * Central differences in the same world units the ambient branch
             * uses: d(height)/d(world) = d(height)/d(uv) / size. */
            float re = 1.0 / 128.0;
            float rl = texture2DLod(s_water_ripple, vec2(ruv.x - re, ruv.y), 0.0).x;
            float rr = texture2DLod(s_water_ripple, vec2(ruv.x + re, ruv.y), 0.0).x;
            float rd = texture2DLod(s_water_ripple, vec2(ruv.x, ruv.y - re), 0.0).x;
            float ru = texture2DLod(s_water_ripple, vec2(ruv.x, ruv.y + re), 0.0).x;
            float inv = 1.0 / max(u_water_ripple.z, 1e-4);
            nx -= (rr - rl) * (0.5 / re) * inv;
            nz -= (ru - rd) * (0.5 / re) * inv;
        }
    }

    vec3 wpos = vec3(px, py, pz);
    vec4 viewPos = mul(u_view, vec4(wpos, 1.0));
    gl_Position  = mul(u_proj, viewPos);

    // Normalize the analytic normal (degenerate -> straight up).
    vec3 nrm = vec3(nx, ny, nz);
    float nl2 = dot(nrm, nrm);
    nrm = (nl2 > 1e-8) ? (nrm * inversesqrt(nl2)) : vec3(0.0, 1.0, 0.0);

    v_normal    = nrm;
    // Tangent/bitangent are unused by fs_water (analytic normal, no normal map)
    // but the varyings must be written for the shared PBR varying def.
    v_tangent   = vec3(1.0, 0.0, 0.0);
    v_bitangent = vec3(0.0, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
    v_localpos  = a_position;
    v_viewdepth = -viewPos.z;
}
