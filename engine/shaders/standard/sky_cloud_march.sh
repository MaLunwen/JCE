#ifndef JCE_SKY_CLOUD_MARCH_SH
#define JCE_SKY_CLOUD_MARCH_SH

#include "sky_cloud_geometry.sh"

float cloud_atlas_view_fetch(float u, float v, float w)
{
    // The atlas is linear within a tile. Blend the two slice centres,
    // including the wrap from the last world-Z slice back to the first.
    float layer = fract(w) * u_cloud_atlas.z - 0.5;
    float lower = floor(layer);
    float inverse_count = 1.0 / u_cloud_atlas.z;
    float a = cloud_atlas_fetch(u, v, fract((lower + 0.5) * inverse_count));
    float b = cloud_atlas_fetch(u, v, fract((lower + 1.5) * inverse_count));
    return mix(a, b, fract(layer));
}

/* Match the texel-centred CPU bake. Interpolate the X seam explicitly so
 * filtering never reads a neighbouring atlas tile, and wrap Z across slices. */
float cloud_atlas_periodic_fetch(float u, float v, float w)
{
    float last_x = u_cloud_atlas.x - 1.0;
    float texel_x = fract(u) * u_cloud_atlas.x - 0.5;
    float texel_y = clamp(v * u_cloud_atlas.y - 0.5, 0.0, u_cloud_atlas.y - 1.0);
    float row = texel_y / max(u_cloud_atlas.y - 1.0, 1.0);
    float column = clamp(texel_x, 0.0, last_x) / max(last_x, 1.0);
    float d = cloud_atlas_view_fetch(column, row, w);
    BRANCH if (texel_x < 0.0 || texel_x > last_x) {
        float opposite_column = texel_x < 0.0 ? 1.0 : 0.0;
        float wrap_weight = abs(texel_x - clamp(texel_x, 0.0, last_x));
        d = mix(d, cloud_atlas_view_fetch(opposite_column, row, w), wrap_weight);
    }
    return d;
}

// Only the modern physical volume binds this small, unshaped detail field.
SAMPLER2D(s_cloud_detail, 4);
uniform vec4 u_cloud_shape; // split, erosion, curl metres, layer thickness metres

vec2 cloud_shape_slice(float u, float v, float w)
{
    float z = fract(w) * u_cloud_atlas.z - 0.5;
    float lo = floor(z);
    return mix(cloud_atlas_fetch_rg(u, v, fract((lo + 0.5) / u_cloud_atlas.z)),
               cloud_atlas_fetch_rg(u, v, fract((lo + 1.5) / u_cloud_atlas.z)), fract(z));
}

vec2 cloud_shape_fetch(float u, float v, float w)
{
    float last = u_cloud_atlas.x - 1.0;
    float x = fract(u) * u_cloud_atlas.x - 0.5;
    float row = clamp(v * u_cloud_atlas.y - 0.5, 0.0, u_cloud_atlas.y - 1.0)
        / max(u_cloud_atlas.y - 1.0, 1.0);
    vec2 d = cloud_shape_slice(clamp(x, 0.0, last) / max(last, 1.0), row, w);
    if (x < 0.0 || x > last)
        d = mix(d, cloud_shape_slice(x < 0.0 ? 1.0 : 0.0, row, w),
                abs(x - clamp(x, 0.0, last)));
    return d;
}

vec3 cloud_detail_slice_x(vec2 texel, float z)
{
    vec2 tile = vec2(mod(z, 8.0), floor(z / 8.0)) * 64.0;
    vec2 clamped = clamp(texel, vec2_splat(0.0), vec2_splat(63.0));
    vec3 value = texture2DLod(s_cloud_detail, (tile + clamped + vec2_splat(0.5)) / 512.0, 0.0).rgb;
    if (texel.x < 0.0 || texel.x > 63.0) {
        float opposite = texel.x < 0.0 ? 63.0 : 0.0;
        vec3 wrapped = texture2DLod(s_cloud_detail,
            (tile + vec2(opposite, clamped.y) + vec2_splat(0.5)) / 512.0, 0.0).rgb;
        value = mix(value, wrapped, abs(texel.x - clamped.x));
    }
    return value;
}

