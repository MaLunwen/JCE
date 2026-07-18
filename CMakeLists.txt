cmake_minimum_required(VERSION 3.20)

# ── MUST be set before project() ──
set(CMAKE_SUPPRESS_REGENERATION TRUE)

project(JCE VERSION 0.10.14 LANGUAGES C)

# CXX is needed for C++ dependencies (bgfx, assimp, imgui) and the editor.
enable_language(CXX)

# AppleClang's enable_language(ASM) succeeds at detection but fails to
# install a CMAKE_ASM_COMPILE_OBJECT rule on cross-compile toolchains.
# Same issue affects Linux ARM64 cross-compile toolchains where the ASM
# rule isn't auto-generated. Force a sensible rule globally before any
# subdir calls enable_language(ASM).
if(APPLE OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"))
	if(NOT CMAKE_ASM_COMPILER)
		set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}" CACHE FILEPATH "" FORCE)
	endif()
	set(CMAKE_ASM_COMPILE_OBJECT
		"<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>"
		CACHE STRING "" FORCE)
	set(CMAKE_ASM_SOURCE_FILE_EXTENSIONS s;S;asm CACHE STRING "" FORCE)
endif()

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

# Build timestamp with timezone, embedded into the version header so the editor
# About dialog shows a TZ-aware build time instead of the per-TU
# __DATE__/__TIME__ macros (which lack timezone info).
#
# For REPRODUCIBLE builds this must NOT be the wall clock: the same commit has
# to embed the same string on every machine/rebuild.  Derive it from the HEAD
# commit's committer date (UTC).  Fall back to the wall clock only outside a git
# checkout (release builds always come from git).  See also /Brepro below.
set(JCE_BUILD_TIMESTAMP_UTC "")
if(Git_FOUND)
	execute_process(
		COMMAND ${CMAKE_COMMAND} -E env TZ=UTC
			${GIT_EXECUTABLE} show -s --format=%cd
			"--date=format-local:%Y-%m-%d %H:%M:%S UTC" HEAD
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		OUTPUT_VARIABLE JCE_BUILD_TIMESTAMP_UTC
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
		RESULT_VARIABLE _git_ts_rc)
	if(NOT _git_ts_rc EQUAL 0)
		set(JCE_BUILD_TIMESTAMP_UTC "")
	endif()
	unset(_git_ts_rc)
endif()
if(NOT JCE_BUILD_TIMESTAMP_UTC)
	string(TIMESTAMP JCE_BUILD_TIMESTAMP_UTC "%Y-%m-%d %H:%M:%S UTC" UTC)
endif()

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
if(JCE_BUILD_JNI)
	# Shared library requires all static libraries compiled with -fPIC.
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# SDK packaging — when ON, engine/CMakeLists.txt registers install rules
# that produce a redistributable SDK consumable via `find_package(JCE)`.
option(JCE_ENABLE_SDK_INSTALL
	"Generate install rules to package JCE as a redistributable SDK." OFF)

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
# Compatibility baseline (jce_compat.h)                              #
# ------------------------------------------------------------------ #
#
# Enforce minimum OS / SIMD targets so that the engine matches the
# guarantees declared in engine/include/jce/jce_compat.h:
#   Windows 7+, iOS 12+, macOS 10.13+, Android API 21+,
#   SSE2 baseline on x86, NEON on arm64.

if(JCE_PLATFORM_WINDOWS)
	add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
endif()

if(NOT MSVC)
	# x86_64 always has SSE2 architecturally; -msse2 is a no-op on
	# x86_64 but is still passed to make 32-bit builds explicit.
	include(CheckCCompilerFlag)
	check_c_compiler_flag("-msse2" JCE_HAS_MSSE2_FLAG)
	if(JCE_HAS_MSSE2_FLAG)
		add_compile_options(-msse2)
	endif()
endif()

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
find_package(assimp       REQUIRED)
find_package(meshoptimizer REQUIRED)
find_package(zstd         REQUIRED)
find_package(xxHash       REQUIRED)
find_package(mimalloc     REQUIRED)
find_package(enkits       REQUIRED)
find_package(PhysFS       REQUIRED)
find_package(cJSON        REQUIRED)
if(JCE_ENABLE_AI_DISPATCH)
	# ai_dispatch T1 HTTPS transport.  QUIET on purpose: build trees whose
	# conan state predates the libcurl requirement still configure -- the
	# module falls back to its plaintext socket transport when absent.
	find_package(CURL QUIET)
endif()
find_package(box2d        REQUIRED)
find_package(Bullet       REQUIRED)
find_package(v-hacd       REQUIRED)
find_package(recastnavigation REQUIRED)
find_package(enet         REQUIRED)
find_package(protobuf     REQUIRED)
find_package(lua          REQUIRED)

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
        # Verify the found binary can actually run on THIS host.
        # Cross-compile packages may ship arm64/WASM executables that the
        # build host cannot execute (Exec format error on Linux, wrong PE arch
        # on Windows, or .js-only for WASM).  Probe with --version and fall
        # back to a glob search over the full Conan2 cache when it fails.
        set(_need_native_search FALSE)
        if(_found_protoc)
            execute_process(
                COMMAND "${_found_protoc}" --version
                RESULT_VARIABLE _run_rc
                OUTPUT_QUIET ERROR_QUIET
                TIMEOUT 10
            )
            if(NOT _run_rc EQUAL 0)
                message(STATUS "JCE: Conan protoc cannot run on this host (cross-compile pkg). Searching Conan2 cache for native protoc.")
                set(_need_native_search TRUE)
                set(_found_protoc "")
            endif()
            unset(_run_rc)
        else()
            message(STATUS "JCE: No native protoc in Conan package bin dir. Searching Conan2 cache for native protoc.")
            set(_need_native_search TRUE)
        endif()
        if(_need_native_search)
            if(CMAKE_HOST_WIN32)
                file(GLOB _protoc_list "$ENV{USERPROFILE}/.conan2/p/b/proto*/p/bin/protoc.exe")
            else()
                file(GLOB _protoc_list "$ENV{HOME}/.conan2/p/b/proto*/p/bin/protoc")
            endif()
            list(SORT _protoc_list ORDER DESCENDING)
            foreach(_candidate IN LISTS _protoc_list)
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
            unset(_protoc_list)
            unset(_candidate)
            unset(_run_rc)
        endif()
        unset(_need_native_search)
        if(_found_protoc)
            set(Protobuf_PROTOC_EXECUTABLE "${_found_protoc}"
                CACHE FILEPATH "The protoc compiler" FORCE)
            if(TARGET protobuf::protoc)
                set_target_properties(protobuf::protoc PROPERTIES
                    IMPORTED_LOCATION               "${_found_protoc}"
                    IMPORTED_LOCATION_RELEASE       "${_found_protoc}"
                    IMPORTED_LOCATION_DEBUG         "${_found_protoc}"
                    IMPORTED_LOCATION_RELWITHDEBINFO "${_found_protoc}"
                    IMPORTED_LOCATION_MINSIZEREL    "${_found_protoc}")
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

