cmake_minimum_required(VERSION 3.20)

# ------------------------------------------------------------------ #
#  scripting/cpp — the C/C++ side of the scripting seam, in two       #
#  directions that must not be confused:                              #
#                                                                     #
#    jce_script_cpp       CALL DOWN.  A script calls the engine.       #
#                         Header-only wrapper over scripting/c_abi.    #
#    jce_script_vm_cpp    CALL UP.  The engine calls a script.  The    #
#                         "cpp" JceScriptVM plus the module registry.  #
#    jce_script_cpp_module  What a MODULE AUTHOR links: headers only,  #
#                         never the registry.                          #
#                                                                     #
#  The middle one is COMPILED and the outer two are INTERFACE, and     #
#  that split is a correctness decision, not packaging.  See the       #
#  banner in include/jce/script_vm/jce_script_cpp.hpp: the module side #
#  must be header-only so the try/catch and the struct_size are        #
#  compiled in the MODULE's translation unit, and the VM side must be  #
#  compiled so the module registry is ONE object per process.          #
# ------------------------------------------------------------------ #

# ================================================================== #
#  jce_script_vm_cpp — the engine calling up.                        #
#                                                                    #
#  Declared FIRST, and outside the jce_script_api guard below,        #
#  because it does not need the C ABI: the class thunks are compiled  #
#  in the module, so this half is a registry, a clamp and eighteen    #
#  forwarders in C99.  Turning JCE_BUILD_SCRIPT_API_SHARED off must   #
#  not silently take the VM with it.                                  #
# ================================================================== #

