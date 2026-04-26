cmake_minimum_required(VERSION 3.20)

# ── MUST be set before project() ──
set(CMAKE_SUPPRESS_REGENERATION TRUE)

project(JCE VERSION 0.7.6 LANGUAGES C)

# CXX is needed for C++ dependencies (bgfx, assimp, imgui) and the editor.
enable_language(CXX)

# Export compile database for tooling (cppcheck/clangd/etc.).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Version header ────────────────────────────────────────────────────
# Resolve git short commit hash (falls back to "unknown" in CI/archives).
find_package(Git QUIET)
set(JCE_GIT_COMMIT "unknown")
if(Git_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		OUTPUT_VARIABLE JCE_GIT_COMMIT
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
		RESULT_VARIABLE _git_rc)
	if(NOT _git_rc EQUAL 0)
		set(JCE_GIT_COMMIT "unknown")
	endif()
	unset(_git_rc)
endif()

# Build timestamp with timezone — captured at configure time and embedded into
# version header so the editor About dialog can display a TZ-aware build time
# instead of the per-TU __DATE__/__TIME__ macros (which lack timezone info).
string(TIMESTAMP JCE_BUILD_TIMESTAMP_UTC "%Y-%m-%d %H:%M:%S UTC" UTC)

configure_file(
	"${CMAKE_SOURCE_DIR}/engine/include/jce/jce_version.h.in"
	"${CMAKE_BINARY_DIR}/include/jce/jce_version.h"
	@ONLY)

# Conan generators in this workspace are produced as Release for iOS/Xcode.
if(CMAKE_GENERATOR STREQUAL "Xcode")
	set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)
	set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
	set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)
endif()

option(JCE_BUILD_JNI "Build JCE as a JNI shared library" OFF)

# Build variant: "release" (default) or "dist" (no logging + SHA-256 hash).
set(JCE_BUILD_VARIANT "release" CACHE STRING "Build variant (release|dist)")
set_property(CACHE JCE_BUILD_VARIANT PROPERTY STRINGS release dist)

# ------------------------------------------------------------------ #
# Platform detection                                                   #
# ------------------------------------------------------------------ #

