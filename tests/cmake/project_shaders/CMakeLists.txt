cmake_minimum_required(VERSION 3.20)
project(JCEProjectShaderFixture NONE)

if(NOT JCE_SOURCE_DIR)
    message(FATAL_ERROR "JCE_SOURCE_DIR is required")
endif()

include("${JCE_SOURCE_DIR}/cmake/JCESDKHelpers.cmake")

if(EXPECT_MISSING_VARYING)
    set(_varying missing.def.sc)
else()
    if(PROJECT_SHADER_VARYING_DEF)
        set(_varying "${PROJECT_SHADER_VARYING_DEF}")
    else()
        set(_varying varying.def.sc)
    endif()
endif()

if(PROJECT_SHADER_SOURCE_DIR)
    set(_shader_source "${PROJECT_SHADER_SOURCE_DIR}")
else()
    set(_shader_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
endif()
if(PROJECT_SHADER_INCLUDE_DIR)
    set(_shader_include "${PROJECT_SHADER_INCLUDE_DIR}")
else()
    set(_shader_include "${CMAKE_CURRENT_SOURCE_DIR}/shaders/include")
endif()

jce_add_project_shaders(
    TARGET FixtureShaders
    SOURCE_DIR "${_shader_source}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders"
    VARYING_DEF "${_varying}"
    INCLUDE_DIRS "${_shader_include}"
    PROFILES ${JCE_SHADER_PROFILES}
    OUT_FILES_VAR FIXTURE_OUTPUTS)

add_custom_target(FixtureShadersAll ALL DEPENDS FixtureShaders)

if(NOT EXPECT_MISSING_VARYING)
    if(NOT FIXTURE_OUTPUTS)
        message(FATAL_ERROR "project shader helper returned no outputs")
    endif()
    get_property(_property TARGET FixtureShaders PROPERTY
        JCE_PROJECT_SHADER_OUTPUTS)
    if(NOT _property STREQUAL "${FIXTURE_OUTPUTS}")
        message(FATAL_ERROR "project shader output property mismatch")
    endif()
    foreach(_output IN LISTS FIXTURE_OUTPUTS)
        if(PROJECT_SHADER_OUTPUT_REGEX)
            set(_output_regex "${PROJECT_SHADER_OUTPUT_REGEX}")
        else()
            set(_output_regex
                "/assets/shaders/(vs|fs)_fixture_(dx11|spv|glsl|essl|mtl)\\.bin$")
        endif()
        if(NOT _output MATCHES "${_output_regex}")
            message(FATAL_ERROR "unexpected project shader output: ${_output}")
        endif()
    endforeach()
endif()
