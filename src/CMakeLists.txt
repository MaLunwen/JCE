# src/CMakeLists.txt
#
# Native build rules for JCE (pure C).
# Root CMakeLists.txt calls add_subdirectory("src"), so this file is
# the single CMake entry point for the native code.
#
# Asset embedding pipeline:
#
#   tools/jce_pak.c           -- Host tool (built first) that:
#                                1. ZSTD-compresses each asset
#                                2. Indexes paths via XXH3_64bits
#                                3. Writes a binary .pak (see pak_format.h)
#                                4. Generates COFF .obj (MSVC) to embed .pak
#   assets.pak (generated)    -- Compressed binary archive
#   assets_pak.obj (MSVC)     -- COFF object embedding assets.pak data
#   assets_embed.S (GCC/Clang)-- .incbin assembly embedding assets.pak
#   embedded_assets.h (gen.)  -- extern declarations for pak blob
#   _assets_manifest.cmake    -- Per-asset sizes consumed by build_report.cmake
#
# Runtime loader (main/native/resource/):
#   pak_format.h              -- On-disk format structs (shared with jce_pak)
#   pak_loader.h / .c         -- C API: pak_open/find/decompress
#
# Build size report (post-build):
#
#   tools/build_report.cmake  -- Reads the manifest + dep locations + exe
#                                and prints a columnar size/percentage table.
#   _dep_locs_<CONFIG>.cmake  -- Config-specific library file paths.
#
# ── Incremental-build design ──────────────────────────────────────────
#
# Fix 1 — CMAKE_SUPPRESS_REGENERATION (set BEFORE project()):
#   Removes the "Checking Build System" target from the VS solution.
#
# Fix 2 — Stamp file as the sole OUTPUT of the custom command:
#   MSBuild sees stamp > all assets on incremental builds.
#
# Expected incremental log (nothing changed):
#   [build] Build started
#   [build] Build succeeded.        <- no files recompiled

# ── MUST be set before project() ──────────────────────────────────────
# Removes "Checking Build System" / "ZERO_CHECK" from the VS solution.
set(CMAKE_SUPPRESS_REGENERATION TRUE)

cmake_minimum_required(VERSION 3.20)
project(JCE C)

# CXX is needed only for linking C++ dependencies (bgfx, assimp, imgui).
# No C++ source files exist in this project.
enable_language(CXX)

# Conan generators in this workspace are produced as Release for iOS/Xcode.
# Map other Xcode configs to Release so include/link properties still resolve.
if(CMAKE_GENERATOR STREQUAL "Xcode")
	set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)
	set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
	set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL Release)
endif()

