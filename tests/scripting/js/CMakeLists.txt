cmake_minimum_required(VERSION 3.20)

# The js backend's suite.  SKIPPED with a REGISTERED reason when the backend is
# not in this build, so the test count never quietly means "of the ones this
# machine could build" — see the summary at the bottom of tests/CMakeLists.txt.
if(NOT TARGET jce_script_vm_js)
	jce_test_suite_skipped("script-js"
		"the js VM suite; jce_script_vm_js was not built (quickjs-ng did not resolve)")
	return()
endif()

jce_add_unit_test(test_jce_script_vm_js
	SOURCES test_jce_script_vm_js.c
	LINK    jce_core jce_script jce_script_vm_js)
