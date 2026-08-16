#ifndef CK_KERR_LENSING_SOLVER_SH
#define CK_KERR_LENSING_SOLVER_SH

/*
 * Caged Kingdom Kerr null-geodesic renderer.
 *
 * Units use G = c = M = 1.  The spin parameter is therefore the signed
 * dimensionless Kerr spin chi.  Rays start in a locally nonrotating (ZAMO)
 * tetrad, are traced backward with the separated Carter potentials, and stop
 * only on an explicit horizon, escape, or optically thick disk event.
 * Nothing in this file is engine policy; it is CK project science content.
 */

#ifndef CK_KERR_MAX_STEPS
#define CK_KERR_MAX_STEPS 128
#endif

#ifndef CK_KERR_FORCE_DIAGNOSTIC
#define CK_KERR_FORCE_DIAGNOSTIC -1
#endif

#ifndef CK_KERR_FORCE_ACTIVE_STEPS
#define CK_KERR_FORCE_ACTIVE_STEPS -1
#endif

#define CK_KERR_PI 3.14159265358979323846
#define CK_KERR_TWO_PI 6.28318530717958647692
#define CK_KERR_EPS 1.0e-6
/* Volumetric disc.  The Page-Thorne model is a razor-thin sheet -- a
 * mathematical surface at theta = pi/2 with no extent in z -- so a ray either
 * crossed it or did not, and the image had hard silhouette edges and no
 * self-occlusion.  Giving it a gaussian vertical profile of scale height
 * H = (H/r) * r and marching emission/extinction through it produces
 * thickness, a soft limb, and the near side genuinely occluding the far side.
 *
 * H/r = 0.06 keeps it a THIN disc in the astrophysical sense (H/r << 1), so
 * the Page-Thorne flux profile it is emitting remains the right model; this
 * is giving the sheet its actual scale height, not switching to a thick
 * torus.  Optical depth is capped so a ray that saturates stops early -- that
 * cap is also what keeps the added cost bounded. */
#ifndef CK_KERR_DISC_H_OVER_R
#define CK_KERR_DISC_H_OVER_R 0.06
#endif

#ifndef CK_KERR_DISC_OPACITY
#define CK_KERR_DISC_OPACITY 0.85
#endif

#ifndef CK_KERR_DISC_EMISSION
#define CK_KERR_DISC_EMISSION 2.2
#endif

#ifndef CK_KERR_DISC_TAU_MAX
#define CK_KERR_DISC_TAU_MAX 3.0
#endif

#ifndef CK_KERR_AXIS_BLUR_SCALE
#define CK_KERR_AXIS_BLUR_SCALE 2.0
#endif

#ifndef CK_KERR_CUBE_FACE_TEXELS
#define CK_KERR_CUBE_FACE_TEXELS 1024.0
#endif

#ifndef CK_KERR_CUBE_PREFILTER_TEXELS
#define CK_KERR_CUBE_PREFILTER_TEXELS 128.0
#endif

#ifndef CK_KERR_DISC_DISPLAY_SCALE
#define CK_KERR_DISC_DISPLAY_SCALE 150.0
#endif

#ifndef CK_KERR_RETRY_COUNT
#define CK_KERR_RETRY_COUNT 12
#endif
#define CK_KERR_OUTCOME_INVALID 0.0
#define CK_KERR_OUTCOME_CAPTURE 1.0
#define CK_KERR_OUTCOME_ESCAPE 2.0
#define CK_KERR_OUTCOME_DISK 3.0
#define CK_KERR_OUTCOME_LIMIT 4.0

float ck_kerr_safe_rcp(float value)
{
    return 1.0 / max(abs(value), CK_KERR_EPS) * (value < 0.0 ? -1.0 : 1.0);
}

float ck_kerr_sign_nonzero(float value)
{
    return value < 0.0 ? -1.0 : 1.0;
}

float ck_kerr_horizon(float spin)
{
    return 1.0 + sqrt(max(0.0, 1.0 - spin * spin));
}

float ck_kerr_cuberoot(float value)
{
    return ck_kerr_sign_nonzero(value) * pow(abs(value), 1.0 / 3.0);
}

float ck_kerr_isco(float spin)
{
    float a2 = spin * spin;
    float z1 = 1.0 + ck_kerr_cuberoot(1.0 - a2) *
        (ck_kerr_cuberoot(1.0 + spin) + ck_kerr_cuberoot(1.0 - spin));
    float z2 = sqrt(max(0.0, 3.0 * a2 + z1 * z1));
    float branch = sqrt(max(0.0,
        (3.0 - z1) * (3.0 + z1 + 2.0 * z2)));
    float signed_branch = abs(spin) < 1.0e-7 ? 0.0 :
        (spin > 0.0 ? branch : -branch);
    return 3.0 + z2 - signed_branch;
}

void ck_kerr_metric(float r, float theta, float spin,
                    out float sigma, out float delta, out float area,
                    out float gtt, out float gtphi, out float gphiphi)
{
    float sin_theta = sin(theta);
    float cos_theta = cos(theta);
    float sin2 = sin_theta * sin_theta;
    float r2pa2 = r * r + spin * spin;
    sigma = r * r + spin * spin * cos_theta * cos_theta;
    delta = r * r - 2.0 * r + spin * spin;
    area = r2pa2 * r2pa2 - spin * spin * delta * sin2;
    gtt = -(1.0 - 2.0 * r / max(sigma, CK_KERR_EPS));
    gtphi = -2.0 * spin * r * sin2 / max(sigma, CK_KERR_EPS);
    gphiphi = area * sin2 / max(sigma, CK_KERR_EPS);
}