# RenderDoc in-application GPU-capture trigger (debug aid). Mirror the Tracy
# pattern: the user-facing option records preference; the effective switch is
# forced OFF for dist so shipped games carry zero RenderDoc code. No find_package
# — the in-app API is resolved at runtime from the injected module.
option(JCE_ENABLE_RENDERDOC "Enable the RenderDoc in-app GPU-capture trigger (debug)" ON)
set(JCE_RENDERDOC_ENABLED ${JCE_ENABLE_RENDERDOC})
if(JCE_BUILD_VARIANT STREQUAL "dist")
	if(JCE_ENABLE_RENDERDOC)
		message(STATUS "JCE_BUILD_VARIANT=dist forces RenderDoc capture OFF.")
	endif()
	set(JCE_RENDERDOC_ENABLED OFF)
endif()
# RenderDoc is a native-desktop-only debug aid (injected module + native GPU
# capture API); its third-party header does not compile under Emscripten, and
# the Web has no RenderDoc.  Force it OFF for wasm regardless of the option.
if(EMSCRIPTEN)
	if(JCE_ENABLE_RENDERDOC)
		message(STATUS "EMSCRIPTEN/wasm forces RenderDoc capture OFF.")
	endif()
	set(JCE_RENDERDOC_ENABLED OFF)
endif()

# Dist builds must never ship patent-encumbered codecs (AAC/H.264/H.265).
# Mirror the Tracy pattern with an intermediate variable: the user-facing
# JCE_ENABLE_PATENTED_CODECS option (declared in engine/CMakeLists.txt,
# default ON) records preference and is never mutated. JCE_PATENTED_CODECS_ENABLED
# is the *effective* switch the build reads — derived from the option, then
# forced OFF for dist/Web. Switching variants does not lose the user's preference.
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
if(JCE_PLATFORM_WEB)
	if(JCE_PATENTED_CODECS_ENABLED)
		message(STATUS "JCE_PLATFORM_WEB forces JCE_ENABLE_PATENTED_CODECS OFF (WASM uses royalty-free codecs only).")
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

# Generated shader binaries are build artifacts.  Keep them per build tree so
# Release/Dist, architectures, parallel CI jobs, and sandbox users never race
# over one source-tree output or accidentally package another build's stale
# backend profile.
set(JCE_ENGINE_ASSET_BUILD_DIR
	"${CMAKE_CURRENT_BINARY_DIR}/generated/engine_assets")
set(JCE_SHADER_OUTPUT_DIR "${JCE_ENGINE_ASSET_BUILD_DIR}/shaders")

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
		# Apple deprecated desktop OpenGL and bgfx no longer reports
		# BGFX_RENDERER_TYPE_OPENGL on Apple platforms, so the `glsl`
		# profile would only bloat the .pak with unreachable binaries.
		set(JCE_SHADER_PROFILES mtl spv)
	elseif(JCE_PLATFORM_ANDROID)
		# NOTE: no essl1 — the runtime shader loader (jce_shaders.c
		# shader_suffix) maps BGFX_RENDERER_TYPE_OPENGLES to "essl"
		# unconditionally, so essl1 binaries were built + PAKed but
		# never loadable (dead .pak weight).  If a real GLES2 tier is
		# ever wanted, add a caps-based suffix pick there first.
		set(JCE_SHADER_PROFILES essl spv)
	elseif(JCE_PLATFORM_WEB)
		# NOTE: no essl1 — see the Android note above (never loaded).
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
set(_all_shader_suffixes dx11 spv glsl essl essl1 mtl)
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

