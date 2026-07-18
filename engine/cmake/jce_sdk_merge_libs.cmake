# jce_sdk_merge_libs.cmake — driver invoked at build time to call lib.exe
# with a clean argument list. Avoids Ninja's escaping issues that arise
# when /OUT:"path-with-quotes" is passed directly.
#
# Inputs (set via -D on the command line):
#   LIB_EXE       — path to lib.exe (CMAKE_AR for MSVC)
#   OUT_LIB       — output .lib path
#   RSP_IMPORTED  — response file with already-resolved imported lib paths
#   RSP_OWNED     — response file with build-time-resolved owned lib paths
#                   (generated via file(GENERATE) at configure time, populated
#                   with $<TARGET_FILE:...> entries that resolve per-config).

if(NOT LIB_EXE OR NOT OUT_LIB OR NOT RSP_IMPORTED OR NOT RSP_OWNED)
	message(FATAL_ERROR "jce_sdk_merge_libs: required vars LIB_EXE/OUT_LIB/RSP_IMPORTED/RSP_OWNED")
endif()

# Compose a single response file with everything lib.exe needs.
# The rsp filename derives from OUT_LIB so concurrent invocations
# (jce_engine_core.lib + jce_engine_deps.lib in parallel) don't race.
get_filename_component(_out_dir "${OUT_LIB}" DIRECTORY)
get_filename_component(_out_stem "${OUT_LIB}" NAME_WE)
file(MAKE_DIRECTORY "${_out_dir}")

set(_full_rsp "${_out_dir}/${_out_stem}.rsp")
set(_content "/NOLOGO\n/OUT:\"${OUT_LIB}\"\n")

# Owned libs (paths resolved by file(GENERATE) at parent scope).
if(EXISTS "${RSP_OWNED}")
	file(READ "${RSP_OWNED}" _owned)
	string(APPEND _content "${_owned}")
endif()

# Imported libs come from a pre-written response file.
if(EXISTS "${RSP_IMPORTED}")
	file(READ "${RSP_IMPORTED}" _imp)
	string(APPEND _content "${_imp}")
endif()

file(WRITE "${_full_rsp}" "${_content}")

# lib.exe updates an existing archive in place and keeps members that are no
# longer present in the input libraries.  A fat SDK archive must be a snapshot
# of the current link closure, so rebuild it from an empty destination every
# time.  This also prevents removed or renamed engine TUs from surviving an
# incremental SDK build.
file(REMOVE "${OUT_LIB}")

execute_process(
	COMMAND "${LIB_EXE}" "@${_full_rsp}"
	RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
	message(FATAL_ERROR "jce_sdk_merge_libs: lib.exe failed (rc=${_rc}). "
		"Response file: ${_full_rsp}")
endif()

# ------------------------------------------------------------------ #
# Strip embedded Win32 .res members.                                  #
# Third-party libs (assimp, freetype, harfbuzz, …) often ship product-
# version resource members.  When user code force-loads the merged fat
# lib via /WHOLEARCHIVE the linker pulls all of them in and dies with
# `LNK1241: resource file ... already specified`.  The engine itself
# never uses those .res payloads, so strip them before install.
# ------------------------------------------------------------------ #
execute_process(
	COMMAND "${LIB_EXE}" "/NOLOGO" "/LIST" "${OUT_LIB}"
	OUTPUT_VARIABLE _members
	OUTPUT_STRIP_TRAILING_WHITESPACE
	RESULT_VARIABLE _list_rc)

if(NOT _list_rc EQUAL 0)
	message(WARNING "jce_sdk_merge_libs: /LIST failed (rc=${_list_rc}); "
		"cannot strip .res members.")
	return()
endif()

string(REPLACE "\n" ";" _member_list "${_members}")
set(_res_members "")
foreach(_m IN LISTS _member_list)
	string(STRIP "${_m}" _m)
	if(_m MATCHES "\\.res$")
		list(APPEND _res_members "${_m}")
	endif()
endforeach()

list(LENGTH _res_members _n_res)
if(_n_res GREATER 0)
	message(STATUS "jce_sdk_merge_libs: stripping ${_n_res} .res member(s) "
		"from ${OUT_LIB}")
	# lib.exe accepts multiple /REMOVE: in one invocation.
	set(_remove_args "/NOLOGO")
	foreach(_r IN LISTS _res_members)
		list(APPEND _remove_args "/REMOVE:${_r}")
	endforeach()
	execute_process(
		COMMAND "${LIB_EXE}" ${_remove_args} "${OUT_LIB}"
		RESULT_VARIABLE _rm_rc)
	if(NOT _rm_rc EQUAL 0)
		message(WARNING "jce_sdk_merge_libs: .res strip failed (rc=${_rm_rc}). "
			"Force-linking the fat lib may fail with LNK1241.")
	endif()
endif()
