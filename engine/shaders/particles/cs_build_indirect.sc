/*
 * cs_build_indirect.sc -- GPU-driven indirect cull: fill the indirect draw-args
 * buffer from the compacted per-run survivor counts (roadmap #18, Direction C).
 *
 * One thread per primitive draw. After cs_cull_compact has compacted one
 * visibility group's survivors into
 * b_visible[run_base .. run_base+survivors), this pass writes every primitive's
 * drawIndexedIndirect args. Primitives from one model share the same survivor
 * counter and visible partition, so visibility is evaluated only once:
 *
 *   numIndices    = run's mesh index count (from b_runmeta)
 *   numInstances  = b_counter[counter_index]    (survivors)
 *   startIndex    = 0
 *   startVertex   = 0
 *   startInstance = run_base                    (partition slot in b_visible)
 *
 * The draw binds the WHOLE compact b_visible as its instance stream from offset
 * 0; startInstance selects the run's partition.  bgfx orders this dispatch after
 * cs_cull_compact on the same compute view (UAV barrier), and the compute view
 * before the color view, so the indirect args + compact instances are resident
 * before the first indirect draw.
 *
 * Buffer / record layout (must mirror jce_gpu_scene.c):
 *   b_runmeta  : RO, 1 vec4 per draw:
 *                (numIndices, run_base, counter_index, _) stored as FLOAT
 *                values (bgfx exposes a vertex buffer's compute SRV as RGBA32F,
 *                so the meta is stored float-typed and uint()'d here — same trick
 *                the scene-record ids use); ring-rebased by u_indirect_params.y
 *                (the run-meta transient alloc vec4 offset).
 *   b_counter  : RO, uint per run (survivor count from cs_cull_compact)
 *   b_indirect : WO, the bgfx indirect buffer (uvec4[]); drawIndexedIndirect
 *                writes 2 uvec4 per element, addressed by run index.
 *
 * Uniforms:
 *   u_indirect_params : (run_count, runmeta_vec4_offset, _, _)
 */

#include <bgfx_compute.sh>

BUFFER_RO(b_runmeta,  vec4, 0);
BUFFER_RO(b_counter,  uint, 1);
BUFFER_WO(b_indirect, uvec4, 2);

uniform vec4 u_indirect_params;

NUM_THREADS(64, 1, 1)
void main()
{
	uint run   = gl_GlobalInvocationID.x;
	uint count = uint(u_indirect_params.x);
	if (run >= count) {
		return;
	}

	uint meta_off = uint(u_indirect_params.y);
	vec4 meta = b_runmeta[meta_off + run];

	uint numIndices   = uint(meta.x);
	uint run_base     = uint(meta.y);
	uint counter_index = uint(meta.z);
	uint numInstances = b_counter[counter_index];

	drawIndexedIndirect(
		b_indirect,        /* target indirect buffer            */
		run,               /* element index (one per run)       */
		numIndices,        /* index count of this run's mesh    */
		numInstances,      /* survivor instance count           */
		0u,                /* startIndex                        */
		0u,                /* startVertex                       */
		run_base           /* startInstance (partition slot)    */
		);
}