# Client/showcase "look" shaders — data-driven custom-post-pass content
# (moebius/toon/painterly/lowpoly/pixel/psx/watercolor/comic), kept separate
# from the engine's built-in postfx. They load by flat name (vs_postfx +
# fs_<name>), so appending them onto the PostFX file list makes them ride
# along in every pak aggregation without touching each one.
jce_compile_shaders(
	TARGET        CompileShadersLooks
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/looks"
	VARYING_DEF   "varying.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_LOOKS_SHADER_FILES)
list(APPEND COMPILED_POSTFX_SHADER_FILES ${COMPILED_LOOKS_SHADER_FILES})

jce_compile_shaders(
	TARGET        CompileShadersParticles
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/particles"
	VARYING_DEF   "varying_particles.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_PARTICLES_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersDecals
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/decals"
	VARYING_DEF   "varying_decals.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_DECALS_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersWeather
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/weather"
	VARYING_DEF   "varying_weather.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_WEATHER_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersSSAO
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/ssao"
	VARYING_DEF   "varying_ssao.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_SSAO_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersSSR
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/ssr"
	VARYING_DEF   "varying_ssr.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_SSR_SHADER_FILES)

jce_compile_shaders(
	TARGET        CompileShadersVolFog
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/engine/shaders/volfog"
	VARYING_DEF   "varying_volfog.def.sc"
	OUTPUT_DIR    "${JCE_SHADER_OUTPUT_DIR}"
	OUT_FILES_VAR COMPILED_VOLFOG_SHADER_FILES)

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
	add_executable(jce_pak
		"${JCE_TOOLS_DIR}/jce_pak.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_cook.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_writer.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_reader.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_path.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_dict.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_archive_crypto.c"
		"${JCE_ENGINE_SRC_DIR}/os/platform/jce_mmap.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_alloc.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_memory.c")
	# When the SDK install is enabled, jce_pak must be built (it ships in
	# <sdk>/bin/) so end-user projects can pack their own assets without
	# having a JCE source tree.  Otherwise keep it out of the default
	# build to save engine-developer build time.
	if(NOT JCE_ENABLE_SDK_INSTALL)
		set_target_properties(jce_pak PROPERTIES EXCLUDE_FROM_ALL ON)
	endif()
	set_target_properties(jce_pak PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
		C_STANDARD            99
		C_STANDARD_REQUIRED   YES
		C_EXTENSIONS          OFF)
	target_include_directories(jce_pak PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/engine/include"
		"${JCE_ENGINE_SRC_DIR}")
	target_link_libraries(jce_pak PRIVATE
		mimalloc-static
		zstd::libzstd_static
		xxHash::xxhash)
	jce_target_warnings(jce_pak)
endif()

# ------------------------------------------------------------------ #
# Host bin→obj tool: jce_bin2obj                                      #
# ------------------------------------------------------------------ #
# Wraps an arbitrary binary file (e.g. a .jbundle) into a COFF .obj
# or GAS .S so it can be linked into an exe.  Used by the SDK helper
# jce_target_embed_bundle() to deliver true single-file builds.

if(CMAKE_CROSSCOMPILING)
	if(JCE_BIN2OBJ_EXECUTABLE)
		add_executable(jce_bin2obj IMPORTED)
		set_target_properties(jce_bin2obj PROPERTIES
			IMPORTED_LOCATION "${JCE_BIN2OBJ_EXECUTABLE}")
	endif()
else()
	add_executable(jce_bin2obj "${JCE_TOOLS_DIR}/jce_bin2obj.c")
	if(NOT JCE_ENABLE_SDK_INSTALL)
		set_target_properties(jce_bin2obj PROPERTIES EXCLUDE_FROM_ALL ON)
	endif()
	set_target_properties(jce_bin2obj PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
		C_STANDARD            99
		C_STANDARD_REQUIRED   YES
		C_EXTENSIONS          OFF)
	jce_target_warnings(jce_bin2obj)
endif()

# ------------------------------------------------------------------ #
# Host cooker tool: jce_cook                                          #
# ------------------------------------------------------------------ #

if(NOT CMAKE_CROSSCOMPILING AND
   EXISTS "${JCE_TOOLS_DIR}/jce_cook.c" AND
   EXISTS "${JCE_TOOLS_DIR}/jce_tex_compress.c" AND
   EXISTS "${JCE_TOOLS_DIR}/miniaudio_impl_cook.c")
	add_executable(jce_cook
		"${JCE_TOOLS_DIR}/jce_cook.c"
		"${JCE_TOOLS_DIR}/jce_cook_catalog.c"
		"${JCE_TOOLS_DIR}/jce_tex_compress.c"
		"${JCE_TOOLS_DIR}/jce_tex_encode.cpp"
		"${JCE_TOOLS_DIR}/miniaudio_impl_cook.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_asset_cooker.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_filesystem_host.c"
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_memory.c"
		# Offline collider cook (--collider): model import (assimp) +
		# collider cook (VHACD) + the JCOL blob codec.  The asset TU is
		# built with JCE_COLLIDER_ASSET_NO_PHYSICS so it brings only the
		# serialize path (no Bullet/physics link).
		"${JCE_ENGINE_SRC_DIR}/os/core/jce_alloc.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_model_importer.cpp"
		"${JCE_ENGINE_SRC_DIR}/middleware/physics/jce_collider_cook.cpp"
		"${JCE_ENGINE_SRC_DIR}/middleware/physics/jce_collider_asset.c"
		# Offline model→GLB conversion (--convert-model): the same assimp+meshopt
		# coercion the bundle packer uses, so every deployment path can normalise
		# .obj/.fbx/.dae to a single runtime mesh format (binary glTF).
		"${JCE_ENGINE_SRC_DIR}/resource/jce_bundle_mesh_convert.cpp"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_mesh_lod_cook.c"
		# stb-backed decode for the gray16-PNG bypass in jce_cook_texture
		# (SDL3_image's libpng path heap-overruns on 16-bit grayscale PNGs).
		"${JCE_ENGINE_SRC_DIR}/resource/jce_image_decode.c"
		"${JCE_ENGINE_SRC_DIR}/renderer/jce_stb_image_impl.c")
	# When the SDK install is enabled, jce_cook ships in <sdk>/bin/ so
	# end-user projects can cook raw assets (textures/audio) without the
	# editor or a JCE source tree.  Otherwise keep it out of the default
	# build to save engine-developer build time — it still builds on
	# demand for the in-tree cook commands via $<TARGET_FILE:jce_cook>.
	if(NOT JCE_ENABLE_SDK_INSTALL)
		set_target_properties(jce_cook PROPERTIES EXCLUDE_FROM_ALL ON)
	endif()
	set_target_properties(jce_cook PROPERTIES
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
		xxHash::xxhash
			bgfx::bimg_encode
			bgfx::bimg
			bgfx::bx
		# Offline collider cook dependencies.
		assimp::assimp
		v-hacd::v-hacd
		# Offline model→GLB conversion (--convert-model) needs the meshopt
		# dedup/simplify core that jce_bundle_mesh_convert.cpp drives.
		meshoptimizer::meshoptimizer)
	# JCE_DIST: compile out the engine LOG_* macros pulled in by the
	#   collider/model-import TUs so jce_cook needs no async log subsystem.
	# JCE_COLLIDER_ASSET_NO_PHYSICS: omit jce_collider_instantiate() so the
	#   blob codec links without the Bullet/physics chain.
	# JCE_MODEL_IMPORTER_COOK_ONLY: compile out the skinned/animated FBX import
	#   path in jce_model_importer.cpp (it references jce_renderer + jce_animation
	#   symbols this lean cook tool does not link); only the static part-
	#   extraction path the --collider cook needs is kept.
	target_compile_definitions(jce_cook PRIVATE
		JCE_DIST
		JCE_COLLIDER_ASSET_NO_PHYSICS
		JCE_MODEL_IMPORTER_COOK_ONLY)
	jce_target_warnings(jce_cook)
endif()


# ------------------------------------------------------------------ #
# Ship host tools with the SDK                                        #
# ------------------------------------------------------------------ #
# When JCE_ENABLE_SDK_INSTALL is ON, install the host cook/pack/wrap
# tools into <sdk>/bin/ so a no-editor CMake/CLI consumer can run the
# full raw-asset -> cooked -> PAK -> embed pipeline via jce_add_pak()
# (cmake/JCESDKHelpers.cmake).  JCEConfig.cmake.in probes these and
# exposes JCE_PAK_EXECUTABLE / JCE_COOK_EXECUTABLE / JCE_BIN2OBJ_EXECUTABLE.
if(JCE_ENABLE_SDK_INSTALL AND NOT CMAKE_CROSSCOMPILING)
	include(GNUInstallDirs)
	set(_jce_sdk_tools "")
	foreach(_tool jce_pak jce_cook jce_bin2obj)
		if(TARGET ${_tool})
			list(APPEND _jce_sdk_tools ${_tool})
		endif()
	endforeach()
	if(_jce_sdk_tools)
		install(TARGETS ${_jce_sdk_tools}
			RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
	endif()
endif()


# ------------------------------------------------------------------ #
# Asset cooking (optional — pre-decode textures & audio at build time)#
# ------------------------------------------------------------------ #

option(JCE_ENABLE_ASSET_COOKING "Pre-cook assets at build time (raw->.jceasset)" ON)

# Caged Kingdom is the first-party SAMPLE GAME.  Default OFF: apps build
# against the packaged SDK as real consumers (scripts/jce.py app) — the same
# path external users take, and the only way the SDK contract is dogfooded.
# Flip ON only for in-tree dev iteration on the sample itself.
option(JCE_BUILD_INTREE_SAMPLE "Build the in-tree Caged Kingdom sample (dev only)" OFF)
# Meadow Valley is the stylized-world dogfood project.  Default OFF; enabled
# independently of caged_kingdom so CI can gate each project separately.
option(JCE_BUILD_INTREE_MEADOW  "Build the in-tree Meadow Valley project (dev only)" OFF)
# Science Lab is a fully engine-isolated SDK consumer: it builds ONLY via
# standalone find_package(JCE) (see science_lab/CMakeLists.txt). It has no
# in-tree build path — no option, no add_subdirectory, no shared game PAK.
if(JCE_ENABLE_ASSET_COOKING AND NOT CMAKE_CROSSCOMPILING AND NOT TARGET jce_cook)
	message(STATUS
		"JCE asset cooking requested, but host cooker sources are not "
		"available; PAK targets will read raw resource directories.")
endif()

# ------------------------------------------------------------------ #
# Asset embedding (per-module PAK files)                              #
# ------------------------------------------------------------------ #

# Engine-owned runtime assets are generated into this build-local root.  At
# present these are the compiled shaders; applications/editor resources retain
# their normal source roots below.
set(ENGINE_RESOURCE_DIR "${JCE_ENGINE_ASSET_BUILD_DIR}")
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
if(JCE_ENABLE_ASSET_COOKING AND NOT CMAKE_CROSSCOMPILING AND TARGET jce_cook)
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
		           --preserve-names --level 0 --max-texture-size 2048 --platform windows
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${ENGINE_UI_DIR}"        "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048 --platform windows
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${GAME_RESOURCE_DIR}"    "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048 --platform windows
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${GAME_UI_DIR}"          "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048 --platform windows
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${GAME_ICON_FILE}" "${GAME_COOKED_DIR}/CK_icon.png"
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${TPL_SRC_FILE}" "${GAME_COOKED_DIR}/THIRD_PARTY_LICENSES.md"
		COMMAND    "${CMAKE_COMMAND}" -E touch "${COOK_GAME_STAMP}"
		DEPENDS    jce_cook ${ENGINE_ASSET_FILES} ${GAME_ASSET_FILES}
		           ${ENGINE_UI_FILES} ${GAME_UI_FILES}
		           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES}
		           ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES} ${COMPILED_PARTICLES_SHADER_FILES} ${COMPILED_DECALS_SHADER_FILES} ${COMPILED_WEATHER_SHADER_FILES} ${COMPILED_SSAO_SHADER_FILES} ${COMPILED_SSR_SHADER_FILES} ${COMPILED_VOLFOG_SHADER_FILES}
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
		           ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES} ${COMPILED_PARTICLES_SHADER_FILES} ${COMPILED_DECALS_SHADER_FILES} ${COMPILED_WEATHER_SHADER_FILES} ${COMPILED_SSAO_SHADER_FILES} ${COMPILED_SSR_SHADER_FILES} ${COMPILED_VOLFOG_SHADER_FILES}
		           "${EDITOR_ICON_FILE}" "${TPL_SRC_FILE}"
		COMMENT    "Cooking editor assets (textures/audio → .jceasset)"
		VERBATIM)

	add_custom_target(CookGameAssets   DEPENDS "${COOK_GAME_STAMP}")
	add_custom_target(CookEditorAssets DEPENDS "${COOK_EDITOR_STAMP}")
	add_dependencies(CookGameAssets   CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX CompileShadersParticles CompileShadersDecals CompileShadersWeather CompileShadersSSAO CompileShadersSSR CompileShadersVolFog)
	add_dependencies(CookEditorAssets CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX CompileShadersParticles CompileShadersDecals CompileShadersWeather CompileShadersSSAO CompileShadersSSR CompileShadersVolFog)

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
	# Derive the embedded-PAK COFF machine type from the compiler's TARGET
	# architecture, NOT CMAKE_SYSTEM_PROCESSOR.  For an x86 build on an x64 host
	# (vcvarsall amd64_x86) Conan does not flag a cross-build, so it never sets
	# CMAKE_SYSTEM_PROCESSOR and it falls back to the host value "AMD64" -- which
	# would wrongly stamp the PAK object as x64 and break the editor link with
	# LNK1112 (module machine type 'x64' conflicts with target 'x86').
	# CMAKE_<LANG>_COMPILER_ARCHITECTURE_ID always reflects what cl.exe/link.exe
	# actually emit (X86 / x64 / ARM64 / ARM) and matches the linker's /machine.
	set(_MSVC_TARGET_ARCH "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
	if(NOT _MSVC_TARGET_ARCH)
		set(_MSVC_TARGET_ARCH "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
	endif()
	string(TOLOWER "${_MSVC_TARGET_ARCH}" _MSVC_TARGET_ARCH)
	if(_MSVC_TARGET_ARCH STREQUAL "x64")
		set(_OBJ_ARCH "x64")
	elseif(_MSVC_TARGET_ARCH STREQUAL "arm64")
		set(_OBJ_ARCH "arm64")
	elseif(_MSVC_TARGET_ARCH STREQUAL "x86")
		set(_OBJ_ARCH "x86")
	elseif(_MSVC_TARGET_ARCH STREQUAL "arm")
		set(_OBJ_ARCH "arm")
	else()
		message(FATAL_ERROR
			"Unsupported MSVC target architecture for COFF: "
			"'${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}' "
			"(CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}')")
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
set(GAME_BOM_FILE       "${CMAKE_CURRENT_BINARY_DIR}/reports/game_assets_bom.json")
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
		"${GAME_BOM_FILE}"
		"${CMAKE_CURRENT_BINARY_DIR}/reports/game_assets_report.html"
		--title "Game Assets Report")
else()
	set(_GAME_REPORT_CMD "")
endif()

add_custom_command(
	OUTPUT     "${GAME_STAMP_FILE}"
	BYPRODUCTS "${GAME_PAK_FILE}" "${GAME_PAK_OBJ_FILE}" "${GAME_PAK_C_ARRAY}" "${GAME_HEADER_FILE}" "${GAME_MANIFEST_FILE}" "${GAME_BOM_FILE}"
	${_GAME_PAK_PREP_COMMANDS}
	COMMAND    "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/reports"
	COMMAND    $<TARGET_FILE:jce_pak>
	           ${_GAME_PAK_RES_FLAGS}
	           --exclude-segment raw_assets
	           ${JCE_PAK_SHADER_EXCLUDE_FLAGS}
	           ${_PAK_LEVEL_FLAGS}
	           --pak-file      "${GAME_PAK_FILE}"
	           --header-file   "${GAME_HEADER_FILE}"
	           --manifest-file "${GAME_MANIFEST_FILE}"
	           --json          "${GAME_BOM_FILE}"
	           ${_GAME_OBJ_FLAGS}
	${_GAME_REPORT_CMD}
	COMMAND    "${CMAKE_COMMAND}" -E touch "${GAME_STAMP_FILE}"
	DEPENDS    jce_pak ${ENGINE_ASSET_FILES} ${GAME_ASSET_FILES}
	           ${ENGINE_UI_FILES} ${GAME_UI_FILES}
	           "${GAME_ICON_FILE}"
	           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES} ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES} ${COMPILED_PARTICLES_SHADER_FILES} ${COMPILED_DECALS_SHADER_FILES} ${COMPILED_WEATHER_SHADER_FILES} ${COMPILED_SSAO_SHADER_FILES} ${COMPILED_SSR_SHADER_FILES} ${COMPILED_VOLFOG_SHADER_FILES}
	COMMENT    "Packing game assets -> ${GAME_PAK_FILE}"
	VERBATIM)

