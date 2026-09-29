cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  scripting/python — two artefacts, and the split is the argument.   #
#                                                                     #
#  1. THE BINDING (jce_script/, the wheel).  Pure Python over the      #
#     plain C ABI in scripting/c_abi: ctypes, not cffi and not an      #
#     extension module, so a consumer needs no compiler, no headers    #
#     and no build step — `pip install` and it works.  Nothing below   #
#     compiles any part of it, and nothing may: the wheel stays        #
#     py3-none-any.                                                    #
#                                                                     #
#  2. THE VM SHIM (src/, jce_script_vm_python).  The MIRROR direction: #
#     the engine calling UP into CPython.  This one is C and cannot    #
#     be anything else, and the reason is structural rather than a     #
#     matter of effort — JceScriptVM is a table of C function          #
#     pointers the engine calls, and in the engine's process there is  #
#     no interpreter until C calls Py_InitializeFromConfig.  Nothing   #
#     written in Python can run before the runtime that would run it   #
#     exists.  The argument is spelled out in jce_script/vm.py.        #
#                                                                     #
#  THE "NO COMPILER" CLAIM THEREFORE MOVED, AND WAS NOT DROPPED.  It   #
#  was never really about this file: it is about the WHEEL.  The gate  #
#  in tests/scripting/python/test_emit_python.py now says so           #
#  precisely — no add_library() may name a file under jce_script/, the #
#  VM shim is the only compiled target here, and pyproject.toml still  #
#  declares no extension module and no dependency.  A blanket ban on   #
#  add_library() in this file would have been satisfied by moving the  #
#  target one directory down, which is the shape of a gate that        #
#  reports green where nobody looks.                                   #
#                                                                     #
#  The presence check below is unchanged in spirit: the generator      #
#  proves the files are CORRECT and says nothing about whether they    #
#  are THERE, and nothing else in the build would notice a missing     #
#  _generated.py — the failure would surface as an ImportError inside  #
#  a test, which reads like a broken test rather than a missing        #
#  artefact.  vm.py is in the list for the same reason, and it is not  #
#  optional: the shim refuses to create a VM without it.               #
# ------------------------------------------------------------------ #

set(_JCE_PY_PKG "${CMAKE_CURRENT_SOURCE_DIR}/jce_script")

foreach(_f _generated.py _generated.pyi __init__.py py.typed vm.py)
	if(NOT EXISTS "${_JCE_PY_PKG}/${_f}")
		message(FATAL_ERROR
			"scripting/python: ${_f} is missing from jce_script/. The ctypes "
			"binding cannot import. Regenerate with:\n"
			"  python tools/scriptgen/gen_script_bindings.py --write")
	endif()
endforeach()

# Published so tests/scripting/python can import the package without knowing
# the layout, and so a future SDK install rule has one name to copy.
set(JCE_SCRIPT_PYTHON_PACKAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
	CACHE INTERNAL "Directory containing the jce_script Python package")

unset(_JCE_PY_PKG)

# ------------------------------------------------------------------ #
#  The VM shim.                                                       #
#                                                                     #
#  OFF is not the default and neither is a hard requirement: the       #
#  target is built when an embeddable CPython is present and skipped   #
#  with a STATUS line when it is not.  A hard find_package(REQUIRED)   #
#  would make every developer without the Python development headers   #
#  unable to configure the ENGINE, for a backend they may not use;     #
#  a default-OFF option would make the backend absent on the machines  #
#  where it works, which is how a target stops being built and nobody  #
#  notices.                                                            #
#                                                                     #
#  Development.Embed and not Development: the full component also      #
#  demands the Module artefacts (a Python extension's link mode),      #
#  which this target is not and must not become.                       #
#                                                                     #
#  A configuration WITHOUT this target is still gated: the Python      #
#  backend's static tests need no build at all and run either way —    #
#  see tests/scripting/python/CMakeLists.txt.                          #
# ------------------------------------------------------------------ #

option(JCE_BUILD_SCRIPT_VM_PYTHON
	"Build jce_script_vm_python — the engine-calls-up CPython backend." ON)

if(NOT JCE_BUILD_SCRIPT_VM_PYTHON)
	message(STATUS
		"JCE scripting: Python VM backend SKIPPED "
		"(-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF)")
	return()
endif()

