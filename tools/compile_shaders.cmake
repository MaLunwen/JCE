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
#       OUTPUT_DIR    ${CMAKE_CURRENT_BINARY_DIR}/generated/assets/shaders
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
    # Priority order:
    #   1. $ENV{JCE_SHADERC_EXECUTABLE}  — environment variable (CI / shell)
    #   2. JCE_SHADERC_EXECUTABLE        — CMake cache variable (-D flag)
    #   3. Conan package folder candidates
    # If none is found, emit a WARNING and skip shader custom commands so
    # that a pre-baked pak can still link the editor without re-compiling
    # shaders.  Do NOT FATAL_ERROR — shipped paks keep working.
    if(DEFINED ENV{JCE_SHADERC_EXECUTABLE} AND EXISTS "$ENV{JCE_SHADERC_EXECUTABLE}")
        set(_shaderc "$ENV{JCE_SHADERC_EXECUTABLE}")
    elseif(JCE_SHADERC_EXECUTABLE AND EXISTS "${JCE_SHADERC_EXECUTABLE}")
        set(_shaderc "${JCE_SHADERC_EXECUTABLE}")
    else()
        # Try Release package folder first (typical native build),
        # then Debug package folder (when CMAKE_BUILD_TYPE=Debug),
        # then the Debug build-tree location (Conan keeps shaderc only there).
        set(_shaderc_candidates
            "${bgfx_PACKAGE_FOLDER_RELEASE}/bin/shaderc${CMAKE_EXECUTABLE_SUFFIX}"
            "${bgfx_PACKAGE_FOLDER_DEBUG}/bin/shaderc${CMAKE_EXECUTABLE_SUFFIX}"
            "${bgfx_PACKAGE_FOLDER_DEBUG}/../b/build/Debug/cmake/bgfx/shaderc${CMAKE_EXECUTABLE_SUFFIX}"
            "${bgfx_PACKAGE_FOLDER_RELEASE}/../b/build/Release/cmake/bgfx/shaderc${CMAKE_EXECUTABLE_SUFFIX}")
        set(_shaderc "")
        foreach(_c IN LISTS _shaderc_candidates)
            if(_c AND EXISTS "${_c}")
                set(_shaderc "${_c}")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _shaderc OR NOT EXISTS "${_shaderc}")
        message(WARNING
            "shaderc not found — shader rebuilds disabled.\n"
            "Pre-baked pak shaders are still usable.\n"
            "To enable shader compilation set one of:\n"
            "  env  JCE_SHADERC_EXECUTABLE=/path/to/shaderc${CMAKE_EXECUTABLE_SUFFIX}\n"
            "  cmake -DJCE_SHADERC_EXECUTABLE=/path/to/shaderc${CMAKE_EXECUTABLE_SUFFIX}\n"
            "Or ensure bgfx is built with tools=True.")
        # Create an empty no-op target so callers that add_dependencies on
        # ARG_TARGET still configure cleanly.
        if(NOT TARGET ${ARG_TARGET})
            add_custom_target(${ARG_TARGET})
        endif()
        return()
    endif()

    # ── bgfx shader include path ──────────────────────────────────
    if(NOT BGFX_SHADER_INCLUDE_PATH)
        message(FATAL_ERROR
            "BGFX_SHADER_INCLUDE_PATH not set.\n"
            "Ensure find_package(bgfx) was called before "
            "jce_compile_shaders().")
    endif()

    set(_varying "${ARG_SHADER_DIR}/${ARG_VARYING_DEF}")

    # ── cross-backend portability lint ────────────────────────────
    # tools/shader_lint.py bans constructs that COMPILE on every bgfx
    # profile but mean different things per backend (raw multi-arg
    # matrix constructors pack rows on HLSL / columns on GLSL; matrix
    # '*' is component-wise on HLSL) — the class of bug where fs_pbr's
    # mat3(T,B,N) TBN was transposed on OpenGL/WASM only and every
    # light pool rendered cut in half. A finding fails the build BEFORE
    # any shaderc invocation. Skipped with a warning when no Python is
    # available (matches the shaderc-missing policy above).
    set(_lint_stamp "")
    find_program(JCE_SHADER_LINT_PYTHON NAMES python3 python py)
    if(JCE_SHADER_LINT_PYTHON)
        set(_lint_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/shader_lint.py")
        file(GLOB _lint_inputs
            "${ARG_SHADER_DIR}/*.sc"
            "${ARG_SHADER_DIR}/*.sh")
        set(_lint_stamp
            "${CMAKE_BINARY_DIR}/shader_lint/${ARG_TARGET}.stamp")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shader_lint")
        add_custom_command(
            OUTPUT  "${_lint_stamp}"
            COMMAND "${JCE_SHADER_LINT_PYTHON}" "${_lint_script}"
                    --quiet "${ARG_SHADER_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_lint_stamp}"
            DEPENDS ${_lint_inputs} "${_lint_script}"
            COMMENT "ShaderLint: ${ARG_TARGET}"
            VERBATIM)
    else()
        message(WARNING
            "shader_lint skipped — no python interpreter found. "
            "Cross-backend shader portability is NOT being checked.")
    endif()

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
    # essl1 = OpenGL ES 2.0 / WebGL 1.0 (P3-34).  Most modern shaders
    # cannot target it (no derivatives without ext, no MRT, no instancing,
    # no SSBO, no compute) — entries in the allowlist below are limited
    # to a hand-vetted basic-rendering subset.  Other profiles continue
    # to compile every shader.
    set(_suffixes  dx11     spv    glsl  essl    essl1   mtl)
    set(_platforms windows  linux  linux android android osx)
    set(_profiles  s_5_0    spirv  120   300_es  100_es  metal)

    list(LENGTH _suffixes _num_profiles)
    math(EXPR _max_idx "${_num_profiles} - 1")

    # JCE_SHADER_PROFILES (parent scope, set by top-level CMakeLists)
    # may restrict the set of suffixes we compile for.  Empty/unset =
    # compile every profile (legacy behaviour).
    set(_profile_filter "${JCE_SHADER_PROFILES}")

    # GLES2 / WebGL1 shader allowlist — only these names are compiled
    # for the essl1 profile.  Keep this in sync with what the renderer
    # actually needs on legacy mobile / WebGL1 backends.  Override from
    # the parent scope by setting JCE_SHADER_GLES2_ALLOWLIST.
    if(NOT DEFINED JCE_SHADER_GLES2_ALLOWLIST)
        set(JCE_SHADER_GLES2_ALLOWLIST
            vs_color fs_color
            vs_textured fs_textured
            vs_mesh fs_mesh
            vs_grid fs_grid
            vs_sky  fs_sky
            vs_imgui fs_imgui
            vs_postfx fs_chromatic fs_grayscale fs_vignette fs_tonemap)
            # fs_composite is NOT in the essl1 allowlist: it uses SAMPLER3D /
            # texture3D for the 3D-LUT colour grade, which are absent from
            # GLSL ES 1.00 (essl1/100_es).  The LUT grade is a non-trivial
            # feature that cannot be expressed in GLES2 anyway.
    endif()

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

            # Skip profiles not requested for this build's target platform.
            if(_profile_filter)
                if(NOT _suffix IN_LIST _profile_filter)
                    continue()
                endif()
            endif()

            # dx11/HLSL requires the DirectX shader compiler (DXC) which is
            # only available on Windows hosts.  Skip on macOS/Linux hosts.
            if(_suffix STREQUAL "dx11" AND NOT CMAKE_HOST_WIN32)
                continue()
            endif()

            # essl1 (GLES2/WebGL1) is opt-in per-shader via the allowlist.
            if(_suffix STREQUAL "essl1")
                if(NOT _name IN_LIST JCE_SHADER_GLES2_ALLOWLIST)
                    continue()
                endif()
                # Compute shaders cannot exist on GLES2.
                if(_type STREQUAL "compute")
                    continue()
                endif()
            endif()

            # Compute shaders need compute-capable profile versions: GLSL 4.30
            # (ARB_compute_shader) / ESSL 3.10.  The vertex/fragment baselines
            # (120 / 300_es) predate compute; shaderc still EMITS a blob for
            # them (`#version 120` + SSBO/barrier/local_size), which then
            # silently fails glCompileShader at RUNTIME — bgfx's handle-level
            # create still "succeeds", so every dispatch becomes a no-op and
            # each GPU-driven feature (foliage cull, Hi-Z, GPU scene) reads
            # back zeros on OpenGL while D3D/Vulkan work.  (Root-caused via
            # RenderDoc + the compiled blob's `#version 120` header.)
            if(_type STREQUAL "compute")
                if(_suffix STREQUAL "glsl")
                    set(_profile "430")
                elseif(_suffix STREQUAL "essl")
                    set(_profile "310_es")
                endif()
            endif()

            set(_out "${ARG_OUTPUT_DIR}/${_name}_${_suffix}.bin")
            # Bone-palette ceiling for the skinned vertex shaders' u_model[]
            # array. MUST stay equal to JCE_MAX_BONES in
            # engine/include/jce/renderer/jce_skinned_mesh.h. 128 mat4 = 512
            # vec4 fits every targeted vertex uniform budget; the skinned
            # shaders clamp the joint index to BGFX_CONFIG_MAX_BONES-1.
            set(_shader_defines
                --define "BGFX_CONFIG_MAX_BONES=128")

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
                        ${_shader_defines}
                DEPENDS "${_src}" "${_varying}" ${_lint_stamp}
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
