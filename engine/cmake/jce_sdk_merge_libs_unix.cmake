# engine/cmake/jce_sdk_merge_libs_unix.cmake
#
# Build-time driver for merging static libraries into a fat archive on
# non-MSVC hosts.  Invoked by jce_register_sdk_install() as a cmake -P script.
#
# Inputs (set via -D on the cmake command line):
#   AR_EXE        — path to the ar executable (CMAKE_AR)
#   OUT_LIB       — output .a path
#   RSP_IMPORTED  — response file: already-resolved imported lib paths
#   RSP_OWNED     — response file: build-time-resolved owned lib paths
#                   (generated via file(GENERATE), populated with
#                   $<TARGET_FILE:...> entries that resolve per-config)
#   JCE_SDK_APPLE — "1" on macOS (use libtool -static), "0" on Linux (ar MRI)

cmake_minimum_required(VERSION 3.20)

if(NOT OUT_LIB OR NOT RSP_IMPORTED OR NOT RSP_OWNED OR NOT AR_EXE)
	message(FATAL_ERROR
		"jce_sdk_merge_libs_unix: required vars: "
		"AR_EXE / OUT_LIB / RSP_IMPORTED / RSP_OWNED")
endif()

# --------------------------------------------------------------------------- #
# Helper: parse a response file into a list of unquoted, non-empty paths.     #
# Response file format: one path per line, optionally double-quoted.           #
# --------------------------------------------------------------------------- #
function(parse_rsp_file _file _out)
	if(NOT EXISTS "${_file}")
		set("${_out}" "" PARENT_SCOPE)
		return()
	endif()
	file(READ "${_file}" _raw)
	string(REPLACE "\n" ";" _lines "${_raw}")
	set(_paths "")
	foreach(_line IN LISTS _lines)
		string(STRIP "${_line}" _line)
		if(NOT _line)
			continue()
		endif()
		# Strip surrounding double-quotes (MSVC lib.exe RSP format).
		string(REGEX REPLACE "^\"(.+)\"$" "\\1" _line "${_line}")
		if(_line)
			list(APPEND _paths "${_line}")
		endif()
	endforeach()
	set("${_out}" "${_paths}" PARENT_SCOPE)
endfunction()

parse_rsp_file("${RSP_OWNED}"    _owned_libs)
parse_rsp_file("${RSP_IMPORTED}" _imported_libs)
list(APPEND _all_libs ${_owned_libs} ${_imported_libs})
list(REMOVE_DUPLICATES _all_libs)

get_filename_component(_out_dir  "${OUT_LIB}" DIRECTORY)
get_filename_component(_out_stem "${OUT_LIB}" NAME_WE)
file(MAKE_DIRECTORY "${_out_dir}")

# Handle the empty-input case (e.g. jce_engine_core when there are no
# owned targets yet) gracefully so the build does not fail.
if(NOT _all_libs)
	message(STATUS "jce_sdk_merge_libs_unix: no inputs for ${OUT_LIB} — creating empty archive")
	execute_process(
		COMMAND "${AR_EXE}" -rcs "${OUT_LIB}"
		RESULT_VARIABLE _rc)
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR "jce_sdk_merge_libs_unix: ar -rcs (empty) failed (rc=${_rc})")
	endif()
	return()
endif()

# --------------------------------------------------------------------------- #
# macOS — libtool -static                                                      #
# libtool is Apple's dedicated static-lib merge tool; it handles             #
# deduplication and symbol table generation internally.                        #
# --------------------------------------------------------------------------- #
if(JCE_SDK_APPLE STREQUAL "1")
	find_program(_libtool NAMES libtool REQUIRED)
	execute_process(
		COMMAND "${_libtool}" -static -o "${OUT_LIB}" ${_all_libs}
		RESULT_VARIABLE _rc
		ERROR_VARIABLE  _err)
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR
			"jce_sdk_merge_libs_unix (libtool -static): failed (rc=${_rc})\n${_err}")
	endif()
	message(STATUS "jce_sdk_merge_libs_unix (libtool): ${OUT_LIB}")

# --------------------------------------------------------------------------- #
# Linux / generic POSIX — GNU ar MRI script                                   #
# ar -M reads a text MRI script that specifies CREATE/ADDLIB/SAVE directives. #
# We write the script to a temp file, pipe it to ar, then run ar -s to        #
# regenerate the symbol table (equivalent to ranlib).                          #
# --------------------------------------------------------------------------- #
else()
	set(_mri_script "${_out_dir}/${_out_stem}.mri")
	set(_mri "CREATE ${OUT_LIB}\n")
	foreach(_lib IN LISTS _all_libs)
		if(NOT EXISTS "${_lib}")
			message(WARNING "jce_sdk_merge_libs_unix: ADDLIB target missing: ${_lib}")
		endif()
		string(APPEND _mri "ADDLIB ${_lib}\n")
	endforeach()
	string(APPEND _mri "SAVE\nEND\n")
	file(WRITE "${_mri_script}" "${_mri}")

	execute_process(
		COMMAND "${AR_EXE}" -M
		INPUT_FILE "${_mri_script}"
		RESULT_VARIABLE _rc
		ERROR_VARIABLE  _err)
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR
			"jce_sdk_merge_libs_unix (ar -M): failed (rc=${_rc})\n"
			"MRI script: ${_mri_script}\n${_err}")
	endif()

	# Regenerate the symbol table so the linker can resolve symbols without
	# scanning every object.  ar -M may not do this automatically.
	execute_process(
		COMMAND "${AR_EXE}" -s "${OUT_LIB}"
		RESULT_VARIABLE _rc2
		ERROR_VARIABLE  _err2)
	if(NOT _rc2 EQUAL 0)
		message(WARNING
			"jce_sdk_merge_libs_unix (ar -s): symbol-table update failed "
			"(rc=${_rc2}) — consumer link may fail: ${_err2}")
	endif()

	message(STATUS "jce_sdk_merge_libs_unix (ar MRI): ${OUT_LIB}")
endif()
