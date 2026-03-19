# build_report.cmake
#
# Post-build size report for JCE in GraalVM Native Image style.
# Invoked in CMake script mode (-P) by a POST_BUILD custom command.
#
# Required -D variables:
#   EXE_FILE      — absolute path to the linked executable
#   PAK_FILE      — absolute path to the generated assets.pak
#   MANIFEST_FILE — absolute path to _assets_manifest.cmake
#   DEP_LOCS_FILE — absolute path to _dep_locs_<CONFIG>.cmake
#   SOURCE_DIR    — absolute path to main/native (for .c globbing)
#   CONFIG        — build configuration (Release, Debug, …)
#
# Sample output:
#
#   ════════════════════════════════════════════════════════════════════════
#     JCE  ·  Release  ·  Build Size Report
#   ════════════════════════════════════════════════════════════════════════
#
#     Native Sources  (source-text sizes)
#     ──────────────────────────────────────────────────────────────────
#        14.01 KB  100.00%  ████████████████████  main.c
#     ──────────────────────────────────────────────────────────────────
#     +   14.01 KB   1 source files
#
#     Embedded Assets  (ZSTD compressed)
#     ──────────────────────────────────────────────────────────────────
#       raw        comp     ratio  bar                   name
#        10.85 MB    5.12 MB  2.12×  █████░░░░░░░░░░░░░░░  fonts/JCE.ttf
#         3.20 MB    2.88 MB  1.11×  ███░░░░░░░░░░░░░░░░░  textures/chalet.jpg
#     ──────────────────────────────────────────────────────────────────
#     +   46.86 MB   8 assets  ·  46.86 MB raw → 15.20 MB compressed  (0.32×)
#
#     ════════════════════════════════════════════════════════════════════
#     =   20.47 MB   JCE.exe
#   ════════════════════════════════════════════════════════════════════════

cmake_minimum_required(VERSION 3.20)

# ================================================================== #
# Input validation                                                    #
# ================================================================== #

# _require_var — warn and abort (not FATAL_ERROR) so a broken report
# never fails the build itself.
macro(_require_var _var)
	if(NOT DEFINED ${_var})
		message(WARNING
			"[build_report] Required variable not set: ${_var}  "
			"(report skipped — check POST_BUILD command in CMakeLists.txt)")
		return()
	endif()
endmacro()

# NOTE: This script uses SOURCE_DIR (main/native) for source file
# globbing.  Do not add RESOURCE_DIR here.
_require_var(EXE_FILE)
_require_var(PAK_FILE)
_require_var(MANIFEST_FILE)
_require_var(DEP_LOCS_FILE)
_require_var(SOURCE_DIR)
_require_var(CONFIG)

# ================================================================== #
# Visual constants                                                     #
# ================================================================== #

# BAR_WIDTH — characters inside each progress bar.
# Every bar loop runs at most BAR_WIDTH iterations (P10 rule 2).
set(BAR_WIDTH 20)

# Outer double-rule: marks the report start and end.
set(RULE_OUTER
	"  ════════════════════════════════════════════════════════════════════════")

# Inner single-rule: separates detail rows from section totals.
set(RULE_INNER
	"    ──────────────────────────────────────────────────────────────────────")

# ================================================================== #
# Formatting helpers                                                  #
# ================================================================== #

# ──────────────────────────────────────────────────────────────────
# human_size — convert bytes to a right-aligned 10-char string.
# Examples:  "  18.52 KB"  "   0.00  B"  " 234.33 MB"
# Unit is always 2 chars (" B" / "KB" / "MB" / "GB").
# Loop bound: pads up to 10 chars (≤ 10 iterations, P10 rule 2).
# ──────────────────────────────────────────────────────────────────
function(human_size bytes out_var)
	if(${bytes} GREATER_EQUAL 1073741824)
		math(EXPR _v "${bytes} * 100 / 1073741824")
		set(_unit "GB")
	elseif(${bytes} GREATER_EQUAL 1048576)
		math(EXPR _v "${bytes} * 100 / 1048576")
		set(_unit "MB")
	elseif(${bytes} GREATER_EQUAL 1024)
		math(EXPR _v "${bytes} * 100 / 1024")
		set(_unit "KB")
	else()
		# Space before "B" keeps the unit at exactly 2 chars.
		math(EXPR _v "${bytes} * 100")
		set(_unit " B")
	endif()

	math(EXPR _i "${_v} / 100")
	math(EXPR _f "${_v} % 100")
	if(_f LESS 10)
		set(_frac "0${_f}")
	else()
		set(_frac "${_f}")
	endif()

	# "NNN.ff XB" — longest possible is "999.99 GB" = 9 chars; pad to 10.
	set(_raw "${_i}.${_frac} ${_unit}")
	string(LENGTH "${_raw}" _len)
	set(_pad "")
	while(_len LESS 10)
		set(_pad " ${_pad}")
		math(EXPR _len "${_len} + 1")
	endwhile()
	set(${out_var} "${_pad}${_raw}" PARENT_SCOPE)
