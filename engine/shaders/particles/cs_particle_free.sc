/* Actual dead-slot compaction; same atomic append ownership as GPU culling. */
#include <bgfx_compute.sh>
BUFFER_RO(b_particles, vec4, 0);
BUFFER_RW(b_free_slots, uint, 1);
BUFFER_RW(b_counts, uint, 2);
uniform vec4 u_update_params;
NUM_THREADS(64, 1, 1)
void main()
{
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = uint(u_update_params.x);
    if (slot >= capacity) return;
    if (!(b_particles[slot * 4u + 1u].w > 0.0)) {
        uint index;
        atomicFetchAndAdd(b_counts[0], 1u, index);
        if (index < capacity) b_free_slots[index] = slot;
    }
}