set_source_files_properties(
	"${GAME_PAK_FILE}" "${GAME_PAK_OBJ_FILE}" "${GAME_HEADER_FILE}"
	"${GAME_MANIFEST_FILE}" "${GAME_STAMP_FILE}" "${GAME_PAK_C_ARRAY}"
	"${GAME_BOM_FILE}"
	PROPERTIES GENERATED TRUE)

add_custom_target(PackGameAssets DEPENDS "${GAME_STAMP_FILE}")
add_dependencies(PackGameAssets CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX CompileShadersParticles CompileShadersDecals CompileShadersWeather CompileShadersSSAO CompileShadersSSR CompileShadersVolFog)
if(_COOK_GAME_DEP)
	add_dependencies(PackGameAssets ${_COOK_GAME_DEP})
endif()

# ── Editor assets PAK (engine base + editor) ──
set(EDITOR_PAK_FILE       "${CMAKE_CURRENT_BINARY_DIR}/editor_assets.pak")
set(EDITOR_PAK_OBJ_FILE   "${CMAKE_CURRENT_BINARY_DIR}/editor_assets_pak.obj")
set(EDITOR_PAK_C_ARRAY    "${CMAKE_CURRENT_BINARY_DIR}/editor_assets_pak_data.c")
set(EDITOR_HEADER_FILE    "${CMAKE_CURRENT_BINARY_DIR}/editor_embedded_assets.h")
set(EDITOR_MANIFEST_FILE  "${CMAKE_CURRENT_BINARY_DIR}/_editor_assets_manifest.cmake")
set(EDITOR_BOM_FILE       "${CMAKE_CURRENT_BINARY_DIR}/reports/editor_assets_bom.json")
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
		"${EDITOR_BOM_FILE}"
		"${CMAKE_CURRENT_BINARY_DIR}/reports/editor_assets_report.html"
		--title "Editor Assets Report")
