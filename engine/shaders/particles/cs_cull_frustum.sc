/*
 * cs_cull_frustum.sc -- GPU-driven frustum cull (roadmap #18, Phase 1).
 *
 * One thread per GPUScene record across the WHOLE color-pass instanced batch.
 * Tests the record's world-space AABB (centre + extent) against the 6 camera
 * frustum planes and writes the per-instance world matrix to the record's OWN
 * fixed visible-buffer slot (a 1:1 record->slot mapping): survivors get their
 * world matrix, culled records get a ZERO matrix so the instanced draw that
 * sources the run's contiguous slice rasterizes them as degenerate (zero-area)
 * triangles.  Every slot is written exactly once by exactly one thread each
 * frame, so there is NO atomic counter and NO pre-clear pass — both eliminated
 * to avoid the bgfx D3D12 per-frame dynamic-buffer staging path (a NULL-deref
 * crash under resource pressure).  See jce_gpu_scene.c.
 *
 * Batching all model-runs into one dispatch is required for correctness: bgfx
 * orders the cull (compute view, base+9) entirely before the color view
 * (base+0), so every instance of every run is resident in the visible buffer by
 * the time the first draw runs.
 *
 * Scene records come from a TRANSIENT instance-data buffer (the per-frame ring
 * bgfx already uploads with a single staging allocation), bound as a compute
 * SRV.  Because the SRV covers the whole ring from element 0, the shader
 * rebases each record by u_cull_params.z (the allocation's vec4 offset in the
 * ring).
 *
 * Buffer / record layout (must mirror jce_gpu_scene.c):
 *   b_scene   : RO, 7 vec4 per record (stride 112 B), ring-rebased by .z
 *       [0..3] world matrix columns col0..col3
 *       [4]    aabb_center.xyz, bounding-sphere radius (w, unused by cull)
 *       [5]    aabb_extent.xyz (half-extents), unused w
 *       [6]    ids: reserved (run_base/run_index no longer used; slot == id)
 *   b_visible : RW, 4 vec4 per slot (a mat4 instance stream); slot == record id
 *
 * Uniforms:
 *   u_cull_planes[6] : normalised frustum planes (xyz = normal, w = d)
 *   u_cull_params    : (record_count, visible_capacity, scene_vec4_offset, _)
 */

#include <bgfx_compute.sh>

BUFFER_RO(b_scene,   vec4, 0);
BUFFER_RW(b_visible, vec4, 1);

uniform vec4 u_cull_planes[6];
uniform vec4 u_cull_params;

#define CULL_RECORD_VEC4 7u

NUM_THREADS(64, 1, 1)
void main()
{
    uint id    = gl_GlobalInvocationID.x;
    uint count = uint(u_cull_params.x);
    if (id >= count) {
        return;
    }

    /* Rebase into the transient ring: the SRV starts at element 0 of the whole
     * ring, so add the allocation's vec4 offset. */
    uint scene_off = uint(u_cull_params.z);
    uint base = scene_off + id * CULL_RECORD_VEC4;

    vec4 c_r = b_scene[base + 4u];   /* center.xyz, radius */
    vec4 ext = b_scene[base + 5u];   /* half-extents.xyz   */
    vec3 center = c_r.xyz;
    vec3 e      = ext.xyz;

    /* AABB vs frustum: cull when the box's farthest extent toward a plane
     * normal is still behind that plane, for ANY of the 6 planes. */
    bool visible = true;
    for (int p = 0; p < 6; ++p) {
        vec4 pl = u_cull_planes[p];
        float dist = dot(pl.xyz, center) + pl.w;
        float r = dot(abs(pl.xyz), e);
        if (dist + r < 0.0) {
            visible = false;
            break;
        }
    }

    /* 1:1 slot: record id -> visible slot id (no compaction, no atomics).
     * Survivors write their world matrix; culled records write a zero matrix
     * (degenerate instance). */
    uint dst = id * 4u;
    if (visible) {
        b_visible[dst + 0u] = b_scene[base + 0u];
        b_visible[dst + 1u] = b_scene[base + 1u];
        b_visible[dst + 2u] = b_scene[base + 2u];
        b_visible[dst + 3u] = b_scene[base + 3u];
    } else {
        b_visible[dst + 0u] = vec4_splat(0.0);
        b_visible[dst + 1u] = vec4_splat(0.0);
        b_visible[dst + 2u] = vec4_splat(0.0);
        b_visible[dst + 3u] = vec4_splat(0.0);
    }
}
