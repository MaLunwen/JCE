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

	set(_sdk_targets "")
	set(_have_api    0)
	set(_api_static  0)
	set(_have_c      0)
	set(_have_cpp    0)
	set(_have_python 0)
	set(_have_java   0)
	set(_have_js     0)
	set(_have_csharp 0)
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
		list(APPEND _sdk_targets jce_script_api)
		set(_have_api 1)
		get_target_property(_api_type jce_script_api TYPE)
		if(_api_type STREQUAL "STATIC_LIBRARY")
			set(_api_static 1)
		endif()
		unset(_api_type)
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
		list(APPEND _sdk_targets jce_script_vm_cpp)
		set(_have_cpp 1)
		install(TARGETS jce_script_vm_cpp
			ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/cpp/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
	endif()

	# ---------------------------------------------------------------- #
	#  scripting/js — JavaScript over quickjs-ng.                       #
	#                                                                   #
	#  ONE archive and ONE header.  The JS ENGINE is not here and must   #
	#  not be: quickjs-ng is a conan dependency (a LOCAL recipe --       #
	#  conan-center's is Bellard's fork, which refuses msvc), so a       #
	#  consumer resolves it through their OWN conan graph, the same way  #
	#  they resolve lua.  Copying the engine's copy into the SDK would   #
	#  give a consumer two quickjs builds, from two toolchains, and one  #
	#  of them would win at link time without anyone choosing.           #
	# ---------------------------------------------------------------- #
	if(TARGET jce_script_vm_js)
		list(APPEND _sdk_targets jce_script_vm_js)
		set(_have_js 1)
		install(TARGETS jce_script_vm_js ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/js/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h")
		# THE JS ENGINE SHIPS TOO, and this is where js differs from python.
		# python's backend links CPython, which the CONSUMER supplies -- and
		# reasonably, since an embeddable CPython is a thing a machine has.
		# quickjs-ng is a LOCAL recipe that exists only in this repository's
		# conan cache, so "the consumer supplies it" would mean a third-party
		# SDK consumer could not get it AT ALL.  Shipping the archive makes js
		# behave like lua (it just works) instead of like python (you supply
		# the runtime), which is the right answer for a runtime nobody else
		# can obtain.
		if(JCE_QUICKJS_LIBRARY AND EXISTS "${JCE_QUICKJS_LIBRARY}")
			install(FILES "${JCE_QUICKJS_LIBRARY}" DESTINATION "${_libdest}")
		else()
			message(FATAL_ERROR
				"JCE SDK: the js backend is built but JCE_QUICKJS_LIBRARY is "
				"empty or missing (${JCE_QUICKJS_LIBRARY}). Shipping "
				"jce_script_vm_js.lib without the quickjs it links produces an "
				"SDK whose js backend fails at the CONSUMER's link step with "
				"unresolved JS_* symbols.")
		endif()
	endif()

	# ---------------------------------------------------------------- #
	#  scripting/csharp — the .NET host.                                #
	#                                                                   #
	#  THREE things ship, and the archive alone is the least useful of  #
	#  them:                                                            #
	#                                                                   #
	#    the archive + headers   the native host                        #
	#    the managed assembly    JceScript.dll and its runtimeconfig,   #
	#                            under share/jce/scripting/csharp/       #
	#    nethost                 the import library AND the runtime,    #
	#                            because this backend links the shared  #
	#                            one -- Microsoft builds libnethost.lib #
	#                            against /MT and this engine is /MD, so #
	#                            static is an LNK2038, not a choice     #
	#                                                                   #
	#  The RUNTIME is the player's, like java's JVM and unlike lua and  #
	#  quickjs: a self-contained .NET publish is 70.3 MB, measured, and #
	#  six languages cannot each add that.                              #
	# ---------------------------------------------------------------- #
	if(TARGET jce_script_vm_csharp)
		list(APPEND _sdk_targets jce_script_vm_csharp)
		set(_have_csharp 1)
		install(TARGETS jce_script_vm_csharp
			ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/csharp/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h")

		if(JCE_NETHOST_LIBRARY AND EXISTS "${JCE_NETHOST_LIBRARY}")
			install(FILES "${JCE_NETHOST_LIBRARY}" DESTINATION "${_libdest}")
		else()
			message(FATAL_ERROR
				"JCE SDK: the csharp backend is built but JCE_NETHOST_LIBRARY "
				"is empty or missing (${JCE_NETHOST_LIBRARY}). Shipping "
				"jce_script_vm_csharp.lib without it produces an SDK whose C# "
				"backend fails at the CONSUMER's link step with an unresolved "
				"get_hostfxr_path.")
		endif()
		if(JCE_NETHOST_RUNTIME AND EXISTS "${JCE_NETHOST_RUNTIME}")
			install(FILES "${JCE_NETHOST_RUNTIME}" DESTINATION "${_libdest}")
		else()
			message(FATAL_ERROR
				"JCE SDK: JCE_NETHOST_RUNTIME is empty or missing "
				"(${JCE_NETHOST_RUNTIME}). The import library alone links and "
				"then the consumer's PROCESS DOES NOT START, printing nothing "
				"from the engine at all.")
		endif()

		# The managed half.  A MISSING one is not fatal here: `dotnet build` is
		# deliberately outside CMake, so an SDK produced on a machine that
		# never ran build_csharp.py is a real and recoverable state -- and a
		# FATAL_ERROR would make `jce.py sdk` depend on the .NET SDK.  It is
		# reported instead, by name, because an SDK that shipped the native
		# host and not the assembly is a C# backend that refuses to register.
		if(EXISTS "${JCE_SCRIPT_CSHARP_ASSEMBLY}")
			install(FILES "${JCE_SCRIPT_CSHARP_ASSEMBLY}"
				"${JCE_SCRIPT_CSHARP_RUNTIMECONFIG}"
				DESTINATION "share/jce/scripting/csharp")
		else()
			message(WARNING
				"JCE SDK: the csharp backend is built but the managed assembly "
				"is not (${JCE_SCRIPT_CSHARP_ASSEMBLY}). Run "
				"`python scripting/csharp/build_csharp.py` and re-install, or "
				"the SDK's C# backend will refuse to register with "
				"'no managed assembly'.")
		endif()
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
		list(APPEND _sdk_targets jce_script_vm_c)
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
		list(APPEND _sdk_targets jce_script_vm_python)
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
		list(APPEND _sdk_targets jce_script_vm_java)
		set(_have_java 1)
		install(TARGETS jce_script_vm_java
			ARCHIVE DESTINATION "${_libdest}")
		install(DIRECTORY "${_scripting_root}/java/include/jce"
			DESTINATION "${_incdest}"
			FILES_MATCHING PATTERN "*.h")
	endif()

	if(TARGET jce_script_java)
		list(APPEND _sdk_targets jce_script_java)
		install(TARGETS jce_script_java
			RUNTIME DESTINATION "${_libdest}"
			LIBRARY DESTINATION "${_libdest}"
			ARCHIVE DESTINATION "${_libdest}")

		# javac AS WELL AS the interpreter.  build_java.py resolves javac through
		# JAVA_HOME then PATH and raises SystemExit when it finds neither, so a
		# machine with jni.h and no javac -- a split or stripped JDK -- would fail
		# the SDK BUILD rather than ship without the classes.  That was tolerable
		# while the Java backend was opt-in; it is not once the backend switches
		# itself on wherever jni.h is found, because then the edge case belongs to
		# everyone.  Probe here and degrade, exactly as the missing-interpreter
		# branch below already does.
		find_program(JCE_JAVAC_EXECUTABLE javac
			HINTS "$ENV{JAVA_HOME}/bin" "${JAVA_HOME}/bin"
			DOC "javac used to compile the com.jce.script classes for the SDK")
		mark_as_advanced(JCE_JAVAC_EXECUTABLE)
		find_package(Python3 COMPONENTS Interpreter QUIET)
		if(NOT JCE_JAVAC_EXECUTABLE)
			message(STATUS
				"JCE SDK: the Java scripting CLASSES will not be installed — no "
				"javac on JAVA_HOME or PATH. The native half still ships; "
				"JCE_SCRIPT_JAVA_CLASS_PATH will be empty for consumers and "
				"JCEScripting.cmake will not declare the java component.")
		elseif(NOT Python3_Interpreter_FOUND)
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
			# ALL, because `cmake --install` runs no build.  AND in
			# _sdk_targets, because ALL is not enough: jce.py sdk builds a
			# NAMED target list (jce_sdk_fat_lib + jce_scripting_sdk_artifacts)
			# and never the default target, so an ALL target it does not name
			# is never built -- and then `cmake --install` aborts on
			#
			#     file INSTALL cannot find ".../sdk_java_classes/classes"
			#
			# with everything the install had not yet reached silently skipped.
			# That is the exact failure the aggregate target below was added to
			# prevent; the derivation just did not include this producer, which
			# is a rule with an install rule and no build edge.
			add_custom_target(jce_sdk_java_classes ALL
				DEPENDS "${_java_out}/.stamp")
			list(APPEND _sdk_targets jce_sdk_java_classes)
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
	set(JCE_SDK_SCRIPT_API_STATIC  "${_api_static}")
	set(JCE_SDK_SCRIPT_HAVE_C      "${_have_c}")
	set(JCE_SDK_SCRIPT_HAVE_CPP    "${_have_cpp}")
	set(JCE_SDK_SCRIPT_HAVE_PYTHON "${_have_python}")
	set(JCE_SDK_SCRIPT_HAVE_JAVA   "${_have_java}")
	set(JCE_SDK_SCRIPT_HAVE_JS     "${_have_js}")
	set(JCE_SDK_SCRIPT_HAVE_CSHARP "${_have_csharp}")
	set(JCE_SDK_SCRIPT_PYTHON_VERSION "${_python_version}")

	configure_file(
		"${_JCE_SCRIPTING_CMAKE_DIR}/JCEScripting.cmake.in"
		"${CMAKE_CURRENT_BINARY_DIR}/JCEScripting.cmake"
		@ONLY)
	install(FILES "${CMAKE_CURRENT_BINARY_DIR}/JCEScripting.cmake"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/JCE")

	# jce_script_enable() and the shim it configures.  Installed UNCONFIGURED,
	# beside JCEScripting.cmake which includes it by ${CMAKE_CURRENT_LIST_DIR}:
	# the shim's @-substitutions are per-CONSUMER-TARGET (which languages, whose
	# JAVA_HOME), so they cannot be resolved when the SDK is built.
	install(FILES
			"${_JCE_SCRIPTING_CMAKE_DIR}/JCEScriptEnable.cmake"
			"${_JCE_SCRIPTING_CMAKE_DIR}/jce_script_register_linked.c.in"
			"${_JCE_SCRIPTING_CMAKE_DIR}/jce_script_register_linked.h.in"
		DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/JCE")

	# ---------------------------------------------------------------- #
	#  THE ROSTERS AND THEIR FRAGMENTS.                                 #
	#                                                                    #
	#  EVERY roster ships, including backends this SDK could not build.  #
	#  That is deliberate and it is the whole reason the roster is        #
	#  separate from the target: "there is no such language" and "that     #
	#  language is not in YOUR SDK" have different fixes, and a consumer   #
	#  who gets only the buildable ones cannot tell which they are         #
	#  looking at.  jce_script_enable() reads them as the KNOWN set and    #
	#  intersects with the imported targets for the AVAILABLE set.         #
	#                                                                      #
	#  The fragment is the backend's register() body.  It is installed      #
	#  BESIDE its roster and its absence is FATAL at the consumer's         #
	#  configure, never skipped: a silently missing fragment produces a      #
	#  binary that LINKS the backend and REGISTERS nothing, which is the     #
	#  quietest failure this whole layer can produce.                        #
	# ---------------------------------------------------------------- #
	file(GLOB _jce_rosters "${_scripting_root}/*/jce_backend.cmake")
	foreach(_r IN LISTS _jce_rosters)
		get_filename_component(_rdir "${_r}" DIRECTORY)
		get_filename_component(_rname "${_rdir}" NAME)
		set(JCE_BACKEND_LANGUAGE "")
		set(JCE_BACKEND_REGISTER_FRAGMENT "")
		include("${_r}")
		if(NOT JCE_BACKEND_LANGUAGE OR NOT JCE_BACKEND_REGISTER_FRAGMENT)
			message(FATAL_ERROR
				"JCE SDK: ${_r} is not a usable roster (needs "
				"JCE_BACKEND_LANGUAGE and JCE_BACKEND_REGISTER_FRAGMENT). "
				"Installing it would ship a language the consumer can name "
				"and cannot register.")
		endif()
		if(NOT EXISTS "${_rdir}/${JCE_BACKEND_REGISTER_FRAGMENT}")
			message(FATAL_ERROR
				"JCE SDK: the ${JCE_BACKEND_LANGUAGE} roster names "
				"${JCE_BACKEND_REGISTER_FRAGMENT}, which does not exist in "
				"${_rdir}.")
		endif()
		# ONE DIRECTORY PER BACKEND, mirroring scripting/<lang>/ exactly.
		# Flattening them would collide: every backend's fragment is called
		# register.c.in, and a RENAME would make the installed roster's own
		# JCE_BACKEND_REGISTER_FRAGMENT wrong -- which jce_script_enable would
		# then have to paper over with an if(EXISTS) fallback chain, i.e. the
		# thing that turns a missing fragment into a silent skip.
		# A "simple" backend has no hand-written install block above, so its
		# archive ships from here — otherwise its roster installs, its target is
		# imported by JCEScripting.cmake, and the import points at a file that
		# was never copied.
		if(JCE_BACKEND_SDK_IMPORT_SIMPLE AND TARGET ${JCE_BACKEND_VM_TARGET})
			list(APPEND _sdk_targets ${JCE_BACKEND_VM_TARGET})
			install(TARGETS ${JCE_BACKEND_VM_TARGET} ARCHIVE DESTINATION "${_libdest}")
			# ITS HEADERS TOO.  The roster's JCE_BACKEND_REGISTER_INCLUDE is emitted
			# verbatim into the consumer's shim, so a backend whose header does not
			# ship produces a shim that will not COMPILE on the consumer -- measured
			# with a throwaway backend: "fatal error C1083: Cannot open include file".
			if(NOT JCE_BACKEND_SDK_HEADER_DIR)
				message(FATAL_ERROR
					"JCE SDK: the ${JCE_BACKEND_LANGUAGE} roster is "
					"SDK_IMPORT_SIMPLE but names no JCE_BACKEND_SDK_HEADER_DIR. "
					"Its register include would not resolve on a consumer.")
			endif()
			install(DIRECTORY "${_rdir}/${JCE_BACKEND_SDK_HEADER_DIR}/"
				DESTINATION "${_incdest}" FILES_MATCHING PATTERN "*.h")
		endif()
		install(FILES "${_r}" "${_rdir}/${JCE_BACKEND_REGISTER_FRAGMENT}"
			DESTINATION
				"${CMAKE_INSTALL_LIBDIR}/cmake/JCE/backends/${JCE_BACKEND_LANGUAGE}")
	endforeach()
	list(LENGTH _jce_rosters _jce_roster_n)
	message(STATUS
		"JCE SDK: ${_jce_roster_n} scripting roster(s) will be installed "
		"(every backend this TREE knows, buildable here or not)")
	unset(_jce_rosters)
	unset(_jce_roster_n)

	message(STATUS
		"JCE SDK: scripting install rules registered "
		"(api=${_have_api} c=${_have_c} cpp=${_have_cpp} "
		"python=${_have_python} java=${_have_java})")

	# ---------------------------------------------------------------- #
	#  ONE build target naming everything the rules above promise.      #
	#                                                                   #
	#  `jce.py sdk` builds a fixed target list, and that list named the  #
	#  engine's fat lib and nothing else -- so `cmake --install` hit the #
	#  first scripting artefact, found no file, and ABORTED.  Everything #
	#  the install had not yet reached was then silently skipped, which  #
	#  reads as "the SDK is missing files" rather than "the build was    #
	#  never asked to produce them".                                     #
	#                                                                   #
	#  Enumerating the backends in jce.py instead would put the build    #
	#  list and the install list in two files that must agree, and the   #
	#  failure mode of them disagreeing is this same aborted install.    #
	#  They are derived from ONE loop here instead: a backend that       #
	#  registers an install rule is, by construction, a backend the SDK  #
	#  build builds.  A backend whose directory is absent registers      #
	#  nothing and is depended on by nothing.                            #
	# ---------------------------------------------------------------- #
	add_custom_target(jce_scripting_sdk_artifacts)
	if(_sdk_targets)
		add_dependencies(jce_scripting_sdk_artifacts ${_sdk_targets})
	endif()

endfunction()