add_library(jce_script_vm_cpp STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src/jce_script_vm_cpp.c")

set_target_properties(jce_script_vm_cpp PROPERTIES
	C_STANDARD          99
	C_STANDARD_REQUIRED YES
	C_EXTENSIONS        OFF)

target_include_directories(jce_script_vm_cpp
	PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# jce_script  — jce_script_vm_register() and the lifecycle header.
# jce_platform — jce_library_open/symbol/close, the shared-object loader.
# Both PUBLIC: a host that links this one links those two anyway (it calls
# jce_script_create_sized), and a private link would hand a consumer a header
# it cannot satisfy.
target_link_libraries(jce_script_vm_cpp PUBLIC jce_script jce_platform)

jce_target_warnings(jce_script_vm_cpp STRICT)

# ── The VM source must actually reach the compiler ──────────────────
#
# Same reasoning as engine/CMakeLists.txt's assertion that the generated Lua
# bindings are in jce_script's SOURCES: a target that built with no VM in it
# would configure, compile and link, and every jce_script_vm_create("cpp")
# would return NULL at runtime with "no script VM registered for language
# 'cpp'" — which reads like a caller mistake.
get_target_property(_jce_vm_cpp_srcs jce_script_vm_cpp SOURCES)
if(NOT _jce_vm_cpp_srcs MATCHES "jce_script_vm_cpp\\.c")
	message(FATAL_ERROR
		"jce_script_vm_cpp has no jce_script_vm_cpp.c in its sources — the "
		"cpp JceScriptVM would not exist and the failure would surface as a "
		"NULL from jce_script_vm_create(\"cpp\").")
endif()
unset(_jce_vm_cpp_srcs)

# ================================================================== #
#  jce_script_cpp — the C++ wrapper over the script-facing C ABI.     #
#                                                                     #
#  AN INTERFACE TARGET, because the wrapper is HEADER-ONLY, and that  #
#  is not a convenience choice.  jce::script::Api::open defaults       #
#  host_size to sizeof(JceScriptHost), and the whole point of the      #
#  short-host ABI is that the number must be the CALLER's.  A compiled #
#  wrapper would evaluate that sizeof in ITS translation unit and hand #
#  the C ABI a size the caller never agreed to, defeating the          #
#  min(caller, engine) copy.  Header-only puts the sizeof in the       #
#  consumer's TU, where it belongs.                                    #
#                                                                     #
#  Second consequence, also deliberate: there is no second ABI         #
#  surface to version.  The only ABI on this path is the C one, which  #
#  already has a version handshake and an export-table test.           #
#                                                                     #
#  THERE IS A scripting/c/, AND THIS COMMENT USED TO SAY THERE WAS     #
#  NOT.  What it said was: the C binding of the scripting surface is   #
#  scripting/c_abi's generated header, and a C consumer needs nothing  #
#  from this directory.  Both halves are still true and they answered  #
#  the WRONG QUESTION — they are about C CALLING DOWN.  The direction   #
#  that was missing is C being CALLED UP: a C author could not write   #
#  an entity script, because there was no "c" language, no extension   #
#  routing to one, no catalog row, and the header they would have had  #
#  to include is named jce_script_vm_cpp.h.                            #
#                                                                     #
#  scripting/c now registers "c" as a second LANGUAGE over the class   #
#  registry THIS directory owns — not a second registry.  It links     #
#  jce_script_vm_cpp and uses jce_script_vm_cpp_create_for(); nothing  #
#  here links or includes anything of its, so the dependency runs one  #
#  way only.  See scripting/c/CMakeLists.txt and README.md.            #
# ------------------------------------------------------------------ #

if(NOT TARGET jce_script_api)
	# Not silent.  The wrapper is a wrapper: without the C ABI there is
	# nothing under it, and a consumer that got an include path but no
	# library would find out at link time instead of here.
	message(STATUS
		"JCE scripting: C++ wrapper SKIPPED — jce_script_api is not built "
		"(-DJCE_BUILD_SCRIPT_API_SHARED=OFF). jce_script_vm_cpp is still "
		"built; a module author needs the wrapper, the engine does not.")
	return()
endif()

add_library(jce_script_cpp INTERFACE)

target_include_directories(jce_script_cpp INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/include")

# PUBLIC on an INTERFACE target: the wrapper's every method is an inline call
# into jce_script_api, so a consumer needs both the include path and the
# library.  Linking it here is what stops "header-only" from meaning
# "silently unlinked".
target_link_libraries(jce_script_cpp INTERFACE jce_script_api)

# The wrapper #errors below C++17 rather than failing deep inside <optional>.
# This asks CMake to ASK for 17 from any consumer that has not set a higher
# one; it is a floor, not a ceiling, and C++20 consumers additionally get the
# std::span overloads.
target_compile_features(jce_script_cpp INTERFACE cxx_std_17)

# ── The generated header must actually be there ──────────────────────
#
# Same reasoning as scripting/c_abi/CMakeLists.txt's assertion about its
# forwarders: the generator proves the file is CORRECT and says nothing about
# whether it is PRESENT in the tree a consumer configured.  A missing header
# would configure cleanly, export an include directory containing nothing, and
# fail at the first #include in someone else's target.
set(_jce_script_cpp_header
	"${CMAKE_CURRENT_SOURCE_DIR}/include/jce/script_api/jce_script_api.hpp")
if(NOT EXISTS "${_jce_script_cpp_header}")
	message(FATAL_ERROR
		"jce_script_cpp: ${_jce_script_cpp_header} is missing — the generated "
		"C++ wrapper is not in the tree. Regenerate with:\n"
		"  python tools/scriptgen/gen_script_bindings.py --write")
endif()
unset(_jce_script_cpp_header)

# ================================================================== #
#  jce_script_cpp_module — what a SCRIPT MODULE links.               #
#                                                                    #
#  INTERFACE, and it deliberately does NOT link jce_script_vm_cpp.    #
#  A module must not carry the registry: the registry is per-process  #
#  state that belongs to the HOST, and a plugin that linked its own   #
#  copy would publish its classes into a registry the engine never    #
#  reads — a module that loads, reports success, and whose classes    #
#  no instantiate() can find.                                        #
#                                                                    #
#  What it does give a module is the two headers plus the call-down   #
#  wrapper, which is the only engine access a script class gets.      #
# ================================================================== #

add_library(jce_script_cpp_module INTERFACE)
target_link_libraries(jce_script_cpp_module INTERFACE jce_script_cpp)
target_compile_features(jce_script_cpp_module INTERFACE cxx_std_17)

# A module includes <jce/middleware/script/jce_script_vm.h> for JceScriptVM's
# types.  That header comes from the engine's public include root, which
# jce_script_api already exports PUBLIC — stated here rather than relied on,
# because "it happens to be transitive today" is how an include path
# disappears in someone else's refactor.
target_include_directories(jce_script_cpp_module INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_SOURCE_DIR}/engine/include")
