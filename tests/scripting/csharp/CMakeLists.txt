cmake_minimum_required(VERSION 3.20)

# The csharp backend's suite.  SKIPPED with a REGISTERED reason when the
# backend is not in this build, so the test count never quietly means "of the
# ones this machine could build" — see the summary at the bottom of
# tests/CMakeLists.txt.
if(NOT TARGET jce_script_vm_csharp)
	jce_test_suite_skipped("script-csharp"
		"the csharp VM suite; jce_script_vm_csharp was not built (no .NET app-host pack)")
	return()
endif()

jce_add_unit_test(test_jce_script_vm_csharp
	SOURCES test_jce_script_vm_csharp.c
	LINK    jce_core jce_script jce_script_vm_csharp)

# THE THREE FILES THIS TEST CANNOT RUN WITHOUT, and none of them is a link
# input — which is exactly why they are easy to forget:
#
#   jce_script_api.dll       the managed side P/Invokes it BY NAME, so the
#                            .NET loader must find it beside the test exe;
#                            missing, every binding is a DllNotFoundException
#                            inside managed code, a long way from here.
#   JceScript.dll            the managed half; the backend looks beside the
#                            executable first.
#   JceScript.runtimeconfig  hostfxr finds it BY NAME beside the assembly, so
#                            an assembly staged without it initialises nothing.
#   nethost.dll              linked through its IMPORT library, because the
#                            static libnethost.lib is /MT and this engine is
#                            /MD (LNK2038); missing, the process does not
#                            start at all and prints nothing.
#
# copy_if_different and not a hard dependency on the managed build: `dotnet
# build` is deliberately outside CMake (see scripting/csharp/build_csharp.py),
# so a missing artifact must fail the TEST with the register() message that
# names build_csharp.py, not fail the build with a CMake error about a file.
add_custom_command(TARGET test_jce_script_vm_csharp POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"$<TARGET_FILE:jce_script_api>"
		"$<TARGET_FILE_DIR:test_jce_script_vm_csharp>"
	VERBATIM)

foreach(_f "${JCE_SCRIPT_CSHARP_ASSEMBLY}" "${JCE_SCRIPT_CSHARP_RUNTIMECONFIG}"
              "${JCE_SCRIPT_CSHARP_NETHOST}")
	if(EXISTS "${_f}")
		add_custom_command(TARGET test_jce_script_vm_csharp POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${_f}" "$<TARGET_FILE_DIR:test_jce_script_vm_csharp>"
			VERBATIM)
	else()
		message(STATUS
			"tests/scripting/csharp: ${_f} is not built yet — run "
			"`python scripting/csharp/build_csharp.py`. The suite is still "
			"registered and will FAIL with the reason, rather than being "
			"skipped as if the backend did not exist.")
	endif()
endforeach()
unset(_f)