vec3 cloud_detail_slice(vec2 texel, float z)
{
    vec3 value = cloud_detail_slice_x(texel, z);
    if (texel.y < 0.0 || texel.y > 63.0) {
        float opposite = texel.y < 0.0 ? 63.0 : 0.0;
        value = mix(value, cloud_detail_slice_x(vec2(texel.x, opposite), z),
                    abs(texel.y - clamp(texel.y, 0.0, 63.0)));
    }
    return value;
}

vec3 cloud_detail_fetch(vec3 coord)
{
    vec3 texel = fract(coord) * 64.0 - vec3_splat(0.5);
    float lower = floor(texel.z);
    return mix(cloud_detail_slice(texel.xy, mod(lower + 64.0, 64.0)),
               cloud_detail_slice(texel.xy, mod(lower + 65.0, 64.0)), fract(texel.z));
}

float cloud_split_density(float u, float v, float w, float footprint_km)
{
    vec2 shape = cloud_shape_fetch(u, v, w);
    if (shape.x <= 0.0) return 0.0;
    if (shape.x >= 0.25 || u_cloud_shape.y <= 0.0)
        return clamp(shape.x * shape.y, 0.0, 1.0);
    // Identical stream-function warp to cloud_shape_xz on the CPU. The base
    // atlas contains this warp already; only detail coordinates are warped here.
    float a = 25.1327412287 * u + 0.43;
    float b = 25.1327412287 * w + 1.17;
    vec2 period_m = 1000.0 / u_cloud_period.xy;
    float offset = min(u_cloud_shape.z, 0.0625 * min(period_m.x, period_m.y));
    vec2 warped = fract(vec2(u, w) + offset / max(period_m.x, period_m.y)
        * vec2(sin(a) * cos(b), -cos(a) * sin(b)));
    vec3 detail_coord = vec3(warped.x * period_m.x, v * u_cloud_shape.w,
                             warped.y * period_m.y) / 512.0;
    // F1 erodes cell walls and retains the small billow centres.
    // GPU RGB stores F1 directly; the scalar CPU sampler returns its inverse.
    vec3 fine = cloud_detail_fetch(detail_coord);
    // The same 64^3 atlas carries F1 at 85/43/21 m in RGB. Keep every
    // octave until its ray footprint approaches half its wavelength, then
    // blend to its neutral mean before a full wavelength is integrated.
    // Vector sampling adds no texture fetches and preserves coarse edges.
    vec3 wavelength = vec3(0.085333333, 0.042666667, 0.021333333);
    vec3 retained = vec3_splat(1.0) - smoothstep(wavelength * 0.5,
        wavelength, vec3_splat(footprint_km));
    float filtered = dot(mix(vec3_splat(0.5), fine, retained),
        vec3(0.571428571, 0.285714286, 0.142857143));
    // Match the CPU F1 contrast; the neutral far-LOD mean stays .5.
    filtered = clamp(0.5 + 2.5 * (filtered - 0.5), 0.0, 1.0);
    float erosion = u_cloud_shape.y * (1.0 - clamp(shape.x / 0.25, 0.0, 1.0)) * filtered;
    return clamp(clamp((shape.x - erosion) / max(1.0 - erosion, 0.001), 0.0, 1.0)
                 * shape.y, 0.0, 1.0);
}

float cloud_phase(float cosine, float eccentricity)
{
    float den = 1.0 + eccentricity * eccentricity - 2.0 * eccentricity * cosine;
    return (1.0 - eccentricity * eccentricity) /
        (12.5663706144 * pow(max(den, 1e-4), 1.5));
}

// Float-only multiply/fract hash for compatibility shader profiles.
float cloud_lattice_hash(vec3 seed)
{
    vec3 p = fract(seed * vec3(0.1031, 0.11369, 0.13787));
    p += vec3_splat(dot(p, p.yzx + vec3_splat(33.33)));
    return fract((p.x + p.y) * p.z);
}

float cloud_pixel_jitter(vec2 pixel_xy)
{
    // Preserve the temporal caller's fractional phase as part of the seed.
    vec2 subpixel = fract(pixel_xy);
    return cloud_lattice_hash(vec3(floor(pixel_xy), subpixel.x + 1.618 * subpixel.y));
}