endfunction()


# ──────────────────────────────────────────────────────────────────
# pct_str — fixed-point percentage, right-aligned 7-char field.
# Examples:  " 99.98%"  "  0.00%"  "100.00%"
# Loop bound: pads up to 7 chars (≤ 7 iterations, P10 rule 2).
# ──────────────────────────────────────────────────────────────────
function(pct_str val total out_var)
	if(${total} EQUAL 0)
		set(${out_var} "  0.00%" PARENT_SCOPE)
		return()
	endif()

	# Integer fixed-point: val × 10000 ÷ total, then split.
	math(EXPR _p  "${val} * 10000 / ${total}")
	math(EXPR _ii "${_p} / 100")
	math(EXPR _ff "${_p} % 100")
	if(_ff LESS 10)
		set(_frac "0${_ff}")
	else()
		set(_frac "${_ff}")
	endif()

	set(_raw "${_ii}.${_frac}%")
	string(LENGTH "${_raw}" _len)
	set(_pad "")
	while(_len LESS 7)
		set(_pad " ${_pad}")
		math(EXPR _len "${_len} + 1")
	endwhile()
	set(${out_var} "${_pad}${_raw}" PARENT_SCOPE)
endfunction()


# ──────────────────────────────────────────────────────────────────
# bar_unicode — Unicode block-character progress bar.
# Filled cells: █ (U+2588 FULL BLOCK)
# Empty cells:  ░ (U+2591 LIGHT SHADE)
# Both loops run at most BAR_WIDTH iterations (P10 rule 2).
# Note: string(LENGTH) counts UTF-8 bytes, not codepoints; we never
# measure the bar string, only append to it, so this is safe.
# ──────────────────────────────────────────────────────────────────
function(bar_unicode val total width out_var)
	if(${total} EQUAL 0 OR ${val} EQUAL 0)
		set(_filled 0)
	else()
		math(EXPR _filled "${val} * ${width} / ${total}")
		if(_filled GREATER ${width})
			set(_filled ${width})
		endif()
	endif()
	math(EXPR _empty "${width} - ${_filled}")

	set(_bar "")
	set(_i 0)
	while(_i LESS ${_filled})
		string(APPEND _bar "█")
		math(EXPR _i "${_i} + 1")
	endwhile()
	set(_j 0)
	while(_j LESS ${_empty})
		string(APPEND _bar "░")
		math(EXPR _j "${_j} + 1")
	endwhile()
	set(${out_var} "${_bar}" PARENT_SCOPE)
endfunction()


# ──────────────────────────────────────────────────────────────────
# ratio_str — encode an integer ratio as "N.NN".
# Used for the C-encoding expansion factor (e.g. "5.00×").
# ──────────────────────────────────────────────────────────────────
function(ratio_str numerator denominator out_var)
	if(${denominator} EQUAL 0)
		set(${out_var} "0.00" PARENT_SCOPE)
		return()
	endif()
	math(EXPR _r100 "${numerator} * 100 / ${denominator}")
	math(EXPR _ri   "${_r100} / 100")
	math(EXPR _rf   "${_r100} % 100")
	if(_rf LESS 10)
		set(_rfrac "0${_rf}")
	else()
		set(_rfrac "${_rf}")
	endif()
	set(${out_var} "${_ri}.${_rfrac}" PARENT_SCOPE)
endfunction()


# ──────────────────────────────────────────────────────────────────
# detail_row — one indented table row inside a section.
# Layout:  "    [10 size]  [7 pct%]  [BAR_WIDTH bar]  name"
# P10 rule 5: two precondition assertions.
# ──────────────────────────────────────────────────────────────────
function(detail_row name bytes section_total)
	if(NOT ${bytes} GREATER_EQUAL 0)
		return()   # defensive: skip malformed entry
	endif()
	if(NOT ${section_total} GREATER_EQUAL 0)
		return()
	endif()

	human_size(${bytes}            _hs)
	pct_str(${bytes} ${section_total} _pct)
	bar_unicode(${bytes} ${section_total} ${BAR_WIDTH} _bar)
	message(STATUS "    ${_hs}  ${_pct}  ${_bar}  ${name}")