else()
	set(_EDITOR_REPORT_CMD "")
endif()

add_custom_command(
	OUTPUT     "${EDITOR_STAMP_FILE}"
	BYPRODUCTS "${EDITOR_PAK_FILE}" "${EDITOR_PAK_OBJ_FILE}" "${EDITOR_PAK_C_ARRAY}" "${EDITOR_HEADER_FILE}" "${EDITOR_MANIFEST_FILE}" "${EDITOR_BOM_FILE}"
	COMMAND    "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/reports"
	COMMAND    $<TARGET_FILE:jce_pak>
	           ${_EDITOR_PAK_RES_FLAGS}
	           --exclude-segment raw_assets
	           ${JCE_PAK_SHADER_EXCLUDE_FLAGS}
	           ${_PAK_LEVEL_FLAGS}
	           --pak-file      "${EDITOR_PAK_FILE}"
	           --header-file   "${EDITOR_HEADER_FILE}"
	           --manifest-file "${EDITOR_MANIFEST_FILE}"
	           --json          "${EDITOR_BOM_FILE}"
	           ${_EDITOR_OBJ_FLAGS}
	${_EDITOR_REPORT_CMD}
	COMMAND    "${CMAKE_COMMAND}" -E touch "${EDITOR_STAMP_FILE}"
	DEPENDS    jce_pak ${ENGINE_ASSET_FILES} ${EDITOR_ASSET_FILES}
	           ${ENGINE_UI_FILES}
	           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES} ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES} ${COMPILED_PARTICLES_SHADER_FILES} ${COMPILED_DECALS_SHADER_FILES} ${COMPILED_WEATHER_SHADER_FILES} ${COMPILED_SSAO_SHADER_FILES} ${COMPILED_SSR_SHADER_FILES} ${COMPILED_VOLFOG_SHADER_FILES}
	COMMENT    "Packing editor assets -> ${EDITOR_PAK_FILE}"
	VERBATIM)

set_source_files_properties(
	"${EDITOR_PAK_FILE}" "${EDITOR_PAK_OBJ_FILE}" "${EDITOR_HEADER_FILE}"
	"${EDITOR_MANIFEST_FILE}" "${EDITOR_STAMP_FILE}" "${EDITOR_PAK_C_ARRAY}"
	"${EDITOR_BOM_FILE}"
	PROPERTIES GENERATED TRUE)

add_custom_target(PackEditorAssets DEPENDS "${EDITOR_STAMP_FILE}")
add_dependencies(PackEditorAssets CompileShaders CompileShadersPBR CompileShadersImGui CompileShadersPostFX CompileShadersParticles CompileShadersDecals CompileShadersWeather CompileShadersSSAO CompileShadersSSR CompileShadersVolFog)
if(_COOK_EDITOR_DEP)
	add_dependencies(PackEditorAssets ${_COOK_EDITOR_DEP})
endif()

set(REPORT_SCRIPT "${JCE_TOOLS_DIR}/build_report.cmake")

# ================================================================== #
# Engine-shader embedded PAK (linked into the engine static lib).    #
#                                                                    #
# A freshly-scaffolded SDK consumer project ships with a 1-byte stub #
# PAK and therefore has no shader bins.  To avoid rendering nothing  #
# we bake the stock engine shaders into `jce_renderer` itself.       #
#                                                                    #
# Output:                                                            #
#   ${CMAKE_BINARY_DIR}/engine_shaders.pak                           #
#   ${CMAKE_BINARY_DIR}/jce_engine_shaders_pak.c   (C array)         #
#   ${CMAKE_BINARY_DIR}/jce_engine_shaders_pak.h   (extern decls)    #
#                                                                    #
# When `JCE_EMBED_ENGINE_SHADERS=OFF`, the stub TU                   #
# `engine/src/renderer/jce_engine_shaders_pak_stub.c` provides the   #
# symbols at zero cost.                                              #
# ================================================================== #
option(JCE_EMBED_ENGINE_SHADERS
	"Bake the stock engine shader PAK into the jce_renderer static library so SDK consumer projects can render without shipping a separate engine_shaders.pak"
	ON)

