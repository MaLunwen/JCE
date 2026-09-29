cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  jce_script_java — the JNI shim for com.jce.script.JceScript.       #
#                                                                     #
#  ON WHEN A JDK IS THERE, OFF WHEN IT IS NOT — the same shape        #
#  scripting/python/CMakeLists.txt has always had: option defaults    #
#  ON, probe QUIETLY, return with a STATUS line when the toolchain    #
#  is absent.  A JDK is therefore an OPTIONAL dependency, never a     #
#  required one, which is the constraint that matters.                #
#                                                                     #
#  IT USED TO DEFAULT OFF, on the reasoning that probing at all would #
#  make a JDK "a configure-time input of the engine build on every    #
#  machine".  That conflates required with detected.  One directory   #
#  away, find_package(Python3 COMPONENTS Development.Embed QUIET) has #
#  been running in every configure of this repository and putting     #
#  Python3_* into every CMakeCache, and no one calls CPython an       #
#  engine build dependency — because not finding it is not an error.  #
#                                                                     #
#  What the old note actually verified was narrower than what it      #
#  claimed: that with the option OFF, no JCE_JNI_* reaches the cache. #
#  True, and not evidence that an optional probe is harmful.          #
#                                                                     #
#  The cost of the old default was measured, not theorised: the       #
#  default configure registers 351 tests and this one makes it 355.   #
#  The four are the Java VM tests and the JNI/Lua DIFFERENTIAL, the   #
#  only check on the generated Java bindings — never built on a       #
#  machine that had JAVA_HOME set the whole time.                     #
#                                                                     #
#  -DJCE_BUILD_SCRIPT_JAVA=OFF still skips the probe entirely, for a  #
#  machine that has a JDK and does not want it used.                  #
#                                                                     #
#  ONLY the C shim is built here.  javac and jar are NOT run from     #
#  CMake — scripting/java/build_java.py owns the Java build, so a     #
#  machine with a C toolchain and no JDK can still build every native #
#  target this directory declares.                                    #
# ------------------------------------------------------------------ #

option(JCE_BUILD_SCRIPT_JAVA
	"Build the Java scripting binding's JNI shim (needs a JDK's headers)." ON)
if(NOT JCE_BUILD_SCRIPT_JAVA)
	message(STATUS
		"JCE scripting: Java JNI shim SKIPPED (-DJCE_BUILD_SCRIPT_JAVA=OFF)")
	return()
endif()

# A STATUS and not a FATAL_ERROR, for the same reason as the JDK probe below:
# with the option ON by default, a configure that turned the C ABI off must
# not fail here — it asked for no Java and should be told, not stopped.
if(NOT TARGET jce_script_api)
	message(STATUS
		"JCE scripting: Java JNI shim SKIPPED — jce_script_api is not built "
		"(-DJCE_BUILD_SCRIPT_API_SHARED=OFF). The Java binding calls that "
		"library and nothing else.")
	return()
endif()

# jni.h + jni_md.h ONLY.  Deliberately not find_package(JNI), which also
# insists on jvm.lib: this shim is loaded BY a JVM and never links one, so
# requiring the launcher library would refuse a header-only JDK for a
# dependency that does not exist.
find_path(JCE_JNI_INCLUDE_DIR jni.h
	HINTS "$ENV{JAVA_HOME}/include" "${JAVA_HOME}/include"
	DOC   "Directory containing jni.h")
find_path(JCE_JNI_MD_INCLUDE_DIR jni_md.h
	HINTS "$ENV{JAVA_HOME}/include/win32"
	      "$ENV{JAVA_HOME}/include/linux"
	      "$ENV{JAVA_HOME}/include/darwin"
	      "${JAVA_HOME}/include/win32"
	      "${JAVA_HOME}/include/linux"
	      "${JAVA_HOME}/include/darwin"
	DOC   "Directory containing the platform jni_md.h")
mark_as_advanced(JCE_JNI_INCLUDE_DIR JCE_JNI_MD_INCLUDE_DIR)
if(NOT JCE_JNI_INCLUDE_DIR OR NOT JCE_JNI_MD_INCLUDE_DIR)
	# SKIPPED, not FATAL: the option is ON by default now, so "no JDK on this
	# machine" is the ordinary case and must not fail a build that never asked
	# for Java.  Exactly what scripting/python does when there is no embeddable
	# CPython.  Pass -DJCE_BUILD_SCRIPT_JAVA=ON explicitly AND look for this
	# line if you expected the backend and did not get it.
	message(STATUS
		"JCE scripting: Java JNI shim SKIPPED — jni.h / jni_md.h not found. "
		"Set JAVA_HOME to a JDK, or pass -DJCE_JNI_INCLUDE_DIR and "
		"-DJCE_JNI_MD_INCLUDE_DIR.")
	return()
