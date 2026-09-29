cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  The Python binding's suite — one cross-language differential.           #
#                                                                          #
#  Three pieces, and the shape of them is the argument:                    #
#                                                                          #
#    jce_script_py_mock   a SHARED recording mock host, generated from the  #
#                         manifest.  Shared and not static because BOTH     #
#                         sides load it: the Lua runner links it, and       #
#                         differential.py dlopens it through ctypes.  Two   #
#                         mocks written to agree would be a second          #
#                         two-sided contract.                               #
#    lua_case_runner      installs that host into a real jce_script VM and  #
#                         prints the recorded stream.                       #
#    differential.py      derives the cases from                            #
#                         contracts/script-api.json, drives both    #
#                         sides, and compares the streams.                  #
#                                                                          #
#  NOT a jce_add_unit_test: the assertion lives in Python, because one of   #
#  the two implementations under comparison is Python.  Registered with     #
#  the same LABELS/TIMEOUT so `ctest -L unit` picks it up.                  #
#                                                                          #
#  And one static test beside it, test_emit_python.py, for the four         #
#  properties the differential is structurally blind to.  It is registered  #
#  BEFORE the jce_script_api guard below on purpose: it needs no library,   #
#  no mock and no build, so a tree configured with                          #
#  -DJCE_BUILD_SCRIPT_API_SHARED=OFF must still run it — a gate that        #
#  disappears with the thing it guards is a gate that reports green on the  #
#  configuration where nobody looks.                                        #
# ----------------------------------------------------------------------- #

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_FOUND)
	message(STATUS
		"JCE tests: Python binding tests SKIPPED — no Python3 interpreter "
		"found")
	return()
endif()

# ── The static gate (no build required) ──────────────────────────────────── #
add_test(NAME test_jce_script_python_emitter
	COMMAND "${Python3_EXECUTABLE}"
	        "${CMAKE_CURRENT_SOURCE_DIR}/test_emit_python.py")
set_tests_properties(test_jce_script_python_emitter PROPERTIES
	TIMEOUT 60
	LABELS  "unit"
	# JCE_TEST_ENVIRONMENT first, because tests/CMakeLists.txt:280 refuses to
	# configure a registered test that does not carry it.  Then
	# PYTHONDONTWRITEBYTECODE, deliberately: this test imports
	# tools/scriptgen/emit_python.py, and a .pyc written into a shared tree is
	# how a reverted mutation stayed live in this worktree once already —
	# CPython invalidates on (mtime, size), and a same-size revert inside one
	# mtime second leaves the mutated bytecode running.
	ENVIRONMENT "${JCE_TEST_ENVIRONMENT};PYTHONDONTWRITEBYTECODE=1")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
	test_jce_script_python_emitter)

if(NOT TARGET jce_script_api)
	# Not silent, for the same reason tests/scripting/c_abi is not: the
	# differential is where the Python binding is accepted at all, and its
	# absence belongs in the configure log rather than in a test count.
	jce_test_suite_skipped("script-python-binding"
		"the Python binding differential; jce_script_api is not built (-DJCE_BUILD_SCRIPT_API_SHARED=OFF)")
	return()
endif()

