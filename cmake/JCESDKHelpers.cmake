# JCESDKHelpers.cmake
#
# Convenience helpers exposed to end-user projects that consume the
# pre-built JCE SDK via `find_package(JCE REQUIRED)`.
#
# Shipped to <sdk>/lib/cmake/JCE/ and auto-included by JCEConfig.cmake.
#
# Public:
#   jce_shader_profiles(<out-var>)
#   jce_shader_pak_exclude_flags(<out-var> [PROFILES <p> [<p>...]])
#   jce_add_project_shaders(TARGET <target> SOURCE_DIR <dir>
#       OUTPUT_DIR <dir> VARYING_DEF <file> [INCLUDE_DIRS <dir> ...]
#       [PROFILES <suffix> ...] [OUT_FILES_VAR <var>])
#
#   jce_target_embed_pak(<target>
#       RESOURCE_DIRS  <dir> [<dir>...]
#       [PAK_FILE      <path>]                  # default: <bin>/<target>_assets.pak
#       [SYMBOL_PREFIX <symbol>]                # default: assets_pak_data
#       [EXCLUDE_SEGMENTS <seg> [<seg>...]]
#       [STRIP_DEBUG_PATHS])
#
# Links editor-prebuilt assets into <target> when
# `JCE_PROJECT_PREBUILT_ASSETS_OBJ`, `JCE_PROJECT_PREBUILT_ASSETS_ASM`, or
# `JCE_PROJECT_PREBUILT_ASSETS_C` is set.  Manual SDK consumers can still
# provide `${JCE_PAK_EXECUTABLE}` and let this helper pack resource dirs.
#
# On MSVC the packer emits a COFF .obj that we link directly.  On
# GNU/Clang/Apple targets the packer emits a tiny .S wrapper that
# `.incbin`s the raw PAK; we add the .S to the target sources.
#
# The embedded symbols are: <SYMBOL_PREFIX> (uint8_t[]) and
# <SYMBOL_PREFIX>_size (size_t).  The engine's bootstrap looks for
# assets_pak_data / assets_pak_data_size by default.

include_guard(GLOBAL)

