cmake_minimum_required(VERSION 3.20)
project(JceRenderLivenessLab C CXX)
find_package(JCE REQUIRED)

# The fixture's complete boot resources are explicit generated inputs. The
# shared SDK helper owns cooking, packing and embedding; no compiler recipe here.
set(_lab_boot "$ENV{JCE_LAB_BOOT_RESOURCE_ROOT}")
if(_lab_boot STREQUAL "" OR NOT IS_DIRECTORY "${_lab_boot}/shaders")
    message(FATAL_ERROR "Set JCE_LAB_BOOT_RESOURCE_ROOT to prepared boot resources")
endif()
foreach(_target JceRenderLivenessLab JceRenderLivenessLabNoReset)
    add_executable(${_target} src/main.c src/fixture.c)
    set_target_properties(${_target} PROPERTIES
        C_STANDARD 99 C_STANDARD_REQUIRED YES)
    target_link_libraries(${_target} PRIVATE JCE::JCE)
    jce_add_pak(${_target} RESOURCE_DIRS "${_lab_boot}" resources
        NO_ENGINE_RESOURCES NO_COOK)
endforeach()
set_target_properties(JceRenderLivenessLab PROPERTIES
    OUTPUT_NAME jce_render_liveness_lab)
set_target_properties(JceRenderLivenessLabNoReset PROPERTIES
    OUTPUT_NAME jce_render_liveness_lab_no_reset)
target_compile_definitions(JceRenderLivenessLabNoReset PRIVATE LAB_DISABLE_RESET=1)
