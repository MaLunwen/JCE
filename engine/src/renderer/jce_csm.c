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

/* Compute 8 frustum corners in world space for a sub-frustum [zn, zf]. */
static void frustum_corners_world(jce_vec3 corners[8],
                                  float fov_deg, float aspect,
                                  float zn, float zf,
                                  const jce_mat4 *inv_view)
{
    float tan_half = tanf(fov_deg * 0.5f * JCE_DEG2RAD);
    float hn = zn * tan_half;
    float wn = hn * aspect;
    float hf = zf * tan_half;
    float wf = hf * aspect;

    /* View-space corners (looking down -Z). */
    jce_vec3 vs[8] = {
        /* Near plane */
        { -wn,  hn, -zn },
        {  wn,  hn, -zn },
        {  wn, -hn, -zn },
        { -wn, -hn, -zn },
        /* Far plane */
        { -wf,  hf, -zf },
        {  wf,  hf, -zf },
        {  wf, -hf, -zf },
        { -wf, -hf, -zf },
    };

    /* Transform to world space. */
    for (int i = 0; i < 8; i++) {
        jce_vec4 v = jce_v4(vs[i].x, vs[i].y, vs[i].z, 1.0f);
        jce_vec4 w = jce_m4_mul_v4(inv_view, v);
        corners[i] = jce_v3(w.x, w.y, w.z);
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
                     float split_lambda)
{
    if (!out || !camera_view || !light_dir) return;
    JCE_PROFILE_ZONE_N("CSM::Compute");
    if (cascade_count < 1) cascade_count = 1;
    if (cascade_count > JCE_CSM_MAX_CASCADES) cascade_count = JCE_CSM_MAX_CASCADES;

    memset(out, 0, sizeof(*out));
    out->cascade_count = cascade_count;

    compute_splits(out->splits, cascade_count, near_plane, far_plane,
                   split_lambda);

    /* Compute inverse view matrix for frustum corner generation. */
    jce_mat4 inv_view = jce_m4_inverse(camera_view);

    jce_vec3 ld = jce_v3_normalize(*light_dir);

    for (uint32_t c = 0; c < cascade_count; c++) {
        float zn = out->splits[c];
        float zf = out->splits[c + 1];

        /* Get world-space frustum corners for this cascade. */
        jce_vec3 corners[8];
        frustum_corners_world(corners, fov_deg, aspect, zn, zf, &inv_view);

        /* Compute frustum center. */
        jce_vec3 center = jce_v3(0, 0, 0);
        for (int i = 0; i < 8; i++)
            center = jce_v3_add(center, corners[i]);
        center = jce_v3_scale(center, 1.0f / 8.0f);

        /* Compute bounding sphere radius for stable cascade sizing. */
        float radius = 0.0f;
        for (int i = 0; i < 8; i++) {
            float d = jce_v3_len(jce_v3_sub(corners[i], center));
            if (d > radius) radius = d;
        }
        /* Quantize radius to a stable absolute step independent of the
         * radius itself.  Earlier "scale by fraction of radius" was an
         * algebraic identity (ceil(r / (r/k)) * (r/k) == r) and provided
         * NO stabilisation.  Use a fixed 0.5 m bucket: small enough to
         * keep cascade-0 tight (radius ~9 m → 18 buckets) and large
         * enough to absorb single-frame fov/aspect/split float jitter so
         * that texel_size below stays bit-exact across frames during
         * pure camera rotation. */
        radius = ceilf(radius * 2.0f) * 0.5f;

        /* Build a stable light-space basis whose orientation depends ONLY on
         * the light direction (not on camera state).  This guarantees the
         * shadow-map texel grid is anchored to a fixed world frame, so when
         * the camera rotates the cascade center snaps to the same texels
         * every frame and shadows stop shimmering. */
        jce_vec3 up_ref = (fabsf(ld.y) > 0.99f) ? jce_v3(0, 0, 1) : jce_v3(0, 1, 0);
        jce_vec3 right_ws = jce_v3_normalize(jce_v3_cross(ld, up_ref));
        jce_vec3 up_ws    = jce_v3_normalize(jce_v3_cross(right_ws, ld));

        /* Snap cascade center onto the world-anchored light-space texel grid.
         * Only the atlas-plane axes need texel snapping. Quantising the
         * light-depth axis changes the shadow compare origin in discrete
         * steps during camera dolly/zoom, which reads as brightness flicker. */
        float map_size = shadow_map_size > 0 ? (float)shadow_map_size : 2048.0f;
        float texel_size = (radius * 2.0f) / map_size;
        if (texel_size > 0.0f) {
            float cx = jce_v3_dot(center, right_ws);
            float cy = jce_v3_dot(center, up_ws);
            float cz = jce_v3_dot(center, ld);
            cx = floorf(cx / texel_size + 0.5f) * texel_size;
            cy = floorf(cy / texel_size + 0.5f) * texel_size;
            center = jce_v3_add(jce_v3_add(jce_v3_scale(right_ws, cx),
                                           jce_v3_scale(up_ws,    cy)),
                                jce_v3_scale(ld, cz));
        }

        jce_vec3 light_pos  = jce_v3_add(center, jce_v3_scale(ld, radius));
        jce_mat4 light_view = jce_m4_look_at(light_pos, center, up_ws);

        /* Ortho projection around bounding sphere with Z padding for
         * world-space normal-offset bias applied in the fragment shader. */
        float z_pad = radius * 0.05f;
        jce_mat4 light_proj = jce_m4_ortho(-radius, radius,
                                            -radius, radius,
                                            -z_pad, radius * 2.0f + z_pad,
                                            homogeneous_depth);

        out->vp[c] = jce_m4_multiply(&light_proj, &light_view);
    }
    JCE_PROFILE_ZONE_END;
}