if(JCE_EMBED_ENGINE_SHADERS)
	set(JCE_ENGSH_PAK_FILE      "${CMAKE_BINARY_DIR}/engine_shaders.pak")
	set(JCE_ENGSH_C_ARRAY       "${CMAKE_BINARY_DIR}/jce_engine_shaders_pak.c")
	set(JCE_ENGSH_HEADER_FILE   "${CMAKE_BINARY_DIR}/jce_engine_shaders_pak.h")
	set(JCE_ENGSH_MANIFEST_FILE "${CMAKE_BINARY_DIR}/_engine_shaders_manifest.cmake")
	set(JCE_ENGSH_BOM_FILE      "${CMAKE_BINARY_DIR}/reports/engine_shaders_bom.json")
	set(JCE_ENGSH_STAMP_FILE    "${CMAKE_BINARY_DIR}/_engine_shaders_pak.stamp")

	if(Python3_FOUND)
		set(_ENGSH_REPORT_CMD COMMAND "${Python3_EXECUTABLE}" "${_PAK_REPORT_SCRIPT}"
			"${JCE_ENGSH_BOM_FILE}"
			"${CMAKE_BINARY_DIR}/reports/engine_shaders_report.html"
			--title "Engine Shaders Report")
	else()
		set(_ENGSH_REPORT_CMD "")
	endif()

	add_custom_command(
		OUTPUT     "${JCE_ENGSH_STAMP_FILE}"
		BYPRODUCTS "${JCE_ENGSH_PAK_FILE}" "${JCE_ENGSH_C_ARRAY}"
		           "${JCE_ENGSH_HEADER_FILE}" "${JCE_ENGSH_MANIFEST_FILE}"
		           "${JCE_ENGSH_BOM_FILE}"
		COMMAND    "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/reports"
		COMMAND    $<TARGET_FILE:jce_pak>
		           --resource-dir "${ENGINE_RESOURCE_DIR}"
		           --exclude-segment i18n
		           --exclude-segment raw_assets
		           ${JCE_PAK_SHADER_EXCLUDE_FLAGS}
		           ${_PAK_LEVEL_FLAGS}
		           --symbol-prefix jce_engine_shaders_pak
		           --pak-file      "${JCE_ENGSH_PAK_FILE}"
		           --header-file   "${JCE_ENGSH_HEADER_FILE}"
		           --manifest-file "${JCE_ENGSH_MANIFEST_FILE}"
		           --json          "${JCE_ENGSH_BOM_FILE}"
		           --obj-format    c-array
		           --c-file        "${JCE_ENGSH_C_ARRAY}"
		${_ENGSH_REPORT_CMD}
		COMMAND    "${CMAKE_COMMAND}" -E touch "${JCE_ENGSH_STAMP_FILE}"
		DEPENDS    jce_pak
		           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES}
		           ${COMPILED_IMGUI_SHADER_FILES} ${COMPILED_POSTFX_SHADER_FILES}
		           ${COMPILED_PARTICLES_SHADER_FILES} ${COMPILED_DECALS_SHADER_FILES}
		           ${COMPILED_WEATHER_SHADER_FILES} ${COMPILED_SSAO_SHADER_FILES}
		           ${COMPILED_SSR_SHADER_FILES} ${COMPILED_VOLFOG_SHADER_FILES}
		COMMENT    "Packing engine shaders -> ${JCE_ENGSH_C_ARRAY}"
		VERBATIM)

	set_source_files_properties(
		"${JCE_ENGSH_PAK_FILE}" "${JCE_ENGSH_C_ARRAY}"
		"${JCE_ENGSH_HEADER_FILE}" "${JCE_ENGSH_MANIFEST_FILE}"
		"${JCE_ENGSH_BOM_FILE}"
		"${JCE_ENGSH_STAMP_FILE}"
		PROPERTIES GENERATED TRUE)

	add_custom_target(PackEngineShaders DEPENDS "${JCE_ENGSH_STAMP_FILE}")
	add_dependencies(PackEngineShaders
		CompileShaders CompileShadersPBR CompileShadersImGui
		CompileShadersPostFX CompileShadersParticles CompileShadersDecals
		CompileShadersWeather CompileShadersSSAO CompileShadersSSR
		CompileShadersVolFog)
endif()

# ------------------------------------------------------------------ #
# 32-bit Windows: large-address-aware by default                      #
# ------------------------------------------------------------------ #
# Without /LARGEADDRESSAWARE a 32-bit (WOW64) process is capped at 2 GB of
# user address space.  Any JCE executable (editor, in-tree sample, host tools)
# exhausts that under D3D12 driver reservations + embedded PAK + a large ImGui
# CJK/Korean font atlas, so a big allocation returns NULL and the next memset
# faults (0xC0000005 in VCRUNTIME140!memset).  Granting the full ~4 GB on a
# 64-bit host fixes it.  64-bit targets are large-address-aware already, so the
# guard keeps this to genuine 32-bit builds.  SDK-consumed game apps get the
# same flag via JCE::JCE's INTERFACE_LINK_OPTIONS (see JCEConfig.cmake.in).
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4)
	string(APPEND CMAKE_EXE_LINKER_FLAGS " /LARGEADDRESSAWARE")
endif()

# ------------------------------------------------------------------ #
# Reproducible builds                                                 #
# ------------------------------------------------------------------ #
# Goal: the same commit + same toolchain + same arch produces a byte-identical
# binary (hence an identical jce_editor_sha256.txt) on every rebuild and every
# machine.  (Different ARCHITECTURES still differ — that is expected; the
# cross-arch identity is the version manifest, see tools/write_manifest.cmake.)
# This removes the two big non-deterministic inputs the wall-clock timestamp
# (handled above) does not cover:
#   * PE TimeDateStamp / debug directory — MSVC stamps link time by default.
#   * Absolute source paths leaking into the binary via __FILE__.
if(MSVC)
	# /Brepro: deterministic TimeDateStamp + debug dir (requires non-incremental
	# linking, which Release already uses).  Release config covers the release
	# AND dist variants (both build_type=Release); Debug is left incremental.
	add_link_options("$<$<CONFIG:Release>:/Brepro>" "$<$<CONFIG:Release>:/INCREMENTAL:NO>")
	# Release debug symbols: emit a .pdb so the in-process crash handler
	# (DbgHelp SymFromAddr/SymGetLineFromAddr64 in jce_crash_handler.c) resolves
	# STATIC functions + file:line instead of nearest-export noise.  /DEBUG turns
	# off /OPT:REF,ICF by default, so restore them to keep the release binary
	# optimized + small.  (Diagnostic aid for the streaming rapid-zoom crash;
	# the .pdb ships beside the .exe and is only read at crash time.)
	add_compile_options("$<$<CONFIG:Release>:/Zi>")
	add_link_options("$<$<CONFIG:Release>:/DEBUG>"
	                 "$<$<CONFIG:Release>:/OPT:REF>"
	                 "$<$<CONFIG:Release>:/OPT:ICF>")
	# Trim the absolute repo prefix from __FILE__ so embedded paths are stable
	# across machines/checkout locations.  Harmless no-op when the prefix is
	# absent.  (/d1trimfile is the practical MSVC equivalent of -ffile-prefix-map.)
	add_compile_options("/d1trimfile:${CMAKE_SOURCE_DIR}/")
