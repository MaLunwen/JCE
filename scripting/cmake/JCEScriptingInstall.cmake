# scripting/cmake/JCEScriptingInstall.cmake
#
# THE SDK SIDE OF scripting/.
#
# Everything else in this directory is about building the language-binding
# layer.  This file is about SHIPPING it, and it exists because until it did,
# the answer to "can a project that is not this repository run a Python, Java
# or C++ script?" was NO — not "it is hard", not "it is undocumented", but no.
# Measured, on the SDK this repo produced on 2026-08-14 at commit d3ecc452:
#
#     find_package(JCE REQUIRED)   ->  JCE::JCE  yes
#                                      JCE::ScriptApi / ScriptVm*  none
#                                      JCE_SCRIPT_PYTHON_PACKAGE_DIR  empty
#
# with no error and no warning, because there was nothing to warn about: the
# SDK's install rules never mentioned scripting/ at all.  A consumer wrote
# `if(TARGET jce_script_vm_python)`, got FALSE, and shipped a game on Lua.
#
# ── WHY THE RULES ARE HERE AND NOT IN engine/cmake/JCESDKInstall.cmake ──
#
# check_dependency_boundaries.py, check 3: nothing under engine/
# may name a scripting/ path or a target scripting/ declares — in an #include
# OR in a CMake file.  `install(TARGETS jce_script_api ...)` written into
# JCESDKInstall.cmake is a violation the gate reports by name.  The direction
# is scripting -> engine, so the install rules for scripting's artefacts are
# scripting's own, and the only thing the engine side contributes is one
# OPTIONAL include() in cmake/JCEConfig.cmake.in that names a FILE, not a
# target and not a path in this tree.
#
# ── WHAT A CONSUMER GETS ──
#
# An installed lib/cmake/JCE/JCEScripting.cmake (generated from
# JCEScripting.cmake.in beside this file) which declares imported targets,
# their in-tree aliases, and the paths of the runtime pieces that are loaded
# by name rather than linked.  See that template for the consumer contract.

include_guard(GLOBAL)

