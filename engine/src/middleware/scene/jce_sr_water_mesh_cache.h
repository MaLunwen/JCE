#ifndef JCE_SR_WATER_MESH_CACHE_H
#define JCE_SR_WATER_MESH_CACHE_H

/* Private draw-time cache owner. The environment TU provides the grid/ring
 * builders. A refused shared GPU allocation gates all water meshes, including
 * cache eviction and other viewports, against the renderer's device frame. */
static JceMesh *sr_water_mesh_acquire(JceSceneRenderer *sr, int slot,
    JceEntity entity, const JceWaterComponent *water, bool ocean_detail,
    bool legacy_fft)
{
    const bool matches = sr->water_cache[slot].used &&
        sr->water_cache[slot].mesh &&
        sr->water_cache[slot].entity == entity &&
        sr->water_cache[slot].size_x == water->size_x &&
        sr->water_cache[slot].size_z == water->size_z &&
        sr->water_cache[slot].ocean_mesh == water->ocean &&
        sr->water_cache[slot].ocean_detail == ocean_detail;
    if (matches && (!legacy_fft || sr->water_cache[slot].fft_source_vertices))
        return sr->water_cache[slot].mesh;

    const uint32_t frame = jce_renderer_get_frame_index(sr->renderer);
    if (sr->water_mesh_retry_pending &&
        sr->water_mesh_retry_context == sr->resource_prepared_context &&
        (uint32_t)(frame - sr->water_mesh_retry_frame) < 60u)
        return matches ? sr->water_cache[slot].mesh : NULL;

    JceMeshVertex *flat = NULL;
    JceMeshVertex **retained = legacy_fft ? &flat : NULL;
    JceMesh *next = water->ocean
        ? sr_water_build_rings(water->size_x, water->size_z, ocean_detail, retained)
        : sr_water_build_grid(water->size_x, water->size_z, retained);
    if (!next) {
        JCE_FREE(flat);
        sr->water_mesh_retry_pending = true;
        sr->water_mesh_retry_frame = frame;
        sr->water_mesh_retry_context = sr->resource_prepared_context;
        if (!sr->water_mesh_failure_logged) {
            const bgfx_stats_t *stats = bgfx_get_stats();
            const bgfx_caps_t *caps = bgfx_get_caps();
            LOG_WARN(LOG_TAG, "water mesh unavailable; retry after 60 device frames "
                "backend_ib=%d/%d backend_vb=%d/%d "
                "(unavailable=-1; pending requests unmeasured)",
                stats ? (int)stats->numIndexBuffers : -1,
                caps ? (int)caps->limits.maxIndexBuffers : -1,
                stats ? (int)stats->numVertexBuffers : -1,
                caps ? (int)caps->limits.maxVertexBuffers : -1);
            sr->water_mesh_failure_logged = true;
        }
        /* Retain prior complete geometry until replacement succeeds. A size
         * or topology mismatch must not submit it as the new authored plane.
         * A missing compatibility FFT source may use the analytic fallback. */
        return matches ? sr->water_cache[slot].mesh : NULL;
    }

    if (sr->water_cache[slot].mesh)
        jce_mesh_destroy(sr->water_cache[slot].mesh);
    JCE_FREE(sr->water_cache[slot].fft_source_vertices);
    sr->water_cache[slot].mesh = next;
    sr->water_cache[slot].fft_source_vertices = flat;
    sr->water_cache[slot].ocean_mesh = water->ocean;
    sr->water_cache[slot].ocean_detail = ocean_detail;
    sr->water_cache[slot].size_x = water->size_x;
    sr->water_cache[slot].size_z = water->size_z;
    sr->water_cache[slot].entity = entity;
    sr->water_cache[slot].used = true;
    if (sr->water_mesh_failure_logged)
        LOG_INFO(LOG_TAG, "water mesh allocation recovered");
    sr->water_mesh_retry_pending = false;
    sr->water_mesh_failure_logged = false;
    return next;
}

#endif
