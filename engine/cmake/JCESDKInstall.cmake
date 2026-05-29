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

	if(NOT MSVC)
		message(STATUS "JCE SDK: non-MSVC host — fat-lib merge skipped (later phase).")
		return()
	endif()

	jce_collect_static_libs(JCE _jce_sdk)

	# --- Belt-and-suspenders: capture sibling abseil libs ---------- #
	# The walker only reaches absl targets the engine *directly* uses.
	# Some absl libs (e.g. absl_log_internal_fnmatch) ship the MSVC STL
	# vectorised helpers (__std_find_first_of_trivial_pos_1, …) that
	# abseil's compiled TUs reference but the local MSVC toolchain's
	# CRT does not export.  If we miss them, the consumer link of the
	# fat lib fails with LNK2019.  Grab every absl_*.lib that sits next
	# to the libs we already collected.
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

	# ---- SDK shim libs (deps-bound, generator-expression paths) ----- #
	# Owned-but-not-core: extra static libs we author that should land  #
	# in the *deps* fat lib so they are linked NORMALLY (not under     #
	# /WHOLEARCHIVE).  Today this is the MSVC STL helper backport — see #
	# engine/src/sdk_shims/jce_msvc_stl_shims.cpp.                      #
	set(_deps_extra_targets "")
	if(TARGET jce_msvc_stl_shims)
		list(APPEND _deps_extra_targets jce_msvc_stl_shims)
	endif()
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

	add_custom_target(jce_sdk_fat_lib ALL
		DEPENDS "${_fat_lib_core}" "${_fat_lib_deps}")

	include(GNUInstallDirs)
	include(CMakePackageConfigHelpers)

	install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/jce"
		DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
		FILES_MATCHING
			PATTERN "*.h"
			PATTERN "*.hpp"
			PATTERN "*.inl")

	install(FILES "${CMAKE_BINARY_DIR}/include/jce/jce_version.h"
		DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/jce")

	install(FILES "${_fat_lib_core}" "${_fat_lib_deps}"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")

	# ---- jce_pak (host packer) — required by end-user projects to bake
	# their own asset PAK at build time.
	if(TARGET jce_pak)
		install(TARGETS jce_pak
			RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
	endif()
	# ---- jce_bin2obj (host binary→COFF/asm wrapper) — required by
	# jce_target_embed_bundle() to bake .jbundle archives into the exe
	# for true single-file consumer builds.
	if(TARGET jce_bin2obj)
		install(TARGETS jce_bin2obj
			RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
	endif()

	# ---- Engine-side runtime resources that the engine *always* expects
	# to find in the PAK at boot (HUD/settings RML, fallback fonts,
	# default shaders, …).  Consumer projects rarely override these so
	# we ship them with the SDK and have jce_target_embed_pak() add them
	# to every consumer PAK automatically.
	set(_engine_share_root "${CMAKE_INSTALL_DATAROOTDIR}/jce")
	if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/engine/resources/assets")
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine/resources/assets/"
			DESTINATION "${_engine_share_root}/engine_resources"
			PATTERN "raw_assets" EXCLUDE)
	endif()
	if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/engine/ui")
		install(DIRECTORY "${CMAKE_SOURCE_DIR}/engine/ui/"
			DESTINATION "${_engine_share_root}/engine_ui")
	endif()
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
			DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/JCE")
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
		# MSVC-only branch (gated above) so the static-lib suffix is .lib.
		install(CODE [[
			set(_redundant_lib "${CMAKE_INSTALL_PREFIX}/lib/fdk-aac.lib")
			if(EXISTS "${_redundant_lib}")
				message(STATUS "JCE SDK: removing redundant ${_redundant_lib}")
				file(REMOVE "${_redundant_lib}")
			endif()
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