float cloud_edge_noise(vec2 wind_uv, float world_y_km, float cell_km)
{
    // An integer number of cells in the atlas period keeps the X/Z lattice
    // faces identical as the wind wraps. The caller filters each cell scale.
    vec2 count = max(floor(1.0 / (u_cloud_period.xy * cell_km) + 0.5), vec2_splat(1.0));
    vec3 p = vec3(wind_uv.x * count.x, world_y_km / cell_km, wind_uv.y * count.y);
    vec3 lo = floor(p);
    vec3 f = fract(p);
    vec3 hi = lo + vec3_splat(1.0);
    lo.xz -= step(count, lo.xz) * count;
    hi.xz -= step(count, hi.xz) * count;
    f = f * f * (3.0 - 2.0 * f);
    float x00 = mix(cloud_lattice_hash(lo), cloud_lattice_hash(vec3(hi.x, lo.y, lo.z)), f.x);
    float x10 = mix(cloud_lattice_hash(vec3(lo.x, hi.y, lo.z)), cloud_lattice_hash(vec3(hi.xy, lo.z)), f.x);
    float x01 = mix(cloud_lattice_hash(vec3(lo.xy, hi.z)), cloud_lattice_hash(vec3(hi.x, lo.y, hi.z)), f.x);
    float x11 = mix(cloud_lattice_hash(vec3(lo.x, hi.yz)), cloud_lattice_hash(hi), f.x);
    return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
}

/* Modern camera/light rays share the same density definition. Only the
 * legacy material retains its original view-only detail approximation. */
float cloud_density_layer(vec3 point_km, float height, bool detail, bool cumulus,
                          float daylight, float density_trim, float footprint_km, vec2 wind)
{
    // World X, normalised altitude, world Z. Wind has units of atlas periods.
    float u = fract(point_km.x * u_cloud_period.x - wind.x);
    float v = clamp(height, 0.0, 1.0);
    float w = fract(point_km.z * u_cloud_period.y - wind.y);
    if (u_cloud_shape.x > 0.5) return cloud_split_density(u, v, w, footprint_km);
    float d;
    BRANCH if (cumulus) {
        // Allocation/capability fallback contains the COMPLETE CPU density,
        // including erosion and optical gain. Do not trim or erode it twice.
        return cloud_atlas_periodic_fetch(u, v, w);
    }
    else if (detail && u_cloud_quality.x >= 16.0)
        d = cloud_atlas_view_fetch(u, v, w);
    else
        d = cloud_atlas_fetch(u, v, w);

    d = clamp((d - 0.02) / 0.98, 0.0, 1.0);
    BRANCH if (density_trim > 0.0) {
        // Remove the broad, dilute skirts before tracing self-shadow. Keep
        // more of the crown and preserve coverage control in the coarse field.
        float lower_skirt = 1.0 - smoothstep(0.14, 0.52, v);
        d = max(d - density_trim * (1.0 + 0.23 * lower_skirt), 0.0);
        float crown = smoothstep(0.36, 0.75, v);
        d *= 1.0 + 0.26 * crown * daylight;
    }
    BRANCH if (detail && d > 0.0) {
        BRANCH if (daylight > 0.0 && d < 0.15) {
            // The baked field already contains eroded Perlin-Worley shape.
            // Reusing that height-shaped volume at 8x frequency creates
            // metre-scale holes that a 30--50 m ray segment cannot resolve.
            // Add only world-space contour detail with a known cell size,
            // and suppress each octave before its Nyquist limit is crossed.
            float broad_weight = 1.0 - smoothstep(0.035, 0.070, footprint_km);
            float fine_weight = 1.0 - smoothstep(0.014, 0.028, footprint_km);
            float contour = smoothstep(0.0, 0.021, d)
                * (1.0 - smoothstep(0.052, 0.15, d));
            float breakup = 0.0;
            BRANCH if (broad_weight > 0.0)
                breakup += (cloud_edge_noise(vec2(u, w), point_km.y, 0.140) - 0.5)
                    * (0.18 * broad_weight);
            BRANCH if (fine_weight > 0.0)
                breakup += (cloud_edge_noise(vec2(u, w), point_km.y, 0.056) - 0.5)
                    * (0.07 * fine_weight);
            d = max(0.0, d + breakup * contour * daylight);
        }
        BRANCH if (daylight < 1.0) {
            // Preserve the original material at night and on legacy clouds.
            float du = fract(u * 8.0 + 0.37);
            float dv = fract(v * 3.0 + 0.11);
            float dw = fract(w * 8.0 + 0.61);
            float high = cumulus ? cloud_atlas_periodic_fetch(dw, du, dv)
                                 : cloud_atlas_fetch(dw, du, dv);
            float erosion = high * (cumulus ? 0.55 : 0.42) * (1.0 - daylight);
            d = clamp((d - erosion) / max(1.0 - erosion, 0.001), 0.0, 1.0);
        }
    }
    // Recover extinction in the surviving cells without filling clear gaps.
    return min(d * (1.0 + 2.3 * daylight), 1.0);
}