# ------------------------------------------------------------------ #
# jce_shader_profiles(<out-var>)                                      #
#                                                                     #
# THE per-platform bgfx shader-profile matrix.  Single definition:    #
# the in-tree engine build includes this file for it, and an          #
# out-of-tree `find_package(JCE)` consumer gets it because this file  #
# is what the SDK installs next to JCEConfig.cmake.  Keeping two      #
# copies drifted before (the SDK's unknown-platform fallback still    #
# listed `essl1`, which the runtime can never load).                  #
#                                                                     #
# Compile (and pak) only the bgfx renderer back-ends the target       #
# platform can actually load: saves compile time and keeps a shipped  #
# .pak slim (e.g. no Metal binaries in a Windows build).              #
#                                                                     #
# The in-tree build sets JCE_PLATFORM_*; a standalone SDK consumer    #
# never processes the engine's root CMakeLists and therefore has only #
# the stock CMake platform variables.  Both arms must answer the      #
# same — that is the point of having one function.                    #
# ------------------------------------------------------------------ #
function(jce_shader_profiles OUT_VAR)
	if(JCE_PLATFORM_WINDOWS)
		set(_plat windows)
	elseif(JCE_PLATFORM_MACOS OR JCE_PLATFORM_IOS)
		set(_plat apple)
	elseif(JCE_PLATFORM_ANDROID)
		set(_plat android)
	elseif(JCE_PLATFORM_WEB)
		set(_plat web)
	elseif(JCE_PLATFORM_LINUX)
		set(_plat linux)
	elseif(EMSCRIPTEN)
		set(_plat web)
	elseif(ANDROID)
		set(_plat android)
	elseif(WIN32)
		set(_plat windows)
	elseif(APPLE)
		set(_plat apple)
	elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
		set(_plat linux)
	else()
		set(_plat other)
	endif()

	if(_plat STREQUAL "windows")
		set(_profiles dx11 spv glsl)
	elseif(_plat STREQUAL "apple")
		# Apple deprecated desktop OpenGL and bgfx no longer reports
		# BGFX_RENDERER_TYPE_OPENGL on Apple platforms, so the `glsl`
		# profile would only bloat the .pak with unreachable binaries.
		set(_profiles mtl spv)
	elseif(_plat STREQUAL "android")
		# NOTE: no essl1 — the runtime shader loader (jce_shaders.c
		# shader_suffix) maps BGFX_RENDERER_TYPE_OPENGLES to "essl"
		# unconditionally, so essl1 binaries were built + PAKed but
		# never loadable (dead .pak weight).  If a real GLES2 tier is
		# ever wanted, add a caps-based suffix pick there first.
		set(_profiles essl spv)
	elseif(_plat STREQUAL "web")
		# NOTE: no essl1 — see the Android note above (never loaded).
		set(_profiles essl)
	elseif(_plat STREQUAL "linux")
		set(_profiles spv glsl)
	else()
		# Unknown platform: keep every *loadable* profile.  essl1 is
		# excluded here too, for the reason spelled out above.
		set(_profiles dx11 spv glsl essl mtl)
	endif()

	set(${OUT_VAR} ${_profiles} PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------ #
# jce_shader_pak_exclude_flags(<out-var> [PROFILES <p> [<p>...]])     #
#                                                                     #
# Turn a profile selection into the `--exclude-suffix _<sfx>.bin`     #
# flags jce_pak needs, so neither a stale binary left in a shared     #
# output dir by a previous build for another platform, nor an SDK     #
# resource tree that ships every platform's precompiled variant, ends #
# up inside the .pak.  PROFILES defaults to jce_shader_profiles().    #
# ------------------------------------------------------------------ #
function(jce_shader_pak_exclude_flags OUT_VAR)
	cmake_parse_arguments(SX "" "" "PROFILES" ${ARGN})

	# Every suffix jce_compile_shaders can emit (tools/compile_shaders.cmake).
	set(_all_profiles dx11 spv glsl essl essl1 mtl)

	# An explicitly passed but EMPTY profile list means "exclude every
	# backend"; only an absent PROFILES keyword falls back to the matrix.
	if(SX_PROFILES OR "PROFILES" IN_LIST SX_KEYWORDS_MISSING_VALUES)
		set(_profiles ${SX_PROFILES})
	else()
		jce_shader_profiles(_profiles)
	endif()

	set(_flags)
	foreach(_p IN LISTS _all_profiles)
		if(NOT _p IN_LIST _profiles)
			list(APPEND _flags --exclude-suffix "_${_p}.bin")
		endif()
	endforeach()

	set(${OUT_VAR} ${_flags} PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------ #
# jce_add_project_shaders(...)                                       #
#                                                                     #
# Project shaders use the engine's exact shaderc/profile/lint path.   #
# The output stays a normal project asset and is never linked into    #
# the engine library. Missing compiler/include inputs are fatal: an   #
# executable with silently absent project shaders is not releasable.  #
# ------------------------------------------------------------------ #
function(jce_add_project_shaders)
	cmake_parse_arguments(PS ""
		"TARGET;SOURCE_DIR;OUTPUT_DIR;VARYING_DEF;OUT_FILES_VAR"
		"INCLUDE_DIRS;PROFILES" ${ARGN})
	foreach(_required TARGET SOURCE_DIR OUTPUT_DIR VARYING_DEF)
		if(NOT PS_${_required})
			message(FATAL_ERROR
				"jce_add_project_shaders: ${_required} is required.")
		endif()
	endforeach()
	if(TARGET ${PS_TARGET})
		message(FATAL_ERROR
			"jce_add_project_shaders: target '${PS_TARGET}' already exists.")
	endif()
	if(NOT IS_DIRECTORY "${PS_SOURCE_DIR}")
		message(FATAL_ERROR
			"jce_add_project_shaders: SOURCE_DIR not found: ${PS_SOURCE_DIR}")
	endif()
	if(NOT EXISTS "${PS_SOURCE_DIR}/${PS_VARYING_DEF}")
		message(FATAL_ERROR
			"jce_add_project_shaders: VARYING_DEF not found: "
			"${PS_SOURCE_DIR}/${PS_VARYING_DEF}")
	endif()

	if(NOT BGFX_SHADER_INCLUDE_PATH AND JCE_BGFX_SHADER_INCLUDE_PATH)
		set(BGFX_SHADER_INCLUDE_PATH "${JCE_BGFX_SHADER_INCLUDE_PATH}")
	endif()
	if(NOT BGFX_SHADER_INCLUDE_PATH)
		message(FATAL_ERROR
			"jce_add_project_shaders: BGFX shader ABI include path is unavailable.")
	endif()

	set(_compile_module
		"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/JCECompileShaders.cmake")
	if(NOT EXISTS "${_compile_module}")
		set(_compile_module
			"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/compile_shaders.cmake")
	endif()
	if(NOT EXISTS "${_compile_module}")
		message(FATAL_ERROR
			"jce_add_project_shaders: compile module is unavailable.")
	endif()
	include("${_compile_module}")
	set(_project_include_dirs ${PS_INCLUDE_DIRS})
	if(JCE_SHADER_INCLUDE_PATH)
		list(APPEND _project_include_dirs "${JCE_SHADER_INCLUDE_PATH}")
	elseif(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../engine/shaders/include")
		list(APPEND _project_include_dirs
			"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../engine/shaders/include")
	endif()

	if(PS_PROFILES)
		set(JCE_SHADER_PROFILES ${PS_PROFILES})
	elseif(NOT JCE_SHADER_PROFILES)
		jce_shader_profiles(JCE_SHADER_PROFILES)
	endif()
	jce_compile_shaders(
		REQUIRED
		TARGET        ${PS_TARGET}
		SHADER_DIR    "${PS_SOURCE_DIR}"
		VARYING_DEF   "${PS_VARYING_DEF}"
		OUTPUT_DIR    "${PS_OUTPUT_DIR}"
		INCLUDE_DIRS  ${_project_include_dirs}
		OUT_FILES_VAR _project_shader_outputs)
	set_property(TARGET ${PS_TARGET} PROPERTY
		JCE_PROJECT_SHADER_OUTPUTS "${_project_shader_outputs}")
	if(PS_OUT_FILES_VAR)
		set(${PS_OUT_FILES_VAR} ${_project_shader_outputs} PARENT_SCOPE)
	endif()
endfunction()

# ------------------------------------------------------------------ #
# jce_configure_application_target(<target>)                           #
#                                                                     #
# Apply the portable shipping policy to an SDK-consumer executable.   #
# `dist` is a JCE variant layered on top of CMake Release, so checking #
# CMAKE_BUILD_TYPE for "Dist" is incorrect.                           #
# ------------------------------------------------------------------ #
function(jce_configure_application_target TARGET)
	if(NOT TARGET ${TARGET})
		message(FATAL_ERROR
			"jce_configure_application_target: '${TARGET}' is not a target.")
	endif()
	if(NOT JCE_BUILD_VARIANT STREQUAL "dist")
		return()
	endif()

	target_compile_definitions(${TARGET} PRIVATE JCE_DIST=1)
	set_target_properties(${TARGET} PROPERTIES
		C_VISIBILITY_PRESET hidden
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN YES)

	include(CheckIPOSupported)
	check_ipo_supported(RESULT _jce_ipo_ok OUTPUT _jce_ipo_error)
	if(_jce_ipo_ok)
		set_target_properties(${TARGET} PROPERTIES
			INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
			INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)
	else()
		message(WARNING
			"JCE dist: IPO/LTO unavailable for ${TARGET}: ${_jce_ipo_error}")
	endif()

	if(WIN32)
		# GUI subsystem: the shipped application opens no terminal window.
		set_target_properties(${TARGET} PROPERTIES WIN32_EXECUTABLE TRUE)
	endif()

	if(MSVC)
		target_compile_options(${TARGET} PRIVATE
			$<$<CONFIG:Release>:/GS>
			$<$<CONFIG:Release>:/guard:cf>
			$<$<CONFIG:Release>:/Gy>
			$<$<CONFIG:Release>:/Gw>)
		target_link_options(${TARGET} PRIVATE
			$<$<CONFIG:Release>:/DYNAMICBASE>
			$<$<CONFIG:Release>:/NXCOMPAT>
			$<$<CONFIG:Release>:/HIGHENTROPYVA>
			$<$<CONFIG:Release>:/GUARD:CF>
			$<$<CONFIG:Release>:/OPT:REF>
			$<$<CONFIG:Release>:/OPT:ICF>
			$<$<CONFIG:Release>:/INCREMENTAL:NO>)
	elseif(APPLE)
		target_compile_options(${TARGET} PRIVATE
			$<$<CONFIG:Release>:-fstack-protector-strong>)
		target_link_options(${TARGET} PRIVATE
			$<$<CONFIG:Release>:LINKER:-dead_strip>)
	elseif(UNIX AND NOT EMSCRIPTEN)
		target_compile_options(${TARGET} PRIVATE
			$<$<CONFIG:Release>:-fstack-protector-strong>)
		target_link_options(${TARGET} PRIVATE
			$<$<CONFIG:Release>:LINKER:-z,relro>
			$<$<CONFIG:Release>:LINKER:-z,now>
			$<$<CONFIG:Release>:LINKER:-z,noexecstack>)
	endif()
endfunction()

# ------------------------------------------------------------------ #
# _jce_embed_pak_key(<target>)                                        #
#                                                                     #
# Links the embedded asset-decryption key TU into <target> when the   #
# editor-driven build provides one through                            #
# JCE_PROJECT_PREBUILT_PAK_KEY_C (jce_generated/jce_pak_key.c, two    #
# XOR shares regenerated per build).  No target-local stub is linked   #
# otherwise: the SDK dependency archive carries a zero fallback member #
# that is linked normally, outside the force-loaded engine core.  A     #
# generated target object resolves the symbols first, so the fallback   #
# member is not extracted.                                              #
# Idempotent per target.                                              #
# ------------------------------------------------------------------ #
function(_jce_embed_pak_key TARGET)
	get_target_property(_done ${TARGET} JCE_PAK_KEY_LINKED)
	if(_done)
		return()
	endif()
	set_target_properties(${TARGET} PROPERTIES JCE_PAK_KEY_LINKED TRUE)

	if(DEFINED JCE_PROJECT_PREBUILT_PAK_KEY_C AND
	   EXISTS "${JCE_PROJECT_PREBUILT_PAK_KEY_C}")
		set_source_files_properties("${JCE_PROJECT_PREBUILT_PAK_KEY_C}"
			PROPERTIES GENERATED TRUE)
		target_sources(${TARGET} PRIVATE "${JCE_PROJECT_PREBUILT_PAK_KEY_C}")
	endif()
endfunction()

# ------------------------------------------------------------------ #
# _jce_prepare_runtime_boot_manifest(<target> <project-json> <out-dir>
#                                    <out-file>)                     #
#                                                                     #
# Shipping applications boot from their embedded PAK, not from the   #
# authoring-only project files staged beside an executable.  Emit the #
# narrow boot contract and, when authored, the runtime input map into #
# reserved virtual paths under one generated resource root.           #
# ------------------------------------------------------------------ #
function(_jce_prepare_runtime_boot_manifest TARGET PROJECT_FILE OUT_DIR OUT_FILE)

	set(${OUT_DIR} "" PARENT_SCOPE)
	set(${OUT_FILE} "" PARENT_SCOPE)
	if(NOT EXISTS "${PROJECT_FILE}")
		return()
	endif()

	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${PROJECT_FILE}")
	file(READ "${PROJECT_FILE}" _project_json)
	string(JSON _startup_scene ERROR_VARIABLE _boot_error
		GET "${_project_json}" startup_scene)
	if(_boot_error)
		if(_boot_error MATCHES "member.*not found")
			set(_startup_scene "")
		else()
			message(FATAL_ERROR
				"jce_add_pak: cannot read startup_scene from ${PROJECT_FILE}: "
				"${_boot_error}")
		endif()
	endif()

	# Escape JSON syntax here; the runtime parser remains the authority for
	# virtual-path validity when it reads the packed manifest at boot.
	set(_scene_json "${_startup_scene}")
	string(REPLACE "\\" "\\\\" _scene_json "${_scene_json}")
	string(REPLACE "\"" "\\\"" _scene_json "${_scene_json}")
	string(REPLACE "\r" "\\r" _scene_json "${_scene_json}")
	string(REPLACE "\n" "\\n" _scene_json "${_scene_json}")

	set(_boot_dir "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_runtime_boot")
	set(_boot_file "${_boot_dir}/jce/runtime_boot.json")
	file(MAKE_DIRECTORY "${_boot_dir}/jce")
	file(WRITE "${_boot_file}"
		"{\n"
		"  \"contract\": \"jce.runtime_boot\",\n"
		"  \"schema\": 1,\n"
		"  \"startup_scene\": \"${_scene_json}\"\n"
		"}\n")

	get_filename_component(_project_dir "${PROJECT_FILE}" DIRECTORY)
	set(_input_actions "${_project_dir}/.jce/input_actions.json")
	if(EXISTS "${_input_actions}")
		set(_input_actions_out
			"${_boot_dir}/settings/input_actions.json")
		file(MAKE_DIRECTORY "${_boot_dir}/settings")
		configure_file("${_input_actions}" "${_input_actions_out}" COPYONLY)
	endif()

	set(${OUT_DIR} "${_boot_dir}" PARENT_SCOPE)
	set(${OUT_FILE} "${_boot_file}" PARENT_SCOPE)
endfunction()

function(jce_target_embed_pak TARGET)
	if(NOT TARGET ${TARGET})
		message(FATAL_ERROR "jce_target_embed_pak: '${TARGET}' is not a target.")
	endif()

	# Always resolve the embedded asset-key externs (generated TU or stub),
	# regardless of which assets path below is taken.
	_jce_embed_pak_key(${TARGET})

	set(_opts NO_ENGINE_RESOURCES STRIP_DEBUG_PATHS)
	set(_one  PAK_FILE SYMBOL_PREFIX)
	set(_multi RESOURCE_DIRS EXCLUDE_SEGMENTS EXTRA_DEPENDS)
	cmake_parse_arguments(EP "${_opts}" "${_one}" "${_multi}" ${ARGN})

	if(NOT EP_SYMBOL_PREFIX)
		# Default matches the engine's expected externs:
		# `assets_pak_data` + `assets_pak_data_size`.
		set(EP_SYMBOL_PREFIX "assets_pak_data")
	endif()

	if(DEFINED JCE_PROJECT_PREBUILT_ASSETS_OBJ AND
	   EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_OBJ}")
		if(NOT EP_SYMBOL_PREFIX STREQUAL "assets_pak_data")
			message(FATAL_ERROR
				"jce_target_embed_pak: editor prebuilt assets use "
				"symbol prefix assets_pak_data, but '${TARGET}' requested "
				"'${EP_SYMBOL_PREFIX}'.")
		endif()
		set_source_files_properties("${JCE_PROJECT_PREBUILT_ASSETS_OBJ}"
			PROPERTIES
				GENERATED       TRUE
				EXTERNAL_OBJECT TRUE)
		target_sources(${TARGET} PRIVATE "${JCE_PROJECT_PREBUILT_ASSETS_OBJ}")
		add_custom_target(${TARGET}_pak
			DEPENDS "${JCE_PROJECT_PREBUILT_ASSETS_OBJ}")
		add_dependencies(${TARGET} ${TARGET}_pak)
		return()
	endif()

	if(DEFINED JCE_PROJECT_PREBUILT_ASSETS_ASM AND
	   EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_ASM}")
		if(NOT EP_SYMBOL_PREFIX STREQUAL "assets_pak_data")
			message(FATAL_ERROR
				"jce_target_embed_pak: editor prebuilt assets use "
				"symbol prefix assets_pak_data, but '${TARGET}' requested "
				"'${EP_SYMBOL_PREFIX}'.")
		endif()
		set_source_files_properties("${JCE_PROJECT_PREBUILT_ASSETS_ASM}"
			PROPERTIES GENERATED TRUE)
		target_sources(${TARGET} PRIVATE "${JCE_PROJECT_PREBUILT_ASSETS_ASM}")
		add_custom_target(${TARGET}_pak
			DEPENDS "${JCE_PROJECT_PREBUILT_ASSETS_ASM}")
		add_dependencies(${TARGET} ${TARGET}_pak)
		return()
	endif()

	if(DEFINED JCE_PROJECT_PREBUILT_ASSETS_C AND
	   EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_C}")
		if(NOT EP_SYMBOL_PREFIX STREQUAL "assets_pak_data")
			message(FATAL_ERROR
				"jce_target_embed_pak: editor prebuilt assets use "
				"symbol prefix assets_pak_data, but '${TARGET}' requested "
				"'${EP_SYMBOL_PREFIX}'.")
		endif()
		set_source_files_properties("${JCE_PROJECT_PREBUILT_ASSETS_C}"
			PROPERTIES GENERATED TRUE)
		target_sources(${TARGET} PRIVATE "${JCE_PROJECT_PREBUILT_ASSETS_C}")
		add_custom_target(${TARGET}_pak
			DEPENDS "${JCE_PROJECT_PREBUILT_ASSETS_C}")
		add_dependencies(${TARGET} ${TARGET}_pak)
		return()
	endif()

	if(JCE_BUILD_VARIANT STREQUAL "dist")
		message(FATAL_ERROR
			"jce_target_embed_pak: dist requires the editor's authenticated "
			"prebuilt PAK and key-share source. Build/package the project "
			"through JCE Editor; manual raw-resource packing is available for "
			"debug/release only.")
	endif()

	if(NOT JCE_PAK_EXECUTABLE)
		message(FATAL_ERROR
			"jce_target_embed_pak: no prebuilt assets were provided "
			"(JCE_PROJECT_PREBUILT_ASSETS_OBJ/ASM/C), and "
			"JCE_PAK_EXECUTABLE is not set. Build through the JCE "
			"editor, or provide a host packer explicitly for manual "
			"CMake builds.")
	endif()

	if(NOT EP_RESOURCE_DIRS)
		message(FATAL_ERROR "jce_target_embed_pak: RESOURCE_DIRS is required.")
	endif()

	# Prepend the engine's stock resource trees (RML HUDs, fallback font,
	# default shaders, license file) shipped with the SDK so the engine
	# can find them at boot.  Caller can opt out with NO_ENGINE_RESOURCES
	# for fully self-contained dist builds.
	if(NOT EP_NO_ENGINE_RESOURCES)
		set(_engine_dirs)
		if(DEFINED JCE_ENGINE_RESOURCES_DIR AND IS_DIRECTORY "${JCE_ENGINE_RESOURCES_DIR}")
			list(APPEND _engine_dirs "${JCE_ENGINE_RESOURCES_DIR}")
		endif()
		if(DEFINED JCE_ENGINE_UI_DIR AND IS_DIRECTORY "${JCE_ENGINE_UI_DIR}")
			list(APPEND _engine_dirs "${JCE_ENGINE_UI_DIR}")
		endif()
		if(_engine_dirs)
			list(PREPEND EP_RESOURCE_DIRS ${_engine_dirs})
		endif()
	endif()

	if(NOT EP_PAK_FILE)
		set(EP_PAK_FILE "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_assets.pak")
	endif()

	# Per-dir CLI flags (+ resolved dir list for the file-level deps below).
	set(_res_flags)
	set(_res_dirs_abs)
	foreach(_d IN LISTS EP_RESOURCE_DIRS)
		if(NOT IS_ABSOLUTE "${_d}")
			set(_d "${CMAKE_CURRENT_SOURCE_DIR}/${_d}")
		endif()
		list(APPEND _res_flags --resource-dir "${_d}")
		list(APPEND _res_dirs_abs "${_d}")
	endforeach()

	set(_excl_flags)
	foreach(_s IN LISTS EP_EXCLUDE_SEGMENTS)
		list(APPEND _excl_flags --exclude-segment "${_s}")
	endforeach()

	# Shader bytecode is backend-specific. Use the shared profile matrix
	# (jce_shader_profiles above — the same one the engine build uses) so an
	# SDK consumer does not embed unreachable ESSL/Metal/etc. binaries merely
	# because the SDK resource tree contains every platform's precompiled
	# variant. Callers may override the selected set with
	# JCE_PAK_SHADER_PROFILES (for example "dx11;spv" on a D3D/Vulkan-only app).
	set(_shader_profile_args)
	if(DEFINED JCE_PAK_SHADER_PROFILES)
		set(_shader_profile_args PROFILES ${JCE_PAK_SHADER_PROFILES})
	endif()
	jce_shader_pak_exclude_flags(_shader_exclude_flags ${_shader_profile_args})

	jce_configure_application_target(${TARGET})

	# Release packages use hash-only archive indices. Keep debug strings for
	# Debug builds unless the caller explicitly requests stripping them.
	set(_debug_path_flags)
	if(EP_STRIP_DEBUG_PATHS OR
	   CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
		list(APPEND _debug_path_flags --strip-debug-paths)
	endif()

	# Repack when any packed FILE changes — not just when the packer exe
	# does. Without these deps an edited texture re-cooked by jce_add_pak()
	# (or an edited raw resource) never dirtied the .pak: the only recorded
	# dependency was JCE_PAK_EXECUTABLE, so Ninja happily reused a stale
	# pak/obj and the exe shipped old assets. The in-tree pipeline lists
	# every asset file in DEPENDS for exactly this reason. EXTRA_DEPENDS
	# lets callers add ordering files (jce_add_pak passes its cook stamp).
	set(_pak_deps)
	foreach(_d IN LISTS _res_dirs_abs)
		file(GLOB_RECURSE _dir_files CONFIGURE_DEPENDS "${_d}/*")
		list(APPEND _pak_deps ${_dir_files})
	endforeach()
	if(EP_EXTRA_DEPENDS)
		list(APPEND _pak_deps ${EP_EXTRA_DEPENDS})
	endif()

	# MSVC: COFF .obj that we link directly.  Other compilers: a .S
	# wrapper using `.incbin` that target_sources() will compile.
	if(MSVC)
		set(_obj_format "coff")
		set(_obj_ext    ".obj")
	else()
		set(_obj_format "asm-incbin")
		set(_obj_ext    ".S")
	endif()
	set(_obj_file      "${EP_PAK_FILE}${_obj_ext}")
	set(_header_file   "${EP_PAK_FILE}.h")
	set(_manifest_file "${EP_PAK_FILE}.manifest.json")
	set(_reports_dir   "${CMAKE_CURRENT_BINARY_DIR}/reports")
	set(_bom_file      "${_reports_dir}/${TARGET}_assets.pak.bom.json")
	file(MAKE_DIRECTORY "${_reports_dir}")

	# Emscripten: do NOT embed the PAK as an object.  Pack it (obj-format
	# defaults to "none" when --obj-file/--obj-format are omitted), preload it
	# into the MEMFS via --preload-file, and link a 1-byte stub providing the
	# assets_pak_data / _size externs the engine references unconditionally
	# (JCE_PLATFORM_WEB is a CMake var, never a -D macro).
	if(EMSCRIPTEN)
		add_custom_command(
			OUTPUT  "${EP_PAK_FILE}" "${_bom_file}"
			COMMAND "${JCE_PAK_EXECUTABLE}"
				${_res_flags}
				${_excl_flags}
				${_shader_exclude_flags}
				${_debug_path_flags}
				--pak-file       "${EP_PAK_FILE}"
				--header-file    "${_header_file}"
				--manifest-file  "${_manifest_file}"
				--json           "${_bom_file}"
				--symbol-prefix  "${EP_SYMBOL_PREFIX}"
			DEPENDS "${JCE_PAK_EXECUTABLE}" ${_pak_deps}
			COMMENT "Packing ${TARGET} assets (wasm) -> ${EP_PAK_FILE}"
			VERBATIM)

		add_custom_target(${TARGET}_pak DEPENDS "${EP_PAK_FILE}" "${_bom_file}")
		add_dependencies(${TARGET} ${TARGET}_pak)

		target_link_options(${TARGET} PRIVATE
			"SHELL:--preload-file ${EP_PAK_FILE}@/game_assets.pak")
		set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${EP_PAK_FILE}")

		# 1-byte stub: the engine links against assets_pak_data / _size even on
		# web (the real PAK is loaded from MEMFS at /game_assets.pak).
		set(_wasm_stub "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_wasm_pak_stub.c")
		file(WRITE "${_wasm_stub}"
			"const unsigned char ${EP_SYMBOL_PREFIX}[1] = {0};\n"
			"const unsigned long ${EP_SYMBOL_PREFIX}_size = 0;\n")
		target_sources(${TARGET} PRIVATE "${_wasm_stub}")
		return()
	endif()

	# Pick a COFF arch flag for MSVC so the linker accepts it for x86/x64/arm64.
	set(_obj_arch_flags)
	if(MSVC)
		if(CMAKE_SIZEOF_VOID_P EQUAL 8)
			if(CMAKE_SYSTEM_PROCESSOR MATCHES "(ARM64|aarch64)")
				list(APPEND _obj_arch_flags --obj-arch arm64)
			else()
				list(APPEND _obj_arch_flags --obj-arch x64)
			endif()
		else()
			list(APPEND _obj_arch_flags --obj-arch x86)
		endif()
	endif()

	add_custom_command(
		OUTPUT  "${EP_PAK_FILE}" "${_obj_file}" "${_header_file}" "${_manifest_file}" "${_bom_file}"
		COMMAND "${JCE_PAK_EXECUTABLE}"
			${_res_flags}
			${_excl_flags}
			${_shader_exclude_flags}
			${_debug_path_flags}
			--pak-file       "${EP_PAK_FILE}"
			--obj-file       "${_obj_file}"
			--header-file    "${_header_file}"
			--manifest-file  "${_manifest_file}"
			--json           "${_bom_file}"
			--obj-format     "${_obj_format}"
			--symbol-prefix  "${EP_SYMBOL_PREFIX}"
			${_obj_arch_flags}
		DEPENDS "${JCE_PAK_EXECUTABLE}" ${_pak_deps}
		COMMENT "Packing assets for ${TARGET} -> ${EP_PAK_FILE}"
		VERBATIM)

	if(MSVC)
		set_source_files_properties("${_obj_file}" PROPERTIES
			GENERATED        TRUE
			EXTERNAL_OBJECT  TRUE)
		target_sources(${TARGET} PRIVATE "${_obj_file}")
	else()
		set_source_files_properties("${_obj_file}" PROPERTIES
			GENERATED TRUE)
		target_sources(${TARGET} PRIVATE "${_obj_file}")
	endif()

	# Force the custom command to run as a dependency of the target.
	add_custom_target(${TARGET}_pak DEPENDS "${EP_PAK_FILE}" "${_obj_file}" "${_bom_file}")
	add_dependencies(${TARGET} ${TARGET}_pak)
endfunction()


# ------------------------------------------------------------------ #
# jce_target_embed_bundle(<target>
#     BUNDLE <path>                                                   #
#     [SYMBOL <ident>])     # default: bundle_<sanitised basename>    #
#                                                                     #
# Wraps a pre-built `.jbundle` file into a COFF .obj (MSVC) or GAS    #
# .S (everyone else) and links it into <target>.  At runtime the      #
# template main.c declares                                             #
#     extern const unsigned char <SYMBOL>[];                          #
#     extern const size_t       <SYMBOL>_size;                        #
# and feeds them to jce_bundle_file_open_memory() so the shipped exe  #
# is a single self-contained file — no sidecar `bundles/` directory.  #
#                                                                     #
# Editor-driven builds consume pre-generated C sources from           #
# JCE_PROJECT_PREBUILT_BUNDLE_DIR.  Manual SDK consumers may still    #
# provide JCE_BIN2OBJ_EXECUTABLE for host-side wrapping.               #
# ------------------------------------------------------------------ #

function(jce_target_embed_bundle TARGET)
	if(NOT TARGET ${TARGET})
		message(FATAL_ERROR "jce_target_embed_bundle: '${TARGET}' is not a target.")
	endif()

	set(_opts)
	set(_one  BUNDLE SYMBOL)
	set(_multi)
	cmake_parse_arguments(EB "${_opts}" "${_one}" "${_multi}" ${ARGN})

	if(NOT EB_BUNDLE)
		message(FATAL_ERROR "jce_target_embed_bundle: BUNDLE <path> is required.")
	endif()
	if(NOT IS_ABSOLUTE "${EB_BUNDLE}")
		set(EB_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/${EB_BUNDLE}")
	endif()
	if(NOT EXISTS "${EB_BUNDLE}")
		message(FATAL_ERROR "jce_target_embed_bundle: bundle not found: ${EB_BUNDLE}")
	endif()

	get_filename_component(_bname "${EB_BUNDLE}" NAME_WE)
	if(NOT EB_SYMBOL)
		# Sanitize basename → valid C identifier, prefixed with bundle_.
		string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _sym "${_bname}")
		set(EB_SYMBOL "bundle_${_sym}")
	endif()

	if(DEFINED JCE_PROJECT_PREBUILT_BUNDLE_DIR AND
	   IS_DIRECTORY "${JCE_PROJECT_PREBUILT_BUNDLE_DIR}")
		set(_prebuilt "")
		set(_prebuilt_is_obj OFF)
		if(MSVC)
			set(_candidate "${JCE_PROJECT_PREBUILT_BUNDLE_DIR}/_embed_bundle_${EB_SYMBOL}.obj")
			if(EXISTS "${_candidate}")
				set(_prebuilt "${_candidate}")
				set(_prebuilt_is_obj ON)
			endif()
		else()
			set(_candidate "${JCE_PROJECT_PREBUILT_BUNDLE_DIR}/_embed_bundle_${EB_SYMBOL}.S")
			if(EXISTS "${_candidate}")
				set(_prebuilt "${_candidate}")
			endif()
		endif()
		if(NOT _prebuilt)
			set(_candidate "${JCE_PROJECT_PREBUILT_BUNDLE_DIR}/_embed_bundle_${EB_SYMBOL}.c")
			if(EXISTS "${_candidate}")
				set(_prebuilt "${_candidate}")
			endif()
		endif()
		if(_prebuilt)
			set_source_files_properties("${_prebuilt}" PROPERTIES GENERATED TRUE)
			if(_prebuilt_is_obj)
				set_source_files_properties("${_prebuilt}"
					PROPERTIES EXTERNAL_OBJECT TRUE)
			endif()
			target_sources(${TARGET} PRIVATE "${_prebuilt}")
			add_custom_target(${TARGET}_embed_${EB_SYMBOL}
				DEPENDS "${_prebuilt}")
			add_dependencies(${TARGET} ${TARGET}_embed_${EB_SYMBOL})
			return()
		endif()
	endif()

	if(NOT JCE_BIN2OBJ_EXECUTABLE)
		message(FATAL_ERROR
			"jce_target_embed_bundle: no prebuilt bundle source was found "
			"for ${EB_SYMBOL}, and JCE_BIN2OBJ_EXECUTABLE is not set. "
			"Build through the JCE editor, or provide a host wrapper "
			"explicitly for manual CMake builds.")
	endif()

	# Pick output format + arch per toolchain (mirrors jce_target_embed_pak).
	if(MSVC)
		set(_fmt "coff")
		set(_ext ".obj")
	else()
		set(_fmt "asm")
		set(_ext ".S")
	endif()
	set(_arch_flags)
	if(MSVC)
		if(CMAKE_SIZEOF_VOID_P EQUAL 8)
			if(CMAKE_SYSTEM_PROCESSOR MATCHES "(ARM64|aarch64)")
				list(APPEND _arch_flags --arch arm64)
			else()
				list(APPEND _arch_flags --arch x64)
			endif()
		else()
			list(APPEND _arch_flags --arch x86)
		endif()
	endif()

	set(_out "${CMAKE_CURRENT_BINARY_DIR}/_embed_bundle_${EB_SYMBOL}${_ext}")

	add_custom_command(
		OUTPUT  "${_out}"
		COMMAND "${JCE_BIN2OBJ_EXECUTABLE}"
			--input  "${EB_BUNDLE}"
			--symbol "${EB_SYMBOL}"
			--output "${_out}"
			--format "${_fmt}"
			${_arch_flags}
		DEPENDS "${JCE_BIN2OBJ_EXECUTABLE}" "${EB_BUNDLE}"
		COMMENT "Embedding bundle ${EB_BUNDLE} -> ${EB_SYMBOL}"
		VERBATIM)

	if(MSVC)
		set_source_files_properties("${_out}" PROPERTIES
			GENERATED        TRUE
			EXTERNAL_OBJECT  TRUE)
	else()
		set_source_files_properties("${_out}" PROPERTIES
			GENERATED TRUE)
	endif()
	target_sources(${TARGET} PRIVATE "${_out}")

	add_custom_target(${TARGET}_embed_${EB_SYMBOL} DEPENDS "${_out}")
	add_dependencies(${TARGET} ${TARGET}_embed_${EB_SYMBOL})
endfunction()


# ------------------------------------------------------------------ #
# jce_add_pak(<target>                                                #
#     RESOURCE_DIRS  <dir> [<dir>...]   # raw asset dirs to cook+pack #
#     [PAK_FILE      <path>]                                          #
#     [SYMBOL_PREFIX <symbol>]          # default: assets_pak_data    #
#     [EXCLUDE_SEGMENTS <seg> [...]]                                  #
#     [NO_ENGINE_RESOURCES]             # don't prepend SDK engine res#
#     [STRIP_DEBUG_PATHS]                # omit virtual-path table      #
#     [NO_COOK]                         # pack raw, skip cooking      #
#     [COOK_LEVEL <0-22>]               # per-asset zstd (default 0)  #
#     [MAX_TEXTURE_SIZE <N>]            # default 2048                #
#     [COOK_PLATFORM <p>])              # jce_cook --platform         #
#                                                                     #
# Turnkey raw-asset → cooked → PAK → embed for SDK consumers that do  #
# NOT go through the packaged editor.  Resolution order:             #
#                                                                     #
#   1. Editor prebuilt assets present (JCE_PROJECT_PREBUILT_ASSETS_*) #
#      → delegate to jce_target_embed_pak() unchanged (already cooked)#
#   2. JCE_COOK_EXECUTABLE available and not NO_COOK                  #
#      → jce_cook each RESOURCE_DIR (+ engine resources) into one     #
#        merged <target>_cooked tree, then pack+embed that tree.      #
#   3. Otherwise → pack the raw dirs (jce_target_embed_pak) + warn.   #
#                                                                     #
# The cook step mirrors the in-tree pipeline (root CMakeLists cook →  #
# pack → embed) so standalone builds match first-party output.       #
# ------------------------------------------------------------------ #

function(jce_add_pak TARGET)
	if(NOT TARGET ${TARGET})
		message(FATAL_ERROR "jce_add_pak: '${TARGET}' is not a target.")
	endif()

	set(_opts  NO_ENGINE_RESOURCES NO_COOK STRIP_DEBUG_PATHS)
	set(_one   PAK_FILE SYMBOL_PREFIX COOK_LEVEL MAX_TEXTURE_SIZE COOK_PLATFORM)
	set(_multi RESOURCE_DIRS EXCLUDE_SEGMENTS EXTRA_COOK_ARGS EXTRA_DEPENDS)
	cmake_parse_arguments(AP "${_opts}" "${_one}" "${_multi}" ${ARGN})

	# ---- 1. Editor path: prebuilt assets already cooked + packed. --- #
	if((DEFINED JCE_PROJECT_PREBUILT_ASSETS_OBJ AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_OBJ}") OR
	   (DEFINED JCE_PROJECT_PREBUILT_ASSETS_ASM AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_ASM}") OR
	   (DEFINED JCE_PROJECT_PREBUILT_ASSETS_C   AND EXISTS "${JCE_PROJECT_PREBUILT_ASSETS_C}"))
		jce_target_embed_pak(${TARGET} ${ARGN})
		return()
	endif()

	if(NOT AP_RESOURCE_DIRS)
		message(FATAL_ERROR "jce_add_pak: RESOURCE_DIRS is required.")
	endif()

	# Authoring manifests are not runtime assets: produce the compact boot
	# manifest in a separate resource root so both raw and cooked SDK builds
	# pack it under its reserved virtual path without leaking SDK metadata.
	_jce_prepare_runtime_boot_manifest(${TARGET}
		"${CMAKE_CURRENT_SOURCE_DIR}/jce_project.json"
		_runtime_boot_dir _runtime_boot_file)
	if(_runtime_boot_dir)
		list(APPEND AP_RESOURCE_DIRS "${_runtime_boot_dir}")
	endif()

	# ---- 3. No cooker (or NO_COOK): pack raw, warn. ----------------- #
	if(AP_NO_COOK OR NOT JCE_COOK_EXECUTABLE)
		if(NOT AP_NO_COOK AND NOT JCE_COOK_EXECUTABLE)
			message(WARNING
				"jce_add_pak: JCE_COOK_EXECUTABLE not found; packing RAW "
				"assets (textures/audio will NOT be pre-decoded).  Install "
				"the SDK with JCE_ENABLE_SDK_INSTALL=ON to ship jce_cook.")
		endif()
		set(_raw_embed_args RESOURCE_DIRS ${AP_RESOURCE_DIRS})
		if(AP_PAK_FILE)
			list(APPEND _raw_embed_args PAK_FILE "${AP_PAK_FILE}")
		endif()
		if(AP_SYMBOL_PREFIX)
			list(APPEND _raw_embed_args SYMBOL_PREFIX "${AP_SYMBOL_PREFIX}")
		endif()
		if(AP_EXCLUDE_SEGMENTS)
			list(APPEND _raw_embed_args EXCLUDE_SEGMENTS ${AP_EXCLUDE_SEGMENTS})
		endif()
		if(AP_NO_ENGINE_RESOURCES)
			list(APPEND _raw_embed_args NO_ENGINE_RESOURCES)
		endif()
		if(AP_STRIP_DEBUG_PATHS)
			list(APPEND _raw_embed_args STRIP_DEBUG_PATHS)
		endif()
		if(_runtime_boot_file)
			list(APPEND _raw_embed_args EXTRA_DEPENDS "${_runtime_boot_file}")
		endif()
		if(AP_EXTRA_DEPENDS)
			list(APPEND _raw_embed_args EXTRA_DEPENDS ${AP_EXTRA_DEPENDS})
		endif()
		jce_target_embed_pak(${TARGET} ${_raw_embed_args})
		return()
	endif()

	# ---- 2. Cook path: jce_cook each dir into one merged tree. ------ #
	if(NOT DEFINED AP_COOK_LEVEL)
		set(AP_COOK_LEVEL 0)
	endif()
	if(NOT DEFINED AP_MAX_TEXTURE_SIZE)
		set(AP_MAX_TEXTURE_SIZE 2048)
	endif()
	set(_cook_platform_args "")
	if(DEFINED AP_COOK_PLATFORM)
		set(_cook_platform_args --platform "${AP_COOK_PLATFORM}")
	endif()

	# Resolve resource dirs to absolute; prepend optional SDK engine resources
	# and the RML UI tree unless opted out, so they are
	# cooked into the same tree (matches the in-tree pipeline).
	set(_src_dirs "")
	if(NOT AP_NO_ENGINE_RESOURCES)
		if(DEFINED JCE_ENGINE_RESOURCES_DIR AND IS_DIRECTORY "${JCE_ENGINE_RESOURCES_DIR}")
			list(APPEND _src_dirs "${JCE_ENGINE_RESOURCES_DIR}")
		endif()
		if(DEFINED JCE_ENGINE_UI_DIR AND IS_DIRECTORY "${JCE_ENGINE_UI_DIR}")
			list(APPEND _src_dirs "${JCE_ENGINE_UI_DIR}")
		endif()
	endif()
	foreach(_d IN LISTS AP_RESOURCE_DIRS)
		if(NOT IS_ABSOLUTE "${_d}")
			set(_d "${CMAKE_CURRENT_SOURCE_DIR}/${_d}")
		endif()
		list(APPEND _src_dirs "${_d}")
	endforeach()

	set(_cooked_dir "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_cooked")
	set(_cook_stamp "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_cook.stamp")

	# Track raw inputs so the cook re-runs when assets change.
	set(_cook_inputs "")
	foreach(_d IN LISTS _src_dirs)
		file(GLOB_RECURSE _files CONFIGURE_DEPENDS "${_d}/*")
		list(APPEND _cook_inputs ${_files})
	endforeach()
	if(AP_EXTRA_DEPENDS)
		list(APPEND _cook_inputs ${AP_EXTRA_DEPENDS})
	endif()

	# One jce_cook --batch per source dir, all merged into _cooked_dir.
	#
	# Each batch gets its OWN incremental catalog (--catalog) kept in the build
	# dir, NOT the default <out>/.jce_cook_catalog.  jce_cook runs a per-run
	# stale-GC that deletes any output file whose source wasn't seen in THIS
	# batch; with a single shared catalog in the merged output dir, the 2nd…Nth
	# batch would delete the 1st batch's files (their sources live in a different
	# source dir), so a multi-dir merged cook kept only the LAST batch's output
	# (e.g. dropping every scenes/chunks/*.scene.json fragment).  A per-dir
	# catalog scopes the GC to that dir's own outputs, so the merge is additive.
	set(_cook_cmds
		COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_cooked_dir}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cooked_dir}")
	set(_cook_idx 0)
	foreach(_d IN LISTS _src_dirs)
		set(_cook_catalog
			"${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_cook_catalog_${_cook_idx}.bin")
		list(APPEND _cook_cmds
			COMMAND "${JCE_COOK_EXECUTABLE}"
				--batch "${_d}" "${_cooked_dir}"
				--catalog "${_cook_catalog}"
				--preserve-names
				--level "${AP_COOK_LEVEL}"
				--max-texture-size "${AP_MAX_TEXTURE_SIZE}"
				${_cook_platform_args}
				${AP_EXTRA_COOK_ARGS})
		math(EXPR _cook_idx "${_cook_idx}+1")
	endforeach()

	# Settings S2: stage the authored render-pipeline asset into the cooked
	# tree (PAK key settings/render_pipeline.rp.json) so shipped single-exe
	# games get their authored quality settings — the runtime re-resolves it
	# after bundle mount (jce_render_pipeline_apply_boot_mounted).
	set(_rp_asset "${CMAKE_CURRENT_SOURCE_DIR}/Settings/RenderPipeline.rp.json")
	if(EXISTS "${_rp_asset}")
		list(APPEND _cook_cmds
			COMMAND "${CMAKE_COMMAND}" -E make_directory
				"${_cooked_dir}/settings"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${_rp_asset}" "${_cooked_dir}/settings/render_pipeline.rp.json")
		list(APPEND _cook_inputs "${_rp_asset}")
	endif()

	add_custom_command(
		OUTPUT  "${_cook_stamp}"
		${_cook_cmds}
		COMMAND "${CMAKE_COMMAND}" -E touch "${_cook_stamp}"
		DEPENDS "${JCE_COOK_EXECUTABLE}" ${_cook_inputs}
		COMMENT "Cooking assets for ${TARGET} → ${_cooked_dir}"
		VERBATIM)
	add_custom_target(${TARGET}_cook DEPENDS "${_cook_stamp}")

	# Pack + embed the cooked tree.  NO_ENGINE_RESOURCES: the engine dirs
	# were already cooked into _cooked_dir above (don't double-add raw).
	# EXTRA_DEPENDS carries the cook stamp into the pak command's DEPENDS:
	# add_dependencies() below only ORDERS the two targets — without a real
	# file-level edge a re-cook never dirtied the .pak and the exe shipped
	# stale assets.
	set(_embed_args RESOURCE_DIRS "${_cooked_dir}" NO_ENGINE_RESOURCES
	                EXTRA_DEPENDS "${_cook_stamp}")
	if(AP_PAK_FILE)
		list(APPEND _embed_args PAK_FILE "${AP_PAK_FILE}")
	endif()
	if(AP_SYMBOL_PREFIX)
		list(APPEND _embed_args SYMBOL_PREFIX "${AP_SYMBOL_PREFIX}")
	endif()
	if(AP_EXCLUDE_SEGMENTS)
		list(APPEND _embed_args EXCLUDE_SEGMENTS ${AP_EXCLUDE_SEGMENTS})
	endif()
	if(AP_STRIP_DEBUG_PATHS)
		list(APPEND _embed_args STRIP_DEBUG_PATHS)
	endif()
	jce_target_embed_pak(${TARGET} ${_embed_args})

	# The pak command reads _cooked_dir (a directory input, not a tracked
	# file), so force cooking to finish first — mirrors the in-tree
	# PackGameAssets ← CookGameAssets dependency.
	if(TARGET ${TARGET}_pak)
		add_dependencies(${TARGET}_pak ${TARGET}_cook)
	endif()
endfunction()