# Captured at include() time, not read inside the function.  What
# CMAKE_CURRENT_LIST_DIR means inside a function body is a detail of which
# file the function was DEFINED in versus called from, and a template that
# resolves to the wrong directory fails as "file not found" pointing at a
# path nobody wrote.  One variable, set where the answer is unambiguous.
set(_JCE_SCRIPTING_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# --------------------------------------------------------------------- #
#  jce_scripting_register_sdk_install()                                  #
#                                                                        #
#  Called from scripting/CMakeLists.txt AFTER every backend subdirectory  #
#  has been added, because it asks `if(TARGET ...)` about each one and a  #
#  target declared by a later add_subdirectory() reads as absent.  That   #
#  is not a hypothetical here: the same mistake, one level up, made the   #
#  editor link none of these backends while configuring green.            #
# --------------------------------------------------------------------- #
function(jce_scripting_register_sdk_install)
	if(NOT JCE_ENABLE_SDK_INSTALL)
		return()
	endif()

	include(GNUInstallDirs)

	set(_scripting_root "${CMAKE_CURRENT_SOURCE_DIR}")
	# Beside the engine's fat libs, in the same per-config directory
	# JCEConfig.cmake already probes for Release/ and Debug/.
	set(_libdest   "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")
	set(_incdest   "${CMAKE_INSTALL_INCLUDEDIR}")
	set(_sharedest "${CMAKE_INSTALL_DATAROOTDIR}/jce/scripting")

	set(_have_api    0)
	set(_have_c      0)
	set(_have_cpp    0)
	set(_have_python 0)
	set(_have_java   0)
	set(_python_version "")

	# ---------------------------------------------------------------- #
	#  jce_script_api — the C ABI shared library.                       #
	#                                                                   #
	#  RUNTIME and ARCHIVE both land in lib/<CONFIG>/: on Windows the    #
	#  .dll and its import .lib belong together, and the .dll's real     #
	#  home is beside the CONSUMER's executable, which is a build step   #
	#  of theirs (jce_script_stage_runtime) rather than a location in    #
	#  this tree.  bin/ is the HOST TOOLS directory — jce_cook and       #
	#  friends, which run on the machine that builds the game — and a    #
	#  redistributable that the shipped game loads is not one of those.  #
	# ---------------------------------------------------------------- #
	if(TARGET jce_script_api)
		set(_have_api 1)
		install(TARGETS jce_script_api
			RUNTIME DESTINATION "${_libdest}"
			LIBRARY DESTINATION "${_libdest}"
			ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/c_abi/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h")
	endif()

	# ---------------------------------------------------------------- #
	#  scripting/cpp — three targets, two of them INTERFACE.            #
	#                                                                   #
	#  jce_script_vm_cpp is the only compiled one and the only one with  #
	#  an archive to install; the wrapper and the module target are      #
	#  headers plus a link rule, so shipping them is shipping the        #
	#  include tree and re-declaring the rule on the consumer side.      #
	#                                                                    #
	#  Read by check_sdk_scripting_export.py, which requires  #
	#  every library scripting/ declares to be installed here or exempted #
	#  here WITH A REASON.  An INTERFACE target has no file; "install" it  #
	#  and CMake asks for an EXPORT set this package does not use.        #
	#                                                                    #
	# SDK-EXEMPT: jce_script_cpp — INTERFACE: no artefact exists to
	#   install.  It is headers plus "link jce_script_api", and both halves
	#   ship: the headers in the install(DIRECTORY) below, the link rule
	#   re-declared in JCEScripting.cmake.in.  Compiling it instead would
	#   move sizeof(JceScriptHost) out of the caller's TU, which is the one
	#   thing the short-host ABI exists to keep there.
	# SDK-EXEMPT: jce_script_cpp_module — INTERFACE, same as above, and it
	#   must stay one: a module that carried the registry would publish its
	#   classes into a registry the engine never reads.
	# ---------------------------------------------------------------- #
	if(TARGET jce_script_vm_cpp)
		set(_have_cpp 1)
		install(TARGETS jce_script_vm_cpp
			ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/cpp/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
	endif()

	# ---------------------------------------------------------------- #
	#  scripting/c — C as a DRIVER language.                            #
	#                                                                   #
	#  ONE archive and ONE header tree, and the header tree is the half  #
	#  that matters most here: the whole point of scripting/c is that a  #
	#  C author includes a header that admits it is for them.  An SDK    #
	#  that shipped jce_script_vm_c.lib and left                          #
	#  <jce/script_vm/jce_script_vm_c.h> behind would put every C        #
	#  consumer back on jce_script_vm_cpp.h, which is the documentation  #
	#  defect this language was added to close.                          #
	#                                                                   #
	#  jce_script_vm_c PUBLIC-links jce_script_vm_cpp, so the consumer   #
	#  side re-declares that edge in JCEScripting.cmake.in — an SDK      #
	#  that shipped one without the other links with an unresolved       #
	#  jce_script_vm_cpp_create_for.                                     #
	#                                                                   #
	# SDK-EXEMPT: jce_script_c_module — INTERFACE: no artefact exists to
	#   install.  It is headers plus "link jce_script_api", and both halves
	#   ship: the headers in the install(DIRECTORY) below, the link rule
	#   re-declared in JCEScripting.cmake.in.  It must STAY an INTERFACE
	#   target: a module that carried the registry would publish its classes
	#   into a registry the engine never reads.
	# ---------------------------------------------------------------- #
	if(TARGET jce_script_vm_c)
		set(_have_c 1)
		install(TARGETS jce_script_vm_c
			ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/c/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h")
	endif()

	# ---------------------------------------------------------------- #
	#  scripting/python — the archive, the shim header, and THE WHEEL.  #
	#                                                                   #
	#  jce_script/ is pure Python with no build step, and the shim       #
	#  refuses to create a VM without it (vm.py is not optional).  An    #
	#  SDK that shipped jce_script_vm_python.lib and not the package     #
	#  would produce a game that links, boots, and fails at the first    #
	#  Python script with an ImportError from inside the engine.         #
	#                                                                   #
	#  THE HEADER IS INSTALLED FLAT, into include/ and not into          #
	#  include/jce/script_vm/ where the Java and C++ shim headers go.    #
	#  That is deliberate and it is not tidy: in-tree the target's       #
	#  PUBLIC include directory is scripting/python/src, so every        #
	#  existing consumer writes `#include "jce_script_vm_python.h"`.     #
	#  Namespacing it in the SDK only would give the same header two     #
	#  spellings depending on how the project is built, which is the     #
	#  trap this whole file exists to remove.  The inconsistency is      #
	#  scripting/python's to fix in-tree, and the SDK follows it.        #
	# ---------------------------------------------------------------- #
	if(TARGET jce_script_vm_python)
		set(_have_python 1)
		# Published by scripting/python/CMakeLists.txt, which is where the
		# ONE find_package(Python3) probe lives.  Not re-probed here: two
		# probes can disagree, and then the SDK advertises a CPython the
		# archive was not compiled against — the same "two probes" hazard
		# editor/CMakeLists.txt calls out for the JDK.
		set(_python_version "${JCE_SCRIPT_PYTHON_VERSION}")
		install(TARGETS jce_script_vm_python
			ARCHIVE DESTINATION "${_libdest}")
		install(FILES "${_scripting_root}/python/src/jce_script_vm_python.h"
			DESTINATION "${_incdest}")
	endif()

	# The package ships whenever it exists, INCLUDING when the VM shim was
	# skipped for want of an embeddable CPython: the two halves are
	# independent directions (a script calling down vs the engine calling
	# up), and the wheel is what a tool, a test or an external process uses
	# to talk to a running engine.  __pycache__ is excluded because a .pyc
	# is invalidated by mtime+size and install() does not preserve mtime
	# reliably across hosts — a stale one shipped in an SDK is a bug report
	# nobody can reproduce.
	if(IS_DIRECTORY "${_scripting_root}/python/jce_script")
		install(DIRECTORY "${_scripting_root}/python/jce_script"
			DESTINATION "${_sharedest}/python"
			PATTERN "__pycache__" EXCLUDE)
	endif()

	# ---------------------------------------------------------------- #
	#  scripting/java — the archive, the header, the JNI shim, and the  #
	#  COMPILED CLASSES.                                                #
	#                                                                   #
	#  scripting/java/CMakeLists.txt states, correctly, that javac is    #
	#  never run from CMake so that a machine with a C toolchain and no  #
	#  JDK can still build every native target it declares.  That rule   #
	#  is about the ENGINE BUILD.  This block runs only when             #
	#  JCE_ENABLE_SDK_INSTALL is on AND the Java backend was built,      #
	#  which already required a JDK's headers — and an SDK that shipped  #
	#  jce_script_java.dll but not com/jce/script/*.class would hand     #
	#  every consumer the same javac invocation to rediscover.  The      #
	#  editor does exactly this already (editor/CMakeLists.txt), for the #
	#  same reason and with the same script.                             #
	# ---------------------------------------------------------------- #
	if(TARGET jce_script_vm_java)
		set(_have_java 1)
		install(TARGETS jce_script_vm_java
			ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/java/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h")
	endif()

	if(TARGET jce_script_java)
		install(TARGETS jce_script_java
			RUNTIME DESTINATION "${_libdest}"
			LIBRARY DESTINATION "${_libdest}"
			ARCHIVE DESTINATION "${_libdest}")

		find_package(Python3 COMPONENTS Interpreter QUIET)
		if(NOT Python3_Interpreter_FOUND)
			# A warning and not an error: the native half still ships and a
			# consumer with a JDK can compile the classes themselves.  Silence
			# is what is forbidden — an SDK missing the classes produces
			# "ClassNotFoundException: com.jce.script.JceScript" at the first
			# Java script, in the consumer's process, hours away from here.
			message(WARNING
				"JCE SDK: the Java scripting CLASSES will not be installed — "
				"they are compiled by scripting/java/build_java.py and this "
				"configure found no Python 3 interpreter. The SDK will ship "
				"jce_script_java but no com/jce/script/*.class, and "
				"JCE_SCRIPT_JAVA_CLASS_PATH will be empty for consumers.")
		else()
			set(_java_out "${CMAKE_CURRENT_BINARY_DIR}/sdk_java_classes")
			file(GLOB_RECURSE _java_srcs CONFIGURE_DEPENDS
				"${_scripting_root}/java/src/main/java/*.java")
			add_custom_command(
				OUTPUT  "${_java_out}/.stamp"
				COMMAND "${Python3_EXECUTABLE}"
				        "${_scripting_root}/java/build_java.py"
				        --out "${_java_out}" --no-jar
				COMMAND "${CMAKE_COMMAND}" -E touch "${_java_out}/.stamp"
				DEPENDS ${_java_srcs}
				COMMENT "Compiling the Java scripting classes for the SDK"
				VERBATIM)
			# ALL, because `cmake --install` runs no build: a class tree that
			# is produced only when somebody happens to build a named target
			# is a class tree the SDK installs empty on a clean tree.
			add_custom_target(jce_sdk_java_classes ALL
				DEPENDS "${_java_out}/.stamp")
			install(DIRECTORY "${_java_out}/classes"
				DESTINATION "${_sharedest}/java")
			unset(_java_srcs)
			unset(_java_out)
		endif()
	endif()

	# ---------------------------------------------------------------- #
	#  The consumer-facing config fragment.                             #
	# ---------------------------------------------------------------- #
	# NOT REACHABLE TODAY, and said so rather than implied: scripting/cpp
	# declares jce_script_vm_cpp OUTSIDE its jce_script_api guard on purpose
	# ("turning JCE_BUILD_SCRIPT_API_SHARED off must not silently take the VM
	# with it"), so _have_cpp is 1 even with all three options off.  Measured:
	# -DJCE_BUILD_SCRIPT_API_SHARED=OFF -DJCE_BUILD_SCRIPT_VM_PYTHON=OFF
	# -DJCE_BUILD_SCRIPT_JAVA=OFF still prints "api=0 cpp=1 python=0 java=0".
	# The branch stays because the condition it tests is the real one — is
	# there anything to ship — and because a checkout without scripting/cpp,
	# or a future gate on that target, makes it live without anybody
	# remembering this file.
	if(NOT (_have_api OR _have_c OR _have_cpp OR _have_python OR _have_java))
		message(STATUS
			"JCE SDK: no scripting backend was built — this SDK ships Lua "
			"only (JCEScripting.cmake will not be installed). "
			"find_package(JCE COMPONENTS Script*) refuses it by name.")
		return()
	endif()

	set(JCE_SDK_SCRIPT_HAVE_API    "${_have_api}")
	set(JCE_SDK_SCRIPT_HAVE_C      "${_have_c}")
	set(JCE_SDK_SCRIPT_HAVE_CPP    "${_have_cpp}")
	set(JCE_SDK_SCRIPT_HAVE_PYTHON "${_have_python}")
	set(JCE_SDK_SCRIPT_HAVE_JAVA   "${_have_java}")
	set(JCE_SDK_SCRIPT_PYTHON_VERSION "${_python_version}")

	configure_file(
		"${_JCE_SCRIPTING_CMAKE_DIR}/JCEScripting.cmake.in"
		"${CMAKE_CURRENT_BINARY_DIR}/JCEScripting.cmake"
		@ONLY)
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/JCEScripting.cmake"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/JCE")

	message(STATUS
		"JCE SDK: scripting install rules registered "
		"(api=${_have_api} c=${_have_c} cpp=${_have_cpp} "
		"python=${_have_python} java=${_have_java})")
endfunction()
