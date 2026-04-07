cmake_minimum_required(VERSION 3.20)

# ── MUST be set before project() ──
set(CMAKE_SUPPRESS_REGENERATION TRUE)

project(JCE C)

# CXX is needed for C++ dependencies (bgfx, assimp, imgui) and the editor.
enable_language(CXX)

# Export compile database for tooling (cppcheck/clangd/etc.).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
	if(MSVC)
		target_compile_definitions(jce_pak PRIVATE
			_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	endif()
endif()

# ------------------------------------------------------------------ #
# Host cooker tool: jce_cook                                          #
# ------------------------------------------------------------------ #

if(NOT CMAKE_CROSSCOMPILING)
	add_executable(jce_cook
		"${JCE_TOOLS_DIR}/jce_cook.c"
		"${JCE_TOOLS_DIR}/miniaudio_impl_cook.c"
		"${JCE_ENGINE_SRC_DIR}/resource/jce_asset_cooker.c")
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
		zstd::libzstd_static
		xxHash::xxhash)
	if(MSVC)
		target_compile_definitions(jce_cook PRIVATE
			_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	endif()
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

file(GLOB_RECURSE ENGINE_ASSET_FILES CONFIGURE_DEPENDS "${ENGINE_RESOURCE_DIR}/*")
file(GLOB_RECURSE GAME_ASSET_FILES   CONFIGURE_DEPENDS "${GAME_RESOURCE_DIR}/*")
file(GLOB_RECURSE EDITOR_ASSET_FILES CONFIGURE_DEPENDS "${EDITOR_RESOURCE_DIR}/*")

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
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${ENGINE_RESOURCE_DIR}"  "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${GAME_RESOURCE_DIR}"    "${GAME_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    "${CMAKE_COMMAND}" -E touch "${COOK_GAME_STAMP}"
		DEPENDS    jce_cook ${ENGINE_ASSET_FILES} ${GAME_ASSET_FILES}
		COMMENT    "Cooking game assets (textures/audio → .jceasset)"
		VERBATIM)

	set(EDITOR_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/editor/resources/JCE_icon.png")

	add_custom_command(
		OUTPUT     "${COOK_EDITOR_STAMP}"
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${ENGINE_RESOURCE_DIR}"  "${EDITOR_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    $<TARGET_FILE:jce_cook>
		           --batch "${EDITOR_RESOURCE_DIR}"  "${EDITOR_COOKED_DIR}"
		           --preserve-names --level 0 --max-texture-size 2048
		COMMAND    "${CMAKE_COMMAND}" -E copy
		           "${EDITOR_ICON_FILE}" "${EDITOR_COOKED_DIR}/JCE_icon.png"
		COMMAND    "${CMAKE_COMMAND}" -E touch "${COOK_EDITOR_STAMP}"
		DEPENDS    jce_cook ${ENGINE_ASSET_FILES} ${EDITOR_ASSET_FILES}
		           "${EDITOR_ICON_FILE}"
		COMMENT    "Cooking editor assets (textures/audio → .jceasset)"
		VERBATIM)

	add_custom_target(CookGameAssets   DEPENDS "${COOK_GAME_STAMP}")
	add_custom_target(CookEditorAssets DEPENDS "${COOK_EDITOR_STAMP}")
	add_dependencies(CookGameAssets   CompileShaders CompileShadersPBR CompileShadersImGui)
	add_dependencies(CookEditorAssets CompileShaders CompileShadersPBR CompileShadersImGui)

	# PAK reads from a single cooked dir (engine + app merged).
	set(_GAME_PAK_RES_FLAGS    --resource-dir "${GAME_COOKED_DIR}")
	set(_EDITOR_PAK_RES_FLAGS  --resource-dir "${EDITOR_COOKED_DIR}")
	set(_COOK_GAME_DEP    CookGameAssets)
	set(_COOK_EDITOR_DEP  CookEditorAssets)
else()
	# No cooking — PAK reads raw resource dirs directly.
	set(_GAME_PAK_RES_FLAGS
		--resource-dir "${ENGINE_RESOURCE_DIR}"
		--resource-dir "${GAME_RESOURCE_DIR}")
	set(_EDITOR_PAK_RES_FLAGS
		--resource-dir "${ENGINE_RESOURCE_DIR}"
		--resource-dir "${EDITOR_RESOURCE_DIR}")
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

add_custom_command(
	OUTPUT     "${GAME_STAMP_FILE}"
	BYPRODUCTS "${GAME_PAK_FILE}" "${GAME_PAK_OBJ_FILE}" "${GAME_PAK_C_ARRAY}" "${GAME_HEADER_FILE}" "${GAME_MANIFEST_FILE}"
	COMMAND    $<TARGET_FILE:jce_pak>
	           ${_GAME_PAK_RES_FLAGS}
	           --pak-file      "${GAME_PAK_FILE}"
	           --header-file   "${GAME_HEADER_FILE}"
	           --manifest-file "${GAME_MANIFEST_FILE}"
	           ${_GAME_OBJ_FLAGS}
	COMMAND    "${CMAKE_COMMAND}" -E touch "${GAME_STAMP_FILE}"
	DEPENDS    jce_pak ${ENGINE_ASSET_FILES} ${GAME_ASSET_FILES}
	           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES} ${COMPILED_IMGUI_SHADER_FILES}
	COMMENT    "Packing game assets -> ${GAME_PAK_FILE}"
	VERBATIM)

set_source_files_properties(
	"${GAME_PAK_FILE}" "${GAME_PAK_OBJ_FILE}" "${GAME_HEADER_FILE}"
	"${GAME_MANIFEST_FILE}" "${GAME_STAMP_FILE}" "${GAME_PAK_C_ARRAY}"
	PROPERTIES GENERATED TRUE)

add_custom_target(PackGameAssets DEPENDS "${GAME_STAMP_FILE}")
add_dependencies(PackGameAssets CompileShaders CompileShadersPBR CompileShadersImGui)
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

add_custom_command(
	OUTPUT     "${EDITOR_STAMP_FILE}"
	BYPRODUCTS "${EDITOR_PAK_FILE}" "${EDITOR_PAK_OBJ_FILE}" "${EDITOR_PAK_C_ARRAY}" "${EDITOR_HEADER_FILE}" "${EDITOR_MANIFEST_FILE}"
	COMMAND    $<TARGET_FILE:jce_pak>
	           ${_EDITOR_PAK_RES_FLAGS}
	           --pak-file      "${EDITOR_PAK_FILE}"
	           --header-file   "${EDITOR_HEADER_FILE}"
	           --manifest-file "${EDITOR_MANIFEST_FILE}"
	           ${_EDITOR_OBJ_FLAGS}
	COMMAND    "${CMAKE_COMMAND}" -E touch "${EDITOR_STAMP_FILE}"
	DEPENDS    jce_pak ${ENGINE_ASSET_FILES} ${EDITOR_ASSET_FILES}
	           ${COMPILED_SHADER_FILES} ${COMPILED_PBR_SHADER_FILES} ${COMPILED_IMGUI_SHADER_FILES}
	COMMENT    "Packing editor assets -> ${EDITOR_PAK_FILE}"
	VERBATIM)

set_source_files_properties(
	"${EDITOR_PAK_FILE}" "${EDITOR_PAK_OBJ_FILE}" "${EDITOR_HEADER_FILE}"
	"${EDITOR_MANIFEST_FILE}" "${EDITOR_STAMP_FILE}" "${EDITOR_PAK_C_ARRAY}"
	PROPERTIES GENERATED TRUE)

add_custom_target(PackEditorAssets DEPENDS "${EDITOR_STAMP_FILE}")
add_dependencies(PackEditorAssets CompileShaders CompileShadersPBR CompileShadersImGui)
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
	target_compile_definitions(JCE PUBLIC JCE_DIST=1)
endif()

# ------------------------------------------------------------------ #
# Static analysis: cppcheck                                           #
# ------------------------------------------------------------------ #

option(JCE_ENABLE_CPPCHECK "Enable cppcheck target" ON)
if(JCE_ENABLE_CPPCHECK)
	find_program(CPPCHECK_EXECUTABLE NAMES cppcheck HINTS "D:/CMake/Cppcheck")
	if(CPPCHECK_EXECUTABLE)
		set(JCE_CPPCHECK_REPORT_DIR "${CMAKE_BINARY_DIR}/reports")
		set(JCE_CPPCHECK_TEXT_REPORT "${JCE_CPPCHECK_REPORT_DIR}/cppcheck-report.txt")
		set(JCE_CPPCHECK_XML_REPORT  "${JCE_CPPCHECK_REPORT_DIR}/cppcheck-report.xml")

		add_custom_target(cppcheck ALL
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${JCE_CPPCHECK_REPORT_DIR}"
			COMMAND "${CPPCHECK_EXECUTABLE}"
				--project=${CMAKE_BINARY_DIR}/compile_commands.json
				--relative-paths=${CMAKE_SOURCE_DIR}
				--enable=warning,style,performance,portability
				--inconclusive --inline-suppr
				--suppress=missingIncludeSystem
				--suppress=*:*/.conan2/*
				--file-filter=${CMAKE_SOURCE_DIR}/engine/*
				--file-filter=${CMAKE_SOURCE_DIR}/editor/*
				--file-filter=${CMAKE_SOURCE_DIR}/caged_kingdom/*
				--std=c99
				--template={file}:{line}:{severity}:{id}:{message}
				--output-file=${JCE_CPPCHECK_TEXT_REPORT}
			COMMAND "${CPPCHECK_EXECUTABLE}"
				--project=${CMAKE_BINARY_DIR}/compile_commands.json
				--relative-paths=${CMAKE_SOURCE_DIR}
				--enable=warning,style,performance,portability
				--inconclusive --inline-suppr
				--suppress=missingIncludeSystem
				--suppress=*:*/.conan2/*
				--file-filter=${CMAKE_SOURCE_DIR}/engine/*
				--file-filter=${CMAKE_SOURCE_DIR}/editor/*
				--file-filter=${CMAKE_SOURCE_DIR}/caged_kingdom/*
				--std=c99 --xml --xml-version=2
				--output-file=${JCE_CPPCHECK_XML_REPORT}
			COMMAND "${CPPCHECK_EXECUTABLE}"
				--project=${CMAKE_BINARY_DIR}/compile_commands.json
				--relative-paths=${CMAKE_SOURCE_DIR}
				--enable=warning,style,performance,portability
				--inconclusive --inline-suppr
				--suppress=missingIncludeSystem
				--suppress=*:*/.conan2/*
				--file-filter=${CMAKE_SOURCE_DIR}/engine/*
				--file-filter=${CMAKE_SOURCE_DIR}/editor/*
				--file-filter=${CMAKE_SOURCE_DIR}/caged_kingdom/*
				--std=c99 --quiet
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