void ck_kerr_cartesian_to_bl(vec3 position, float spin,
                             out float r, out float theta, out float phi)
{
    float rho2 = dot(position, position);
    float a2 = spin * spin;
    float term = rho2 - a2;
    float r2 = 0.5 * (term + sqrt(max(0.0,
        term * term + 4.0 * a2 * position.y * position.y)));
    r = sqrt(max(r2, CK_KERR_EPS));
    theta = acos(clamp(position.y / r, -1.0, 1.0));
    phi = atan2(position.z, position.x);
}

vec3 ck_kerr_bl_to_cartesian(float r, float theta, float phi, float spin)
{
    float oblate = sqrt(max(CK_KERR_EPS, r * r + spin * spin));
    float sin_theta = sin(theta);
    return vec3(oblate * sin_theta * cos(phi),
                r * cos(theta),
                oblate * sin_theta * sin(phi));
}

void ck_kerr_spatial_basis(float r, float theta, float phi, float spin,
                           out vec3 radial, out vec3 polar, out vec3 azimuthal)
{
    float sigma = max(CK_KERR_EPS,
        r * r + spin * spin * cos(theta) * cos(theta));
    float root_sigma = sqrt(sigma);
    float oblate = sqrt(max(CK_KERR_EPS, r * r + spin * spin));
    float st = sin(theta);
    float ct = cos(theta);
    float cp = cos(phi);
    float sp = sin(phi);
    radial = vec3(r * st * cp, oblate * ct, r * st * sp) / root_sigma;
    polar = vec3(oblate * ct * cp, -r * st, oblate * ct * sp) / root_sigma;
    azimuthal = vec3(-sp, 0.0, cp);
}

float ck_kerr_radial_potential(float r, float spin, float lambda, float eta);
float ck_kerr_polar_potential(float theta, float spin,
                              float lambda, float eta);

bool ck_kerr_zamo_constants(vec3 observer_position, vec3 camera_ray,
                            float spin, out float r, out float theta,
                            out float phi, out float lambda, out float eta,
                            out float radial_sign,
                            out float polar_sign,
                            out float energy_at_infinity)
{
    r = 0.0;
    theta = 0.0;
    phi = 0.0;
    lambda = 0.0;
    eta = 0.0;
    radial_sign = -1.0;
    polar_sign = 1.0;
    energy_at_infinity = 0.0;
    ck_kerr_cartesian_to_bl(observer_position, spin, r, theta, phi);
    float sigma, delta, area, gtt, gtphi, gphiphi;
    ck_kerr_metric(r, theta, spin, sigma, delta, area,
                   gtt, gtphi, gphiphi);
    float sin_theta = sin(theta);
    float stationary_limit = 1.0 + sqrt(max(0.0,
        1.0 - spin * spin * cos(theta) * cos(theta)));
    if (delta <= CK_KERR_EPS || area <= CK_KERR_EPS ||
        abs(sin_theta) <= 1.0e-5 || r <= stationary_limit + 0.01)
        return false;

    vec3 er, etheta, ephi;
    ck_kerr_spatial_basis(r, theta, phi, spin, er, etheta, ephi);

    /* Trace from the observer into the viewed scene.  The local spatial
     * components match the CPU reference's inward ZAMO screen ray; the
     * separated equations carry only branch signs, not drifting velocities. */
    vec3 local_ray = normalize(camera_ray);
    float nr = dot(local_ray, er);
    float nt = dot(local_ray, etheta);
    float np = dot(local_ray, ephi);
    float lapse = sqrt(max(CK_KERR_EPS, sigma * delta / area));
    float omega = 2.0 * spin * r / area;
    float pt = 1.0 / lapse;
    float pr = nr * sqrt(max(0.0, delta / sigma));
    float ptheta = nt / sqrt(sigma);
    float pphi = omega / lapse + np *
        sqrt(max(0.0, sigma / area)) / sin_theta;
    float p_t = gtt * pt + gtphi * pphi;
    float p_phi = gtphi * pt + gphiphi * pphi;
    energy_at_infinity = -p_t;
    if (energy_at_infinity <= CK_KERR_EPS) return false;

    lambda = p_phi / energy_at_infinity;
    float ptheta_cov = sigma * ptheta / energy_at_infinity;
    float cos_theta = cos(theta);
    float sin2 = max(CK_KERR_EPS, sin_theta * sin_theta);
    eta = ptheta_cov * ptheta_cov + cos_theta * cos_theta *
        (lambda * lambda / sin2 - spin * spin);
    float radial = ck_kerr_radial_potential(r, spin, lambda, eta);
    float polar = ck_kerr_polar_potential(theta, spin, lambda, eta);
    float radial_scale = max(1.0, r * r * r * r);
    if (radial < -1.0e-5 * radial_scale || polar < -1.0e-5)
        return false;
    radial_sign = ck_kerr_sign_nonzero(pr);
    polar_sign = ck_kerr_sign_nonzero(ptheta);
    return true;
}

float ck_kerr_radial_potential(float r, float spin, float lambda, float eta)
{
    float delta = r * r - 2.0 * r + spin * spin;
    float p = r * r + spin * spin - spin * lambda;
    float q = eta + (lambda - spin) * (lambda - spin);
    return p * p - delta * q;
}

float ck_kerr_polar_potential(float theta, float spin,
                              float lambda, float eta)
{
    float s = sin(theta);
    float c = cos(theta);
    float sin2 = max(1.0e-8, s * s);
    return eta + spin * spin * c * c - lambda * lambda * c * c / sin2;
}

float ck_kerr_polar_u_potential(float polar_u, float spin,
                                float lambda, float eta)
{
    float u2 = polar_u * polar_u;
    return eta + (spin * spin - eta - lambda * lambda) * u2 -
        spin * spin * u2 * u2;
}