option(JCE_BUILD_JNI "Build JCE as a JNI shared library" OFF)

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
	# Keep runtime compatibility when building on newer macOS hosts.
	set(JCE_MACOS_DEFAULT_DEPLOYMENT_TARGET "11.0")
	if((DEFINED CMAKE_OSX_ARCHITECTURES AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64") OR CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
		set(JCE_MACOS_DEFAULT_DEPLOYMENT_TARGET "10.15")
	endif()

	if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
		set(CMAKE_OSX_DEPLOYMENT_TARGET "${JCE_MACOS_DEFAULT_DEPLOYMENT_TARGET}" CACHE STRING "Minimum macOS deployment target" FORCE)
	endif()
endif()

# ------------------------------------------------------------------ #
# Platform-driven option overrides                                     #
# ------------------------------------------------------------------ #




# ------------------------------------------------------------------ #
# C standard                                                          #
# ------------------------------------------------------------------ #

set(CMAKE_C_STANDARD          99)
set(CMAKE_C_STANDARD_REQUIRED YES)
if(EMSCRIPTEN)
	# Emscripten EM_ASM macros (used by miniaudio) require GNU extensions.
	set(CMAKE_C_EXTENSIONS ON)
else()
	set(CMAKE_C_EXTENSIONS OFF)
endif()

# ------------------------------------------------------------------ #
# Source files                                                        #
# ------------------------------------------------------------------ #

# CONFIGURE_DEPENDS: the build system re-checks the glob on every build.
# New / removed source files are picked up automatically — no manual
# re-configure required (supported by Ninja and Makefiles generators).
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/main/native/*.c")

if(JCE_BUILD_JNI)
	file(GLOB_RECURSE JNI_BRIDGE_SOURCES CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/main/bridge/*.c")
	list(APPEND ENGINE_SOURCES ${JNI_BRIDGE_SOURCES})
	list(REMOVE_ITEM ENGINE_SOURCES
		"${CMAKE_CURRENT_SOURCE_DIR}/main/native/main.c")
endif()

# On Apple platforms, miniaudio's implementation includes AVFoundation
# (Objective-C framework) — must be compiled as ObjC, not plain C.
if(APPLE)
	set_source_files_properties(
		"${CMAKE_CURRENT_SOURCE_DIR}/main/native/audio/miniaudio_impl.c"
		PROPERTIES LANGUAGE OBJC)
	enable_language(OBJC)
endif()

# Android/JNI: shared library loaded by Java side.
# iOS: app bundle in executable mode.
# Others: standalone executable.
if(JCE_PLATFORM_ANDROID OR JCE_BUILD_JNI)
	add_library(JCE SHARED ${ENGINE_SOURCES})
	if(JCE_BUILD_JNI)
		set_target_properties(JCE PROPERTIES OUTPUT_NAME jce)
	endif()
else()
	add_executable(JCE ${ENGINE_SOURCES})
	if(JCE_PLATFORM_IOS)
		set(IOS_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist")
		set(IOS_APP_ICON   "${CMAKE_CURRENT_SOURCE_DIR}/ios/jce_i1024.png")
		if(NOT EXISTS "${IOS_INFO_PLIST}")
			message(FATAL_ERROR "Missing iOS Info.plist: ${IOS_INFO_PLIST}")
		endif()
		if(NOT EXISTS "${IOS_APP_ICON}")
			message(FATAL_ERROR "Missing iOS app icon: ${IOS_APP_ICON}")
		endif()

		set_target_properties(JCE PROPERTIES
			MACOSX_BUNDLE TRUE
			MACOSX_BUNDLE_INFO_PLIST "${IOS_INFO_PLIST}"
			MACOSX_BUNDLE_BUNDLE_NAME "JCE"
			MACOSX_BUNDLE_GUI_IDENTIFIER "com.jce"
			MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
			MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
			XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.jce"
			XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
			XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "4C98CBUY4F"
			XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

		target_sources(JCE PRIVATE "${IOS_APP_ICON}")
		set_source_files_properties("${IOS_APP_ICON}" PROPERTIES
			MACOSX_PACKAGE_LOCATION "Resources")
	endif()
endif()

# ------------------------------------------------------------------ #
# Compiler warnings (P10 rule 10)                                     #
# ------------------------------------------------------------------ #

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
	target_compile_options(JCE PRIVATE
		-Wall -Wextra -Wpedantic
		-Wconversion -Wshadow -Wdouble-promotion
		-Wno-unused-parameter)
elseif(MSVC)
	target_compile_options(JCE PRIVATE
		/W4
		/wd4100    # unreferenced formal parameter
		/wd4127    # conditional expression is constant (P10 assert)
		/Zc:preprocessor)  # standards-conforming preprocessor (required by bgfx/bx)
	target_compile_definitions(JCE PRIVATE
		_CRT_SECURE_NO_WARNINGS
		NOMINMAX
		WIN32_LEAN_AND_MEAN)
endif()

# SDL_MAIN_USE_CALLBACKS is only needed for standalone executable mode.
if(NOT JCE_BUILD_JNI)
	target_compile_definitions(JCE PRIVATE SDL_MAIN_USE_CALLBACKS=1)
endif()

# ------------------------------------------------------------------ #
# Third-party dependencies (provided by Conan via CMakeDeps)         #
# ------------------------------------------------------------------ #

find_package(SDL3_image   REQUIRED)
find_package(SDL3_ttf        REQUIRED)
find_package(SDL3           REQUIRED)
find_package(bgfx              REQUIRED)
find_package(flecs             REQUIRED)
find_package(glm             REQUIRED)
find_package(miniaudio      REQUIRED)
find_package(imgui           REQUIRED)
option(JCE_USE_ASSIMP "Use assimp for model loading" ON)
if(JCE_USE_ASSIMP)
	find_package(assimp      REQUIRED)
endif()
find_package(zstd            REQUIRED)
find_package(xxHash          REQUIRED)

if(JCE_BUILD_JNI)
	find_package(JNI REQUIRED)
endif()

target_link_libraries(JCE PRIVATE
	SDL3_image::SDL3_image
	SDL3_ttf::SDL3_ttf-static
	sdl::sdl
	bgfx::bgfx
	flecs::flecs_static
	glm::glm
	miniaudio::miniaudio
	imgui::imgui
	zstd::libzstd_static
	xxHash::xxhash)

if(JCE_USE_ASSIMP)
	target_link_libraries(JCE PRIVATE assimp::assimp)
else()
	target_compile_definitions(JCE PRIVATE JCE_NO_ASSIMP)
endif()

# ── Platform-specific link options ─────────────────────────────────
if(JCE_PLATFORM_WEB)
	# Generate a minimal HTML shell with AudioContext resume on first click.
	# Emscripten's --shell-file replaces {{{ SCRIPT }}} with the JS loader.
	set(_SHELL "${CMAKE_CURRENT_BINARY_DIR}/shell.html")
	file(WRITE "${_SHELL}" [=[<!doctype html>
<html><head><meta charset="utf-8"><title>JCE</title>
<style>
html,body{margin:0;padding:0;overflow:hidden;width:100%;height:100%;background:#000}
canvas{display:block;width:100%;height:100%;border:0;padding:0}
</style></head><body>
<canvas id="canvas" oncontextmenu="event.preventDefault()" tabindex="-1"></canvas>
<script>
var Module={canvas:document.getElementById('canvas')};
document.addEventListener('click',function r(){
  var ctx=typeof _ma_device__AudioContext!=='undefined'?_ma_device__AudioContext
         :typeof AL!=='undefined'&&AL.currentCtx?AL.currentCtx.audioCtx:null;
  if(ctx)ctx.resume();
  document.removeEventListener('click',r);
},true);
</script>
{{{ SCRIPT }}}
</body></html>
]=])

	set_target_properties(JCE PROPERTIES SUFFIX ".html")
	target_link_options(JCE PRIVATE
		-sWASM=1
		-sALLOW_MEMORY_GROWTH=1
		-sMAX_WEBGL_VERSION=2
		-sMIN_WEBGL_VERSION=2
		-sFULL_ES3=1
		-sSTACK_SIZE=1048576
		"SHELL:--shell-file ${_SHELL}")
elseif(JCE_PLATFORM_ANDROID)
	# Android system libraries required by bgfx (EGL/GLES) and SDL3.
	target_link_libraries(JCE PRIVATE
		EGL
		GLESv3
		android
		log)
elseif(JCE_PLATFORM_IOS)
	# iOS system frameworks required by bgfx (Metal) and SDL3.
	target_link_libraries(JCE PRIVATE
		"-framework Metal"
		"-framework QuartzCore"
		"-framework UIKit"
		"-framework Foundation"
		"-framework CoreGraphics"
		"-framework CoreText"
		"-framework AVFoundation"
		"-framework AudioToolbox"
		"-framework CoreAudio"
		"-framework GameController"
		"-framework CoreHaptics"
		"-framework CoreMotion"
		"-framework CoreServices")
elseif(JCE_PLATFORM_MACOS)
	# macOS system frameworks required by bgfx (Metal) and SDL3.
	target_link_libraries(JCE PRIVATE
		"-framework Metal"
		"-framework QuartzCore"
		"-framework Cocoa"
		"-framework IOKit"
		"-framework CoreVideo"
		"-framework CoreAudio"
		"-framework AudioToolbox"
		"-framework Carbon"
		"-framework ForceFeedback"
		"-framework GameController"
		"-framework CoreHaptics")
elseif(WIN32)
	# Windows system libraries required by bgfx (process enumeration).
	target_link_libraries(JCE PRIVATE psapi)
endif()

# Statically link the C++ runtime on Linux so the binary runs on systems
# without a matching libstdc++ version.  LINKER_LANGUAGE CXX ensures g++
# is used as the linker driver (the project is pure C but links C++ deps).
#
# Problem: -static-libstdc++ is a g++ DRIVER flag that only replaces the
# *implicit* -lstdc++ added by g++ itself.  Conan transitive dependencies
# (e.g. assimp) inject an *explicit* -lstdc++ on the link line, which
# still resolves to the shared libstdc++.so — defeating the flag.
#
# Solution: create a linker script that redirects every -lstdc++ on the
# entire link line (implicit AND explicit) to libstdc++.a.  We place
# this script in a directory that comes first on the library search path
# via -L, so the linker finds our script before the real libstdc++.so.
# The same trick is applied to libgcc_s → libgcc.a + libgcc_eh.a.
if(JCE_PLATFORM_LINUX)
	set_target_properties(JCE PROPERTIES LINKER_LANGUAGE CXX)

	# ── Locate the static archives ──
	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a
		OUTPUT_VARIABLE _STATIC_STDCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT EXISTS "${_STATIC_STDCXX}")
		message(FATAL_ERROR
			"libstdc++.a not found (looked for: ${_STATIC_STDCXX}).\n"
			"Install it:  sudo apt install libstdc++-11-dev")
	endif()
	message(STATUS "Static libstdc++: ${_STATIC_STDCXX}")

	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgcc.a
		OUTPUT_VARIABLE _STATIC_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgcc_eh.a
		OUTPUT_VARIABLE _STATIC_LIBGCC_EH OUTPUT_STRIP_TRAILING_WHITESPACE)

	# ── Create linker scripts that redirect .so → .a ──
	# A linker script named "libstdc++.so" containing GROUP( libstdc++.a )
	# makes the linker pull in the static archive whenever it would have
	# loaded the shared library.  We place these scripts in a private
	# directory and prepend -L<dir> so they shadow the real .so files.
	set(_STATIC_SHIM_DIR "${CMAKE_CURRENT_BINARY_DIR}/_static_shims")
	file(MAKE_DIRECTORY "${_STATIC_SHIM_DIR}")

	file(WRITE "${_STATIC_SHIM_DIR}/libstdc++.so"
		"/* Linker script — redirect to static archive */\n"
		"GROUP ( ${_STATIC_STDCXX} )\n")

	if(EXISTS "${_STATIC_LIBGCC}" AND EXISTS "${_STATIC_LIBGCC_EH}")
		file(WRITE "${_STATIC_SHIM_DIR}/libgcc_s.so"
			"/* Linker script — redirect to static archive */\n"
			"GROUP ( ${_STATIC_LIBGCC} ${_STATIC_LIBGCC_EH} )\n")
		file(WRITE "${_STATIC_SHIM_DIR}/libgcc_s.so.1"
			"/* Linker script — redirect to static archive */\n"
			"GROUP ( ${_STATIC_LIBGCC} ${_STATIC_LIBGCC_EH} )\n")
		message(STATUS "Static libgcc:    ${_STATIC_LIBGCC}")
	endif()

	# Prepend our shim directory so the linker finds our scripts first.
	target_link_options(JCE PRIVATE
		"-L${_STATIC_SHIM_DIR}"
		-static-libstdc++ -static-libgcc)

	# Also link with -Wl,--as-needed to avoid pulling unnecessary DT_NEEDED.
endif()

# ------------------------------------------------------------------ #
# Include directories                                                 #
# ------------------------------------------------------------------ #

target_include_directories(JCE PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/main/native"   # project headers
	"${CMAKE_CURRENT_SOURCE_DIR}/main/bridge"   # JNI bridge headers
	"${CMAKE_CURRENT_BINARY_DIR}")              # generated headers

if(JCE_BUILD_JNI)
	target_include_directories(JCE PRIVATE ${JNI_INCLUDE_DIRS})
endif()

# ------------------------------------------------------------------ #
# Shader compilation (bgfx shaderc)                                   #
# ------------------------------------------------------------------ #
#
# Compiles all vs_*.sc / fs_*.sc in the shader source directory to
# .bin files for four API profiles (D3D11, Vulkan, GLSL, ESSL).
# Output goes into the assets tree so jce_pak embeds them in the PAK.

include("${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_shaders.cmake")

jce_compile_shaders(
	TARGET        CompileShaders
	SHADER_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/main/native/shaders"
	OUTPUT_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/main/resources/assets/shaders"
	OUT_FILES_VAR COMPILED_SHADER_FILES)

# ------------------------------------------------------------------ #
# Host packer tool: jce_pak                                           #
# ------------------------------------------------------------------ #
#
# jce_pak is a C99 host tool that compresses assets with ZSTD,
# indexes them with XXH3, writes a binary .pak, and (on MSVC)
# generates a COFF .obj that embeds the pak blob directly.
#
# When cross-compiling, jce_pak cannot be built for the target — it
# must run on the host.  Provide a pre-built path via:
#   -DJCE_PAK_EXECUTABLE=/path/to/host/jce_pak

if(CMAKE_CROSSCOMPILING)
	if(NOT JCE_PAK_EXECUTABLE)
		message(FATAL_ERROR
			"Cross-compiling: set -DJCE_PAK_EXECUTABLE=<path> "
			"to a host-built jce_pak binary.")
	endif()
	# Create an imported target so the rest of the file can use
	# $<TARGET_FILE:jce_pak> uniformly.
	add_executable(jce_pak IMPORTED)
	set_target_properties(jce_pak PROPERTIES
		IMPORTED_LOCATION "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_DEBUG "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_RELEASE "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_RELWITHDEBINFO "${JCE_PAK_EXECUTABLE}"
		IMPORTED_LOCATION_MINSIZEREL "${JCE_PAK_EXECUTABLE}")
else()
	add_executable(jce_pak
		"${CMAKE_CURRENT_SOURCE_DIR}/tools/jce_pak.c")

	# jce_pak is a build-time-only tool — exclude from the default ALL
	# target so it doesn't appear as a "product" in the IDE.  The
	# PackAssets custom command depends on it explicitly, which ensures
	# it gets built exactly when needed.
	set_target_properties(jce_pak PROPERTIES
		EXCLUDE_FROM_ALL      ON
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools"
		C_STANDARD            99
		C_STANDARD_REQUIRED   YES
		C_EXTENSIONS          OFF)

	target_include_directories(jce_pak PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/main/native")

	target_link_libraries(jce_pak PRIVATE
		zstd::libzstd_static
		xxHash::xxhash)

	if(MSVC)
		target_compile_definitions(jce_pak PRIVATE
			_CRT_SECURE_NO_WARNINGS
			NOMINMAX
			WIN32_LEAN_AND_MEAN)
	endif()
endif()

# ------------------------------------------------------------------ #
# Asset embedding                                                     #
# ------------------------------------------------------------------ #

set(RESOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/main/resources/assets")
set(PAK_FILE       "${CMAKE_CURRENT_BINARY_DIR}/assets.pak")
set(PAK_OBJ_FILE   "${CMAKE_CURRENT_BINARY_DIR}/assets_pak.obj")
set(HEADER_FILE    "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.h")
set(MANIFEST_FILE  "${CMAKE_CURRENT_BINARY_DIR}/_assets_manifest.cmake")

# Stamp file: the single OUTPUT that MSBuild uses to determine freshness.
set(STAMP_FILE "${CMAKE_CURRENT_BINARY_DIR}/_assets_pak.stamp")

file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${RESOURCE_DIR}/*")

# Determine embedding method and jce_pak flags.
set(PAK_C_ARRAY_FILE "${CMAKE_CURRENT_BINARY_DIR}/assets_pak_data.c")

if(JCE_PLATFORM_WEB)
	# Emscripten: .incbin not supported — use C-array embedding.
	set(_OBJ_FORMAT "c-array")
	set(_OBJ_FLAGS  --obj-format c-array --c-file "${PAK_C_ARRAY_FILE}")
elseif(MSVC)
	# Map CMAKE_SYSTEM_PROCESSOR to a jce_pak --obj-arch value.
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
	set(_OBJ_FLAGS  --obj-file "${PAK_OBJ_FILE}" --obj-format coff --obj-arch "${_OBJ_ARCH}")
else()
	# GCC/Clang on ELF/Mach-O: .incbin assembly.
	set(_OBJ_FORMAT "none")
	set(_OBJ_FLAGS  --obj-format none)
endif()

add_custom_command(
	OUTPUT     "${STAMP_FILE}"
	BYPRODUCTS "${PAK_FILE}" "${PAK_OBJ_FILE}" "${PAK_C_ARRAY_FILE}" "${HEADER_FILE}" "${MANIFEST_FILE}"
	COMMAND    $<TARGET_FILE:jce_pak>
	           --resource-dir  "${RESOURCE_DIR}"
	           --pak-file      "${PAK_FILE}"
	           --header-file   "${HEADER_FILE}"
	           --manifest-file "${MANIFEST_FILE}"
	           ${_OBJ_FLAGS}
	COMMAND    "${CMAKE_COMMAND}" -E touch "${STAMP_FILE}"
	DEPENDS    jce_pak ${ASSET_FILES} ${COMPILED_SHADER_FILES}
	COMMENT    "Packing assets -> ${PAK_FILE}"
	VERBATIM)

set_source_files_properties(
	"${PAK_FILE}" "${PAK_OBJ_FILE}" "${HEADER_FILE}"
	"${MANIFEST_FILE}" "${STAMP_FILE}"
	PROPERTIES GENERATED TRUE)

add_custom_target(PackAssets DEPENDS "${STAMP_FILE}")
add_dependencies(PackAssets CompileShaders)
add_dependencies(JCE PackAssets)

# ── Link the embedded .pak blob ────────────────────────────────────

if(JCE_PLATFORM_WEB)
	# Emscripten: C-array source file generated by jce_pak.
	target_sources(JCE PRIVATE "${PAK_C_ARRAY_FILE}")
	set_source_files_properties("${PAK_C_ARRAY_FILE}"
		PROPERTIES GENERATED TRUE)
elseif(MSVC)
	# COFF .obj generated by jce_pak: contains assets_pak_data and
	# assets_pak_data_size symbols in .rdata.
	add_library(pak_obj OBJECT IMPORTED)
	set_target_properties(pak_obj PROPERTIES
		IMPORTED_OBJECTS "${PAK_OBJ_FILE}")
	target_link_libraries(JCE PRIVATE pak_obj)
else()
	# GCC/Clang: generate a .S file that uses .incbin to embed the .pak.
	enable_language(ASM)
	set(PAK_FILE_PATH "${PAK_FILE}")
	configure_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/tools/assets_embed.S.in"
		"${CMAKE_CURRENT_BINARY_DIR}/assets_embed.S"
		@ONLY)
	target_sources(JCE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/assets_embed.S")
	set_source_files_properties(
		"${CMAKE_CURRENT_BINARY_DIR}/assets_embed.S"
		PROPERTIES GENERATED TRUE LANGUAGE ASM)
endif()

# ------------------------------------------------------------------ #
# Build size report — dependency locations                            #
# ------------------------------------------------------------------ #
#
# file(GENERATE) evaluates generator expressions at generation time
# and writes a config-specific .cmake file.  build_report.cmake includes
# this file at POST_BUILD time to get resolved library paths.

set(_dep_locs_content "# Auto-generated by src/CMakeLists.txt -- DO NOT EDIT\n")
string(APPEND _dep_locs_content "set(DEP_LOCATIONS\n")

# Enumerate every linked target and the user-facing name to display.
# Pairs are  "DisplayName|TargetName".
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

set(DEP_LOCS_FILE
	"${CMAKE_CURRENT_BINARY_DIR}/_dep_locs_$<CONFIG>.cmake")

file(GENERATE
	OUTPUT  "${DEP_LOCS_FILE}"
	CONTENT "${_dep_locs_content}")

# ------------------------------------------------------------------ #
# Build size report — POST_BUILD command                              #
# ------------------------------------------------------------------ #

set(REPORT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_report.cmake")

# Remove MSVC-generated .exp/.lib — produced because SDL_MAIN_USE_CALLBACKS
# exports callback symbols via __declspec(dllexport); not needed for a standalone exe.
if(MSVC AND NOT JCE_BUILD_JNI)
	add_custom_command(TARGET JCE POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -f
		        "$<TARGET_FILE_DIR:JCE>/$<TARGET_FILE_BASE_NAME:JCE>.exp"
		        "$<TARGET_FILE_DIR:JCE>/$<TARGET_FILE_BASE_NAME:JCE>.lib"
		COMMENT "Removing .exp/.lib"
		VERBATIM)
endif()

add_custom_command(TARGET JCE POST_BUILD
	COMMAND "${CMAKE_COMMAND}"
	        -DEXE_FILE=$<TARGET_FILE:JCE>
	        -DPAK_FILE=${PAK_FILE}
	        -DMANIFEST_FILE=${MANIFEST_FILE}
	        -DDEP_LOCS_FILE=${CMAKE_CURRENT_BINARY_DIR}/_dep_locs_$<CONFIG>.cmake
	        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/main/native
	        -DCONFIG=$<CONFIG>
	        -P "${REPORT_SCRIPT}"
	COMMENT "Build size report"
	VERBATIM)

# ------------------------------------------------------------------ #
# Install                                                             #
# ------------------------------------------------------------------ #

install(TARGETS JCE DESTINATION bin)