# ── The shared recording mock host ───────────────────────────────────────── #
add_library(jce_script_py_mock SHARED jce_script_py_mock.gen.c)
set_target_properties(jce_script_py_mock PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	# Beside the runner, so Windows finds it through the executable's own
	# directory and the test needs no PATH surgery for the LINKED dependency.
	# ctypes still loads it by absolute path.
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
	# The export set here is NOT a security surface — this object is a test
	# fixture and exports whatever it exports.  That is the opposite of
	# jce_script_api, whose curated .def is the sandbox claim; saying so here
	# keeps the two from being read as the same kind of target.
	WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_include_directories(jce_script_py_mock PRIVATE
	"${CMAKE_SOURCE_DIR}/engine/include"
	"${CMAKE_CURRENT_SOURCE_DIR}")

# The generated mock must actually be compiled.  Same reasoning as
# scripting/c_abi/CMakeLists.txt: the generator proves the file is CORRECT and
# says nothing about whether it is BUILT, and a mock whose members were all
# NULL would make every differential case take the absent path — on BOTH sides,
# identically, and pass.
get_target_property(_jce_py_mock_srcs jce_script_py_mock SOURCES)
if(NOT _jce_py_mock_srcs MATCHES "jce_script_py_mock\\.gen\\.c")
	message(FATAL_ERROR
		"jce_script_py_mock does not compile jce_script_py_mock.gen.c. "
		"Regenerate with:\n"
		"  python tools/scriptgen/gen_script_bindings.py --write")
endif()
unset(_jce_py_mock_srcs)

# ── The Lua half ─────────────────────────────────────────────────────────── #
add_executable(jce_lua_case_runner lua_case_runner.c)
set_target_properties(jce_lua_case_runner PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
jce_configure_test_link_pool(jce_lua_case_runner)
target_link_libraries(jce_lua_case_runner PRIVATE
	jce_core jce_script jce_script_py_mock)
target_include_directories(jce_lua_case_runner PRIVATE
	"${CMAKE_SOURCE_DIR}/engine/include"
	"${CMAKE_CURRENT_SOURCE_DIR}")
# EVERY binary the differential opens, named on jce_tests — not just the one
# ctest is told to run.  `jce_lua_case_runner` LINKS the mock, so that one
# arrives either way.  `jce_script_api` does NOT: the differential LOADS it
# through ctypes, and the only mention of it here is a $<TARGET_FILE:>
# expansion, which creates no build edge.  It reaches jce_tests today only
# because tests/scripting/c_abi's unit test depends on it — measured, not
# assumed — and that is ANOTHER DIRECTORY'S file.  A suite whose binaries get
# built because a sibling happens to want them is one deletion away from
# `cmake --build --target jce_tests` succeeding while `ctest -L unit` fails
# inside differential.py's "the api was not built" guard, which reads like a
# broken test rather than a missing artefact.
add_dependencies(jce_tests
	jce_lua_case_runner jce_script_py_mock jce_script_api)

# ── The test ─────────────────────────────────────────────────────────────── #
add_test(NAME test_jce_script_python_differential
	COMMAND "${Python3_EXECUTABLE}"
	        "${CMAKE_CURRENT_SOURCE_DIR}/differential.py"
	        --repo    "${CMAKE_SOURCE_DIR}"
	        --runner  "$<TARGET_FILE:jce_lua_case_runner>"
	        --mock    "$<TARGET_FILE:jce_script_py_mock>"
	        --api     "$<TARGET_FILE:jce_script_api>")
set_tests_properties(test_jce_script_python_differential PROPERTIES
	TIMEOUT 120
	LABELS  "unit"
	ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
	test_jce_script_python_differential)

# ----------------------------------------------------------------------- #
#  The LIFECYCLE differential — the engine calling UP.                     #
#                                                                          #
#  differential.py above compares the two CALL-DOWN bindings.  This pair    #
#  compares the two VMs: one driver, two languages, the same public         #
#  jce_script_* entry points, the same recording mock host.                 #
#                                                                          #
#  Guarded on jce_script_vm_python because that target is itself guarded    #
#  on an embeddable CPython.  A configuration without it still runs         #
#  test_jce_script_python_emitter, which reads the VM's SOURCES and needs   #
#  no build — so the properties that hold statically are still gated where  #
#  the backend cannot be built, and the STATUS below says which ones are    #
#  not.                                                                     #
# ----------------------------------------------------------------------- #

if(NOT TARGET jce_script_vm_python)
	jce_test_suite_skipped("script-python-vm-lifecycle"
		"jce_script_vm_python was not built (no embeddable CPython); test_jce_script_python_emitter still covers the static half")
	return()
endif()

add_executable(jce_lifecycle_runner lifecycle_runner.c)
set_target_properties(jce_lifecycle_runner PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
jce_configure_test_link_pool(jce_lifecycle_runner)
target_link_libraries(jce_lifecycle_runner PRIVATE
	jce_core jce_script jce_script_vm_python jce_script_py_mock)
target_include_directories(jce_lifecycle_runner PRIVATE
	"${CMAKE_SOURCE_DIR}/engine/include"
	"${CMAKE_CURRENT_SOURCE_DIR}")

# The Unity test for what the differential is structurally blind to: the ABI
# clamp, the GIL counters, interpreter lifetime across create/destroy cycles,
# and the ONE documented divergence from Lua.
jce_add_unit_test(test_jce_script_vm_python
	SOURCES  test_jce_script_vm_python.c
	LINK     jce_core jce_script jce_script_vm_python jce_script_py_mock
	INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")

# bob.lua and turret.py IN THE SAME SCENE, through the real
# jce_runtime_create / jce_runtime_step path.  Links the JCE aggregate
# (jce_runtime_create pulls the whole middleware stack) PLUS the Python
# backend, which is the only combination in the tree that holds both
# languages at once.  It lives here rather than in tests/application because
# jce_script_vm_python only exists when CMake found an embeddable CPython,
# and this directory already carries that condition; the build-independent
# half is tests/application/test_jce_runtime_multilang.c.
jce_add_unit_test(test_jce_runtime_python_scene
	SOURCES  test_jce_runtime_python_scene.c
	LINK     JCE jce_script_vm_python)
target_compile_definitions(test_jce_runtime_python_scene PRIVATE
	JCE_PY_TEST_PACKAGE_DIR="${JCE_SCRIPT_PYTHON_PACKAGE_DIR}"
	JCE_PY_TEST_API="$<TARGET_FILE:jce_script_api>")
add_dependencies(test_jce_runtime_python_scene jce_script_api)

# EVERY binary the differential opens, named on jce_tests — the same reasoning
# as the block above: jce_script_api is LOADED through ctypes, which creates no
# build edge, so a suite built with `--target jce_tests` would otherwise be one
# deletion away from failing inside the driver's "the api was not built" path.
add_dependencies(jce_tests
	jce_lifecycle_runner jce_script_vm_python jce_script_api)

add_test(NAME test_jce_script_python_lifecycle
	COMMAND "${Python3_EXECUTABLE}"
	        "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle_differential.py"
	        --repo    "${CMAKE_SOURCE_DIR}"
	        --runner  "$<TARGET_FILE:jce_lifecycle_runner>"
	        --api     "$<TARGET_FILE:jce_script_api>"
	        --package "${JCE_SCRIPT_PYTHON_PACKAGE_DIR}"
	        --work    "${CMAKE_CURRENT_BINARY_DIR}/lifecycle_work")
set_tests_properties(test_jce_script_python_lifecycle PROPERTIES
	TIMEOUT 180
	LABELS  "unit"
	ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
	test_jce_script_python_lifecycle)

# The two paths the Unity test needs.  Compile definitions and NOT a ctest
# ENVIRONMENT property, on purpose: the test must behave identically when run
# straight out of the build tree (`./tests/test_jce_script_vm_python.exe`),
# which is how a mutation battery runs it and how a ctest ENVIRONMENT property
# silently stops applying.
target_compile_definitions(test_jce_script_vm_python PRIVATE
	JCE_PY_TEST_PACKAGE_DIR="${JCE_SCRIPT_PYTHON_PACKAGE_DIR}"
	JCE_PY_TEST_API="$<TARGET_FILE:jce_script_api>")
