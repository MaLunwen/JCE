/* On-demand diagnostic export. Exact uint32 counters as two float16-bit
 * words; reuse the typed RGBA32F asynchronous texture readback contract. */
#include <bgfx_compute.sh>

BUFFER_RO(b_counter, uint, 0);
IMAGE2D_WO(i_counts, rgba32f, 1);
uniform vec4 u_snapshot_params; /* count low/high16, width, height */

NUM_THREADS(8, 8, 1)
void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    uint width = uint(u_snapshot_params.z);
    uint height = uint(u_snapshot_params.w);
    if (pixel.x >= width || pixel.y >= height) return;
    uint count = uint(u_snapshot_params.x) | (uint(u_snapshot_params.y) << 16u);
    uint index = pixel.y * width + pixel.x;
    uint survivors = 0u;
    if (index < count) survivors = b_counter[index];
    imageStore(i_counts, ivec2(pixel), vec4(
        float(survivors & 65535u), float(survivors >> 16u), 0.0, 0.0));
}
