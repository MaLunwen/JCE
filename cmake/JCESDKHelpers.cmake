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
#       [SYMBOL_PREFIX <symbol>]                # default: assets_pak_data
#       [EXCLUDE_SEGMENTS <seg> [<seg>...]])
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
# _jce_embed_pak_key(<target>)                                        #
#                                                                     #
# Links the embedded asset-decryption key TU into <target>.  Editor-  #
# driven builds pass the generated source through                     #
# JCE_PROJECT_PREBUILT_PAK_KEY_C (jce_generated/jce_pak_key.c, two    #
# XOR shares regenerated per build).  When absent, a zeroed stub is   #
# linked instead — the exact assets_pak_data stub pattern — so the    #
# engine's jce_embedded_pak_key_present extern always resolves from   #
# an exe-level object and unencrypted projects behave unchanged.      #
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
		return()
	endif()

	set(_key_stub "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_pak_key_stub.c")
	file(WRITE "${_key_stub}"
		"/* Auto-generated: no asset encryption key for this build. */\n"
		"const unsigned char jce_embedded_pak_key_shares[64] = {0};\n"
		"const int           jce_embedded_pak_key_present    = 0;\n")
	target_sources(${TARGET} PRIVATE "${_key_stub}")
endfunction()

function(jce_target_embed_pak TARGET)
	if(NOT TARGET ${TARGET})
		message(FATAL_ERROR "jce_target_embed_pak: '${TARGET}' is not a target.")
	endif()

	# Always resolve the embedded asset-key externs (generated TU or stub),
	# regardless of which assets path below is taken.
	_jce_embed_pak_key(${TARGET})

	set(_opts NO_ENGINE_RESOURCES)
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

	# Emscripten: do NOT embed the PAK as an object.  Pack it (obj-format
	# defaults to "none" when --obj-file/--obj-format are omitted), preload it
	# into the MEMFS via --preload-file, and link a 1-byte stub providing the
	# assets_pak_data / _size externs the engine references unconditionally
	# (JCE_PLATFORM_WEB is a CMake var, never a -D macro).
	if(EMSCRIPTEN)
		add_custom_command(
			OUTPUT  "${EP_PAK_FILE}"
			COMMAND "${JCE_PAK_EXECUTABLE}"
				${_res_flags}
				${_excl_flags}
				--pak-file       "${EP_PAK_FILE}"
				--header-file    "${_header_file}"
				--manifest-file  "${_manifest_file}"
				--symbol-prefix  "${EP_SYMBOL_PREFIX}"
			DEPENDS "${JCE_PAK_EXECUTABLE}" ${_pak_deps}
			COMMENT "Packing ${TARGET} assets (wasm) -> ${EP_PAK_FILE}"
			VERBATIM)

		add_custom_target(${TARGET}_pak DEPENDS "${EP_PAK_FILE}")
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

	set(_opts  NO_ENGINE_RESOURCES NO_COOK)
	set(_one   PAK_FILE SYMBOL_PREFIX COOK_LEVEL MAX_TEXTURE_SIZE COOK_PLATFORM)
	set(_multi RESOURCE_DIRS EXCLUDE_SEGMENTS EXTRA_COOK_ARGS)
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

	# ---- 3. No cooker (or NO_COOK): pack raw, warn. ----------------- #
	if(AP_NO_COOK OR NOT JCE_COOK_EXECUTABLE)
		if(NOT AP_NO_COOK AND NOT JCE_COOK_EXECUTABLE)
			message(WARNING
				"jce_add_pak: JCE_COOK_EXECUTABLE not found; packing RAW "
				"assets (textures/audio will NOT be pre-decoded).  Install "
				"the SDK with JCE_ENABLE_SDK_INSTALL=ON to ship jce_cook.")
		endif()
		jce_target_embed_pak(${TARGET} ${ARGN})
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

	# Resolve resource dirs to absolute; prepend the SDK engine resource
	# trees (RML HUDs, fallback font, …) unless opted out, so they are
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

	# One jce_cook --batch per source dir, all merged into _cooked_dir.
	set(_cook_cmds
		COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_cooked_dir}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cooked_dir}")
	foreach(_d IN LISTS _src_dirs)
		list(APPEND _cook_cmds
			COMMAND "${JCE_COOK_EXECUTABLE}"
				--batch "${_d}" "${_cooked_dir}"
				--preserve-names
				--level "${AP_COOK_LEVEL}"
				--max-texture-size "${AP_MAX_TEXTURE_SIZE}"
				${_cook_platform_args}
				${AP_EXTRA_COOK_ARGS})
	endforeach()

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
	jce_target_embed_pak(${TARGET} ${_embed_args})

	# The pak command reads _cooked_dir (a directory input, not a tracked
	# file), so force cooking to finish first — mirrors the in-tree
	# PackGameAssets ← CookGameAssets dependency.
	if(TARGET ${TARGET}_pak)
		add_dependencies(${TARGET}_pak ${TARGET}_cook)
	endif()
endfunction()