# The browser runtime has neither a process CPython nor a loadable ScriptApi
# shared library for the ctypes package.  Letting FindPython3 run while
# cross-compiling with Emscripten searches for a target Development.Embed
# package that this backend cannot consume and can stall configuration for
# minutes.  The pure-Python package is still installed for host-side tools;
# only the in-process VM shim is unavailable on Web.
if(EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
	message(STATUS
		"JCE scripting: Python VM backend SKIPPED — Web targets do not "
		"provide an embeddable process CPython/runtime shared library.")
	return()
endif()

find_package(Python3 COMPONENTS Development.Embed QUIET)
if(NOT Python3_Development.Embed_FOUND)
	message(STATUS
		"JCE scripting: Python VM backend SKIPPED — no embeddable CPython "
		"(Python3 COMPONENTS Development.Embed). Install the Python "
		"development headers and import library to build it.")
	return()
endif()

# FindPython3 proves that an embeddable development package exists, but it
# does not prove that its import/static library matches the target
# architecture.  That distinction matters for cross builds: on Windows an
# x64 host Python is otherwise accepted while generating an ARM64 SDK, and
# the mismatch is only discovered by the first downstream application's
# linker.  Link one target-architecture probe now so an incompatible optional
# backend is omitted from the SDK roster instead of being advertised broken.
include(CheckCSourceCompiles)
include(CMakePushCheckState)
string(SHA256 _jce_python_probe_key
	"${CMAKE_SYSTEM_NAME}|${CMAKE_SYSTEM_PROCESSOR}|${Python3_VERSION}|${Python3_INCLUDE_DIRS}|${Python3_LIBRARIES}")
string(SUBSTRING "${_jce_python_probe_key}" 0 16 _jce_python_probe_key)
set(_jce_python_probe_var
	"JCE_SCRIPT_PYTHON_TARGET_LINKS_${_jce_python_probe_key}")
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES Python3::Python)
check_c_source_compiles(
	"#include <Python.h>\nint main(void) { return Py_IsInitialized(); }"
	${_jce_python_probe_var})
cmake_pop_check_state()
if(NOT ${_jce_python_probe_var})
	message(STATUS
		"JCE scripting: Python VM backend SKIPPED — CPython ${Python3_VERSION} "
		"cannot link for target ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. "
		"Provide a target-architecture Development.Embed package or pass "
		"-DJCE_BUILD_SCRIPT_VM_PYTHON=OFF.")
	unset(_jce_python_probe_key)
	unset(_jce_python_probe_var)
	return()
endif()
unset(_jce_python_probe_key)
unset(_jce_python_probe_var)

add_library(jce_script_vm_python STATIC src/jce_script_vm_python.c)
set_target_properties(jce_script_vm_python PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF
	# So the archive can be linked into a shared object later without a
	# rebuild; JCE_BUILD_JNI sets the same property for the same reason.
	POSITION_INDEPENDENT_CODE ON)

target_include_directories(jce_script_vm_python
	PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/src"
	PRIVATE "${CMAKE_SOURCE_DIR}/engine/include")

# jce_script carries jce_script_vm_register and the public lifecycle
# forwarders; jce_core carries jce_malloc/jce_free and the log.  NOT the
# umbrella jce_engine: this backend touches the script layer and nothing else,
# and linking the umbrella would drag the renderer into a game that wanted a
# scripting language.
target_link_libraries(jce_script_vm_python
	PUBLIC  jce_script
	PRIVATE jce_core Python3::Python)

# The version this was compiled against, for a bug report and for the tests.
target_compile_definitions(jce_script_vm_python PRIVATE
	JCE_PY_VM_PYTHON_VERSION="${Python3_VERSION}")

# Python.h is not warning-clean at the levels this repo builds its own code
# at, and the noise would be attributed to this file.  Scoped to this target.
if(MSVC)
	target_compile_options(jce_script_vm_python PRIVATE /wd4100 /wd4127 /wd4232)
endif()

message(STATUS
	"JCE scripting: Python VM backend ON (CPython ${Python3_VERSION}, "
	"${Python3_LIBRARIES})")

# The version, published out of the ONE scope that probed for it.  An SDK
# install rule in scripting/cmake/ records it so a consumer's find_package
# can refuse a CPython whose major.minor is not this one; without publishing
# it, that rule would have to run a SECOND find_package(Python3), and two
# probes are allowed to disagree.  CACHE INTERNAL for the same reason
# JCE_SCRIPT_PYTHON_PACKAGE_DIR above is: subdirectory scope does not reach
# scripting/CMakeLists.txt, where the install rules live.
set(JCE_SCRIPT_PYTHON_VERSION "${Python3_VERSION}" CACHE INTERNAL
	"The CPython jce_script_vm_python was compiled against")