bool ck_kerr_phase_derivative(vec4 phase, float spin, float lambda, float eta,
                              out vec4 derivative, out float phi_velocity)
{
    float r = phase.x;
    float polar_u = phase.y;
    float u2 = polar_u * polar_u;
    float raw_sin2 = 1.0 - u2;
    float sin2 = max(1.0e-8, raw_sin2);
    float delta = r * r - 2.0 * r + spin * spin;
    float p = r * r + spin * spin - spin * lambda;
    float carter = eta + (lambda - spin) * (lambda - spin);

    derivative = vec4(0.0, 0.0, 0.0, 0.0);
    phi_velocity = 0.0;
    /* Only phi is singular on the axis; r/u obey the regular quartic polar
     * potential and must keep integrating.  The polar turning point sits at
     * 1 - u^2 ~ lambda^2/eta, below float resolution for |lambda| under ~5e-3,
     * so raw_sin2 cancels to the clamp floor there.  Refusing the step on that
     * condition starved the step control and the ray died at STEP_LIMIT; the
     * sin2 clamp above bounds the azimuth quadrature instead, and the
     * phi_velocity magnitude check below still rejects a true blow-up. */
    if (abs(delta) < 1.0e-6 || abs(polar_u) > 1.05)
        return false;
    derivative = vec4(
        phase.z,
        phase.w,
        2.0 * r * p - (r - 1.0) * carter,
        (spin * spin - eta - lambda * lambda) * polar_u -
            2.0 * spin * spin * polar_u * u2);
    phi_velocity = spin * p / delta + lambda / sin2 - spin;
    return abs(derivative.x) < 1.0e12 &&
           abs(derivative.y) < 1.0e12 &&
           abs(derivative.z) < 1.0e12 &&
           abs(derivative.w) < 1.0e12 &&
           abs(phi_velocity) < 1.0e12;
}

