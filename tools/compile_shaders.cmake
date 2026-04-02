# compile_shaders.cmake
#
# bgfx shader compilation utilities for JCE.
# Provides jce_compile_shaders() for CMakeLists.txt integration.
#
# Scans a directory for vs_*.sc / fs_*.sc / cs_*.sc files and compiles
# each with bgfx shaderc to four API profiles:
#
#   D3D11      (vs_5_0 / ps_5_0)
#   Vulkan     (spirv)
#   OpenGL     (120)
#   OpenGL ES  (300_es)
#
# Output: {name}_{profile}.bin  (e.g. vs_color_dx11.bin)
#
# Requires: bgfx Conan package with tools=True  (provides shaderc).
#           find_package(bgfx) must be called before this function.
#
# Usage:
#   include(tools/compile_shaders.cmake)
#   jce_compile_shaders(
#       TARGET        CompileShaders
#       SHADER_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/main/native/shaders
#       OUTPUT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/main/resources/assets/shaders
#       OUT_FILES_VAR COMPILED_SHADER_FILES
#   )

function(jce_compile_shaders)
    cmake_parse_arguments(ARG ""
        "TARGET;SHADER_DIR;VARYING_DEF;OUTPUT_DIR;OUT_FILES_VAR"
        "" ${ARGN})

    # ── defaults ──────────────────────────────────────────────────
    if(NOT ARG_VARYING_DEF)
        set(ARG_VARYING_DEF "varying.def.sc")
    endif()

    # ── validation ────────────────────────────────────────────────
    foreach(_req TARGET SHADER_DIR OUTPUT_DIR)
        if(NOT ARG_${_req})
            message(FATAL_ERROR
                "jce_compile_shaders: ${_req} is required")
        endif()
    endforeach()

    # ── locate shaderc ────────────────────────────────────────────
    # JCE_SHADERC_EXECUTABLE can override the auto-detected path.
    # Required for cross-compilation (e.g. Emscripten/Android) where the bgfx
    # target package has tools=False and a host-built shaderc.exe must be used.
    if(JCE_SHADERC_EXECUTABLE)
        set(_shaderc "${JCE_SHADERC_EXECUTABLE}")
    else()
        set(_shaderc
            "${bgfx_PACKAGE_FOLDER_RELEASE}/bin/shaderc${CMAKE_EXECUTABLE_SUFFIX}")
    endif()
    if(NOT EXISTS "${_shaderc}")
        message(FATAL_ERROR
            "shaderc not found at ${_shaderc}.\n"
            "Native builds:      ensure bgfx is built with tools=True.\n"
            "Cross-compilation:  pass -DJCE_SHADERC_EXECUTABLE=/path/to/shaderc.exe")
    endif()

    # ── bgfx shader include path ──────────────────────────────────
    if(NOT BGFX_SHADER_INCLUDE_PATH)
        message(FATAL_ERROR
            "BGFX_SHADER_INCLUDE_PATH not set.\n"
            "Ensure find_package(bgfx) was called before "
            "jce_compile_shaders().")
    endif()

    set(_varying "${ARG_SHADER_DIR}/${ARG_VARYING_DEF}")

    # ── enumerate shader sources ──────────────────────────────────
    file(GLOB _sources
        "${ARG_SHADER_DIR}/vs_*.sc"
        "${ARG_SHADER_DIR}/fs_*.sc"
        "${ARG_SHADER_DIR}/cs_*.sc")

    if(NOT _sources)
        message(WARNING
            "jce_compile_shaders: no .sc files found in "
            "${ARG_SHADER_DIR}")
    endif()

    # ── profile table ─────────────────────────────────────────────
    #  suffix    platform   profile
    set(_suffixes  dx11     spv    glsl  essl    mtl)
    set(_platforms windows  linux  linux android osx)
    set(_profiles  s_5_0    spirv  120   300_es  metal)

    list(LENGTH _suffixes _num_profiles)
    math(EXPR _max_idx "${_num_profiles} - 1")

    # Ensure output directory exists.
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")

    # ── per-shader, per-profile custom commands ───────────────────
    set(_all_outputs "")

    foreach(_src IN LISTS _sources)
        get_filename_component(_name "${_src}" NAME_WE)

        # Determine shader type from filename prefix.
        if(_name MATCHES "^vs_")
            set(_type "vertex")
        elseif(_name MATCHES "^fs_")
            set(_type "fragment")
        elseif(_name MATCHES "^cs_")
            set(_type "compute")
        else()
            continue()
        endif()

        foreach(_idx RANGE 0 ${_max_idx})
            list(GET _suffixes  ${_idx} _suffix)
            list(GET _platforms ${_idx} _platform)
            list(GET _profiles  ${_idx} _profile)

            # dx11/HLSL requires the DirectX shader compiler (DXC) which is
            # only available on Windows hosts.  Skip on macOS/Linux hosts.
            if(_suffix STREQUAL "dx11" AND NOT CMAKE_HOST_WIN32)
                continue()
            endif()

            set(_out "${ARG_OUTPUT_DIR}/${_name}_${_suffix}.bin")

            add_custom_command(
                OUTPUT  "${_out}"
                COMMAND "${_shaderc}"
                        -f "${_src}"
                        -o "${_out}"
                        --type "${_type}"
                        --platform "${_platform}"
                        -p "${_profile}"
                        --varyingdef "${_varying}"
                        -i "${BGFX_SHADER_INCLUDE_PATH}"
                DEPENDS "${_src}" "${_varying}"
                COMMENT "Shader: ${_name} (${_suffix})"
                VERBATIM)

            list(APPEND _all_outputs "${_out}")
        endforeach()
    endforeach()

    # ── custom target ─────────────────────────────────────────────
    add_custom_target(${ARG_TARGET} DEPENDS ${_all_outputs})

    # ── return output list to caller ──────────────────────────────
    if(ARG_OUT_FILES_VAR)
        set(${ARG_OUT_FILES_VAR} ${_all_outputs} PARENT_SCOPE)
    endif()
endfunction()
