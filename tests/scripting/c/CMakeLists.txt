cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  The "c" JceScriptVM's suite.                                            #
#                                                                          #
#  THE MODULE IS THE SUBJECT.  scripting/c's claim is that a PURE C         #
#  translation unit can be an entity script, so the module here is built    #
#  from a .c file, twice, and both targets are asserted to be C:            #
#                                                                          #
#   * compiled INTO the test executable — the shape a shipped game uses,    #
#     reached through the accessor JCE_C_MODULE_END emits.                  #
#   * built as a MODULE library — the shape editor Play uses, reached       #
#     through the EXPORTED entry symbol and an ABSOLUTE path.  A statically  #
#     linked stand-in cannot fail the way a plugin fails, and the exported   #
#     symbol is the half a C author is most likely not to have.             #
#                                                                          #
#  One source, two -D: the registry is process-wide and refuses a duplicate  #
#  module OR class name, so the two copies must not share either.           #
# ----------------------------------------------------------------------- #

if(NOT TARGET jce_script_vm_c)
	# Not silent.  tests/scripting/ discovers by glob, so an absent suite is
	# indistinguishable from a suite that never existed unless it says so.
	message(STATUS
		"JCE tests: C scripting suite SKIPPED — there is no jce_script_vm_c "
		"target. If scripting/ IS being built this is an add_subdirectory "
		"ordering problem, not a configuration choice.")
	return()
endif()

set(_jce_c_module_src "${CMAKE_CURRENT_SOURCE_DIR}/vm_module/jce_c_testmodule.c")

# ── The MODULE library ──────────────────────────────────────────────────
#
# jce_script_c_module, NOT jce_script_vm_c: a module must not carry the
# registry.  A plugin that linked its own copy would publish its classes into
# a registry the engine never reads — a module that loads, reports success,
# and whose classes no instantiate() can find.
add_library(jce_c_testmodule MODULE "${_jce_c_module_src}")
set_target_properties(jce_c_testmodule PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
target_compile_definitions(jce_c_testmodule PRIVATE JCE_C_TESTMODULE_DLL=1)
target_link_libraries(jce_c_testmodule PRIVATE jce_script_c_module)
jce_configure_test_link_pool(jce_c_testmodule)

# ── The suite ───────────────────────────────────────────────────────────
# TWO static modules, and the second one is here to prove the first thing a
# five-language project needs: that two native script modules can be in one
# binary at all.  vm_module/jce_c_testmodule_two.c explains the LNK2005 that
# used to make that impossible; JCE_SCRIPT_MODULE_NO_ENTRY below is the fix,
# and removing it is a mutation whose result is a BUILD failure rather than a
# red assertion.
jce_add_unit_test(test_jce_script_vm_c
	SOURCES test_jce_script_vm_c.c
	        vm_module/jce_c_testmodule.c
	        vm_module/jce_c_testmodule_two.c
	LINK    jce_core jce_script jce_script_vm_c)

# THE EXECUTABLE, NOT THE MODULE LIBRARY.  The exported entry point is one
# external symbol per shared object; inside an executable it is dead code (a
# statically linked host reaches a module through its accessor and
# jce_script_vm_cpp_add_module) and two of them do not link.  The
# jce_c_testmodule MODULE target above deliberately does NOT get this define:
# there the entry is the only door, and suppressing it would produce a library
# that loads and is then refused as "not a JCE script module" -- the failure
# that survives shipping.
target_compile_definitions(test_jce_script_vm_c PRIVATE
	JCE_SCRIPT_MODULE_NO_ENTRY=1)

# The module is loaded BY PATH, not linked, so nothing in the link graph would
# say it has to exist first.
add_dependencies(test_jce_script_vm_c jce_c_testmodule)

# jce_script_vm_cpp_load_library() REFUSES a bare name — jce_library_open()
# would attach to whatever module of that name is already resident — so the
# test cannot spell the file name, and $<TARGET_FILE:> is the only absolute
# path that stays correct across generators and configurations.
target_compile_definitions(test_jce_script_vm_c PRIVATE
	"JCE_C_TESTMODULE_PATH=\"$<TARGET_FILE:jce_c_testmodule>\"")

# ── Both halves must actually be COMPILED AS C ──────────────────────────
#
# This is the assertion the whole suite rests on.  If either target were
# compiled as C++ — a rename to .cpp, an inherited LINKER_LANGUAGE, a
# set_source_files_properties(LANGUAGE CXX) somewhere — every case below would
# still pass and would be testing the one thing that was never in question.
# There is no warning for it and no runtime symptom, so it is asserted here.
foreach(_jce_c_tgt jce_c_testmodule test_jce_script_vm_c)
	get_target_property(_jce_c_std ${_jce_c_tgt} C_STANDARD)
	if(NOT _jce_c_std STREQUAL "99")
		message(FATAL_ERROR
			"${_jce_c_tgt} has C_STANDARD ${_jce_c_std}, not 99 — the C script "
			"header claims C99 and nothing would be testing it.")
	endif()
endforeach()
get_source_file_property(_jce_c_mod_lang "${_jce_c_module_src}" LANGUAGE)
if(_jce_c_mod_lang AND NOT _jce_c_mod_lang STREQUAL "NOTFOUND"
   AND NOT _jce_c_mod_lang STREQUAL "C")
	message(FATAL_ERROR
		"vm_module/jce_c_testmodule.c has LANGUAGE ${_jce_c_mod_lang}, not C — "
		"the module that is supposed to prove a C translation unit can be a "
		"script would be compiled by the C++ compiler.")
endif()
message(STATUS
	"JCE tests: C scripting suite wired (module built twice from "
	"vm_module/jce_c_testmodule.c, as C99, static + MODULE).")

unset(_jce_c_tgt)
unset(_jce_c_std)
unset(_jce_c_mod_lang)
unset(_jce_c_module_src)
