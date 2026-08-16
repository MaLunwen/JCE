cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  jce_script_java — the JNI shim for com.jce.script.JceScript.       #
#                                                                     #
#  IT IS OFF BY DEFAULT, AND THAT IS THE POINT.  scripting/           #
#  CMakeLists.txt discovers every subdirectory by glob, so this file  #
#  is add_subdirectory()'d in EVERY configure of the project.  If it  #
#  probed for a JDK before the guard below, a JDK would become a      #
#  configure-time input of the engine build on every machine — which  #
#  is exactly what "the JVM must not become an engine build           #
#  dependency" forbids.  The guard is therefore the FIRST thing here, #
#  ahead of every find_path.                                          #
#                                                                     #
#  Verified rather than asserted: configuring with the option unset   #
#  leaves no JCE_JNI_* entry in CMakeCache.txt.  A `find_path` moved  #
#  above the return() puts them there, which is the mutation.         #
#                                                                     #
#  ONLY the C shim is built here.  javac and jar are NOT run from     #
#  CMake — scripting/java/build_java.py owns the Java build, so a     #
#  machine with a C toolchain and no JDK can still build every native #
#  target this directory declares.                                    #
# ------------------------------------------------------------------ #

option(JCE_BUILD_SCRIPT_JAVA
	"Build the Java scripting binding's JNI shim (needs a JDK's headers)." OFF)
if(NOT JCE_BUILD_SCRIPT_JAVA)
	return()
endif()

if(NOT TARGET jce_script_api)
	message(FATAL_ERROR
		"JCE_BUILD_SCRIPT_JAVA=ON but jce_script_api is not built. The Java "
		"binding calls that library and nothing else; configure with "
		"-DJCE_BUILD_SCRIPT_API_SHARED=ON.")
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
if(NOT JCE_JNI_INCLUDE_DIR OR NOT JCE_JNI_MD_INCLUDE_DIR)
	message(FATAL_ERROR
		"JCE_BUILD_SCRIPT_JAVA=ON but jni.h / jni_md.h were not found. Set "
		"JAVA_HOME to a JDK, or pass -DJCE_JNI_INCLUDE_DIR and "
		"-DJCE_JNI_MD_INCLUDE_DIR.")
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
