/* Optional post-allocation GPU count: exact 24-bit RGB, alpha valid. */
#include <bgfx_compute.sh>
BUFFER_RO(b_counts, uint, 0);
IMAGE2D_WO(i_stats, rgba8, 1);
uniform vec4 u_update_params;
NUM_THREADS(1, 1, 1)
void main()
{
    uint capacity = uint(u_update_params.x);
    uint free_count = b_counts[0];
    uint emitted = b_counts[1];
    bool valid = capacity < 16777216u && free_count <= capacity && emitted <= free_count;
    uint alive = valid ? capacity - free_count + emitted : 0u;
    imageStore(i_stats, ivec2(0, 0), vec4(float(alive & 255u),
        float((alive >> 8u) & 255u), float((alive >> 16u) & 255u),
        valid ? 255.0 : 0.0) / 255.0);
}