endif()

add_library(jce_script_java SHARED
	"${CMAKE_CURRENT_SOURCE_DIR}/native/jce_script_jni.gen.c")

set_target_properties(jce_script_java PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	# JNIEXPORT is the export mechanism (__declspec(dllexport) on Windows,
	# default visibility elsewhere).  ON would export every symbol in the
	# object and make the surface wider than the surface.
	WINDOWS_EXPORT_ALL_SYMBOLS OFF
	DEFINE_SYMBOL       ""
	# Beside jce_script_api, so the loader finds it without a PATH edit: on
	# Windows a DLL's dependencies are searched in the DLL's own directory.
	LIBRARY_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:jce_script_api>"
	RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:jce_script_api>")

target_include_directories(jce_script_java PRIVATE
	"${JCE_JNI_INCLUDE_DIR}" "${JCE_JNI_MD_INCLUDE_DIR}")
target_link_libraries(jce_script_java PRIVATE jce_script_api)

jce_target_warnings(jce_script_java STRICT)

# -- The runtime inputs this backend cannot discover ------------------
#
# Published as CACHE INTERNAL the way scripting/python publishes
# JCE_SCRIPT_PYTHON_PACKAGE_DIR, so jce_script_enable() -- which only ever
# CONSUMES the variables a roster NAMES -- finds them in-tree exactly as it
# finds them from an installed SDK.  The provider is whoever knows where the
# artefact is, and for all three of these that is this directory.
#
# THE JVM IS DERIVED FROM THE PROBE THAT FOUND jni.h, never from a second look
# at JAVA_HOME.  editor/CMakeLists.txt got this right first and said why: two
# probes can disagree, and then the jni.h this backend compiled against and the
# JVM it starts are different JDKs -- an UnsatisfiedLinkError with no obvious
# cause.  It is only the FALLBACK in any case: the generated shim prefers
# JAVA_HOME at RUN time, because the JVM belongs to whoever runs the game.
get_filename_component(_jce_java_home "${JCE_JNI_INCLUDE_DIR}" DIRECTORY)
if(WIN32)
	set(_jce_jvm "${_jce_java_home}/bin/server/jvm.dll")
elseif(APPLE)
	set(_jce_jvm "${_jce_java_home}/lib/server/libjvm.dylib")
else()
	set(_jce_jvm "${_jce_java_home}/lib/server/libjvm.so")
endif()
if(NOT EXISTS "${_jce_jvm}")
	set(_jce_jvm "")
endif()
set(JCE_SCRIPT_JAVA_JVM_LIBRARY "${_jce_jvm}" CACHE INTERNAL
	"Absolute JVM library the java backend opens at runtime (fallback)")

# ---- the compiled com.jce.script classes ------------------------------ #
#
# COMPILED HERE, ONCE.  This was the same build_java.py call in three places --
# the editor's binary dir for Play, the SDK install tree, and a sample project's
# in-tree branch -- which is three chances for editor Play and a shipped exe to
# disagree about what com.jce.script is.
#
# javac is NOT a requirement of the engine build: absent, the classes are simply
# not produced and the path stays empty, which the consumer reports by name.
# Same shape as the jni.h probe above.
find_package(Python3 COMPONENTS Interpreter QUIET)
find_program(JCE_JAVAC_EXECUTABLE javac
	HINTS "${_jce_java_home}/bin" "$ENV{JAVA_HOME}/bin"
	DOC "javac used to compile the com.jce.script classes")
mark_as_advanced(JCE_JAVAC_EXECUTABLE)
set(_jce_java_classes "")
if(Python3_Interpreter_FOUND AND JCE_JAVAC_EXECUTABLE)
	set(_jce_java_classes "${CMAKE_CURRENT_BINARY_DIR}/java_classes")
	file(GLOB_RECURSE _jce_java_srcs CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/main/java/*.java")
	add_custom_command(
		OUTPUT  "${_jce_java_classes}/.stamp"
		COMMAND "${Python3_EXECUTABLE}"
		        "${CMAKE_CURRENT_SOURCE_DIR}/build_java.py"
		        --out "${_jce_java_classes}" --no-jar
		COMMAND "${CMAKE_COMMAND}" -E touch "${_jce_java_classes}/.stamp"
		DEPENDS ${_jce_java_srcs}
		COMMENT "Compiling the Java scripting classes"
		VERBATIM)
	# NOTHING LINKS THESE, so the build edge must be add_dependencies() on every
	# consumer.  Without it they are built only when somebody happens to build
	# "all", and the symptom is a green build followed by "no script VM claims
	# .java" at run time.
	add_custom_target(jce_script_java_classes
		DEPENDS "${_jce_java_classes}/.stamp")
	unset(_jce_java_srcs)
else()
	message(STATUS
		"JCE scripting: the Java CLASSES will not be compiled — "
		"Python3 interpreter: ${Python3_Interpreter_FOUND}, javac: "
		"${JCE_JAVAC_EXECUTABLE}. The native half still builds; a .java script "
		"would reach the JVM and die with ClassNotFoundException, so consumers "
		"report the empty class path by name instead of registering.")
endif()
# ".../classes", not the --out directory.  build_java.py's own docstring
# states the layout it produces -- "<out>/classes/com/jce/script/*.class" --
# and this pointed one level above it, so the JVM looked for
# com/jce/script/JceScript.class directly under <out> and found a directory
# named "classes" instead.
#
# The cost was the whole Java backend in an in-tree build: the shim and the
# .dll staged correctly, the VM registered, and then every .java script was
# refused with "com.jce.script.vm.JceScriptRuntime is not on the class path.
# Build the scripting classes with scripting/java/build_java.py" -- advice
# that does not help, because the classes WERE built and were sitting one
# directory below where this pointed.  A packaged game was unaffected: the
# SDK sets this variable from its own install layout, which is correct, so
# java worked in a shipped exe and not in the editor from the same scene.
set(JCE_SCRIPT_JAVA_CLASS_PATH "${_jce_java_classes}/classes" CACHE INTERNAL
	"Directory holding compiled com/jce/script/*.class")
unset(_jce_java_home)
unset(_jce_jvm)
unset(_jce_java_classes)

# The generated shim must actually reach the compiler.  Same reasoning as
# scripting/c_abi's assertion: the generator proves the file is CORRECT and
# says nothing about whether it is BUILT, and a shim that is not built produces
# a DLL that loads and then throws UnsatisfiedLinkError on the first call —
# far from the cause.
get_target_property(_jce_java_srcs jce_script_java SOURCES)
if(NOT _jce_java_srcs MATCHES "jce_script_jni\\.gen\\.c")
	message(FATAL_ERROR
		"jce_script_java sources do not include jce_script_jni.gen.c. "
		"Regenerate with:\n"
		"  python tools/scriptgen/gen_script_bindings.py --write")
endif()
unset(_jce_java_srcs)

message(STATUS "JCE scripting: Java JNI shim ON (jni.h from ${JCE_JNI_INCLUDE_DIR})")

# ------------------------------------------------------------------ #
#  jce_script_vm_java — the OTHER direction: the engine calling UP.   #
#                                                                     #
#  jce_script_java (above) is the script->engine half and links only  #
#  the C ABI shared library, because it is loaded BY a JVM.  This     #
#  target is the engine->script half: it CREATES the JVM, so it links #
#  the engine (jce_script for the JceScriptVM registry, jce_platform  #
#  for jce_library_open) and is STATIC, linked into whatever binary   #
#  wants Java scripts.                                                #
#                                                                     #
#  IT STILL LINKS NO JVM.  The JVM library is opened at RUNTIME by    #
#  absolute path, so the JDK stays a runtime choice a game makes and  #
#  never becomes a link-time input.  That is the same reason the      #
#  find_path above asks for jni.h and not for jvm.lib.                #
#                                                                     #
#  Under the SAME option guard on purpose: this needs the identical   #
#  JNI headers, and a second option would be a second way for the     #
#  engine build to acquire a JDK.  The guard is still the first thing #
#  in this file, which is what                                        #
#  test_script_java_gate.py checks.                                   #
# ------------------------------------------------------------------ #

if(NOT TARGET jce_script)
	message(FATAL_ERROR
		"JCE_BUILD_SCRIPT_JAVA=ON but the jce_script target does not exist. "
		"The Java JceScriptVM registers itself into the engine's script VM "
		"registry and cannot be built without it.")
endif()

add_library(jce_script_vm_java STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/native/jce_script_vm_java.c")

set_target_properties(jce_script_vm_java PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	POSITION_INDEPENDENT_CODE ON)

target_include_directories(jce_script_vm_java
	PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
	        "${CMAKE_SOURCE_DIR}/engine/include"
	PRIVATE "${JCE_JNI_INCLUDE_DIR}" "${JCE_JNI_MD_INCLUDE_DIR}")

target_link_libraries(jce_script_vm_java
	PUBLIC  jce_core jce_script
	PRIVATE jce_platform)

jce_target_warnings(jce_script_vm_java STRICT)

message(STATUS "JCE scripting: Java JceScriptVM ON (engine -> JVM)")
