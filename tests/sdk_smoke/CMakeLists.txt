# tests/sdk_smoke — plain-C99 SDK consumer (C ABI regression gate).
#
# Built OUT-OF-TREE against an installed SDK via find_package(JCE):
#
#     cmake -S tests/sdk_smoke -B <bdir> -DJCE_DIR=<sdk>/lib/cmake/JCE
#
# Driven by `python scripts/jce.py sdk --smoke` / `jce.py smoke`. It gates:
#   * the public headers compile as strict C99 (no C++ leakage),
#   * the fat lib links from a foreign project (JCE::JCE),
#   * shipped host tools cook+pack real assets (jce_add_pak turnkey path),
#   * the engine boots headless (JCE_BACKEND=noop) and runs one frame.
cmake_minimum_required(VERSION 3.20)
# CXX is required even though our sources are pure C: the JCE fat lib
# contains C++ objects (bgfx, physics, ...) and the final link must use
# the C++ driver (same note as the science_lab consumer).
project(JceSdkSmoke C CXX)

find_package(JCE REQUIRED)

if(NOT JCE_HOST_TOOLS_FOUND)
	message(FATAL_ERROR
		"sdk_smoke: this SDK ships no usable host tools "
		"(jce_cook/jce_pak/jce_bin2obj) — it cannot cook assets and is "
		"degraded. The smoke gate intentionally refuses to test it.")
endif()

add_executable(JceSdkSmoke src/main.c)
set_target_properties(JceSdkSmoke PROPERTIES
	OUTPUT_NAME          jce_sdk_smoke
	C_STANDARD           99
	C_STANDARD_REQUIRED  YES)
target_link_libraries(JceSdkSmoke PRIVATE JCE::JCE)

# Turnkey cook + pack + embed — the exact consumer path under test.
jce_add_pak(JceSdkSmoke RESOURCE_DIRS assets)

# Verify the actual packaged AV1 provider, not a source-tree dependency path.
if(MSVC)
    set(_codec_map "${CMAKE_CURRENT_BINARY_DIR}/sdk_codec.map")
    target_link_options(JceSdkSmoke PRIVATE "/MAP:${_codec_map}")
    add_custom_command(TARGET JceSdkSmoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DMAP_FILE=${_codec_map}"
            -P "${JCE_DIR}/JCEVerifyCodecLink.cmake"
        VERBATIM)
endif()