else()
	# Clang/GCC: map the build path to a stable token for both __FILE__ and
	# debug info, so binaries do not embed the absolute source/build paths.
	add_compile_options(
		"-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
		"-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=.")

	# Size: GCC/Clang link the whole of every static lib by default, so the
	# binary is far larger than the MSVC .exe (MSVC Release defaults to
	# /OPT:REF + /OPT:ICF and puts debug in a separate .pdb).  Drop unused
	# code/data to align — using each linker's own dead-strip facility.
	if(APPLE)
		# ld64: native atom-level dead-strip (≈ /OPT:REF; --gc-sections is a
		# GNU-only flag and would error here).
		add_link_options("$<$<CONFIG:Release>:-Wl,-dead_strip>")
		if(JCE_BUILD_VARIANT STREQUAL "dist")
			add_compile_options("$<$<CONFIG:Release>:-O2>")  # /O2-equivalent; -O3 only bloats
		endif()
	else()
		# GNU ld: per-function/data sections + --gc-sections removes
		# unreferenced functions (≈ /OPT:REF).  (ICF via gold was tried and
		# reverted — it saved only ~0.4MB and added a linker dependency.)
		add_compile_options("$<$<CONFIG:Release>:-ffunction-sections>"
		                    "$<$<CONFIG:Release>:-fdata-sections>")
		add_link_options("$<$<CONFIG:Release>:-Wl,--gc-sections>")
		if(JCE_BUILD_VARIANT STREQUAL "dist")
			# dist = shipping: strip symbols (like the MSVC .pdb split) + -O2.
			add_link_options("$<$<CONFIG:Release>:-s>")
			add_compile_options("$<$<CONFIG:Release>:-O2>")
		endif()
	endif()
endif()

# ------------------------------------------------------------------ #
# Binary-size report: emit a linker map so tools/gen_binsize_report.py #
# can attribute bytes per library/module. Opt-in (maps add link time   #
# and clutter); OFF by default.                                        #
# ------------------------------------------------------------------ #
option(JCE_EMIT_LINK_MAP "Emit a linker .map under <build>/reports/ (for binsize report)" OFF)
function(jce_emit_link_map tgt)
	if(NOT JCE_EMIT_LINK_MAP)
		return()
	endif()
	# Emit the linker map under <build>/reports/ (alongside the other generated
	# reports) rather than next to the exe. The dir must exist before the link
	# step writes into it, so create it now at configure time.
	set(_map_dir "${CMAKE_BINARY_DIR}/reports")
	file(MAKE_DIRECTORY "${_map_dir}")
	if(MSVC)
		target_link_options(${tgt} PRIVATE "/MAP:${_map_dir}/${tgt}.map")
	else()
		target_link_options(${tgt} PRIVATE "LINKER:-Map=${_map_dir}/${tgt}.map")
	endif()
endfunction()

# ------------------------------------------------------------------ #
# Sub-modules                                                         #
# ------------------------------------------------------------------ #

add_subdirectory(engine)
add_subdirectory(editor)
# CK is downstream of the SDK: it builds as an SDK consumer (scripts/jce.py
# app), not as a first-party in-tree target.  Gated ON only for dev iteration.
if(JCE_BUILD_INTREE_SAMPLE)
	add_subdirectory(caged_kingdom)
endif()
# Meadow Valley is gated independently so CI can enable it without also
# enabling caged_kingdom (JCE_BUILD_INTREE_SAMPLE).
if(JCE_BUILD_INTREE_MEADOW)
	add_subdirectory(meadow_valley)
endif()
# science_lab is intentionally NOT add_subdirectory()'d: it is a standalone
# find_package(JCE) consumer built out-of-tree (see science_lab/CMakeLists.txt).

# After jce_renderer has been declared in engine/CMakeLists.txt, hook
# in the generated embedded-shader C array (and exclude the stub TU).
if(JCE_EMBED_ENGINE_SHADERS AND TARGET jce_renderer)
	target_sources(jce_renderer PRIVATE "${JCE_ENGSH_C_ARRAY}")
	target_compile_definitions(jce_renderer PRIVATE JCE_ENGINE_SHADERS_EMBED=1)
	add_dependencies(jce_renderer PackEngineShaders)
endif()