endfunction()


# ──────────────────────────────────────────────────────────────────
# section_total_line — the GraalVM-style "+" summary line that closes
# each section.
# Layout:  "  + [10 size]   label"
# ──────────────────────────────────────────────────────────────────
function(section_total_line bytes label)
	human_size(${bytes} _hs)
	message(STATUS "  +  ${_hs}   ${label}")
endfunction()

# ================================================================== #
# Data collection                                                     #
# ================================================================== #

# ── 1. Asset manifest ─────────────────────────────────────────────

set(ASSET_PATHS      "")
set(ASSET_SIZES      "")
set(ASSET_COMPRESSED "")
set(ASSET_RAW_TOTAL   0)
set(ASSET_COMP_TOTAL  0)
set(ASSET_PAK_TOTAL   0)
set(ASSET_FILE_COUNT  0)

if(EXISTS "${MANIFEST_FILE}")
	include("${MANIFEST_FILE}")
else()
	message(WARNING
		"[build_report] Manifest not found: ${MANIFEST_FILE}  "
		"(run a full build first)")
endif()

# ── 2. Native source files ────────────────────────────────────────

file(GLOB_RECURSE _src_files "${SOURCE_DIR}/*.cpp" "${SOURCE_DIR}/*.c"
	"${SOURCE_DIR}/*.h")
list(SORT _src_files)

set(_src_names  "")
set(_src_bytes  "")
set(_src_total   0)

foreach(_f ${_src_files})
	cmake_path(GET _f FILENAME _fname)
	file(SIZE "${_f}" _sz)
	list(APPEND _src_names "${_fname}")
	list(APPEND _src_bytes "${_sz}")
	math(EXPR _src_total "${_src_total} + ${_sz}")
endforeach()

# Measure the .pak file (binary, not a C source anymore).
set(_pak_size 0)
if(EXISTS "${PAK_FILE}")
	file(SIZE "${PAK_FILE}" _pak_size)
endif()

# ── 3. External library files ─────────────────────────────────────

set(DEP_LOCATIONS "")
if(EXISTS "${DEP_LOCS_FILE}")
	include("${DEP_LOCS_FILE}")
else()
	message(WARNING
		"[build_report] Dep-locations file not found: ${DEP_LOCS_FILE}  "
		"(external library sizes skipped)")
endif()

set(_lib_names   "")
set(_lib_bytes   "")
set(_lib_total    0)
set(_lib_missing "")

foreach(_entry ${DEP_LOCATIONS})
	string(FIND "${_entry}" "=" _eq_pos)
	if(_eq_pos LESS 1)
		continue()
	endif()
	string(SUBSTRING "${_entry}" 0 ${_eq_pos} _lib_name)
	math(EXPR _path_start "${_eq_pos} + 1")
	string(SUBSTRING "${_entry}" ${_path_start} -1 _lib_path)

	if(_lib_path STREQUAL "INTERFACE" OR _lib_path STREQUAL "NOTFOUND")
		continue()
	endif()

	if(EXISTS "${_lib_path}")
		file(SIZE "${_lib_path}" _sz)
		list(APPEND _lib_names "${_lib_name}")
		list(APPEND _lib_bytes "${_sz}")
		math(EXPR _lib_total "${_lib_total} + ${_sz}")
	else()
		list(APPEND _lib_missing "${_lib_name}")
	endif()
endforeach()

# ── 4. Executable ─────────────────────────────────────────────────

set(_exe_size 0)
set(_exe_name "")
if(EXISTS "${EXE_FILE}")
	file(SIZE "${EXE_FILE}" _exe_size)
	cmake_path(GET EXE_FILE FILENAME _exe_name)
else()
	message(WARNING
		"[build_report] Executable not found: ${EXE_FILE}")
endif()

# ================================================================== #
# Report: header                                                      #
# ================================================================== #

message(STATUS "")
message(STATUS "${RULE_OUTER}")
message(STATUS "    JCE  ·  ${CONFIG}  ·  Build Size Report")
message(STATUS "${RULE_OUTER}")

# ================================================================== #
# Report: section 1 — Native Sources                                  #
# ================================================================== #

message(STATUS "")
message(STATUS "    Native Sources  (source-text sizes)")
message(STATUS "${RULE_INNER}")

list(LENGTH _src_names _n_src)
set(_idx 0)
while(_idx LESS ${_n_src})    # bounded by _n_src (finite file count)
	list(GET _src_names ${_idx} _name)
	list(GET _src_bytes ${_idx} _sz)
	detail_row("${_name}" ${_sz} ${_src_total})
	math(EXPR _idx "${_idx} + 1")
