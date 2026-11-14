/*
 * jce_csm.c  Cascaded Shadow Map computation.
 *
 * Implements practical split scheme (log-linear blend) and
 * tight-fitting ortho projections per cascade.
 */

#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_csm.h>
#include <math.h>
#include <string.h>

static void compute_splits(float *splits, uint32_t count,
                           float near, float far, float lambda)
{
    if (lambda < 0.0f) lambda = 0.0f;
    if (lambda > 1.0f) lambda = 1.0f;
    splits[0] = near;
    for (uint32_t i = 1; i <= count; i++) {
        float p = (float)i / (float)count;
        float log_split = near * powf(far / near, p);
        float lin_split = near + (far - near) * p;
        splits[i] = lambda * log_split + (1.0f - lambda) * lin_split;
    }
}

void jce_csm_compute(JceCsmData *out,
                     uint32_t cascade_count,
                     float near_plane,
                     float far_plane,
                     float fov_deg,
                     float aspect,
                     const jce_mat4 *camera_view,
                     const jce_vec3 *light_dir,
                     bool homogeneous_depth,
                     uint16_t shadow_map_size,
                     float split_lambda,
                     const jce_vec3 *caster_aabb_min,
                     const jce_vec3 *caster_aabb_max)
{
    if (!out || !camera_view || !light_dir) return;
    JCE_PROFILE_ZONE_N("CSM::Compute");
    if (cascade_count < 1) cascade_count = 1;
    if (cascade_count > JCE_CSM_MAX_CASCADES) cascade_count = JCE_CSM_MAX_CASCADES;

    memset(out, 0, sizeof(*out));
    out->cascade_count = cascade_count;

    compute_splits(out->splits, cascade_count, near_plane, far_plane,
                   split_lambda);

    /* Only the sphere centre needs a world transform.  Fitting its radius in
     * view space keeps camera position/rotation rounding out of the half-metre
     * radius buckets (a bucket change changes the entire shadow texel grid). */
    jce_mat4 inv_view = jce_m4_inverse(camera_view);
    const float tan_half = tanf(fov_deg * 0.5f * JCE_DEG2RAD);
    const float map_size = shadow_map_size > 0 ? (float)shadow_map_size : 2048.0f;

    jce_vec3 ld = jce_v3_normalize(*light_dir);

    /* A continuous light basis over the daylight hemisphere.  Switching the
     * reference-up axis at |sun.y| = .99 rotated the atlas abruptly near
     * zenith.  This closed-form tangent frame has its singularity BELOW the
     * horizon instead; the fallback covers that unused downward pole. */
    jce_vec3 right_ws, up_ws;
    if (ld.y < -0.9999f) {
        right_ws = jce_v3_normalize(jce_v3_cross(ld, jce_v3(0, 0, 1)));
        up_ws = jce_v3_cross(right_ws, ld);
    } else {
        const float a = 1.0f / (1.0f + ld.y);
        const float b = -ld.x * ld.z * a;
        right_ws = jce_v3(1.0f - ld.x * ld.x * a, -ld.x, b);
        up_ws = jce_v3(b, -ld.z, 1.0f - ld.z * ld.z * a);
    }

    /* The near-plane extension toward the sun (so tall casters between the
     * cascade sphere and the sun are captured instead of clipped) is computed
     * PER CASCADE below, from casters clamped to that cascade's local footprint.
     * Computing it once over the WHOLE scene caster AABB made a single distant
     * caster (e.g. the always-resident HLOD city skyline ~1.5 km away) drive
     * near_extend to its cap on every cascade, ballooning the ortho depth range
     * to ~1 km and collapsing shadow-depth precision. */
    bool have_caster_extent = (caster_aabb_min && caster_aabb_max);

    for (uint32_t c = 0; c < cascade_count; c++) {
        float zn = out->splits[c];
        float zf = out->splits[c + 1];

        /* A symmetric perspective slice has its corner centroid on -Z.
         * Far corners bound the sphere because zf >= zn.  Evaluate the same
         * centroid/radius without subtracting large world-space positions. */
        const float mid_z = (zn + zf) * 0.5f;
        const float half_depth = (zf - zn) * 0.5f;
        const float half_height = zf * tan_half;
        const float half_width = half_height * aspect;
        jce_vec4 cw = jce_m4_mul_v4(&inv_view, jce_v4(0, 0, -mid_z, 1));
        jce_vec3 center = jce_v3(cw.x, cw.y, cw.z);
        float radius = sqrtf(half_width * half_width +
                             half_height * half_height +
                             half_depth * half_depth);
        /* Quantize radius to a stable absolute step independent of the
         * radius itself.  Earlier "scale by fraction of radius" was an
         * algebraic identity (ceil(r / (r/k)) * (r/k) == r) and provided
         * NO stabilisation.  Use a fixed 0.5 m bucket: small enough to
         * keep cascade-0 tight (radius ~9 m → 18 buckets) and large
         * enough to absorb single-frame fov/aspect/split float jitter so
         * that texel_size below stays bit-exact across frames during
         * pure camera rotation. */
        radius = ceilf(radius * 2.0f) * 0.5f;

        /* Snap cascade center onto the world-anchored light-space texel grid.
         * Only the atlas-plane axes need texel snapping. Quantising the
         * light-depth axis changes the shadow compare origin in discrete
         * steps during camera dolly/zoom, which reads as brightness flicker. */
        float texel_size = (radius * 2.0f) / map_size;
        float cx = jce_v3_dot(center, right_ws);
        float cy = jce_v3_dot(center, up_ws);
        const float cz = jce_v3_dot(center, ld);
        if (texel_size > 0.0f) {
            cx = floorf(cx / texel_size + 0.5f) * texel_size;
            cy = floorf(cy / texel_size + 0.5f) * texel_size;
            center = jce_v3_add(jce_v3_add(jce_v3_scale(right_ws, cx),
                                           jce_v3_scale(up_ws,    cy)),
                                jce_v3_scale(ld, cz));
        }

        /* Publish the world-space cascade sphere so the shadow pass can cull
         * casters outside this cascade's coverage (identical numbers to the
         * ortho fit below => the cull volume matches the rendered volume). */
        out->center[c] = center;
        out->radius[c] = radius;

        /* Extend the light camera back toward the sun so its near plane clears
         * the tallest caster between this cascade and the sun (else tall casters
         * are clipped and their shadows truncate).  KEY: only casters whose
         * shadow actually lands in THIS cascade should drive the extension, so
         * clamp the caster bounds to this cascade's local HORIZONTAL (XZ)
         * footprint — keeping full height — before projecting onto the light
         * axis.  Projecting the whole-scene AABB instead let one distant caster
         * (the always-resident HLOD skyline ~1.5 km away) pin near_extend to its
         * cap on every cascade, blowing the ortho depth range out to ~1 km and
         * collapsing depth precision (acne → huge biases → peter-panning +
         * washed-out shadows).  Full height is kept so tall LOCAL buildings are
         * still captured = no truncation regression.  Anchored to world-space
         * bounds => still camera-independent. */
        float cz_center = cz;
        float near_extend = 0.0f;
        if (have_caster_extent) {
            const float K = 2.0f; /* footprint slack for the tilted light axis */
            float fx0 = center.x - radius * K, fx1 = center.x + radius * K;
            float fz0 = center.z - radius * K, fz1 = center.z + radius * K;
            jce_vec3 lo = *caster_aabb_min, hi = *caster_aabb_max;
            if (lo.x < fx0) lo.x = fx0;
            if (hi.x > fx1) hi.x = fx1;
            if (lo.z < fz0) lo.z = fz0;
            if (hi.z > fz1) hi.z = fz1;
            if (lo.x <= hi.x && lo.z <= hi.z) { /* footprint overlaps casters */
                float cmp = -3.4e38f;
                for (int ci = 0; ci < 8; ci++) {
                    jce_vec3 cr = jce_v3((ci & 1) ? hi.x : lo.x,
                                         (ci & 2) ? hi.y : lo.y,
                                         (ci & 4) ? hi.z : lo.z);
                    float p = jce_v3_dot(cr, ld);
                    if (p > cmp) cmp = p;
                }
                near_extend = cmp - (cz_center + radius);
                if (near_extend < 0.0f) near_extend = 0.0f;
            }
            float cap = radius * 16.0f; /* backstop only; rarely hit now */
            if (near_extend > cap) near_extend = cap;
        }

        /* Reuse the exact basis AND snapped coordinates.  look_at(light_pos,
         * center) subtracts large positions to reconstruct this direction;
         * that roundoff made the atlas orientation change during camera motion
         * even though light_dir was fixed.  Its X row is -right_ws. */
        jce_mat4 light_view = jce_m4_identity();
        light_view.raw[0][0] = -right_ws.x;
        light_view.raw[1][0] = -right_ws.y;
        light_view.raw[2][0] = -right_ws.z;
        light_view.raw[0][1] = up_ws.x;
        light_view.raw[1][1] = up_ws.y;
        light_view.raw[2][1] = up_ws.z;
        light_view.raw[0][2] = ld.x;
        light_view.raw[1][2] = ld.y;
        light_view.raw[2][2] = ld.z;
        light_view.raw[3][0] = cx;
        light_view.raw[3][1] = -cy;
        light_view.raw[3][2] = -(cz + radius + near_extend);

        /* Ortho projection around bounding sphere with Z padding for
         * world-space normal-offset bias applied in the fragment shader.  The
         * far plane extends by near_extend too so the receiver slab (cascade
         * sphere) stays fully inside [near,far]. */
        float z_pad = radius * 0.05f;
        jce_mat4 light_proj = jce_m4_ortho(-radius, radius,
                                            -radius, radius,
                                            -z_pad,
                                            radius * 2.0f + near_extend + z_pad,
                                            homogeneous_depth);

        out->vp[c] = jce_m4_multiply(&light_proj, &light_view);
        /* The same two planes the projection above was built from, so the two
         * cannot drift apart: anything that reads depth_range is reading the
         * range this matrix actually normalizes over. */
        out->depth_range[c] = (radius * 2.0f + near_extend + z_pad) - (-z_pad);
    }
    JCE_PROFILE_ZONE_END;
}
