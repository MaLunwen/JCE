# JCESDKHelpers.cmake
#
# Convenience helpers exposed to end-user projects that consume the
# pre-built JCE SDK via `find_package(JCE REQUIRED)`.
#
# Shipped to <sdk>/lib/cmake/JCE/ and auto-included by JCEConfig.cmake.
#
# Public:
#   jce_target_embed_pak(<target>
#       RESOURCE_DIRS  <dir> [<dir>...]
#       [PAK_FILE      <path>]                  # default: <bin>/<target>_assets.pak
#       [SYMBOL_PREFIX <symbol>]                # default: assets
#       [EXCLUDE_SEGMENTS <seg> [<seg>...]])    # forwarded to jce_pak
#
# Builds a PAK from the listed resource dirs using `${JCE_PAK_EXECUTABLE}`
# and links it into <target> so the engine can mount it at boot.
#
# On MSVC the packer emits a COFF .obj that we link directly.  On
# GNU/Clang/Apple targets the packer emits a tiny .S wrapper that
# `.incbin`s the raw PAK; we add the .S to the target sources.
#
# The embedded symbols are: <SYMBOL_PREFIX>_pak_data (uint8_t[]) and
# <SYMBOL_PREFIX>_pak_data_size (uint32_t).  The engine's bootstrap
# looks for "assets" by default.

include_guard(GLOBAL)

function(jce_target_embed_pak TARGET)
	if(NOT TARGET ${TARGET})
		message(FATAL_ERROR "jce_target_embed_pak: '${TARGET}' is not a target.")
	endif()
	if(NOT JCE_PAK_EXECUTABLE)
		message(FATAL_ERROR
			"jce_target_embed_pak: JCE_PAK_EXECUTABLE is not set. "
			"The SDK at '${PACKAGE_PREFIX_DIR}' does not ship jce_pak — "
			"rebuild the SDK with -DJCE_ENABLE_SDK_INSTALL=ON.")
	endif()

	set(_opts NO_ENGINE_RESOURCES)
	set(_one  PAK_FILE SYMBOL_PREFIX)
	set(_multi RESOURCE_DIRS EXCLUDE_SEGMENTS)
	cmake_parse_arguments(EP "${_opts}" "${_one}" "${_multi}" ${ARGN})

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
	if(NOT EP_SYMBOL_PREFIX)
		# Default matches the engine's expected externs:
		# `assets_pak_data` + `assets_pak_data_size`.
		set(EP_SYMBOL_PREFIX "assets_pak_data")
	endif()

	# Per-dir CLI flags.
	set(_res_flags)
	foreach(_d IN LISTS EP_RESOURCE_DIRS)
		if(NOT IS_ABSOLUTE "${_d}")
			set(_d "${CMAKE_CURRENT_SOURCE_DIR}/${_d}")
		endif()
		list(APPEND _res_flags --resource-dir "${_d}")
	endforeach()

	set(_excl_flags)
	foreach(_s IN LISTS EP_EXCLUDE_SEGMENTS)
		list(APPEND _excl_flags --exclude-segment "${_s}")
	endforeach()

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
		OUTPUT  "${EP_PAK_FILE}" "${_obj_file}" "${_header_file}" "${_manifest_file}"
		COMMAND "${JCE_PAK_EXECUTABLE}"
			${_res_flags}
			${_excl_flags}
			--pak-file       "${EP_PAK_FILE}"
			--obj-file       "${_obj_file}"
			--header-file    "${_header_file}"
			--manifest-file  "${_manifest_file}"
			--obj-format     "${_obj_format}"
			--symbol-prefix  "${EP_SYMBOL_PREFIX}"
			${_obj_arch_flags}
		DEPENDS "${JCE_PAK_EXECUTABLE}"
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
	add_custom_target(${TARGET}_pak DEPENDS "${EP_PAK_FILE}" "${_obj_file}")
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
# Requires JCE_BIN2OBJ_EXECUTABLE (set by JCEConfig.cmake when the    #
# SDK was built with -DJCE_ENABLE_SDK_INSTALL=ON).                    #
# ------------------------------------------------------------------ #

function(jce_target_embed_bundle TARGET)
	if(NOT TARGET ${TARGET})
		message(FATAL_ERROR "jce_target_embed_bundle: '${TARGET}' is not a target.")
	endif()
	if(NOT JCE_BIN2OBJ_EXECUTABLE)
		message(FATAL_ERROR
			"jce_target_embed_bundle: JCE_BIN2OBJ_EXECUTABLE is not set. "
			"The SDK does not ship jce_bin2obj — rebuild the SDK with "
			"-DJCE_ENABLE_SDK_INSTALL=ON.")
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