# Binary-size report target (needs JCE_Editor from add_subdirectory(editor)).
if(Python3_FOUND AND JCE_EMIT_LINK_MAP)
	set(_BINSIZE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_binsize_report.py")
	add_custom_target(binsize-report
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/reports"
		COMMAND ${Python3_EXECUTABLE} "${_BINSIZE_SCRIPT}"
			"${CMAKE_BINARY_DIR}/reports/JCE_Editor.map"
			"${CMAKE_BINARY_DIR}/reports/binsize_report.html"
			--title "JCE Editor binary composition"
			--json "${CMAKE_BINARY_DIR}/reports/binsize_report.json"
		DEPENDS JCE_Editor
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "Generating binary-size report"
		VERBATIM)
endif()

# ------------------------------------------------------------------ #
# Unit tests (opt-in via -DJCE_BUILD_TESTS=ON; default OFF to keep   #
# baseline configure-time small for non-developer builds).            #
# ------------------------------------------------------------------ #
option(JCE_BUILD_TESTS "Build JCE unit tests (Unity + CTest)" OFF)
if(JCE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Dist mode: suppress all logging ──
if(JCE_BUILD_VARIANT STREQUAL "dist")
	target_compile_definitions(jce_core PUBLIC JCE_DIST=1)
endif()

# ------------------------------------------------------------------ #
# Public-header dependency guard                                      #
#                                                                     #
# Fails the build if any header under engine/include/jce/ pulls in a  #
# third-party header (SDL, bgfx, imgui, assimp, ...).  Guarantees the #
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
# i18n JSON normalization                                             #
#                                                                     #
# `i18n-sort`  — rewrite editor/resources/assets/i18n/*.json in place #
# `i18n-check` — CI gate: fails on formatting drift, a UTF-8 BOM, OR  #
#               any locale missing a key en.json defines (--strict).  #
#                                                                     #
# Automation: a stamp-driven custom command runs sort_i18n.py on      #
# every build whose JSON inputs have changed, so the in-tree files    #
# stay normalized without anyone having to remember to run a target.  #
# The editor target depends on the stamp so an editor build always    #
# re-normalizes drifted i18n files.                                   #
# ------------------------------------------------------------------ #

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
	set(_JCE_I18N_SCRIPT "${CMAKE_SOURCE_DIR}/tools/sort_i18n.py")
	file(GLOB _JCE_I18N_FILES
		"${CMAKE_SOURCE_DIR}/editor/resources/assets/i18n/*.json")

	add_custom_target(i18n-sort
		COMMAND ${Python3_EXECUTABLE} "${_JCE_I18N_SCRIPT}"
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "Normalizing editor i18n JSON files"
		VERBATIM)

	add_custom_target(i18n-check
		COMMAND ${Python3_EXECUTABLE} "${_JCE_I18N_SCRIPT}" --check --report --strict
		WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
		COMMENT "Checking editor i18n JSON normalization + en-based completeness"
		VERBATIM)

	if(_JCE_I18N_FILES)
		set(_JCE_I18N_STAMP
			"${CMAKE_BINARY_DIR}/jce_i18n_sort.stamp")
		add_custom_command(
			OUTPUT  "${_JCE_I18N_STAMP}"
			COMMAND ${Python3_EXECUTABLE} "${_JCE_I18N_SCRIPT}"
			COMMAND ${CMAKE_COMMAND} -E touch "${_JCE_I18N_STAMP}"
			DEPENDS ${_JCE_I18N_FILES} "${_JCE_I18N_SCRIPT}"
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			COMMENT "Auto-normalizing editor i18n JSON files"
			VERBATIM)
		add_custom_target(jce_i18n_auto ALL
			DEPENDS "${_JCE_I18N_STAMP}")

		# The ALL membership only fires for the default build; an explicit
		# `--target JCE_Editor` build skips it.  Wire the editor (and game)
		# to depend on the stamp so every editor build re-normalizes drifted
		# i18n files, matching this block's documented contract.
		if(TARGET JCE_Editor)
			add_dependencies(JCE_Editor jce_i18n_auto)
		endif()
		if(TARGET caged_kingdom)
			add_dependencies(caged_kingdom jce_i18n_auto)
		endif()
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

option(JCE_ENABLE_CPPCHECK "Enable cppcheck target (x64 Windows release only)" ON)
# Static analysis is restricted to the x64 Windows *release* variant: it is a
# source-level gate, not a per-arch/per-config build artifact, so creating it
# for arm64 / 32-bit / non-Windows / dist / debug is pointless work. It is also
# NOT part of the default build (the target has no ALL — see below), so it never
# re-runs on an ordinary `cmake --build`; invoke it on demand with
# `cmake --build <dir> --target cppcheck` (scripts/jce.py editor --variant
# release does this automatically).
if(JCE_ENABLE_CPPCHECK
   AND WIN32
   AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "(AMD64|amd64|x86_64)"
   AND JCE_BUILD_VARIANT STREQUAL "release")
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
		# Incremental analysis: cppcheck caches per-translation-unit results here
		# and on the next run re-checks ONLY the files that changed (and their
		# dependents) — like a professional engine's static-analysis cache. The
		# dir must exist before cppcheck runs and must persist across builds (it
		# lives under the build dir, which is not wiped between builds). The three
		# invocations below run sequentially and share this cache, so only the
		# first does real work; the XML/quiet passes are cache hits.
		set(JCE_CPPCHECK_CACHE_DIR "${JCE_CPPCHECK_REPORT_DIR}/cppcheck-cache")
		file(MAKE_DIRECTORY "${JCE_CPPCHECK_CACHE_DIR}")

		# Common flags shared by all three cppcheck invocations.
		set(_CPPCHECK_COMMON_FLAGS
			--project=${CMAKE_BINARY_DIR}/compile_commands.json
			--cppcheck-build-dir=${JCE_CPPCHECK_CACHE_DIR}
			--relative-paths=${CMAKE_SOURCE_DIR}
			--enable=warning,style,performance,portability
			--inconclusive --inline-suppr
			--suppress=missingIncludeSystem
			--suppress=*:*/.conan2/*
			--suppress=*:*/third_party/*
			# ── Verified cppcheck false positives (2026-06 triage: 0 real bugs) ──
			# Each was confirmed harmless by reading the code; suppressed in config
			# (not inline) so the engine/editor sources stay untouched.
			# Generated protobuf headers: PROTOBUF_VERSION version-guard #error fires
			# because the conan runtime header is not on cppcheck's include path.
			--suppress=*:*proto_gen/*
			# Vendored third-party single-header (loop-invariant / early-return FPs).
			--suppress=*:*stb_image.h
			# Deliberate unsupported-platform #error in an #else arm; cppcheck does
			# not define the JCE_PLATFORM_* selector macros.
			--suppress=preprocessorErrorDirective:*jce_renderer_caps.c
			# cJSON_ArrayForEach: external library macro cppcheck cannot expand, so it
			# reports an unknown macro plus spurious null-derefs of the loop variable
			# (the macro's own `elem != NULL` condition guarantees non-NULL in body).
			--suppress=unknownMacro
			--suppress=nullPointer:*jce_bundle_deps.c
			--suppress=nullPointer:*jce_bundle_loader.c
			# VFS reader returns its own heap buffer, not &local; cppcheck mis-tracks
			# the out-size arg's address as the returned pointer.
			--suppress=returnDanglingLifetime:*jce_filesystem_host.c
			# Param renamed to avoid shadowing; positions/types match the header.
			--suppress=funcArgOrderDifferent:*jce_game_hud.c
			# sqrtf(sum of squares) is always >= 0; cppcheck reports the operand (-1),
			# not its square.
			--suppress=invalidFunctionArg:*jce_ibl.c
			# put_bytes' `if (n)` guard is deliberate (NULL+0 memcpy is UB); the caller
			# keeps n == 0 whenever the pointer is NULL.
			--suppress=nullPointerRedundantCheck:*jce_session.c
			# Fixed-size arrays filled for [0,count) and only [0,count) is read.
			--suppress=uninitvar:*jce_scene_renderer.c
			--suppress=uninitvar:*inspector_physics.cpp
			# Harmless dead store (records which pak the asset came from).
			--suppress=uselessAssignmentPtrArg:*jce_shaders.c
			-i${CMAKE_SOURCE_DIR}/engine/src/middleware/audio/third_party
			-i${CMAKE_SOURCE_DIR}/engine/src/middleware/video/third_party
			--file-filter=${CMAKE_SOURCE_DIR}/engine/*
			--file-filter=${CMAKE_SOURCE_DIR}/editor/*
			--file-filter=${CMAKE_SOURCE_DIR}/caged_kingdom/*
			-j${_JCE_CPPCHECK_JOBS}
			--std=c99)

		# No ALL: on-demand only (avoids re-running cppcheck on every build).
		add_custom_target(cppcheck
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