bool ck_kerr_phase_rk4(vec4 phase, float phi, float step,
                       float spin, float lambda, float eta,
                       out vec4 next_phase, out float next_phi)
{
    vec4 k1 = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 k2 = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 k3 = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 k4 = vec4(0.0, 0.0, 0.0, 0.0);
    float p1 = 0.0;
    float p2 = 0.0;
    float p3 = 0.0;
    float p4 = 0.0;

    next_phase = phase;
    next_phi = phi;
    if (!ck_kerr_phase_derivative(phase, spin, lambda, eta, k1, p1))
        return false;
    if (!ck_kerr_phase_derivative(phase + 0.5 * step * k1,
                                  spin, lambda, eta, k2, p2))
        return false;
    if (!ck_kerr_phase_derivative(phase + 0.5 * step * k2,
                                  spin, lambda, eta, k3, p3))
        return false;
    if (!ck_kerr_phase_derivative(phase + step * k3,
                                  spin, lambda, eta, k4, p4))
        return false;
    next_phase = phase + step *
        (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
    next_phi = phi + step * (p1 + 2.0 * p2 + 2.0 * p3 + p4) / 6.0;
    return abs(next_phase.x) < 1.0e12 &&
           abs(next_phase.y) < 1.0e6 &&
           abs(next_phase.z) < 1.0e12 &&
           abs(next_phase.w) < 1.0e12 &&
           abs(next_phi) < 1.0e12;
}

bool ck_kerr_fold_polar_axis(float lambda,
                             inout vec4 phase, inout float phi)
{
    if (abs(phase.y) <= 1.0) return true;
    if (abs(lambda) > 1.0e-4 || abs(phase.y) > 1.05) return false;
    phase.y = phase.y > 1.0 ? 2.0 - phase.y : -2.0 - phase.y;
    phase.w = -phase.w;
    phi += CK_KERR_PI;
    return true;
}

float ck_kerr_phase_residual(vec4 phase, float spin, float lambda, float eta)
{
    float radial = ck_kerr_radial_potential(phase.x, spin, lambda, eta);
    float polar = ck_kerr_polar_u_potential(phase.y, spin, lambda, eta);
    float radial_scale = max(1.0,
        phase.x * phase.x * phase.x * phase.x);
    float radial_error = abs(phase.z * phase.z - radial) / radial_scale;
    float polar_error = abs(phase.w * phase.w - polar);
    return max(radial_error, polar_error);
}

float ck_kerr_phase_step(vec4 phase, float spin, float lambda, float eta,
                         vec4 integration)
{
    vec4 derivative = vec4(0.0, 0.0, 0.0, 0.0);
    float phi_velocity = 0.0;
    float radial_scale = max(phase.x, 1.0);
    float polar_radius = sqrt(max(1.0e-8,
        1.0 - phase.y * phase.y));

    if (!ck_kerr_phase_derivative(phase, spin, lambda, eta,
                                  derivative, phi_velocity))
        return 1.0e-6;
    float bound = integration.x * radial_scale /
        max(abs(phase.z), CK_KERR_EPS);
    bound = min(bound, integration.y / max(abs(phase.w), CK_KERR_EPS));
    bound = min(bound, integration.z /
        max(abs(phi_velocity) * polar_radius, CK_KERR_EPS));
    bound = min(bound, sqrt(2.0 * integration.x * radial_scale /
        max(abs(derivative.z), CK_KERR_EPS)));
    bound = min(bound, sqrt(2.0 * integration.y /
        max(abs(derivative.w), CK_KERR_EPS)));
    return clamp(0.85 * bound, 1.0e-6, 0.25);
}

float ck_kerr_project_velocity(float potential, float velocity,
                               float acceleration)
{
    float direction = abs(velocity) >= 1.0e-6 ? velocity : acceleration;
    if (abs(direction) < 1.0e-6) direction = 1.0;
    float magnitude = sqrt(max(0.0, potential));
    return direction < 0.0 ? -magnitude : magnitude;
}

float ck_kerr_redshift(float disk_r, float spin, float lambda,
                       float eta, float energy_at_infinity,
                       out float emission_cosine)
{
    float omega = 1.0 / max(1.0e-5, pow(disk_r, 1.5) + spin);
    float sigma, delta, area, gtt, gtphi, gphiphi;
    ck_kerr_metric(disk_r, 0.5 * CK_KERR_PI, spin,
                   sigma, delta, area, gtt, gtphi, gphiphi);
    float norm = -(gtt + 2.0 * gtphi * omega +
                   gphiphi * omega * omega);
    if (norm <= CK_KERR_EPS) {
        emission_cosine = 0.0;
        return 0.0;
    }
    float ut = inversesqrt(norm);
    float emitter_frequency = ut * (1.0 - omega * lambda);
    if (emitter_frequency <= CK_KERR_EPS ||
        energy_at_infinity <= CK_KERR_EPS) {
        emission_cosine = 0.0;
        return 0.0;
    }
    emission_cosine = clamp(sqrt(max(0.0, eta)) /
        max(CK_KERR_EPS, disk_r * emitter_frequency), 0.0, 1.0);
    return clamp(1.0 /
        (energy_at_infinity * emitter_frequency), 0.0, 8.0);
}

vec3 ck_kerr_rotate_quaternion(vec3 value, vec4 quaternion)
{
    vec3 qv = quaternion.xyz;
    return value + 2.0 * cross(qv,
        cross(qv, value) + quaternion.w * value);
}

vec2 ck_kerr_temporal_jitter(float phase)
{
    float sample_index = mod(floor(max(phase, 0.0)), 8.0);
    if (sample_index < 0.5) return vec2(0.0, -0.1666667);
    if (sample_index < 1.5) return vec2(-0.25, 0.1666667);
    if (sample_index < 2.5) return vec2(0.25, -0.3888889);
    if (sample_index < 3.5) return vec2(-0.375, -0.0555556);
    if (sample_index < 4.5) return vec2(0.125, 0.2777778);
    if (sample_index < 5.5) return vec2(-0.125, -0.2777778);
    if (sample_index < 6.5) return vec2(0.375, 0.0555556);
    return vec2(-0.4375, 0.3888889);
}

/* Direction -> 3x2 cube-atlas uv.  Face order +X, -X, +Y, -Y, +Z, -Z;
 * column = face % 3, row = face / 3.  This is the exact inverse of
 * ck_cube_face_direction in ck_kerr_asset_generator.c and the two sign tables
 * must stay in lockstep, otherwise stars render in the wrong part of the sky.
 *
 * Replaces an equirectangular lookup whose poles were a genuine coordinate
 * singularity: longitude is undefined on the axis, and texel solid angle
 * collapses as sin(theta), so angular resolution varied by latitude and the
 * spin axis aliased.  A cube map is uniform to within ~1.3x everywhere.
 *
 * uv is inset by half a texel so bilinear taps never cross a face boundary --
 * the atlas puts unrelated faces side by side, so the usual wrap/clamp modes
 * cannot protect the seams and the inset has to be explicit. */
vec2 ck_kerr_cube_atlas_uv(vec3 d, float face_texels)
{
    vec3 a = abs(d);
    float ma;
    vec2 st;
    float face;

    if (a.x >= a.y && a.x >= a.z) {
        ma = a.x;
        st = (d.x > 0.0) ? vec2(-d.z, -d.y) : vec2(d.z, -d.y);
        face = (d.x > 0.0) ? 0.0 : 1.0;
    } else if (a.y >= a.z) {
        ma = a.y;
        st = (d.y > 0.0) ? vec2(d.x, d.z) : vec2(d.x, -d.z);
        face = (d.y > 0.0) ? 2.0 : 3.0;
    } else {
        ma = a.z;
        st = (d.z > 0.0) ? vec2(d.x, -d.y) : vec2(-d.x, -d.y);
        face = (d.z > 0.0) ? 4.0 : 5.0;
    }

    vec2 uv = 0.5 * (st / max(ma, 1.0e-8) + vec2(1.0, 1.0));
    float inset = 0.5 / max(face_texels, 1.0);
    uv = clamp(uv, vec2(inset, inset), vec2(1.0 - inset, 1.0 - inset));
    return vec2((floor(mod(face, 3.0)) + uv.x) * (1.0 / 3.0),
                (floor(face / 3.0) + uv.y) * 0.5);
}

vec3 ck_kerr_sample_background(vec3 direction, vec4 orientation,
                               float image_order, float accepted_steps,
                               float lambda)
{
    vec3 d = normalize(ck_kerr_rotate_quaternion(direction, orientation));
    vec2 uv = ck_kerr_cube_atlas_uv(d, CK_KERR_CUBE_FACE_TEXELS);
    vec2 uv_prefiltered =
        ck_kerr_cube_atlas_uv(d, CK_KERR_CUBE_PREFILTER_TEXELS);
    /* A cube face carries CK_KERR_CUBE_FACE_TEXELS texels across 90 degrees,
     * uniformly over the whole sphere, so the footprint is now just the pixel
     * solid angle in texels -- no latitude term, because there is no longer a
     * latitude-dependent stretch to correct.
     *
     * KNOWN LIMITATION, unchanged by the cube map: the speckled column along
     * the spin axis is a strong-lensing CAUSTIC, not a projection artefact.
     * Measured -- outcome mode over that column is clean, CPU rays at
     * screen_x = 0 resolve with zero rejected steps, and a seam probe showed
     * the column spans the full u range within a few pixels.  Neighbouring
     * pixels there map to genuinely unrelated points on the sky, which no
     * choice of projection fixes; it needs a real magnification-aware
     * footprint.  dFdx would supply one but is illegal inside this ray-march
     * loop (D3DCompile X3570/X3511), so the remaining fix is to carry the
     * footprint analytically through the integration. */
    float angular_pixel = 2.0 * u_jceCameraProjection.x /
        max(u_jceOutputViewport.y, 1.0);
    float base_footprint = angular_pixel * CK_KERR_CUBE_FACE_TEXELS /
        (0.5 * CK_KERR_PI);
    /* Azimuth-compression term.  Rays along the spin axis carry lambda ~ 0 and
     * move in a plane containing the axis, so the SIGN of lambda decides which
     * side they pass -- and the final azimuth therefore flips by pi across
     * lambda = 0.  Two adjacent screen columns then sample points 180 degrees
     * apart on the sky, which is why the axis renders as a dense speckled
     * column: a wide patch of a finite star catalogue compressed into a few
     * pixels.  Real sky is continuous there, so the honest reconstruction is
     * the PREFILTERED map, whose 8x8 box average restores continuity.
     *
     * Scale set from measurement, not taste: the column is ~25 px wide, one
     * pixel is d(lambda) ~ 32/2560 = 0.0125, so it spans |lambda| < ~0.16 and
     * the blur has to be saturated by then.  An earlier attempt used 0.20 and
     * only bit below |lambda| < 0.005 -- 0.4 px -- which is why it measured as
     * doing nothing.  Tracking the closest approach to the photon sphere was
     * also tried and measured WORSE: it blurred the lensing arcs, whose rays do
     * grazie the hole, while leaving the column untouched, whose rays do not. */
    float axis_blur = clamp(CK_KERR_AXIS_BLUR_SCALE /
        max(abs(lambda), 1.0e-3), 1.0, 200.0);
    base_footprint *= axis_blur;
    float path_complexity = 1.0 + 3.0 * clamp(
        accepted_steps / float(CK_KERR_MAX_STEPS), 0.0, 1.0);
    float footprint_texels = base_footprint * path_complexity *
        exp2(min(image_order, 4.0));
    float prefiltered_weight = smoothstep(1.0, 8.0, footprint_texels);
    vec3 resolved = texture2DLod(s_jceUser0, uv, 0.0).rgb;
    vec3 prefiltered = texture2DLod(s_jceUser3, uv_prefiltered, 0.0).rgb;
    return mix(resolved, prefiltered, prefiltered_weight);
}

vec3 ck_kerr_disk_radiance(float disk_r, float spin, float lambda, float eta,
                           float energy_at_infinity, vec4 disk_events,
                           float luminosity, vec4 spectral, vec4 lut_axes,
                           out float redshift)
{
    float r_isco = ck_kerr_isco(spin);
    float radial_u = clamp(log(max(disk_r, r_isco) / r_isco) /
        max(CK_KERR_EPS, log(disk_events.x / r_isco)), 0.0, 1.0);
    float spin_v = clamp((spin - lut_axes.x) * lut_axes.y /
        max(1.0, lut_axes.w), 0.0, 1.0);
    vec4 flux_sample = texture2DLod(s_jceUser1,
        vec2(radial_u, spin_v), 0.0);
    float flux = max(0.0, flux_sample.x);
    float emitted_temperature = mix(spectral.x, spectral.y,
        clamp(pow(max(flux, 1.0e-8), 0.25), 0.0, 1.0));
    float emission_cosine;
    redshift = ck_kerr_redshift(disk_r, spin, lambda, eta,
                                energy_at_infinity, emission_cosine);
    float observed_temperature = clamp(
        redshift * emitted_temperature, 1000.0, 40000.0);
    float temperature_u = (observed_temperature - 1000.0) / 39000.0;
    vec3 source_color = texture2DLod(s_jceUser2,
        vec2(temperature_u, 0.5), 0.0).rgb;
    float limb = mix(1.0, emission_cosine, clamp(spectral.z, 0.0, 1.0));
    /* Display calibration.  The Page-Thorne flux LUT is normalised to its own
     * maximum (ck_kerr_asset_generator.c divides by `maximum`), so `flux` is
     * dimensionless in [0,1] and carries no absolute radiance.  The celestial
     * background, by contrast, is authored in arbitrary units whose brightest
     * stars reach ~18.  Without a scale the disc peaks near 0.02 and lands
     * BELOW the Milky Way band -- measured 3-8/255 against a 4-6/255
     * background -- so the physically dominant source rendered as noise.
     * This factor puts a 0.1-Eddington disc above the star field, as it must
     * be for a 1e8 solar-mass hole.  It is a DISPLAY calibration between two
     * arbitrary-unit sources, not an absolute radiance, and the diagnostic
     * modes (redshift/steps/residual) are unaffected by it. */
    float observed = flux * max(luminosity, 0.0) *
        pow(redshift, 4.0) * limb * CK_KERR_DISC_DISPLAY_SCALE;
    return source_color * observed;
}

vec3 ck_kerr_outcome_color(float outcome)
{
    if (outcome < 0.5) return vec3(1.0, 0.05, 0.7);
    if (outcome < 1.5) return vec3(0.015, 0.02, 0.03);
    if (outcome < 2.5) return vec3(0.08, 0.35, 0.95);
    if (outcome < 3.5) return vec3(1.0, 0.52, 0.08);
    return vec3(0.8, 0.0, 0.0);
}

vec3 ck_kerr_diagnostic(float mode, float outcome, float redshift,
                        float image_order, float accepted_steps,
                        float max_residual)
{
    if (mode < 1.5) return ck_kerr_outcome_color(outcome);
    if (mode < 2.5) {
        float g = clamp(redshift / 2.0, 0.0, 1.0);
        return vec3(clamp(2.0 - 2.0 * g, 0.0, 1.0),
                    1.0 - abs(2.0 * g - 1.0),
                    clamp(2.0 * g - 1.0, 0.0, 1.0));
    }
    if (mode < 3.5) {
        if (image_order < 0.5) return vec3(0.15, 0.85, 1.0);
        if (image_order < 1.5) return vec3(1.0, 0.58, 0.08);
        return vec3(0.9, 0.12, 0.7);
    }
    if (mode < 4.5) {
        float value = clamp(accepted_steps / float(CK_KERR_MAX_STEPS), 0.0, 1.0);
        return vec3(value, value * value, 1.0 - value);
    }
    float residual_value = clamp((log2(max(max_residual, 1.0e-12)) + 40.0) /
                                 32.0, 0.0, 1.0);
    return vec3(residual_value, 1.0 - residual_value, 0.15);
}

vec4 ck_kerr_trace(vec2 uv, int parameter_base)
{
    vec4 metric = u_jceParams[parameter_base + 0];
    vec4 disk_events = u_jceParams[parameter_base + 1];
    vec4 quality = u_jceParams[parameter_base + 2];
    vec4 spectral = u_jceParams[parameter_base + 3];
    vec4 background_orientation = u_jceParams[parameter_base + 4];
    vec4 lut_axes = u_jceParams[parameter_base + 5];
    vec4 integration = u_jceParams[parameter_base + 6];
    vec4 script_invariants = u_jceParams[parameter_base + 7];
    float spin = clamp(metric.x, -0.998, 0.998);
    float active_steps = CK_KERR_FORCE_ACTIVE_STEPS > 0
        ? min(float(CK_KERR_FORCE_ACTIVE_STEPS), float(CK_KERR_MAX_STEPS))
        : clamp(quality.x, 1.0, float(CK_KERR_MAX_STEPS));

    float temporal_phase = quality.z >= 0.0
        ? quality.z : u_jceTimeFrame.z;
    vec2 pixel = floor(uv * u_jceOutputViewport.xy) + vec2(0.5, 0.5) +
        ck_kerr_temporal_jitter(temporal_phase);
    vec2 sample_uv = pixel / max(u_jceOutputViewport.xy, vec2(1.0, 1.0));
    vec2 screen = vec2(sample_uv.x * 2.0 - 1.0,
                       1.0 - sample_uv.y * 2.0);
    if (u_jceCameraProjection.z > 0.5) {
#if CK_KERR_FORCE_DIAGNOSTIC == 9
        return vec4(1.0, 1.0, 0.0, 1.0);
#else
        return vec4(ck_kerr_outcome_color(CK_KERR_OUTCOME_INVALID), 1.0);
#endif
    }

    vec3 world_ray = normalize(u_jceCameraBasis[2].xyz +
        u_jceCameraBasis[0].xyz * screen.x * u_jceCameraProjection.y *
            u_jceCameraProjection.x +
        u_jceCameraBasis[1].xyz * screen.y * u_jceCameraProjection.x);
    vec3 observer = mul(u_jceEffectWorldInv,
        vec4(u_jceCameraPosition.xyz, 1.0)).xyz;
    vec3 ray = normalize(mul(u_jceEffectWorldInv,
        vec4(world_ray, 0.0)).xyz);

#if CK_KERR_FORCE_DIAGNOSTIC == 7
    {
        float debug_r, debug_theta, debug_phi;
        vec3 debug_er, debug_etheta, debug_ephi;
        ck_kerr_cartesian_to_bl(observer, spin,
                                debug_r, debug_theta, debug_phi);
        ck_kerr_spatial_basis(debug_r, debug_theta, debug_phi, spin,
                              debug_er, debug_etheta, debug_ephi);
        vec3 local_ray = vec3(dot(ray, debug_er),
                              dot(ray, debug_etheta),
                              dot(ray, debug_ephi));
        return vec4(local_ray * 0.5 + vec3(0.5, 0.5, 0.5), 1.0);
    }
#endif

    float r = 0.0;
    float theta = 0.0;
    float phi = 0.0;
    float lambda = 0.0;
    float eta = 0.0;
    float radial_sign = -1.0;
    float polar_sign = 1.0;
    float energy_at_infinity = 0.0;
    if (!ck_kerr_zamo_constants(observer, ray, spin, r, theta, phi,
                                lambda, eta,
                                radial_sign, polar_sign,
                                energy_at_infinity)) {
#if CK_KERR_FORCE_DIAGNOSTIC == 8
        return vec4(1.0, 0.0, 1.0, 1.0);
#elif CK_KERR_FORCE_DIAGNOSTIC == 9
        return vec4(0.0, 1.0, 1.0, 1.0);
#else
        return vec4(ck_kerr_outcome_color(CK_KERR_OUTCOME_INVALID), 1.0);
#endif
    }

#if CK_KERR_FORCE_DIAGNOSTIC == 8
    return vec4(0.0, 1.0, 0.0, 1.0);
#endif

    float computed_horizon = ck_kerr_horizon(spin);
    float computed_isco = ck_kerr_isco(spin);
    if (abs(computed_horizon - script_invariants.x) > 1.0e-3 ||
        abs(computed_isco - script_invariants.y) > 2.0e-3) {
#if CK_KERR_FORCE_DIAGNOSTIC == 9
        return vec4(1.0, 1.0, 1.0, 1.0);
#else
            return vec4(ck_kerr_outcome_color(CK_KERR_OUTCOME_INVALID), 1.0);
#endif
    }

    vec4 phase = vec4(
        r,
        cos(theta),
        radial_sign * sqrt(max(0.0,
            ck_kerr_radial_potential(r, spin, lambda, eta))),
        -sin(theta) * polar_sign * sqrt(max(0.0,
            ck_kerr_polar_potential(theta, spin, lambda, eta))));

    float outcome = CK_KERR_OUTCOME_LIMIT;
    float accepted_steps = 0.0;
    float max_residual = 0.0;
    float disk_radius = 0.0;
    float disk_redshift = 0.0;
    float image_order = 0.0;
    vec3  disc_accum = vec3(0.0, 0.0, 0.0);   /* emission integrated so far  */
    float disc_tau = 0.0;                     /* optical depth accumulated   */
    vec3 radiance = vec3(0.0, 0.0, 0.0);
    bool finished = false;

    for (int step_index = 0; step_index < CK_KERR_MAX_STEPS; ++step_index) {
        if (float(step_index) >= active_steps || finished) break;
        r = phase.x;
        theta = acos(clamp(phase.y, -1.0, 1.0));
        if (r <= computed_horizon + disk_events.z) {
            outcome = CK_KERR_OUTCOME_CAPTURE;
            finished = true;
            break;
        }
        if (r >= disk_events.y && phase.z > 0.0 && step_index > 1) {
            outcome = CK_KERR_OUTCOME_ESCAPE;
            vec3 source_direction = normalize(
                ck_kerr_bl_to_cartesian(r, theta, phi, spin));
            radiance = ck_kerr_sample_background(
                source_direction, background_orientation,
                image_order, accepted_steps, lambda);
            finished = true;
            break;
        }

        vec4 old_phase = phase;
        float h = ck_kerr_phase_step(phase, spin, lambda, eta, integration);
        float next_phi = phi;
        vec4 next_phase = phase;
        float step_residual = 1.0e30;
        bool accepted = false;
        for (int retry = 0; retry < CK_KERR_RETRY_COUNT; ++retry) {
            bool valid_step = ck_kerr_phase_rk4(
                phase, phi, h, spin, lambda, eta, next_phase, next_phi);
            valid_step = valid_step && ck_kerr_fold_polar_axis(
                lambda, next_phase, next_phi);
            float radial_next = ck_kerr_radial_potential(
                next_phase.x, spin, lambda, eta);
            float polar_next = ck_kerr_polar_u_potential(
                next_phase.y, spin, lambda, eta);
            float radial_scale = max(1.0, next_phase.x * next_phase.x *
                                          next_phase.x * next_phase.x);
            bool potential_allowed =
                radial_next >= -disk_events.w * radial_scale &&
                polar_next >= -disk_events.w;
            step_residual = ck_kerr_phase_residual(
                next_phase, spin, lambda, eta);
            if (valid_step && potential_allowed &&
                step_residual <= disk_events.w) {
                accepted = true;
                break;
            }
            h *= 0.5;
        }
        max_residual = max(max_residual, step_residual);
        if (!accepted) {
#if CK_KERR_FORCE_DIAGNOSTIC == 9
            float radial_here = ck_kerr_radial_potential(
                phase.x, spin, lambda, eta);
            float polar_here = ck_kerr_polar_u_potential(
                phase.y, spin, lambda, eta);
            if (radial_here < -disk_events.w * max(1.0,
                    phase.x * phase.x * phase.x * phase.x))
                return vec4(1.0, 0.0, 0.0, 1.0);
            if (polar_here < -disk_events.w)
                return vec4(0.0, 1.0, 0.0, 1.0);
            if (step_residual > disk_events.w)
                return vec4(1.0, 0.5, 0.0, 1.0);
            return vec4(0.0, 0.25, 1.0, 1.0);
#else
            outcome = CK_KERR_OUTCOME_INVALID;
            finished = true;
            break;
#endif
        }

        phase = next_phase;
        phi = next_phi;
        r = phase.x;
        phase.y = clamp(phase.y, -1.0, 1.0);
        theta = acos(phase.y);
        accepted_steps += 1.0;

        /* March the disc as a MEDIUM rather than testing a surface crossing.
         * Emission is weighted by the gaussian density at this height and
         * attenuated by everything already accumulated in front of it, so the
         * near side occludes the far side by construction -- which is what
         * reads as thickness.  The ray keeps going until the optical depth
         * saturates, so a grazing ray through the outer disc stays
         * translucent while one down the throat goes opaque. */
        if (r >= computed_isco && r <= disk_events.x &&
            disc_tau < CK_KERR_DISC_TAU_MAX) {
            /* Flaring profile.  A constant H/r is a cylinder, and it read as
             * one: the disc had the same thickness at the rim as at the
             * throat.  A real thin disc puffs up outward (H grows faster than
             * r in the Shakura-Sunyaev outer solution), so the inner edge
             * stays sharp where the emission is strongest and the outer body
             * is softer -- which is also what breaks up the hard horizontal
             * line the constant-thickness slab showed edge-on. */
            float r_norm = clamp((r - computed_isco) /
                max(disk_events.x - computed_isco, 1.0e-3), 0.0, 1.0);
            float h_over_r = CK_KERR_DISC_H_OVER_R *
                (0.70 + 0.45 * pow(r_norm, 0.6));
            float half_h = max(h_over_r * r, 1.0e-3);
            float zn = (r * phase.y) / half_h;
            float density = exp(-0.5 * zn * zn);
            /* Both rims were hard cuts (r >= r_isco, r <= r_outer) and drew
             * straight edges across the image.  Fade them instead: the ISCO
             * edge is physically sharp but not a step, and the outer edge is
             * where the model simply stops being authored. */
            density *= smoothstep(0.0, 0.10, r_norm) *
                       (1.0 - smoothstep(0.80, 1.0, r_norm));
            if (density > 0.002) {
                float d_r = r - old_phase.x;
                float d_th = r * (acos(clamp(phase.y, -1.0, 1.0)) -
                                  acos(clamp(old_phase.y, -1.0, 1.0)));
                float ds = sqrt(d_r * d_r + d_th * d_th);
                float g_local = 1.0;
                vec3 em = ck_kerr_disk_radiance(
                    r, spin, lambda, eta, energy_at_infinity,
                    disk_events, metric.z, spectral, lut_axes, g_local);
                disc_accum += em * (density * exp(-disc_tau) * ds *
                                    CK_KERR_DISC_EMISSION);
                disc_tau += CK_KERR_DISC_OPACITY * density * ds;
                if (disk_radius <= 0.0) {
                    disk_radius = r;
                    disk_redshift = g_local;
                }
            }
        }

        /* Equatorial crossings still drive the image-order counter (direct
         * image, first higher-order image, ...) even though they no longer
         * terminate the ray. */
        float old_side = old_phase.y;
        float new_side = phase.y;
        if (old_side * new_side <= 0.0 && abs(old_side - new_side) > 1.0e-8) {
            float crossing = clamp(old_side / (old_side - new_side), 0.0, 1.0);
            float crossing_r = mix(old_phase.x, r, crossing);
            if (!(crossing_r >= computed_isco && crossing_r <= disk_events.x))
                image_order += 1.0;
        }

        if (disc_tau >= CK_KERR_DISC_TAU_MAX) {
            outcome = CK_KERR_OUTCOME_DISK;
            finished = true;
        }

        if (!finished) {
            vec4 derivative = vec4(0.0, 0.0, 0.0, 0.0);
            float phi_velocity = 0.0;
            if (!ck_kerr_phase_derivative(phase, spin, lambda, eta,
                                           derivative, phi_velocity)) {
                outcome = CK_KERR_OUTCOME_INVALID;
                finished = true;
            } else {
                phase.z = ck_kerr_project_velocity(
                    ck_kerr_radial_potential(phase.x, spin, lambda, eta),
                    phase.z, derivative.z);
                phase.w = ck_kerr_project_velocity(
                    ck_kerr_polar_u_potential(phase.y, spin, lambda, eta),
                    phase.w, derivative.w);
            }
        }

        if (!finished && r <= computed_horizon + disk_events.z) {
            outcome = CK_KERR_OUTCOME_CAPTURE;
            finished = true;
        }
        if (!finished && r >= disk_events.y && phase.z > 0.0) {
            outcome = CK_KERR_OUTCOME_ESCAPE;
            vec3 source_direction = normalize(
                ck_kerr_bl_to_cartesian(r, theta, phi, spin));
            radiance = ck_kerr_sample_background(
                source_direction, background_orientation,
                image_order, accepted_steps, lambda);
            finished = true;
        }

    }

#if CK_KERR_FORCE_DIAGNOSTIC == 9
    {
        float terminal = 0.08 + 0.08 * outcome;
        return vec4(terminal, terminal, terminal, 1.0);
    }
#elif CK_KERR_FORCE_DIAGNOSTIC == 10
    {
        float radius_value = clamp(log(max(r, 1.0)) /
            log(max(disk_events.y, 2.0)), 0.0, 1.0);
        return vec4(radius_value, radius_value, radius_value, 1.0);
    }
#elif CK_KERR_FORCE_DIAGNOSTIC == 11
    r = phase.x;
    if (phase.z < 0.0)
        return vec4(1.0, 0.0, 1.0, 1.0);
    if (r < 100.0)
        return vec4(1.0, 0.0, 0.0, 1.0);
    if (r < 200.0)
        return vec4(1.0, 0.5, 0.0, 1.0);
    if (r < 500.0)
        return vec4(1.0, 1.0, 0.0, 1.0);
    if (r < 900.0)
        return vec4(0.0, 0.25, 1.0, 1.0);
    return vec4(0.0, 1.0, 0.0, 1.0);
#elif CK_KERR_FORCE_DIAGNOSTIC == 12
    r = phase.x;
    if (phase.z < 0.0)
        return vec4(1.0, 0.0, 1.0, 1.0);
    if (r < 10.0)
        return vec4(1.0, 0.0, 0.0, 1.0);
    if (r < 30.0)
        return vec4(1.0, 0.5, 0.0, 1.0);
    if (r < 60.0)
        return vec4(1.0, 1.0, 0.0, 1.0);
    if (r < 90.0)
        return vec4(0.0, 0.25, 1.0, 1.0);
    if (r < 100.0)
        return vec4(0.0, 1.0, 1.0, 1.0);
    return vec4(0.0, 1.0, 0.0, 1.0);
#endif

    float diagnostic_mode = CK_KERR_FORCE_DIAGNOSTIC >= 0
        ? float(CK_KERR_FORCE_DIAGNOSTIC) : quality.y;
    if (diagnostic_mode >= 5.5) {
        return vec4(outcome, disk_radius > 0.0 ? disk_radius : r,
                    max_residual, accepted_steps);
    }
    if (diagnostic_mode >= 0.5) {
        return vec4(ck_kerr_diagnostic(diagnostic_mode, outcome,
                    disk_redshift, image_order, accepted_steps,
                    max_residual), 1.0);
    }

    if (outcome == CK_KERR_OUTCOME_LIMIT) {
        if (phase.z > 0.0) {
            vec3 source_direction = normalize(
                ck_kerr_bl_to_cartesian(phase.x, theta, phi, spin));
            radiance = ck_kerr_sample_background(
                source_direction, background_orientation,
                image_order, accepted_steps, lambda);
        } else {
            radiance = vec3(0.0, 0.0, 0.0);
        }
    } else if (outcome == CK_KERR_OUTCOME_INVALID) {
        radiance = vec3(0.0, 0.0, 0.0);
    }
    /* Composite: the disc is a translucent foreground, so whatever the ray
     * terminated on (background, capture black) is attenuated by exp(-tau)
     * and the integrated emission sits on top.  A fully saturated ray keeps
     * only its own emission, which is the opaque case. */
    radiance = disc_accum + radiance * exp(-disc_tau);
    if (disc_tau > 0.02 && outcome == CK_KERR_OUTCOME_ESCAPE)
        outcome = CK_KERR_OUTCOME_DISK;

    vec4 current = vec4(max(radiance, vec3(0.0, 0.0, 0.0)), 1.0);
    if (u_jceTimeFrame.w > 0.5 && quality.w > 0.0) {
        vec4 history = texture2D(s_jceHistory,
            jce_fullscreen_render_target_uv(sample_uv));
        current.rgb = mix(current.rgb, history.rgb, clamp(quality.w, 0.0, 0.99));
    }
    return current;
}

vec4 ck_kerr_render(vec2 uv)
{
    float split = u_jceParams[3].w;
    int base = split >= 0.0 && uv.x >= split ? 8 : 0;
    return ck_kerr_trace(uv, base);
}

#endif
