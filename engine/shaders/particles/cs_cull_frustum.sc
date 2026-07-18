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

/* Hi-Z occlusion inputs (large-world #5) — see cs_cull_compact.sc. s_hiz at
 * stage 2 (this 1:1 path has no counter buffer). Gated by u_cull_params.w. */
SAMPLER2D(s_hiz, 2);
uniform vec4 u_cull_planes[6];
uniform vec4 u_cull_params;
uniform mat4 u_cull_viewproj;
uniform vec4 u_hiz_params;

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

    /* Hi-Z occlusion (large-world #5, opt-in via u_cull_params.w > 0.5). INLINED
     * — NOT a helper taking s_hiz as an argument (that GLSL crashed the GL driver
     * at editor startup, revert d7d785c4). Window-space depth [0,1] on every
     * backend; only NDC-z reconstruction differs (mirrors fs_volfog.sc). */
    if (visible && u_cull_params.w > 0.5) {
        vec2  mn_uv = vec2(1.0e9, 1.0e9);
        vec2  mx_uv = vec2(-1.0e9, -1.0e9);
        float mn_z  = 1.0e9;
        bool  keep_visible = false;
        for (int cc = 0; cc < 8; ++cc) {
            vec3 corner = center + vec3(
                (cc & 1) != 0 ?  e.x : -e.x,
                (cc & 2) != 0 ?  e.y : -e.y,
                (cc & 4) != 0 ?  e.z : -e.z);
            vec4 clip = mul(u_cull_viewproj, vec4(corner, 1.0));
            if (clip.w <= 1.0e-6) { keep_visible = true; break; }
            vec3 ndc = clip.xyz / clip.w;
#if BGFX_SHADER_LANGUAGE_GLSL
            float win_z = ndc.z * 0.5 + 0.5;
#else
            float win_z = ndc.z;
#endif
            if (win_z < 0.0) { keep_visible = true; break; }
            vec2 uv = ndc.xy * 0.5 + 0.5;
            mn_uv = min(mn_uv, uv);
            mx_uv = max(mx_uv, uv);
            mn_z  = min(mn_z, win_z);
        }
        if (!keep_visible &&
            !(mx_uv.x < 0.0 || mn_uv.x > 1.0 || mx_uv.y < 0.0 || mn_uv.y > 1.0)) {
            float span = max((mx_uv.x - mn_uv.x) * u_hiz_params.x,
                             (mx_uv.y - mn_uv.y) * u_hiz_params.y);
            float mip  = ceil(log2(max(span, 1.0)));
            if (mip < u_hiz_params.z) {
                float occ = texture2DLod(s_hiz, vec2(mn_uv.x, mn_uv.y), mip).r;
                occ = max(occ, texture2DLod(s_hiz, vec2(mx_uv.x, mn_uv.y), mip).r);
                occ = max(occ, texture2DLod(s_hiz, vec2(mn_uv.x, mx_uv.y), mip).r);
                occ = max(occ, texture2DLod(s_hiz, vec2(mx_uv.x, mx_uv.y), mip).r);
                if (mn_z > occ) visible = false;
            }
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