#if defined(JCE_SKY_CLOUD_LAYERED)
float cloud_layer_blend(float height)
{
    return smoothstep(0.38, 0.62, height);
}
#endif

float cloud_density_at(vec3 point_km, float height, bool detail, bool cumulus,
                       float daylight, float density_trim, float footprint_km, vec3 upper_wind)
{
#if defined(JCE_SKY_CLOUD_LAYERED)
    float layer = upper_wind.z > 0.5 ? cloud_layer_blend(height) : 0.0;
    float density = 0.0;
    BRANCH if (layer < 1.0)
        density = cloud_density_layer(point_km, height, detail, cumulus,
            daylight, density_trim, footprint_km, u_cloud_quality.yz);
    BRANCH if (layer > 0.0) {
        // Mix density rather than wrapped offsets. The latter jumps at the
        // atlas period whenever only one of the two wind layers wraps.
        float upper = cloud_density_layer(point_km, height, detail, cumulus,
            daylight, density_trim, footprint_km, upper_wind.xy);
        density = mix(density, upper, layer);
    }
    return density;
#else
    return cloud_density_layer(point_km, height, detail, cumulus,
        daylight, density_trim, footprint_km, u_cloud_quality.yz);
#endif
}

/* Premultiplied linear in-scatter RGB and transmittance A. The distance
 * is camera-relative in kilometres and zero for clear rays. */
