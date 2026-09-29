cmake_minimum_required(VERSION 3.20)

# ----------------------------------------------------------------------- #
#  tests/scripting/java/vm — the Java JceScriptVM's suites.               #
#                                                                         #
#  Batch 1 spent the one free correctness oracle and deleted it, so LUA    #
#  IS THE REFERENCE LIFECYCLE.  The differential expresses one script's    #
#  semantics in both languages, drives both through the ENGINE'S OWN entry #
#  points, and compares the sequence of host calls and the typed           #
#  observations.                                                           #
#                                                                         #
#  ONE driver binary, run TWICE as two processes.  A JVM that aborts       #
#  (which is what a JNI misuse does under -Xcheck:jni) must not be able to #
#  take the reference stream with it, and the two exit codes have to be    #
#  readable on their own.                                                  #
# ----------------------------------------------------------------------- #

if(NOT TARGET jce_script_vm_java)
	message(STATUS
		"JCE tests: Java JceScriptVM suites SKIPPED — jce_script_vm_java "
		"was not configured")
	return()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
	message(FATAL_ERROR
		"The Java lifecycle differential's comparator is Python. Install "
		"Python 3 or configure with -DJCE_BUILD_SCRIPT_JAVA=OFF.")
endif()

# The JDK this suite runs against, derived from the ONE place the configure
# already found it.  Deriving beats a second probe: two probes can disagree,
# and then the headers we compiled against and the JVM we start are different
# JDKs — which shows up as an UnsatisfiedLinkError with no obvious cause.
get_filename_component(_jce_java_home "${JCE_JNI_INCLUDE_DIR}" DIRECTORY)

# ── The driver: one C program, both languages ──
add_executable(jce_script_vm_java_lifecycle
	"${CMAKE_CURRENT_SOURCE_DIR}/jce_vm_lifecycle_driver.c"
	"${CMAKE_CURRENT_SOURCE_DIR}/jce_vm_lifecycle_host.c")
