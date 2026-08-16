# engine/cmake/JCESDKInstall.cmake
#
# SDK packaging support — when JCE_ENABLE_SDK_INSTALL is ON, this module
# installs the engine in a form that an end-user CMake project can consume
# via `find_package(JCE REQUIRED)` without needing Conan or the JCE source
# tree.
#
# Strategy: walk the transitive link closure of the JCE INTERFACE facade,
# collect every static library file path, then merge them into a single
# fat static lib `jce_engine_full`. End-user code links one library.
#
# Public entry points:
#   jce_collect_static_libs(<root_target> <out_var>)
#   jce_register_sdk_install()    — invoked from engine/CMakeLists.txt
#

include_guard(GLOBAL)

# ------------------------------------------------------------------ #
# jce_collect_static_libs — recursively walk LINK_LIBRARIES of a     #
# target, returning the set of static library targets and absolute   #
# file paths of imported static libs in the transitive closure.      #
# ------------------------------------------------------------------ #
function(jce_collect_static_libs ROOT_TARGET OUT_VAR)
	set(_visited "")
	set(_static_targets "")
	set(_imported_files "")
	set(_pending "${ROOT_TARGET}")

	while(_pending)
		list(GET _pending 0 _t)
		list(REMOVE_AT _pending 0)

		if(_t IN_LIST _visited)
			continue()
		endif()
		list(APPEND _visited "${_t}")

		if(NOT TARGET "${_t}")
			continue()
		endif()

		get_target_property(_type "${_t}" TYPE)

		if(_type STREQUAL "STATIC_LIBRARY")
			get_target_property(_imp "${_t}" IMPORTED)
			if(_imp)
				set(_loc "")
				if(CMAKE_BUILD_TYPE)
					string(TOUPPER "${CMAKE_BUILD_TYPE}" _cfg_u)
					get_target_property(_loc "${_t}" "IMPORTED_LOCATION_${_cfg_u}")
				endif()
				if(NOT _loc)
					get_target_property(_loc "${_t}" IMPORTED_LOCATION_RELEASE)
				endif()
				if(NOT _loc)
					get_target_property(_loc "${_t}" IMPORTED_LOCATION)
				endif()
				if(NOT _loc)
					get_target_property(_configs "${_t}" IMPORTED_CONFIGURATIONS)
					if(_configs)
						list(GET _configs 0 _c)
						get_target_property(_loc "${_t}" "IMPORTED_LOCATION_${_c}")
					endif()
				endif()
				if(_loc)
					list(APPEND _imported_files "${_loc}")
				endif()
			else()
				list(APPEND _static_targets "${_t}")
			endif()
		endif()

		foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
			get_target_property(_deps "${_t}" "${_prop}")
			if(NOT _deps)
				continue()
			endif()

			# Conan emits properties like
			#   $<$<CONFIG:Release>:libyuv::libyuv;libaom-av1::libaom-av1;...>
			# which CMake splits on ';' into broken fragments before we see them.
			# Rejoin and resolve $<$<CONFIG:X>:...> blocks ourselves so the inner
			# target list survives.
			set(_cfg_u "")
			if(CMAKE_BUILD_TYPE)
				string(TOUPPER "${CMAKE_BUILD_TYPE}" _cfg_u)
			endif()
			string(JOIN ";" _joined ${_deps})
			# Iteratively peel innermost $<$<CONFIG:X>:CONTENT> wrappers.
			while(_joined MATCHES "\\\$<\\\$<CONFIG:([A-Za-z]+)>:([^<>]*)>")
				set(_gate "${CMAKE_MATCH_1}")
				set(_inner "${CMAKE_MATCH_2}")
				string(TOUPPER "${_gate}" _gate_u)
				set(_replacement "")
				if(_cfg_u STREQUAL "" OR _gate_u STREQUAL "${_cfg_u}")
					set(_replacement "${_inner}")
				endif()
				string(REPLACE "\$<\$<CONFIG:${_gate}>:${_inner}>" "${_replacement}" _joined "${_joined}")
			endwhile()
			# Strip $<LINK_ONLY:X> → X.
			string(REGEX REPLACE "\\\$<LINK_ONLY:([^>]+)>" "\\1" _joined "${_joined}")
			# Strip $<BUILD_INTERFACE:X> → X (CONFIG-style wrapper sometimes appears
			# from Conan's link directives).
			string(REGEX REPLACE "\\\$<BUILD_INTERFACE:([^>]+)>" "\\1" _joined "${_joined}")

			foreach(_d IN LISTS _joined)
				if(NOT _d)
					continue()
				endif()
				# Anything still genex-wrapped: skip silently.
				if(_d MATCHES "^\\\$<.*>$")
					continue()
				endif()
				if(_d MATCHES "\\.(lib|a)$" AND IS_ABSOLUTE "${_d}")
					list(APPEND _imported_files "${_d}")
				else()
					list(APPEND _pending "${_d}")
				endif()
			endforeach()
		endforeach()
	endwhile()

	# ai_dispatch links libcurl PRIVATE (engine/CMakeLists.txt) so the
	# public-closure walk above never reaches it.  Merge its static lib
	# explicitly, otherwise a private-SDK consumer cannot resolve curl_*.
	if(JCE_ENABLE_AI_DISPATCH AND TARGET CURL::libcurl)
		get_target_property(_curl_loc CURL::libcurl IMPORTED_LOCATION_RELEASE)
		if(NOT _curl_loc)
			get_target_property(_curl_loc CURL::libcurl IMPORTED_LOCATION)
		endif()
		if(_curl_loc AND EXISTS "${_curl_loc}")
			list(APPEND _imported_files "${_curl_loc}")
		endif()
	endif()
	list(REMOVE_DUPLICATES _static_targets)
	list(REMOVE_DUPLICATES _imported_files)

	if(JCE_SDK_DEBUG_WALKER)
		list(SORT _visited)
		list(LENGTH _visited _nv)
		message(STATUS "JCE SDK walker: visited ${_nv} targets:")
		foreach(_v IN LISTS _visited)
			message(STATUS "  - ${_v}")
		endforeach()
	endif()

	set("${OUT_VAR}_TARGETS" "${_static_targets}" PARENT_SCOPE)
	set("${OUT_VAR}_FILES"   "${_imported_files}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------ #
# jce_register_sdk_install                                            #
# ------------------------------------------------------------------ #
function(jce_register_sdk_install)
	if(NOT JCE_ENABLE_SDK_INSTALL)
		return()
	endif()

	if(NOT MSVC AND NOT APPLE AND NOT UNIX)
		message(STATUS "JCE SDK: unsupported host — fat-lib merge skipped.")
		return()
	endif()

	jce_collect_static_libs(JCE _jce_sdk)

	# Libraries in this set must retain ordinary archive extraction semantics.
	# In particular, jce_pak_key_defaults provides zero-valued fallback symbols
	# only when an application did not compile a generated protected-PAK key TU.
	# Merging it into the force-loaded core archive would create duplicate strong
	# definitions in protected Dist applications.
	set(_deps_extra_targets jce_pak_key_defaults)
	foreach(_t IN LISTS _deps_extra_targets)
		list(REMOVE_ITEM _jce_sdk_TARGETS "${_t}")
	endforeach()

	# --- Belt-and-suspenders: capture sibling abseil libs (MSVC only) ---- #
	# On MSVC, some absl libs ship vectorised STL helpers
	# (__std_find_first_of_trivial_pos_1, …) that abseil TUs reference but
	# the MSVC CRT doesn't export.  If we miss them the consumer link fails
	# with LNK2019.  Grab every absl_*.lib next to the already-collected
	# imported libs.  GCC/Clang expose these symbols from the standard
	# library automatically; no extra glob needed on Linux/macOS.
	if(MSVC)
		set(_extra_libs "")
		foreach(_f IN LISTS _jce_sdk_FILES)
			get_filename_component(_d "${_f}" DIRECTORY)
			file(GLOB _siblings "${_d}/absl_*.lib" "${_d}/absl_*.a")
			list(APPEND _extra_libs ${_siblings})
		endforeach()
		if(_extra_libs)
			list(APPEND _jce_sdk_FILES ${_extra_libs})
			list(REMOVE_DUPLICATES _jce_sdk_FILES)
		endif()
	endif()

	list(LENGTH _jce_sdk_TARGETS _n_owned)
	list(LENGTH _jce_sdk_FILES   _n_imp)
	message(STATUS "JCE SDK: owned=${_n_owned}, imported=${_n_imp}")

	set(_merge_dir "${CMAKE_CURRENT_BINARY_DIR}/sdk_merge")
	file(MAKE_DIRECTORY "${_merge_dir}")

	# Write imported (already-resolved) library paths into a response file
	# so the merge command line stays well under cmd.exe's 8 KB limit and
	# avoids quote-escaping issues. One path per line.
	set(_rsp_imported "${_merge_dir}/imported_libs.rsp")
	set(_rsp_content "")
	foreach(_f IN LISTS _jce_sdk_FILES)
		string(APPEND _rsp_content "\"${_f}\"\n")
	endforeach()
	file(WRITE "${_rsp_imported}" "${_rsp_content}")

	# Owned libs use generator-expressions (paths only known at build time)
	# so they must stay on the command line.  We write them via file(GENERATE)
	# into a per-config response file so the merge driver can ingest them
	# without semicolon-list-expansion issues that COMMAND_EXPAND_LISTS
	# would inflict when the list is passed inline.
	set(_owned_rsp "${_merge_dir}/$<CONFIG>/owned_libs.rsp")
	set(_owned_genex_content "")
	foreach(_t IN LISTS _jce_sdk_TARGETS)
		string(APPEND _owned_genex_content "\"$<TARGET_FILE:${_t}>\"\n")
	endforeach()
	file(GENERATE OUTPUT "${_owned_rsp}" CONTENT "${_owned_genex_content}")

	# Empty placeholder rsp — the merge driver requires both files to exist
	# even when one half is intentionally empty (core vs deps split).
	set(_empty_rsp "${_merge_dir}/empty.rsp")
	file(WRITE "${_empty_rsp}" "")

	# ---- Normally linked first-party fallback libs ------------------- #
	# These targets are merged with dependencies, not the force-loaded core,
	# so their members are extracted only to satisfy unresolved symbols.
	set(_deps_extra_rsp "${_merge_dir}/$<CONFIG>/deps_extra.rsp")
	set(_deps_extra_content "")
	foreach(_t IN LISTS _deps_extra_targets)
		string(APPEND _deps_extra_content "\"$<TARGET_FILE:${_t}>\"\n")
	endforeach()
	file(GENERATE OUTPUT "${_deps_extra_rsp}" CONTENT "${_deps_extra_content}")

	# We produce TWO fat libs instead of one:                              #
	#                                                                      #
	#   jce_engine_core — first-party JCE TUs only.  The SDK consumer      #
	#       force-links this via /WHOLEARCHIVE so dead-strip can't drop    #
	#       RmlUI bindings, SDL_main, the event pump, or any TU that       #
	#       self-registers via static initialisers.                        #
	#                                                                      #
	#   jce_engine_deps — every imported 3rd-party static lib.  Linked     #
	#       normally so unused 3rd-party tools (e.g. protoc compiler TUs   #
	#       under protobuf) stay dead-stripped and don't pull unresolved   #
	#       symbols (upb_*) into the user exe.                             #
	set(_fat_lib_core "${_merge_dir}/$<CONFIG>/jce_engine_core${CMAKE_STATIC_LIBRARY_SUFFIX}")
	set(_fat_lib_deps "${_merge_dir}/$<CONFIG>/jce_engine_deps${CMAKE_STATIC_LIBRARY_SUFFIX}")

	if(MSVC)
		# Drive lib.exe via a tiny wrapper that resolves paths into a single
		# response file — avoids Ninja's quoting weirdness with /OUT:"...".
		set(_driver "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jce_sdk_merge_libs.cmake")

		add_custom_command(
			OUTPUT  "${_fat_lib_core}"
			COMMAND "${CMAKE_COMMAND}"
				-DLIB_EXE=${CMAKE_AR}
				-DOUT_LIB=${_fat_lib_core}
				-DRSP_IMPORTED=${_empty_rsp}
				-DRSP_OWNED=${_owned_rsp}
				-P "${_driver}"
			DEPENDS ${_jce_sdk_TARGETS} "${_empty_rsp}" "${_owned_rsp}" "${_driver}"
			VERBATIM)

		add_custom_command(
			OUTPUT  "${_fat_lib_deps}"
			COMMAND "${CMAKE_COMMAND}"
				-DLIB_EXE=${CMAKE_AR}
				-DOUT_LIB=${_fat_lib_deps}
				-DRSP_IMPORTED=${_rsp_imported}
				-DRSP_OWNED=${_deps_extra_rsp}
				-P "${_driver}"
			DEPENDS "${_rsp_imported}" "${_deps_extra_rsp}" "${_driver}"
				${_deps_extra_targets}
			VERBATIM)

	elseif(EMSCRIPTEN)
		# Emscripten: emar (CMAKE_AR) extract + re-archive.  emar does not
		# honour `-M` MRI scripts, so jce_sdk_merge_libs_unix.cmake takes an
		# explicit extract+qc path when JCE_SDK_EMSCRIPTEN=1.
		message(STATUS "JCE SDK: emscripten fat-lib merge (emar) branch")
		set(_unix_driver "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jce_sdk_merge_libs_unix.cmake")

		add_custom_command(
			OUTPUT  "${_fat_lib_core}"
			COMMAND "${CMAKE_COMMAND}"
				-DAR_EXE=${CMAKE_AR}
				-DOUT_LIB=${_fat_lib_core}
				-DRSP_IMPORTED=${_empty_rsp}
				-DRSP_OWNED=${_owned_rsp}
				-DJCE_SDK_APPLE=0
				-DJCE_SDK_EMSCRIPTEN=1
				-P "${_unix_driver}"
			DEPENDS ${_jce_sdk_TARGETS} "${_empty_rsp}" "${_owned_rsp}" "${_unix_driver}"
			VERBATIM)

		add_custom_command(
			OUTPUT  "${_fat_lib_deps}"
			COMMAND "${CMAKE_COMMAND}"
				-DAR_EXE=${CMAKE_AR}
				-DOUT_LIB=${_fat_lib_deps}
				-DRSP_IMPORTED=${_rsp_imported}
				-DRSP_OWNED=${_deps_extra_rsp}
				-DJCE_SDK_APPLE=0
				-DJCE_SDK_EMSCRIPTEN=1
				-P "${_unix_driver}"
			DEPENDS "${_rsp_imported}" "${_deps_extra_rsp}" "${_unix_driver}"
			VERBATIM)

	elseif(APPLE OR UNIX)
		# macOS: libtool -static.  Linux: ar MRI script.
		# Both are handled by jce_sdk_merge_libs_unix.cmake.
		set(_unix_driver "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jce_sdk_merge_libs_unix.cmake")
		set(_is_apple "0")
		if(APPLE)
			set(_is_apple "1")
		endif()

		add_custom_command(
			OUTPUT  "${_fat_lib_core}"
			COMMAND "${CMAKE_COMMAND}"
				-DAR_EXE=${CMAKE_AR}
				-DOUT_LIB=${_fat_lib_core}
				-DRSP_IMPORTED=${_empty_rsp}
				-DRSP_OWNED=${_owned_rsp}
				-DJCE_SDK_APPLE=${_is_apple}
				-P "${_unix_driver}"
			DEPENDS ${_jce_sdk_TARGETS} "${_empty_rsp}" "${_owned_rsp}" "${_unix_driver}"
			VERBATIM)

		add_custom_command(
			OUTPUT  "${_fat_lib_deps}"
			COMMAND "${CMAKE_COMMAND}"
				-DAR_EXE=${CMAKE_AR}
				-DOUT_LIB=${_fat_lib_deps}
				-DRSP_IMPORTED=${_rsp_imported}
				-DRSP_OWNED=${_deps_extra_rsp}
				-DJCE_SDK_APPLE=${_is_apple}
				-P "${_unix_driver}"
			DEPENDS "${_rsp_imported}" "${_deps_extra_rsp}" "${_unix_driver}"
			VERBATIM)

	else()
		message(WARNING "JCE SDK: unrecognised toolchain — fat-lib merge skipped.")
		return()
	endif()

	add_custom_target(jce_sdk_fat_lib ALL
		DEPENDS "${_fat_lib_core}" "${_fat_lib_deps}")

	include(GNUInstallDirs)
	include(CMakePackageConfigHelpers)

	# ai_dispatch is a PRIVATE module (spec C.8): its headers must never ride
	# an SDK/editor package unless this build explicitly enabled the module.
	# Without these excludes the directory GLOB below ships the full ABI
	# surface (api_ai_dispatch.h + middleware/ai_dispatch/) in every SDK.
	set(_jce_sdk_hdr_excludes "")
	if(NOT JCE_ENABLE_AI_DISPATCH)
		list(APPEND _jce_sdk_hdr_excludes
			PATTERN "api_ai_dispatch.h" EXCLUDE
			PATTERN "middleware/ai_dispatch" EXCLUDE)
	endif()

	install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/jce"
		DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
		FILES_MATCHING
			PATTERN "*.h"
			PATTERN "*.hpp"
			PATTERN "*.inl"
			${_jce_sdk_hdr_excludes})

	# install(DIRECTORY) does not prune headers removed from the source tree.
	# Delete the old public copy now that the bgfx encoder shim is internal.
	install(CODE
		"file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/jce/renderer/jce_render_encoder.h\")")

	install(FILES "${CMAKE_BINARY_DIR}/include/jce/jce_version.h"
		DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/jce")

	install(FILES "${_fat_lib_core}" "${_fat_lib_deps}"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")

	if(TARGET jce_msvc_stl_shims)
		install(TARGETS jce_msvc_stl_shims
			ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")
	endif()

	# Host-side cook/pack/wrap tools (jce_cook / jce_pak / jce_bin2obj)
	# are installed into <sdk>/bin/ from the root CMakeLists.txt when
	# JCE_ENABLE_SDK_INSTALL is ON, so a no-editor CMake/CLI consumer can
	# run the full raw → cooked → PAK → embed pipeline via jce_add_pak().
	# The packaged editor still cooks/packs in-process and feeds
	# JCE_PROJECT_PREBUILT_ASSETS_* for editor-driven builds.

	# Engine shaders are already authenticated and embedded in JCE::JCE.  Do not
	# install a second loose copy: it bloats every consumer PAK and can preserve
	# stale backend profiles across incremental SDK installs.  Remove only this
	# SDK-managed legacy directory; engine UI remains a normal consumer asset.
	set(_engine_share_root "${CMAKE_INSTALL_DATAROOTDIR}/jce")
	install(CODE
		"file(REMOVE_RECURSE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_engine_share_root}/engine_resources\")")
	if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/engine/ui")
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine/ui/"
			DESTINATION "${_engine_share_root}/engine_ui")
	endif()

	# Project-authored shaders are compiled by the SDK with the same shaderc,
	# profile matrix and lint module as the engine build. Only the stable bgfx
	# shader ABI includes are installed; project source remains project-owned.
	set(_jce_shaderc_candidates
		"${bgfx_PACKAGE_FOLDER_RELEASE}/bin/shaderc${CMAKE_EXECUTABLE_SUFFIX}"
		"${bgfx_PACKAGE_FOLDER_DEBUG}/bin/shaderc${CMAKE_EXECUTABLE_SUFFIX}")
	foreach(_jce_shaderc IN LISTS _jce_shaderc_candidates)
		if(_jce_shaderc AND EXISTS "${_jce_shaderc}")
			install(PROGRAMS "${_jce_shaderc}"
				DESTINATION "${CMAKE_INSTALL_BINDIR}")
			break()
		endif()
	endforeach()
	if(BGFX_SHADER_INCLUDE_PATH AND IS_DIRECTORY "${BGFX_SHADER_INCLUDE_PATH}")
		install(DIRECTORY "${BGFX_SHADER_INCLUDE_PATH}/"
			DESTINATION "${_engine_share_root}/shader_include"
			FILES_MATCHING PATTERN "*.sh")
	endif()
	install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine/shaders/include/"
		DESTINATION "${_engine_share_root}/shader_include"
		FILES_MATCHING PATTERN "*.sh")
	# Project-scaffolding templates consumed by jce_project_create_from_template().
	# Editor's "New Project" walks share/jce/templates/<tpl_name>/...
	if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/engine/templates")
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine/templates/"
			DESTINATION "${_engine_share_root}/templates")
	endif()
	if(EXISTS "${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md")
		install(FILES "${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md"
			DESTINATION "${_engine_share_root}")
	endif()

	set(_pkg_config_in "${CMAKE_SOURCE_DIR}/cmake/JCEConfig.cmake.in")
	if(EXISTS "${_pkg_config_in}")
		configure_package_config_file(
			"${_pkg_config_in}"
			"${CMAKE_CURRENT_BINARY_DIR}/JCEConfig.cmake"
			INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/JCE")

		write_basic_package_version_file(
			"${CMAKE_CURRENT_BINARY_DIR}/JCEConfigVersion.cmake"
			VERSION "${PROJECT_VERSION}"
			COMPATIBILITY SameMajorVersion)

		install(FILES
				"${CMAKE_CURRENT_BINARY_DIR}/JCEConfig.cmake"
				"${CMAKE_CURRENT_BINARY_DIR}/JCEConfigVersion.cmake"
				"${CMAKE_SOURCE_DIR}/cmake/JCESDKHelpers.cmake"
				"${CMAKE_SOURCE_DIR}/tools/shader_lint.py"
			DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/JCE")
		install(FILES "${CMAKE_SOURCE_DIR}/tools/compile_shaders.cmake"
			DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/JCE"
			RENAME JCECompileShaders.cmake)
	else()
		message(WARNING "JCE SDK: ${_pkg_config_in} missing.")
	endif()

	# ---- Clean up redundant standalone artefacts ------------------- #
	# The vendored fdk-aac CMakeLists.txt carries its own                #
	# `install(TARGETS fdk-aac …)` + header install rules that fire      #
	# whenever the upstream subdirectory is added.  We don't want those: #
	# fdk-aac is already merged into jce_engine_core${SUFFIX} (see       #
	# _owned_rsp above), and consumer projects only ever consume AAC     #
	# through `<jce/api_audio.h>`, never fdk-aac directly.  Stripping    #
	# the standalone lib + headers shaves ~6 MB and prevents accidental  #
	# double-link / pulled-in CRT symbol collisions on the consumer end. #
	if(JCE_PATENTED_CODECS_ENABLED)
		# fdk-aac's own CMakeLists.txt fires install(TARGETS fdk-aac …) and
		# header install rules whenever we add_subdirectory it.  Strip those
		# artefacts: fdk-aac is already inside jce_engine_core/deps, and
		# consumers only reach it through <jce/api_audio.h>.
		install(CODE [[
			foreach(_redundant_lib
					"${CMAKE_INSTALL_PREFIX}/lib/fdk-aac.lib"
					"${CMAKE_INSTALL_PREFIX}/lib/libfdk-aac.a")
				if(EXISTS "${_redundant_lib}")
					message(STATUS "JCE SDK: removing redundant ${_redundant_lib}")
					file(REMOVE "${_redundant_lib}")
				endif()
			endforeach()
			set(_redundant_inc "${CMAKE_INSTALL_PREFIX}/include/fdk-aac")
			if(IS_DIRECTORY "${_redundant_inc}")
				message(STATUS "JCE SDK: removing redundant ${_redundant_inc}")
				file(REMOVE_RECURSE "${_redundant_inc}")
			endif()
			set(_redundant_cmake "${CMAKE_INSTALL_PREFIX}/lib/cmake/fdk-aac")
			if(IS_DIRECTORY "${_redundant_cmake}")
				message(STATUS "JCE SDK: removing redundant ${_redundant_cmake}")
				file(REMOVE_RECURSE "${_redundant_cmake}")
			endif()
			set(_redundant_pc "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig/fdk-aac.pc")
			if(EXISTS "${_redundant_pc}")
				message(STATUS "JCE SDK: removing redundant ${_redundant_pc}")
				file(REMOVE "${_redundant_pc}")
			endif()
		]])
	endif()

	message(STATUS "JCE SDK: install rules registered (prefix=${CMAKE_INSTALL_PREFIX})")
endfunction()