vec4 jce_cloud_march_bounded(vec3 camWKm, vec3 dir, float mu, vec3 T, vec3 ms,
                            vec2 pixel_xy, float max_distance_km, vec3 upper_wind,
                            out float distance_km, out float cloud_rim)
{
    distance_km = 0.0;
    cloud_rim = 0.0;
    if (u_cloud_params.x <= 0.001)
        return vec4(0.0, 0.0, 0.0, 1.0);

    float botKm = (u_cloud_params.z > 0.0) ? u_cloud_params.z : 1.5;
    float topKm = (u_cloud_params.w > botKm) ? u_cloud_params.w : 4.0;
    float dens = max(u_cloud_params.y, 0.001);
    bool cumulus = u_cloud_quality.w > 0.5;
    vec2 interval = topKm > botKm
        ? cloud_shell_range(camWKm, dir, botKm, topKm) : vec2(0.0, 0.0);
    float t0 = interval.x;
    float t1 = min(interval.y, max_distance_km);
    if (t1 > t0)
    {
        /* The loop BOUND is a compile-time constant -- a dynamic bound
         * does not compile on the oldest profile this shader targets -- so
         * the march always loops to the maximum and breaks on the budget.
         * dt is derived from the BUDGET, not the bound, so a cheaper tier
         * still traverses the whole slab with fewer, longer steps rather
         * than stopping part-way through the cloud. */
        const int STEPS_MAX = 48;
        float budget = min(float(STEPS_MAX), max(u_cloud_quality.x *
            (u_cloud_shape.x > 0.5 ? 2.0 : 1.0), 1.0));
        vec3 moon_direction;
        vec3 lighting = sky_cloud_celestial_light(mu, moon_direction);
        vec3 light_direction = normalize(u_sky_sun_dir.xyz);
        vec3 direct_light = T;
        vec3 ambient_light = ms;
        if (lighting.x < 1.0)
            direct_light *= lighting.x;
        if (lighting.y > 0.0) {
            light_direction = moon_direction;
            direct_light = sky_cloud_moon_tint(moon_direction.y) * lighting.y;
        }
        if (lighting.z > 0.0)
            ambient_light = mix(ambient_light, sky_cloud_night_ambient(), lighting.z);
        float light_cos = clamp(dot(normalize(dir), light_direction), -1.0, 1.0);

        // The phase terms are constant for this view ray. Only the
        // extinction for each scattering order changes along the march.
        vec3 phase = vec3(max(cloud_phase(light_cos, 0.60),
                             0.70 * cloud_phase(light_cos, 0.91)),
                          0.50 * cloud_phase(light_cos, 0.30),
                          0.25 * cloud_phase(light_cos, 0.15));
        // The cumulus daytime source separates transmitted direct sun from
        // broad sky fill. Desaturate the bounced component only as day rises.
        float daylight = cumulus ? lighting.x * smoothstep(-0.10, 0.12, mu) : 0.0;
        // At grazing angles the same weather atlas repeats into vanishing
        // lanes. Fade distant density before the repetition dominates and
        // let the continuous atmosphere carry the horizon.
        float horizon_lod = smoothstep(0.30, 0.80, daylight);
        float period_km = 1.0 / max(max(u_cloud_period.x, u_cloud_period.y), 0.0001);
        float fade_start = clamp(1.3 * period_km, 3.0, 18.0);
        float fade_end = clamp(3.8 * period_km, fade_start + 2.0, 60.0);
        if (horizon_lod >= 1.0) {
            t1 = min(t1, fade_end);
            if (t1 <= t0) return vec4(0.0, 0.0, 0.0, 1.0);
        }
        float span = t1 - t0;
        float inv_budget = 1.0 / budget;
        float dt = span / budget;
        // Long grazing rays through a nearby or enclosing cloud need their
        // first intervals close to the camera, even with the same step budget.
        float near_bias = daylight * smoothstep(2.5, 12.0, span)
            * (1.0 - smoothstep(0.1, 2.0, t0));
        vec2 pixel_angle = 2.0 * u_viewTexel.xy /
            max(abs(vec2(u_proj[0][0], u_proj[1][1])), vec2_splat(0.0001));
        float pixel_span = max(pixel_angle.x, pixel_angle.y);
        bool orthographic = abs(u_proj[3][3]) > 0.5;
        float coverage = smoothstep(0.32, 0.65, u_cloud_params.x);
        float density_trim = daylight * mix(0.17, 0.12, coverage);
        vec3 luma_weights = vec3(0.2126, 0.7152, 0.0722);
        vec3 cloud_fill = mix(ambient_light, vec3_splat(dot(ambient_light, luma_weights)), 0.93);
        vec3 bounced_sun = mix(T, vec3_splat(dot(T, luma_weights)), 0.88);
        float sun_lobe = clamp(0.55 + 1.6 * phase.x, 0.55, 1.65);
        vec3 scatter = vec3_splat(0.0);
        float trans = 1.0;
        float distance_integral = 0.0;
        float inv_thickness = 1.0 / (topKm - botKm);

        // Decorrelate the modern march across pixels and temporal phases.
        // Keep the authored legacy interleaved pattern on the old material.
        float jitter = cumulus ? cloud_pixel_jitter(pixel_xy)
            : fract(52.9829189 * fract(dot(pixel_xy, vec2(0.06711056, 0.00583715))));
        // Preserve decorrelation while reducing under-resolved edge grain.
        jitter = mix(jitter, 0.5, 0.28 * daylight);

        LOOP for (int si = 0; si < STEPS_MAX; ++si)
        {
            if (float(si) >= budget) break;
            float t = t0 + (float(si) + jitter) * dt;
            float step_width = dt;
            BRANCH if (near_bias > 0.0) {
                float first = float(si) * inv_budget;
                float start = t0 + span * mix(first, first * first, near_bias);
                step_width = dt * mix(1.0, 2.0 * first + inv_budget, near_bias);
                t = start + jitter * step_width;
            }
            // The atlas has one mip. Fade only the contour frequencies once
            // either the ray segment or the projected pixel cannot resolve them.
            float footprint = max(step_width, (orthographic ? 0.001 : t) * pixel_span);
            vec3 wp = camWKm + dir * t;
            float h01 = clamp((cloud_world_altitude(wp) - botKm) * inv_thickness,
                              0.0, 1.0);
            float sample_density = cloud_density_at(wp, h01, true, cumulus,
                daylight, density_trim, footprint, upper_wind);
            float distant_visibility = 1.0 - horizon_lod * smoothstep(fade_start, fade_end, t);
            float sigma = sample_density * dens * distant_visibility;
            if (sigma <= 0.0) continue;

            /* One self-shadow march with the same budget for either light. */
            float light_tau = 0.0;
            {
                float lstep = (topKm - botKm) * (cumulus ? 0.04 : 0.25);
                float light_t = 0.0;
                float light_limit = JCE_CLOUD_MARCH_LIMIT_KM;
                BRANCH if (cumulus) {
                    // Local tangent to the spherical layer. Over this short
                    // shadow ray its curvature error is below one atlas cell.
                    float vertical = light_direction.y
                        + dot(wp.xz, light_direction.xz) / JCE_CLOUD_PLANET_KM;
                    if (vertical > 0.0001)
                        light_limit = (1.0 - h01) / (inv_thickness * vertical);
                    else if (vertical < -0.0001)
                        light_limit = -h01 / (inv_thickness * vertical);
                }
                LOOP for (int li = 0; li < 8; ++li)
                {
                    if ((!cumulus || budget < 16.0) && float(li) * 4.0 >= budget) break;
                    float width = cumulus ? min(lstep, max(light_limit - light_t, 0.0)) : lstep;
                    if (width <= 0.0) break;
                    // Midpoint integration retains the nearby occluder that
                    // endpoint-only samples skipped in small cloud billows.
                    float offset = cumulus ? 0.5 * width : width;
                    vec3 lp = wp + light_direction * (light_t + offset);
                    float light_height = cloud_world_altitude(lp);
                    if (light_height > topKm || light_height < botKm) break;
                    float lh01 = (light_height - botKm) * inv_thickness;
                    light_tau += cloud_density_at(lp, lh01, false, cumulus,
                        daylight, density_trim, 0.0, upper_wind) * dens * width;
                    light_t += width;
                    lstep *= cumulus ? 1.6 : 1.5;
                }
            }

            vec3 extinction = exp(-light_tau * vec3(1.0, 0.5, 0.25));
            float day_bounce = 0.10 * lighting.x * exp(-0.125 * light_tau);
            vec3 in_light = direct_light * (dot(phase, extinction) + day_bounce);
            in_light += ambient_light * mix(0.22, 0.70, h01);
            BRANCH if (daylight > 0.0) {
                // Successive scattering orders use the same optical depth
                // with decreasing extinction. Even the broad diffuse solar
                // term vanishes in an optically deep core; it is not an
                // unoccluded constant added on top of the direct source.
                float sunlit = sun_lobe * dot(extinction, vec3(1.25, 0.65, 0.25));
                float height_fill = smoothstep(0.08, 0.90, h01);
                float fringe = (1.0 - smoothstep(0.08, 0.43, sample_density)) * extinction.y;
                float silver = fringe * mix(0.35, 1.0, smoothstep(0.12, 0.92, light_cos));
                vec3 fill = cloud_fill * mix(0.69, 1.32, height_fill);
                fill += bounced_sun * mix(0.75, 1.20, height_fill) * exp(-0.06 * light_tau);
                // A bounded higher scattering order redistributes light into
                // optically thicker regions. It is zero for a clear segment
                // and decays again in a deep core, rather than adding flat fill.
                float high_order = 0.45 * (1.0 - extinction.z) * exp(-0.02 * light_tau);
                fill += bounced_sun * high_order;
                in_light = mix(in_light, direct_light * (sunlit + 0.65 * silver) + fill, daylight);
            }

            // Exact integration of a constant source over this segment
            // keeps thick clouds stable when the step budget changes.
            float step_t = exp(-sigma * step_width);
            float segment_opacity = trans * (1.0 - step_t);
            distance_integral += t * segment_opacity;
            scatter += in_light * segment_opacity;
            trans *= step_t;
            if (trans < 0.01) break;
        }

        // Keep sky grading based on the unfaded silhouette in the inline path.
        float edge = 1.0 - smoothstep(28.0, JCE_CLOUD_MARCH_LIMIT_KM, t0);
        cloud_rim = clamp(4.0 * trans * (1.0 - trans), 0.0, 1.0) * edge;
        float opacity = 1.0 - trans;
        if (opacity * edge > 0.0001)
            distance_km = distance_integral / opacity;
        return vec4(scatter * edge, mix(1.0, trans, edge));
    }
    return vec4(0.0, 0.0, 0.0, 1.0);
}

vec4 jce_cloud_march_with_rim(vec3 camera_km, vec3 ray, float sun_elevation,
                            vec3 solar_transmittance, vec3 sky_ambient,
                            vec2 pixel_xy, out float distance_km, out float cloud_rim)
{
    return jce_cloud_march_bounded(camera_km, ray, sun_elevation,
        solar_transmittance, sky_ambient, pixel_xy, JCE_CLOUD_MARCH_LIMIT_KM,
        vec3_splat(0.0), distance_km, cloud_rim);
}

#endif