endwhile()

message(STATUS "${RULE_INNER}")
section_total_line(${_src_total} "${_n_src} source files")

# ================================================================== #
# Report: section 2 — Embedded Assets                                 #
# ================================================================== #

message(STATUS "")
message(STATUS "    Embedded Assets  (ZSTD compressed)")
message(STATUS "${RULE_INNER}")

list(LENGTH ASSET_PATHS _n_assets)
set(_idx 0)
while(_idx LESS ${_n_assets})    # bounded by _n_assets
	list(GET ASSET_PATHS ${_idx} _apath)
	list(GET ASSET_SIZES ${_idx} _asz)

	# Try to get compressed size from ASSET_COMPRESSED list.
	set(_acz 0)
	list(LENGTH ASSET_COMPRESSED _n_comp)
	if(_idx LESS ${_n_comp})
		list(GET ASSET_COMPRESSED ${_idx} _acz)
	endif()

	# Format: raw size, compressed size, ratio, bar, name
	human_size(${_asz} _hs_raw)
	human_size(${_acz} _hs_comp)
	if(${_acz} GREATER 0)
		ratio_str(${_asz} ${_acz} _rx)
	else()
		set(_rx "-.--")
	endif()
	bar_unicode(${_asz} ${ASSET_RAW_TOTAL} ${BAR_WIDTH} _bar)
	message(STATUS "    ${_hs_raw}  ${_hs_comp}  ${_rx}×  ${_bar}  ${_apath}")

	math(EXPR _idx "${_idx} + 1")
endwhile()

if(_n_assets EQUAL 0)
	message(STATUS "    (no assets found in RESOURCE_DIR)")
endif()

# Build the compression-ratio annotation for the "+" line.
set(_comp_note "")
if(_pak_size GREATER 0 AND ASSET_RAW_TOTAL GREATER 0)
	human_size(${ASSET_RAW_TOTAL}  _hs_raw_t)
	human_size(${ASSET_COMP_TOTAL} _hs_comp_t)
	human_size(${_pak_size}        _hs_pak)
	ratio_str(${ASSET_COMP_TOTAL} ${ASSET_RAW_TOTAL} _crx)
	string(STRIP "${_hs_raw_t}"  _hs_raw_ts)
	string(STRIP "${_hs_comp_t}" _hs_comp_ts)
	string(STRIP "${_hs_pak}"    _hs_pak_s)
	set(_comp_note
		"  ·  ${_hs_raw_ts} raw → ${_hs_comp_ts} compressed  (${_crx}×)  ·  pak ${_hs_pak_s}")
endif()

message(STATUS "${RULE_INNER}")
section_total_line(${ASSET_RAW_TOTAL}
	"${ASSET_FILE_COUNT} assets${_comp_note}")

# # ================================================================== #
# # Report: section 3 — External Libraries                              #
# # ================================================================== #

# message(STATUS "")
# message(STATUS "    External Libraries  (import libs · .lib / .a)")
# message(STATUS "${RULE_INNER}")

# list(LENGTH _lib_names _n_libs)
# set(_idx 0)
# while(_idx LESS ${_n_libs})    # bounded by _n_libs
# 	list(GET _lib_names ${_idx} _lname)
# 	list(GET _lib_bytes ${_idx} _lsz)
# 	detail_row("${_lname}" ${_lsz} ${_lib_total})
# 	math(EXPR _idx "${_idx} + 1")
# endwhile()

# foreach(_m ${_lib_missing})    # bounded by number of missing libs
# 	message(STATUS "      ${_m}  [file not found — skipped]")
# endforeach()

# if(_n_libs EQUAL 0 AND NOT _lib_missing)
# 	message(STATUS
# 		"      (no import libraries resolved"
# 		" — dependencies may be DLL-only or header-only)")
# endif()

# message(STATUS "${RULE_INNER}")
# section_total_line(${_lib_total}
# 	"${_n_libs} libraries  (import libs only; deployed DLLs may be larger)")

# ================================================================== #
# Report: footer — GraalVM-style "= total" line                       #
# ================================================================== #

message(STATUS "")
message(STATUS "${RULE_OUTER}")

if(_exe_size GREATER 0)
	human_size(${_exe_size} _hs_exe)
	message(STATUS "  =  ${_hs_exe}   ${_exe_name}")
else()
	message(STATUS "  =  (executable not found)")
endif()

message(STATUS "${RULE_OUTER}")
message(STATUS "")