# cook_script_names_setup.cmake — the fixture builder for the cook trap gate.
#
# Runs as a CTest FIXTURES_SETUP step, NOT as a build-time custom command, and
# that is deliberate: a failure here has to surface as a RED TEST that names
# the stage it died in.  As an add_custom_command it would surface as a build
# error, and the test that depends on it would never run at all — and "the
# test did not run" is the shape of green this campaign keeps finding.
#
# It invokes the REAL jce_cook and the REAL jce_pak, and it invokes jce_cook
# WITHOUT --preserve-names on purpose.  Every caller in this repository passes
# that flag; the bug was never in the callers that pass it, so a fixture that
# passes it would exercise nothing.
#
# TWO COOKS, not one.  jce_cook names an output in two places — once for the
# asset it is writing, and once in the per-run stale sweep that deletes the
# output of a source that has since been deleted.  A rule that holds in the
# first place and not the second leaves a removed script SHIPPING FOREVER:
# the sweep hunts <stem>.jceasset, never finds it, and the file the author
# deleted stays in every subsequent build.  A single cook cannot see that, so
# this fixture cooks, deletes one script, and cooks again with the same
# incremental catalog.  (Measured: with the sweep site left on the old rule,
# a one-cook fixture passed — the mutation survived the whole suite.)
#
# Required -D arguments: JCE_COOK_EXE, JCE_PAK_EXE, WORK_DIR

foreach(_req JCE_COOK_EXE JCE_PAK_EXE WORK_DIR)
	if(NOT DEFINED ${_req})
		message(FATAL_ERROR "cook_script_names_setup: -D${_req} is required")
	endif()
endforeach()

set(_src    "${WORK_DIR}/src")
set(_cooked "${WORK_DIR}/cooked")
set(_pak    "${WORK_DIR}/cook_script_names.pak")
# OUTSIDE the cooked tree on purpose: jce_cook's default catalog location is
# <out>/.jce_cook_catalog, and jce_pak packs whatever it finds under the
# resource dir — the cache bookkeeping would ship inside the archive.
set(_cat    "${WORK_DIR}/cook.catalog")

# Wipe first.  Without this, an output written by a PREVIOUS build of jce_cook
# — for instance the .jceasset a mutation produced — survives into the next
# run and the gate reads a tree that no single binary produced.
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${_src}/scripts" "${_src}/data")

# One file per row of the script catalog (engine/src/resource/jce_asset_ext.c).
# Probe.java and Probe.class SHARE A STEM on purpose: that pair is what the
# rename collapsed into a single output file, silently destroying one of them.
# Each says which side it is in its own text so the survivor is identifiable.
file(WRITE "${_src}/scripts/probe.lua"    "-- lua source fixture\n")
file(WRITE "${_src}/scripts/probe.py"     "# python source fixture\n")
file(WRITE "${_src}/scripts/Probe.java"   "// SOURCE-SIDE java fixture\nclass Probe {}\n")
file(WRITE "${_src}/scripts/Probe.class"  "BYTECODE-SIDE java fixture (not real bytecode)\n")
file(WRITE "${_src}/scripts/Probe.jcecpp" "// cpp class-reference fixture\n")
file(WRITE "${_src}/scripts/Probe.jcec"   "// c class-reference fixture\n")

# The control: a non-script the cooker SHOULD still rename to .jceasset.
file(WRITE "${_src}/data/table.json"      "{ \"control\": true }\n")

# The script that gets deleted between the two cooks.  Its own stem, so its
# disappearance cannot be confused with any other fixture's.
file(WRITE "${_src}/scripts/doomed.lua"   "-- deleted between the two cooks\n")

function(_csn_cook LABEL)
	message(STATUS "cook_script_names: cook ${LABEL} — ${_src} -> ${_cooked} "
	               "(deliberately WITHOUT --preserve-names)")
	execute_process(
		COMMAND "${JCE_COOK_EXE}" --batch "${_src}" "${_cooked}"
		        --catalog "${_cat}" --level 0 --verbose
		RESULT_VARIABLE _rc
		OUTPUT_VARIABLE _out
		ERROR_VARIABLE  _err)
	message(STATUS "${_out}${_err}")
	if(NOT _rc EQUAL 0)
		message(FATAL_ERROR
			"cook_script_names: jce_cook failed on cook ${LABEL} (rc=${_rc})")
	endif()
endfunction()

_csn_cook(1)

# LIVENESS ONLY, and the line between the two halves is deliberate.
#
# This step asserts that the tools RAN AND PRODUCED SOMETHING — silence
# compares equal to silence, and an empty cooked tree would let every
# assertion in the checker pass vacuously.  It does NOT judge the names: the
# first draft failed here on "8 went in, 6 came out", which made the setup
# step red and left the checker NOT RUN, so the mutation that models the
# original bug was never actually shown to the assertion written for it.
# Every semantic verdict belongs to test_jce_cook_script_names.c, which can
# name the offending PAK key.
file(GLOB_RECURSE _cooked_files "${_cooked}/*")
list(LENGTH _cooked_files _cooked_n)
if(_cooked_n LESS 1)
	message(FATAL_ERROR
		"cook_script_names: jce_cook reported success and produced no files "
		"at all in ${_cooked}")
endif()
message(STATUS "cook_script_names: ${_cooked_n} files after cook 1 "
               "(8 went in; the checker decides whether that is right)")

# The author deletes a script.  The next cook's stale sweep has to notice.
file(REMOVE "${_src}/scripts/doomed.lua")
if(EXISTS "${_src}/scripts/doomed.lua")
	message(FATAL_ERROR "cook_script_names: could not delete the doomed script")
endif()

_csn_cook(2)

message(STATUS "cook_script_names: packing ${_cooked} -> ${_pak}")
execute_process(
	COMMAND "${JCE_PAK_EXE}"
	        --resource-dir  "${_cooked}"
	        --pak-file      "${_pak}"
	        --header-file   "${WORK_DIR}/cook_script_names_assets.h"
	        --manifest-file "${WORK_DIR}/cook_script_names_manifest.cmake"
	        --level 1
	RESULT_VARIABLE _pak_rc
	OUTPUT_VARIABLE _pak_out
	ERROR_VARIABLE  _pak_err)
message(STATUS "${_pak_out}${_pak_err}")
if(NOT _pak_rc EQUAL 0)
	message(FATAL_ERROR "cook_script_names: jce_pak failed (rc=${_pak_rc})")
endif()
if(NOT EXISTS "${_pak}")
	message(FATAL_ERROR
		"cook_script_names: jce_pak reported success and wrote no ${_pak}")
endif()

message(STATUS "cook_script_names: setup OK — ${_pak}")
