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
    src/async_load_witness.c src/physics_pressure_witness.c
    src/probe_capture_witness.c)
set_target_properties(JceBenchmark PROPERTIES
    OUTPUT_NAME jce_benchmark C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_link_libraries(JceBenchmark PRIVATE JCE::JCE)
jce_add_pak(JceBenchmark RESOURCE_DIRS resources)

# Exercise the installed SDK's ordinary host, including its post composites.
add_executable(JceBenchmarkDefault src/default_main.c src/default_rt.c src/metrics.c)
set_target_properties(JceBenchmarkDefault PROPERTIES
    OUTPUT_NAME jce_benchmark_default C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_link_libraries(JceBenchmarkDefault PRIVATE JCE::JCE)
jce_add_pak(JceBenchmarkDefault RESOURCE_DIRS resources)

# Installed-SDK CPU transport controls, excluded from normal product builds.
# They include the complete metrics owner and real SDK JSON facade; only public
# GPU/introspection/output calls are typed ports, no renderer/GPU is created.
set(_bench_metrics_owner "${CMAKE_CURRENT_SOURCE_DIR}/src/metrics.c")
file(READ "${_bench_metrics_owner}" _bench_metrics_source)
set(_bench_metrics_wire "observation.gpu_timing_available = jce_renderer_get_gpu_timing_observation(\n        &observation.gpu_timing,sizeof(observation.gpu_timing));")
string(FIND "${_bench_metrics_source}" "${_bench_metrics_wire}" _bench_metrics_first)
if(_bench_metrics_first LESS 0)
    message(FATAL_ERROR "Benchmark metrics control requires actual public timing getter wire")
endif()
string(LENGTH "${_bench_metrics_wire}" _bench_metrics_length)
math(EXPR _bench_metrics_last "${_bench_metrics_first}+${_bench_metrics_length}")
string(SUBSTRING "${_bench_metrics_source}" ${_bench_metrics_last} -1 _bench_metrics_tail)
string(FIND "${_bench_metrics_tail}" "${_bench_metrics_wire}" _bench_metrics_duplicate)
if(NOT _bench_metrics_duplicate LESS 0)
    message(FATAL_ERROR "Benchmark metrics getter wire must be unique")
endif()
string(REPLACE "${_bench_metrics_wire}" "observation.gpu_timing_available = false;"
    _bench_metrics_old "${_bench_metrics_source}")
set(_bench_metrics_dir "${CMAKE_CURRENT_BINARY_DIR}/metrics-control")
file(MAKE_DIRECTORY "${_bench_metrics_dir}")
set(_bench_metrics_old_path "${_bench_metrics_dir}/missing_timing_wire.c")
unset(_bench_metrics_previous)
if(EXISTS "${_bench_metrics_old_path}")
    file(READ "${_bench_metrics_old_path}" _bench_metrics_previous)
endif()
if(NOT _bench_metrics_previous STREQUAL _bench_metrics_old)
    file(WRITE "${_bench_metrics_old_path}" "${_bench_metrics_old}")
endif()
set(_bench_metrics_initial_wire "const bool initial = frame == 0 && event &&\n            (!strcmp(event,\"initialized\") || !strcmp(event,\"default-initialized\"));")
string(FIND "${_bench_metrics_source}" "${_bench_metrics_initial_wire}" _bench_metrics_initial_first)
if(_bench_metrics_initial_first LESS 0)
    message(FATAL_ERROR "Benchmark zero-draw control requires actual initial-event wire")
endif()
string(LENGTH "${_bench_metrics_initial_wire}" _bench_metrics_initial_length)
math(EXPR _bench_metrics_initial_last "${_bench_metrics_initial_first}+${_bench_metrics_initial_length}")
string(SUBSTRING "${_bench_metrics_source}" ${_bench_metrics_initial_last} -1 _bench_metrics_initial_tail)
string(FIND "${_bench_metrics_initial_tail}" "${_bench_metrics_initial_wire}" _bench_metrics_initial_duplicate)
if(NOT _bench_metrics_initial_duplicate LESS 0)
    message(FATAL_ERROR "Benchmark initial-event wire must be unique")
endif()
string(REPLACE "${_bench_metrics_initial_wire}" "const bool initial = frame == 0;"
    _bench_metrics_old_initial "${_bench_metrics_source}")
set(_bench_metrics_old_initial_path "${_bench_metrics_dir}/old_frame_replace.c")
unset(_bench_metrics_previous)
if(EXISTS "${_bench_metrics_old_initial_path}")
    file(READ "${_bench_metrics_old_initial_path}" _bench_metrics_previous)
endif()
if(NOT _bench_metrics_previous STREQUAL _bench_metrics_old_initial)
    file(WRITE "${_bench_metrics_old_initial_path}" "${_bench_metrics_old_initial}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bench_metrics_owner}")
foreach(_bench_metrics_target IN ITEMS JceBenchmarkMetricsWire JceBenchmarkMetricsMissingWire JceBenchmarkMetricsOldFrameReplace)
    add_executable(${_bench_metrics_target} EXCLUDE_FROM_ALL src/test_metrics_gpu_timing.c)
    # Sanctioned native test target: same SDK archive/deps, caller-owned main.
    # Product targets above retain JCE::JCE and its application registration.
    target_link_libraries(${_bench_metrics_target} PRIVATE JCE::Core)
    target_include_directories(${_bench_metrics_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(${_bench_metrics_target} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
    if(_bench_metrics_target STREQUAL "JceBenchmarkMetricsMissingWire")
        target_compile_definitions(${_bench_metrics_target} PRIVATE JCE_BENCH_METRICS_OWNER="${_bench_metrics_old_path}")
    elseif(_bench_metrics_target STREQUAL "JceBenchmarkMetricsOldFrameReplace")
        target_compile_definitions(${_bench_metrics_target} PRIVATE JCE_BENCH_METRICS_OWNER="${_bench_metrics_old_initial_path}")
    endif()
endforeach()
foreach(_bench_metrics_var IN ITEMS owner source wire first length last tail duplicate old dir old_path previous target initial_wire initial_first initial_length initial_last initial_tail initial_duplicate old_initial old_initial_path)
    unset(_bench_metrics_${_bench_metrics_var})
endforeach()
unset(_bench_metrics_var)