set_target_properties(jce_script_vm_java_lifecycle PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
jce_configure_test_link_pool(jce_script_vm_java_lifecycle)
target_compile_definitions(jce_script_vm_java_lifecycle PRIVATE
	JCE_LIFECYCLE_WITH_JAVA=1)
target_include_directories(jce_script_vm_java_lifecycle PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/engine/include")
target_link_libraries(jce_script_vm_java_lifecycle PRIVATE
	jce_core jce_script jce_script_vm_java)
add_dependencies(jce_tests jce_script_vm_java_lifecycle)

add_test(NAME test_jce_script_vm_java_lifecycle
	COMMAND "${Python3_EXECUTABLE}"
	        "${CMAKE_CURRENT_SOURCE_DIR}/run_lifecycle_differential.py"
	        --driver   "$<TARGET_FILE:jce_script_vm_java_lifecycle>"
	        --scripts  "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
	        --work-dir "${CMAKE_BINARY_DIR}/tests/java_vm_lifecycle")
#
# A bare add_test() and not jce_add_unit_test: the COMMAND is the Python
# comparator, not a Unity executable.  tests/CMakeLists.txt refuses a bare
# add_test() that is not armed with JCE_TEST_ENVIRONMENT (the Tracy listener
# would otherwise open a socket on every interface), so both halves of that
# registration are done explicitly — and tools/lint/check_jce_tests_closure.py
# is satisfied by the add_dependencies above, which names the real build input.
set_tests_properties(test_jce_script_vm_java_lifecycle PROPERTIES
	TIMEOUT     600
	LABELS      "unit"
	ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
	test_jce_script_vm_java_lifecycle)

# ── The scripting classes the unit test needs at runtime ──
#
# build_java.py, not javac from here: the Java build belongs to that script
# (a JDK must not become a configure-time input of the engine build) and
# having two places that know how to compile these classes is one place too
# many.
set(_jce_java_vm_classes "${CMAKE_BINARY_DIR}/tests/java_vm_classes")
file(GLOB_RECURSE _jce_java_vm_srcs CONFIGURE_DEPENDS
	"${CMAKE_SOURCE_DIR}/scripting/java/src/main/java/*.java")
add_custom_command(
	OUTPUT  "${_jce_java_vm_classes}/.stamp"
	COMMAND "${Python3_EXECUTABLE}"
	        "${CMAKE_SOURCE_DIR}/scripting/java/build_java.py"
	        --out "${_jce_java_vm_classes}" --no-jar
	COMMAND "${CMAKE_COMMAND}" -E touch "${_jce_java_vm_classes}/.stamp"
	DEPENDS ${_jce_java_vm_srcs}
	COMMENT "Compiling the Java scripting classes for the VM tests"
	VERBATIM)
add_custom_target(jce_script_vm_java_classes
	DEPENDS "${_jce_java_vm_classes}/.stamp")
add_dependencies(jce_tests jce_script_vm_java_classes)

# ── The pre-compiled fixture: the shape a shipped Java game actually uses ──
#
# javac IS invoked directly here, unlike the block above, and the difference is
# deliberate: build_java.py owns the RUNTIME LIBRARY build (a fixed source root,
# a jar, an API version) and this is a single test fixture that must not become
# part of that product.  It stays a BUILD-time step — the rule the block above
# protects is that a JDK is not a CONFIGURE-time input, and _jce_java_home was
# already derived from JCE_JNI_INCLUDE_DIR for the JVM path, not probed again.
#
# THE OUTPUT IS RENAMED.  Precompiled.class becomes shipped_asset.class, so the
# file the test loads has a stem that matches no class name.  A .class carries
# its own identity in its bytes; a .java does not, and the source path derives
# the class name from the PATH it was given.  Loading the renamed file is
# therefore the one arrangement in which success PROVES the bytecode branch ran
# — the source branch could not have produced a class called Precompiled from a
# file called shipped_asset, and could not have compiled binary input at all.
set(_jce_java_precompiled "${CMAKE_BINARY_DIR}/tests/java_vm_precompiled")
add_custom_command(
	OUTPUT  "${_jce_java_precompiled}/shipped_asset.class"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${_jce_java_precompiled}"
	COMMAND "${_jce_java_home}/bin/javac${CMAKE_EXECUTABLE_SUFFIX}"
	        --release 11
	        -cp "${_jce_java_vm_classes}/classes"
	        -d "${_jce_java_precompiled}"
	        "${CMAKE_CURRENT_SOURCE_DIR}/precompiled/Precompiled.java"
	COMMAND "${CMAKE_COMMAND}" -E rename
	        "${_jce_java_precompiled}/Precompiled.class"
	        "${_jce_java_precompiled}/shipped_asset.class"
	DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/precompiled/Precompiled.java"
	        "${_jce_java_vm_classes}/.stamp"
	COMMENT "Compiling the pre-compiled (.class) Java script fixture"
	VERBATIM)
add_custom_target(jce_script_vm_java_precompiled
	DEPENDS "${_jce_java_precompiled}/shipped_asset.class")
add_dependencies(jce_tests jce_script_vm_java_precompiled)

# ── The unit suite: everything the differential structurally cannot see ──
jce_add_unit_test(test_jce_script_vm_java
	SOURCES  "${CMAKE_CURRENT_SOURCE_DIR}/test_jce_script_vm_java.c"
	LINK     jce_core jce_script jce_script_vm_java
	INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_jce_script_vm_java PRIVATE
	"JCE_JAVA_VM_TEST_CLASSES=\"${_jce_java_vm_classes}/classes\""
	"JCE_JAVA_VM_TEST_JVM_WIN=\"${_jce_java_home}/bin/server/jvm.dll\""
	"JCE_JAVA_VM_TEST_JVM_NIX=\"${_jce_java_home}/lib/server/libjvm.so\""
	"JCE_JAVA_VM_TEST_JVM_MAC=\"${_jce_java_home}/lib/server/libjvm.dylib\""
	# The two halves of Java scripting meet in this process: the JNI shim the
	# JVM loads, and the C ABI library it imports.  Passed as paths rather
	# than found at run time, because "which build of the shim did this test
	# load" is the first question when it fails on another machine.
	"JCE_JAVA_VM_TEST_SCRIPT_LIB=\"$<TARGET_FILE:jce_script_java>\""
	"JCE_JAVA_VM_TEST_SCRIPT_API_LIB=\"$<TARGET_FILE:jce_script_api>\""
	# The directory holding shipped_asset.class (see the fixture block above).
	"JCE_JAVA_VM_TEST_PRECOMPILED_DIR=\"${_jce_java_precompiled}\"")
target_link_libraries(test_jce_script_vm_java PRIVATE jce_platform)
add_dependencies(test_jce_script_vm_java jce_script_vm_java_classes
	jce_script_vm_java_precompiled jce_script_java jce_script_api)

# ── The local-reference gate: static, because dynamic does not work ──
#
# Measured by the batch-1 agent on JDK 21.0.9: a shim leaking 200,000 local
# references inside ONE native frame runs clean under -Xcheck:jni.  So the
# discipline is held structurally (every slot inside PushLocalFrame /
# PopLocalFrame) and the structure is what gets checked.
add_test(NAME test_jce_script_vm_java_local_frames
	COMMAND "${Python3_EXECUTABLE}"
	        "${CMAKE_CURRENT_SOURCE_DIR}/test_local_frames.py")
set_tests_properties(test_jce_script_vm_java_local_frames PROPERTIES
	TIMEOUT     120
	LABELS      "unit"
	ENVIRONMENT "${JCE_TEST_ENVIRONMENT}")
set_property(GLOBAL APPEND PROPERTY JCE_TESTS_WITH_ENV
	test_jce_script_vm_java_local_frames)
