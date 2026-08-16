cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  scripting/ — the language-binding layer.                           #
#                                                                     #
#  A sibling of engine/, editor/, tools/ and tests/, not a child of    #
#  engine/: bindings live at the SCRIPT layer, the engine's own build  #
#  forces BUILD_SHARED_LIBS OFF (engine/CMakeLists.txt), and the       #
#  toolchains these backends need — a JDK, a Python — must never reach #
#  the engine's CMake or the SDK install rules.                        #
#                                                                     #
#  THIS FILE DISCOVERS ITS SUBDIRECTORIES.  Backends are being added   #
#  in parallel (scripting/python, scripting/java, scripting/cpp), and  #
#  an explicit add_subdirectory() list would make every one of those   #
#  agents edit this one file — the exact collision the generator split #
#  is being done to avoid on the Python side.  Adding a backend is     #
#  therefore: create scripting/<name>/CMakeLists.txt.  Nothing here    #
#  changes, and nothing in any sibling's directory changes.            #
#                                                                     #
#  CONFIGURE_DEPENDS so a new subdirectory re-runs CMake by itself     #
#  rather than being silently absent until someone reconfigures.       #
# ------------------------------------------------------------------ #

# c_abi is added FIRST and by name: it declares the jce_script_api target the
# other backends link or load, and CMake needs a target to exist before a
# sibling directory can reference it.  Ordering by glob would make that
# dependency a property of alphabetical luck.
option(JCE_BUILD_SCRIPT_API_SHARED
	"Build jce_script_api — the script-facing shared library (C ABI)." ON)
if(JCE_BUILD_SCRIPT_API_SHARED)
	add_subdirectory(c_abi)
endif()

file(GLOB _jce_scripting_backends CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
foreach(_cml IN LISTS _jce_scripting_backends)
	get_filename_component(_dir "${_cml}" DIRECTORY)
	get_filename_component(_name "${_dir}" NAME)
	if(NOT _name STREQUAL "c_abi")
		add_subdirectory("${_name}")
	endif()
endforeach()
unset(_jce_scripting_backends)
unset(_cml)
unset(_dir)
unset(_name)

# ------------------------------------------------------------------ #
#  THE SDK SIDE, and it is LAST on purpose.                           #
#                                                                     #
#  jce_scripting_register_sdk_install() asks `if(TARGET ...)` about    #
#  every backend above, and a target declared by a LATER               #
#  add_subdirectory() reads as absent — silently, with a green         #
#  configure and a green build.  That exact mistake, one level up in   #
#  the root CMakeLists.txt, made the editor link none of these         #
#  backends while reporting nothing (see a7a2dbfc).  Here the cost of  #
#  getting it wrong is an SDK that ships the engine and no scripting,  #
#  which is the state this file is being added to end.                 #
#                                                                     #
#  It is a no-op unless JCE_ENABLE_SDK_INSTALL is ON, so a normal      #
#  developer build declares no install rules and no extra targets.     #
# ------------------------------------------------------------------ #
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/JCEScriptingInstall.cmake")
jce_scripting_register_sdk_install()
