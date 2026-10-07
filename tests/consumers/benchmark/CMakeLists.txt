cmake_minimum_required(VERSION 3.20)
project(JceBenchmark C CXX)

find_package(JCE REQUIRED)
add_executable(JceBenchmark src/main.c src/probes.c src/probes_core.c
    src/probes_ai_physics.c src/probes_contacts.c src/probes_animation.c
    src/probes_streaming.c src/probes_ui.c src/text_witness.c
    src/metrics.c src/mutations.c src/nodraw_witness.c src/asset_witness.c src/culling_witness.c
    src/occlusion_witness.c src/gpu_survivors.c src/hiz_probe.c src/rt_witness.c
    src/lod_witness.c src/light_transition.c src/light_timing_witness.c
    src/shadow_witness.c
    src/particle_witness.c src/stream_route.c src/load_witness.c
    src/async_load_witness.c src/physics_pressure_witness.c)
set_target_properties(JceBenchmark PROPERTIES
    OUTPUT_NAME jce_benchmark C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_link_libraries(JceBenchmark PRIVATE JCE::JCE)
jce_add_pak(JceBenchmark RESOURCE_DIRS resources)

# Exercise the installed SDK's ordinary host, including its post composites.
add_executable(JceBenchmarkDefault src/default_main.c src/default_rt.c)
set_target_properties(JceBenchmarkDefault PROPERTIES
    OUTPUT_NAME jce_benchmark_default C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_link_libraries(JceBenchmarkDefault PRIVATE JCE::JCE)
jce_add_pak(JceBenchmarkDefault RESOURCE_DIRS resources)