if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
	set(JCE_PLATFORM_WEB TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
	set(JCE_PLATFORM_ANDROID TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	set(JCE_PLATFORM_IOS TRUE)
elseif(APPLE)
	set(JCE_PLATFORM_MACOS TRUE)
elseif(WIN32)
	set(JCE_PLATFORM_WINDOWS TRUE)
elseif(UNIX)
	set(JCE_PLATFORM_LINUX TRUE)
endif()

if(JCE_PLATFORM_MACOS)
	set(JCE_MACOS_DEFAULT_DEPLOYMENT_TARGET "11.0")
	if((DEFINED CMAKE_OSX_ARCHITECTURES AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
	   OR CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
		set(JCE_MACOS_DEFAULT_DEPLOYMENT_TARGET "10.15")
	endif()
	if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
		set(CMAKE_OSX_DEPLOYMENT_TARGET "${JCE_MACOS_DEFAULT_DEPLOYMENT_TARGET}"
			CACHE STRING "Minimum macOS deployment target" FORCE)
	endif()
endif()

# ------------------------------------------------------------------ #
# C / C++ standards                                                    #
# ------------------------------------------------------------------ #

set(CMAKE_C_STANDARD          99)
set(CMAKE_C_STANDARD_REQUIRED YES)
if(EMSCRIPTEN)
	set(CMAKE_C_EXTENSIONS ON)
else()
	set(CMAKE_C_EXTENSIONS OFF)
endif()

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ------------------------------------------------------------------ #
# Common compiler warnings helper                                     #
# ------------------------------------------------------------------ #
#
# jce_target_warnings(<target> [STRICT])
#   Applies project-wide warning flags.
#   STRICT mode adds extra conversion / shadow checks (used by engine).
#
function(jce_target_warnings TARGET)
	cmake_parse_arguments(ARG "STRICT" "" "" ${ARGN})
	if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
		target_compile_options(${TARGET} PRIVATE -Wall -Wextra)
		if(ARG_STRICT)
			target_compile_options(${TARGET} PRIVATE
				-Wpedantic -Wconversion -Wshadow
				-Wdouble-promotion -Wno-unused-parameter)
		endif()
	elseif(MSVC)
		target_compile_options(${TARGET} PRIVATE /W4 /utf-8 /Zc:preprocessor)
		if(ARG_STRICT)
			target_compile_options(${TARGET} PRIVATE
				/wd4100    # unreferenced formal parameter
				/wd4127)   # conditional expression is constant
		endif()
		target_compile_definitions(${TARGET} PRIVATE
			_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	endif()
endfunction()

# ------------------------------------------------------------------ #
# Third-party dependencies (provided by Conan via CMakeDeps)         #
# ------------------------------------------------------------------ #

find_package(SDL3_image   REQUIRED)
find_package(SDL3         REQUIRED)
find_package(bgfx         REQUIRED)
find_package(flecs        REQUIRED)
find_package(miniaudio    REQUIRED)
find_package(imgui        REQUIRED)
find_package(Freetype     REQUIRED)
find_package(harfbuzz     REQUIRED)
find_package(cgltf        REQUIRED)
find_package(zstd         REQUIRED)
find_package(xxHash       REQUIRED)
find_package(mimalloc     REQUIRED)
find_package(cglm         REQUIRED)
find_package(enkits       REQUIRED)
find_package(PhysFS       REQUIRED)
find_package(cJSON        REQUIRED)
find_package(box2d        REQUIRED)
find_package(Bullet       REQUIRED)
find_package(enet         REQUIRED)
find_package(protobuf     REQUIRED)

# ── Patent-free codecs (preferred path) ──────────────────────────────
# Audio:
#   - Opus    : RFC 6716 codec, plugged into miniaudio as a custom backend
#   - Ogg     : container only (RFC 3533)
#   - Vorbis  : handled internally by miniaudio (stb_vorbis), no Conan dep
#   - FLAC    : handled internally by miniaudio (dr_flac), no Conan dep
#   - WAV/MP3 : handled internally by miniaudio (dr_wav / dr_mp3)
# Video:
#   - dav1d   : AV1 decoder
#   - libvpx  : VP8/VP9 decoder
find_package(Opus         REQUIRED)
find_package(Ogg          REQUIRED)
find_package(dav1d        REQUIRED)
find_package(libvpx       REQUIRED)
find_package(webm         REQUIRED)

# fdk-aac and openh264 are now vendored at engine/src/<layer>/third_party/
# (added as subdirectories by engine/CMakeLists.txt). Reasons:
#   * libfdk_aac: upstream Conan recipe blocks Android cross-builds.
#   * openh264:   upstream meson doesn't recognise emscripten.
# Single source of truth that works across all platforms.

# Ensure protoc comes from the Conan package, not a system/conda protoc.
# Conan sets protobuf_PACKAGE_FOLDER_RELEASE after find_package(protobuf).
# Override Protobuf_PROTOC_EXECUTABLE so protobuf_generate() uses the
# version-matching protoc (e.g. 6.33.x, not Anaconda 4.25.x).
# CROSS-COMPILE NOTE: When building for Android/WASM from a Windows host,
# Conan compiles the entire protobuf package (including protoc) for the target
# architecture, producing an ELF binary that cannot run on Windows. In that
# case we find a Windows-native protoc.exe from the Conan2 package cache.
foreach(_pb_cfg RELEASE RELWITHDEBINFO DEBUG)
    if(DEFINED "protobuf_PACKAGE_FOLDER_${_pb_cfg}")
        set(_pb_bin_dir "${protobuf_PACKAGE_FOLDER_${_pb_cfg}}/bin")
        # Probe .exe first (native build), then bare binary (cross-compile package)
        set(_found_protoc "")
        foreach(_protoc_name "protoc.exe" "protoc")
            if(EXISTS "${_pb_bin_dir}/${_protoc_name}")
                set(_found_protoc "${_pb_bin_dir}/${_protoc_name}")
                break()
            endif()
        endforeach()
        # On Windows host, verify the found binary actually runs on this host.
        # Cross-compile Conan packages ship executables the host cannot run:
        #   - Linux ELF protoc (Android cross-compile)
        #   - ARM64 PE protoc (Windows ARM64 cross-compile, can't run on x64 host)
        #   - Only .js protoc files (Emscripten/WASM) with no native executable.
        # Strategy: probe the found binary with execute_process(--version).
        # If it fails (wrong arch, ELF, missing, .js), fall back to glob search.
        if(CMAKE_HOST_WIN32)
            set(_need_native_search FALSE)
            if(_found_protoc)
                execute_process(
                    COMMAND "${_found_protoc}" --version
                    RESULT_VARIABLE _run_rc
                    OUTPUT_QUIET ERROR_QUIET
                    TIMEOUT 10
                )
                if(NOT _run_rc EQUAL 0)
                    message(STATUS "JCE: Conan protoc cannot run on this host (cross-compile pkg). Searching Conan2 cache for native protoc.exe")
                    set(_need_native_search TRUE)
                    set(_found_protoc "")
                endif()
                unset(_run_rc)
            else()
                # No executable protoc found in bin dir (e.g. WASM: only .js files)
                message(STATUS "JCE: No native protoc in Conan package bin dir. Searching Conan2 cache for native protoc.exe")
                set(_need_native_search TRUE)
            endif()
            if(_need_native_search)
                file(GLOB _win_protoc_list
                    "$ENV{USERPROFILE}/.conan2/p/proto*/p/bin/protoc.exe")
                list(SORT _win_protoc_list ORDER DESCENDING)
                foreach(_candidate IN LISTS _win_protoc_list)
                    if(EXISTS "${_candidate}")
                        execute_process(
                            COMMAND "${_candidate}" --version
                            RESULT_VARIABLE _run_rc
                            OUTPUT_QUIET ERROR_QUIET
                            TIMEOUT 10
                        )
                        if(_run_rc EQUAL 0)
                            set(_found_protoc "${_candidate}")
                            break()
                        endif()
                    endif()
                endforeach()
                unset(_win_protoc_list)
                unset(_candidate)
                unset(_run_rc)
            endif()
            unset(_need_native_search)
        endif()
        if(_found_protoc)
            set(Protobuf_PROTOC_EXECUTABLE "${_found_protoc}"
                CACHE FILEPATH "The protoc compiler" FORCE)
            if(TARGET protobuf::protoc)
                set_property(TARGET protobuf::protoc
                    PROPERTY IMPORTED_LOCATION "${_found_protoc}")
            endif()
            message(STATUS "JCE: Using Conan protoc: ${_found_protoc}")
        endif()
        break()
    endif()
endforeach()
unset(_found_protoc)
unset(_pb_bin_dir)
unset(_pb_cfg)
unset(_protoc_name)

find_package(behaviortree_cpp REQUIRED)
find_package(RmlUi        REQUIRED)
find_package(ozz-animation REQUIRED)

# Tracy profiling switch.
option(JCE_ENABLE_PROFILING "Enable Tracy profiling instrumentation" ON)

# Dist builds must never ship with Tracy.
set(JCE_TRACY_ENABLED ${JCE_ENABLE_PROFILING})
if(JCE_BUILD_VARIANT STREQUAL "dist")
	if(JCE_ENABLE_PROFILING)
		message(WARNING "JCE_BUILD_VARIANT=dist forces Tracy OFF.")
	endif()
	set(JCE_TRACY_ENABLED OFF)
endif()

if(JCE_TRACY_ENABLED)
	find_package(Tracy REQUIRED)
endif()

# Dist builds must never ship patent-encumbered codecs (AAC/H.264/H.265).
# Mirror the Tracy pattern with an intermediate variable: the user-facing
# JCE_ENABLE_PATENTED_CODECS option (declared in engine/CMakeLists.txt,
# default ON) records preference and is never mutated. JCE_PATENTED_CODECS_ENABLED
# is the *effective* switch the build reads — derived from the option, then
# forced OFF for dist. Switching variants does not lose the user's preference.
set(JCE_PATENTED_CODECS_ENABLED ON)  # default; flipped below if needed
if(DEFINED JCE_ENABLE_PATENTED_CODECS AND NOT JCE_ENABLE_PATENTED_CODECS)
	set(JCE_PATENTED_CODECS_ENABLED OFF)
endif()
if(JCE_BUILD_VARIANT STREQUAL "dist")
	if(JCE_PATENTED_CODECS_ENABLED)
		message(STATUS "JCE_BUILD_VARIANT=dist forces JCE_ENABLE_PATENTED_CODECS OFF (royalty-free redistribution).")
	endif()
	set(JCE_PATENTED_CODECS_ENABLED OFF)
endif()

if(JCE_BUILD_JNI)
	find_package(JNI REQUIRED)
endif()

# ------------------------------------------------------------------ #
# Convenience variables for sub-modules                               #
# ------------------------------------------------------------------ #

set(JCE_ROOT_DIR       "${CMAKE_CURRENT_SOURCE_DIR}")
set(JCE_ENGINE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/src")
set(JCE_TOOLS_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/tools")
set(JCE_IOS_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/macos/ios")

# ------------------------------------------------------------------ #
# Shader compilation (bgfx shaderc)                                   #
# ------------------------------------------------------------------ #

include("${JCE_TOOLS_DIR}/compile_shaders.cmake")

set(JCE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/resources/assets/shaders")

# ── Per-platform shader profile selection ───────────────────────────
# Compile (and pak) only the bgfx renderer back-ends that the target
# platform can actually load.  Saves compile time and keeps shipped
# .pak slim (e.g. no Metal binaries on Windows builds).
#
# Override at configure time with -DJCE_SHADER_PROFILES="dx11;spv".
if(NOT JCE_SHADER_PROFILES)
	if(JCE_PLATFORM_WINDOWS)
		set(JCE_SHADER_PROFILES dx11 spv glsl)
	elseif(JCE_PLATFORM_MACOS OR JCE_PLATFORM_IOS)
		set(JCE_SHADER_PROFILES mtl spv)
	elseif(JCE_PLATFORM_ANDROID)
		set(JCE_SHADER_PROFILES essl spv)
	elseif(JCE_PLATFORM_WEB)
		set(JCE_SHADER_PROFILES essl)
	elseif(JCE_PLATFORM_LINUX)
		set(JCE_SHADER_PROFILES spv glsl)
	else()
		set(JCE_SHADER_PROFILES dx11 spv glsl essl mtl)
	endif()
endif()
message(STATUS "JCE shader profiles: ${JCE_SHADER_PROFILES}")

# Build a list of `--exclude-suffix _<sfx>.bin` flags for jce_pak so
# that any *stale* shader binaries left in the shared output dir (from
# a previous build for another platform) are not packed into the .pak.
set(_all_shader_suffixes dx11 spv glsl essl mtl)
set(JCE_PAK_SHADER_EXCLUDE_FLAGS "")
foreach(_sfx IN LISTS _all_shader_suffixes)
	if(NOT _sfx IN_LIST JCE_SHADER_PROFILES)
		list(APPEND JCE_PAK_SHADER_EXCLUDE_FLAGS
			--exclude-suffix "_${_sfx}.bin")
	endif()
endforeach()

jce_compile_shaders(
	TARGET        CompileShaders
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/standard"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersPBR
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/pbr"
	VARYING_DEF   "varying_pbr.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_PBR_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersImGui
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/imgui"
	VARYING_DEF   "varying_imgui.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_IMGUI_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersPostFX
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/postfx"
	VARYING_DEF   "varying_postfx.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_POSTFX_SHADER_FILES)

# ------------------------------------------------------------------ #
# Host packer tool: jce_pak                                           #
# ------------------------------------------------------------------ #

if(CMAKE_CROSSCOMPILING)
	if(NOT JCE_PAK_EXECUTABLE)
		message(FATAL_ERROR
			"Cross-compiling: set -DJCE_PAK_EXECUTABLE=<path> "
			"to a host-built jce_pak binary.")
	endif()
	add_executable(jce_pak IMPORTED)
	set_target_properties(jce_pak PROPERTIES
		IMPORTED_LOCATION "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_DEBUG "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_RELEASE "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_RELWITHDEBINFO "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_MINSIZEREL "${JCE_PAK_EXECUTABLE}")
else()
	add_executable(jce_pak "${JCE_TOOLS_DIR}/jce_pak.c")
	set_target_properties(jce_pak PROPERTIES
		EXCLUDE_FROM_ALL      ON
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
		C_STANDARD            99
		C_STANDARD_REQUIRED   YES
		C_EXTENSIONS          OFF)
	target_include_directories(jce_pak PRIVATE "${JCE_ENGINE_SRC_DIR}")
	target_link_libraries(jce_pak PRIVATE
		zstd::libzstd_static
		xxHash::xxhash)
	jce_target_warnings(jce_pak)
endif()

# ------------------------------------------------------------------ #
# Host cooker tool: jce_cook                                          #
# ------------------------------------------------------------------ #

if(NOT CMAKE_CROSSCOMPILING)
	add_executable(jce_cook
		"${JCE_TOOLS_DIR}/jce_cook.c"
		"${JCE_TOOLS_DIR}/jce_tex_compress.c"
		"${JCE_TOOLS_DIR}/miniaudio_impl_cook.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_asset_cooker.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_filesystem_host.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_memory.c")
	set_target_properties(jce_cook PROPERTIES
		EXCLUDE_FROM_ALL      ON
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
		C_STANDARD            99
		C_STANDARD_REQUIRED   YES
		C_EXTENSIONS          OFF)
	target_include_directories(jce_cook PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/engine/include"
		"${JCE_ENGINE_SRC_DIR}")
	target_link_libraries(jce_cook PRIVATE
		SDL3_image::SDL3_image
		sdl::sdl
		miniaudio::miniaudio
		mimalloc-static
		zstd::libzstd_static
		xxHash::xxhash)
	jce_target_warnings(jce_cook)
endif()

# ------------------------------------------------------------------ #
# Asset cooking (optional — pre-decode textures & audio at build time)#
# ------------------------------------------------------------------ #

option(JCE_ENABLE_ASSET_COOKING "Pre-cook assets at build time (raw→.jceasset)" ON)

# ------------------------------------------------------------------ #
# Asset embedding (per-module PAK files)                              #
# ------------------------------------------------------------------ #

set(ENGINE_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/engine/resources/assets")
set(GAME_RESOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/caged_kingdom/resources/assets")
set(EDITOR_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/editor/resources/assets")
set(GAME_ICON_FILE      "${CMAKE_CURRENT_SOURCE_DIR}/caged_kingdom/resources/CK_icon.png")
set(GAME_ICON_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_raw_game_icon")
set(ENGINE_UI_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/engine/ui")
set(GAME_UI_DIR         "${CMAKE_CURRENT_SOURCE_DIR}/caged_kingdom/ui")

# THIRD_PARTY_LICENSES.md is sourced from the repo root and copied
# directly into each PAK at build time. We do NOT mirror it into the
# source tree (no engine/resources/THIRD_PARTY_LICENSES.md).
set(TPL_SRC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES.md")

file(GLOB_RECURSE ENGINE_ASSET_FILES CONFIGURE_DEPENDS "${ENGINE_RESOURCE_DIR}/*")
file(GLOB_RECURSE GAME_ASSET_FILES   CONFIGURE_DEPENDS "${GAME_RESOURCE_DIR}/*")
file(GLOB_RECURSE EDITOR_ASSET_FILES CONFIGURE_DEPENDS "${EDITOR_RESOURCE_DIR}/*")
file(GLOB_RECURSE ENGINE_UI_FILES    CONFIGURE_DEPENDS "${ENGINE_UI_DIR}/*")
file(GLOB_RECURSE GAME_UI_FILES      CONFIGURE_DEPENDS "${GAME_UI_DIR}/*")

# Safety: source-only raw_assets must never participate in cook/pack pipelines.
list(FILTER ENGINE_ASSET_FILES EXCLUDE REGEX "(/|\\\\)raw_assets(/|\\\\)")
list(FILTER GAME_ASSET_FILES   EXCLUDE REGEX "(/|\\\\)raw_assets(/|\\\\)")
list(FILTER EDITOR_ASSET_FILES EXCLUDE REGEX "(/|\\\\)raw_assets(/|\\\\)")

# ── Cook step: convert raw assets → .jceasset containers ──
# When enabled, jce_cook pre-decodes textures (PNG→RGBA8) and audio
# (WAV/OGG→PCM s16).  PAK then packs the cooked containers.
# Level 0 = no per-chunk ZSTD (PAK already compresses the whole entry).
if(JCE_ENABLE_ASSET_COOKING AND NOT CMAKE_CROSSCOMPILING)
	set(GAME_COOKED_DIR    "${CMAKE_CURRENT_BINARY_DIR}/_cooked/game")
	set(EDITOR_COOKED_DIR  "${CMAKE_CURRENT_BINARY_DIR}/_cooked/editor")
	set(COOK_GAME_STAMP    "${CMAKE_CURRENT_BINARY_DIR}/_cook_game.stamp")
	set(COOK_EDITOR_STAMP  "${CMAKE_CURRENT_BINARY_DIR}/_cook_editor.stamp")

	add_custom_command(
		OUTPUT     "${COOK_GAME_STAMP}"
		COMMAND    "${CMAKE_COMMAND}" -E rm -f "${COOK_GAME_STAMP}"
		COMMAND    "${CMAKE_COMMAND}" -E rm -rf "${GAME_COOKED_DIR}"
		COMMAND    "${CMAKE_COMMAND}" -E make_directory "${GAME_COOKED_DIR}"
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${ENGINE_RESOURCE_DIR}"  "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${ENGINE_UI_DIR}"        "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${GAME_RESOURCE_DIR}"    "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${GAME_UI_DIR}"          "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${GAME_ICON_FILE}" "${GAME_COOKED_DIR}/CK_icon.png"
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${TPL_SRC_FILE}" "${GAME_COOKED_DIR}/THIRD_PARTY_LICENSES.md"
		COMMAND    "${CMAKE_COMMAND}" -E touch "${COOK_GAME_STAMP}"
		DEPENDS    jce_cook ${ENGINE_ASSET_FILES} ${GAME_ASSET_FILES}
		           ${ENGINE_UI_FILES} ${GAME_UI_FILES}
		           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES}
		           ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES}
		           "${GAME_ICON_FILE}" "${TPL_SRC_FILE}"
		COMMENT    "Cooking game assets (textures/audio → .jceasset)"
		VERBATIM)

	set(EDITOR_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/editor/resources/JCE_icon.png")

	add_custom_command(
		OUTPUT     "${COOK_EDITOR_STAMP}"
		COMMAND    "${CMAKE_COMMAND}" -E rm -f "${COOK_EDITOR_STAMP}"
		COMMAND    "${CMAKE_COMMAND}" -E rm -rf "${EDITOR_COOKED_DIR}"
		COMMAND    "${CMAKE_COMMAND}" -E make_directory "${EDITOR_COOKED_DIR}"
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${ENGINE_RESOURCE_DIR}"  "${EDITOR_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${ENGINE_UI_DIR}"        "${EDITOR_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${EDITOR_RESOURCE_DIR}"  "${EDITOR_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${EDITOR_ICON_FILE}" "${EDITOR_COOKED_DIR}/JCE_icon.png"
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${TPL_SRC_FILE}" "${EDITOR_COOKED_DIR}/THIRD_PARTY_LICENSES.md"
		COMMAND    "${CMAKE_COMMAND}" -E touch "${COOK_EDITOR_STAMP}"
		DEPENDS    jce_cook ${ENGINE_ASSET_FILES} ${EDITOR_ASSET_FILES}
		           ${ENGINE_UI_FILES}
		           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES}
		           ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES}
		           "${EDITOR_ICON_FILE}" "${TPL_SRC_FILE}"
		COMMENT    "Cooking editor assets (textures/audio → .jceasset)"
		VERBATIM)

	add_custom_target(CookGameAssets   DEPENDS "${COOK_GAME_STAMP}")
	add_custom_target(CookEditorAssets DEPENDS "${COOK_EDITOR_STAMP}")
	add_dependencies(CookGameAssets   CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX)
	add_dependencies(CookEditorAssets CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX)

	# PAK reads from a single cooked dir (engine + app merged).
	set(_GAME_PAK_RES_FLAGS    --resource-dir "${GAME_COOKED_DIR}")
	set(_EDITOR_PAK_RES_FLAGS  --resource-dir "${EDITOR_COOKED_DIR}")
	set(_COOK_GAME_DEP    CookGameAssets)
	set(_COOK_EDITOR_DEP  CookEditorAssets)
else()
	# No cooking — PAK reads raw resource dirs directly.
	set(_GAME_PAK_RES_FLAGS
		--resource-dir "${ENGINE_RESOURCE_DIR}"
		--resource-dir "${ENGINE_UI_DIR}"
		--resource-dir "${GAME_RESOURCE_DIR}"
		--resource-dir "${GAME_UI_DIR}"
		--resource-dir "${GAME_ICON_STAGE_DIR}")
	set(_EDITOR_PAK_RES_FLAGS
		--resource-dir "${ENGINE_RESOURCE_DIR}"
		--resource-dir "${ENGINE_UI_DIR}"
		--resource-dir "${EDITOR_RESOURCE_DIR}")
	set(_GAME_PAK_PREP_COMMANDS
		COMMAND    "${CMAKE_COMMAND}" -E rm -rf "${GAME_ICON_STAGE_DIR}"
		COMMAND    "${CMAKE_COMMAND}" -E make_directory "${GAME_ICON_STAGE_DIR}"
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${GAME_ICON_FILE}" "${GAME_ICON_STAGE_DIR}/CK_icon.png")
	set(_COOK_GAME_DEP   "")
	set(_COOK_EDITOR_DEP "")
endif()

# ── Object format detection (shared by both pak targets) ──
if(JCE_PLATFORM_WEB)
	# Emscripten cannot compile 1.5 GB C arrays; use --preload-file instead.
	set(_OBJ_FORMAT "none")
elseif(MSVC)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
		set(_OBJ_ARCH "x64")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
		set(_OBJ_ARCH "arm64")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(X86|i[3-6]86)$")
		set(_OBJ_ARCH "x86")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM|armv7)")
		set(_OBJ_ARCH "arm")
	else()
		message(FATAL_ERROR "Unsupported SYSTEM_PROCESSOR for COFF: ${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	set(_OBJ_FORMAT "coff")
else()
	set(_OBJ_FORMAT "none")
endif()

# ── Game assets PAK (engine base + caged_kingdom) ──

# Locate Python early so the HTML report generator can be wired into the
# pak custom commands.  Optional: if missing, the report step is skipped.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
	set(_PAK_REPORT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_pak_report.py")
endif()

# zstd compression level: high effort for Release/Dist (ship size matters),
# default 3 for Debug/RelWithDebInfo (iteration speed matters).  Override
# with -DJCE_PAK_LEVEL=N at configure time.
if(NOT DEFINED JCE_PAK_LEVEL)
	if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "Dist")
		set(JCE_PAK_LEVEL 19)
	else()
		set(JCE_PAK_LEVEL 3)
	endif()
endif()
set(_PAK_LEVEL_FLAGS --level ${JCE_PAK_LEVEL})
message(STATUS "JCE PAK zstd level: ${JCE_PAK_LEVEL}")

set(GAME_PAK_FILE       "${CMAKE_CURRENT_BINARY_DIR}/game_assets.pak")
set(GAME_PAK_OBJ_FILE   "${CMAKE_CURRENT_BINARY_DIR}/game_assets_pak.obj")
set(GAME_PAK_C_ARRAY    "${CMAKE_CURRENT_BINARY_DIR}/game_assets_pak_data.c")
set(GAME_HEADER_FILE    "${CMAKE_CURRENT_BINARY_DIR}/game_embedded_assets.h")
set(GAME_MANIFEST_FILE  "${CMAKE_CURRENT_BINARY_DIR}/_game_assets_manifest.cmake")
set(GAME_STAMP_FILE     "${CMAKE_CURRENT_BINARY_DIR}/_game_assets_pak.stamp")

if(_OBJ_FORMAT STREQUAL "c-array")
	set(_GAME_OBJ_FLAGS --obj-format c-array --c-file "${GAME_PAK_C_ARRAY}")
elseif(_OBJ_FORMAT STREQUAL "coff")
	set(_GAME_OBJ_FLAGS --obj-file "${GAME_PAK_OBJ_FILE}" --obj-format coff --obj-arch "${_OBJ_ARCH}")
else()
	set(_GAME_OBJ_FLAGS --obj-format none)
endif()

if(Python3_FOUND)
	set(_GAME_REPORT_CMD COMMAND "${Python3_EXECUTABLE}" "${_PAK_REPORT_SCRIPT}"
		"${GAME_MANIFEST_FILE}"
		"${CMAKE_CURRENT_BINARY_DIR}/reports/game_assets_report.html"
		--title "Game Assets Report")
else()
	set(_GAME_REPORT_CMD "")
endif()

add_custom_command(
	OUTPUT     "${GAME_STAMP_FILE}"
	BYPRODUCTS "${GAME_PAK_FILE}" "${GAME_PAK_OBJ_FILE}" "${GAME_PAK_C_ARRAY}" "${GAME_HEADER_FILE}" "${GAME_MANIFEST_FILE}"
	${_GAME_PAK_PREP_COMMANDS}
	COMMAND    $<TARGET_FILE:jce_pak>
	           ${_GAME_PAK_RES_FLAGS}
	           --exclude-segment raw_assets
	           ${JCE_PAK_SHADER_EXCLUDE_FLAGS}
	           ${_PAK_LEVEL_FLAGS}
	           --pak-file      "${GAME_PAK_FILE}"
	           --header-file   "${GAME_HEADER_FILE}"
	           --manifest-file "${GAME_MANIFEST_FILE}"
	           ${_GAME_OBJ_FLAGS}
	${_GAME_REPORT_CMD}
	COMMAND    "${CMAKE_COMMAND}" -E touch "${GAME_STAMP_FILE}"
	DEPENDS    jce_pak ${ENGINE_ASSET_FILES} ${GAME_ASSET_FILES}
	           ${ENGINE_UI_FILES} ${GAME_UI_FILES}
	           "${GAME_ICON_FILE}"
	           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES} ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES}
	COMMENT    "Packing game assets -> ${GAME_PAK_FILE}"
	VERBATIM)

set_source_files_properties(
	"${GAME_PAK_FILE}" "${GAME_PAK_OBJ_FILE}" "${GAME_HEADER_FILE}"
	"${GAME_MANIFEST_FILE}" "${GAME_STAMP_FILE}" "${GAME_PAK_C_ARRAY}"
	PROPERTIES GENERATED TRUE)

add_custom_target(PackGameAssets DEPENDS "${GAME_STAMP_FILE}")
add_dependencies(PackGameAssets CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX)
if(_COOK_GAME_DEP)
	add_dependencies(PackGameAssets ${_COOK_GAME_DEP})
endif()

# ── Editor assets PAK (engine base + editor) ──
set(EDITOR_PAK_FILE       "${CMAKE_CURRENT_BINARY_DIR}/editor_assets.pak")
set(EDITOR_PAK_OBJ_FILE   "${CMAKE_CURRENT_BINARY_DIR}/editor_assets_pak.obj")
set(EDITOR_PAK_C_ARRAY    "${CMAKE_CURRENT_BINARY_DIR}/editor_assets_pak_data.c")
set(EDITOR_HEADER_FILE    "${CMAKE_CURRENT_BINARY_DIR}/editor_embedded_assets.h")
set(EDITOR_MANIFEST_FILE  "${CMAKE_CURRENT_BINARY_DIR}/_editor_assets_manifest.cmake")
set(EDITOR_STAMP_FILE     "${CMAKE_CURRENT_BINARY_DIR}/_editor_assets_pak.stamp")

if(_OBJ_FORMAT STREQUAL "c-array")
	set(_EDITOR_OBJ_FLAGS --obj-format c-array --c-file "${EDITOR_PAK_C_ARRAY}")
elseif(_OBJ_FORMAT STREQUAL "coff")
	set(_EDITOR_OBJ_FLAGS --obj-file "${EDITOR_PAK_OBJ_FILE}" --obj-format coff --obj-arch "${_OBJ_ARCH}")
else()
	set(_EDITOR_OBJ_FLAGS --obj-format none)
endif()

if(Python3_FOUND)
	set(_EDITOR_REPORT_CMD COMMAND "${Python3_EXECUTABLE}" "${_PAK_REPORT_SCRIPT}"
		"${EDITOR_MANIFEST_FILE}"
		"${CMAKE_CURRENT_BINARY_DIR}/reports/editor_assets_report.html"
		--title "Editor Assets Report")
else()
	set(_EDITOR_REPORT_CMD "")
endif()

add_custom_command(
	OUTPUT     "${EDITOR_STAMP_FILE}"
	BYPRODUCTS "${EDITOR_PAK_FILE}" "${EDITOR_PAK_OBJ_FILE}" "${EDITOR_PAK_C_ARRAY}" "${EDITOR_HEADER_FILE}" "${EDITOR_MANIFEST_FILE}"
	COMMAND    $<TARGET_FILE:jce_pak>
	           ${_EDITOR_PAK_RES_FLAGS}
	           --exclude-segment raw_assets
	           ${JCE_PAK_SHADER_EXCLUDE_FLAGS}
	           ${_PAK_LEVEL_FLAGS}
	           --pak-file      "${EDITOR_PAK_FILE}"
	           --header-file   "${EDITOR_HEADER_FILE}"
	           --manifest-file "${EDITOR_MANIFEST_FILE}"
	           ${_EDITOR_OBJ_FLAGS}
	${_EDITOR_REPORT_CMD}
	COMMAND    "${CMAKE_COMMAND}" -E touch "${EDITOR_STAMP_FILE}"
	DEPENDS    jce_pak ${ENGINE_ASSET_FILES} ${EDITOR_ASSET_FILES}
	           ${ENGINE_UI_FILES}
	           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES} ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES}
	COMMENT    "Packing editor assets -> ${EDITOR_PAK_FILE}"
	VERBATIM)

set_source_files_properties(
	"${EDITOR_PAK_FILE}" "${EDITOR_PAK_OBJ_FILE}" "${EDITOR_HEADER_FILE}"
	"${EDITOR_MANIFEST_FILE}" "${EDITOR_STAMP_FILE}" "${EDITOR_PAK_C_ARRAY}"
	PROPERTIES GENERATED TRUE)

add_custom_target(PackEditorAssets DEPENDS "${EDITOR_STAMP_FILE}")
add_dependencies(PackEditorAssets CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX)
if(_COOK_EDITOR_DEP)
	add_dependencies(PackEditorAssets ${_COOK_EDITOR_DEP})
endif()

set(REPORT_SCRIPT "${JCE_TOOLS_DIR}/build_report.cmake")

# ------------------------------------------------------------------ #
# Sub-modules                                                         #
# ------------------------------------------------------------------ #

add_subdirectory(engine)
add_subdirectory(editor)
add_subdirectory(caged_kingdom)

# ── Dist mode: suppress all logging ──
if(JCE_BUILD_VARIANT STREQUAL "dist")
	target_compile_definitions(jce_core PUBLIC JCE_DIST=1)
endif()

# ------------------------------------------------------------------ #
# Public-header dependency guard                                      #
#                                                                     #
# Fails the build if any header under engine/include/jce/ pulls in a  #
# third-party header (SDL, bgfx, cglm, imgui, ...).  Guarantees the   #
# "users only call our API" invariant.                                #
# ------------------------------------------------------------------ #

option(JCE_ENABLE_PUBLIC_API_GUARD "Enable public-header leak guard" ON)
if(JCE_ENABLE_PUBLIC_API_GUARD)
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_FOUND)
		add_custom_target(check_public_api
			COMMAND ${Python3_EXECUTABLE}
				"${CMAKE_SOURCE_DIR}/tools/check_public_headers.py"
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			COMMENT "Verifying public headers have no third-party leaks"
			VERBATIM)
	endif()
endif()

# ------------------------------------------------------------------ #
# Code-style targets: clang-format                                    #
#                                                                     #
# `format`       — apply formatting in-place                          #
# `format-check` — fail if any file would be reformatted (CI gate)    #
# Excludes vendored third_party trees and Conan caches.               #
# ------------------------------------------------------------------ #

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format
	HINTS
		"C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
		"C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/bin")
if(CLANG_FORMAT_EXECUTABLE)
	file(GLOB_RECURSE _jce_fmt_files
		"${CMAKE_SOURCE_DIR}/engine/include/*.h"
		"${CMAKE_SOURCE_DIR}/engine/src/*.c"
		"${CMAKE_SOURCE_DIR}/engine/src/*.h"
		"${CMAKE_SOURCE_DIR}/editor/src/*.cpp"
		"${CMAKE_SOURCE_DIR}/editor/src/*.h"
		"${CMAKE_SOURCE_DIR}/caged_kingdom/src/*.c"
		"${CMAKE_SOURCE_DIR}/caged_kingdom/src/*.h")
	list(FILTER _jce_fmt_files EXCLUDE REGEX "/third_party/")
	list(FILTER _jce_fmt_files EXCLUDE REGEX "/internal/stb_image\\.h$")

	add_custom_target(format
		COMMAND ${CLANG_FORMAT_EXECUTABLE} -i --style=file ${_jce_fmt_files}
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "Formatting JCE C/C++ sources via clang-format"
		VERBATIM)

	add_custom_target(format-check
		COMMAND ${CLANG_FORMAT_EXECUTABLE}
			--dry-run --Werror --style=file ${_jce_fmt_files}
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "Checking JCE source formatting"
		VERBATIM)
endif()

# ------------------------------------------------------------------ #
# Static analysis: cppcheck                                           #
# ------------------------------------------------------------------ #

option(JCE_ENABLE_CPPCHECK "Enable cppcheck target" ON)
if(JCE_ENABLE_CPPCHECK)
	find_program(CPPCHECK_EXECUTABLE NAMES cppcheck HINTS "D:/CMake/Cppcheck")
	if(CPPCHECK_EXECUTABLE)
		include(ProcessorCount)
		ProcessorCount(_JCE_CPPCHECK_JOBS)
		if(NOT _JCE_CPPCHECK_JOBS OR _JCE_CPPCHECK_JOBS LESS 1)
			set(_JCE_CPPCHECK_JOBS 4)
		endif()

		set(JCE_CPPCHECK_REPORT_DIR "${CMAKE_BINARY_DIR}/reports")
		set(JCE_CPPCHECK_TEXT_REPORT "${JCE_CPPCHECK_REPORT_DIR}/cppcheck-report.txt")
		set(JCE_CPPCHECK_XML_REPORT  "${JCE_CPPCHECK_REPORT_DIR}/cppcheck-report.xml")

		# Common flags shared by all three cppcheck invocations.
		set(_CPPCHECK_COMMON_FLAGS
			--project=${CMAKE_BINARY_DIR}/compile_commands.json
			--relative-paths=${CMAKE_SOURCE_DIR}
			--enable=warning,style,performance,portability
			--inconclusive --inline-suppr
			--suppress=missingIncludeSystem
			--suppress=*:*/.conan2/*
			--suppress=*:*/third_party/*
			-i${CMAKE_SOURCE_DIR}/engine/src/middleware/audio/third_party
			-i${CMAKE_SOURCE_DIR}/engine/src/middleware/video/third_party
			--file-filter=${CMAKE_SOURCE_DIR}/engine/*
			--file-filter=${CMAKE_SOURCE_DIR}/editor/*
			--file-filter=${CMAKE_SOURCE_DIR}/caged_kingdom/*
			-j${_JCE_CPPCHECK_JOBS}
			--std=c99)

		add_custom_target(cppcheck ALL
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${JCE_CPPCHECK_REPORT_DIR}"
			COMMAND "${CPPCHECK_EXECUTABLE}" ${_CPPCHECK_COMMON_FLAGS}
				--template={file}:{line}:{severity}:{id}:{message}
				--output-file=${JCE_CPPCHECK_TEXT_REPORT}
			COMMAND "${CPPCHECK_EXECUTABLE}" ${_CPPCHECK_COMMON_FLAGS}
				--xml --xml-version=2
				--output-file=${JCE_CPPCHECK_XML_REPORT}
			COMMAND "${CPPCHECK_EXECUTABLE}" ${_CPPCHECK_COMMON_FLAGS}
				--quiet
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			COMMENT "Running cppcheck"
			VERBATIM)
	endif()
endif()

# ------------------------------------------------------------------ #
# Build size report — dependency locations                            #
# ------------------------------------------------------------------ #

set(_dep_locs_content "# Auto-generated -- DO NOT EDIT\n")
string(APPEND _dep_locs_content "set(DEP_LOCATIONS\n")

set(_dep_targets
	"SDL3|SDL3::SDL3"
	"miniaudio|miniaudio::miniaudio"
	"imgui|imgui::imgui"
	"assimp|assimp::assimp")

foreach(_entry ${_dep_targets})
	string(REPLACE "|" ";" _parts "${_entry}")
	list(GET _parts 0 _disp_name)
	list(GET _parts 1 _tgt_name)
	if(NOT TARGET ${_tgt_name})
		continue()
	endif()
	get_target_property(_tgt_type ${_tgt_name} TYPE)
	if(_tgt_type STREQUAL "INTERFACE_LIBRARY")
		continue()
	endif()
	string(APPEND _dep_locs_content
		"  \"${_disp_name}=$<TARGET_FILE:${_tgt_name}>\"\n")
endforeach()

string(APPEND _dep_locs_content ")\n")

set(DEP_LOCS_FILE "${CMAKE_CURRENT_BINARY_DIR}/_dep_locs_$<CONFIG>.cmake")
file(GENERATE OUTPUT "${DEP_LOCS_FILE}" CONTENT "${_dep_locs_content}")
