/*
 * cs_cull_reset.sc -- GPU-driven indirect cull: per-run counter reset.
 *
 * The compact/indirect cull path (cs_cull_compact + cs_build_indirect, roadmap
 * #18 Direction C) keeps a PERSISTENT per-run survivor counter buffer that must
 * be zeroed before each frame's atomic-append compaction.  The original 1:1 cull
 * (cs_cull_frustum) had no counters; this path does, so it needs a clear.
 *
 * Clearing on the GPU (one thread per run) instead of a CPU
 * bgfx_update_dynamic_* avoids the D3D12 per-frame Upload-heap staging path that
 * NULL-derefs under resource pressure (see jce_gpu_scene.c header).  One dispatch
 * before cs_cull_compact; bgfx inserts a UAV barrier between same-view compute
 * dispatches so the zero is visible to the atomicAdd.
 *
 * Buffer / uniform layout (must mirror jce_gpu_scene.c):
 *   b_counter : RW uint per run (the survivor counter), zeroed here.
 *   u_cull_reset_params.x : run_count (number of counters to clear)
 */

#include <bgfx_compute.sh>

BUFFER_RW(b_counter, uint, 0);

uniform vec4 u_cull_reset_params;

NUM_THREADS(64, 1, 1)
void main()
{
	uint id    = gl_GlobalInvocationID.x;
	uint count = uint(u_cull_reset_params.x);
	if (id >= count) {
		return;
	}
	b_counter[id] = 0u;
}
