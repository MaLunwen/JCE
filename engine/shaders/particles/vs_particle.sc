$input  a_position, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_texcoord0, v_pcolor

/*
 * vs_particle.sc -- view-aligned billboard expansion for GPU particles.
 *
 * One quad (4 vertices) is dispatched per draw, instanced once per
 * particle slot.  i_data0..3 carry the particle state directly out of
 * the compute pool.  Dead slots (size == 0) collapse to a degenerate
 * point so the rasteriser discards them with no fragment work.
 *
 * CPU instances carry velocity*stretch in i_data1.xyz. GPU instances carry
 * raw velocity there and set u_particle_misc.w to the stretch multiplier.
 * u_particle_misc.y enables motion-aligned streaks for the draw.
 */

#include <bgfx_shader.sh>

uniform vec4 u_particle_misc; // .y enables stretch, .w scales raw GPU velocity
#include "particle_roof.sh"
#if BGFX_SHADER_LANGUAGE_HLSL
#include "particle_rain_surface.sh"
#endif

void main()
{
    vec3  center = i_data0.xyz;
    float size   = i_data3.x;

    // CPU instances have zero in the GPU lifetime slot and cull before upload.
    bool gpu = i_data1.w > 0.0;
    bool hidden = size <= 0.0 || (gpu && particle_roof_blocks(center, i_data1.xyz));
#if BGFX_SHADER_LANGUAGE_HLSL
    bool impact = gpu && i_data3.z > 0.5;
    hidden = hidden || (impact && !particle_rain_enabled());
#endif
    if (hidden) {
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        v_pcolor    = vec4(0.0, 0.0, 0.0, 0.0);
        v_texcoord0 = vec2(0.0, 0.0);
#if BGFX_SHADER_LANGUAGE_HLSL
    } else if (impact) {
        float progress = clamp(i_data0.w / i_data1.w, 0.0, 1.0);
        // A bounded quarter-metre surface response, not a metre-wide halo.
        float diameter = mix(0.07, 0.34, progress);
        vec2 xz = center.xz + vec2(a_position.x, -a_position.y) * diameter;
        float surface_y;
        bool wet = particle_rain_surface(xz, surface_y);
        vec3 world = vec3(xz.x, (wet ? surface_y : center.y) + 0.035, xz.y);
        gl_Position = mul(u_viewProj, vec4(world, 1.0));
        float fade = (1.0 - progress) * (1.0 - progress);
        // Rain-ring branch passes world XZ for the same wet test in FS.
        v_pcolor = vec4(xz, i_data2.b, i_data2.a * fade);
        v_texcoord0 = a_texcoord0 + vec2(2.0, 0.0);
#endif
    } else {
        float stretch_scale = u_particle_misc.w > 0.0 ? u_particle_misc.w : 1.0;
#define JCE_PARTICLE_STRETCH_VECTOR (i_data1.xyz * stretch_scale)
#define JCE_PARTICLE_CORNER a_position
#include "particle_billboard.sh"
#undef JCE_PARTICLE_CORNER
#undef JCE_PARTICLE_STRETCH_VECTOR
        v_pcolor    = i_data2;
        v_texcoord0 = a_texcoord0;
    }
}
