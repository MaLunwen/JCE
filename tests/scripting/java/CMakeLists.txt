cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  The Java binding's cross-language differential.                        #
#                                                                         #
#  Batch 1 spent the one free correctness oracle: it proved the generated  #
#  Lua bindings equivalent to the hand-written originals and then deleted  #
#  the originals.  LUA IS NOW THE REFERENCE IMPLEMENTATION, so this suite  #
#  drives the SAME manifest-derived cases through both surfaces over ONE   #
#  recording mock host and compares the full result (type and value per    #
#  slot) and the host-call trace.                                          #
#                                                                         #
#  Two processes, not two threads of one:                                  #
#    jce_script_java_lua_reference   links jce_script, runs the Lua VM     #
#    java ... JceDifferential        loads the production JNI shim, which  #
#                                    links only the C ABI shared library   #
#  Each writes a stream file; run_differential.py compares them.  Separate #
#  processes because the JVM's -Xcheck:jni diagnostics have to be readable #
#  on their own, and because a JNI crash must not take the reference       #
#  stream with it.                                                         #
# ----------------------------------------------------------------------- #

if(NOT JCE_BUILD_SCRIPT_JAVA)
	# Not silent.  A suite that is absent because an option is off belongs in
	# the configure log; inferring it from a test count is how a skipped gate
	# becomes an invisible one.
	# REGISTERED, not just printed.  A STATUS line in a 200-line configure log
	# is only seen by somebody already looking for it; the registry is read
	# back beside the test COUNT, which is the number people quote.
	jce_test_suite_skipped("script-java"
		"4 tests including the JNI/Lua differential, which is the only check on the generated Java bindings. JCE_BUILD_SCRIPT_JAVA was set OFF explicitly")
	return()
endif()

# NOT a FATAL_ERROR any more, and the change is not cosmetic: since
# scripting/java defaults its option ON and probes for a JDK, "option ON and
# no target" is the ORDINARY state of a machine without one. Asserting here
# made a JDK-less configure die outright -- measured, by running the configure
# with JAVA_HOME unset, which is the only reason it was caught.
if(NOT TARGET jce_script_java)
	jce_test_suite_skipped("script-java"
		"4 tests including the JNI/Lua differential, which is the only check on the generated Java bindings. No JDK on this machine — set JAVA_HOME to build them")
	return()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
	message(FATAL_ERROR
		"The Java differential's driver is Python. Install Python 3 or "
		"configure with -DJCE_BUILD_SCRIPT_JAVA=OFF.")
endif()

# ── The reference side: the Lua VM, with the shared recording mock host ──
add_executable(jce_script_java_lua_reference
	"${CMAKE_CURRENT_SOURCE_DIR}/jce_java_diff_lua_reference.gen.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/jce_java_diff_host.gen.c")
set_target_properties(jce_script_java_lua_reference PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
jce_configure_test_link_pool(jce_script_java_lua_reference)
target_include_directories(jce_script_java_lua_reference PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/engine/include")
target_link_libraries(jce_script_java_lua_reference PRIVATE
	jce_core jce_script)
add_dependencies(jce_tests jce_script_java_lua_reference)

# ── The Java side's test-only natives: the SAME mock host, a second time ──
#
# Compiled again rather than shared as a library, because the two sides are
# two processes: each needs its own copy of the same object code.  "Its own
# copy of the SAME source" is the property that matters — a trace difference
# is then a difference in what the bindings did, never in what the mock did.
add_library(jce_script_java_diff SHARED
	"${CMAKE_CURRENT_SOURCE_DIR}/jce_java_diff_jni.gen.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/jce_java_diff_host.gen.c")
set_target_properties(jce_script_java_diff PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	WINDOWS_EXPORT_ALL_SYMBOLS OFF
	DEFINE_SYMBOL       ""
	LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:jce_script_java>"
	RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:jce_script_java>")
target_include_directories(jce_script_java_diff PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/engine/include"
	"${JCE_JNI_INCLUDE_DIR}" "${JCE_JNI_MD_INCLUDE_DIR}")
target_link_libraries(jce_script_java_diff PRIVATE jce_core)
add_dependencies(jce_tests jce_script_java_diff jce_script_java)

# ── The comparison ──
add_test(NAME test_jce_script_java_differential
	COMMAND "${Python3_EXECUTABLE}"
	        "${CMAKE_CURRENT_SOURCE_DIR}/run_differential.py"
	        --lua-exe    "$<TARGET_FILE:jce_script_java_lua_reference>"
	        --script-lib "$<TARGET_FILE:jce_script_java>"
	        --diff-lib   "$<TARGET_FILE:jce_script_java_diff>"
	        --work-dir   "${CMAKE_BINARY_DIR}/tests/java_differential")
#
# A bare add_test() and not jce_add_unit_test: this test's COMMAND is the
# Python comparator, not a Unity executable, and jce_add_unit_test builds an
# executable and runs it directly.  tests/CMakeLists.txt:280 refuses a bare
# add_test() that is not armed with JCE_TEST_ENVIRONMENT — the Tracy listener
# would otherwise open a socket on every interface and raise a Windows
# firewall dialog on every rebuild — so both halves of that registration are
# done here explicitly.
set_tests_properties(test_jce_script_java_differential PROPERTIES
	TIMEOUT     300
	LABELS      "unit"
	ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
	test_jce_script_java_differential)

# ----------------------------------------------------------------------- #
#  The OTHER direction's suites.                                          #
#                                                                         #
#  tests/scripting/CMakeLists.txt globs ONE level deep, so this           #
#  subdirectory is added by name.  It is a subdirectory rather than more  #
#  files here because the two efforts test opposite directions: above,    #
#  the Java surface calling DOWN into the engine; below, the engine       #
#  calling UP into a JVM through JceScriptVM.                             #
# ----------------------------------------------------------------------- #
add_subdirectory(vm)
